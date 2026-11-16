/* xz_music owns mount/scan/track changes; the SDK MP3 worker reads file data.
 * TTS pause/resume shares output_lock.
 * Decoder callbacks only signal EOF: closing there would deadlock. */
#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_posix.h>
#include <dfs_fs.h>
#include <dfs_romfs.h>
#include <audio_mp3ctrl.h>
#include <mem_section.h>
#include <string.h>
#include <ctype.h>
#include "xiaozhi_music.h"
#include "xiaozhi_music_source.h"

extern void xiaozhi_ui_music(const char *name, bool paused);

#define MUSIC_MAX 64
#define MUSIC_PATH 512
#define MUSIC_QUEUE 6
#define MOUNT_RETRY_MS 15000
#define MUSIC_GUARD_MS 10000

enum { CMD_MOUNT, CMD_SCAN, CMD_PLAY, CMD_STOP, CMD_LIST };
typedef struct { struct rt_semaphore sem; int result; } music_reply_t;
typedef struct {
    unsigned cmd;
    rt_uint32_t epoch;
    char keyword[256];
    music_reply_t *reply;
} music_cmd_t;

static char tracks[MUSIC_MAX][MUSIC_PATH] L2_RET_BSS_SECT(xz_music_tracks);
/* mp3ctrl retains the filename pointer. */
static char current_path[MUSIC_PATH] L2_RET_BSS_SECT(xz_music_path);
static rt_uint8_t music_stack[6144] L2_RET_BSS_SECT(xz_music_stack) ALIGN(RT_ALIGN_SIZE);
static rt_uint8_t monitor_stack[3072] L2_RET_BSS_SECT(xz_music_monitor) ALIGN(RT_ALIGN_SIZE);
static rt_uint8_t music_audio_pool[512 * 1024] L2_RET_BSS_SECT(xz_music_audio) ALIGN(RT_ALIGN_SIZE);
static rt_uint8_t music_queue_pool[MUSIC_QUEUE * (sizeof(music_cmd_t) + sizeof(void *))]
    L2_RET_BSS_SECT(xz_music_queue) ALIGN(RT_ALIGN_SIZE);

static struct rt_thread music_thread;
static struct rt_memheap music_audio_heap;
static struct rt_messagequeue music_queue;
static struct rt_mutex output_lock;
static struct rt_thread music_monitor;
static bool ready, mounted;
static volatile bool tts_active, wanted, paused;
static volatile bool playing;
static int count, current;
static mp3ctrl_handle volatile player;
static volatile rt_uint32_t eof_generation;
static rt_uint32_t generation;
static rt_tick_t last_mount_try;
static volatile rt_tick_t worker_tick;
static volatile rt_uint32_t control_epoch, alloc_failures;
static volatile bool stop_pending;
static const char *volatile operation = "idle";

static void request_stop(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    control_epoch++;
    stop_pending = true;
    playing = false;
    rt_hw_interrupt_enable(level);
}

void *xz_music_alloc(rt_size_t size)
{
    void *p = rt_memheap_alloc(&music_audio_heap, size);
    if (!p)
    {
        rt_base_t level = rt_hw_interrupt_disable();
        alloc_failures++;
        rt_hw_interrupt_enable(level);
        rt_kprintf("[music] alloc fail size=%lu rc=%d\n", (unsigned long)size, -RT_ENOMEM);
    }
    return p;
}
void *xz_music_calloc(rt_size_t n, rt_size_t size)
{
    if (size && n > (rt_size_t)-1 / size) return NULL;
    void *p = xz_music_alloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}
void xz_music_free(void *ptr)
{
    if (ptr) rt_memheap_free(ptr);
}
void xz_music_free_any(void *ptr)
{
    if ((rt_ubase_t)ptr >= (rt_ubase_t)music_audio_pool &&
        (rt_ubase_t)ptr < (rt_ubase_t)(music_audio_pool + sizeof(music_audio_pool)))
        rt_memheap_free(ptr);
    else rt_free(ptr);
}

static int music_heap_init(void)
{
    return rt_memheap_init(&music_audio_heap, "mp3_psram", music_audio_pool,
                           sizeof(music_audio_pool));
}
INIT_COMPONENT_EXPORT(music_heap_init);

/* DFS requires a real directory to attach a filesystem below root.
 * No disk writes, formatting, or persistent root allocation are needed. */
static const struct romfs_dirent root_entries[] = {
    {ROMFS_DIRENT_DIR, "sdcard", RT_NULL, 0},
    {ROMFS_DIRENT_DIR, "dev", RT_NULL, 0},
};
static const struct romfs_dirent music_root = {
    ROMFS_DIRENT_DIR, "/", (const rt_uint8_t *)root_entries,
    sizeof(root_entries) / sizeof(root_entries[0])
};

static const char *basename_of(const char *path)
{
    const char *name = strrchr(path, '/');
    return name ? name + 1 : path;
}

static void report_track(void)
{
    playing = !stop_pending && wanted && player && !paused;
    xiaozhi_ui_music(wanted && player ? basename_of(current_path) : NULL, paused);
}

/* Caller owns output_lock. Mounting/scanning never holds this lock, so an
 * absent/slow card cannot delay TTS when there is no music output to pause. */
static void close_player_locked(void)
{
    playing = false;
    if (player)
    {
        operation = "close";
        /* Pause removes only this TX source (owner TXRX remains). Avoid the
         * SDK close fade loop, which otherwise waits for further PCM writes. */
        mp3ctrl_pause(player);
        mp3ctrl_close(player);
    }
    player = NULL;
    eof_generation = 0;
    paused = false;
    operation = "idle";
}

static void close_player(void)
{
    rt_mutex_take(&output_lock, RT_WAITING_FOREVER);
    close_player_locked();
    rt_mutex_release(&output_lock);
}

static int mount_sd(void)
{
    static const char *const devices[] = {"sd0", "sd1"};
    struct dfs_filesystem *fs;
    if (mounted) return RT_EOK;
    last_mount_try = rt_tick_get();
    fs = dfs_filesystem_lookup("/");
    if (!fs && dfs_mount(NULL, "/", "rom", 0, &music_root) < 0)
    {
        rt_kprintf("[music] root mount failed, retry later\n");
        return -RT_ERROR;
    }
    /* Respect an existing root (if one is added to the project later). */
    if (fs) mkdir("/sdcard", 0);
    fs = dfs_filesystem_lookup("/sdcard");
    if (fs && fs->path && !strcmp(fs->path, "/sdcard"))
    {
        mounted = true;
        return RT_EOK;
    }
    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
    {
        rt_device_t dev = rt_device_find(devices[i]);
        /* Wi-Fi is an SDIO function, never a block device. */
        if (!dev || dev->type != RT_Device_Class_Block) continue;
        if (!dfs_mount(devices[i], "/sdcard", "elm", 0, NULL))
        {
            mounted = true;
            rt_kprintf("[music] %s mounted at /sdcard\n", devices[i]);
            return RT_EOK;
        }
    }
    rt_kprintf("[music] no FAT SD card; retry in 15s or xz_music mount\n");
    return -RT_ERROR;
}

static bool is_mp3(const char *name)
{
    size_t n = strlen(name);
    return n >= 4 && name[n-4] == '.' &&
           tolower((unsigned char)name[n-3]) == 'm' &&
           tolower((unsigned char)name[n-2]) == 'p' && name[n-1] == '3';
}

static int scan_dir(const char *path, unsigned depth)
{
    DIR *dir = opendir(path);
    struct dirent *entry;
    if (!dir) return -RT_ERROR;
    while (count < MUSIC_MAX && (entry = readdir(dir)) != NULL)
    {
        char full[MUSIC_PATH];
        struct stat st;
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (strlen(path) + 1 + strlen(entry->d_name) >= sizeof(full))
        {
            rt_kprintf("[music] skip overlong path: %s\n", entry->d_name);
            continue;
        }
        rt_snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode) && depth == 0) scan_dir(full, 1);
        else if (S_ISREG(st.st_mode) && is_mp3(entry->d_name))
            rt_strncpy(tracks[count++], full, MUSIC_PATH);
    }
    closedir(dir);
    return RT_EOK;
}

static int scan_sd(void)
{
    /* A rescan cannot invalidate mp3ctrl's current_path or reorder its index. */
    if (wanted) return -RT_EBUSY;
    if (mount_sd()) return -RT_ERROR;
    count = 0;
    if (scan_dir("/sdcard", 0))
    {
        mounted = false;
        rt_kprintf("[music] scan failed, mount can be retried\n");
        return -RT_ERROR;
    }
    /* Stable bytewise filename order, with no temporary playlist allocation. */
    for (int i = 1; i < count; i++)
        for (int j = i; j > 0 && strcmp(tracks[j-1], tracks[j]) > 0; j--)
        {
            char tmp[MUSIC_PATH];
            memcpy(tmp, tracks[j], MUSIC_PATH);
            memcpy(tracks[j], tracks[j-1], MUSIC_PATH);
            memcpy(tracks[j-1], tmp, MUSIC_PATH);
        }
    rt_kprintf("[music] %d MP3 tracks (limit %d, root + one level)\n", count, MUSIC_MAX);
    return count;
}

static int music_callback(audio_server_callback_cmt_t cmd, void *userdata, uint32_t reserved)
{
    if (cmd == as_callback_cmd_play_to_end)
        eof_generation = (rt_uint32_t)userdata;
    return 0;
}

static int start_track(int index)
{
    rt_mutex_take(&output_lock, RT_WAITING_FOREVER);
    close_player_locked();
    audio_parameter_t owner;
    if (stop_pending || !xz_audio_music_parameters(&owner))
    {
        rt_kprintf("[music] start rejected rc=%d: RX owner unavailable or stop pending\n", -RT_EBUSY);
        rt_mutex_release(&output_lock);
        return -RT_EBUSY;
    }
    operation = "open";
    current = index;
    rt_strncpy(current_path, tracks[index], sizeof(current_path));
    if (++generation == 0) ++generation;
    player = mp3ctrl_open2(AUDIO_TYPE_LOCAL_MUSIC, current_path, music_callback,
                          (void *)generation, AUDIO_DEVICE_SPEAKER);
    if (!player)
    {
        rt_kprintf("[music] open failed rc=%d: %s\n", -RT_ERROR, current_path);
        operation = "idle";
        rt_mutex_release(&output_lock);
        return -RT_ERROR;
    }
    mp3ctrl_ioctl(player, MP3CTRL_IOCTRL_LOOP_TIMES, 0);
    paused = tts_active;
    /* open starts paused: do not send PLAY while TTS owns the output. */
    if (!paused && mp3ctrl_play(player))
    {
        close_player_locked();
        rt_mutex_release(&output_lock);
        return -RT_ERROR;
    }
    rt_kprintf("[music] %s%s\n", paused ? "waiting for TTS: " : "playing: ", current_path);
    report_track();
    operation = "idle";
    rt_mutex_release(&output_lock);
    return RT_EOK;
}

static int play_keyword(const char *keyword)
{
    int index = 0;
    if (!count && scan_sd() <= 0) return -RT_ERROR;
    if (keyword && *keyword)
    {
        for (index = 0; index < count; index++)
            if (strstr(basename_of(tracks[index]), keyword)) break;
        if (index == count)
        {
            rt_kprintf("[music] no filename matches: %s\n", keyword);
            return -RT_ERROR;
        }
    }
    wanted = true;
    if (!start_track(index)) return RT_EOK;
    wanted = false;
    report_track();
    return -RT_ERROR;
}

static int execute(const music_cmd_t *cmd)
{
    /* A stop cancels every play already queued before it, without waiting
     * behind a scan or needing space in the command queue. */
    if (cmd->cmd == CMD_PLAY && cmd->epoch != control_epoch) return -RT_EBUSY;
    switch (cmd->cmd)
    {
    case CMD_MOUNT: return mount_sd();
    case CMD_SCAN: return scan_sd();
    case CMD_PLAY: return play_keyword(cmd->keyword);
    case CMD_STOP:
        wanted = false;
        close_player();
        report_track();
        return RT_EOK;
    case CMD_LIST:
        if (!wanted && scan_sd() < 0) return -RT_ERROR;
        for (int i = 0; i < count; i++) rt_kprintf("%02d %s\n", i + 1, tracks[i]);
        return count;
    }
    return -RT_EINVAL;
}

/* Independent of DFS, output_lock and synchronous mp3ctrl calls.
 * No audio APIs, heap locks, forced thread deletion or freeing live buffers.
 * Print on a dedicated low-priority thread, never on the shared soft-timer
 * thread or in an ISR. A blocked worker is diagnosed; cleanup stays safe. */
static void music_monitor_tick(void *arg)
{
    static uint32_t last_mic;
    static rt_tick_t mic_tick;
    static uint32_t last_writes;
    static rt_tick_t pcm_tick;
    static bool was_playing;
    static bool guard_reported;
    rt_tick_t now = rt_tick_get();
    uint32_t mic = xz_audio_music_rx_frames();
    xz_music_source_stats_t stats;
    xz_music_source_snapshot(&stats);
    if (mic != last_mic || !mic_tick) { last_mic = mic; mic_tick = now; }
    if (!playing || !was_playing || stats.writes != last_writes)
        pcm_tick = now;
    last_writes = stats.writes;
    was_playing = playing;
    if (!wanted && !player && !stop_pending)
    {
        guard_reported = false;
        return;
    }
    rt_kprintf("[music] hb op=%s play=%u pause=%u mic=%lu writes=%lu\n",
        operation, playing, paused, (unsigned long)mic, (unsigned long)stats.writes);
    rt_kprintf("[music] mem psram_free=%lu alloc_fail=%lu source_rc=%d\n",
        (unsigned long)music_audio_heap.available_size, (unsigned long)alloc_failures, stats.error);
    const char *reason = NULL;
    rt_tick_t timeout = rt_tick_from_millisecond(MUSIC_GUARD_MS);
    if (now - worker_tick >= timeout) reason = "worker_stall";
    else if (playing && now - mic_tick >= timeout) reason = "mic_stall";
    else if (playing && now - pcm_tick >= timeout) reason = "pcm_stall";
    if (reason && !guard_reported)
    {
        guard_reported = true;
        request_stop();
        rt_kprintf("[music] guard %s rc=%d: stop requested, op=%s\n",
                   reason, -RT_ETIMEOUT, operation);
    }
}

static void music_monitor_entry(void *arg)
{
    while (1)
    {
        rt_thread_mdelay(5000);
        music_monitor_tick(NULL);
    }
}

/* Keep RX valid until the decoder has joined: its initial audio_open2 can
 * still be pending after mp3ctrl_play acknowledges PLAY. Invalidate the RX
 * snapshot before dropping output_lock so another play cannot race teardown. */
void xz_music_audio_shutdown(void (*disable_rx_snapshot)(void))
{
    if (!ready) { disable_rx_snapshot(); return; }
    request_stop();
    rt_mutex_take(&output_lock, RT_WAITING_FOREVER);
    wanted = false;
    close_player_locked();
    disable_rx_snapshot();
    rt_mutex_release(&output_lock);
}

static void music_entry(void *arg)
{
    music_cmd_t cmd;
    /* Let UI/Wi-Fi/audio startup run before even attempting a mount. */
    rt_thread_mdelay(3000);
    mount_sd();
    if (mounted) scan_sd();
    while (1)
    {
        worker_tick = rt_tick_get();
        if (stop_pending)
        {
            stop_pending = false;
            wanted = false;
            close_player();
            report_track();
            rt_kprintf("[music] stopped psram_free=%lu alloc_fail=%lu\n",
                (unsigned long)music_audio_heap.available_size, (unsigned long)alloc_failures);
        }
        if (rt_mq_recv(&music_queue, &cmd, sizeof(cmd), rt_tick_from_millisecond(100)) == RT_EOK)
        {
            int result = execute(&cmd);
            if (cmd.reply)
            {
                cmd.reply->result = result;
                rt_sem_release(&cmd.reply->sem);
            }
            else if (result < 0) rt_kprintf("[music] command failed: %d\n", result);
        }
        if (!stop_pending && wanted && !tts_active && eof_generation == generation && generation)
        {
            int next = (current + 1) % count;
            rt_kprintf("[music] eof gen=%lu next=%d/%d\n", (unsigned long)generation, next + 1, count);
            /* Try each track once; invalid media must not cause a busy loop. */
            int attempts;
            for (attempts = 0; attempts < count; attempts++, next = (next + 1) % count)
                if (!start_track(next)) break;
            if (attempts == count) { wanted = false; report_track(); }
        }
        if (!mounted && !wanted &&
            rt_tick_get() - last_mount_try >= rt_tick_from_millisecond(MOUNT_RETRY_MS))
        {
            if (!mount_sd()) scan_sd();
        }
    }
}

static int music_init(void)
{
    if (rt_mutex_init(&output_lock, "music_out", RT_IPC_FLAG_PRIO)) return -RT_ERROR;
    if (rt_mq_init(&music_queue, "xz_music", music_queue_pool, sizeof(music_cmd_t),
                   sizeof(music_queue_pool), RT_IPC_FLAG_FIFO)) return -RT_ERROR;
    if (rt_thread_init(&music_thread, "xz_music", music_entry, NULL,
                       music_stack, sizeof(music_stack), 24, 10))
    {
        rt_mq_detach(&music_queue);
        return -RT_ERROR;
    }
    if (rt_thread_init(&music_monitor, "music_hb", music_monitor_entry, NULL,
                       monitor_stack, sizeof(monitor_stack), 25, 10))
    {
        rt_thread_detach(&music_thread);
        rt_mq_detach(&music_queue);
        rt_mutex_detach(&output_lock);
        return -RT_ERROR;
    }
    ready = true;
    worker_tick = rt_tick_get();
    rt_thread_startup(&music_monitor);
    rt_thread_startup(&music_thread);
    return RT_EOK;
}
INIT_APP_EXPORT(music_init);

static int submit(unsigned command, const char *keyword, bool value, bool wait)
{
    music_cmd_t cmd = {0};
    music_reply_t reply;
    if (!ready) return -RT_ERROR;
    if (keyword && strlen(keyword) >= sizeof(cmd.keyword)) return -RT_EINVAL;
    cmd.cmd = command;
    cmd.epoch = control_epoch;
    if (keyword) rt_strncpy(cmd.keyword, keyword, sizeof(cmd.keyword));
    if (wait)
    {
        rt_sem_init(&reply.sem, "music_ack", 0, RT_IPC_FLAG_FIFO);
        cmd.reply = &reply;
    }
    int result;
    do {
        result = rt_mq_send(&music_queue, &cmd, sizeof(cmd));
        if (wait && result == -RT_EFULL) rt_thread_mdelay(1);
    } while (wait && result == -RT_EFULL);
    if (wait)
    {
        if (!result)
        {
            rt_sem_take(&reply.sem, RT_WAITING_FOREVER);
            result = reply.result;
        }
        rt_sem_detach(&reply.sem);
    }
    return result;
}

int xz_music_mount(void) { return submit(CMD_MOUNT, NULL, false, true); }
int xz_music_scan(void) { return submit(CMD_SCAN, NULL, false, true); }
int xz_music_play(const char *keyword) { return submit(CMD_PLAY, keyword, false, true); }
void xz_music_stop(void)
{
    if (!ready) return;
    request_stop();
    rt_kprintf("[music] stop requested\n");
}
bool xz_music_is_playing(void) { return playing; }
const char *xz_music_current_track(void) { return basename_of(current_path); }
void xz_music_tts(bool speaking)
{
    if (!ready) return;
    /* Independent of the filesystem queue: only wait for actual music output.
     * Setting the gate also prevents pending play commands starting in TTS. */
    rt_mutex_take(&output_lock, RT_WAITING_FOREVER);
    tts_active = speaking;
    if (player && wanted && paused != tts_active)
    {
        operation = tts_active ? "pause" : "resume";
        if (tts_active) mp3ctrl_pause(player);
        else if (!eof_generation) mp3ctrl_resume(player);
        paused = tts_active;
        report_track();
        operation = "idle";
    }
    rt_mutex_release(&output_lock);
}

static bool contains_any(const char *text, const char *const *words, unsigned n)
{
    for (unsigned i = 0; i < n; i++) if (strstr(text, words[i])) return true;
    return false;
}
#define ANY(text, words) contains_any(text, words, sizeof(words) / sizeof(words[0]))
bool xz_music_handle_text(const char *text)
{
    static const char *const stop[] = {"停止", "停", "别放", "关掉", "关闭"};
    static const char *const stop_object[] = {"音乐", "歌", "播放"};
    static const char *const play[] = {"播放", "放", "来点", "听"};
    static const char *const play_object[] = {"音乐", "歌", "歌曲", "SD", "sd"};
    if (!text) return false;
    /* Stop first: 别放音乐 also contains 放音乐. */
    if (ANY(text, stop) && ANY(text, stop_object))
    {
        xz_music_stop();
        return ready;
    }
    if (ANY(text, play) && ANY(text, play_object))
        return submit(CMD_PLAY, NULL, false, false) == RT_EOK;
    return false;
}

static int xz_music(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "mount")) return xz_music_mount();
    if (argc == 2 && !strcmp(argv[1], "list")) return submit(CMD_LIST, NULL, false, true) < 0 ? -RT_ERROR : 0;
    if (argc >= 2 && argc <= 3 && !strcmp(argv[1], "play")) return xz_music_play(argc == 3 ? argv[2] : NULL);
    if (argc == 2 && !strcmp(argv[1], "stop")) { xz_music_stop(); return 0; }
    rt_kprintf("xz_music mount | list | play [keyword] | stop\n");
    return -RT_EINVAL;
}
MSH_CMD_EXPORT(xz_music, SD MP3 player);
