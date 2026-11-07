#!/usr/bin/env python3
"""apply_patches.py (fixer L, n015-level-info): byte patches, md5-locked.
usage: python3 apply_patches.py <overlay-r3 pak23/ps4 dir> <backup dir> [--dry] [--revert]"""
import sys, os, json, glob, hashlib, shutil
here = os.path.dirname(os.path.abspath(__file__))
md5 = lambda b: hashlib.md5(b).hexdigest()
a = [x for x in sys.argv[1:] if not x.startswith('--')]; dry = '--dry' in sys.argv; rev = '--revert' in sys.argv
pak_dir, bak = a[0], a[1]
for pf in sorted(glob.glob(os.path.join(here, 'patches', '*.json'))):
    P = json.load(open(pf)); dst = os.path.join(pak_dir, P['pak']); b = os.path.join(bak, P['pak'])
    if rev:
        if os.path.exists(b) and md5(open(b, 'rb').read()) == P['md5_before']:
            if not dry: shutil.copyfile(b, dst)
            print('reverted', P['pak'])
        else: print('no backup', P['pak'])
        continue
    d = bytearray(open(dst, 'rb').read()); m = md5(d)
    if m == P['md5_after']: print('already', P['pak']); continue
    if m != P['md5_before']: print('SKIP (md5 %s)' % m, P['pak']); continue
    for off, old, new in P['writes']:
        o, n = bytes.fromhex(old), bytes.fromhex(new)
        assert d[off:off + len(o)] == o, (P['pak'], off)
        d[off:off + len(n)] = n
    if md5(d) != P['md5_after']: print('FAILED md5', P['pak']); continue
    if dry: print('would patch', P['pak']); continue
    os.makedirs(bak, exist_ok=True)
    if not os.path.exists(b): shutil.copyfile(dst, b)
    open(dst, 'wb').write(d); print('patched', P['pak'])
