"""Compile actual callback/DSP functions against deterministic PCM/RTOS mocks.

Only host GCC is executed. No serial, network, flashing, or board access.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
AUDIO = (ROOT / "app/src/xiaozhi/xiaozhi_audio.c").read_text(encoding="utf-8")


def function(source, name):
    match = re.search(r"^(?:static\s+)?(?:void|bool|int)\s+" + name +
                      r"\([^;{}]*\)\s*\{", source, re.M)
    assert match, name
    depth = 1
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
                             r'//[^\n]*|/\*.*?\*/|[{}]', source[match.end():], re.S):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[match.start():match.end() + token.end()]
    raise AssertionError(name)


names = ["xz_audio_reset_barge_in", "xz_audio_barge_in_armed", "xz_mic_downmix", "xz_mic_log", "xz_mic_barge_in", "xz_mic_mono_frame", "mic_callback"]
functions = "\n".join(function(AUDIO, n) for n in names)
reference = (ROOT / "app/src/xiaozhi/xiaozhi_barge_ref.c").read_text(encoding="utf-8")
assert not re.search(r"\b(?:malloc|calloc|realloc|rt_malloc|audio_mem_malloc)\s*\(", functions + reference)
# Target pointers are 32 bit. Widen only the host mock transport argument.
functions = functions.replace("uint32_t reserved)", "uintptr_t reserved)")
for n in ["xz_aec_mic_open", "xz_audio_decoder_encoder_open"]:
    f = function(AUDIO, n)
    assert "pa.read_channnel_num = 2;" in f
    assert "pa.read_which_mic = AUDIO_MIC_ALL;" in f
    assert "pa.is_need_3a = 1;" in f
    assert "pa.disable_uplink_agc = 1;" in f

kws = (ROOT / "app/src/xiaozhi/kws/app_recorder_process.c").read_text(encoding="utf-8")
# All five capture opens (including standalone KWS) must set the saved
# startup gain before audio_open and reapply to live registers afterwards.
for source in [AUDIO, kws]:
    opens = list(re.finditer(r"(?:thiz->mic|thiz->client) = audio_open\([^;]+;", source))
    assert len(opens) == (3 if source == AUDIO else 2)
    for opened in opens:
        assert source[:opened.start()].rstrip().endswith("xz_audio_apply_pdm_gain();")
        assert re.match(r"\s*RT_ASSERT\(thiz->(?:mic|client)\);\s*xz_audio_apply_pdm_gain\(\);",
                        source[opened.end():])

gcc = shutil.which("gcc")
assert gcc, "Host GCC required"
with tempfile.TemporaryDirectory(prefix="xz-dual-mic-") as folder:
    folder = Path(folder)
    kconfig = (ROOT / "app/project/Kconfig.proj").read_text(encoding="utf-8")
    config = []
    for name in ["XZ_BARGE_IN_" + symbol for symbol in
                 ["RMS", "FRAMES", "TRAIN_MS", "ECHO_MULT", "PRE_RATIO"]] + [
                 "XZ_BARGE_ARM_RATIO", "XZ_BARGE_ARM_MS", "XZ_BARGE_ARM_MIN_PRE",
                 "XZ_BARGE_DTD_MARGIN_PM"]:
        block = re.search(r"^config " + name + r"\n(.*?)(?=^config |\Z)", kconfig, re.M | re.S)[1]
        value = re.search(r"^    default (\d+)$", block, re.M)[1]
        config.append(f"#define {name} {value}")
    (folder / "config-under-test.h").write_text("\n".join(config), encoding="utf-8")
    (folder / "audio-under-test.inc").write_text(functions, encoding="utf-8")
    (folder / "rtthread.h").write_text(
        "#include <stdint.h>\ntypedef uint32_t rt_tick_t;\n"
        "#define RT_TICK_PER_SECOND 1000\n"
        "void rt_enter_critical(void);\nvoid rt_exit_critical(void);\n"
        "rt_tick_t rt_tick_get(void);\nrt_tick_t rt_tick_from_millisecond(unsigned ms);\n",
        encoding="utf-8")
    (folder / "gain-under-test.inc").write_text(
        function(AUDIO, "xz_audio_apply_pdm_gain") + "\n" + function(kws, "kws_push_pcm"),
        encoding="utf-8")
    for pdm in [0, 1]:
        exe = folder / f"gain-{pdm}.exe"
        subprocess.run([gcc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", f"-DTEST_PDM={pdm}",
                        "-I", str(folder), "-I", str(ROOT / "app/src/xiaozhi"),
                        str(HERE / "gain_host.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    sdk = (ROOT.parent / "SiFli-SDK/middleware/audio/audio_manager/audio_server.c").read_text(encoding="utf-8")
    start = sdk.index("#if defined(XZ_PDM1_STEREO) && defined(AUDIO_RX_USING_PDM)")
    end = sdk.index("\n    if (my->is_need_3a)", start)
    block = sdk[start:end].replace("(uint32_t)&data", "(uintptr_t)&data")
    (folder / "sdk-stereo-under-test.inc").write_text(block, encoding="utf-8")
    for log, mix in [(1, 0), (0, 0), (1, 1)]:
        exe = folder / f"audio-{log}-{mix}.exe"
        subprocess.run([gcc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", "-Wno-unused-function",
                        f"-DCONFIG_XZ_MIC_LEVEL_LOG={log}", f"-DXZ_MIC_MIX_MAX_ABS={mix}",
                        "-I", str(folder), "-I", str(ROOT / "app/src/xiaozhi"),
                        str(HERE / "audio_host.c"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    (folder / "playback-under-test.inc").write_text("\n".join(
        function(AUDIO, name) for name in
        ["audio_write_and_wait", "xz_speaker_close", "xz_audio_tts_drained"]), encoding="utf-8")
    exe = folder / "playback.exe"
    subprocess.run([gcc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-I", str(folder),
                    str(HERE / "playback_host.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: both AEC open sites stereo/ALL/16k/AGC-off; callback path has no heap allocation")
