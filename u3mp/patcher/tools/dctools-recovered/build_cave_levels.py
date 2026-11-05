#!/usr/bin/env python3
"""build_cave_levels.py <installed pak23_ps4.txt> <ps3 1.19 pak23.txt> <PS4 modules.bin> <out> <log.json>
Adds the Cave level definitions the map entry needs (cave-multi-stage is already installed).
Rule (earlier agent, 601/605 levels): PS4 block = PS3 1.19 block + the nine texturedict1
package-ts lines, CRLF. The installed file has no trailing space after level-name, so none is added.
Lines dropped because the target does not exist on PS4 (same treatment as the installed
san-multi-comp-pak / multi-all blocks): `symbol` lines whose sid is not exported by PS4
modules.bin, and the two DLC sound banks (no PS4 level references them)."""
import re, sys, json, struct
sys.path.insert(0, '.')
from common import DC, sid
TS = ['texturedict1/chunk-41-tstrm', 'texturedict1/chunk-41-tstrm-1', 'texturedict1/chunk-42-tstrm',
      'texturedict1/chunk-43-tstrm', 'texturedict1/common-tstrm', 'texturedict1/common-tstrm-1',
      'texturedict1/common-tstrm-2', 'texturedict1/common-tstrm-3', 'texturedict1/common-tstrm-4']
LEVELS = ['cave-multi-comp-pak', 'cave-multi-killvis', 'cave-multi', 'cave-multi-waterfalls', 'sky-cave-multi-2']
DROP_BANKS = {'cave-multi-dlc', 'ice-cave-multi'}

def split(t):
    out = []
    for b in re.split(r'\n(?=level-name )', t.replace('\r', '').rstrip('\n')):
        out.append((re.search(r'^level-id (\S+)', b, re.M).group(1), b.split('\n')))
    return out

def main(p4, p3, mod, out, logp):
    raw = open(p4, 'rb').read(); assert raw.count(b'\r\n') == raw.count(b'\n')
    b4 = split(raw.decode('latin1')); b3 = split(open(p3, 'rb').read().decode('latin1'))
    ids4 = [k for k, _ in b4]; ids3 = [k for k, _ in b3]; d3 = dict(b3)
    md = open(mod, 'rb').read()
    log = []; inserts = {}
    for lv in LEVELS:
        assert lv not in ids4, lv
        blk, dropped = [], []
        for x in d3[lv]:
            typ, _, nm = x.partition(' ')
            if typ == 'symbol' and struct.pack('<I', sid(nm.strip())) not in md: dropped.append(x); continue
            if typ == 'sound-bank' and nm.strip() in DROP_BANKS: dropped.append(x); continue
            blk.append(x.rstrip())
        blk += ['package-ts ' + t for t in TS]
        i3 = ids3.index(lv); pos = len(b4)
        for k in ids3[i3 + 1:]:
            if k in ids4: pos = ids4.index(k); break
        inserts.setdefault(pos, []).append((i3, blk))
        log.append(dict(level=lv, before=ids4[pos] if pos < len(b4) else '<end>', dropped=dropped, lines=blk))
    outl = []
    for i, (k, lines) in enumerate(b4 + [(None, None)]):
        for _, blk in sorted(inserts.get(i, [])): outl += blk
        if lines is not None: outl += lines
    data = ('\r\n'.join(outl) + '\r\n').encode('latin1')
    open(out, 'wb').write(data); json.dump(log, open(logp, 'w'), indent=1)
    chk = split(data.decode('latin1'))
    assert [x for x in chk if x[0] not in LEVELS] == b4
    for l in log: assert dict(chk)[l['level']] == l['lines']
    print('levels', len(b4), '->', len(chk)); print(json.dumps([(l['level'], l['before'], l['dropped']) for l in log], indent=0))

if __name__ == '__main__':
    main(*sys.argv[1:6])
