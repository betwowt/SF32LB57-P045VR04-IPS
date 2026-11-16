/* Local SD-card player exposed as an IoT thing.
 *
 * Without this descriptor the server only knows its own music/search plugin,
 * so "播放音乐" made the cloud LLM search a remote library while the local
 * keyword matcher also started the SD track. Advertising Play/Stop here lets
 * the LLM drive the device's own player instead of the cloud catalogue. */
#include "thing.h"

extern "C" {
#include "../xiaozhi_music.h"
}

namespace iot {
class Music : public Thing {
public:
    Music() : Thing("Music",
                    "本地SD卡音乐播放器：只能播放设备SD卡里已有的歌曲，没有网络曲库") {
        properties_.AddBooleanProperty("playing", "是否正在输出音乐（TTS 暂停期间为 false）",
                                       []() -> bool {
            return xz_music_is_playing();
        });
        properties_.AddStringProperty("track", "当前曲目文件名，未播放时为空",
                                      []() -> std::string {
            const char *name = xz_music_current_track();
            return name ? std::string(name) : std::string();
        });
        methods_.AddMethod("Play", "从SD卡第一首开始顺序播放，播完自动下一首",
                           ParameterList(),
                           [](const ParameterList&) { xz_music_play(NULL); });
        methods_.AddMethod("PlayNamed", "播放文件名包含 keyword 的歌曲",
                           ParameterList({
                               Parameter("keyword", "文件名关键字，例如歌名", kValueTypeString, true)
                           }),
                           [](const ParameterList& parameters) {
                               xz_music_play(parameters["keyword"].string().c_str());
                           });
        methods_.AddMethod("Stop", "停止播放并释放播放器", ParameterList(),
                           [](const ParameterList&) { xz_music_stop(); });
    }
};
}
DECLARE_THING(Music);
