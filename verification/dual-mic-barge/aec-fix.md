# 首轮 TTS / AEC barge-in 修复（2026-11-16）

此为上一轮历史方案。实测发现 TTS 到音延迟会使固定时间训练过早武装；当前实现及复测说明见 [convergence-fix.md](convergence-fix.md)，已改用连续收敛确认和窗口残差均值。

仅编译、主机回归和静态检查；没有烧录，没有访问串口或 COM8。原有双麦版本已在工作区，修改在该版本上完成，未撤销其已有改动。当前 SDK 的 raw → 3A → post 同步回调适配已存在，本次没有修改 SDK 或补丁 0005。

## 判据与默认值

每个 speaking generation 单独训练，窗口从现有 `ws_barge_start_tick`（tts start 进入 Speaking 的时刻）计时。`echo_floor` 是窗口内 **10 ms post-AEC RMS 的最大值**，不是 PCM 样点 peak，也不是 2 s `MIC_PCM mono_rms` 平均值。训练期忽略 VAD 结果，均计入峰值；训练结束后 floor 冻结，下一代重新计算。

```text
echo_floor = max(post_rms[t]), 0 <= t - tts_start < TRAIN_MS
threshold = max(RMS, ECHO_MULT * echo_floor)
合格帧 = 活跃 Speaking && 非提示音/断开 && 未收到 tts stop
         && 有效配对 pre 数据 && pre_rms > 0 && WebRTC VAD == 1
         && post_rms >= threshold && post_rms * PRE_RATIO >= pre_rms
触发 = 训练结束后连续 FRAMES 个合格 10 ms 帧；每代最多发布一次
```

所有数值为整数。pre_rms 为同一帧原始左右两路 RMS 的较大值，取自下混与 3A 之前；避免平均下混的相位相消使回声比值虚高。pre 回调只记录参考，不参与 KWS/VAD/编码/触发；参考只允许使用一次。该统计独立于 `CONFIG_XZ_MIC_LEVEL_LOG`。

| 配置（`app/project/Kconfig.proj`） | 范围 | 默认 | 理由 |
|---|---|---|---|
| `CONFIG_XZ_BARGE_IN_TRAIN_MS`（新增） | 600–2000 ms | 800 | 替换 200 ms 预热，为重启首轮 AEC 留更长收敛时间 |
| `CONFIG_XZ_BARGE_IN_ECHO_MULT`（新增） | 3–4 | 4 | 用训练峰值抑制残差波动；允许经实测调整为 3 |
| `CONFIG_XZ_BARGE_IN_PRE_RATIO`（新增） | 2–32 | 8 | 要求 post/pre ≥ 1/8，拒绝现场 1/50–1/100 的纯回声 |
| `CONFIG_XZ_BARGE_IN_RMS`（保留） | 1–32767 | 600 | 仅作绝对噪声下限，没有提高固定阈值 |
| `CONFIG_XZ_BARGE_IN_FRAMES`（保留） | 3–20 | 6 | 维持 60 ms 连续性，过滤孤立尖峰 |

Kconfig 的 CONFIG_ 名称写入 proj.conf / .config；SDK 生成的 C 宏为无 CONFIG_ 前缀的 `XZ_BARGE_IN_*`。增大 PRE_RATIO 会放宽双讲条件，不能将它理解为越大越严格。触发使用乘法原始整数比较；日志千分比的取整不参与判决。

## 生命周期

| 事件/阶段 | 检测器行为 | 会话/播放行为 |
|---|---|---|
| tts start | 在现有 Speaking 边界前同步清空；新 generation 的首次音频帧建立训练状态，按原边界 tick 计时 | 原流程暂停音乐、打开 TTS |
| 训练中 | 禁止自动语音触发，连续计数为 0；记录每个 post 帧 RMS 最大值，包括 VAD 非语音帧 | 保持会话和播放 |
| 正常播放 | floor 冻结；逐帧同时检查能量、双讲、VAD 和连续帧数；缺 pre、VAD 错误、失败帧或 >30 ms 回调间隙清连续计数 | 成功后沿用 wake_word_detected abort → 关 TTS → Listening，保留会话 |
| tts stop | 收到消息即同步清空 floor/frames/fired/generation/pre 参考及 pending 请求；stop pending 期间禁止新的自动请求，消费端也拒绝旧语音请求 | 保持原 PCM 排空后结束 Speaking 的行为；不截断正常尾音 |
| abort | ws_send_speak_abort 在网络发送前同步清空检测器及 pending 请求；音频线程退出 Speaking 后继续保持清空 | 原 abort reason 和状态迁移不变，无新增 Standby、goodbye 或重连 |

完全错过训练窗口时不以 floor=0 直接武装；这一代自动检测保持关闭，下一代重新训练。提示音、Thinking、Standby 和断开条件下不检测。KWS 仍可显式打断 stop 后的 PCM 排空，保留 wake_word_detected 语义。

## 日志

新增日志不依赖 `CONFIG_XZ_MIC_LEVEL_LOG`，没有逐帧输出：

```text
BARGE_TRAIN generation=... phase=start ms=800 echo_mult=4 pre_ratio=8
BARGE_TRAIN generation=... phase=ready floor=... threshold=... rms=... pre=... ratio=.../1000 pre_valid=...
BARGE_MON generation=... training=0 floor=... threshold=... rms=... pre=... ratio=.../1000 pre_valid=1 frames=...
BARGE_IN rms=... floor=... pre=... ratio=.../1000 frames=6 generation=...
```

`BARGE_TRAIN` 正常每代 start/ready 各一行；`BARGE_MON` 与现有 MIC_PCM 的 2 s 周期同步，使用该次最新 10 ms 判决快照；`MIC_PCM mono_rms` 是累计窗口的 RMS，两者不必相等。`ratio=125/1000` 即 post/pre=0.125，默认双讲门限边界。`BARGE_IN` 只在主事件线程接受打断时打印，数据取触发帧快照，训练/监测日志不会使用该前缀。

现有 `MIC_L/MIC_R`、`MIC_PCM`、`SPEAK_INTERRUPT source=speech/kws/button generation=...`、`SPEAK_ABORT reason=wake_word_detected(1)/none(0)` 格式/语义保留。没有新增 WS_STANDBY 路径。

## 本机验证与资源

```powershell
python verification/dual-mic-barge/test_audio.py
python verification/standby-wake/test_state_machine.py
powershell -NoProfile -ExecutionPolicy Bypass -File verification/music-rx/test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

测试提取并编译实际生产 C 回调、检测器、协议与 SDK 适配分支；没有板卡 I/O。覆盖日志开/关及两种下混、683/3746 RMS 启动残差三轮模拟、训练尾部峰值、3x/4x 系数、600 RMS/8:1/6 帧等号边界、50:1 回声拒绝、左右反相、缺 pre、错过训练、tick 回绕、VAD/提示音/四态门控、同代单次触发、stop 清理和排空、旧请求拒绝、实际 abort JSON 与会话保持。

检测器仍为 16 字节，固定打断请求通过 16/8 位字段收紧后仍为 16 字节；新增 pre 缓存 4 字节。无新音频缓冲、逐帧堆分配、浮点运算或回调内网络 I/O。32 位门限保留完整结果，训练 floor=32768 时门限 131072，不截断为可误触发的范围。

`aec-preserved.json` 记录与本次编辑前工作区的比对：ws_set_state / ws_enter_standby / ws_end_speaking / ws_abort_speaking、上行/播放门控、麦克风打开、播放停止、提示音、IoT 描述符和受约束命令函数未改；main.c、xiaozhi_ui.c、xiaozhi_music.c、build.ps1 字节不变。构建/测试日志和产物校验值分别见 aec-build.log / aec-tests.log / aec-artifacts.json。

## 仍需上板确认

1. 连续三次 reboot → 唤醒 → `xz_listen_text 请慢慢讲一个小故事`，每次首轮无人声时均不出现 BARGE_IN / SPEAK_INTERRUPT / WS_STANDBY；xz_wsstat 的 session 必须保持 1。
2. BARGE_TRAIN ready 是否在首轮 TTS 捕获到实际启动残差峰值；随后 BARGE_MON floor 固定、rms 降至约 10–70、pre_valid=1。验证正常音量及不同 TTS 音量均不自触发。
3. 训练完成后在 2–3 m 正常室内说话：应仅一次 BARGE_IN → SPEAK_INTERRUPT source=speech → UI_STATE listen；abort reason=wake_word_detected，session=1，音乐互斥与恢复正常。
4. 主机测试用确定性替身替代 AEC/VAD，无法证明真实房间、音量和远场效果。保留 600 下限不等于已证明远场灵敏度：如 floor=683，4x 门限就是 2732；3x 仍为 2049。应依据 floor/threshold/rms/pre 日志评估是否将 ECHO_MULT 改为 3，不能以放弃自适应/双讲条件来换取通过。训练期有人说话也会计入峰值，须在现场检查其影响。
