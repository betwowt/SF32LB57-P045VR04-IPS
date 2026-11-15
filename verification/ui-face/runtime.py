"""Reboot final firmware through msh; retain boot/network evidence on COM8."""
import argparse
import json
import re
import time
from pathlib import Path

import serial

HERE = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("name", default="runtime-final", nargs="?")
    p.add_argument("--seconds", type=float, default=150)
    args = p.parse_args()
    data = bytearray()
    pending = bytearray()
    start = time.monotonic()
    report = 15
    with serial.Serial("COM8", 1000000, timeout=0.2, write_timeout=3) as port:
        with (HERE/f"{args.name}.log").open("wb") as log:
            port.write(b"\r\nreboot\r\n")
            while time.monotonic() - start < args.seconds:
                chunk = port.read(8192)
                if chunk:
                    data.extend(chunk)
                    pending.extend(chunk)
                    log.write(chunk)
                    log.flush()
                    while b"\n" in pending:
                        line, _, pending = pending.partition(b"\n")
                        text = line.decode("utf-8", "replace").strip()
                        if any(k in text for k in ("ws on_connect", "listen start", "ws tx sent", "FACE_VERIFY", "Assertion", "fault", "face ui start")):
                            print(f"{time.monotonic()-start:.1f}s {text}", flush=True)
                if time.monotonic() - start >= report:
                    print(f"runtime {report}s bytes={len(data)}", flush=True)
                    report += 15
    text = data.decode("utf-8", "replace")
    counts = {k: text.count(k) for k in ("ws on_connect status=101", "listen start", "sdio buf alloc fail", "WS_TX_STALL", "[rtos]malloc fail", "Assertion failed")}
    faults = re.findall(r"(?im)^.*(?:\bfault\b|hardfault|busfault|memmanage|fatal error).*$", text)
    report = dict(duration_seconds=round(time.monotonic()-start, 2), counts=counts,
                  fault_lines=faults,
                  tx_frame_progress=[int(n) for n in re.findall(r"ws tx sent (\d+) frames", text)])
    report["pass"] = (counts["ws on_connect status=101"] > 0 and counts["listen start"] > 0 and
                       all(counts[k] == 0 for k in ("sdio buf alloc fail", "WS_TX_STALL", "[rtos]malloc fail", "Assertion failed")) and not faults)
    (HERE/f"{args.name}.json").write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report, indent=2), flush=True)
    assert report["pass"], "runtime acceptance failed; inspect raw log"


if __name__ == "__main__":
    main()
