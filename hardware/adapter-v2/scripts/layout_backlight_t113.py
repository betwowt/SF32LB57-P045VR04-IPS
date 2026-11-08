"""Lay out the H743 backlight sheet as one wired T113-style circuit block.

This edits only the schematic presentation and wires.  Component UUIDs,
references, footprints, and instance paths are retained so the PCB association
is not disturbed.
"""
from __future__ import annotations

import re
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCH = ROOT / "03_Power_Backlight.kicad_sch"


def uid(label: str) -> str:
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "h743-rgb-backlight-layout/" + label))


def balanced_end(text: str, start: int) -> int:
    depth = 0
    quoted = False
    escaped = False
    for i in range(start, len(text)):
        ch = text[i]
        if quoted:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                quoted = False
        else:
            if ch == '"':
                quoted = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    return i + 1
    raise ValueError("unbalanced schematic expression")


def split_root(text: str) -> tuple[str, list[str]]:
    lib_start = text.index("\n\t(lib_symbols")
    lib_end = balanced_end(text, lib_start)
    prefix = text[:lib_end]
    rest = text[lib_end:]
    items: list[str] = []
    i = 0
    while i < len(rest):
        if rest[i] != "(":
            i += 1
            continue
        end = balanced_end(rest, i)
        items.append(rest[i:end])
        i = end
    return prefix, items


def fmt(value: float) -> str:
    return f"{value:.4f}".rstrip("0").rstrip(".")


def xy_at(block: str) -> tuple[float, float]:
    match = re.search(r"\(at\s+([^\s]+)\s+([^\s]+)\s+[^\s\)]+\)", block)
    if not match:
        raise ValueError("missing at coordinate")
    return float(match.group(1)), float(match.group(2))


def ref_of(block: str) -> str | None:
    match = re.search(r'\(property "Reference" "([^"]+)"', block)
    return match.group(1) if match else None


def shift_symbol(block: str, dx: float, dy: float) -> str:
    def replace(match: re.Match[str]) -> str:
        x, y, angle = float(match.group(1)), float(match.group(2)), match.group(3)
        return f"(at {fmt(x + dx)} {fmt(y + dy)} {angle})"

    return re.sub(r"\(at\s+([^\s]+)\s+([^\s]+)\s+([^\s\)]+)\)", replace, block)


def wire(name: str, *points: tuple[float, float]) -> str:
    pts = " ".join(f"(xy {fmt(x)} {fmt(y)})" for x, y in points)
    return (
        f'\t(wire\n\t\t(pts {pts})\n\t\t(stroke (width 0) (type default))\n'
        f'\t\t(uuid "{uid("wire/" + name)}")\n\t)'
    )


def junction(name: str, x: float, y: float) -> str:
    return (
        f'\t(junction\n\t\t(at {fmt(x)} {fmt(y)})\n'
        '\t\t(diameter 0)\n\t\t(color 0 0 0 0)\n'
        f'\t\t(uuid "{uid("junction/" + name)}")\n\t)'
    )


def label(name: str, net: str, x: float, y: float, angle: int = 0) -> str:
    return (
        f'\t(global_label "{net}"\n\t\t(shape passive)\n'
        f'\t\t(at {fmt(x)} {fmt(y)} {angle})\n'
        '\t\t(effects\n\t\t\t(font (size 1.016 1.016))\n\t\t\t(justify left)\n\t\t)\n'
        f'\t\t(uuid "{uid("label/" + name)}")\n'
        '\t\t(property "Intersheetrefs" "${INTERSHEET_REFS}"\n'
        f'\t\t\t(at {fmt(x)} {fmt(y)} 0)\n\t\t\t(hide yes)\n'
        '\t\t\t(show_name no)\n\t\t\t(do_not_autoplace no)\n'
        '\t\t\t(effects (font (size 1.27 1.27)))\n\t\t)\n\t)'
    )


def text_item(content: str, x: float, y: float, size: float = 1.524) -> str:
    # Text is deliberately kept simple and matches the format emitted by the
    # project generator.  The UUID is deterministic so repeated runs are safe.
    escaped = content.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return (
        f'\t(text "{escaped}"\n\t\t(exclude_from_sim no)\n'
        f'\t\t(at {fmt(x)} {fmt(y)} 0)\n\t\t(effects\n'
        f'\t\t\t(font (size {fmt(size)} {fmt(size)}))\n\t\t\t(justify left top)\n'
        f'\t\t)\n\t\t(uuid "{uid("text/" + content)}")\n\t)'
    )


def move_text(block: str, x: float, y: float, content: str | None = None) -> str:
    if content is not None:
        block = re.sub(r'^\(text\s+"(?:\\.|[^"\\])*"',
                       lambda _: "(text " + '"' + content.replace('"', '\\"').replace("\n", "\\n") + '"',
                       block, count=1, flags=re.S)
    return re.sub(r"\(at\s+[^\s]+\s+[^\s]+\s+[^\s\)]+\)",
                  f"(at {fmt(x)} {fmt(y)} 0)", block, count=1)


def main() -> None:
    original = SCH.read_text(encoding="utf-8")
    prefix, items = split_root(original)

    symbols: dict[str, str] = {}
    texts: list[str] = []
    sheet_instances: str | None = None
    for item in items:
        stripped = item.lstrip()
        if stripped.startswith("(symbol"):
            ref = ref_of(item)
            if ref:
                symbols[ref] = item
        elif stripped.startswith("(text "):
            texts.append(item)
        elif stripped.startswith("(sheet_instances"):
            sheet_instances = item

    # These are the only symbols moved.  LDO power stays in its existing
    # upper-left block; the complete backlight stage occupies the lower half.
    targets = {
        "L1": (170.18, 152.4),
        "D1": (248.92, 152.4),
        "U2": (223.52, 190.5),
        "C9": (152.4, 190.5),
        "C10": (172.72, 220.98),
        "C11": (294.64, 177.8),
        "R4": (294.64, 218.44),
        "U4": (345.44, 127.0),
        "R5": (355.6, 152.4),
        "R6": (355.6, 185.42),
        "R7": (314.96, 220.98),
        "C12": (386.08, 152.4),
    }
    for ref, (x, y) in targets.items():
        if ref not in symbols:
            raise ValueError(f"missing component {ref}")
        ox, oy = xy_at(symbols[ref])
        symbols[ref] = shift_symbol(symbols[ref], x - ox, y - oy)

    # Re-title and relocate the notes so the page reads like the T113 sheet.
    moved_texts: list[str] = []
    for item in texts:
        if '"+5V INPUT / BOOST POWER"' in item:
            # Re-added below at a single stable location on every run.
            continue
        if '"BACKLIGHT BOOST / I_LED = 0.2V / R4"' in item:
            moved_texts.append(move_text(item, 145.0, 112.0,
                                         "06  LED BACKLIGHT - SY7200ABC / H743 INTERLOCK"))
        elif '"BACKLIGHT INTERLOCK"' in item:
            moved_texts.append(move_text(item, 305.0, 112.0, "BACKLIGHT ENABLE / READY GATE"))
        elif '"LEDK is the sense node' in item:
            moved_texts.append(move_text(item, 145.0, 242.0))
        elif '"PY_READY=0 during reset/init' in item:
            moved_texts.append(move_text(item, 305.0, 92.0))
        else:
            moved_texts.append(item)

    # Remove the old pin-to-label stubs and redraw the complete power stage.
    new_items: list[str] = moved_texts
    for ref, block in symbols.items():
        new_items.append(block)

    # U1 NC pin (pin 4) is retained explicitly.  Its symbol was not moved.
    new_items.append('\t(no_connect\n\t\t(at 106.68 53.34)\n\t\t(uuid "' + uid("nc/U1/4") + '")\n\t)')

    # LDO / local power block: explicit short rails keep the page readable.
    new_items.extend([
        wire("ldo/in", (76.2, 48.26), (76.2, 50.8)),
        wire("ldo/inlabel", (76.2, 48.26), (66.04, 48.26)),
        label("ldo/+5V", "+5V", 66.04, 48.26),
        wire("ldo/out", (106.68, 48.26), (116.84, 48.26)),
        label("ldo/+3V3", "+3V3", 116.84, 48.26),
        wire("ldo/gnd", (106.68, 50.8), (116.84, 50.8)),
        label("ldo/gnd", "GND", 116.84, 50.8),
    ])
    for ref, x, net in [("C5", 33.02, "+5V"), ("C6", 53.34, "+5V")]:
        new_items.extend([
            wire(f"{ref}/top", (x, 78.74), (x, 73.66)),
            label(f"{ref}/top", net, x, 73.66),
            wire(f"{ref}/bottom", (x, 88.9), (x, 93.98)),
            label(f"{ref}/bottom", "GND", x, 93.98),
        ])
    for ref, x in [("C7", 147.32), ("C8", 177.8)]:
        new_items.extend([
            wire(f"{ref}/top", (x, 78.74), (x, 73.66)),
            label(f"{ref}/top", "+3V3", x, 73.66),
            wire(f"{ref}/bottom", (x, 88.9), (x, 93.98)),
            label(f"{ref}/bottom", "GND", x, 93.98),
        ])

    # T113-style boost current path.  All electrical endpoints lie on the
    # 2.54 mm connection grid so KiCad treats every branch as connected.
    new_items.extend([
        text_item("+5V INPUT / BOOST POWER", 144.78, 121.92, 1.27),
        junction("boost/j1", 147.32, 152.4),
        junction("boost/j2", 147.32, 185.42),
        junction("boost/j3", 147.32, 187.96),
        junction("boost/j4", 147.32, 215.9),
        junction("boost/j5", 147.32, 231.14),
        junction("boost/j6", 243.84, 152.4),
        junction("boost/j18", 177.8, 152.4),
        junction("boost/j8", 274.32, 152.4),
        junction("boost/j9", 274.32, 190.5),
        junction("boost/j10", 294.64, 152.4),
        junction("boost/j11", 246.38, 193.04),
        junction("boost/j12", 246.38, 218.44),
        junction("boost/j13", 152.4, 231.14),
        junction("boost/j14", 172.72, 231.14),
        junction("boost/j15", 208.28, 231.14),
        junction("boost/j16", 294.64, 231.14),
        junction("boost/j17", 299.72, 231.14),
        junction("gate/j1", 350.52, 124.46),
        junction("gate/j2", 350.52, 152.4),
        junction("gate/j3", 350.52, 185.42),
        junction("gate/j4", 330.2, 231.14),
        junction("gate/j5", 360.68, 231.14),
        junction("gate/j6", 320.04, 231.14),
        junction("gate/j7", 386.08, 231.14),
        # +5V rail: L1 input, U2 IN, C9 and C10.
        wire("boost/+5-rail", (147.32, 147.32), (147.32, 215.9)),
        wire("boost/l1-in", (147.32, 152.4), (162.56, 152.4)),
        wire("boost/u2-in", (147.32, 187.96), (208.28, 187.96)),
        wire("boost/c9-in", (147.32, 185.42), (152.4, 185.42)),
        wire("boost/c10-in", (147.32, 215.9), (172.72, 215.9)),
        label("boost/+5", "+5V", 147.32, 147.32),
        # Switching node L1 -> D1 anode -> U2 LX.
        wire("boost/lx-top", (177.8, 152.4), (243.84, 152.4)),
        label("boost/lx-l1", "BL_LX", 177.8, 152.4),
        label("boost/lx-d1", "BL_LX", 243.84, 152.4),
        label("boost/u2-lx", "BL_LX", 238.76, 187.96),
        # LEDA / OVP node and output capacitor.
        wire("boost/leda-top", (254.0, 152.4), (274.32, 152.4)),
        wire("boost/leda-u2", (274.32, 152.4), (274.32, 190.5)),
        wire("boost/leda-u2-pin", (274.32, 190.5), (238.76, 190.5)),
        wire("boost/leda-c11", (274.32, 152.4), (294.64, 152.4)),
        wire("boost/leda-c11-down", (294.64, 152.4), (294.64, 172.72)),
        wire("boost/leda-out", (294.64, 152.4), (312.42, 152.4)),
        label("boost/LEDA", "LEDA", 312.42, 152.4),
        # LEDK sense path and retained 5R1 feedback resistor.
        wire("boost/ledk", (238.76, 193.04), (246.38, 193.04)),
        wire("boost/ledk-down", (246.38, 193.04), (246.38, 218.44)),
        wire("boost/ledk-r4", (246.38, 218.44), (289.56, 218.44)),
        label("boost/LEDK", "LEDK", 246.38, 193.04),
        # Common ground rail for the boost stage.
        wire("boost/gnd-rail", (147.32, 231.14), (386.08, 231.14)),
        wire("boost/c9-gnd", (152.4, 195.58), (152.4, 231.14)),
        wire("boost/c10-gnd", (172.72, 226.06), (172.72, 231.14)),
        wire("boost/u2-gnd", (208.28, 193.04), (208.28, 231.14)),
        wire("boost/c11-gnd", (294.64, 182.88), (294.64, 231.14)),
        wire("boost/r4-gnd", (299.72, 218.44), (299.72, 231.14)),
        label("boost/GND", "GND", 147.32, 231.14),
        # Gate/enable interlock at the right, with BL_GATE bus and BL_EN
        # returned to the driver by global net labels.
        wire("gate/bus", (350.52, 124.46), (350.52, 185.42)),
        wire("gate/u4-out", (360.68, 124.46), (350.52, 124.46)),
        label("gate/BL_GATE", "BL_GATE", 350.52, 139.7),
        wire("gate/blen", (360.68, 152.4), (375.92, 152.4)),
        label("gate/BL_EN", "BL_EN", 375.92, 152.4),
        wire("gate/u2-blen", (208.28, 190.5), (198.12, 190.5)),
        label("gate/u2-blen", "BL_EN", 198.12, 190.5),
        wire("gate/pwm", (330.2, 124.46), (312.42, 124.46)),
        label("gate/H_BL_PWM", "H_BL_PWM", 312.42, 124.46),
        wire("gate/ready", (330.2, 127.0), (312.42, 127.0)),
        label("gate/LCD_READY", "LCD_READY", 312.42, 127.0),
        wire("gate/u4-vcc", (360.68, 127.0), (375.92, 127.0)),
        label("gate/u4-vcc", "+3V3", 375.92, 127.0),
        wire("gate/u4-gnd", (330.2, 129.54), (330.2, 231.14)),
        wire("gate/r6-gnd", (360.68, 185.42), (360.68, 231.14)),
        # Route the PWM sense branch above R4's grounded end so it never
        # crosses the vertical GND return.
        wire("gate/r7-pwm-up", (309.88, 220.98), (309.88, 203.2)),
        wire("gate/r7-pwm-left", (309.88, 203.2), (284.48, 203.2)),
        label("gate/r7-pwm", "H_BL_PWM", 284.48, 203.2),
        wire("gate/r7-gnd", (320.04, 220.98), (320.04, 231.14)),
        # Local +3V3 decoupling for the logic gate.
        wire("gate/3v3", (386.08, 147.32), (386.08, 137.16)),
        label("gate/3v3", "+3V3", 386.08, 137.16),
        wire("gate/c12-gnd", (386.08, 157.48), (386.08, 231.14)),
        # PWR_FLAG pins are placed directly on their named global nets.
        label("flag/+5", "+5V", 223.52, 43.18),
        label("flag/GND", "GND", 223.52, 66.04),
    ])

    if sheet_instances is not None:
        new_items.append(sheet_instances)
    SCH.write_text(prefix + "\n" + "\n".join(new_items) + "\n)\n", encoding="utf-8")
    print(f"Re-laid out {len(targets)} backlight components as one wired T113-style block")


if __name__ == "__main__":
    main()
