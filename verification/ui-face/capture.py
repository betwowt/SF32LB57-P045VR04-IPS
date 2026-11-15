"""Capture an exclusive COM8 session, retaining the unmodified UART bytes."""
import argparse
import time
from pathlib import Path

import serial


def main():
    p = argparse.ArgumentParser()
    p.add_argument("output", type=Path)
    p.add_argument("--command", default="xz_shot")
    p.add_argument("--seconds", type=float, default=180)
    p.add_argument("--until-shot", action="store_true")
    p.add_argument("--delay", type=float, default=1)
    args = p.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with serial.Serial("COM8", 1000000, bytesize=8, parity="N", stopbits=1,
                       timeout=0.25, write_timeout=3, rtscts=False, dsrdtr=False) as port:
        with args.output.open("wb") as out:
            start = time.monotonic()
            sent = False
            tail = b""
            total = 0
            while time.monotonic() - start < args.seconds:
                if not sent and time.monotonic() - start >= args.delay:
                    port.write((args.command + "\r\n").encode("ascii"))
                    sent = True
                    print("sent:", args.command, flush=True)
                data = port.read(max(1, port.in_waiting))
                if data:
                    out.write(data)
                    out.flush()
                    total += len(data)
                    tail = (tail + data)[-256:]
                    if args.until_shot and b"SHOTEND" in tail:
                        print(f"SHOTEND: {total} bytes, {time.monotonic()-start:.1f}s", flush=True)
                        return
            print(f"capture ended: {total} bytes", flush=True)
            if args.until_shot:
                raise RuntimeError("SHOTEND not received before deadline")


if __name__ == "__main__":
    main()
