项目说明
================================================================================
项目名称 : P045VR04-IPS 屏幕初始化(SPI初始化固件)
芯片型号 : PY32F002AW15S6TU (SOP16, Cortex-M0+, 24MHz, 20KB Flash / 3KB RAM)
适用硬件 : H743_RGB_Adapter 转接板 (原理图 04 - PY32 display initialization)
面板型号 : P045VR04-IPS (480x854, ST7701S, 3线SPI初始化 + RGB565显示)

依赖     : Puya PY32F0xx_Firmware_V1.5.0(HAL/CMSIS)。MDK 与 EIDE 工程都按相对路径
           ../../../PY32F0xx_Firmware_V1.5.0 引用, 请把该固件包放在仓库根目录
           (与 app/ board/ 同级); MDK 还需安装包内 Packs/MDK 下的器件包。
宿主     : 本固件与宿主无关。宿主为 STM32H743 时由 PG3 读 READY; 宿主为本仓库的
           SF32LB57 时, drivers/panel/p045vr04.c 用固定延时等待初始化, READY 可不接。

--------------------------------------------------------------------------------
一、本工程做什么
--------------------------------------------------------------------------------
H743只负责RGB接口时序(LTDC), 屏幕内部的ST7701S寄存器必须由本芯片通过3线SPI
(9bit: 1bit DC + 8bit 数据)在上电后配置。固件流程(对应原理图"FIRMWARE SEQUENCE"):

  1. READY=0; CS=1; SCL/SDA为3线SPI空闲电平(低);
  2. 等待电源稳定(PANEL_POWER_ON_MS), 复位面板(RST低10ms -> 高, 等120ms);
  3. 发送面板初始化表(与ESP32参考例程完全一致, 每条命令后按表等待);
  4. 所有延时结束后 READY=1;
  5. H743的LTDC初始化时等待PG3(READY)为高, 之后才允许打开背光.

当前状态会通过日志串口输出: USART1_TX = PA9, 115200-8-N-1.

--------------------------------------------------------------------------------
二、引脚分配(PA13/PA14保留给SWD)
--------------------------------------------------------------------------------
  PA0  LCD_SCK   3线SPI时钟   (经R8  100R)
  PA1  LCD_SDA   3线SPI数据   (经R9  100R)
  PA2  LCD_CS    3线SPI片选   (经R10 100R)
  PA3  LCD_RST   面板复位      (经R11 100R, 上拉R13 10k)
  PA6  READY     输出给H743(PG3), 初始化完成前为低电平
  PA9  TX_LOG    调试日志(USART1_TX)
  PA13/PA14      SWDIO/SWCLK

--------------------------------------------------------------------------------
三、目录结构
--------------------------------------------------------------------------------
  Inc/            头文件(main.h = 引脚定义/接口, panel_st7701.h = 面板参数)
  Src/main.c      主流程: 时钟(HSI 24MHz) -> GPIO -> 日志 -> 面板初始化 -> READY=1
  Src/panel_st7701.c  ST7701S初始化表 + 3线SPI软件时序 + 面板复位
  Src/py32f0xx_hal_msp.c  HAL引脚配置(USART1_TX=PA9)
  MDK-ARM/        Keil MDK工程(P045VR04_Init.uvprojx) + MDK启动文件
  EIDE/           EIDE(VSCode插件)工程: Makefile + 链接脚本 + 启动文件 + 工程配置
                  (与MDK工程共用同一份 Src/Inc 源码, 仅启动文件/工具链不同)

--------------------------------------------------------------------------------
四、编译与下载
--------------------------------------------------------------------------------
1) Keil MDK(推荐, 与 stm32h743 工程同一套环境):
   打开 MDK-ARM/P045VR04_Init.uvprojx -> 目标 PY32F002Ax5_Project -> Build / Download
   输出: MDK-ARM/Output/P045VR04_Init.hex(.axf)
   器件支持包: PY32F0xx_DFP, SDK内已附带(双击安装):
     PY32F0xx_Firmware_V1.5.0/Packs/MDK/Puya.PY32F0xx_DFP.1.2.6.pack
   工程配置: Device=PY32F002Ax5(Puya), ARMCC(AC5), C99, 宏 USE_HAL_DRIVER/PY32F002Ax5,
   包含路径 ..\Inc + SDK的CMSIS/HAL头文件, 启动文件 MDK-ARM/startup_py32f002xx.s,
   IROM 0x08000000(20KB) / IRAM 0x20000000(3KB)。

2) EIDE / GCC(可选, 与MDK工程共用同一份源码):
     cd EIDE
     make GCC_PATH=<gcc-arm-none-eabi的bin目录>
   生成 EIDE/Project/P045VR04_Init.elf/.hex/.bin

3) SWD下载: J6(2.54mm) VTREF/GND/SWDIO/SWCLK/NRST/TX_LOG, 对应PY_SWDIO/PY_SWCLK/PY_NRST.

--------------------------------------------------------------------------------
五、面板参数(H743 LTDC必须一致, 见 stm32h743/Drivers/BSP/LCD/ltdc.c)
--------------------------------------------------------------------------------
  分辨率      : 480(行内像素) x 854(行数)
  像素时钟    : 28MHz (PLL3: N=280, M=25, R=10)
  水平: HSW=10, HBP=30, HFP=30
  垂直: VSW=4,  VBP=15, VFP=12
  MADCTL      : 0x00 (RGB顺序, 不镜像)
  COLMOD      : 0x50 (16bit/RGB565接口格式, VIPF=0101)
                面板按每通道低位脚取数(R4..R0=DB20..16, G5..G0=DB13..08, B4..B0=DB04..00),
                所以转接板必须把H743的R3..R7/G2..G7/B3..B7对应接到面板
                R0..R4/G0..G5/B0..B4(标准RGB565接线);
                若用"高位对高位"接线的REV 0.1-DRAFT转接板(面板低位脚接地),
                需改回0x70(24bit/RGB888), 否则灰度/图片颜色会错乱.
  3线SPI      : SCL空闲低, 上升沿采样, DC=0命令/DC=1数据, MSB first, ~500kHz
                (与ESP32 esp_lcd_panel_io_3wire_spi 配置一致)

--------------------------------------------------------------------------------
六、面板SPI/复位接线确认(这几款屏的SPI脚位置不固定, 必须一块一块量)
--------------------------------------------------------------------------------
转接板上PY32是固定的(原理图04_PY32_Init): PA0=SCL(R8 100R), PA1=SDA(R9 100R),
PA2=CS(R10 100R), PA3=RESET(R11 100R, 10k上拉R13)。
屏座只把这4个网络引到第1~4脚(其余脚是RGB/GND/VCC/背光):
  J2(屏A): 1=SDA  2=SCL  3=RESET  4=CS
  J7(屏B): 1=SDA  2=SCL  3=CS     4=RESET
  两座相同: 5 GND / 6 NC或GND / 7 DE / 8 VSYNC / 9 HSYNC / 10 NC或VCC / 11 PCLK /
            12 GND / 13~20 B7~B0 / 21~29 G7~R7 / 30~36 R6~R0 / 37 VCC / 38 GND /
            39 LEDA / 40 LEDK

调试步骤(每块屏都做一遍):
  1) 断电、不插屏。万用表通断档量"屏FPC焊盘"与"PY32引脚PA0~PA3"(或R8~R11的PY32侧);
  2) 记下 面板SCL/SDA/CS/RESET 分别通到 PA 几;
  3) 在 Inc/main.h 里选映射并重新编译(默认已是 PANEL_MAP_P045VR04 = 本屏实测值):
       PANEL_MAP_P045VR04  直插J2实测: 屏SCL=PA0, 屏SDA=PA3, 屏CS=PA1, 屏RESET=PA2
       PANEL_MAP_STD       与转接板网络名1:1: PA0=SCL, PA1=SDA, PA2=CS, PA3=RESET
       PANEL_MAP_CUSTOM    任意置换, 自己填 PANEL_CUSTOM_SCL/SDA/CS/RST 四行
     (也可在编译命令里加 -DPANEL_MAP=x 覆盖, 便于同时出多份固件)
   实测记录(P045VR04-IPS, 直插J2): 屏CS -> PA1, 屏SCL -> PA0, 屏SDA -> PA3, 屏RESET -> PA2
  4) 必须确认屏的RESET没有接到3.3V/GND(若屏的SPI脚落在座的35~40脚, 那里是
     R1/R0/VCC/GND/LEDA/LEDK, 直插会把屏的RESET接到VCC —— 这种屏必须飞线,
     把屏的4个信号飞到R8~R11的PY32侧, 固件无法补救)。

结论: 4个信号只要都落在PA0~PA3(顺序不同没关系) → 只改固件选映射;
      落在其它网络(VCC/GND/LEDA/LEDK/RGB…) → 必须改硬件。

--------------------------------------------------------------------------------
七、更换其它面板
--------------------------------------------------------------------------------
转接板支持多种面板, 更换面板时只需修改:
  1. panel_st7701.c 中的初始化表(按厂家提供的初始化码);
  2. panel_st7701.h 中的分辨率/时序参数;
  3. Inc/main.h 中的 PANEL_MAP(SPI/复位引脚映射, 见第六节);
  4. stm32h743 工程中 ltdc.c/ltdc.h 的面板ID与时序(分辨率不同时)。
