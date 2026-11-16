# fix4：原始削顶保护、仅收敛武装、默认播放余量

本轮仅本机编译和主机回归，没有烧录、串口操作或 COM8 访问。SDK 源码及 AEC 断言没有修改，继续使用已有 0005 补丁，无需新增 0006。主机测试不能证明板上峰值、AEC 收敛或远场打断已通过。

## 完整判据

每帧仍为 16 kHz / 160 点 / 10 ms。原始 L/R 在下混和 3A 之前，分别计算 RMS 和绝对样点峰值；`-32768` 的绝对值按 32 位运算得到 32768。`pre` 取两路 RMS 较大者。峰值和配对参考的计算不受 `CONFIG_XZ_MIC_LEVEL_LOG` 开关影响。

1. 只在当前有效会话 Speaking、非提示音、非断开、未收到 tts stop 时检测；缺 pre、坏长度或采集间隙超过 30 ms 均打断连续窗口。
2. 任一原始声道 `peak >= 32000`，该帧不参与武装、不计入人声连续帧、不能触发；清零 arm_frames、arm_sum 和 speech frames。已武装的 floor 和已触发的同代锁存保留，下一合格帧重新累计人声。
3. 删除 quiet 路径。仅当每帧 `pre >= XZ_BARGE_ARM_MIN_PRE` 且 `post * 1000 <= pre * XZ_BARGE_ARM_RATIO` 时累计武装。默认连续 `ceil(250/10)=25` 帧，冻结这些帧 post RMS 的整数均值为 floor。低回声或静默均重置窗口，等待多久也不能绕过；stop 仍未 armed，输出 `skip reason=no_convergence`。
4. 武装帧本身不计人声。之后每帧必须不削顶、pre 配对有效且非零、`post >= max(600, 4 * floor)`、`post * 8 >= pre`、VAD 恰为 1，连续 6 帧才发布请求。同一 generation 最多一次；新一代重新训练。

`proj.conf` 明确恢复 `CONFIG_XZ_BARGE_IN_RMS=600`，防止沿用现场观测固件的 32000。ARM_MIN_PRE=800、ARM_RATIO=125、ARM_MS=250、ECHO_MULT=4、PRE_RATIO=8、FRAMES=6 均未提高。Standby 零上行、Thinking/提示音门控、speech/KWS 的 `wake_word_detected` abort、PCM 排空、会话和音乐命令沿用原逻辑，无逐帧堆分配。

## 采集链评估和余量方案

先核实实际信号路径：板子使用 PDM1 双通道。SDK `drv_audprc.c` 的 `mic_gain_decrease()` 调的是 AUDCODEC ADC，原来的 `12-10=+2 dB` 并不是 PDM1 的增益。PDM 另有 `g_pdm_volume=56`（0.5 dB 单位，即默认 +28 dB，运行时可能有配置覆盖）。因此本轮不调整 XZ_MIC_GAIN_TRIM_DB，也不假定改它就能消除 PDM 削顶。

SDK `audio_3a.c` 的 uplink AGC 位于 AEC 之后。开启它无法恢复进入 AEC 之前已经丢失的峰值，还会改变 post/floor 和 VAD 的电平关系。本轮保留两处 `disable_uplink_agc=1`，避免把后级增益当成前级防削顶。

采用一个幅度改动：默认 `AUDIO_TYPE_LOCAL_MUSIC` 私有音量从原 WebSocket 的 8 降至 6。统一 `VOL_DEFAULE_LEVEL=6`，使 main 初始化、WebSocket 初始化和 MQTT 初始化都使用同一默认值，避免启动路径重新设置为 12。SDK stock `g_music_vol_level` 中 8=-20 dB、6=-24 dB，差值为 -4 dB，线性幅度系数 `10^(-4/20)=0.63096`。保留原音量命令和可调范围；用户随后主动加大音量仍可能削顶，保护始终有效。

以下是任务中已有实测值和线性估算，不是本轮上板实测。原日志为约 2 s 窗口峰值，不能据此把同一行 post RMS 当成峰值所在 10 ms 帧的 AEC 效果。

| 观测项 | 旧版已提供的值 | -4 dB 线性估算 |
|---|---:|---:|
| TTS 两路最高已编码峰值 | 32767，已饱和 | 20675，**仅以满量程作为输入假设** |
| TTS 未饱和观测峰值 | 31984 | 20180 |
| TTS 另一路未饱和峰值 | 27786 | 17532 |
| 较大回声 RMS | 4516 | 2849，仍高于 800 的真实回声门槛 |
| 低回声 RMS | 403 | 254，不能武装，按要求 fail-closed |
| 2–3 m 人声绝对峰值 | 工作区无实测记录，无法从 TTS 推导 | 麦克风链不变；相同人声输入的增益变化为 0 dB，即新峰值约等于旧峰值 |

以旧真实峰值恰为 32767 的假设计算，新峰值约 20675，到 28000 有 `20*log10(28000/20675)=2.63 dB` 余量，到满量程有约 4 dB 余量。等价地，旧真实峰值若小于约 44377（满量程以上 2.63 dB），-4 dB 后才会小于 28000。由于旧日志已经削顶，真实峰值上界未知，**不能保证这个条件成立**；实际 DAC 音量表、EQ、房间耦合及麦克风本体饱和也需要实测核对。

2–3 m 人声的绝对 peak/SNR 没有可用录音、原始样本或标定数据，不能诚实给出实测数值。仅作量级示例：若实际人声 RMS 为 300–800、假设 crest factor 为 3–6，则 peak 为 900–4800，到 28000 的余量约 29.9–15.3 dB；这两个输入是假设，不能用于验收，也不能推出 AEC 后必然超过 600。选择降低播放音量的依据是人声链路增益保持 0 dB 变化，同时在局部线性条件下人声/回声比改善约 4 dB。

## 日志

```text
MIC_PEAK L=... R=... clip=0/1 pre_valid=0/1
BARGE_CLIP pre=... peakL=... peakR=... generation=...
BARGE_TRAIN generation=... phase=armed reason=converged t=...ms window_ms=250 frames=25 floor=... threshold=... rms=... pre=... ratio=.../1000 pre_valid=1
BARGE_TRAIN generation=... phase=skip reason=no_convergence
BARGE_CURVE gen=... t=...ms pre=... post=... ratio=.../1000 state=train/armed pre_valid=... clip=0/1
```

- MIC_PEAK：每 2 s 一行，L/R 为该窗口真实 pre-3A 各自最大绝对峰值，clip 表示窗口内是否出现过危险峰值。独立于可关闭的 MIC_L/MIC_R RMS 日志。缺少任何 raw 帧时输出 `L=0 R=0 clip=0 pre_valid=0`，这些零不代表已测得静音。
- BARGE_CLIP：仅 eligible Speaking 的削顶帧，首次立即打印，此后相邻行至少 2 s；日志限频不限制每帧保护。pre 和两个 peak 是同一 10 ms 帧。
- armed：每代成功武装一次，只可能 reason=converged；floor 为有效连续窗口均值，threshold 为实际门限。
- BARGE_CURVE：沿用从 tts start 起前 3 s、每 100 ms 至多一行，增加 clip 字段。坏帧仍显示真实 pre/post，clip=1 表示不能使用它武装或触发。
- 既有 start、BARGE_MON、MIC_PCM、BARGE_IN 和 SPEAK_INTERRUPT/ABORT 保留。

## 本机验证及上板待确认

执行记录：`clip-build.log`、`clip-audio-tests.log`、`clip-state-tests.log`、`clip-music-tests.log`；固件位置、最终配置及 SHA256 见 `clip-artifacts.json`。

```powershell
python verification/dual-mic-barge/test_audio.py
python verification/standby-wake/test_state_machine.py
powershell -NoProfile -ExecutionPolicy Bypass -File verification/music-rx/test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

生产回调/DSP 主机回归覆盖：L/R 各自 +32000、-32000、32767、-32768 的单采样削顶，31999 的边界，24 个收敛帧后削顶重计，5 个人声帧后削顶禁止第六帧触发，已 armed/已 fired 锁存保留，缺 pre 不复用，长时间静默/低回声不武装，2000 ms 峰值和削顶日志限频，平均/取大值下混、可选日志关闭，以及原状态机、stop skip、abort、会话和音乐 RX/3A 回归。

上板按任务原复测方法执行三次 reboot 和故事播放：默认 level=6 时每轮都要 peak<28000、clip=0、出现 converged armed；无人声不得 BARGE_IN/SPEAK_INTERRUPT/WS_STANDBY，结束 session=1。另在 TTS 关闭时记录 2–3 m 人声 MIC_L/MIC_R RMS/peak，之后在已 armed 的 TTS 中验证一次 source=speech → UI_STATE listen、session=1。若任一轮 skip，它在安全逻辑上是正确的 fail-closed，但**不满足本任务的功能验收**，不能把 skip 当作修复成功。

音乐后期 AEC 退化未修。当前 arming 只接受本代真实连续收敛，既不借用音乐阶段状态，也不以超时放行；已有 armed 后若 AEC 再退化，单靠当前规定的 RMS/ratio/VAD 判据不能保证永不误判，需用用户的完整长故事复测确认。
