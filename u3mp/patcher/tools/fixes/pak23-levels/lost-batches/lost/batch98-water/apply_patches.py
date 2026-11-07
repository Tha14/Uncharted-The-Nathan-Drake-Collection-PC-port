#!/usr/bin/env python3
"""apply_patches.py (agent R, batch92-shadowblockers; reused by fixer L for batch97-water-layer): apply patches/*.json to the installed level paks.
usage:  python3 apply_patches.py <overlay pak23/ps4 dir> <backup dir> [--revert] [--dry]
Each patch lists accepted bases (variants: the pak as on overlay-r3, with or without batch90-ownfix / batch90n-normals), each
with its own md5_before / md5_after / writes. A pak matching no base is reported and skipped (e.g. a batch91 pak: batch91
already contains this fix). batch97: each write sets one u32, the Havok collision filter palette entry of a water collision mesh. The backup is taken once,
before the first write; --revert copies it back."""
import sys, os, json, glob, hashlib, shutil
here = os.path.dirname(os.path.abspath(__file__))
def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''): h.update(b)
    return h.hexdigest()
def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]; dry = '--dry' in sys.argv; rev = '--revert' in sys.argv
    pak_dir, bak = a[0], a[1]; os.makedirs(bak, exist_ok=True); ok = skip = 0
    for pf in sorted(glob.glob(os.path.join(here, 'patches', '*.json'))):
        P = json.load(open(pf)); dst = os.path.join(pak_dir, P['pak']); b = os.path.join(bak, P['pak'])
        if rev:
            if os.path.exists(b):
                if not dry: shutil.copyfile(b, dst)
                print('reverted', P['pak']); ok += 1
            continue
        if not os.path.exists(dst): print('MISSING', P['pak']); skip += 1; continue
        m = md5(dst); V = P['variants']
        if any(m == v['md5_after'] for v in V): print('already', P['pak']); ok += 1; continue
        v = [v for v in V if v['md5_before'] == m]
        if not v: print('SKIP (other base, e.g. batch91)', P['pak'], m); skip += 1; continue
        v = v[0]
        if dry: print('would patch', P['pak'], '(base: %s)' % v['base']); continue
        if not os.path.exists(b): shutil.copyfile(dst, b)
        with open(dst, 'r+b') as f:
            for off, hx in v['writes']:
                f.seek(off); f.write(int(hx, 16).to_bytes(4, 'little'))
        if md5(dst) != v['md5_after']: print('MD5 MISMATCH after patch, restoring', P['pak']); shutil.copyfile(b, dst); skip += 1; continue
        print('patched', P['pak'], '(base: %s)' % v['base']); ok += 1
    print('ok', ok, 'skipped', skip)
if __name__ == '__main__': main()
