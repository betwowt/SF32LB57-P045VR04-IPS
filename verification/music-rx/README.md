# SD MP3 导致 PDM ISR 断言：应用侧修复与复测

本次仅编译，没有烧录、打开串口或占用 COM8。真机结果仍待用户复测。

## 根因和修复范围

原调用顺序：

1. `xiaozhi_audio.c` 启动常驻 `AUDIO_TYPE_LOCAL_MUSIC / AUDIO_TXRX` 客户端，读写均为 16 kHz、mono，`is_need_3a=1`。
2. `xiaozhi_music.c:start_track()` 启动 SDK MP3 控制器；SDK `audio_mp3ctrl.c:1158–1189` 用零初始化的 `audio_parameter_t` 只填写 TX 字段，之后调用 `audio_open2(... AUDIO_TX ...)`。示例 MP3 是 44.1 kHz、stereo，所有 RX 字段及 `is_need_3a` 仍为零。
3. `audio_server.c:2493` 先把设备的 `rx_samplerate` 赋为新客户端的零值。`hardware_device_open()` 继续调用 `audio_device_speaker_open()`。
4. `audio_server.c:1498–1502` 无条件覆盖共享的 `is_need_3a`、`rx_channels`、`rx_samplerate`、`mic_used`、`all_mic_channels`；这一赋值发生在引用计数/`need_*_init` 判断前。已有 TXRX 引用，所以 `need init tx=0 rx=0`，不会重新配置硬件，PDM 仍按原帧长产生中断，而软件 RX 字段已变成零。
5. 下一次 `mic_rx_ind()` 在 `audio_server.c:3028` 校验 `rx_channels * PDM_MOMO_FRAME_SIZE == size`，左侧为零，直接 ISR 断言。除此之外，`is_need_3a=0` 还会破坏 AEC 路径，故仅补通道数也不完整。

`mount/list` 不创建音频客户端；唤醒音、TTS 和 `xz_say` 使用原有 TXRX 输出，所以只有 MP3 加入时暴露此路径。现场 `fatal error on ISR` 与上述执行路径一致；不需要用关闭断言掩盖问题。

修改文件：

- `xiaozhi_audio.c`：保存实际成功打开的 TXRX 参数，提供只读快照和单调递增的麦克风回调计数。麦克风显式销毁时，先让音乐退出，之后在同一输出锁内使快照失效，避免待启动解码器与 RX 关闭竞争。
- `xiaozhi_music_source.c/.h`：音乐始终保持 `AUDIO_TX`；复制当前 RX owner 的全部 RX 字段、`is_need_3a` 和 AGC 标志。保留 MP3 原始 TX 采样率/声道/缓存，固定使用 speaker；不关闭或重开 RX。
- `xiaozhi_music_mp3_port.h`、`app/project/SConscript`：只对 SDK `audio_mp3ctrl.c` 的 `audio_open2/audio_write` 引入应用适配入口；其余客户端不受此入口影响。SDK 音频源码和断言均未修改。
- `xiaozhi_music.c/.h`：独立停止请求、取消此前排队的 play、先 pause 再 close 以避免 SDK 淡出等待、EOF 日志、独立低优先级监测线程及安全停止请求。保留现有 EOF 回调仅置位的设计。
- `xiaozhi_music_server_alloc.h`：仅澄清注释，分配策略未改变。

软件重采样仍使用 SDK `audio_server.c:3320 audio_write_resample()`：源格式保留 44100/2，目标仍是常驻 owner 的 16000/1。没有把 44.1 kHz PCM 伪装成 16 kHz PCM。

音乐加入时设备引用从 TX=1/RX=1 变成 TX=2/RX=1，`need_init` 为 0/0；暂停、停止和换曲关闭音乐源后只回到 TX=1/RX=1，`need_deinit` 为 0/0。SDK 暂停会关闭音乐的客户端对象，恢复会重新加入一个 TX 客户端；常驻 TXRX owner 始终保留，所以不会重配设备或改变 PDM 帧长。新音乐客户端没有 `is_3a_opened` 所有权，因此音乐关闭不会关闭 owner 的 3A。

未修改圆脸绘制、mic_gain/ADC 增益、Standby/唤醒/退下状态语义，以及 `xz_say`、`xz_btn_sim`、`xz_wsstat` 三个命令的实现。

## allocator 逐项核对

实际宏替换是 `rt_*` / `audio_mem_*`，并非全局 libc `malloc/calloc/free`。当前配置为 `RT_USING_SMALL_MEM + RT_USING_MEMHEAP`，未定义 `SOLUTION`、`SYS_HEAP_IN_PSRAM`；专用池不链接到系统堆。

| audio_server 中的分配对象/位置 | 真实来源 | 分配与释放上下文及配对 |
| --- | --- | --- |
| client `handle`，3799；`ring_pool`，3833 | `audio_mem_calloc → rt_calloc → xz_music_calloc → rt_memheap_alloc(mp3_psram)` | MP3 线程调用 `audio_open2` 创建；audiosvr 的 `audio_client_stop` 在 2864/2867 或 2873/2884 释放，`xz_music_free_any → rt_memheap_free`。RX owner 的同类对象同池，属于常驻基线。 |
| OPEN 命令 3877；关闭/切设备命令 4208/4295/4324；micbias 命令 1430/1459 | 同上，PSRAM 池 | 发起 API 的线程分配；audiosvr `audio_process_cmd:2932` 取下命令并释放后再处理，不保留命令指针。 |
| `tx_data_tmp` 1607；`rx_data_tmp/rx_data_tmp2` 1625/1627 | `audio_mem_malloc → rt_malloc`，内部系统堆 | audiosvr 创建；1971/1973/1992 在相应引用归零且停止硬件后经 `xz_music_free_any → rt_free` 释放。音乐期间 owner 持有引用，均不释放。 |
| `mixed_4_channel` 1617，ANYKA 条件分支 | 同上，内部系统堆 | 1984 释放；本配置 ANYKA 未启用。 |
| `hfp_dev_input_buf` 2117 | 同上，内部系统堆 | 2190 释放；当前 speaker 播放路径不使用 HFP 分支。 |
| `device->tx_mixed_pool` 2092 | `resample_malloc → xz_music_alloc`，PSRAM 池 | audiosvr 创建，最后一个 TX 退出时 2182 经 `resample_free → xz_music_free` 释放。音乐停止后 owner 仍保留 TX，混音池继续常驻。 |
| client 的 `resample` 及 `dst`，`audio_write_resample` 调用 | `sifli_resample_open/process → xz_music_alloc`，PSRAM 池 | MP3 线程写入时创建/扩容；audiosvr 关闭客户端时 `sifli_resample_close → xz_music_free`。不存在把此指针传给系统 `rt_free` 的路径。 |
| `p_audio_dump` 4670 | libc `calloc → _calloc_r →` 原始 `rt_calloc`，内部系统堆 | 4689 libc `free → _free_r →` 原始 `rt_free`。由目标 ELF 反汇编核对，不受 server 头中的宏影响；普通 MP3 路径没有启动 audio dump。 |
| event、mutex、sem、动态线程/栈、ringbuffer 对象 | 各 RTOS 创建函数自身编译单元中的系统堆 | 强制 include 不会传播到被调用函数内部；按对应 delete/destroy/RTOS idle 回收。 |

`process_speaker_tx:456–542` 从 PSRAM 混音环形缓冲 **复制** 到内部 `tx_data_tmp`，交给 DMA 的是后者。没有把音乐池作为该硬件 DMA staging buffer。

MP3 控制器、解码器 `buffers.c`、`sifli_resample.c` 的软件分配/释放由目标文件未定义符号核对，均成对使用 `xz_music_alloc/calloc/free`。MP3 退出顺序为关闭音频客户端 → 释放 decoder → 关闭 fd → 释放输入缓存、PCM 缓冲、待处理命令 → 删除 IPC 对象 → 通知 CLOSE → 释放控制器；本配置动态线程/栈由 RTOS idle 回收。不能仅凭立即打印的 `free` 要求 idle 回收已完成。

`xz_music_free_any` 只按唯一静态池的地址区间路由；池不与系统堆重叠，池内地址走 memheap，其余（包括 NULL）走原始 `rt_free`。`memheap.c:290/774` 分配和释放持有该 heap 的 semaphore，释放根据块头 `pool_ptr` 找原池，不依赖线程身份。审查没有发现跨线程走错池或 DMA/CPU 指针混用；这支持排除它们作为当前确定性 ISR 断言的原因，但不是长期堆完整性实机证明。

## 文件 I/O、回调和控制台

`mp3_audio_server_callback:409` 的缓存事件只发送 DECODE 事件；实际 `read` 在 MP3 解码线程 `load_file_to_cache:349`，不在 PDM ISR 或 audiosvr 回调中进行。应用 EOF 回调只记录 generation，下一首由 xz_music 线程处理；`mp3ctrl_ioctl(LOOP_TIMES)` 也在该工作线程调用。没有 EOF 回调重入 `mp3ctrl_*`。

mount/scan 不持有音乐输出锁；播放时 list 不 rescan。文件打开/探测与解码文件读取仍属于 SDK 的同步 DFS 路径；未模拟坏卡/拔卡时 SDMMC/FatFs 的底层超时。检查到的代码没有持有 `rt_kprintf`/ulog 锁跨越文件读取或 mp3ctrl 等待；现场 ISR 断言已足以解释控制台静默。没有通过硬件验证所有独立的控制台锁故障。

心跳运行在独立 `music_hb` 线程（优先级 25，静态 PSRAM 栈 3 KB），每 5 秒采样，避免在 ISR 或共享高优先级软定时器线程里打印。它可以定位音乐工作线程失去进展，不能作为硬件 ISR 活性的独立证明，也不能在 fatal ISR/调度器停止/全局日志锁阻塞后保证继续输出。

## 串口复测判据

先在 Standby 测一次，再经原有唤醒流程进入 Listening 测一次；两种状态都执行以下步骤。

1. 播放前执行 `free`、`xz_wsstat`、`xz_music list` 记录基线。
2. `xz_music play` 后提示符返回，接着执行 `free`、`xz_wsstat`，应立即响应。首次加入音乐时应见：

   ```text
   [music] source TX=44100/2 RX_KEEP=16000/1 mic=0 3a=1
   ... need init tx=0 rx=0
   ... (tx rx): ch[1,1] rate(16000 16000)
   ... hardware_device_open out: rx=1 tx=2 b=1
   ```

   不能再出现音乐加入把设备变为 `ch[1,0]`、RX rate=0、3a=0 的日志；不能重新出现 config pdm/config_rx/3A open。服务端重采样日志目标应保持 16000/1。
3. 连续播放至少 60 秒，多次执行 `free`、`xz_wsstat`；保持唤醒词、VAD、AEC 上行可用。每 5 秒见 `[music] hb ... mic=... writes=...`，其中 mic 持续增加；正常播放时 writes 增加。TTS 暂停时允许 writes 停止，但 mic 必须继续增加；恢复后 writes 再增长，设备仍为原 16 kHz mono RX/3A。
4. 单首 22 秒示例在 60 秒内应至少观察两次 `[music] eof gen=... next=1/1` 后重新播放。多首时 next 应递增并循环；换曲期间控制台、mic、`xz_wsstat` 仍工作。
5. `xz_music stop` 应立即打印 `[music] stop requested` 并返回提示符。正常清理后出现 `[music] stopped psram_free=... alloc_fail=0`；等待约 1 秒使 RTOS idle 回收，再执行 `free`。内部动态堆应回落，PSRAM pool 应回到稳定的播放前基线，反复 play/stop 不应累计下降。RX owner、其环形缓存、混音池及 xiaozhi 重采样器常驻，不要求专用池回到全空。
6. 全程不得出现 `Assertion failed`、`fatal error on ISR`、`malloc fail`、`alloc fail`、`WS_TX_STALL`、`[music] guard` 或控制台静默。

新增 grep 标签：

- `[music] source TX=... RX_KEEP=...`：加入/恢复/换曲使用的真实源格式及保留的 RX 配置。
- `[music] hb op=... play=... pause=... mic=... writes=...`：线程状态、麦克风帧和成功 PCM 写入次数。
- `[music] mem psram_free=... alloc_fail=... source_rc=...`：池余量及错误；`source_rc` 是上一次负写入结果或最近打开结果。
- `[music] guard worker_stall|mic_stall|pcm_stall rc=-2`：至少约 10 秒无进展、5 秒采样粒度，发出独立停止请求。不会强杀 audiosvr/MP3 线程或释放仍在使用的缓冲；SDK/驱动永久阻塞时只诊断，无法保证清理完成。
- `[music] start rejected/source reject rc=-7`：RX owner 不可用、停止中或参数不符合 TX-only speaker 约束；正常启动与生命周期同步避免在解码线程里遇到此拒绝。
- `[music] open failed rc=-1`、`alloc fail ... rc=-5`：文件打开或内存分配失败。

没有关闭任何现有音频日志；不需要额外开关恢复。原 `xz_say/xz_btn_sim/xz_wsstat` 使用方法不变。

## 已完成验证和仍需真机排除的风险

运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File verification/music-rx/test.ps1
python verification/music-rx/verify_link.py
```

`build.log` 记录固件编译结果；host 测试直接编译生产参数适配器，使用真实 SDK `audio_server.h`，验证零 RX 回归模型、17 次不同格式/重复加入、参数复制、TX-only、进度计数和拒绝路径。host 测试没有运行真实 RTOS、DMA、AEC 或状态机，不能代替 Standby/Listening 真机测试。`link-evidence.json` 保存真实 ARM 对象调用符号及 ELF/分段固件 SHA256。

构建仍有工具链链接警告（GNU-stack/RWX；ftab 的 newlib syscall stub 和 entry；acpu image info 未定义）。本次没有新增 C 编译告警，编译/链接成功；警告未在此次修复中扩展处理。

仍未排除：真实 SDMMC/FatFs 长时间或坏卡 I/O 阻塞；同时运行 MP3、AEC/Opus、Wi-Fi 的 CPU/内存峰值和长期碎片；播放音乐时唤醒/VAD/AEC 的实际声学效果；独立的全局日志锁故障。SDK `mp3ctrl_*`/audio API 仍有永久等待路径，监测只能安全请求停止，不能可靠地强行恢复一个底层永久阻塞。异常增加第三个 speaker TX 客户端/切换蓝牙设备的策略未覆盖；本修复针对当前两客户端、常驻 TXRX + SD MP3 的工作方式。SDK 升级后应重新核对以上无条件赋值及分配释放路径。
