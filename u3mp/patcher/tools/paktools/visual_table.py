#!/usr/bin/env python3
"""VISUAL table for published maps-b maps: per BG pak materials / templates / PS3 constants written / flat slots /
caps / crashcheck V1-V3 / VRAM.  usage: visual_table.py map [map ...]  (markdown to stdout)"""
import sys, os, json, glob
sys.path[:0] = [os.path.dirname(os.path.abspath(__file__))]
import crashcheck
OUT = '/mnt/user-data/outputs/maps-b'
B7 = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/maps/build7'
rows = ['| map | BG pak | MB | materials | template | PS3 consts | flat slots | capped | V1 | V2 | V3 |',
        '|---|---|---|---|---|---|---|---|---|---|---|']
for m in sys.argv[1:]:
    for f in sorted(glob.glob('%s/%s/u3data/build/main/pak23/ps4/*.pak' % (OUT, m))):
        if f.endswith('-ingame12.pak'):
            continue
        e, info = crashcheck.check(f)
        v = info.get('visual')
        if not v or not v['materials']:
            continue
        lv = os.path.basename(f)[:-4]
        rep = None
        for rp in ('%s/%s/%s.report.json' % (B7, m, lv),):
            if os.path.exists(rp):
                rep = json.load(open(rp))
        tm = sum(1 for a in (rep or {}).get('approx', []) if a.startswith('material ') and '-> template' in a)
        rows.append('| %s | %s | %.1f | %d | %s | %s | %s | %s | %d | %d | %d |' % (
            m, lv, os.path.getsize(f) / 1e6, v['materials'], tm if rep else 'local',
            (rep or {}).get('ps3params', '-') if rep else '-', len(set((rep or {}).get('flat', []))) if rep else '-',
            len((rep or {}).get('capped', [])) if rep else 0, v['missing_bindings'], v['wind'], v['srgb']))
print('\n'.join(rows))
