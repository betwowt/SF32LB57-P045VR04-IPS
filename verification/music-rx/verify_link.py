"""Verify actual ARM objects route MP3 and allocation calls as intended."""
from pathlib import Path
import hashlib
import json
import os
import subprocess

root = Path(__file__).resolve().parents[2]
build = root / "app/project/build_sf32lb57-p045vr04_hcpu"
sdk = root.parent / "SiFli-SDK"
tool_root = Path(os.environ["USERPROFILE"]) / ".sifli/tools/arm-none-eabi-gcc"
nm = next(tool_root.glob("*/bin/arm-none-eabi-nm.exe"))

cases = {
    "sifli_sdk/middleware/audio/audio_local_music/audio_mp3ctrl.o": (
        {"xz_music_source_open", "xz_music_source_write", "xz_music_alloc", "xz_music_calloc", "xz_music_free"},
        {"audio_open2", "audio_write", "rt_malloc", "rt_calloc", "rt_free"}),
    "src/xiaozhi/xiaozhi_music_source.o": (
        {"audio_open2", "audio_write", "xz_audio_music_parameters"},
        {"xz_music_source_open", "xz_music_source_write"}),
    "sifli_sdk/middleware/audio/audio_manager/audio_server.o": (
        {"rt_malloc", "xz_music_calloc", "xz_music_free_any", "resample_malloc", "resample_free", "calloc", "free"},
        {"xz_music_alloc", "xz_music_free", "rt_calloc", "rt_free"}),
    "sifli_sdk/middleware/audio/audio_local_music/sifli_resample.o": (
        {"xz_music_alloc", "xz_music_free"}, {"rt_malloc", "rt_free"}),
    "sifli_sdk/external/libhelix-mp3/real/buffers.o": (
        {"xz_music_alloc", "xz_music_free"}, {"rt_malloc", "rt_free"}),
}

checks = {}
for relative, (required, forbidden) in cases.items():
    obj = build / relative
    if not obj.exists():
        matches = list(build.rglob(Path(relative).name))
        assert len(matches) == 1, (relative, "ambiguous/missing object", matches)
        obj = matches[0]
    output = subprocess.check_output([str(nm), "-u", str(obj)], text=True)
    symbols = {line.split()[-1] for line in output.splitlines() if line.strip()}
    assert required <= symbols, (relative, "missing", required - symbols)
    assert not forbidden & symbols, (relative, "forbidden", forbidden & symbols)
    actual = obj.relative_to(build).as_posix()
    checks[actual] = sorted(symbols)
    print(f"PASS: {actual}")

audio_sources = [
    "middleware/audio/audio_manager/audio_server.c",
    "middleware/audio/audio_local_music/audio_mp3ctrl.c",
    "middleware/audio/audio_local_music/sifli_resample.c",
    "external/libhelix-mp3/real/buffers.c",
]
subprocess.run(["git", "-C", str(sdk), "diff", "--exit-code", "--", *audio_sources], check=True)
print("PASS: SDK audio sources and asserts have no changes")

def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

artifacts = [build / "main.elf", *sorted((build / "main.bin").glob("*.bin"))]
report = {
    "objects": checks,
    "sdk_audio_sha256": {str(p): sha256(sdk / p) for p in audio_sources},
    "artifacts": {str(p.relative_to(root)): {"bytes": p.stat().st_size, "sha256": sha256(p)} for p in artifacts},
}
(Path(__file__).parent / "link-evidence.json").write_text(
    json.dumps(report, indent=2) + "\n", encoding="utf-8")
print("PASS: ELF and firmware hashes written to link-evidence.json")
