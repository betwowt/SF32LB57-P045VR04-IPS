"""Capture/decode complete RGB565 SHOT or SNAP dumps; reject missing/bad bytes."""
import argparse
import functools
import operator
import re
import time
from pathlib import Path

import numpy as np
from PIL import Image
import serial

HERE = Path(__file__).resolve().parent


def decode(raw, kind, output):
    tag = "SHOT" if kind == "shot" else "SNAP"
    text = raw.decode("utf-8", "replace")
    header = re.search(rf"{tag} (\d+) (\d+) (\d+) (\S+)", text)
    if not header or f"{tag}END" not in text:
        raise ValueError(f"incomplete {tag} dump")
    w, h, third = map(int, header.groups()[:3])
    if kind == "shot":
        assert third == 16, f"unsupported bpp: {third}"
        stride = w * 2
    else:
        stride = third
        assert int(header[4]) == stride * h
    data, seen = bytearray(stride*h), bytearray(stride*h)
    for line in text[header.end():].splitlines():
        match = re.match(r"\s*([0-9a-fA-F]{6}):([0-9a-fA-F]+)(?: ([0-9a-fA-F]{2}))?\s*$", line)
        if not match:
            continue
        offset = int(match[1], 16)
        body = bytes.fromhex(match[2])
        if kind == "shot":
            assert match[3] is not None, f"missing checksum at {offset:x}"
            assert functools.reduce(operator.xor, body, 0) == int(match[3], 16), f"checksum at {offset:x}"
        assert offset + len(body) <= len(data)
        assert not any(seen[offset:offset+len(body)]), f"duplicate at {offset:x}"
        data[offset:offset+len(body)] = body
        seen[offset:offset+len(body)] = b"\1" * len(body)
    assert all(seen), f"missing {len(seen)-sum(seen)} bytes"
    values = np.frombuffer(data, dtype="<u2").reshape(h, stride//2)[:, :w]
    r, g, b = values >> 11, (values >> 5) & 63, values & 31
    rgb = np.stack(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)), axis=-1).astype("uint8")
    Image.fromarray(rgb).save(output)
    print(f"{tag} {w}x{h}: {sum(seen)}/{len(data)} bytes verified; wrote {output}", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("name")
    p.add_argument("--kind", choices=("shot", "snap"), default="shot")
    p.add_argument("--state", help="optional fixed xz_face state before capture")
    p.add_argument("--decode", type=Path, help="decode existing log without opening COM8")
    p.add_argument("--seconds", type=float, default=180)
    args = p.parse_args()
    output = HERE / args.name
    if args.decode:
        raw = args.decode.read_bytes()
    else:
        raw = bytearray()
        end = b"SHOTEND" if args.kind == "shot" else b"SNAPEND"
        with serial.Serial("COM8", 1000000, timeout=0.25, write_timeout=3) as port:
            port.write(b"\r\n")
            time.sleep(4)  # Also allow UI startup immediately after flash readback.
            if args.state:
                port.write(f"xz_face {args.state}\r\n".encode("ascii"))
                prep = bytearray()
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    prep.extend(port.read(4096))
                    if b"FACE_VERIFY state=" in prep:
                        break
                output.with_suffix(".prepare.log").write_bytes(prep)
                assert b"FACE_VERIFY state=" in prep, "xz_face did not acknowledge; see .prepare.log"
                time.sleep(1)
            port.reset_input_buffer()
            port.write(f"xz_{args.kind}\r\n".encode("ascii"))
            start, report = time.monotonic(), 0
            with output.with_suffix(".hex.log").open("wb") as log:
                while time.monotonic() - start < args.seconds:
                    chunk = port.read(16384)
                    raw.extend(chunk)
                    log.write(chunk)
                    log.flush()
                    elapsed = time.monotonic() - start
                    if elapsed >= report:
                        print(f"{args.kind}: {elapsed:.0f}s {len(raw)} UART bytes", flush=True)
                        report += 15
                    if end in raw:
                        break
    decode(raw, args.kind, output.with_suffix(".png"))


if __name__ == "__main__":
    main()
