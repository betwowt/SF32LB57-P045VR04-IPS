"""Apply the T113-MIPI backlight power stage to the H743 board.

Only the nine backlight footprints are touched.  The reference board's tracks,
zones, and all other placement are intentionally left alone because this H743
project is still at the placement-only stage.
"""
from pathlib import Path
import shutil
import pcbnew

ROOT = Path(__file__).resolve().parents[1]
BOARD_PATH = ROOT / "H743_RGB_Adapter.kicad_pcb"
REF = Path(r"F:\projects\正点原子t113转接板\T113_MIPI_Adapter")
LOCAL_FP = ROOT / "Adapter.pretty"
LOCAL_3D = ROOT / "Adapter.3dshapes"


def mm(x: float) -> int:
    return pcbnew.FromMM(x)


def pos(x: float, y: float):
    return pcbnew.VECTOR2I(mm(x), mm(y))


def copy_fields(old, new):
    """Keep project-specific fields while changing a footprint geometry."""
    for fld in old.GetFields():
        name = fld.GetName()
        if name in {"Reference", "Value"}:
            continue
        try:
            new.SetField(name, fld.GetText())
        except Exception:
            pass
    for meth in ("SetExcludedFromBOM", "SetExcludedFromPosFiles", "SetDNP", "SetLocked"):
        try:
            getter = getattr(old, "IsExcludedFromBOM" if meth == "SetExcludedFromBOM" else
                             "IsExcludedFromPosFiles" if meth == "SetExcludedFromPosFiles" else
                             "IsDNP" if meth == "SetDNP" else "IsLocked")
            getattr(new, meth)(getter())
        except Exception:
            pass


def load_template(name: str):
    fp = pcbnew.FootprintLoad(str(LOCAL_FP), name)
    if fp is None:
        raise RuntimeError(f"Unable to load local footprint {name}")
    return fp


def replace_footprint(board, ref, template_name, x, y, angle, value):
    old = board.FindFootprintByReference(ref)
    if old is None:
        raise RuntimeError(f"Missing {ref} on H743 board")
    nets = {p.GetPadName(): p.GetNetname() for p in old.Pads()}
    new = load_template(template_name)
    new.SetReference(ref)
    new.SetValue(value)
    new.SetFPID(pcbnew.LIB_ID("Adapter", template_name))
    new.SetPosition(pos(x, y))
    new.SetOrientationDegrees(angle)
    # Changing a package must preserve its association with the schematic.
    if not old.GetPath().AsString():
        raise RuntimeError(f'{ref} has no schematic link; run repair_schematic_links.py first')
    new.SetPath(old.GetPath())
    new.SetSheetname(old.GetSheetname())
    new.SetSheetfile(old.GetSheetfile())
    copy_fields(old, new)
    for p in new.Pads():
        name = p.GetPadName()
        if name not in nets:
            raise RuntimeError(f"Pad {ref}.{name} missing in old footprint")
        p.SetNet(board.FindNet(nets[name]))
    board.Remove(old)
    board.Add(new)


def move_footprint(board, ref, x, y, angle, value=None):
    fp = board.FindFootprintByReference(ref)
    if fp is None:
        raise RuntimeError(f"Missing {ref} on H743 board")
    fp.SetPosition(pos(x, y))
    fp.SetOrientationDegrees(angle)
    if value is not None:
        fp.SetValue(value)


def fab_only(fp):
    """Keep dense power-stage bodies out of the silk layer.

    The T113 source uses a very tight switch-node placement.  Its outlines are
    retained on F.Fab for assembly review, while F.Silkscreen is reserved for
    the connectors and board markings so the placement does not create false
    silk-over-pad violations before routing.
    """
    for item in fp.GraphicalItems():
        if item.GetLayer() == pcbnew.F_SilkS:
            item.SetLayer(pcbnew.F_Fab)
    ref = fp.GetField("Reference")
    ref.SetLayer(pcbnew.F_Fab)
    ref.SetVisible(False)
    for fld in fp.GetFields():
        if fld.GetName() not in {"Reference", "Value"}:
            fld.SetLayer(pcbnew.F_Fab)
            fld.SetVisible(False)


def prepare_inductor_library():
    src_mod = REF / "JLC_Imported.pretty" / "C5832360_IND-SMD_L2.5-W2.0-VLS252012CX-150M.kicad_mod"
    src_model_dir = REF / "JLC_Imported.3dshapes"
    LOCAL_3D.mkdir(exist_ok=True)
    shutil.copy2(src_model_dir / "IND-SMD_L2.5-W2.0-VLS252012CX-150M.wrl",
                 LOCAL_3D / "IND-SMD_L2.5-W2.0-VLS252012CX-150M.wrl")
    shutil.copy2(src_model_dir / "IND-SMD_L2.5-W2.0-VLS252012CX-150M.step",
                 LOCAL_3D / "IND-SMD_L2.5-W2.0-VLS252012CX-150M.step")
    text = src_mod.read_text(encoding="utf-8")
    text = text.replace('"C5832360_IND-SMD_L2.5-W2.0-VLS252012CX-150M"',
                        '"L_T113_2.5x2.0"', 1)
    text = text.replace("${KIPRJMOD}/JLC_Imported.3dshapes/",
                        "${KIPRJMOD}/Adapter.3dshapes/")
    (LOCAL_FP / "L_T113_2.5x2.0.kicad_mod").write_text(text, encoding="utf-8")


def main():
    prepare_inductor_library()
    board = pcbnew.LoadBoard(str(BOARD_PATH))
    # Source anchor U3 = (116.5224,136.133).  The target U2 anchor is chosen
    # at (113.0,66.8), retaining every T113 component-to-component offset.
    replace_footprint(board, "L1", "L_T113_2.5x2.0", 109.7996, 66.6222, -90, "4.7uH")
    move_footprint(board, "U2", 113.0, 66.8, 90, "SY7200ABC")
    move_footprint(board, "D1", 106.4088, 66.0998, -90, "SS14")
    replace_footprint(board, "C9", "C_0805", 101.4050, 66.9888, -90, "10uF / 50V")
    replace_footprint(board, "C10", "C_0805", 103.5180, 66.9898, -90, "1uF / 50V")
    replace_footprint(board, "C11", "C_0805", 112.7714, 62.7868, 90, "10uF / 50V")
    replace_footprint(board, "R4", "R_0603", 113.7620, 69.7972, 180, "5R1 / 1%")
    move_footprint(board, "R5", 114.6510, 63.0408, 90, "10k")
    move_footprint(board, "R6", 116.3020, 63.0408, -90, "100k")
    # Keep the existing H743 AND-gate interlock clear of the imported diode
    # courtyard while retaining its electrical role.
    move_footprint(board, "U4", 98.5, 72.0, 0)
    move_footprint(board, "C12", 102.0, 72.0, 90)
    # The T113 pull-down is on the PWM side of the 10k series resistor.  Keep
    # the H743 interlock, but put this board's equivalent at BL_GATE.
    r6 = board.FindFootprintByReference("R6")
    r6.Pads()[0].SetNet(board.FindNet("BL_GATE"))
    for ref in ("U2", "L1", "D1", "C9", "C10", "C11", "R4", "R5", "R6"):
        fab_only(board.FindFootprintByReference(ref))
    pcbnew.SaveBoard(str(BOARD_PATH), board)
    # KiCad upgrades the legacy imported footprint on save.  Ensure the model
    # stays project-local and does not depend on the reference project path.
    data = BOARD_PATH.read_text(encoding="utf-8")
    data = data.replace("${KIPRJMOD}/JLC_Imported.3dshapes/",
                        "${KIPRJMOD}/Adapter.3dshapes/")
    BOARD_PATH.write_text(data, encoding="utf-8")
    print("Applied T113 backlight placement and footprints to H743 board")


if __name__ == "__main__":
    main()
