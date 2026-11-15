# SF32LB57 小智连续上行修复与实机验证

## 压力验收结果

`stress-11`：COM8 / 1,000,000 baud / 8N1，从首次 WebSocket 101 响应后计时 **140.076 秒**。
循环离线合成语音，以 16 kHz、单声道、PCM16 输入现有 Opus 编码器，每 60 ms 一帧；使用应用原本的 realtime 聆听模式。

| 项目 | 实测 |
| --- | ---: |
| `sdio buf alloc fail` | 0 |
| `[rtos]malloc fail` | 0 |
| `node->buf is null` | 0 |
| `WS_TX_STALL` | 0 |
| `ws tx backpressure`（发送重试超限丢帧） | 0 |
| `ws on_connect` | 1 |
| `ws lost` / 服务端 CLOSE | 0 / 0 |
| assert / fatal error | 0 / 0 |
| 成功提交 WebSocket 的音频帧 | 至少 2300 |
| 第 50 至第 2300 帧的时间跨度 | 135.063 秒 |
| 服务端 STT / TTS 消息 | 22 / 66 |

`ws tx sent` 在 WebSocket 写入返回 `ERR_OK` 后计数；不是只统计编码器生产的 `uplink` 帧。
成功发送进度从 2.642 秒的 50 帧持续增长到 137.705 秒的 2300 帧，同一连接期间没有重建会话。
服务端持续返回识别及语音消息，确认上行数据到达并被处理。

证据：[完整计数及时间序列](stress-11.json)、[验收断言结果](acceptance.json)、[提取后的安全日志](stress-11.evidence.log)、[压力固件及源码 SHA256](stress-firmware-sha256.json)。
原始串口日志保存在本机 `stress-11.log`；提取日志不包含 Wi-Fi 凭据、Cookie、会话标识及识别正文。

## 改动及原因

### 1. 将软件缓冲搬至 PSRAM，给 SDIO 留出内部 SRAM

| 内容 | 字节 | 存储 |
| --- | ---: | --- |
| `xz_audio` 编解码状态 | 8460 | `L2_RET_BSS_SECT(xz_audio_state)` |
| 编码输入 ring | 7680 | `L2_RET_BSS_SECT(xz_encode_rb)` |
| VAD ring | 3840 | `L2_RET_BSS_SECT(xz_vad_rb)` |
| WebSocket TX ring | 8192 | `L2_RET_BSS_SECT(xz_tx_rb)` |
| WebSocket TX 线程栈 | 3072 | `L2_RET_BSS_SECT(xz_tx_stack)` |
| 合计 | **31244（30.5 KiB）** | PSRAM |

修改 `app/src/xiaozhi/xiaozhi_audio.c`、`xiaozhi_websocket.c`。
ring 使用静态 PSRAM 存储配合 `rt_ringbuffer_init/reset`，不再对静态内存执行 `destroy/free`。
TX 线程使用 `rt_thread_init` 和 PSRAM 栈，初始化失败时清理 event。
SDIO 线性 DMA 缓冲仍由内部 SRAM 堆分配；没有增加内部 `.bss` 大块池，没有修改驱动库 ABI 或关闭零拷贝。

retained PSRAM 在软复位后可能留下旧值，因此新增普通 SRAM BSS 的首次启动标志，在初始化前清空 `xz_audio`，避免复用旧线程和音频句柄。

带上行和 TTS 的压力采样：内部 heap total **262812 B**，used **222704–224240 B**，采样时空闲 **38572–40108 B**；
maximum allocated 为 **231896 B**，对应峰值余量 **30916 B**。释放空间后，原先失败的驱动申请会成功，因此不能仅用 used 的下降量判断释放效果。

### 2. 修复 SDIO IRQ 与 TX 的锁顺序死锁

仅释放内存后，仍实际复现了发送窗口停滞。控制台锁状态显示：

| 线程 | 已持有 | 正在等待 |
| --- | --- | --- |
| `fhost_tx` | `sdio_dev.bus_txrx` | MMC host 锁 `sd_bus_l` |
| `sdio_irq` | MMC host 锁 `sd_bus_l` | `sdio_dev.bus_txrx` |

修改 SDK 的 `customer/peripherals/wifi/aic8800d40-n/platform/hal/sdio_port.c` 中 `aicwf_host_sdio_interrupt()`：
在调用 AIC 同步 RX 回调前释放 IRQ dispatcher 持有的外层 host 锁，回调返回前重新持有。
每条实际 CMD52/CMD53 仍经 `mmcsd_send_request()` 自行持有 host 锁，TX/RX 的锁顺序统一为 `bus_txrx -> host`。
通用 SDIO core 和驱动线程优先级没有新增改动。

证据：[锁顺序分析](lock-order.md)、[死锁时控制台](stall-state.log)。这也解释了为什么内存失败归零之后仍需要第二项修复；没有通过修改 TCP 重传参数掩盖下层死锁。

### 3. 保留并完善可重复压力源

保留 `XZ_TX_STRESS` 开关，将测试输入改为离线语音 PCM，并扣除编码耗时来维持约 60 ms 喂帧周期。
测试 PCM、定时注入及屏蔽真实麦克风输入的代码均在 `#if XZ_TX_STRESS` 内；普通固件使用真实麦克风和原有聆听模式。
`stress_pcm.h` 是约 5.88 秒的循环语音，正文为 “Testing continuous audio upload. One two three four five.”。

此前静音以及手动聆听测试均观察到约 60–70 秒的服务端主动 CLOSE，即使分配失败和发送停顿已为零，也没有将这些轮次算作通过。
最终验收使用 realtime 模式的离线语音，没有增加周期重连、重复 listen 请求或服务端超时绕过。

用户原有 WebSocket `ERR_MEM`/pbuf 处理、退避重试、水位保护、`tcp_dbg_dump()`、`NET_TXBUF_CNT=28`、`NET_RX_LWIP_CNT=40` 均保留。

## 最终普通固件

源码已恢复 `XZ_TX_STRESS=0`，重新构建并实际烧录；ELF 符号检查确认不存在 `xz_stress_pcm` 及测试模式覆盖接口。
固定构建环境后再次构建，5 个烧录镜像的 SHA256 均保持一致，见 [普通固件 SHA256](release-firmware-sha256.json)。

烧录配置保持 `verify=true`，另用技能脚本独立读取每个镜像并逐字节比较，全部一致：

| 镜像 | Flash 地址 | 字节 | 本地 / Flash CRC32 |
| --- | --- | ---: | --- |
| bootloader | `0x12010000` | 40252 | `aabc976f` / `aabc976f` |
| ER_IROM1 | `0x12020000` | 2405812 | `516c9343` / `516c9343` |
| ER_IROM2 | `0x12320000` | 43 | `f591f3e6` / `f591f3e6` |
| ER_IROM3 | `0x12760000` | 43 | `6ee21932` / `6ee21932` |
| ftab | `0x12000000` | 11288 | `ec7013b1` / `ec7013b1` |

ftab 的 FCES magic 与 `0x12000000` 基址检查通过，见 [独立读回记录](verify-release.log)。
最终 map 也重新检查了全部迁移对象的 PSRAM 地址及启动标志的内部 SRAM 地址，见 [放置检查](memory-placement.json)。

普通模式烧录与独立读回后再次启动，21.920 秒连上服务器；从连接后继续采集 **60.228 秒**。
音频初始化成功，`ws on_connect=1`、`listen start,mode=2=1`，分配失败、发送停顿、断连、assert/fatal 均为 0；
没有合成压力帧发送。内部 heap total **262780 B**、used **219988 B**、max **226556 B**。
见 [普通模式启动计数](release-boot.json)。板上最终保留此普通固件，串口采集已结束。

## 补丁与复现

- [应用补丁](app-fix.patch)：相对本次会话开始时的 `*.before.c`，不会混入用户既有修改。
- [SDK 补丁](sdk-fix.patch)：相对 `sdio_port.before.c`，只包含 IRQ 锁顺序修复。
- `xiaozhi_audio.stress-tested.c` 保存实际压力固件的音频源码；与最终普通源码仅 `XZ_TX_STRESS` 的 1/0 不同。
- `plat_port.c` / `plat_port.h` 与会话开始备份逐字节相同，没有保留优先级试验。

复现时将 `XZ_TX_STRESS` 临时置 1，执行 `build.ps1`，在构建目录运行 `sftool -c SF32LB57 -m nor -p COM8 config sftool_param.json`，
随后从工程根运行 `python verification/tcp-window/capture.py NAME --seconds 140 --wait-connected`。
完成后恢复 0、重新构建烧录。串口采集与烧录必须顺序执行。

重复构建的镜像对比固定 `PYTHONHASHSEED=0`。SDK 证书生成器使用 `list(set(ROOT_CA_FILE))`，默认随机哈希种子会改变证书拼接顺序；未修改证书内容或信任集合。

```powershell
$env:PYTHONHASHSEED = "0"
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

## 剩余风险

本次完成的是当前硬件、网络和服务端下的 140 秒持续上行，并同时收到了 TTS 下行；未覆盖数小时运行、弱网或其他高峰并发内存负载。
驱动仍使用动态内部堆，新增功能仍需保持内部 SRAM 余量。PSRAM 迁移依赖当前链接段配置，换板型或改链接脚本时需重新检查 map，确保迁移对象继续位于 PSRAM。
