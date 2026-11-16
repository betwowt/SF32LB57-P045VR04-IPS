# 三次修复：收敛或弱回声均可武装（2026-11-16）

只做本机编译和主机回归，没有烧录、打开串口或访问 COM8。前两轮说明保留为历史，当前实现以本文为准。

## 武装状态机

```text
tts start -> 新 generation，清空检测器 -> train
  A: 每帧有效配对 pre >= 800 且 post * 1000 <= pre * 125
     连续 25 帧 / 250 ms -> converged -> armed
     floor = 该窗口 post 的整数均值（保留原算法）
  B: 每帧有效配对 pre < 800 且 post <= 800
     连续 25 帧 / 250 ms -> quiet -> armed
     floor = max(1, 该窗口 post 的整数均值)
  armed: floor 冻结，threshold = max(600, 4 * floor)
     保留 post * 8 >= pre、配对 pre > 0、VAD == 1、连续 6 帧
     -> 每 generation 至多一次 speech 打断
     -> wake_word_detected abort -> UI_STATE listen，会话保持
  tts stop 时仍在 train -> skip reason=no_convergence
```

`converged/quiet` 是窗口原因，完成窗口才进入 `armed`。两条路径不拼接：原因切换时清零计数与累加值，再计入当前帧；默认必须有同一种原因的连续 25 帧。配置默认值均不变，非整十 ARM_MS 仍向上取整为 10 ms 帧数。

强回声且比例不合格的帧不会武装，持续不收敛直到 stop 仍 skip；没有按时钟、超时或固定延迟放行。缺少配对 pre、非法音频块、超过 30 ms 的采集断点、提示音和断开中的数据保护继续保留。有效的低电平或全零配对帧可以完成 quiet 窗口；缺失 pre 不能以零冒充安静。武装帧本身不算语音帧，后续波动不再修改基线。

Standby 零上行、提示音/Thinking 不检测、同代单次触发、stop/abort 清理、旧代请求拒绝及 KWS 的 wake_word_detected 语义均沿用原代码。WebSocket 状态机、IoT/音乐命令、UI、麦克风增益和 SDK 源码均未在本轮修改。

## 日志清单

唯一改变的日志格式为武装事件，在原字段前新增原因：

```text
BARGE_TRAIN generation=N phase=armed reason=converged t=...ms window_ms=250 frames=25 floor=... threshold=... rms=... pre=... ratio=.../1000 pre_valid=1
BARGE_TRAIN generation=N phase=armed reason=quiet t=...ms window_ms=250 frames=25 floor=... threshold=... rms=... pre=... ratio=.../1000 pre_valid=1
```

其他日志保留：

| 日志 | 行为 |
|---|---|
| `BARGE_TRAIN phase=start` | 每代一次，打印现有配置 |
| `BARGE_TRAIN phase=skip reason=no_convergence` | 正常 stop 时仍未武装，重复 stop 不重复打印；armed 后不打印 skip |
| `BARGE_CURVE` | tts start 后 `0 <= t < 3000 ms`，同代相邻行至少 100 ms，保留 pre/post/ratio/state/pre_valid |
| `BARGE_MON` | 仍每 2 s，floor/threshold 现在也可来自 quiet 窗口 |
| `BARGE_IN` / `SPEAK_INTERRUPT` / `SPEAK_ABORT` | 格式与触发语义保留 |

## 验证与产物

```powershell
python verification/dual-mic-barge/test_audio.py
python verification/standby-wake/test_state_machine.py
powershell -NoProfile -ExecutionPolicy Bypass -File verification/music-rx/test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

测试日志为 `quiet-audio-tests.log`、`quiet-state-tests.log`、`quiet-music-tests.log`，构建日志为 `quiet-build.log`。最终产物路径、SHA256、配置和保护文件校验见 [quiet-artifacts.json](quiet-artifacts.json)。

音频测试覆盖原收敛路径、实测低音量数值构造的 quiet 窗口、均值与冻结、post=800/801、pre=799/800、全零 floor=1、缺失参考、模式切换、单代一次及原有保护。协议和音乐回归覆盖 Standby 零上行、speech/KWS abort、session 保持、stop/排空、旧代拒绝、音乐 RX/3A 保留。主机 AEC/VAD 使用确定性替身，这些结果不能证明房间中的真实声学表现。

新增一个静态原因字段，检测器由 20 增至 24 字节。没有逐帧堆分配、浮点运算或新音频缓冲；日志继续在临界区外输出。

## 仍需上板确认

1. 按用户流程执行三次 reboot，均等待 `connected=1 state=3` 再唤醒并执行 `xz_listen_text 请慢慢讲一个小故事`。每次都应出现 `phase=armed reason=converged|quiet`；无人讲话时不得出现 BARGE_IN / SPEAK_INTERRUPT / WS_STANDBY，结束时 `session=1`。
2. 检查低音量、正常音量和 TTS 延迟到音时的安静播放；quiet 武装后的启动残差是否仍不会自打断，需要实际声学复测。
3. 真人在 2–3 m 说话，确认仅一次 `BARGE_IN` 和 `SPEAK_INTERRUPT source=speech`，随后 `UI_STATE listen`、会话存活；确认实际灵敏度、延迟和尾音。
4. 继续确认 Standby 零上行、提示音/Thinking 无能量打断、KWS、IoT 和音乐行为保持。
