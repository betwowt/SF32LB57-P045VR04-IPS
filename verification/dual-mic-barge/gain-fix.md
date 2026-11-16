# fix5：PDM 增益修剪与独立 PCM 补偿

仅本机编译和主机回归；没有烧录、串口操作或 COM8 访问。本轮没有修改 SDK 源码或增加 SDK 导出，继续使用已有 0005 补丁及现有 `Set_pdm_gain(int8_t)`。编译可能在 SDK 目录更新其原有构建产物。

## 增益调用点和幂等性

`xiaozhi_mic_dsp.h` 定义 `XZ_PDM_GAIN_TRIM=44`。`xiaozhi_audio.c` 的 `xz_audio_apply_pdm_gain()` 调用现有 SDK 导出 `Set_pdm_gain(44)`，同时设置保存的 `g_pdm_volume` 和实时双通道寄存器。44 与默认 56 相差 12 个 0.5 dB 单位，即修剪 6 dB；不是把 SDK 的绝对增益解释成 -6 dB。

以下每个 `audio_open` 之前和成功返回之后均调用此函数：

| 打开点 | 覆盖路径 |
|---|---|
| `xz_audio_decoder_encoder_open()` | 启动、会话首次初始化、音频服务重建 |
| `xz_aec_mic_open()` | TXRX 麦克风重新打开，包括 `reinit_audio()` |
| `xz_mic_open()` | 非 AEC 采集备用路径 |
| `record_data_thread_callback()` 首次打开和 KWS_EVT_START | 独立 KWS 模式首次启动和恢复；当前产品使用共享模式 |

SDK `audio_server.c` 在 PDM 配置完成、采集启动之前读取 `get_pdm_volume()` 并应用到硬件。因此前置调用可让首次采集使用 44，后置调用再次确认实时值；音乐阶段内部设备重开也会读取保存值 44。每次都是绝对赋值，不会在重启、恢复或重复调用时累计衰减。应用层 `PDM_GAIN set=44` 每次启动仅打印一次；SDK setter 自身原有日志仍会打印。

原 `mic_gain_decrease(10)` 调用保留，它仅作用于模拟 ADC。本轮没有修改音量默认值、AGC 设置、状态机、IoT 或音乐命令。

## PCM 补偿位置

`xz_mic_makeup_sample()` 使用 32 位乘法和 int16 饱和：正值最多 32767，负值最少 -32768。只在 `AUDIO_RX_USING_PDM` 启用时乘 2；非 PDM 路径保持原样。

上行：`xz_mic_mono_frame()` 先完成原始 post 统计、KWS 调用、共享 VAD 和 barge 检测，通过已有上行状态门控后，将每帧 160 点复制到静态 `xz_mic_uplink` 并补偿。所有后续上行队列和旧 VAD 预卷缓存都使用此副本，原始音频和 AEC 缓冲不变；没有逐帧堆分配。Standby/Think/提示音继续按原逻辑阻断上行。

KWS：当前共享模式的 `kws_feed_mic()` 收到 AEC 后的 16 kHz、单声道、16 bit PCM。`kws_push_pcm()` 在复制到 KWS 私有 40 ms 队列时逐点补偿，所以共享模式与独立采集模式均只补偿一次。KWS 补偿不会回写到 barge、VAD 或 post 度量。

`xz_micstat`、MIC_PCM 仍报告补偿前的 post 捕获电平；不能据此直接声称服务器收到的补偿后电平已经实测等同旧版，服务器 ASR 电平须上板确认。

## 新判据

| 参数/保护 | 当前值 |
|---|---:|
| PDM 设置 | 44（原默认 56，修剪 6 dB） |
| KWS、上行副本补偿 | ×2，int16 饱和，约 +6 dB |
| XZ_BARGE_ARM_MIN_PRE | 120 |
| XZ_BARGE_ARM_RATIO | 250/1000 |
| XZ_BARGE_ARM_MS | 250 ms，即连续 25 帧 |
| XZ_BARGE_IN_RMS | 150 |
| XZ_BARGE_IN_ECHO_MULT | 4 |
| XZ_BARGE_IN_PRE_RATIO | 8 |
| XZ_BARGE_IN_FRAMES | 6，即连续 60 ms |
| 任一原始声道削顶拒绝边界 | peak ≥ 32000 |

只接受真实回声下连续收敛，不使用 quiet 或超时武装。触发仍要求 `post >= max(150, 4*floor)`、`post*8 >= pre`、VAD=1，以及每代一次。floor=25–50 时实际绝对门槛为 150–200；floor=50 时 post=150 尚不足以触发，符合保留的自适应判据。

MIC_PEAK、BARGE_CLIP、BARGE_TRAIN、BARGE_CURVE 保留；提示音/Think 不检测、Standby 零上行、`wake_word_detected` abort 语义均保留。

## 本机验证

执行记录：`gain-build.log`、`gain-audio-tests.log`、`gain-state-tests.log`、`gain-music-tests.log`；最终固件、配置和校验值见 `gain-artifacts.json`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
python verification/dual-mic-barge/test_audio.py
python verification/standby-wake/test_state_machine.py
powershell -NoProfile -ExecutionPolicy Bypass -File verification/music-rx/test.ps1
```

主机回归覆盖实际 setter 包装和 KWS 队列函数、重复打开增益幂等、一次性日志、全部 65536 个 int16 输入的补偿饱和、非 PDM 不补偿、上行饱和、post/VAD/输入不受补偿影响、新武装/触发边界、三次模拟重启的实测 193→29 回声轨迹、六帧人声打断、原削顶/会话/Standby/提示音/Think/音乐 RX/3A 约束。模拟轨迹不能替代实机测量。

## 用户上板复测

1. 三次 `reboot` → 唤醒 → `xz_listen_text 请慢慢讲一个比较长的故事`：每次出现一次 `PDM_GAIN set=44`、每轮 `phase=armed`，无人声时零 `SPEAK_INTERRUPT`，MIC_PEAK clip=0 且 peak<28000。
2. 已武装的故事播放中，真人在 2–3 m 说话：单次 `BARGE_IN source=speech` → `UI_STATE listen`；会话保留。
3. 在 2–3 m 说“小智小智”仍可唤醒，确认实际 KWS 灵敏度。
4. 用服务器侧日志确认 ASR PCM 电平及识别效果与修剪前相当；本机代码仅能证明上行副本乘 2。

硬件峰值、真实 AEC 收敛、远场 KWS/打断和服务器电平均待确认。
