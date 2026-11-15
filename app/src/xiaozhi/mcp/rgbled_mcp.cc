#include "rtthread.h"
#include "rgbled_mcp.h"
#include "stdio.h"
#include "string.h"
#include "../board/board_hardware.h" 
RGBLEDController& GetRGBLEDController() {
    static RGBLEDController instance;
    return instance;
}

bool RGBLEDTool::is_color_cycling_ = false;
void RGBLEDTool::ColorCycleThreadEntry(void* param) 
{
    open_led();    
}
bool RGBLEDTool::IsLightOn() {
    // 如果正在循环变色，则认为灯是开启状态
    return board_light_power();
}
void RGBLEDTool::RegisterRGBLEDTool(McpServer* server) {
    // 循环变色工具
        server->AddTool(
            "self.led.turn_on_the_light",
            "turn on the light.",
            PropertyList(),
            [](const PropertyList&) -> ReturnValue {
            if (board_light_power()) return true;
            is_color_cycling_ = true;
            // Board hook uses AW9523 only; no SoC pin mux changes.
            set_mode_led(); 
            open_led();
            return true;
    }
    );

    server->AddTool(
        "self.led.turn_off_the_light",
        "turn off the light.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            close_led();
            is_color_cycling_ = false;
            return true;
        }
    );

    server->AddTool(
        "self.led.get_light_status",
        "Get the current status of the LED (on or off).",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return RGBLEDTool::IsLightOn();
        }
    );
}
