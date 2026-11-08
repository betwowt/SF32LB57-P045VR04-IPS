"""Update the auditable manifest/BOM after the T113 backlight migration."""
from pathlib import Path
import csv
import json

ROOT = Path(__file__).resolve().parents[1]
CHANGES = {
    "L1": {"value": "4.7uH", "footprint": "Adapter:L_T113_2.5x2.0",
           "notes": "T113_MIPI_Adapter placement/footprint; selected 4.7uH part, verify Isat, DCR and thermal."},
    "U2": {"value": "SY7200ABC"},
    "D1": {"value": "SS14"},
    "C9": {"value": "10uF / 50V"},
    "C10": {"value": "1uF / 50V", "footprint": "Adapter:C_0805"},
    "C11": {"value": "10uF / 50V", "footprint": "Adapter:C_0805",
            "notes": "T113 backlight output capacitor; 50V X7R, account for DC bias."},
    "R4": {"value": "5R1 / 1%", "footprint": "Adapter:R_0603",
           "notes": "Feedback value retained at 5.1 ohm / 1%; only the T113 0603 body and placement are adopted."},
    "R6": {"nets": {"1": "BL_GATE", "2": "GND"}},
}

manifest_path = ROOT / "docs/design_manifest.json"
data = json.loads(manifest_path.read_text(encoding="utf-8"))
for item in data:
    if item["ref"] in CHANGES:
        for key, value in CHANGES[item["ref"]].items():
            item[key] = value
manifest_path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

bom_path = ROOT / "docs/BOM.csv"
with bom_path.open(newline="", encoding="utf-8-sig") as f:
    rows = list(csv.reader(f))
header = rows[0]
indexes = {name: header.index(name) for name in header}
for row in rows[1:]:
    if not row or row[0] not in CHANGES:
        continue
    change = CHANGES[row[0]]
    for key in ("value", "footprint", "notes"):
        if key in change:
            col = indexes["Value"] if key == "value" else indexes["Footprint"] if key == "footprint" else indexes["Design_note"]
            row[col] = change[key]
with bom_path.open("w", newline="", encoding="utf-8-sig") as f:
    csv.writer(f).writerows(rows)
print("Updated docs/design_manifest.json and docs/BOM.csv")
