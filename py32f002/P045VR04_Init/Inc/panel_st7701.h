/**
  ******************************************************************************
  * @file    panel_st7701.h
  * @brief   P045VR04-IPS(480x854, ST7701S) 面板初始化接口
  ******************************************************************************
  */

#ifndef __PANEL_ST7701_H
#define __PANEL_ST7701_H

#ifdef __cplusplus
extern "C" {
#endif

#include "py32f0xx_hal.h"

/* 面板参数(H743 LTDC侧必须与这些参数保持一致) */
#define PANEL_ST7701_H_RES          480         /* 面板横向像素(行内像素数) */
#define PANEL_ST7701_V_RES          854         /* 面板纵向行数             */

/* RGB时序参数(与H743 LTDC/stm32h743工程 ltdc.c 的 P045VR04 时序块保持一致, 供参考) */
#define PANEL_HSYNC_PULSE_WIDTH     10
#define PANEL_HSYNC_BACK_PORCH      30
#define PANEL_HSYNC_FRONT_PORCH     30
#define PANEL_VSYNC_PULSE_WIDTH     4
#define PANEL_VSYNC_BACK_PORCH      15
#define PANEL_VSYNC_FRONT_PORCH     12

/* 初始化时序(ms) */
#define PANEL_POWER_ON_MS           50          /* 上电到复位前的等待时间   */
#define PANEL_RST_LOW_MS            10          /* 复位低电平时间           */
#define PANEL_RST_HIGH_MS           120         /* 复位释放后的等待时间     */

/* 3线SPI(9bit)SCL半周期, 单位:延迟循环次数(约1us/次), 调大可以降低SPI速度 */
#define PANEL_SPI_HALF_LOOPS        4

void panel_gpio_init(void);                     /* 面板相关IO初始化(READY=0)  */
void panel_st7701_reset(void);                  /* 面板硬件复位               */
void panel_st7701_init(void);                   /* 面板复位 + 发送初始化表    */

#ifdef __cplusplus
}
#endif

#endif /* __PANEL_ST7701_H */
