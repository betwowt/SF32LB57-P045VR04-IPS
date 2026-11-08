# sf32lb57-p045vr04

SiFli SF32LB57-LCD DevKit（SF32LB57GYD7N6，N16R16：MPI1 PSRAM 16 MB +
MPI2 NOR 16 MB，MPI3 未贴片）+ P045VR04-IPS 480×854 RGB565 面板 +
FT6236 触摸。

- HCPU 运行 RT-Thread + `rt_driver` 演示；
- LCDC 使用 DPI，RGB565，28 MHz；
- 面板 3-wire SPI 初始化由 H743_RGB_Adapter 上的 PY32F002 完成；
- 背光 PA_11 / GPTIM2_CH2，触摸 I2C1（PA_22/PA_23），触摸中断 PA_20；
- 面板/PY32 复位由 AW9523 P1_7 控制，触摸复位由 AW9523 P1_6 控制。

关键文件：

- `hcpu/board.conf`：`CONFIG_LCD_USING_P045VR04_IPS_DPI=y`
- `hcpu/Kconfig.board`：板级符号 `BSP_USING_BOARD_SF32LB57_P045VR04`，
  可选 `LCD_READY_PIN`
- `../../drivers/panel/p045vr04.c`：DPI 时序与 LCD 操作
- `../../drivers/touch/ft6236.c`：FT6236 触摸

时序：HSW=10/HBP=30/HFP=30，VSW=4/VBP=15/VFP=12，PCLK=28 MHz。

接线与构建说明见工程根目录 `README.md`。
