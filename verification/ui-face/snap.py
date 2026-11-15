"""Grab an LVGL snapshot (xz_snap) from COM8 and decode it to PNG.

Usage: python snap.py <outname> [--seconds 200]
Writes <outname>.png and <outname>.hex.log next to this script.
"""
import re
import struct
import sys
import time
import zlib
from pathlib import Path

import serial

name = sys.argv[1] if len(sys.argv) > 1 else "snap"
seconds = 200
if "--seconds" in sys.argv:
    seconds = int(sys.argv[sys.argv.index("--seconds") + 1])

here = Path(__file__).resolve().parent
port = serial.Serial("COM8", 1000000, timeout=0.5)
port.write(b"\r\n")
time.sleep(0.3)
port.reset_input_buffer()
port.write(b"xz_snap\r\n")

buf = bytearray()
start = time.monotonic()
while time.monotonic() - start < seconds:
    chunk = port.read(16384)
    if chunk:
        buf.extend(chunk)
        if b"SNAPEND" in buf:
            break
port.close()

text = buf.decode("utf-8", "replace")
(here / f"{name}.hex.log").write_text(text, encoding="utf-8")

m = re.search(r"SNAP (\d+) (\d+) (\d+) (\d+)", text)
if not m:
    raise SystemExit("no SNAP header in capture")
w, h, stride, size = (int(v) for v in m.groups())
data = bytearray(stride * h)
received = bytearray(stride * h)

got = 0
for line in text.splitlines():
    mm = re.match(r"\s*([0-9a-fA-F]{6}):([0-9a-fA-F]+)", line)
    if not mm:
        continue
    off = int(mm.group(1), 16)
    body = bytes.fromhex(mm.group(2))
    if off + len(body) <= len(data):
        data[off:off + len(body)] = body
        received[off:off + len(body)] = b"\x01" * len(body)

got = sum(received)

print(f"snapshot {w}x{h} stride={stride} bytes={got}/{len(data)}")
if got != len(data) or b"SNAPEND" not in buf or size != len(data):
    raise SystemExit("incomplete snapshot; refusing to write a misleading PNG")

# RGB565 little-endian -> RGB888
rows = []
for y in range(h):
    row = bytearray()
    base = y * stride
    for x in range(w):
        lo = data[base + x * 2]
        hi = data[base + x * 2 + 1]
        v = lo | (hi << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
    rows.append(bytes(row))


def png(path, width, height, rows_):
    raw = b"".join(b"\x00" + r for r in rows_)
    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(raw, 6))
    out += chunk(b"IEND", b"")
    Path(path).write_bytes(out)


png(here / f"{name}.png", w, h, rows)
print(f"wrote {here / (name + '.png')}")

# quick color probe: centre of the face (FACE_X=50, FACE_Y=130, FACE_SZ=380)
cx, cy = 50 + 190, 130 + 190
px = rows[cy][cx * 3:cx * 3 + 3]
print("face centre pixel:", "#%02X%02X%02X" % tuple(px))
