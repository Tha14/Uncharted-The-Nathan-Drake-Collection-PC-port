#!/usr/bin/env python3
"""waterlayer.py (fixer L, batch97-water-layer): set the Havok collision filter info of water collision meshes to the
retail value (layer 2), in place.

Retail rule (Bluepoint's tools; exact on 3,625,841 primitives of 604 retail U3 PS4 level paks, 0 mismatches):
  filter = 2 if flags & 0x4000 (water) else 13 if flags & 0x400 else 1
           | 0x4000000 if flags & 0x100 | 0x8000000 if flags & 0x200 | 0x20000000 if flags & 0x2000 | 0x40000000 if flags & 0x10000
(flags = the PS3 ND collision material flags = hkpBvCompressedMeshShape userData). Our converter wrote filter 1 for every
primitive, so water surfaces sat in layer 1, not the water layer 2.
In place: only items whose filter palette has ONE entry and whose primitives all map to ONE filter value are patched
(every water collision item in the installed set). BG items that need several filter values are reported, not patched.
usage: waterlayer.py <pak>...   -> one JSON line per pak: {pak, md5_before, md5_after, writes [[file off, hex u32 LE]], skipped}"""
import sys, os, struct, json, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ndpak

def rule(u):
    f = 2 if u & 0x4000 else (13 if u & 0x400 else 1)
    for b, m in ((0x100, 0x4000000), (0x200, 0x8000000), (0x2000, 0x20000000), (0x10000, 0x40000000)):
        if u & b: f |= m
    return f

def plan(path):
    d = open(path, 'rb').read(); p = ndpak.Pak(d)
    writes, skipped, items = [], [], []
    for k, it in enumerate(p.items_of('COLLISION_DATA_HAVOK_BG')):
        t = p.resolve(it.page, it.data_off + 0x70)
        if not t: continue
        tb = p.pbase(t[0]) + t[1]; pfp = p.resolve(t[0], t[1] + 8); size = struct.unpack_from('<I', d, tb + 0x10)[0]
        pfb = p.pbase(pfp[0]) + pfp[1]; pf = d[pfb:pfb + size]
        st, lf, gf = struct.unpack_from('<III', pf, 0x40 + 0x30 * 2 + 20)
        loc = {}
        for o in range(st + lf, st + gf, 8):
            a, tt = struct.unpack_from('<II', pf, o)
            if a != 0xffffffff: loc[a] = tt
        D = pf[st:st + lf]
        arr = lambda o: (loc.get(o), struct.unpack_from('<I', D, o + 8)[0])
        fa, fn = arr(0x30); ua, un = arr(0x40); ra, rn = arr(0xe0)
        fpal = struct.unpack_from('<%dI' % fn, D, fa) if fa is not None and fn else ()
        upal = struct.unpack_from('<%dI' % un, D, ua) if ua is not None and un else ()
        want = set(); nwrong = 0
        for i in range(rn if ra is not None else 0):
            v, i0, cnt, _ = struct.unpack_from('<IBBH', D, ra + 8 * i)
            w = rule(upal[(v >> 8) & 0xff]); want.add(w)
            if fpal[v & 0xff] != w: nwrong += cnt
        f35 = d[p.pbase(it.page) + it.data_off + 0x35]
        items.append(dict(item=k, f35=f35, palette=['%#x' % x for x in fpal], want=['%#x' % x for x in sorted(want)], wrong=nwrong))
        if not nwrong: continue
        if len(fpal) == 1 and len(want) == 1:
            writes.append([pfb + st + fa, struct.pack('<I', want.pop()).hex()])
        else:
            skipped.append(dict(item=k, f35=f35, wrong=nwrong, want=len(want)))
    m0 = hashlib.md5(d).hexdigest(); b = bytearray(d)
    for off, hx in writes: b[off:off + 4] = bytes.fromhex(hx)
    return dict(pak=os.path.basename(path), md5_before=m0, md5_after=hashlib.md5(b).hexdigest(), size=len(d),
                writes=[[o, h] for o, h in writes], skipped=skipped, items=items), bytes(b)

if __name__ == '__main__':
    out = sys.argv[1] if sys.argv[1].endswith('.jsonl') else None
    for f in sys.argv[2 if out else 1:]:
        r, _ = plan(f)
        s = json.dumps(r)
        if out: open(out, 'a').write(s + '\n')
        else: print(s)
