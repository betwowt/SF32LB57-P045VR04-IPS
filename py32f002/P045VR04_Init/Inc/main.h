/**
  ******************************************************************************
  * @file    main.h
  * @brief   P045VR04-IPS 屏幕初始化(PY32F002AW15S6TU) 头文件
  * @note    硬件: H743_RGB_Adapter 转接板 "04 - PY32 display initialization"
  *
  *          PY32管脚分配(与转接板原理图一致):
  *            PA0  LCD_SCK   : ST7701S 3线SPI时钟   (经R8  100R)
  *            PA1  LCD_SDA   : ST7701S 3线SPI数据   (经R9  100R)
  *            PA2  LCD_CS    : ST7701S 3线SPI片选   (经R10 100R)
  *            PA3  LCD_RST   : 面板复位             (经R11 100R, 上拉R13 10k)
  *            PA6  READY     : 输出给H743(PG3), 初始化完成前为低
  *            PA9  TX_LOG    : USART1_TX 调试日志(115200-8-N-1)
  *            PA13/PA14      : SWDIO/SWCLK, 保留给SWD下载(不做任何配置)
  *
  *          固件流程(对应原理图"FIRMWARE SEQUENCE"):
  *            1. READY=0; CS=1; SCL/SDA为3线SPI空闲电平(低);
  *            2. 等待电源稳定, 复位面板;
  *            3. 发送面板初始化表(3线SPI, 9bit);
  *            4. 等待所有延时, 置READY=1;
  *            5. H743检测到PG3为高后才打开背光.
  ******************************************************************************
  */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "py32f0xx_hal.h"
#include "panel_st7701.h"

/* Private defines -----------------------------------------------------------*/
/* 面板3线SPI/复位/READY引脚(全部在GPIOA)
 * 转接板上PY32固定接到4个网络(原理图04_PY32_Init, 各串100R; 屏座J2/J7只把这4个网络
 * 引到第1~4脚):
 *   PA0 -> LCD_SCL   R8  100R              (J2.2 / J7.2)
 *   PA1 -> LCD_SDA   R9  100R              (J2.1 / J7.1)
 *   PA2 -> LCD_CS    R10 100R              (J2.4 / J7.3)
 *   PA3 -> LCD_RST   R11 100R(R13 10k上拉) (J2.3 / J7.4)
 * 不同屏的这4个信号在FPC上的位置/顺序不同, 必须先用万用表通断实测(屏脚 -> PY32脚),
 * 再选下面的映射. 只能在PA0/PA1/PA2/PA3之间置换;
 * PA6=READY, PA9=TX_LOG, PA13/PA14=SWD 不可改.
 *
 *   PANEL_MAP_P045VR04 : 本屏实测(直插J2): 屏SCL=PA0, 屏SDA=PA3, 屏CS=PA1, 屏RESET=PA2  ←默认
 *   PANEL_MAP_STD      : 与转接板网络名1:1: PA0=SCL, PA1=SDA, PA2=CS, PA3=RESET
 *   PANEL_MAP_CUSTOM   : 实测任意置换, 填下面 PANEL_CUSTOM_xxx 四行
 */
#define PANEL_MAP_P045VR04              1
#define PANEL_MAP_STD                   0
#define PANEL_MAP_CUSTOM                9

#ifndef PANEL_MAP
#define PANEL_MAP                       PANEL_MAP_P045VR04
#endif

#define LCD_PANEL_PORT          GPIOA

#if   (PANEL_MAP == PANEL_MAP_P045VR04)
  #define LCD_SCK_PIN           GPIO_PIN_0          /* 屏SCL   -> PA0 */
  #define LCD_SDA_PIN           GPIO_PIN_3          /* 屏SDA   -> PA3 */
  #define LCD_CS_PIN            GPIO_PIN_1          /* 屏CS    -> PA1 */
  #define LCD_RST_PIN           GPIO_PIN_2          /* 屏RESET -> PA2 */
#elif (PANEL_MAP == PANEL_MAP_STD)
  #define LCD_SCK_PIN           GPIO_PIN_0          /* 屏SCL   -> PA0 */
  #define LCD_SDA_PIN           GPIO_PIN_1          /* 屏SDA   -> PA1 */
  #define LCD_CS_PIN            GPIO_PIN_2          /* 屏CS    -> PA2 */
  #define LCD_RST_PIN           GPIO_PIN_3          /* 屏RESET -> PA3 */
#elif (PANEL_MAP == PANEL_MAP_CUSTOM)
  #define PANEL_CUSTOM_SCL      GPIO_PIN_0          /* 屏SCL  实测接到的PY32引脚 */
  #define PANEL_CUSTOM_SDA      GPIO_PIN_1          /* 屏SDA  实测接到的PY32引脚 */
  #define PANEL_CUSTOM_CS       GPIO_PIN_2          /* 屏CS   实测接到的PY32引脚 */
  #define PANEL_CUSTOM_RST      GPIO_PIN_3          /* 屏RESET实测接到的PY32引脚 */
  #define LCD_SCK_PIN           PANEL_CUSTOM_SCL
  #define LCD_SDA_PIN           PANEL_CUSTOM_SDA
  #define LCD_CS_PIN            PANEL_CUSTOM_CS
  #define LCD_RST_PIN           PANEL_CUSTOM_RST
#else
  #error "PANEL_MAP value is invalid (see Inc/main.h)"
#endif

#define LCD_READY_PIN           GPIO_PIN_6          /* H743的PG3输入 */

/* 日志串口(UART1 TX) */
#define LOG_UART                USART1
#define LOG_UART_PORT           GPIOA
#define LOG_TX_PIN              GPIO_PIN_9
#define LOG_TX_AF               GPIO_AF1_USART1

/* Private function prototypes -----------------------------------------------*/
void APP_ErrorHandler(void);
void app_log(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
