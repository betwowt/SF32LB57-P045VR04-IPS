/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp_board.h"
#include "aw9523.h"

#define EPD_GMODE_PIN           (54)         // GPIO_A54

#define EPD_VDD33_EN            (52)         // GPIO_A52
#define CTP_VDD33_EN            (53)         // GPIO_A53
#define DPI_VSYNC_PIN   (51)          // GPIO_A51
#define LCD_BL_PIN      (11)          // GPIO_A11 -> FPC1 pin 34 BL_PWM


void BSP_LCD_Reset(uint8_t high1_low0)
{
    aw9523_ensure_init();
    aw9523_set_pin(AW9523_PIN_LCD_RST, high1_low0);
}


void BSP_LCD_PowerDown(void)
{
    BSP_LCD_Reset(0);
}
static void QSPI_PIN_Set(int pad, pin_function func, int flags, int hcpu)
{
    HAL_PIN_Set(pad, func, flags, hcpu);
#ifdef BSP_LCDC_USING_DDR_QADSPI
    // Set the pin driver strength to 12mA
    HAL_PIN_SetDS(pad, PIN_DS_12MA);
#else
    // Set the pin driver strength to 8mA
    HAL_PIN_SetDS(pad, PIN_DS_8MA);
#endif /* BSP_LCDC_USING_DDR_QADSPI */
}

void BSP_LCD_PowerUp(void)
{
#if defined(BSP_USING_DCMI)
    /*
     * The panel and the DVP camera share PA32/PA33/PA43/PA44/PA45 on this
     * devkit, so only one of them can own those pads. Power the camera down
     * before the LCD takes them over.
     */
    BSP_CAMERA_PowerDown();
#endif /* BSP_USING_DCMI */

#ifdef BSP_LCDC_USING_DPI
    /*
     * RGB565 on a 40-pin RGB565 carrier (H743_RGB_Adapter):
     *
     * The adapter only carries the HIGH bits of each channel -- R3..R7,
     * G2..G7, B3..B7 -- and ties R0..R2 / G0..G1 / B0..B2 to GND.  In
     * 16-bit DPI mode the LCDC drives its colour outputs LSB-aligned
     * (R0..R4 / G0..G5 / B0..B4), so the function of each *pad* has to be
     * shifted: the pad that the FPC routes to the panel's R3 carries
     * LCDC1_DPI_R0, and so on.
     *
     * Pads are fixed by the SF32LB57-DevKit-LCD FPC (see the schematic,
     * page "LCD"): R3..R7 = PA27..PA31, G2..G7 = PA37..PA42,
     * B3..B7 = PA52/PA53/PA54/PA46/PA47.
     */

    /* R: panel R3..R7 <- LCDC R0..R4  (the 3 MSBs come from these 5 bits) */
    HAL_PIN_Set(PAD_PA27, LCDC1_DPI_R0, PIN_NOPULL, 1);   /* panel R3 */
    HAL_PIN_Set(PAD_PA28, LCDC1_DPI_R1, PIN_NOPULL, 1);   /* panel R4 */
    HAL_PIN_Set(PAD_PA29, LCDC1_DPI_R2, PIN_NOPULL, 1);   /* panel R5 */
    HAL_PIN_Set(PAD_PA30, LCDC1_DPI_R3, PIN_NOPULL, 1);   /* panel R6 */
    HAL_PIN_Set(PAD_PA31, LCDC1_DPI_R4, PIN_NOPULL, 1);   /* panel R7 */

    /* G: panel G2..G7 <- LCDC G0..G5 */
    HAL_PIN_Set(PAD_PA37, LCDC1_DPI_G0, PIN_NOPULL, 1);   /* panel G2 */
    HAL_PIN_Set(PAD_PA38, LCDC1_DPI_G1, PIN_NOPULL, 1);   /* panel G3 */
    HAL_PIN_Set(PAD_PA39, LCDC1_DPI_G2, PIN_NOPULL, 1);   /* panel G4 */
    HAL_PIN_Set(PAD_PA40, LCDC1_DPI_G3, PIN_NOPULL, 1);   /* panel G5 */
    HAL_PIN_Set(PAD_PA41, LCDC1_DPI_G4, PIN_NOPULL, 1);   /* panel G6 */
    HAL_PIN_Set(PAD_PA42, LCDC1_DPI_G5, PIN_NOPULL, 1);   /* panel G7 */

    /* B: panel B3..B7 <- LCDC B0..B4 */
    HAL_PIN_Set(PAD_PA52, LCDC1_DPI_B0, PIN_NOPULL, 1);   /* panel B3 */
    HAL_PIN_Set(PAD_PA53, LCDC1_DPI_B1, PIN_NOPULL, 1);   /* panel B4 */
    HAL_PIN_Set(PAD_PA54, LCDC1_DPI_B2, PIN_NOPULL, 1);   /* panel B5 */
    HAL_PIN_Set(PAD_PA46, LCDC1_DPI_B3, PIN_NOPULL, 1);   /* panel B6 */
    HAL_PIN_Set(PAD_PA47, LCDC1_DPI_B4, PIN_NOPULL, 1);   /* panel B7 */
    /* PA24..PA26 / PA32..PA33 / PA43..PA45 stay GPIO inputs: they are the
     * FPC's low bits, grounded on the adapter, and no DPI bit is routed there. */

    /* Timing control */
    HAL_PIN_Set(PAD_PA48, LCDC1_DPI_CLK,   PIN_NOPULL, 1);
    // Set the clk pin driver strength to 8mA
    HAL_PIN_SetDS(PAD_PA48, PIN_DS_8MA);
    HAL_PIN_Set(PAD_PA49, LCDC1_DPI_HSYNC, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA51, LCDC1_DPI_VSYNC, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA50, LCDC1_DPI_DE,    PIN_NOPULL, 1);

    /* Backlight: FPC1 pin 34 (BL_PWM) is PA11.  On SF32LB57 PA11 only offers
     * GPIO_A11 / MPI3_CS2 / SD1_DIO1 / AUD_CLK_EXT -- there is no GPTIM
     * function on this pad, so a PWM mux silently fails (HAL_PIN_Set returns
     * -1) and the backlight stays at whatever level the boost's EN pin floats
     * to.  Drive it as a plain GPIO, exactly like the ATK H743 reference
     * (PB5 high).  Kept low here: LCD_SetBrightness() turns it on only after
     * the PY32 has initialized the panel and the first frame is out. */
    BSP_LCD_Backlight(0);

#elif defined(BSP_LCDC_USING_DBI)
    HAL_PIN_Set(PAD_PA24, LCDC1_8080_TE,    PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA25, LCDC1_8080_DIO15, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA26, LCDC1_8080_CS,    PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA27, LCDC1_8080_DIO14, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA28, LCDC1_8080_DC,    PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA29, LCDC1_8080_DIO13, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA30, LCDC1_8080_WR,    PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA31, LCDC1_8080_DIO12, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA32, LCDC1_8080_RD,    PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA33, LCDC1_8080_DIO11, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA37, LCDC1_8080_DIO10, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA39, LCDC1_8080_DIO9, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA41, LCDC1_8080_DIO8, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA43, LCDC1_8080_DIO7, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA44, LCDC1_8080_DIO0, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA45, LCDC1_8080_DIO6, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA46, LCDC1_8080_DIO1, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA47, LCDC1_8080_DIO5, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA48, LCDC1_8080_DIO2, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA49, LCDC1_8080_DIO4, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA50, LCDC1_8080_DIO3, PIN_NOPULL, 1);

    HAL_PIN_Set(PAD_PA20, GPTIM2_CH2, PIN_NOPULL, 1);   // LCDC1_BL_PWM_CTRL, LCD backlight PWM (pwmt2 CH2)

    //Unused pins
    HAL_PIN_Set(PAD_PA38, GPIO_A38, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA40, GPIO_A40, PIN_NOPULL, 1);

#elif defined(BSP_LCDC_USING_DIRECT_EPD)
    BSP_GPIO_Set(EPD_VDD33_EN, 1, 1);
    BSP_GPIO_Set(CTP_VDD33_EN, 1, 1);

    HAL_PIN_Set(PAD_PA25, LCDC1_TCON_GDSP,   PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA27, LCDC1_TCON_GDCLK,  PIN_NOPULL, 1);
    // Set the pin driver strength to 2mA?
    HAL_PIN_SetDS(PAD_PA27, PIN_DS_2MA);
    HAL_PIN_Set(PAD_PA28, LCDC1_TCON_SDCLK,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA29, LCDC1_TCON_SDAT15, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA30, LCDC1_TCON_SDLE,   PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA31, LCDC1_TCON_SDAT14, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA32, LCDC1_TCON_SDOE,   PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA33, LCDC1_TCON_SDAT13, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA37, LCDC1_TCON_SDAT12, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA38, LCDC1_TCON_SDSTL,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA39, LCDC1_TCON_SDAT11, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA40, LCDC1_TCON_SDAT0,  PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA41, LCDC1_TCON_SDAT10, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA42, LCDC1_TCON_SDAT1, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA43, LCDC1_TCON_SDAT9, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA44, LCDC1_TCON_SDAT2, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA45, LCDC1_TCON_SDAT8, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA46, LCDC1_TCON_SDAT3, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA47, LCDC1_TCON_SDAT7, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA48, LCDC1_TCON_SDAT4, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA49, LCDC1_TCON_SDAT6, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA50, LCDC1_TCON_SDAT5, PIN_NOPULL, 1);

    HAL_PIN_Set(PAD_PA00 + EPD_GMODE_PIN, GPIO_A0 + EPD_GMODE_PIN, PIN_PULLUP, 1);  //GMODE

    HAL_PIN_Set(PAD_PA11, GPTIM2_CH2, PIN_NOPULL, 1);   // LCDC1_BL_PWM_CTRL, LCD backlight PWM (pwmt2 CH2)

    //TPS chip control pins
    HAL_PIN_Set(PAD_PA00 + EPD_TPS_WAKEUP_PIN, GPIO_A0 + EPD_TPS_WAKEUP_PIN, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA00 + EPD_TPS_PWRCOM_PIN, GPIO_A0 + EPD_TPS_PWRCOM_PIN, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA00 + EPD_TPS_PWRUP_PIN, GPIO_A0 + EPD_TPS_PWRUP_PIN, PIN_NOPULL, 1);

    HAL_PIN_Set(PAD_PA22, I2C1_SDA, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA23, I2C1_SCL, PIN_PULLUP, 1);
#ifdef EPD_TPS_GOOD_PIN
    HAL_PIN_Set(PAD_PA00 + EPD_TPS_GOOD_PIN, GPIO_A0 + EPD_TPS_GOOD_PIN, PIN_NOPULL, 1);
#endif

#elif defined(BSP_LCDC_USING_QADSPI)
    /*
     * QSPI routing of the sf32lb57-lcd devkit with the 57 QSPI ribbon.
     *
     * DIO0..DIO2 use the pads shared with the DPI connector, while DIO3, CLK,
     * CS and TE are routed on the pads of the dedicated 57 QSPI ribbon:
     *   DIO0 PA39   DIO1 PA38   DIO2 PA37   DIO3 PA30
     *   CLK  PA31 (12mA)       CS   PA29   TE   PA28
     * The panel reset is not a SoC pad, it is driven by the AW9523 (P1_7).
     */
    QSPI_PIN_Set(PAD_PA39, LCDC1_MATRIX_SPI_DIO0, PIN_NOPULL, 1);
    QSPI_PIN_Set(PAD_PA38, LCDC1_MATRIX_SPI_DIO1, PIN_NOPULL, 1);
    QSPI_PIN_Set(PAD_PA37, LCDC1_MATRIX_SPI_DIO2, PIN_NOPULL, 1);
    QSPI_PIN_Set(PAD_PA30, LCDC1_MATRIX_SPI_DIO3, PIN_NOPULL, 1);
    QSPI_PIN_Set(PAD_PA31, LCDC1_MATRIX_SPI_CLK,  PIN_NOPULL, 1);
    HAL_PIN_SetDS(PAD_PA31, PIN_DS_12MA);
    HAL_PIN_Set(PAD_PA29, LCDC1_MATRIX_SPI_CS,    PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA28, LCDC1_MATRIX_SPI_TE,    PIN_NOPULL, 1);

    HAL_PIN_Set(PAD_PA45, GPTIM2_CH2, PIN_NOPULL, 1);   // LCDC1_BL_PWM_CTRL, LCD backlight PWM (pwmt2 CH2)

#endif /* BSP_LCDC_USING_QADSPI */

    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_LCDC, RCC_CLK_LCDC_SYSCLK);

    /* Release the panel reset. On this board it is driven by the AW9523
       (P1_7) and drv_lcd never calls BSP_LCD_Reset() on its own, so without
       this the panel stays in reset and never answers the ID read. */
    BSP_LCD_Reset(1);
}

void BSP_LCD_Backlight(uint8_t on)
{
    BSP_GPIO_Set(LCD_BL_PIN, on ? 1 : 0, 1);
}

void BSP_LCD_GMODE_Set(uint8_t high1_low0)
{
    BSP_GPIO_Set(EPD_GMODE_PIN, high1_low0, 1);
}


void BSP_TP_PowerUp(void)
{
    aw9523_ensure_init();
    aw9523_set_pin(AW9523_PIN_CTP_RST, 1);

    //TP irq pin
    HAL_PIN_Set(PAD_PA00 + TOUCH_IRQ_PIN, GPIO_A0 + TOUCH_IRQ_PIN, PIN_PULLUP, 1);
}

void BSP_TP_PowerDown(void)
{
    aw9523_ensure_init();
    aw9523_set_pin(AW9523_PIN_CTP_RST, 0);
    //TP irq pin
    HAL_PIN_Set(PAD_PA00 + TOUCH_IRQ_PIN, GPIO_A0 + TOUCH_IRQ_PIN, PIN_NOPULL, 1);
}

void BSP_TP_Reset(uint8_t high1_low0)
{
    aw9523_ensure_init();
    aw9523_set_pin(AW9523_PIN_CTP_RST, high1_low0);
}

uint32_t BSP_GET_DPI_AUX_VSYNC_PIN(void)
{
    return DPI_VSYNC_PIN;
}