"""Capture COM8 exclusively; record raw bytes, timed TX progress and counters."""
import argparse
import json
import re
import time
from pathlib import Path

import serial

parser = argparse.ArgumentParser()
parser.add_argument("name")
parser.add_argument("--seconds", type=float, default=140)
parser.add_argument("--reset", action="store_true")
parser.add_argument("--wait-connected", action="store_true")
args = parser.parse_args()
base = Path(__file__).resolve().parent / args.name
start = time.monotonic()
launched = start
capture_started = not args.wait_connected
startup_wait = 0
data = bytearray()
pending = bytearray()
progress = []
next_report = 10
commands = [(25, b"free\r\n"), (45, b"list_thread\r\n"),
            (70, b"free\r\n"), (130, b"free\r\n")]
port = serial.Serial("COM8", baudrate=1000000, timeout=0.2)
with port, base.with_suffix(".log").open("wb") as log:
    if args.reset:
        port.rts = True
        time.sleep(0.1)
        port.rts = False
    while not capture_started or time.monotonic() - start < args.seconds:
        elapsed = time.monotonic() - start
        if not capture_started and time.monotonic() - launched > 180:
            raise RuntimeError("No WebSocket connection within 180 seconds")
        if capture_started and commands and elapsed >= commands[0][0]:
            port.write(commands.pop(0)[1])
        chunk = port.read(8192)
        if chunk:
            data.extend(chunk)
            if not capture_started and b"ws on_connect status=101" in data:
                startup_wait = time.monotonic() - launched
                start = time.monotonic()
                elapsed = 0
                capture_started = True
                print(f"connected after {startup_wait:.2f}s; starting {args.seconds}s capture", flush=True)
            log.write(chunk)
            log.flush()
            pending.extend(chunk)
            while b"\n" in pending:
                line, _, pending = pending.partition(b"\n")
                text = line.decode("utf-8", "replace").strip()
                match = re.search(r"ws tx sent (\d+) frames", text)
                if match:
                    progress.append({"seconds": round(elapsed, 3), "sent": int(match[1])})
                if any(key in text for key in ("ws tx sent", "ws on_connect", "listen start", "total memory:", "WS_TX_STALL", "ws lost", "assert", "fatal error")):
                    print(f"{elapsed:7.2f}s {text}", flush=True)
        if capture_started and elapsed >= next_report:
            print(f"capture {elapsed:.0f}s bytes={len(data)} successful_tx={progress[-1] if progress else None}", flush=True)
            next_report += 10
text = data.decode("utf-8", "replace")
keys = ("sdio buf alloc fail", "[rtos]malloc fail", "node->buf is null",
        "WS_TX_STALL", "ws lost", "ws on_connect", "listen start",
        "ws tx backpressure", "ws tx sent", "fatal error", "Assertion failed",
        "received CLOSE", "node is null")
summary = {"duration_seconds": round(time.monotonic() - start, 3),
           "startup_wait_seconds": round(startup_wait, 3),
           "bytes": len(data), "counts": {key: text.count(key) for key in keys},
           "successful_tx_progress": progress,
           "heap": re.findall(r"total memory:[^\n]+", text)}
base.with_suffix(".json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(json.dumps(summary, indent=2), flush=True)
