#!/usr/bin/env python3
"""Raise the AIC8800 SoftAP command timeout in the prebuilt driver library.

`fhost_ap_cfg()` in libaic8800_driver.a waits only 300 ms for each
`fhost_wpa_execute_cmd()` reply.  The `SET_NETWORK 0 ssid "..."` command is the
one that makes the firmware reset the RF link and recalibrate the channel
(`ap start:<freq>` in the FW log), which takes ~1-2 s, so hostapd's reply always
arrives after the host gave up (select() timeout, `wlan_start_ap res:-9`) and
the SoftAP is torn down before it can beacon.  The STA config path
(`fhost_sta_cfg`) uses 500 ms for the same call, which is why STA works.

This script rewrites the three `mov.w r3, #300` immediates of `fhost_ap_cfg` to
`mov.w r3, #8000` in place.  Both encodings are 4-byte Thumb-2 instructions, so
nothing else moves.  The scan is gated on the instruction that follows them
(`mov r0, r8; bl fhost_wpa_execute_cmd`), so it cannot hit an unrelated #300.

Usage:
    python patch_aic_ap_timeout.py [--revert] [--check]

The library path defaults to the SDK copy next to this repository and can be
overridden with the AIC_LIB environment variable.
"""

import os
import sys

ORIG = bytes.fromhex("4ff49673")   # mov.w r3, #300
PATCHED = bytes.fromhex("4ff4fa53")  # mov.w r3, #8000
CALL = bytes.fromhex("4046fff7")   # mov r0, r8; bl <fhost_wpa_execute_cmd>

DEFAULT_LIB = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "..", "SiFli-SDK", "customer", "peripherals", "wifi", "aic8800d40-n",
    "lib", "libaic8800_driver.a",
)


def main() -> int:
    lib = os.environ.get("AIC_LIB", DEFAULT_LIB)
    if not os.path.isfile(lib):
        print(f"error: library not found: {lib}", file=sys.stderr)
        return 2

    with open(lib, "rb") as f:
        data = bytearray(f.read())

    sites = []
    pos = 0
    while True:
        pos = data.find(ORIG, pos)
        if pos < 0:
            break
        if data[pos + 4:pos + 8] == CALL:
            sites.append(pos)
        pos += 1

    already = 0
    pos = 0
    while True:
        pos = data.find(PATCHED, pos)
        if pos < 0:
            break
        if data[pos + 4:pos + 8] == CALL:
            already += 1
        pos += 1

    print(f"sites with #300: {len(sites)}  already patched: {already}")

    if "--check" in sys.argv:
        return 0 if sites or already else 1

    if "--revert" in sys.argv:
        for pos in sites:
            data[pos:pos + 4] = ORIG
        for pos in range(len(data) - 8):
            if data[pos:pos + 4] == PATCHED and data[pos + 4:pos + 8] == CALL:
                data[pos:pos + 4] = ORIG
        with open(lib, "wb") as f:
            f.write(data)
        print("reverted")
        return 0

    if not sites:
        print("nothing to patch" if already else "error: no matching site", file=sys.stderr)
        return 0 if already else 1

    for pos in sites:
        data[pos:pos + 4] = PATCHED
    with open(lib, "wb") as f:
        f.write(data)
    print(f"patched {len(sites)} site(s) to 8000 ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
