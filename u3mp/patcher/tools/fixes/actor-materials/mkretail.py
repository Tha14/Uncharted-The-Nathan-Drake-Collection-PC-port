#!/usr/bin/env python3
"""Replace conversions with renamed retail paks, drop the Flynn hair conversion, refresh the manifest, write the list of
other character paks (retail_survey.tsv). Rename is ps4rename: retail bytes plus the new name strings."""
import sys, os, json, hashlib, pickle, re, io, contextlib
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); sys.path.insert(0, HERE)
from lib import *
import ps4check, ps4verify, crashcheck, ps4rename, ps3geom
import ps4actor as A
from ps3mat_am import read_materials
RETAIL = {'mp-hero-french-torso-coop': 'mp-hero-french-torso', 'mp-hero-french-hair-coop': 'mp-hero-french-hair', 'mp-coop-cutter': 'cutter'}


def errs(f):
    with contextlib.redirect_stdout(io.StringIO()):
        e, _, _ = ps4check.check(f, quiet=True)
    return [x for x in e if not x.startswith('unknown item type') and not (x.startswith('tiled size') and 'env-reflection' in x)]


for n, t in RETAIL.items():
    src = f'{RET}/{t}.pak'; dst = f'out/{n}.pak'; tmp = dst + '.tmp'
    ps4rename.rename_pak(src, tmp, t, n, verbose=False)
    # same two post steps as the conversions: no VRAM gap, cbuffer capacity raised (one field per prototype, same size)
    q_ = Pak(tmp); end = q_.pages[-1][0] + q_.pages[-1][1]
    if end != q_.vram_base:
        raw = open(tmp, 'rb').read(); open(tmp, 'wb').write(raw[:end] + raw[q_.vram_base:])
    sys.path.insert(0, T + '/stage'); import cb3fix; cb3fix.fix(tmp, tmp)
    old = find('actor33', n)[0]; q, o = Pak(tmp), Pak(old)
    e, eo = errs(tmp), errs(old); v, _ = ps4verify.verify(tmp, name=n, quiet=True); c, _ = crashcheck.check(tmp)
    ty = lambda p: {i.type for i in p.items} - {'RAW_DATA', 'VRAM_DESC', 'EFFECT'}
    print(n, '<-', t, 'check', len(eo), len(e), 'crash', len(c), c[:2], 'verify', v[:2], 'vram %#x->%#x' % (o.u32(0x20), q.u32(0x20)), 'size', os.path.getsize(old), os.path.getsize(tmp),
          'types+', sorted(ty(q) - ty(o)), 'types-', sorted(ty(o) - ty(q)), 'arts', [i.name for i in q.items_of('ARTGROUP_DATA')], 'skel', hex(struct.unpack_from('<I', q.d, q.pbase(q.items_of('FOREGROUND_SECTION_2')[0].page) + q.items_of('FOREGROUND_SECTION_2')[0].data_off + 8)[0]), 'fg', [i.name for i in q.items_of('FOREGROUND_SECTION_2')])
    vr, _ = ps4verify.verify(src, name=t, quiet=True)   # limits the retail pak itself breaks are not caused by the copy
    v = [x for x in v if x.split(' (')[0] not in {y.split(' (')[0] for y in vr}]
    if len(e) > len(eo) or c or v:
        print('   REJECT', n); os.remove(tmp); continue
    os.replace(tmp, dst)
    if os.path.exists(f'out/{n}.json'): os.remove(f'out/{n}.json')
    json.dump(dict(src='retail', template=t, cap=0, roles=[], cubes=[], cb3=0, vram=q.u32(0x20)), open(f'out/{n}.json', 'w'))

for x in ('out/mp-hero-flynn-hair.pak', 'out/mp-hero-flynn-hair.json'):
    if os.path.exists(x): os.remove(x)
m = {f: hashlib.sha256(open('out/' + f, 'rb').read()).hexdigest() for f in sorted(os.listdir('out')) if f.endswith('.pak')}
json.dump(m, open('manifest.json', 'w'), indent=0); print('manifest', len(m))
