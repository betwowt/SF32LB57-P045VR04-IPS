# 二次修复：观察 AEC 收敛后才武装（2026-11-16）

本次只编译和执行主机回归，没有烧录、串口操作或 COM8 访问。此文取代 [aec-fix.md](aec-fix.md) 中的固定 800 ms 峰值训练方案。

## 武装状态机

```text
tts start：原有 Speaking 边界分配新 generation，清空旧检测器和参考
  -> train：等待真实音频，无固定时间自动武装、无超时放行
      同一 10 ms 帧必须有有效配对 pre 数据，且：
        pre_rms >= ARM_MIN_PRE（默认 800）
        post_rms * 1000 <= pre_rms * ARM_RATIO（默认 125）
      连续 ceil(ARM_MS / 10) 帧合格（默认 25 帧 / 250 ms）
  -> armed：冻结上述窗口 post_rms 的算术均值为 echo_floor
      后续帧同时满足：
        post_rms >= max(600, 4 * echo_floor)
        post_rms * 8 >= pre_rms
        有效配对 pre、pre > 0、VAD == 1
      连续 6 帧后沿用 speech -> wake_word_detected abort -> Listening
      每 generation 最多一次，保留 session
  -> tts stop：立即关闭检测、清空 pending 请求和检测器，原流程排空 PCM
      若此前没有 armed，打印 phase=skip reason=no_convergence
```

`skip` 是 stop 时的结果日志，不是一个允许继续检测的新状态。安静环境、低音量、缺少 pre 回调、AEC 不收敛，乃至整轮完全没有麦克风回调，都会保持未武装；stop 后的 PCM 排空期间也不会启用自动检测。重复 stop 在排空期间不会重复打印 skip。armed 后的正常 stop 不打印 skip。显式 KWS 唤醒词仍可打断训练期及 stop 后的排空，abort reason 继续为 `wake_word_detected`。

任一收敛条件失败、缺失配对 pre、未知/空音频块、超过 30 ms 的回调间隙、提示音或正在断开，都会清掉连续收敛计数和累加值。armed 后这些情况只清连续计数，保留基线和单次触发锁存。训练不依赖 VAD；VAD 仅用于武装后的真人语音检测。完成武装的最后一个训练帧也不计入 6 帧语音窗口。

`pre_rms` 沿用既有数据源：**同一块音频、下混及 3A 之前原始 L/R RMS 的较大值**，并非新增扬声器参考通道；不依赖 MIC_LEVEL_LOG。它在本方案中作为实际播放存在的电平门槛。pre 只消费一次，缺失时曲线显示 `pre=0 ratio=0/1000 pre_valid=0`，并且绝不凭这组零值完成收敛。

## Kconfig

在 `app/project/Kconfig.proj` 定义，通过 proj.conf 使用 CONFIG_ 前缀；生成 C 宏没有 CONFIG_ 前缀。

| 配置 | 默认 | 范围 | 理由 |
|---|---:|---|---|
| `CONFIG_XZ_BARGE_ARM_RATIO` | 125 | 50–250 | 千分比上限，125 等价于 `post * 8 <= pre`，拒绝约 0.6 的起播残差 |
| `CONFIG_XZ_BARGE_ARM_MS` | 250 ms | 100–1000 | 连续 25 个 10 ms 帧，过滤起播阶段短暂的低残差；非 10 的倍数向上取整 |
| `CONFIG_XZ_BARGE_ARM_MIN_PRE` | 800 | 100–8000 | 排除 TTS 音频延迟到达前的低电平环境噪声；低音量宁可 skip |
| `CONFIG_XZ_BARGE_IN_TRAIN_MS` | 800 ms | 600–2000 | 保留兼容旧配置，已不参与武装、计时放行或基线计算，帮助文本明确标记 legacy |
| `CONFIG_XZ_BARGE_IN_RMS` | 600 | 1–32767 | 保留原绝对噪声下限 |
| `CONFIG_XZ_BARGE_IN_ECHO_MULT` | 4 | 3–4 | 保留倍数，基线改为收敛窗口均值 |
| `CONFIG_XZ_BARGE_IN_PRE_RATIO` | 8 | 2–32 | 保留武装后双讲条件 `post * 8 >= pre` |
| `CONFIG_XZ_BARGE_IN_FRAMES` | 6 | 3–20 | 保留 60 ms 连续人声确认 |

注意两个比例方向不同：ARM_RATIO 限制武装前残差的上限；PRE_RATIO 检查武装后近端语音是否足够突出。比较直接使用整数乘法，日志千分比取整不参与判决。启动瞬态不进入均值，armed 后均值不随语音或后续峰值更新。

## 日志清单

以下格式中的省略号是字段占位，不是板上数据：

```text
BARGE_TRAIN generation=N phase=start arm_ms=250 arm_ratio=125/1000 arm_min_pre=800 echo_mult=4 pre_ratio=8
BARGE_CURVE gen=N t=...ms pre=... post=... ratio=.../1000 state=train pre_valid=1
BARGE_TRAIN generation=N phase=armed t=...ms window_ms=250 frames=25 floor=... threshold=... rms=... pre=... ratio=.../1000 pre_valid=1
BARGE_CURVE gen=N t=...ms pre=... post=... ratio=.../1000 state=armed pre_valid=1
BARGE_TRAIN generation=N phase=skip reason=no_convergence
BARGE_MON generation=N training=0 floor=... threshold=... rms=... pre=... ratio=.../1000 pre_valid=1 frames=...
BARGE_IN rms=... floor=... pre=... ratio=.../1000 frames=6 generation=N
SPEAK_INTERRUPT source=speech generation=N
SPEAK_ABORT reason=wake_word_detected(1) state=...
```

- start：在实际接收并处理 tts start 时输出一次，即使没有音频回调也可见。
- curve：当前有效 Speaking 的音频回调输出，每 generation 相邻行至少 100 ms；仅 `0 <= t < 3000 ms`，正常约 30 行。t 从 tts start 原有 Speaking tick 计时，不从首包 PCM 或武装时刻重新计时。3 s 后仍可继续等待收敛、武装，但曲线停止输出。无回调时不补造样点，不补发错过的行。
- armed：收敛完成时输出一次，`t` 明确标记该代武装时刻；`window_ms` 是有效连续音频时长，`floor` 是窗口均值，`threshold` 是实际能量门限。
- skip：仅正常 stop 时仍未 armed 输出一次；不改变会话，不触发 abort。
- MON：保留原 2 s 诊断，`training=1` 时门限尚未启用。
- IN / INTERRUPT / ABORT：保留原成功打断日志和语义。`floor` 字段兼容旧日志格式，但现在代表 converged_residual 均值。

已有 `MIC_L/MIC_R`、`MIC_PCM`、UI、KWS、WS_STANDBY 和会话日志保留，没有新增 Standby 或连接状态迁移。

## 验证与资源

执行下列本机命令；输出保存在同目录 `convergence-*-tests.log` 和 `convergence-build*.log`，最终固件及源码 SHA256 见 `convergence-artifacts.json`：

```powershell
python verification/dual-mic-barge/test_audio.py
python verification/standby-wake/test_state_machine.py
powershell -NoProfile -ExecutionPolicy Bypass -File verification/music-rx/test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

主机测试编译实际生产回调/DSP 和协议函数，覆盖日志开关及两种下混方式，三轮模拟延迟 1.5–2 s 到音、2231/3672 启动残差、24 帧后坏帧重置、均值基线、收敛/检测等号边界、长时间不收敛、低音量、缺 pre/缺全部回调、采集断点、tick 回绕、100 ms/3 s 曲线限频、默认/非整十/最长窗口、6 帧和同代单次、stop skip/armed/重复 stop、PCM 排空、KWS abort、Standby 零上行、会话保持、音乐 RX/3A 保留。

检测器由 16 增至 20 字节；增加 32 位残差累加器及连续收敛帧计数，并利用布局空隙。曲线限频使用固定静态 tick/generation/valid 字段。没有新音频缓冲、浮点运算或逐帧堆分配；音频临界区仅更新状态和发布既有固定请求，日志在退出临界区后输出。没有修改 AEC 算法、麦克风增益、IoT/音乐命令和 UI 状态机。

## 用户上板复测

1. 连续三次 reboot，每次先等 `connected=1 state=3` 再唤醒，再执行 `xz_listen_text 请慢慢讲一个小故事`。无人声时均不得出现 BARGE_IN / SPEAK_INTERRUPT / WS_STANDBY；结束时 session=1，且每轮能看到 armed 或 skip。
2. 对齐同一 generation 的 tts start 和曲线时间。音频延迟到达前保持 train；实际起播后的首秒观察 ratio 从约 600/1000 降至 10–20/1000，并在满足连续窗口后出现 armed。若始终未出现合格窗口，应 stop skip，不能出现超时武装。真实曲线和低音量门槛仍需由板上数据确认。
3. armed 后真人在 2–3 m 讲话，应只出现一次 BARGE_IN 和 `SPEAK_INTERRUPT source=speech`，随后 `UI_STATE listen`、session=1；检查实际延迟、远场灵敏度和播放尾音。训练期说话可使本轮延后武装或 skip，这是 fail-closed 预期。
4. 确认提示音和 Thinking 不参与检测、Standby 上行仍为零、IoT/音乐和 KWS 行为不变。主机替身不模拟真实房间 AEC 收敛，不能替代以上硬件验收。
