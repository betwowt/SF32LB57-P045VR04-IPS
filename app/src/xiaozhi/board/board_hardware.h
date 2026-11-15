#ifndef BOARD_HARDWARE_H
#define BOARD_HARDWARE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif
//MCP --led
int board_led_set(int r, int g, int b);
int board_light_set_power(bool power);
int board_light_set_color(const char *color);
bool board_light_power(void);
const char *board_light_color(void);
void open_led();
void close_led();
void set_mode_led();
//pinmux
void set_pinmux();
//关机
void PowerDownCustom(void);
//开机检查
void check_poweron_reason(void);
#ifdef __cplusplus
}
#endif

#endif // BOARD_HARDWARE_H
