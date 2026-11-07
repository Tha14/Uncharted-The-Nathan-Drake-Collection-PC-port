#!/usr/bin/env python3
"""revertB.py (fixer W): undo ONLY batch100 class B (PS3 x8-0x10000 blockers that batch100 hid) on the installed paks.
Classes A (+0x24 = 0x101, 1:1 retail) and C (0/1 placeholder lightmap uvs) stay.

usage:  python3 revertB.py <work dir that holds overlay-r3> <backup dir> [--dry | --revert]
Each pak is md5-locked: md5_before = batch100's md5_after, md5_after = after this revert (computed on the PC files).
For every word the script also checks the current value before writing. --revert restores the backups taken here."""
import sys, os, json, struct, hashlib, shutil
here = os.path.dirname(os.path.abspath(__file__))

def md5(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''): h.update(b)
    return h.hexdigest()

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]; dry = '--dry' in sys.argv; rev = '--revert' in sys.argv
    work, bak = a[0], a[1]; os.makedirs(bak, exist_ok=True); ok = skip = 0
    for pak, ov, m0, m1, W in json.load(open(os.path.join(here, 'revertB.json'))):
        dst = os.path.join(work, 'overlay-' + ov, 'u3data', 'build', 'main', 'pak23', 'ps4', pak)
        bk = os.path.join(bak, 'overlay-' + ov + '__' + pak)
        if rev:
            if os.path.exists(bk):
                if not dry: shutil.copyfile(bk, dst)
                print('reverted', pak); ok += 1
            continue
        if not os.path.exists(dst): print('MISSING', pak); skip += 1; continue
        m = md5(dst)
        if m == m1: print('already', pak); ok += 1; continue
        if m != m0: print('SKIP (md5 is not batch100 md5_after)', pak, m); skip += 1; continue
        if dry: print('would revert B in', pak, len(W)); ok += 1; continue
        if not os.path.exists(bk): shutil.copyfile(dst, bk)
        with open(dst, 'r+b') as f:
            for off, cur, new in W:
                f.seek(off); v = struct.unpack('<I', f.read(4))[0]
                if v != int(cur, 16): raise SystemExit('unexpected word %s+%#x %08x' % (pak, off, v))
                f.seek(off); f.write(struct.pack('<I', int(new, 16)))
        if md5(dst) != m1: print('MD5 MISMATCH, restoring', pak); shutil.copyfile(bk, dst); skip += 1; continue
        print('B reverted', pak, len(W)); ok += 1
    print('ok', ok, 'skipped', skip)

if __name__ == '__main__': main()
