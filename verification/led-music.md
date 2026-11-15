# RGB / SD 音乐交付说明

仅编译，不烧录，不访问 COM8。构建命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
```

构建日志：`verification/led-music-build.log`。产物目录：
`app/project/build_sf32lb57-p045vr04_hcpu/`，使用该目录整套最新构建产物
（包括中文字体 ER_IROM3），保持现有分区和烧录地址。

最终构建结果：`scons: done building targets.`，无编译错误；仍有工具链原有
GNU-stack / RWX 链接警告及无 acpu 镜像提示。已核对编译数据库中四个
软件音频源的 allocator include 和 map 中的实际引用。
map 中音乐五个大段均位于 `0x60...` PSRAM（音频池 `0x6010fc54`，
栈 `0x6018fc54`，播放列表 `0x60191654`）。静态 RAM 用量 204,280 B，
PSRAM 2,184,672 B；这些是链接统计，不代表运行时空闲主堆或最大连续块。

## 灯控

新增 `app/src/xiaozhi/iot/light.cc`，在 `iot_c_api.cc` 注册 `Light`。
属性 `power` 是 boolean；`color` 是
`red/green/blue/yellow/cyan/magenta/white`，不接受其他值。

服务端按现有 IoT 协议下发，例如 commands 数组中的单条命令：

```json
{"name":"Light","method":"SetColor","parameters":{"color":"red"}}
```

```json
{"name":"Light","method":"SetPower","parameters":{"power":false}}
```

`SetColor` 设置颜色并开灯；`SetPower(true)` 恢复上次颜色（初始蓝色）。
关闭时保留所选颜色。沿用 WebSocket IoT 分支的 `iot_invoke()` →
`send_iot_states()`；hello 时描述和状态包含 Light。没有开启 MCP。

离线 shell：

```text
xz_led off
xz_led on
xz_led r
xz_led g
xz_led b
xz_led rg
xz_led rb
xz_led gb
xz_led rgb
```

`rgb` 为白色，`on` 恢复上次颜色。硬件只有七种 RGB 组合和全关，
**AW9523 P1_1/P1_2/P1_3 为普通 GPIO，无 PWM、无亮度调节**。
按现有驱动的 LED off 初始值修正为低电平点亮。
已确认 SDK `aw9523_init()` 写 LEDMODE1(0x13)=0xff、CONFIG1(0x05)=0x00，
所以三路均为 GPIO 输出，无需重复改方向寄存器。

板级 `board_hardware.h` 新接口：

- `board_led_set(r,g,b)`：非零点亮对应通道，返回硬件写入结果。
- `board_light_set_power(bool)` / `board_light_set_color(const char *)`：IoT 和 shell 共用入口。
- `board_light_power()` / `board_light_color()`：最后一次成功写入的软件状态。

保留的 `mcp/rgbled_mcp.cc/.h` 已去掉旧 PA32/PWM/rgbled 设备依赖，
即使误调用也只访问 AW9523，不更改 SoC 管脚复用，不新建灯效线程。
`iot_c_api.cc` 同时修正原 JSON 打印泄漏和 new[]/释放不匹配。

## SD 音乐

`app/project/proj.conf` 开启 FatFs、UTF-8 长文件名、栈上 LFN 工作区、
ROMFS 根目录和软件混音。该 SDK 的 DFS 本来就编译 `dfs_posix.c`，
没有名为 `DFS_USING_POSIX` 的配置项，无需为目录/文件 API 增加选项。

SDMMC2 保持 AIC Wi-Fi；新增 SDMMC1 单线 SD 卡模式，使用现有板级
PA08/PA09/PA10 复用，不改其余引脚。启动的独立 `xz_music` 线程延后
3 秒尝试挂载到 `/sdcard`，失败打印日志，15 秒重试，也可手动 mount。
仅将 block 类的 sd0/sd1 交给 FatFs，不把 Wi-Fi SDIO function 当作磁盘。
用只读 ROM 根目录提供 `/sdcard` 挂载点，不格式化 SD 卡。

新增 `app/src/xiaozhi/xiaozhi_music.c/.h`：

| 接口 | 用法 |
|---|---|
| `int xz_music_mount(void)` | 重试挂载；0 成功，负数失败 |
| `int xz_music_scan(void)` | 扫描根目录及一级子目录，最多 64 首 `.mp3`（扩展名忽略大小写）；返回数量或负数；播放期间返回 busy |
| `int xz_music_play(const char *keyword)` | NULL/空串从列表第一首顺序播放；非空按文件名子串匹配第一首，区分大小写；播完下一首，末尾回第一首 |
| `void xz_music_stop(void)` | 关闭播放器，清除恢复意图 |
| `bool xz_music_is_playing(void)` | 正在输出音乐时 true；TTS 暂停期间 false |
| `void xz_music_tts(bool speaking)` | 开始 TTS 前暂停；停止/打断/断线关闭 TTS 输出后恢复，期间新 play 请求保持暂停 |
| `bool xz_music_handle_text(const char *text)` | 集中本地关键词识别，异步投递，不消费原 STT |

路径最多 511 UTF-8 字节，超长路径跳过并日志；关键词最多 255 字节。
顺序按完整路径字节序排序。`list` 在停止状态重新扫描；放歌时列出现有列表。

```text
xz_music mount
xz_music list
xz_music play
xz_music play 夜曲
xz_music stop
```

语音例子：“播放音乐”“来点歌曲”“听SD里的歌”“停止播放音乐”“别放歌了”。
停止优先匹配，防止“别放音乐”触发播放。原 STT UI 和服务端对话照常执行。

`xiaozhi_websocket.c` 新增 STT 和 tts start 接点；`xiaozhi_audio.c` 在关闭
TTS 输出后解除音乐暂停，因此 stop、abort、sleep、断线都能释放暂停。
TTS 门控使用独立输出锁，不排在 SD 挂载/扫描队列后面。

`xiaozhi_ui.c/.h` 新增 `EV_MUSIC` / `xiaozhi_ui_music(name, paused)`：
通过 `ui_post` 在 `xz_ui` 显示 `Music: 曲名` 或 `Paused: 曲名`，停止后恢复
当前对话状态标签。曲名过长省略，UTF-8 截断不拆字符；中文使用现有
DroidSansFallback 字体，16 个字形缓存，内存由 LVGL PSRAM 堆管理。
未更改圆脸绘制代码及三个受保护的状态机函数。

## 内存与构建接线

`app/project/SConscript` 接入现有 `app/font/SConscript`，并为 SDK 的四个
软件音频源单独加 allocator forced include；SDK 文件没有修改。

- 新增 `xiaozhi_music_alloc.h`：MP3 控制器、Helix buffers、软件重采样用固定 PSRAM 池。
- 新增 `xiaozhi_music_server_alloc.h`：audio_server 的 calloc 客户端/命令/软件环形缓冲用同一池；malloc 分配的 DMA 缓冲仍用内部 SRAM，free 按指针所属池释放。
- 池大小 512 KiB，播放列表 32 KiB、工作线程栈 6 KiB、队列与当前路径均为 `L2_RET_BSS_SECT`；只初始化一次，无新增每帧 malloc。
- 软件混音保证音乐输出和现有 AEC TXRX 客户端同时工作；TTS 用显式暂停保证互斥。

## 留给上板验证的项目

尚未做任何硬件运行验证：LED 实际极性/颜色顺序、SDMMC1 TF 卡枚举、
FAT 卡长中文文件名、插卡后的驱动重新枚举/热插拔、MP3 连续循环、
TTS/打断/断线恢复、音乐时麦克风上行与 AEC、Wi-Fi 32 KiB 连续分配和
长时间主堆碎片情况、中文曲名实际显示均需用户烧录后确认。
此版本启用 FAT/FAT32；未开启 exFAT。建议先插入已备好 MP3 的 FAT32 卡再启动。
