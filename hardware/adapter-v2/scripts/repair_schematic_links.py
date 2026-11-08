"""Repair PCB associations from a freshly exported KiCad XML netlist.

Run with KiCad's bundled Python. Changes only path/sheet metadata; saves a
timestamped backup and verifies all other PCB bytes are preserved.
"""
from pathlib import Path
from datetime import datetime
import json
import shutil
import subprocess
import sys
from schematic_links import audit_links, footprints, netlist_links, set_link, without_links

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / 'H743_RGB_Adapter.kicad_pcb'
SCHEMATIC = ROOT / 'H743_RGB_Adapter.kicad_sch'
NETLIST = ROOT / 'output/H743_RGB_Adapter.xml'
CLI = Path(r'C:\Program Files\KiCad\10.0\bin\kicad-cli.exe')


def main():
    schematics = {p: p.read_bytes() for p in ROOT.glob('*.kicad_sch')}
    before_bytes = BOARD.read_bytes()
    subprocess.run([str(CLI), 'sch', 'export', 'netlist', str(SCHEMATIC),
                    '--format', 'kicadxml', '-o', str(NETLIST)], check=True)
    before = audit_links(BOARD, NETLIST)
    if '--check' in sys.argv:
        print(json.dumps(before, indent=2))
        assert not before['mismatches'] and not before['duplicate_paths'], 'Broken schematic links'
        return
    text = before_bytes.decode('utf8')
    fps = footprints(text)
    links = netlist_links(NETLIST)
    edits, changed = [], []
    for ref, (start, end, block) in fps.items():
        new_block = block
        for key, value in [('path', links[ref]['paths'][0]),
                           ('sheetname', links[ref]['sheetname']),
                           ('sheetfile', links[ref]['sheetfile'])]:
            new_block = set_link(new_block, key, value)
        assert without_links(block) == without_links(new_block), f'Non-link change in {ref}'
        if new_block != block:
            edits.append((start, end, new_block))
            changed.append(ref)
    for start, end, block in sorted(edits, reverse=True):
        text = text[:start] + block + text[end:]
    assert BOARD.read_bytes() == before_bytes, 'Board changed during repair; run again on latest save'
    assert all(p.read_bytes() == content for p, content in schematics.items()), 'Schematic changed during repair'
    backup = ROOT / 'backups' / ('schematic_links_' + datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
    backup.mkdir(parents=True)
    for p in [BOARD, *schematics, ROOT / 'H743_RGB_Adapter.kicad_pro']:
        shutil.copy2(p, backup / p.name)
    if edits:
        BOARD.write_bytes(text.encode('utf8'))
    after = audit_links(BOARD, NETLIST)
    assert not after['mismatches'] and not after['duplicate_paths']
    # Independently query the native PCB API with the exact exported identity.
    import pcbnew as pcb
    board = pcb.LoadBoard(str(BOARD))
    for ref, link in links.items():
        fp = board.FindFootprintByPath(pcb.KIID_PATH(link['paths'][0]))
        assert fp and fp.GetReference() == ref, f'Native identity lookup failed: {ref}'
    for ref, (_, _, old_block) in fps.items():
        assert without_links(old_block) == without_links(footprints(text)[ref][2])
    report = {'before': before, 'after': after, 'metadata_updated': sorted(changed),
              'pcb_changes_limited_to_link_metadata': True,
              'native_pcb_identity_lookups_passed': len(links),
              'tracks_and_vias': len(board.GetTracks()), 'zones': len(board.Zones()),
              'backup': str(backup),
              'method': 'Netlist identifier matching and native FindFootprintByPath; not a GUI F8 run'}
    (ROOT / 'docs/schematic_link_repair.json').write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + '\n', encoding='utf8')
    print(f'Repaired {len(changed)} metadata records; {after["linked"]}/{after["components"]} native identity matches. Layout unchanged.')
    print(f'Backup: {backup}')


if __name__ == '__main__':
    main()
