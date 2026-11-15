# Standby / Wake / Listening 验收说明

本次只在主机修改、编译和运行模拟测试，未烧录、未连接串口、未占用 COM8。

## 最终状态迁移

`kDeviceStateIdle` 对应 UI `FS_IDLE`，界面文字为 **Standby**。
`kDeviceStateUnknown / FS_SLEEP` 只保留给已有的显式休眠/关机路径。

| 当前情形 | 事件 | 动作 | 最终状态 | 麦克风上行 |
|---|---|---|---|---|
| 开机 / 连接恢复 | 收到初次 `hello` | 保存协商结果；不发 `listen_start` | Standby | 0 |
| Standby，连接及 hello 就绪 | KWS / 按键 pressed | 音频线程播 80 ms 提示音，等缓存排空及 40 ms 尾音保护，再发 realtime `listen_start` | Listening | 提示音期间 0；成功进入 Listening 后允许 |
| Standby，旧会话已 goodbye，WS 仍连接 | KWS / 按键 pressed | 重新 `hello` → Standby → 提示音 → realtime `listen_start` | Listening | 同上 |
| WS 未连接 | KWS / 按键 pressed | Connect → WS + `hello` → Standby → 提示音 → realtime `listen_start` | Listening | 连接和提示音期间 0 |
| Listening | 普通 `stt` → `tts.start` → `tts.stop` | Thinking → Speaking → Listening，保留激活会话 | Listening | Thinking 为 0；Listening / Speaking 通过 VAD 后允许 |
| Speaking | 按键 / KWS 打断 | 发 abort，停止当前播报逻辑，保持当前会话 | Listening | 允许；不重复新建会话 |
| 激活会话 | STT 命中“退下吧 / 退下 / 结束 / 不用了 / 关闭对话” | 先关闭上行、使音频代次失效，再发 abort + goodbye | Standby | 0 |
| 任意会话阶段 | 服务端 `goodbye`，包括服务端会话超时 | 清 active / hello-ready / pending-wake，停扬声器逻辑；不重连、不发 hello、不再聆听 | Standby | 0 |
| goodbye 后 Standby | 服务端随后关闭 TCP | 保持待命，连接监守线程等待下一次显式唤醒 | Standby | 0 |
| Speaking | 4 s 未收到服务端文本/音频（保留已有恢复逻辑） | 结束 Speaking，保留 realtime 会话 | Listening | 允许 |
| 等待 `hello` | 超过 10 s | 取消本次待处理唤醒，关闭该连接，重试连接；恢复后只进入待命 | Connect → Standby | 0，恢复后仍为 0 |
| 正常连接 / 激活会话 | 意外断线，非 goodbye 导致 | 清会话和音频代次；自动恢复 WS + hello；不恢复旧会话聆听 | Connect → Standby | 0，需再次唤醒 |
| 显式唤醒还在建连 / 播提示音 | 意外断线 | 保留该唤醒请求，废弃旧提示音完成事件；连接恢复后播放新提示音 | Connect → Standby → Listening | 新提示音成功前为 0 |
| 唤醒提示音 | 音频服务失败 / 缓存无法排空 | 恢复原音量，取消此次唤醒，不发 `listen_start` | Standby | 0 |

没有新增本地“静默 N 秒就退下”的策略。会话正常问答结束会继续 Listening；
服务端用 goodbye 结束会话时回 Standby。

## 上行是否为 0

生产代码的限制包含以下层次：

1. 原始麦克风保留给本地 KWS；Standby、Connecting、Thinking、提示音阶段不向编码器写入麦克风 PCM，也不走旧的普通 VAD 自动唤醒分支。
2. Opus 编码、发送队列入队都检查会话及状态；`xz_uplink` 是额外手动闸门。
3. 真正调用 `wsock_write(..., OPCODE_BINARY)` 前，在发送互斥锁内重新检查
   `connected && session_active && (Listening || Speaking) && xz_uplink`。
   状态发布与实际写入共用该锁，ERR_MEM 重试也重新检查。
4. goodbye / 断线递增音频代次；已编码、已排队、正退避的旧代次帧无法漏入待命或下一会话。
5. 提示音由现有 Opus 音频线程处理，不在 KWS 线程里等待。
   在提示音及尾音保护期间暂停送入 KWS，且会话尚未激活，因此本地提示音不会上行。

主机测试中，开机待命时连续投递 100 帧，实际二进制写入计数为 **0**；
goodbye 后的队列排空、ERR_MEM 重试、重新唤醒后的旧代次帧也都没有产生二进制写入。
这里的 0 指进入待命后不再新提交麦克风音频帧；已在状态切换前提交到 TCP 的数据无法撤回。
设备端尚未烧录实测，下面的计数器可用于用户串口验收。

## 串口日志和命令

UI 原有日志直接复用，UI 文件没有改动：

```text
UI_STATE connect
UI_STATE idle
UI_STATE listen
UI_STATE think
UI_STATE speak
```

其中 `UI_STATE idle` 就是界面上的 Standby。
普通对话返回顺序为 `listen → think → speak → listen`；退下时进入 `idle`。
没有实际状态变化时，原 UI 日志不会重复打印。

新增日志：

```text
WS_HELLO ready -> Standby wake_pending=0
WAKE_REQUEST connected=1 ready=1
WAKE_BEEP begin token=<n> ms=80 vol=6
WAKE_BEEP drained token=<n> result=0 restore_vol=<原音量>
WAKE_BEEP complete token=<n> -> listen_start
DISMISS_INTENT keyword=退下吧 text=<完整 STT>
WS_STANDBY reason=local_dismiss audio_tx=<累计成功提交帧数>
WS_STANDBY reason=server_goodbye audio_tx=<累计成功提交帧数>
```

新增诊断 / 失败分支日志：

```text
WS_AUDIO connected=<0/1> ready=<0/1> session=<0/1> state=<枚举值> wake=<0/1> uplink=<0/1> audio_tx=<累计帧数> blocked=<写入处拒绝次数> epoch=<音频代次>
WAKE_BEEP failed token=<n> err=<错误码>; stay Standby
WS_HELLO timeout; cancel wake, reconnect to Standby
WS monitor init failed: <错误码>
usage: xz_uplink on|off|1|0
```

原断线日志改为 `ws lost reconnect_allowed=<0/1>`，可确认 goodbye 后没有自动重连。
`play 1kHz ...`、`play done ...`、`private volume level ...`、`listen start,mode=2`
沿用既有提示音/控制实现中的日志。

手动验收建议：

1. 开机等 `UI_STATE idle`，执行 `xz_wsstat`。应为 `session=0 state=3 uplink=0`。
   暴露于电视/环境声一段时间后再次执行，`audio_tx` 应保持不变；此时仍可本地识别唤醒词。
2. 说唤醒词或执行既有 `xz_btn_sim 100`。应先出现 begin、drained、complete，
   然后 `listen start,mode=2` 和 `UI_STATE listen`。板上听感由用户确认。
3. 连续问两句话，确认每轮 `tts.stop` 后回 `UI_STATE listen`，不需重新唤醒。
4. 说“退下吧”，确认命中日志和 `UI_STATE idle`，随后两次 `xz_wsstat`
   的 `audio_tx` 不增长。服务端继续发送的迟到 TTS 不会重新激活会话。
5. 直接再次说唤醒词，不要求 KWS 产生 released。应重新 hello → 提示音 → Listening。
6. `xz_uplink off` 或 `xz_uplink 0` 应打印 `uplink=0`；`on` 或 `1` 恢复手动闸门。
   Standby 不会因为手动闸门 on 而发送音频。

## 内存与改动范围

- 新增连接监守线程的栈为 6144 字节，放在 `L2_RET_BSS_SECT(xz_ws_connect_stack)`。
  最终 map 地址为 `0x6020b8e0`，大小 `0x1800`，位于 PSRAM。
- 新线程控制块静态大小 `0x84`；发送互斥锁静态大小 `0x24`，替代原先动态创建的锁。
- 提示音复用现有音频线程和 audio_server client，不新增音频线程、播放 client 或每帧分配。
- UI 源文件 SHA256 与改动前完全一致；未改圆脸绘制、LVGL 线程接口、mic_gain、ADC 增益。
- 修复了原连接循环占用 main、后续 KWS 事件无法消费的问题。
  main 现在每 20 ms 检查唤醒完成，同时正常处理原有邮箱事件。
- 修复了 KWS 仅发 pressed 时，第二次唤醒被按键 last_action 去重丢弃的问题。
- 修复 `xz_uplink off` 原先按首字母 `o` 判断而误开启的问题。

生产文件仅修改 `app/src/main.c`、`xiaozhi_websocket.c/.h`、`xiaozhi_audio.c/.h`。
`before/` 保留本次任务开始时的这五个文件，不覆盖工程已有未提交修改。

## 构建与主机测试

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
python verification/standby-wake/test_state_machine.py
```

构建成功，日志为 `build.log`；无源码变更重建日志为 `build-noop.log`。
构建仍有工具链 / ftab 的链接告警，未将本任务扩展为修改 SDK 链接配置。
主机回归结果在 `host-tests.log`。测试提取生产 C 函数，链接 SDK 自带 cJSON，
仅模拟 RTOS / 网络 / 音频外设；覆盖退下、队列积压、重试、过期回调、音频失败及断线竞争。

固件目录：`app/project/build_sf32lb57-p045vr04_hcpu/`。
主应用为 `main.elf` 和 `main.bin/ER_IROM1.bin`、`ER_IROM2.bin`、`ER_IROM3.bin`；
烧录清单仍为工程生成的 `sftool_param.json`。
各应用二进制大小与 SHA256 记录在 `artifacts.json`。

额外的无源码变更重建对比中，`ER_IROM2.bin` / `ER_IROM3.bin` 哈希一致，
`ER_IROM1.bin` 哈希变化（该次构建重新生成并编译了 SDK 的 `tls_certificate.c`，
并重新链接主应用；尚未定位二进制差异的具体原因）。
因此这里只确认两次构建均通过，不声称现有整个构建链字节级可复现；
对比记录为 `rebuild-check.json`，`artifacts.json` 记录当前最终产物。
