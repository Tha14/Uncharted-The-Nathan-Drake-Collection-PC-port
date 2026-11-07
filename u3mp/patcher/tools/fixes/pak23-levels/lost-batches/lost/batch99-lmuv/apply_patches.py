#!/usr/bin/env python3
"""apply_patches.py (fixer W, batch99-lmuv): clear the false "own lightmap uvs" flag (instsub +0x20 bit 0x100) on vertex-lit submeshes.
usage:  python3 apply_patches.py <work dir that holds overlay-r3/-r2/-port> <backup dir> [--revert] [--dry]
Each patches/<pak>.json names the overlay the pak was scanned in (variants[0].base) and the exact md5 of the installed file.
A pak whose md5 differs (re-installed / rebuilt since the scan) is reported and skipped; re-scan it with tools/scanlm.py.
Writes: 4-byte little-endian words, instance-submesh +0x20 (0x100 cleared). The backup is taken once, before the first write;
--revert copies it back."""
import sys, os, json, glob, hashlib, shutil
here = os.path.dirname(os.path.abspath(__file__))
def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''): h.update(b)
    return h.hexdigest()
def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]; dry = '--dry' in sys.argv; rev = '--revert' in sys.argv
    work, bak = a[0], a[1]; os.makedirs(bak, exist_ok=True); ok = skip = 0
    for pf in sorted(glob.glob(os.path.join(here, 'patches', '*.json'))):
        P = json.load(open(pf)); v = P['variants'][0]
        ov = v['base'].split('overlay-')[1].split()[0]
        dst = os.path.join(work, 'overlay-' + ov, 'u3data', 'build', 'main', 'pak23', 'ps4', P['pak'])
        b = os.path.join(bak, 'overlay-' + ov + '__' + P['pak'])
        if rev:
            if os.path.exists(b):
                if not dry: shutil.copyfile(b, dst)
                print('reverted', ov, P['pak']); ok += 1
            continue
        if not os.path.exists(dst): print('MISSING', ov, P['pak']); skip += 1; continue
        m = md5(dst)
        if m == v['md5_after']: print('already', ov, P['pak']); ok += 1; continue
        if m != v['md5_before']: print('SKIP (md5 changed since the scan)', ov, P['pak'], m); skip += 1; continue
        if dry: print('would patch', ov, P['pak'], len(v['writes'])); continue
        if not os.path.exists(b): shutil.copyfile(dst, b)
        with open(dst, 'r+b') as f:
            for (off, hx), old in zip(v['writes'], v['old']):
                f.seek(off); cur = int.from_bytes(f.read(4), 'little')
                if cur != int(old, 16): raise SystemExit('unexpected word at %s+%#x' % (P['pak'], off))
                f.seek(off); f.write(int(hx, 16).to_bytes(4, 'little'))
        if md5(dst) != v['md5_after']: print('MD5 MISMATCH after patch, restoring', P['pak']); shutil.copyfile(b, dst); skip += 1; continue
        print('patched', ov, P['pak'], len(v['writes'])); ok += 1
    print('ok', ok, 'skipped', skip)
if __name__ == '__main__': main()
