"""Host regression tests against extracted production C functions, no hardware I/O."""
from pathlib import Path
import hashlib
import json
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
WS = (ROOT / "app/src/xiaozhi/xiaozhi_websocket.c").read_text(encoding="utf-8")
AUDIO = (ROOT / "app/src/xiaozhi/xiaozhi_audio.c").read_text(encoding="utf-8")
SDK = ROOT.parent / "SiFli-SDK"


def function(source, name):
    match = re.search(r"^(?:static[ \t]+)?(?:const[ \t]+)?"
                      r"(?:void|bool|int|err_t|uint32_t|char)[ \t*]+" +
                      re.escape(name) + r"\([^;{}]*?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError(f"Missing production function: {name}")
    # Tokenize comments/strings so braces inside JSON literals do not count.
    depth = 1
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
                         r'//[^\n]*|/\*.*?\*/|[{}]', source[match.end():], re.S)
    for token in tokens:
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():match.end() + token.end()]
    raise AssertionError(f"Unbalanced function: {name}")


names = ["xz_ws_uplink_allowed", "xz_ws_audio_epoch", "xz_ws_playback_allowed",
         "xz_ws_write_frame", "xz_ws_write_locked", "ws_set_state",
         "ws_enter_standby", "ws_dismiss_intent", "ws_end_speaking",
         "ws_json_string", "ws_send_speak_abort", "ws_send_listen_start",
         "ws_send_listen_stop", "ws_send_hello", "xz_ws_wake_beep_done",
         "xz_ws_process_wake", "xz_audio_send_using_websocket", "xz_ws_tx_entry",
         "my_wsapp_fn", "xz_button_event_handler", "parse_helLo"]
functions = [function(WS, name) for name in names]
functions += [function((ROOT / "app/src/main.c").read_text(encoding="utf-8"), "xz_uplink")]
functions += [function(AUDIO, name) for name in
              ["xz_beep", "xz_audio_reset_uplink", "xz_audio_request_wake_beep",
               "xz_audio_wake_beep"]]
prototypes = [f[:f.index("{")].rstrip() + ";" for f in functions]

# Guard the constraints that cannot be exercised by the mocked peripherals.
ui = ROOT / "app/src/xiaozhi/xiaozhi_ui.c"
before = json.loads((HERE / "ui-before.json").read_text(encoding="utf-8-sig"))
assert hashlib.sha256(ui.read_bytes()).hexdigest().upper() == before["Hash"]
old_audio = (HERE / "before/xiaozhi_audio.c").read_text(encoding="utf-8")
assert re.findall(r"^.*(?:mic_gain|XZ_MIC_GAIN_TRIM_DB).*$", AUDIO, re.M) == \
       re.findall(r"^.*(?:mic_gain|XZ_MIC_GAIN_TRIM_DB).*$", old_audio, re.M)
hello_branch = WS.split('if (strcmp(type, "hello") == 0)', 1)[1].split(
    'else if (strcmp(type, "goodbye") == 0)', 1)[0]
assert "ws_send_listen_start" not in hello_branch
mic = function(AUDIO, "mic_callback")
assert mic.index("!xz_ws_uplink_allowed()") < mic.index("VOICE_STATE_IDLE ==")
assert "xz_wake_beep_busy" in mic and "kws_feed_mic" in mic
assert "g_kws_force_exit" not in function(WS, "xz_button_event_handler")
assert 'static button_action_t last_action' not in WS
assert "while (!ws_reconnect_allowed)" in function(WS, "xiaozhi_ws_connect")
assert 'rt_thread_init(&ws_connect_thread' in function(WS, "xiaozhi2")
assert 'xz_ws_process_wake();' in (ROOT / "app/src/main.c").read_text(encoding="utf-8")
assert not re.search(r"\b(?:malloc|rt_malloc|rt_thread_create)\s*\(", "\n".join(functions))

with tempfile.TemporaryDirectory(prefix="xz-standby-") as tmp:
    tmp = Path(tmp)
    (tmp / "production-under-test.inc").write_text(
        "\n".join(prototypes + functions), encoding="utf-8")
    (tmp / "rtthread.h").write_text(
        "#include <stdlib.h>\n#include <string.h>\n"
        "#define rt_malloc malloc\n#define rt_free free\n"
        "#define rt_realloc realloc\n#define rt_memcpy memcpy\n"
        "#define rt_memset memset\n", encoding="utf-8")
    gcc = shutil.which("gcc")
    if not gcc:
        raise SystemExit("Host GCC required for C regression tests")
    exe = tmp / "state-test.exe"
    subprocess.run([gcc, "-std=c11", "-O0", "-g", "-Wall", "-Wextra",
                    "-Wno-unused-parameter", "-Wno-pointer-to-int-cast",
                    "-Wno-unused-variable", "-Wno-pointer-sign",
                    "-I", str(tmp), "-I", str(SDK / "external/cJSON-1.7.17"),
                    str(HERE / "host_test.c"),
                    str(SDK / "external/cJSON-1.7.17/cJSON.c"), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: UI unchanged, mic gain unchanged, no auto-listen, no per-frame allocation")
