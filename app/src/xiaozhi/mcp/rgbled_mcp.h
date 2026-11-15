#ifndef RGBLED_MCP_H
#define RGBLED_MCP_H

#include "mcp_server.h"
#include "rtthread.h"
#include "stdio.h"
#include "string.h"
#include "../board/board_hardware.h"

// 定义颜色常量
#define RGBLED_NAME    "rgbled"

struct rt_color
{
    const char *color_name;
    uint32_t color;
};

static struct rt_color rgb_color_arry[] =
{
    {"black", 0x000000},
    {"blue", 0x0000ff},
    {"green", 0x00ff00},
    {"cyan",  0x00ffff},
    {"red", 0xff0000},
    {"purple", 0xff00ff},
    {"yellow", 0xffff00},
    {"white", 0xffffff}
};

// RGB LED控制类
class RGBLEDController {
public:
    RGBLEDController() = default;

    void SetColor(uint32_t color) {
        board_led_set((color & 0xff0000) != 0, (color & 0x00ff00) != 0,
                      (color & 0x0000ff) != 0);
    }

};

class RGBLEDTool {
public:
    static void RegisterRGBLEDTool(McpServer* server);
    static bool is_color_cycling_;
    static void ColorCycleThreadEntry(void* param);
    static bool IsLightOn();  //灯光状态
};

RGBLEDController& GetRGBLEDController();

#endif // RGBLED_MCP_H
