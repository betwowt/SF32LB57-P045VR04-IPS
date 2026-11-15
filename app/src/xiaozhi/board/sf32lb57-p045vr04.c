/*
 * Board glue for the xiaozhi application on SF32LB57-DevKit-LCD-V1
 * (P045VR04-IPS adapter).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include "bf0_hal.h"
#include "board.h"
#include "bsp_board.h"
#include "aw9523.h"
#include "board_hardware.h"
#include <string.h>

static uint8_t led_mask;
static uint8_t led_color = 4; /* Remember blue while powered off. */
static struct rt_mutex led_mutex;
static bool led_mutex_ready;
static const char *const led_colors[] = {
    "white", "red", "green", "yellow", "blue", "magenta", "cyan", "white"
};

int board_led_set(int r, int g, int b)
{
    int result;
    uint8_t mask = (!!r) | ((!!g) << 1) | ((!!b) << 2);
    aw9523_ensure_init();
    /* aw9523_init: LEDMODE1=0xff (GPIO), CONFIG1=0x00 (outputs).
     * P1_1..P1_3 are active low; never use any SoC pin or PWM here. */
    rt_enter_critical();
    if (!led_mutex_ready)
    {
        rt_mutex_init(&led_mutex, "xz_led", RT_IPC_FLAG_PRIO);
        led_mutex_ready = true;
    }
    rt_exit_critical();
    rt_mutex_take(&led_mutex, RT_WAITING_FOREVER);
    result = aw9523_set_pin(AW9523_PIN_LED_R, !r);
    result |= aw9523_set_pin(AW9523_PIN_LED_G, !g);
    result |= aw9523_set_pin(AW9523_PIN_LED_B, !b);
    if (!result)
    {
        led_mask = mask;
        if (mask) led_color = mask;
    }
    rt_mutex_release(&led_mutex);
    return result;
}

bool board_light_power(void) { return led_mask != 0; }
const char *board_light_color(void) { return led_colors[led_color]; }
int board_light_set_power(bool power)
{
    uint8_t mask = power ? led_color : 0;
    return board_led_set(mask & 1, mask & 2, mask & 4);
}
int board_light_set_color(const char *color)
{
    for (int mask = 1; color && mask < 8; mask++)
        if (!strcmp(color, led_colors[mask]))
            return board_led_set(mask & 1, mask & 2, mask & 4);
    rt_kprintf("[Light] invalid color\n");
    return -RT_EINVAL;
}

static int xz_led(int argc, char **argv)
{
    static const char *const modes[] = {"off", "r", "g", "rg", "b", "rb", "gb", "rgb"};
    int result = -RT_EINVAL;
    if (argc == 2)
    {
        if (!strcmp(argv[1], "on")) result = board_light_set_power(true);
        else for (int mask = 0; mask < 8; mask++)
            if (!strcmp(argv[1], modes[mask]))
                result = board_led_set(mask & 1, mask & 2, mask & 4);
    }
    rt_kprintf("xz_led off|on|r|g|b|rg|rb|gb|rgb: result=%d power=%d color=%s\n",
               result, board_light_power(), board_light_color());
    return result;
}
MSH_CMD_EXPORT(xz_led, control AW9523 RGB GPIOs);

/* LED hooks used by the MCP / UI code (upstream drives an SK6812 RGB LED). */
void open_led(void)
{
    board_led_set(0, 0, 1);
}

void close_led(void)
{
    board_led_set(0, 0, 0);
}

void set_mode_led(void)
{
}

void set_pinmux(void)
{
    /* Pin mux is fully installed by BSP_PIN_Init()/BSP_LCD_PowerUp(). */
}

void check_poweron_reason(void)
{
    /* DevKit is USB powered; nothing to validate at boot. */
}

void PowerDownCustom(void)
{
    rt_kprintf("PowerDownCustom\n");

    BSP_LCD_Backlight(0);
    BSP_SPK_PowerDown();
    board_led_set(0, 0, 0);

    /* Waking up is done with the on-board reset button / re-plugging USB. */
    rt_hw_interrupt_disable();
    HAL_PMU_EnterHibernate();
    while (1)
    {
    }
}
