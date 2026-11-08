"""Align the backlight sheet with the T113 reference power stage."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SCH = ROOT / "03_Power_Backlight.kicad_sch"


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


def symbol_block(text: str, ref: str):
    marker = f'(property "Reference" "{ref}"'
    at = text.find(marker)
    if at < 0:
        raise ValueError(f"missing symbol {ref}")
    start = text.rfind("(symbol ", 0, at)
    end = balanced_end(text, start)
    return start, end


def set_symbol_properties(text: str, ref: str, *, value=None, footprint=None):
    start, end = symbol_block(text, ref)
    block = text[start:end]
    if value is not None:
        block, count = re.subn(r'(\(property "Value" ")[^"]*(")',
                               lambda m: m.group(1) + value + m.group(2), block, count=1)
        if not count:
            raise ValueError(f"missing value property for {ref}")
    if footprint is not None:
        block, count = re.subn(r'(\(property "Footprint" ")[^"]*(")',
                               lambda m: m.group(1) + footprint + m.group(2), block, count=1)
        if not count:
            raise ValueError(f"missing footprint property for {ref}")
    return text[:start] + block + text[end:]


def main():
    text = SCH.read_text(encoding="utf-8")
    updates = {
        "L1": {"value": "4.7uH", "footprint": "Adapter:L_T113_2.5x2.0"},
        "U2": {"value": "SY7200ABC"},
        "D1": {"value": "SS14"},
        "C9": {"value": "10uF / 50V"},
        "C10": {"value": "1uF / 50V", "footprint": "Adapter:C_0805"},
        "C11": {"value": "10uF / 50V", "footprint": "Adapter:C_0805"},
        # Preserve the user's feedback value; the T113 board uses the smaller
        # 0603 body, but it remains 5.1 ohm at 1%.
        "R4": {"value": "5R1 / 1%", "footprint": "Adapter:R_0603"},
        "R5": {"value": "10k"},
        "R6": {"value": "100k"},
    }
    for ref, props in updates.items():
        text = set_symbol_properties(text, ref, **props)
    # Match the T113 placement's pull-down point: R6 is on the PWM/gate side
    # of the 10k series resistor.  The H743 U4 interlock still drives BL_GATE.
    old = '(global_label "BL_EN" (shape passive) (at 304.8 210.82 0)'
    new = old.replace('"BL_EN"', '"BL_GATE"')
    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise ValueError("R6 BL_EN label not found")
    SCH.write_text(text, encoding="utf-8")
    print("Updated 03_Power_Backlight.kicad_sch for T113 backlight stage")


if __name__ == "__main__":
    main()
