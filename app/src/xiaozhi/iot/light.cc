#include "thing.h"
#include "../board/board_hardware.h"

namespace iot {
class Light : public Thing {
public:
    Light() : Thing("Light", "板载RGB灯，仅支持开关和七种颜色，无亮度调节") {
        properties_.AddBooleanProperty("power", "灯是否开启", []() -> bool {
            return board_light_power();
        });
        properties_.AddStringProperty("color", "red|green|blue|yellow|cyan|magenta|white", []() -> std::string {
            return board_light_color();
        });
        methods_.AddMethod("SetPower", "开关灯，开启时恢复上次颜色", ParameterList({
            Parameter("power", "true开启，false关闭", kValueTypeBoolean, true)
        }), [](const ParameterList& p) { board_light_set_power(p["power"].boolean()); });
        methods_.AddMethod("SetColor", "设置颜色并开灯", ParameterList({
            Parameter("color", "red|green|blue|yellow|cyan|magenta|white", kValueTypeString, true)
        }), [](const ParameterList& p) { board_light_set_color(p["color"].string().c_str()); });
    }
};
}
DECLARE_THING(Light);
