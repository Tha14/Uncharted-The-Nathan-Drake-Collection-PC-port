#!/usr/bin/env python3
"""apply100.py (fixer W, batch100-blockers): Village / London / Fort black surfaces = shadow-blocker shells drawn in the colour pass,
plus lightmapped submeshes whose lightmap uvs are a PS3 placeholder of only 0 / 1.

usage:  python3 apply100.py <work dir that holds overlay-r3/-r2/-port> <backup dir> [--dry | --check | --revert]
  --check   compute every write in memory and compare the result with SWAP.tsv md5_after (no file is written)
  --dry     list what would be patched
  --revert  copy the backups back
Every pak in SWAP.tsv is md5-locked: a pak whose md5 is neither md5_before nor md5_after is skipped.

Writes (4-byte little-endian words in GEOMETRY_1 instance-submesh records, no size change):
  A  blocker submesh with +0x20 bit 0x10000 (shadow-only) but +0x24 != 0x101  ->  +0x24 |= 0x101 (retail hidden-blocker word)
  B  blocker submesh drawn (+0x20 has no 0x10000)                              ->  +0x20 |= 0x10000 (ik-only 0x1010000), +0x24 |= 0x101
  C  lightmapped submesh (+0x20 bit 0x100) whose slot-5 lightmap uvs are ALL integers (PS3 placeholder, rows (0,0)/(1,1)/(M,M))
                                                                               ->  +0x20 &= ~0x100 (vertex lit, as batch99)
  blocker = material name containing shadowblocker, ik-only or canopy-shadow. museum-multi(-stage) is not in SWAP.tsv."""
import sys, os, struct, hashlib, shutil
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(here, 'tools'))
import numpy as np
from ndpak import Pak
from ps4bg import streams

def writes_for(data):
    q = Pak(data)
    its = q.items_of('GEOMETRY_1')
    if not its: return []
    g = its[0]; pg, b = g.page, g.data_off; il = q.resolve(pg, b + 0x30); n = q.u64(q.pbase(pg) + b + 0x28)
    if not il: return []
    nsub = q.u64(q.pbase(pg) + b + 8); sref = q.resolve(pg, b + 0x10); mat = {}
    for k in range(nsub):
        o = sref[1] + 0x88 * k; nm = q.pstr(*q.resolve(sref[0], o + 8)); mr = q.resolve(sref[0], o + 0x48)
        mat[nm] = (q.pstr(*mr) if mr else '').split(',')[0]
    W = {}
    for i in range(n):
        r = q.resolve(il[0], il[1] + 8 * i); ns = q.pu32(r[0], r[1] + 0x20); sr = q.resolve(r[0], r[1] + 0x30)
        if not sr: continue
        for k in range(ns):
            o = sr[1] + 0x50 * k; ab = q.pbase(sr[0]) + o; m = mat.get(q.pstr(*q.resolve(sr[0], o)), '?')
            f, f2 = struct.unpack_from('<II', q.d, ab + 0x20)
            blk = 'shadowblocker' in m or 'ik-only' in m or 'canopy-shadow' in m
            n20, cls = f, ''
            if f & 0x100:
                nst = q.pu32(sr[0], o + 0x10); ref = q.resolve(sr[0], o + 0x18)
                for s in (streams(q, ref, nst) if ref else []):
                    if s['slot'] == 5 and s['type'] == 0x22 and s['data'] is not None:
                        d = s['data']
                        if len(d) and (d == np.round(d)).all(): n20 &= ~0x100; cls = 'C'
            if blk and not (f & 0x10000 and f2 == 0x101):
                if not f & 0x10000: n20 |= (0x1010000 if 'ik-only' in m else 0x10000); cls = 'B'
                W[ab + 0x24] = (f2, f2 | 0x101, 'A' if f & 0x10000 else 'B', i, k, m)
            if n20 != f: W[ab + 0x20] = (f, n20, cls, i, k, m)
    return sorted(W.items())

def md5b(b): return hashlib.md5(b).hexdigest()

def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]; mode = ([x for x in sys.argv[1:] if x.startswith('--')] or ['--apply'])[0]
    work, bak = a[0], a[1]; ok = skip = 0
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(here, 'SWAP.tsv')) if l.strip() and not l.startswith('pak\t')]
    if mode != '--check': os.makedirs(bak, exist_ok=True)
    for r in rows:
        pak, ov, m0, m1 = r[:4]
        dst = os.path.join(work, 'overlay-' + ov, 'u3data', 'build', 'main', 'pak23', 'ps4', pak)
        bk = os.path.join(bak, 'overlay-' + ov + '__' + pak)
        if mode == '--revert':
            if os.path.exists(bk): shutil.copyfile(bk, dst); print('reverted', ov, pak); ok += 1
            continue
        if not os.path.exists(dst): print('MISSING', ov, pak); skip += 1; continue
        data = bytearray(open(dst, 'rb').read()); m = md5b(data)
        if m == m1: print('already', ov, pak); ok += 1; continue
        if m != m0: print('SKIP (md5 changed since the scan)', ov, pak, m); skip += 1; continue
        W = writes_for(dst)
        for off, (old, new, *_x) in W:
            if struct.unpack_from('<I', data, off)[0] != old: raise SystemExit('unexpected word %s+%#x' % (pak, off))
            struct.pack_into('<I', data, off, new)
        if md5b(data) != m1: print('MD5 MISMATCH (not written)', ov, pak, len(W)); skip += 1; continue
        cnt = {c: sum(1 for _o, w in W if w[2] == c) for c in 'ABC'}
        if mode in ('--check', '--dry'): print('ok-would-patch', ov, pak, len(W), cnt); ok += 1; continue
        if not os.path.exists(bk): shutil.copyfile(dst, bk)
        with open(dst, 'r+b') as fh:
            for off, (old, new, *_x) in W: fh.seek(off); fh.write(struct.pack('<I', new))
        if md5b(open(dst, 'rb').read()) != m1: print('MD5 MISMATCH after write, restoring', pak); shutil.copyfile(bk, dst); skip += 1; continue
        print('patched', ov, pak, len(W), cnt); ok += 1
    print('ok', ok, 'skipped', skip)

if __name__ == '__main__': main()
