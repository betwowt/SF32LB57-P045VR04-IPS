# fix7：参考归零后的 300 ms 回声拖尾保持

已完成工程修改、主机回归和固件编译；未烧录，未访问串口或 COM8，未修改 SDK 源码。

## 实现位置与边界

- `app/src/xiaozhi/xiaozhi_mic_dsp.h`：`XZ_BARGE_REF_MIN_RMS=50`、`XZ_BARGE_REF_HOLD_MS=300`；`xz_barge_step()` 保存 `ref_hold` 和 `ref_age_ms`。
- 有效配对的 `ref_rms >= 50` 更新保持值并将年龄归零；低参考时保留最近值。`ref_age_ms < 300` 使用 `pre*1000000 >= g*ref_hold*DTD_MARGIN_PM`，64 位乘积；`ref_age_ms >= 300` 恢复原有无播放分支。
- `app/src/xiaozhi/xiaozhi_audio.c`：沿用现有 `last_frame`，将实际 RTOS tick 间隔换算成毫秒传入检测器，不按收到的帧数估算时间。当前构建 `RT_TICK_PER_SECOND=1000`；缺帧也会计入年龄，tick 回绕用无符号差值处理，年龄累加饱和防止长静音溢出。
- `xz_barge_break_streak()` 不清除保持值或年龄。同步异常仍拒绝该帧并打断原连续窗口，保留 `BARGE_DTD_SYNC`；恢复配对后的零参考仍受剩余保持窗口约束。生命周期 reset/新 generation 清空保持历史。
- 原两帧参考包络不变，日志 `ref` 仍表示配对后的参考 RMS；`refh` 表示最近保持值。年龄达到 300 ms 后 `refh` 可仍非零，但不再参与 DTD。
- 削顶保护、仅收敛武装、floor/gain 冻结、每代一次、VAD、连续六帧、PDM gain=44、KWS/上行副本 +6 dB、判据数值和其他状态/音乐/IoT/提示音门控保持原行为。没有新增逐帧堆分配。

## 日志样例

以下是说明样例，不是实机采集。`BARGE_TRAIN phase=armed`、`BARGE_IN` 追加 `refh`/`ref_age`；原 `BARGE_CURVE` 和 `BARGE_MON` 也追加同样字段，便于看到被拒绝的拖尾帧。日志频率不变。

```text
BARGE_TRAIN generation=1 phase=armed reason=converged t=1613ms window_ms=250 frames=25 floor=30 threshold=150 rms=30 pre=345 ratio=86/1000 pre_valid=1 g=345 ref=1000 refh=1000 ref_age=0ms
BARGE_CURVE gen=1 t=1800ms pre=385 post=305 ratio=792/1000 state=armed pre_valid=1 clip=0 ref=0 ref_peak=0 g=345 dtd=0 sync=1 refh=1000 ref_age=100ms
BARGE_IN gen=2 source=speech pre=700 post=305 ref=0 g=345 dtd=1 rms=305 floor=30 frames=6 refh=1000 ref_age=60ms
SPEAK_INTERRUPT source=speech generation=2
```

第一代示例中 `pre=385 < 2*345*1000/1000=690`，虽然 post 超过 150 且参考归零，仍被保持 DTD 拦下。第二代示例中真人语音达到 DTD，连续六帧后可在保持期内触发。触发时将年龄、保持值和原指标一起快照到请求，避免主线程打印后续帧的值。

## 验证和产物

以下命令均通过，退出码为 0：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
python verification/dual-mic-barge/test_audio.py
python verification/standby-wake/test_state_machine.py
```

主机回归运行实际 DSP、参考包装、raw/post 回调与 WebSocket 发布/消费代码，覆盖：

- `g=345 / floor=30 / pre=385 / post=305`，参考为 0 或 49 时，拖尾在 299 ms 内不触发。
- 299/300 ms 精确边界、保持期内真人语音的 DTD 等号边界、连续六帧和每代一次。
- 超三帧序号失配保留保持值/年龄，恢复配对后仍拒绝拖尾；一秒采集间隙按实际时间过期并恢复绝对阈值。
- 参考 50 刷新为最近值、生命周期清理、tick 回绕；原 VAD、削顶、增益、AEC、提示音、Thinking/Standby、KWS、音乐恢复等回归继续通过。
- 中断请求保持触发时的 `refh`/`ref_age` 快照。

构建完成 `scons: done building targets.`，生成 `app/project/build_sf32lb57-p045vr04_hcpu/main.elf` 和 `main.bin/` 分段固件。构建仍有已有的链接器/ftab 告警，见 `tail-build.log`。源码/配置/产物校验值见 `tail-artifacts.json`，测试日志为 `tail-audio-tests.log`、`tail-state-tests.log`。

## 待用户上板确认

1. 五次 `reboot` → 唤醒 → `xz_listen_text 请慢慢讲一个比较长的故事`：零次误触发 `SPEAK_INTERRUPT`，每次 `phase=armed`，`MIC_PEAK clip=0`。
2. TTS 期间 2–3 m 真人说话：单次 `SPEAK_INTERRUPT`，触发日志 `dtd=1`、`ref_age<300ms`。
3. 已武装后参考静默一秒再说话：恢复原绝对阈值、floor 倍数和 VAD 判据，仍能触发。

主机模拟通过不能替代上述实机声学确认。
