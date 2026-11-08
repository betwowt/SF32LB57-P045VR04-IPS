# H743 RGB 屏幕转接板

本目录是可直接打开的 **KiCad 10** 项目。当前交付范围为 **原理图、项目内符号/封装、PCB 元件布局**，不含 PCB 布线、生产文件或屏幕初始化固件。

打开 `正点原子h743-RGB转接板-v2.kicad_pro`。主原理图为 `正点原子h743-RGB转接板-v2.kicad_sch`，PCB 为同名 `.kicad_pcb`。原理图 PDF 和检查报告在 `output/`。

## 本仓库收录范围

只收录设计源文件：工程/原理图/PCB、项目库（`Adapter.*`、`easyeda_convertlib.*`）、
`docs/`、`scripts/`、原理图 PDF。以下目录为生成物或第三方资料，未入库：

| 未入库 | 说明 |
|---|---|
| `output/`、`Gerber/`、`*Gerber.zip`、`bom/` | 由 KiCad 重新导出 |
| `.history/`、`backups/`、`tmp/` | KiCad 自动备份 |
| `references/` | 厂商 datasheet（AP2112 / PY32F002A / SY7200A 等），自行下载 |

## 已采用的方案

- J1：正点原子 H743 开发板 RGB 接口，40Pin、0.5mm，按提供的图 1 编号。
- J2：图 2 的屏幕线序；J7：图 3 的屏幕线序。**两个独立插座，各自固定接线**。
- 实际是 **五款屏幕、两种接口线序**。按所插屏幕手动刷写对应的 PY32 初始化程序，没有屏幕选择跳线、自动识别或通用初始化程序。
- 两个屏幕座共用 RGB、SPI、复位和背光电路，**一次只接一块屏幕**。三个触摸座也一次只接一个。
- RGB565 的 16 根有效数据线及 PCLK/HSYNC/VSYNC/DE 共用 5 组 **33Ω×4 独立排阻**，0603×4 / 3216 封装，参考料号 CAY16-330J4LF。每组内部四路互相独立，不能换成共端排阻。
- PY32F002AW15S，SOP16；PA0=SCK、PA1=SDA、PA2=CS、PA3=RESET。这些是 GPIO 软 SPI 分配，不依赖硬件 SPI 复用。
- AP2112K-3.3 从开发板 5V 产生本板 3.3V；背光参考 T113_MIPI_Adapter 的最终版本，采用 SY7200ABC、4.7µH、SS14、5.1Ω 采样。L1 使用参考工程已确认的 2.5×2.0mm C5832360 封装，反馈电阻值保持不变。
- PA6 的 READY 信号控制背光许可，并送到 H743 PG3。上电未初始化时背光关闭；初始化程序完成后应把 PA6 拉高。

## 文件索引

| 文件 | 用途 |
|---|---|
| `正点原子h743-RGB转接板-v2.kicad_sch` | 开发板接口、RGB 排阻、层次页入口 |
| `02_LCD_Touch.kicad_sch` | 两个屏幕座、三个触摸座 |
| `03_Power_Backlight.kicad_sch` | 3.3V、背光升压、背光使能 |
| `04_PY32_Init.kicad_sch` | PY32、复位、软 SPI、SWD |
| `Adapter.kicad_sym` | 项目自带符号库 |
| `Adapter.pretty/` | 项目自带封装库 |
| `sym-lib-table` / `fp-lib-table` | 使用 `${KIPRJMOD}` 的相对库路径 |
| `docs/design_notes.md` | 完整接线、封装依据、未确认参数 |
| `docs/BOM.csv` | BOM；元件值和待确认项 |
| `docs/connector_pinout.csv` | 各连接器逐脚网络 |
| `docs/verification.md` | 验证范围和结果 |

## PCB 布局

当前板框 **78×75mm**，4 层规划；板框尺寸和安装方式为本次布局的暂定值，尚无外壳/孔位约束。J1 从左侧插入，两个屏幕座在右侧上下排列，三个触摸座沿上边排列，PY32/SWD 位于下方。左上是 LDO，右上是背光升压。

5 组 RGB 排阻放在 J1 后方，中央留走线空间。本阶段无走线、无过孔、无铺铜。DRC 中的未连接项是保留的飞线，不应据此误认为可以直接制板。

## 确认边界

用户已确认三个 40Pin 接口间距均为 0.5mm；还没有提供五款屏幕完整规格、背光串并联关系/电流、排线厚度及开发板至本板排线的同面/异面关系。当前封装采用有资料的 Omron XF2M 双面接触座候选，**不能仅凭 40Pin/0.5mm 判断实际排线完全适配**。具体边界见 `docs/design_notes.md`。

脚本仅供复现。手动编辑 KiCad 文件后不要重新运行生成脚本，否则会覆盖设计。`scripts/build_project.py` 生成原理图，`scripts/build_pcb.py` 需要使用 KiCad 自带 Python，并在它之前导出 XML 网表。
