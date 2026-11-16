# fix6：参考电平 DTD

本轮仅修改工程、执行本机主机回归和固件编译；没有烧录，没有访问 COM8，没有修改 SDK 源码。SDK 原有的 0005 双麦补丁继续使用。构建可能更新 SDK 目录中的已有目标文件/库。

## 参考采样和配对

工程链接增加 `--wrap=audio_3a_far_put`，实现位于 `app/src/xiaozhi/xiaozhi_barge_ref.c`。SDK 音频服务在实际 TX DMA 提交前、`speaker_update_volume()` 之后调用该函数，所以测量的是送给 3A 的实际参考，不是提前排队的 TTS 解码 PCM。包装函数读同一块 PCM，保留对原函数的调用；不改 PCM、不拷贝音频、不做堆分配。

当前 TXRX/3A 配置固定为单声道 16 kHz，每块 320 字节、160 点、10 ms。每块只计算一次整数 RMS 和绝对峰值，用 64 位平方和处理包括 -32768 在内的输入；非 320 字节块使配对失效，不把未知格式当静音。

仅保留最近两帧的电平、时间及 TX/MIC 序号。raw 麦克风回调取参考快照，post 回调消费同一快照；前后麦克风 PCM 继续沿用 SDK 的同步 raw→3A→post 路径。取当前参考与时间在 10 ms 内的上一帧参考的较大 RMS，容忍一帧回调顺序或声学延迟，也避免单个参考零帧立即解除 DTD。峰值同样取该范围内的最大值。日志 `ref` 是此配对后的参考电平。

最新 TX 距 raw 大于 30 ms、相对初始配对的 TX/MIC 序号差大于 3 帧、参考缺失或格式无效，均使此帧无效并打断连续触发/武装窗口；输出 `BARGE_DTD_SYNC`，最多每两秒一次。错误配对后的下一次新鲜有效配对重新建立序号基点，本帧不计数。已武装的 floor/gain 和每代一次锁存保留。新一代和生命周期 reset 清空参考历史，避免旧 TTS 参与新标定。

## 标定窗口及触发公式

仍然只接受连续 250 ms（25 帧）的收敛：原始双麦较大 RMS `pre >= 120`、`post*1000 <= pre*250`、双声道均不削顶、raw 和参考配对有效，且原有会话/提示音门控允许。没有 quiet 或定时武装。

在该窗口内，仅对配对参考 `ref >= 50` 的帧累计 `sum_pre`、`sum_ref`。武装时：

```
g = clamp(sum_pre * 1000 / sum_ref, 50, 4000)
floor = 所有 25 帧 post 的平均值
```

没有合格参考累计时不武装，不用任意默认增益。任一坏帧清空两种累计；武装后冻结 floor 与 g。

播放时 `ref >= 50` 的 DTD 条件为：

```
pre * 1000 >= g * ref * DTD_MARGIN_PM / 1000
```

默认 `XZ_BARGE_DTD_MARGIN_PM=2000`，即原始近端电平至少为标定回声估计 `g*ref/1000` 的两倍，约 +6 dB。代码用等价的 `pre*1000000 >= g*ref*margin` 和 64 位乘积，避免溢出和中间除法截断。

已武装后，新鲜有效参考 `ref < 50` 跳过 DTD 电平比较，仍要求 `post >= max(150, 4*floor)`、`VAD==1`、raw 有效、双声道不削顶、连续六帧、每代一次。静音参考来自 SDK 的实际零 TX 块；缺失/过期参考不视为静音。无播放分支不引入新的武装方式，保留“仅收敛武装”约束。

`XZ_BARGE_IN_PRE_RATIO` 仅为旧配置兼容保留，不参与判断；曲线中的 post/pre 比例仅作诊断。原始削顶峰值边界仍为 32000，PDM 设置仍为 44，KWS/上行副本补偿及 VAD 输入保持原行为。

## 日志样例

下列为格式和数值一致的说明样例，不是实机采集。`dtd=1` 表示该帧通过 DTD 条件（包括低参考时跳过电平比较）。

```
BARGE_TRAIN generation=7 phase=armed reason=converged t=250ms window_ms=250 frames=25 floor=13 threshold=150 rms=13 pre=500 ratio=26/1000 pre_valid=1 g=500 ref=1000
BARGE_CURVE gen=7 t=1000ms pre=2000 post=1900 ratio=950/1000 state=armed pre_valid=1 clip=0 ref=4000 ref_peak=8000 g=500 dtd=0 sync=1
BARGE_IN gen=7 source=speech pre=4200 post=700 ref=4000 g=500 dtd=1 rms=700 floor=13 frames=6
SPEAK_INTERRUPT source=speech generation=7
BARGE_DTD_SYNC gen=8 age_ms=40 skew=-3 pre_valid=1
```

触发时将 pre/post/ref/g/dtd/floor/frames 一起写入原请求结构；主线程消费快照打印日志，避免异步日志误用后续帧。KWS 的原入口、状态门控与 `wake_word_detected` abort 原因不变。原 3 秒/100 ms 曲线、MIC_PEAK、BARGE_CLIP、SPEAK_INTERRUPT 保留。

## 验证及产物

```
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
python verification/dual-mic-barge/test_audio.py
python verification/standby-wake/test_state_machine.py
powershell -NoProfile -ExecutionPolicy Bypass -File verification/music-rx/test.ps1
```

完整构建与最终增量构建均成功（退出码 0），三组主机测试通过。链接映射已确认 `audio_server.o` 调用工程 `__wrap_audio_3a_far_put`，包装再调用 SDK 原 `audio_3a_far_put`。SDK 依赖和工具链原有的 Opus/SDIO、链接/ftab 告警仍在，详见构建日志。

日志：`dtd-build.log`、`dtd-final-build.log`、`dtd-audio-tests.log`、`dtd-state-tests.log`、`dtd-music-tests.log`。最终产物和校验值见 `dtd-artifacts.json`。

主机测试使用实际参考包装、实际 raw/post 回调、实际 DSP 和 WebSocket 发布/消费函数。覆盖高 post/pre 残留回声拒绝、三次模拟重启、标定增益、六帧 DTD 边界、静音参考/VAD/绝对阈值、参考 49/50 边界、一帧参考电平包络、过期/序号失配/异常格式、削顶两声道/两极性、夹限/64 位运算、tick 回绕、每代一次、3 秒曲线，以及 Standby/Think/提示音/KWS/音乐 RX/3A 约束。模拟不能证明实机声学效果。

## 待用户上板确认

1. 三次 reboot→唤醒→`xz_listen_text 请慢慢讲一个比较长的故事`：每次 `phase=armed` 且 g 稳定合理、没有 `SPEAK_INTERRUPT`、`MIC_PEAK clip=0`。
2. TTS 期间 2–3 m 真人说话：单次 `SPEAK_INTERRUPT source=speech`；对应 `dtd=1`，pre 至少达到 `2*g*ref/1000`，并观察实际识别距离/延迟。
3. 已完成收敛武装后的无播放/参考静音时说话：仍按绝对阈值、floor 倍数和 VAD 连续六帧触发。
4. 观察 ref、g、ref_peak 和 `BARGE_DTD_SYNC`，确认硬件上的参考采样率/10 ms 帧格式与麦克风配对正常；若 g 持续撞到夹限，需结合原始曲线检查参考/麦克风比例。
