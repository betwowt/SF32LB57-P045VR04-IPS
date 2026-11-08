"""KiCad netlist identities and minimal, layout-preserving PCB link repair.

PCB paths use exported sheetpath/tstamps + symbol tstamps. Unlike schematic
instance paths, they must NOT be prefixed with the root schematic UUID.
"""
import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path


def netlist_links(path):
    result = {}
    for comp in ET.parse(path).getroot().findall('components/comp'):
        ref = comp.attrib['ref']
        assert ref not in result, f'Duplicate schematic reference {ref}'
        props = {p.attrib['name']: p.attrib.get('value', '') for p in comp.findall('property')}
        stamps = comp.findtext('tstamps').split()
        base = comp.find('sheetpath').attrib['tstamps'].rstrip('/')
        result[ref] = {
            'paths': [base + '/' + stamp for stamp in stamps],
            'sheetname': props['Sheetname'],
            'sheetfile': props['Sheetfile'],
        }
    return result


def children(text):
    """Return spans of direct child expressions, respecting quoted strings."""
    depth, quoted, escaped, start = 0, False, False, None
    for i, ch in enumerate(text):
        if quoted:
            if escaped:
                escaped = False
            elif ch == '\\':
                escaped = True
            elif ch == '"':
                quoted = False
        elif ch == '"':
            quoted = True
        elif ch == '(':
            depth += 1
            if depth == 2:
                start = i
        elif ch == ')':
            if depth == 2:
                yield start, i + 1
            depth -= 1
    assert depth == 0 and not quoted, 'Unbalanced KiCad file'


def footprints(text):
    result = {}
    for start, end in children(text):
        block = text[start:end]
        if not block.startswith('(footprint '):
            continue
        ref = re.search(r'\(property "Reference" "([^"]+)"', block).group(1)
        assert ref not in result, f'Duplicate PCB reference {ref}'
        result[ref] = (start, end, block)
    return result


def get_link(block, key):
    for start, end in children(block):
        expr = block[start:end]
        if expr.startswith('(' + key + ' '):
            return json.loads(expr[len(key) + 2:-1])
    return ''


def set_link(block, key, value):
    expression = f'({key} {json.dumps(value, ensure_ascii=False)})'
    for start, end in children(block):
        if block[start:end].startswith('(' + key + ' '):
            return block[:start] + expression + block[end:]
    # Insert metadata immediately after the footprint's opening line.
    at = block.index('\n') + 1
    return block[:at] + '\t\t' + expression + '\n' + block[at:]


def without_links(block):
    spans = [(s, e) for s, e in children(block)
             if any(block[s:e].startswith('(' + k + ' ') for k in ('path', 'sheetname', 'sheetfile'))]
    for start, end in reversed(spans):
        # All link metadata is stored on its own line.
        line_start = block.rfind('\n', 0, start) + 1
        assert not block[line_start:start].strip()
        line_end = block.find('\n', end)
        assert line_end >= 0 and not block[end:line_end].strip()
        block = block[:line_start] + block[line_end + 1:]
    return block


def audit_links(board_path, netlist_path):
    text = Path(board_path).read_bytes().decode('utf8')
    links, fps = netlist_links(netlist_path), footprints(text)
    assert links.keys() == fps.keys(), 'Schematic and PCB reference sets differ'
    paths = [get_link(block, 'path') for _, _, block in fps.values()]
    errors = []
    for ref, link in links.items():
        actual = get_link(fps[ref][2], 'path')
        if actual not in link['paths']:
            errors.append({'reference': ref, 'actual': actual, 'expected': link['paths']})
    report = {
        'components': len(links), 'linked': len(links) - len(errors),
        'missing_paths': sum(not p for p in paths),
        'duplicate_paths': len([p for p in paths if p]) - len(set(p for p in paths if p)),
        'mismatches': errors,
        'unmatched_by_identifier': [x['reference'] for x in errors],
    }
    return report
