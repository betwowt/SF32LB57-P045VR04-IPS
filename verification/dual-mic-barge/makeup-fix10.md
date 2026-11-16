# fix10：KWS / VAD / 录音 / 上行统一补偿

已完成本机编译与主机回归；未烧录、未访问 COM8、未修改 SDK 源码。

`xz_mic_mono_frame()` 在 KWS/VAD 调用前，将原始 post-3A PCM 饱和乘 2 写入现有静态 `xz_mic_uplink[160]`。KWS、WebRTC VAD 和所有后续上行路径（包括旧 VAD 预卷缓存）共用该补偿缓冲，删除原上行分支中的重复计算。`xz_rec` 保存补偿后的块峰值。没有新增逐帧堆分配。

原始 `sm` 不被改写；post 能量、barge 输入 RMS、MIC_L/MIC_R、MIC_PCM 和原始 peak >= 32000 削顶保护维持原口径。PDM 增益仍为 44；DTD、收敛武装、每代一次、Standby 零上行、提示音/Think 门控及 `wake_word_detected` 未改。首个有效 PDM post 帧仅打印一次：

```text
MIC_MKUP uw=1 kws=1 vad=1
```

实际代码与任务描述有一处差异：fix5 已在 `kws_push_pcm()` 的私有队列拷贝中补偿 KWS。因此此次同时调整该处：共享采集直接复制已补偿 PCM，独立采集继续在队列中补偿，避免共享路径累计 +12 dB。相对修改前，VAD 新增补偿，KWS 识别器和上行的最终增益保持一次 ×2。

验证均通过：

- `powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1`：退出码 0，`scons: done building targets.`。日志见工程根目录 `fix10-build.log`。构建有 newlib 系统调用桩、RWX 段和 ftab 入口等链接警告，无编译失败。
- `python verification/dual-mic-barge/test_audio.py`：日志见根目录 `fix10-audio-tests.log`。覆盖 PDM/非 PDM、共享/独立 KWS 仅补偿一次、全 int16 饱和边界、KWS/VAD/上行逐点一致、录音峰值、单次日志、补偿削顶不影响原始削顶判定、原始 barge RMS、现有 DTD/收敛/状态门控回归。
- `python verification/standby-wake/test_state_machine.py`：Standby 零上行、提示音门控、speech/KWS abort、旧 generation 和迟到 stop 等现有回归通过。

待上板确认（由用户执行）：

1. 静默环境 2–3 m 连续说 5 次“小智小智”，全部唤醒；检查 `wake=`、`MIC_PEAK` 及单次 `MIC_MKUP`。
2. 唤醒后同距离说“把灯调成红色”，确认出现 `stt` 且灯变红。
3. 连续 3 轮长 TTS 无人说话，确认 0 误打断。
4. `MIC_L/MIC_R` 和 post 统计按约束保留原始电平，不应期待这些日志增加 6 dB。未饱和样本的补偿缓冲幅值应为原始 PCM 的 2 倍（约 +6.02 dB）；现有 KWS 私有队列在 fix10 前已补偿，最终识别输入也不应再增加 6 dB。

主机 VAD/AEC 使用替身，以上通过结果不代替真实远场灵敏度和回声误打断验收。
