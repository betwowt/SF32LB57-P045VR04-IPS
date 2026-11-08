/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * P045VR04-IPS 480x854 RGB565 panel (ST7701S + FT6236 touch).
 *
 * The panel SPI initialization is performed by the PY32F002 on the
 * H743_RGB_Adapter board.  This driver only programs the LCDC DPI timing
 * and expects the panel to be already initialized (READY high, or a fixed
 * delay when the READY net is not wired to the SoC).
 */

#include <rtthread.h>
#include <rtdevice.h>
#include "string.h"
#include "board.h"
#include "drv_io.h"
#include "drv_lcd.h"
#include "log.h"

#define LCD_ID              0x4585

static LCDC_InitTypeDef lcdc_int_cfg =
{
    .lcd_itf = AUTO_SELECTED_DPI_INTFACE,
    .freq = 28 * 1000 * 1000,
    .color_mode = LCDC_PIXEL_FORMAT_RGB565,

    .cfg = {
        .dpi = {
            .PCLK_polarity = 0,
            .DE_polarity   = 0,
            .VS_polarity   = 1,
            .HS_polarity   = 1,

            .VS_width      = 4,     /* VSW */
            .HS_width      = 10,    /* HSW */

            .VBP = 15,
            .VAH = LCD_VER_RES_MAX,
            .VFP = 12,

            .HBP = 30,
            .HAW = LCD_HOR_RES_MAX,
            .HFP = 30,

            .interrupt_line_num = 1,
        },
    },
};

static void wait_for_panel_ready(void)
{
#if defined(LCD_READY_PIN) && (LCD_READY_PIN >= 0)
    int pin = LCD_READY_PIN;

    rt_pin_mode(pin, PIN_MODE_INPUT);
    for (int i = 0; i < 100; i++)
    {
        if (rt_pin_read(pin))
        {
            return;
        }
        LCD_DRIVER_DELAY_MS(10);
    }
    LOG_W("LCD READY timeout, continue anyway");
#else
    /* PY32 initializes the panel in about 500 ms (reset + 43 commands). */
    LCD_DRIVER_DELAY_MS(1000);
#endif
}

static void LCD_Init(LCDC_HandleTypeDef *hlcdc)
{
    /*
     * Wait for the PY32 on the adapter first: it owns the panel's 3-wire SPI
     * initialization and needs ~500 ms.  Starting the DPI stream before the
     * panel is ready would push pixels into an unconfigured ST7701S.
     *
     * Do not call BSP_LCD_Reset() here.  On the H743_RGB_Adapter the FPC RESET
     * pin is the PY32 NRST net, so toggling it would reset the initializer MCU
     * instead of the panel.  BSP_LCD_PowerUp() has already released it.
     */
    wait_for_panel_ready();

    memcpy(&hlcdc->Init, &lcdc_int_cfg, sizeof(LCDC_InitTypeDef));
    HAL_LCDC_Init(hlcdc);

    HAL_LCDC_SetROIArea(hlcdc, 0, 0, LCD_HOR_RES_MAX - 1, LCD_VER_RES_MAX - 1);
}

static uint32_t LCD_ReadID(LCDC_HandleTypeDef *hlcdc)
{
    return LCD_ID;
}

static void LCD_DisplayOn(LCDC_HandleTypeDef *hlcdc)
{
}

static void LCD_DisplayOff(LCDC_HandleTypeDef *hlcdc)
{
}

static void LCD_SetRegion(LCDC_HandleTypeDef *hlcdc, uint16_t Xpos0, uint16_t Ypos0,
                          uint16_t Xpos1, uint16_t Ypos1)
{
    HAL_LCDC_SetROIArea(hlcdc, 0, 0, LCD_HOR_RES_MAX - 1, LCD_VER_RES_MAX - 1);
}

static void LCD_WriteMultiplePixels(LCDC_HandleTypeDef *hlcdc, const uint8_t *RGBCode,
                                    uint16_t Xpos0, uint16_t Ypos0,
                                    uint16_t Xpos1, uint16_t Ypos1)
{
    HAL_LCDC_LayerSetData(hlcdc, HAL_LCDC_LAYER_DEFAULT, (uint8_t *)RGBCode,
                          Xpos0, Ypos0, Xpos1, Ypos1);
    HAL_LCDC_SendLayerData_IT(hlcdc);
}

static void LCD_SetColorMode(LCDC_HandleTypeDef *hlcdc, uint16_t color_mode)
{
    /* The adapter wires only R3..R7/G2..G7/B3..B7 and the PY32 sets
       COLMOD to 0x50 (RGB565).  Other formats are not usable. */
    if (color_mode != RTGRAPHIC_PIXEL_FORMAT_RGB565)
    {
        return;
    }

    lcdc_int_cfg.color_mode = LCDC_PIXEL_FORMAT_RGB565;
    HAL_LCDC_SetOutFormat(hlcdc, LCDC_PIXEL_FORMAT_RGB565);
}

static void LCD_SetBrightness(LCDC_HandleTypeDef *hlcdc, uint8_t br)
{
    /* BL_PWM is PA11, which has no GPTIM function on SF32LB57, so the
     * backlight boost's EN pin is driven as a plain GPIO (same as the ATK
     * H743 reference, where PB5 is simply held high).  0 = off, else on. */
    (void)hlcdc;
    BSP_LCD_Backlight(br ? 1 : 0);
}

static const LCD_DrvOpsDef LCD_drv =
{
    .Init = LCD_Init,
    .ReadID = LCD_ReadID,
    .DisplayOn = LCD_DisplayOn,
    .DisplayOff = LCD_DisplayOff,
    .SetRegion = LCD_SetRegion,
    .WriteMultiplePixels = LCD_WriteMultiplePixels,
    .SetColorMode = LCD_SetColorMode,
    .SetBrightness = LCD_SetBrightness,
};

LCD_DRIVER_EXPORT2(p045vr04, LCD_ID, &lcdc_int_cfg, &LCD_drv, 1);
