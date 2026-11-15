/*
 * xiaozhi AI chatbot demo for SF32LB57-DevKit-LCD-V1 + P045VR04-IPS.
 *
 * Networking is Wi-Fi (AIC8800D40LS-N on SDIO2) instead of the BLE PAN
 * transport used by the upstream LB52 port.  The rest of the application
 * (WebSocket protocol, Opus/3A audio, LVGL UI, MCP) is reused unchanged.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdbool.h>
#include <string.h>
#include "bf0_hal.h"
#include "drv_io.h"
#include "mem_section.h"
#include "board.h"
#include "lwip/netif.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "xiaozhi/xiaozhi_websocket.h"
#include "xiaozhi/xiaozhi_client_public.h"
#include "xiaozhi/xiaozhi_ui.h"
#include "xiaozhi/xiaozhi_audio.h"
#include "xiaozhi/xiaozhi_mqtt.h"
#include "xiaozhi/iot/iot_c_api.h"
#include "xiaozhi/weather/weather.h"
#include "xiaozhi/board/board_hardware.h"
#include "xiaozhi/xz_provision.h"

/* AIC8800 port-internal API (wifi_driver_action for the chip power cycle). */
#include "wifi_port_priv.h"

#include "lv_timer.h"
#include "lv_display.h"

#define XZ_UI_THREAD_STACK_SIZE   (6144)
#define XZ_UI_THREAD_PRIO         30
#define XZ_WIFI_THREAD_STACK_SIZE (4096)
#define XZ_WIFI_THREAD_PRIO       20

/* Application event codes shared with xiaozhi_websocket.c */
#define XZ_EVT_WEBSOCKET_RECONNECT  4
#define XZ_EVT_BUTTON_PRESS         5
#define XZ_EVT_BUTTON_RELEASE       6
#define XZ_EVT_UPDATE_WEATHER_TIME  11

extern rt_mailbox_t g_ui_task_mb;
extern rt_mailbox_t g_button_event_mb;
extern lv_timer_t *ui_sleep_timer;
extern void xz_do_button_press(void);
extern void xz_do_button_release(void);
extern rt_tick_t last_listen_tick;
extern BOOL g_pan_connected;
extern BOOL first_pan_connected;
extern uint8_t Initiate_disconnection_flag;

extern void xz_ws_audio_init(void);
extern void xz_ws_button_init(void);
extern void xz_ws_button_init2(void);
extern void xiaozhi2(int argc, char **argv);
extern int xiaozhi_time_weather_init(void);
extern void xiaozhi_time_weather(void);

/* AIC8800 platform entry points (platform/wifi/wifi_port*.c) */
extern void aic_wifi_demo_test(void);
extern int app_wlan_sta_start(uint8_t *ssid, uint8_t *pw, int timeout_ms);

/* The chip occasionally never raises the ready flag after the CALIB firmware
 * upload, which leaves aic_wifi_demo_test() polling GetWiFiInitStat() forever.
 * The module is powered through the AW9523 expander, so a chip power cycle
 * (AIC_DRIVER_REBOOT_PU) restarts the download without a physical unplug. */
#define XZ_AIC_INIT_TIMEOUT_S   (240)
#define XZ_AIC_PWR_TRIES        (3)
static char s_aic_wd_stack[1024];
static struct rt_thread s_aic_wd_thread;

static void xz_aic_watchdog_entry(void *parameter)
{
    (void)parameter;

    for (int attempt = 1; attempt <= XZ_AIC_PWR_TRIES; attempt++)
    {
        for (int waited = 0; waited < XZ_AIC_INIT_TIMEOUT_S; waited += 5)
        {
            if (GetWiFiInitStat())
                return;
            rt_thread_mdelay(5000);
        }

        rt_kprintf("xz: AIC8800 init stalled %ds, power cycling (%d/%d)\n",
                   XZ_AIC_INIT_TIMEOUT_S, attempt, XZ_AIC_PWR_TRIES);
        wifi_driver_action(AIC_DRIVER_REBOOT_PU);
        rt_thread_mdelay(3000);
    }

    rt_kprintf("xz: AIC8800 not ready after %d power cycles\n", XZ_AIC_PWR_TRIES);
}

static void xz_aic_watchdog_start(void)
{
    rt_thread_init(&s_aic_wd_thread, "aic_wd", xz_aic_watchdog_entry, RT_NULL,
                   s_aic_wd_stack, sizeof(s_aic_wd_stack),
                   XZ_WIFI_THREAD_PRIO + 2, 10);
    rt_thread_startup(&s_aic_wd_thread);
}

rt_mailbox_t g_bt_app_mb;

/* Network/battery state shared with the xiaozhi UI and client modules. */
BOOL g_pan_connected = FALSE;
BOOL first_pan_connected = FALSE;
uint8_t Initiate_disconnection_flag = 0;
bool g_skip_startup = true;
bool low_battery_shutdown_triggered = true;
bool lowpower_shutdown_state = true;
bool g_low_power_mode = false;
rt_mailbox_t g_battery_mb;

static struct rt_thread s_ui_thread;
static struct rt_thread s_wifi_thread;

#if defined(__CC_ARM) || defined(__CLANG_ARM)
L2_RET_BSS_SECT_BEGIN(xz_ui_stack)
static uint32_t s_ui_stack[XZ_UI_THREAD_STACK_SIZE / sizeof(uint32_t)];
L2_RET_BSS_SECT_END
L2_RET_BSS_SECT_BEGIN(xz_wifi_stack)
static uint32_t s_wifi_stack[XZ_WIFI_THREAD_STACK_SIZE / sizeof(uint32_t)];
L2_RET_BSS_SECT_END
#else
static uint32_t s_ui_stack[XZ_UI_THREAD_STACK_SIZE / sizeof(uint32_t)] L2_RET_BSS_SECT(xz_ui_stack);
static uint32_t s_wifi_stack[XZ_WIFI_THREAD_STACK_SIZE / sizeof(uint32_t)] L2_RET_BSS_SECT(xz_wifi_stack);
#endif

static const char *xz_wifi_ssid(void)
{
#ifdef CONFIG_XZ_WIFI_SSID
    return CONFIG_XZ_WIFI_SSID;
#else
    return "";
#endif
}

static const char *xz_wifi_password(void)
{
#ifdef CONFIG_XZ_WIFI_PASSWORD
    return CONFIG_XZ_WIFI_PASSWORD;
#else
    return "";
#endif
}

/* The Wi-Fi port keeps the DHCP lease inside the AIC8800 driver and never
 * installs a resolver, so lwIP's dns_gethostbyname() rejects every query with
 * ERR_VAL ("no server").  Publish one: DHCP option 6 if the stack has it,
 * otherwise the default gateway (routers answer DNS on their own address). */
static void xz_dns_from_dhcp(void)
{
    extern int aicwf_net_set_dns(uint32_t dns_server);
    const ip_addr_t *cur = dns_getserver(0);
    ip4_addr_t dns4;

    if (cur && !ip_addr_isany(cur))
    {
        rt_kprintf("xz: dns already set %s\n", ipaddr_ntoa(cur));
        return;
    }

    if (netif_default && !ip4_addr_isany_val(*netif_ip4_gw(netif_default)))
        dns4 = *netif_ip4_gw(netif_default);
    else
        ip4addr_aton("223.5.5.5", &dns4);

    aicwf_net_set_dns(ip4_addr_get_u32(&dns4));
    rt_kprintf("xz: dns server %s\n", ipaddr_ntoa((const ip_addr_t *)&dns4));
}

static void xz_net_up(void)
{
    g_pan_connected = TRUE;
    first_pan_connected = TRUE;
    Initiate_disconnection_flag = 0;

    rt_kprintf("xz: network ready\n");
    xz_dns_from_dhcp();
    rt_mb_send(g_bt_app_mb, XZ_EVT_UPDATE_WEATHER_TIME);

    xiaozhi_ui_update_ble("open");
    xiaozhi_ui_chat_status("初始化...");
    xiaozhi_ui_standby_chat_output("初始化 请稍等...");
    xiaozhi_ui_update_emoji("neutral");
    /* Optional cloud registration / OTA check is a no-op on this board. */
    register_device_with_server();

    xz_ws_button_init();

    if (!ui_sleep_timer)
    {
        ui_sleep_timer = lv_timer_create(ui_sleep_callback, 40000, NULL);
    }

    xiaozhi_ui_standby_chat_output("请按键连接小智...");
    lv_display_trigger_activity(NULL);

    /* Fetch the OTA/activation status once the link is up: an unpaired device
     * only learns its 6-digit code from this response, and the websocket token
     * comes from the same call. Without it the code is never shown. */
    rt_mb_send(g_bt_app_mb, XZ_EVT_WEBSOCKET_RECONNECT);
}

static void xz_wifi_thread_entry(void *parameter)
{
    char ssid[40] = {0};
    char pwd[80] = {0};
    int ret;

    (void)parameter;

    extern uint8_t mmcsd_get_stat(void);
    rt_kprintf("xz: sdio detected=%u\n", mmcsd_get_stat());

    rt_kprintf("xz: AIC8800 init\n");
    xz_aic_watchdog_start();
    aic_wifi_demo_test();

    if (!xz_prov_load(ssid, sizeof(ssid), pwd, sizeof(pwd)))
    {
        rt_kprintf("xz: no stored Wi-Fi, entering provisioning\n");
        xz_prov_start();
        return;
    }

    rt_kprintf("xz: AIC8800 ready, connecting to '%s'\n", ssid);
    /* 有凭据就一直重连，不要因为一次关联超时就永久停在 SoftAP 配网模式：
     * AIC8800 首次扫描/关联经常要几十秒，3×15 s 放弃会让设备永远回不来。 */
    for (int attempt = 0;; attempt++)
    {
        ret = app_wlan_sta_start((uint8_t *)ssid, (uint8_t *)pwd, 20000);
        rt_kprintf("xz: app_wlan_sta_start ret=%d (attempt %d)\n", ret,
                   attempt + 1);
        if (ret >= 0)
        {
            xz_net_up();
            return;
        }
        xiaozhi_ui_chat_status("重连网络中...");
        rt_thread_mdelay(5000);
    }
}

static void xz_ui_thread_entry(void *parameter)
{
    (void)parameter;
    xiaozhi_ui_task(NULL);
}

#ifdef RT_USING_FINSH
static int xz_wifi_join(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage: xz_wifi_join <ssid> [password]\n");
        return -1;
    }

    int ret = app_wlan_sta_start((uint8_t *)argv[1],
                                 (uint8_t *)(argc > 2 ? argv[2] : ""), 15000);
    rt_kprintf("join %s ret=%d\n", argv[1], ret);
    if (ret >= 0)
    {
        xz_prov_save(argv[1], argc > 2 ? argv[2] : "");
        xz_net_up();
    }
    return ret;
}
MSH_CMD_EXPORT(xz_wifi_join, join Wi-Fi and start xiaozhi);

static int xz_wifi_reset(int argc, char **argv)
{
    (void)argc; (void)argv;
    xz_prov_erase();
    rt_kprintf("Wi-Fi credentials erased, rebooting\n");
    rt_thread_mdelay(200);
    HAL_NVIC_SystemReset();
    return 0;
}
MSH_CMD_EXPORT(xz_wifi_reset, erase Wi-Fi credentials and reboot);
/* 只写凭据、不尝试连接，然后重启让 Wi-Fi 线程用新 SSID 重新关联。 */
static int xz_wifi_set(int argc, char **argv)
{
    if (argc < 3)
    {
        rt_kprintf("usage: xz_wifi_set <ssid> <password>\n");
        return -1;
    }
    xz_prov_save(argv[1], argv[2]);
    rt_kprintf("Wi-Fi set to '%s', rebooting\n", argv[1]);
    rt_thread_mdelay(300);
    HAL_NVIC_SystemReset();
    return 0;
}
MSH_CMD_EXPORT(xz_wifi_set, save Wi-Fi credentials and reboot);
extern int g_xz_uplink_on;
static int xz_uplink(int argc, char **argv)
{
    if (argc > 1)
    {
        g_xz_uplink_on = (argv[1][0] == '1' || argv[1][0] == 'o');
    }
    rt_kprintf("uplink=%d\n", g_xz_uplink_on);
    return 0;
}
MSH_CMD_EXPORT(xz_uplink, toggle mic uplink 0/1);



/* Same path as a button press: open the xiaozhi websocket and start talking. */
static int xz_talk(int argc, char **argv)
{
    extern void xiaozhi_ws_connect(void);
    (void)argc; (void)argv;
    xiaozhi_ws_connect();
    return 0;
}
MSH_CMD_EXPORT(xz_talk, open the xiaozhi websocket like a button press);

/* The AIC8800 is powered through the AW9523 expander, so it can be power
 * cycled from software: AIC_DRIVER_REBOOT_PU cuts the supply, waits, restores
 * it and re-runs aic_wifi_init(). Use this when the firmware download stalls
 * before the ready handshake (GetWiFiInitStat() stays 0 forever). */
static int xz_aic_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    rt_kprintf("AIC8800: power cycle + re-init\n");
    wifi_driver_action(AIC_DRIVER_REBOOT_PU);
    return 0;
}
MSH_CMD_EXPORT(xz_aic_reboot, power cycle the AIC8800 WiFi chip and re-init);

/* LVGL 官方快照：把当前活动屏幕渲染到内存缓冲再 dump，用于不依赖相机、
 * 也不依赖面板扫描输出的一次"软件截图"。 */
#include "lvgl.h"
#include "lv_snapshot.h"

static int xz_snap(int argc, char **argv)
{
    (void)argc; (void)argv;

    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!buf)
    {
        rt_kprintf("snap failed\n");
        return -1;
    }

    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    const uint8_t *p = buf->data;

    rt_kprintf("SNAP %u %u %u %u\n", w, h, stride, (unsigned)buf->data_size);
    for (uint32_t y = 0; y < h; y++)
    {
        for (uint32_t x = 0; x < stride; x += 32)
        {
            char line[96];
            uint32_t n = (stride - x) < 32 ? (stride - x) : 32;
            int q = rt_snprintf(line, sizeof(line), "%06x:", y * stride + x);
            for (uint32_t i = 0; i < n; i++)
            {
                q += rt_snprintf(line + q, sizeof(line) - q, "%02x", p[x + i]);
            }
            rt_snprintf(line + q, sizeof(line) - q, "\n");
            rt_kprintf("%s", line);
        }
        p += stride;
    }
    rt_kprintf("SNAPEND\n");

    lv_draw_buf_destroy(buf);
    return 0;
}
MSH_CMD_EXPORT(xz_snap, dump an LVGL snapshot of the active screen as hex);

/* Dump the LCD framebuffer as hex over the console so the host can rebuild the
 * exact picture the panel is scanning out (480x854 RGB565 = 1.6 MB of text). */
static int xz_shot(int argc, char **argv)
{
    struct rt_device_graphic_info info;
    rt_device_t dev = rt_device_find("lcd");
    extern LCDC_HandleTypeDef *get_drv_lcd_handler(void);

    (void)argc; (void)argv;
    rt_memset(&info, 0, sizeof(info));

    if (dev)
        rt_device_control(dev, RTGRAPHIC_CTRL_GET_INFO, &info);

    uint32_t w = info.width;
    uint32_t h = info.height;
    uint32_t bpp = info.bits_per_pixel ? info.bits_per_pixel : 16;
    const uint8_t *fb = info.framebuffer;

    /* The lcd_fb driver keeps the scan-out buffer in the LCDC layer, which is
     * what the panel actually displays. */
    LCDC_HandleTypeDef *hlcdc = get_drv_lcd_handler();
    if (hlcdc && hlcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data)
    {
        fb = hlcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data;
        w = hlcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_area.x1 -
            hlcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_area.x0 + 1;
        h = hlcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_area.y1 -
            hlcdc->Layer[HAL_LCDC_LAYER_DEFAULT].data_area.y0 + 1;
    }

    if (!fb || !w || !h)
    {
        rt_kprintf("no lcd framebuffer\n");
        return -1;
    }

    uint32_t stride = w * (bpp / 8);

    rt_kprintf("SHOT %u %u %u %p\n", w, h, bpp, fb);

    /* No thread freeze: suspending the LVGL loop mid-frame exposes a torn
     * panel buffer, not what a settled screen looks like. */

    for (uint32_t y = 0; y < h; y++)
    {
        const uint8_t *row = fb + y * stride;
        for (uint32_t x = 0; x < stride; x += 32)
        {
            char line[80];
            uint32_t n = (stride - x) < 32 ? (stride - x) : 32;
            uint8_t sum = 0;
            int p = rt_snprintf(line, sizeof(line), "%06x:", y * stride + x);

            for (uint32_t i = 0; i < n; i++)
            {
                sum ^= row[x + i];
                p += rt_snprintf(line + p, sizeof(line) - p, "%02x", row[x + i]);
            }
            rt_snprintf(line + p, sizeof(line) - p, " %02x\n", sum);
            rt_kprintf("%s", line);
        }
    }
    rt_kprintf("SHOTEND\n");
    return 0;
}
MSH_CMD_EXPORT(xz_shot, dump the LCD framebuffer as hex);
#endif

int main(void)
{
    check_poweron_reason();

    g_button_event_mb = rt_mb_create("btn_evt", 8, RT_IPC_FLAG_FIFO);
    g_bt_app_mb = rt_mb_create("xz_evt", 8, RT_IPC_FLAG_FIFO);
    g_battery_mb = rt_mb_create("battery_level", 1, RT_IPC_FLAG_FIFO);
    RT_ASSERT(g_button_event_mb != RT_NULL);
    RT_ASSERT(g_bt_app_mb != RT_NULL);
    RT_ASSERT(g_battery_mb != RT_NULL);

    rt_kprintf("\n[xiaozhi] SF32LB57-DevKit-LCD / P045VR04-IPS\n");

    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, VOL_DEFAULE_LEVEL);
    xz_set_lcd_brightness(LCD_BRIGHTNESS_DEFAULT);
    iot_initialize();
    xiaozhi_time_weather_init();
    xz_ws_audio_init();
    xz_ws_button_init2();
    set_pinmux();

    if (rt_thread_init(&s_ui_thread, "xz_ui", xz_ui_thread_entry, NULL,
                       s_ui_stack, sizeof(s_ui_stack),
                       XZ_UI_THREAD_PRIO, 10) != RT_EOK)
    {
        rt_kprintf("xz: failed to init UI thread\n");
        return -1;
    }
    rt_thread_startup(&s_ui_thread);

    if (rt_thread_init(&s_wifi_thread, "xz_wifi", xz_wifi_thread_entry, NULL,
                       s_wifi_stack, sizeof(s_wifi_stack),
                       XZ_WIFI_THREAD_PRIO, 10) != RT_EOK)
    {
        rt_kprintf("xz: failed to init Wi-Fi thread\n");
        return -1;
    }
    rt_thread_startup(&s_wifi_thread);

    while (1)
    {
        rt_uint32_t value;

        if (rt_mb_recv(g_bt_app_mb, (rt_uint32_t *)&value, RT_WAITING_FOREVER) != RT_EOK)
            continue;

        switch (value)
        {
        case XZ_EVT_WEBSOCKET_RECONNECT:
            xiaozhi2(0, NULL);
            break;
        case XZ_EVT_UPDATE_WEATHER_TIME:
            xiaozhi_time_weather();
            break;
        case XZ_EVT_BUTTON_PRESS:
            xz_do_button_press();
            break;
        case XZ_EVT_BUTTON_RELEASE:
            xz_do_button_release();
            break;
        default:
            break;
        }
    }

    return 0;
}
