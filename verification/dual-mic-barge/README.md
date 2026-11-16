双麦和本地说话打断已实现，仅做本机编译和主机回归；没有烧录，没有打开串口，也没有访问 COM8。

2026-11-16 fix7：参考低于 50 后保持最近有效参考 300 ms，期间继续执行 DTD；同步异常仅打断原连续窗口，不清空保持值。新增 `refh`/`ref_age` 日志。编译与主机回归已通过，当前交付见 [tail-fix.md](tail-fix.md)，下文为历史说明。

2026-11-16 fix5：PDM 增益在所有麦克风打开点前后设为 44（相对默认 56 修剪 6 dB）；KWS 私有队列和上行 PCM 副本饱和乘 2，barge/VAD/post 度量不变。ARM_MIN_PRE=120、ARM_RATIO=250、IN_RMS=150；保留 fix4 保护。当前交付见 [gain-fix.md](gain-fix.md)，fix4 以下为历史说明。

2026-11-16 fix4：原始 L/R 任一路 peak >= 32000 即拒绝该帧；删除 quiet 武装，只接受真实回声下连续收敛；默认私有播放音量从 8 降为 6（stock 表 -4 dB），保留人声采集增益。当前交付、余量估算与待测限制见 [clip-fix.md](clip-fix.md)。quiet-fix.md、convergence-fix.md 和 aec-fix.md 均为历史说明，quiet 路径已经删除。

麦克风两处 AEC 打开点是 `app/src/xiaozhi/xiaozhi_audio.c` 的 `xz_aec_mic_open()` 和 `xz_audio_decoder_encoder_open()`，后者也用于累计 TTS 时间到达阈值后的重建。两处参数完全一致：

```c
pa.read_bits_per_sample = 16;
pa.read_channnel_num = 2;
pa.read_which_mic = AUDIO_MIC_ALL;
pa.read_samplerate = 16000;
pa.read_cache_size = 0;
pa.write_bits_per_sample = 16;
pa.write_channnel_num = 1;
pa.write_samplerate = 16000;
pa.write_cache_size = 32000;
pa.is_need_3a = 1;
pa.disable_uplink_agc = 1;
```

当前 SDK 的 `mic_used` 实际选择 PDM 设备：直接传 `AUDIO_MIC_ALL` 会查找 PDM2 并等待两条总线。当前 WebRTC 的 `audio_3a_uplink()` 又要求 320 字节单声道，因此必须同时使用项目补丁 [0005-pdm1-stereo-barge-in.patch](../../patches/0005-pdm1-stereo-barge-in.patch)。补丁已应用于本机 `../SiFli-SDK`；在迁移到另一个 SDK 工作副本时，从本工程根目录执行：

```powershell
git -C ../SiFli-SDK apply --check ../SF32LB57-P045VR04-IPS/patches/0005-pdm1-stereo-barge-in.patch
git -C ../SiFli-SDK apply ../SF32LB57-P045VR04-IPS/patches/0005-pdm1-stereo-barge-in.patch
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

补丁由隐藏 Kconfig `CONFIG_XZ_PDM1_STEREO` 限定，只在 PDM1 已开、PDM2 未开、WebRTC 已启用时默认生效。客户端参数仍为 `2 / AUDIO_MIC_ALL`，SDK 内部把设备选择归一到 PDM1，保持 PDM1 两声道采集，不启用 PDM2。采集流程是：

```text
PDM1 640 B / 10 ms (160 对 L/R)
  -> mic_callback(as_callback_cmd_mic_stereo_before_3a)
  -> 原始每通道电平 + 定点下混，320 B / 160 点
  -> 现有 WebRTC AEC（保留启动时 6 帧消噪保护）
  -> mic_callback(as_callback_cmd_data_coming)，320 B 单声道
  -> 本地 KWS / VAD / RMS / 上行 Opus
```

`mic_callback()` 也接受另一个兼容 SDK 提供的 640 字节 AEC 后立体声，先下混到 160 点；320 字节则直接按已下混单声道处理。其他长度、空指针被丢弃并统计 `bad_len`，同时清理未编码上行并重置连续人声帧计数。没有把不确定长度当作两个 10 ms 帧处理的分支。

默认 `(L+R)/2` 用 32 位有符号加法，避免 16 位溢出；可改成逐采样取幅值较大的通道。固定的 320 字节下混缓冲在 `L2_RET_BSS_SECT`，没有新增逐帧堆分配。链接 BSS 比任务开始时增加 480 字节，其中 PSRAM 320 字节、内部静态 RAM 160 字节；后者包含静态播放互斥量和少量统计/检测状态。SDK 的 PDM DMA 开机缓冲因立体声从 640 增至 1280 字节，额外 640 字节一次性 SRAM 分配。原有 16 kHz、160 点/10 ms 和 Opus 每 6 帧编码一次的节奏不变。

| 状态 | 麦克风采集 | 本地 KWS | 本地 barge-in | 云端上行 |
|---|---|---|---|---|
| Standby / Idle | 双麦持续采集并经过 3A | 开启，命中后走原有唤醒、提示音、listen_start 流程 | 关闭 | 强制关闭 |
| Listening | 双麦持续采集并经过 3A | 开启 | 关闭 | 保留原 VAD 和 800 ms 拖尾门控 |
| Thinking | 双麦持续采集并经过 3A | 开启，显式唤醒词保留原会话唤醒行为 | 关闭，能量不会触发 | 关闭 |
| Speaking | 双麦持续采集并经过 3A | 开启，命中发送同一中断请求 | 原始双路 peak < 32000，连续 250 ms 收敛（pre >= 800 且 post/pre <= 1/8）后武装，冻结窗口残差均值 | 保留原会话/VAD 门控 |

提示音 `xz_wake_beep_busy` 期间，KWS 喂入、barge-in 和上行均关闭，麦克风采集和诊断日志继续。主动打断与 `g_xz_uplink_on` 无关，只在已连接且活跃会话的 Speaking 中运行；不会绕过 Standby/Thinking 的上行门控。

语音判定使用同一 10 ms 帧的 pre/post 数据：每个 generation 等待连续 25 帧原始双路峰值均小于 32000、pre >= 800 且满足收敛比例；将该窗口 post RMS 均值冻结为 echo_floor 并武装。静默/弱回声不能武装，削顶帧清零武装和人声连续计数。没有按时钟自动放行的路径。武装后要求 post_rms >= max(600, 4 * echo_floor)、post_rms * 8 >= pre_rms，并且 WebRTC VAD 连续 6 帧判为人声。pre_rms 取下混前原始 L/R RMS 的较大者，不依赖 MIC_LEVEL_LOG。缺少参考或到 stop 都未完成有效窗口时保持不武装。失败帧、提示音或超 30 ms 采集间隙会清零对应连续计数；armed 后保留基线和单次触发锁存。每次 TTS start 使用新 generation，延迟的旧请求无法打断新播报。tts stop 收到时立即清理检测器与待处理打断请求，PCM 排空期间也不再自动检测；abort 同步清理检测器。音频和 KWS 线程只发布固定大小的请求，主事件线程约每 20 ms 检查并执行：

```text
ws_send_speak_abort(..., kAbortReasonWakeWordDetected)
  -> xz_speaker(0)
  -> g_is_speaking = false
  -> ws_set_state(kDeviceStateListening)
```

不会重连、重发 listen_start 或播放提示音。KWS 的 Speaking 分支直接发布相同请求，保留物理按键和 `xz_btn_sim` 的 `kAbortReasonNone` 语义。原先被未定义 `STOP_SPEAKER_WHEN_DETECTED_MIC_VOICE` 屏蔽的停播现已生效：清理待解码 TTS 和本客户端 TX PCM 缓存；generation 阻止已经取出的旧块在停止后重新写回。正常 `tts stop` 等当前 PCM 排空再关闭 TX，避免截尾；打断后的迟到 stop 不会将 Thinking 改回 Listening。共享音乐混音器、RX 和 AEC 不重建，音乐按原 `xz_music_tts(false)` 规则恢复；已经进入混音器/DMA 的少量声音仍可能有硬件尾音，实际延迟需要上板测量。

没有改 `xiaozhi_ui.c`、圆脸绘制、IoT 描述符或所约束 shell 命令的函数体；UI 仍使用现有 `xiaozhi_ui_*` 的 `ui_post(EV_...)` 封装。

在 `app/project/proj.conf` 添加以下配置后用同一构建命令重编：

```ini
# 关闭每通道 RMS 日志；MIC_PEAK 和 MIC_PCM 仍每 2 s 输出一次
CONFIG_XZ_MIC_LEVEL_LOG=n

# 比较逐样点取幅值较大者；不设置或 n 为平均下混
CONFIG_XZ_MIC_DOWNMIX_MAX_ABS=y

# 可选：按实际远场/AEC 后 mono_rms 调整。默认值如下
CONFIG_XZ_BARGE_IN_RMS=600
# legacy，仅保留兼容，不再用于武装
CONFIG_XZ_BARGE_IN_TRAIN_MS=800
CONFIG_XZ_BARGE_ARM_RATIO=125
CONFIG_XZ_BARGE_ARM_MS=250
CONFIG_XZ_BARGE_ARM_MIN_PRE=800
CONFIG_XZ_BARGE_IN_ECHO_MULT=4
CONFIG_XZ_BARGE_IN_PRE_RATIO=8
CONFIG_XZ_BARGE_IN_FRAMES=6
```

新增日志和 grep 关键字：

| grep | 内容和用途 |
|---|---|
| `MIC_L rms=` / `MIC_R rms=` | 每 2 s 同一行：`MIC_L rms=... peak=... \| MIC_R rms=... peak=... source=pre3a_LR`；分别拍左、右麦确认对应通道变化。两路均来自下混前原始信号。 |
| `MIC_PEAK` | 每 2 s 原始双路窗口峰值 L/R、clip；缺 raw 时 pre_valid=0。与可选 RMS 日志开关无关。 |
| `BARGE_CLIP` | 当前 Speaking 削顶帧的 pre/peakL/peakR；首次立即，此后至少间隔 2 s。保护每帧执行。 |
| `MIC_PCM` | 每 2 s：`raw_lr_blocks`、`post_blocks`、`mono_samples`、`mono_rms`、`mono_peak`、`post_bytes`、`post_ch`、`frame_samples=160 fs=16000 frame_ms=10`、`mix=average/max_abs`、`bad_len`。稳定 2 s 窗口约为 200/200/32000，当前 SDK 的 `post_bytes=320 post_ch=1` 是正确的 AEC 后单声道。 |
| `BARGE_IN` | `BARGE_IN rms=... floor=... pre=... ratio=.../1000 frames=6 generation=...`，仅自动语音打断成功消费时打印；ratio 是 post/pre 千分比。 |
| `BARGE_TRAIN` | 每代 phase=start；仅连续不削顶收敛后 phase=armed reason=converged；stop 时仍未武装则 phase=skip reason=no_convergence。 |
| `BARGE_CURVE` | tts start 后 3 s 内，每 100 ms 一行 pre/post/ratio/state=train/armed/pre_valid/clip；t 从 tts start 计时。 |
| `BARGE_MON` | 与 MIC_PCM 同步每 2 s 输出训练状态、floor、threshold、当前 rms/pre/ratio 和 pre_valid，不打断也能观察判据。 |
| `SPEAK_INTERRUPT` | `source=speech/kws/button` 和 generation，区分触发来源。 |
| `SPEAK_ABORT` | `reason=wake_word_detected(1)` 用于语音/KWS；`reason=none(0)` 保留按键、mute 和其他原路径。 |
| `KWS_WAKE` | KWS 命中时的设备状态，可串联 `source=kws` 和 `SPEAK_ABORT`。 |

如果迁移 SDK 仅提供 AEC 后单声道，且没有原始 L/R 事件，日志会诚实显示 `MIC_L rms=NA ... MIC_R rms=NA ... source=post3a_mono`，不会伪造两路电平。缺少原始事件时保留 KWS/Opus 的单声道兼容路径，但自动语音打断禁止触发，不回退到固定 RMS。当前已打补丁 SDK 应显示 `pre3a_LR`。

验证命令均已通过：

```powershell
python verification/dual-mic-barge/test_audio.py
python verification/standby-wake/test_state_machine.py
powershell -NoProfile -ExecutionPolicy Bypass -File verification/music-rx/test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

主机测试包含实际生产回调和 SDK 适配分支，分别编译电平日志开/关、平均/取大幅值组合；覆盖 2231/3672 起播残差、连续不削顶收敛均值、L/R 正负削顶边界、静默/弱回声拒绝、削顶后重新累计、峰值日志限频、失败窗口重置、冻结基线、门限边界、缺失 pre/全部回调、tick 回绕、曲线限频、VAD/提示音/四态门控，以及 speech/KWS abort、旧 generation、迟到 stop、正常排空和音乐 RX 参数继承。AEC 算法在主机测试中被确定性替身替代，因此不能以此证明真实扬声器回声消除效果。此次使用 `clip-build.log`、`clip-*-tests.log` 和 [clip-artifacts.json](clip-artifacts.json)；此前产物记录保留为历史数据。

必须由你上板确认：分别拍左/右麦时两路 `MIC_L/MIC_R` 电平是否各自响应；不同音量 TTS 在无人说话时是否完全不自打断；收敛期结束后普通讲话和唤醒词是否各自打断；2–2.5 m 灵敏度及 600/6 默认门限是否合适；实际停播尾音/延迟、会话连续性、音乐恢复，以及待命上行始终为零。上述结果本机无法验证，当前不宣称硬件验收已通过。
