#!/usr/bin/env python3
"""In-place fix of converted BG stage/level paks whose per-instance stream lists disagree with their
submesh defaults (crash: 'desc.GetNumAttributes() <= kMaxAttrDefs', vertex-stream.cpp:146).

Engine (0xe9a490): for every instance submesh it copies <submesh ns2> 0x18-byte stream records from the
submesh's default streams2 array (+0x2c count / +0x38 ptr), then, when the instsub has its own streams
(+0x10 count > 0), copies <submesh ns2> records from the instsub array (+0x18) - the instsub count is not
used.  Retail always has instsub count == submesh ns2.  Older ps4stage builds dropped the lightmap-uv
stream (slot 5) from every instsub of a stage without lightmaps but kept it in the submesh streams2, so the
engine read one record past the instsub array.

Fix: reorder the submesh streams2 records to the instsub slot order (extra records moved to the end) and
set ns2 to the instsub count.  Records are permuted together with the destination page of their pointer
fixups (u16 srcPage, u16 dstPage, u32 offset entries), so no fixup is added, moved or re-sorted.
usage: ns2fix.py <in.pak> <out.pak>   |  ns2fix.scan(pak) -> list of mismatches"""
import os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
from ndpak import Pak


def _geo(q):
    g = q.items_of('GEOMETRY_1')
    if not g:
        return None
    return g[0].page, g[0].data_off


def _slot(q, page, rec_off):
    fr = q.resolve(page, rec_off + 8)
    return q.d[q.pbase(fr[0]) + fr[1]] if fr else None


def scan(q):
    """-> {submesh index: dict(sp, so, ns2, slots, inst_slots set, bad)}"""
    gg = _geo(q)
    if gg is None:
        return {}
    pg, b = gg
    nsub = q.u64(q.pbase(pg) + b + 8); sref = q.resolve(pg, b + 0x10)
    subs = {}
    names = {}
    for k in range(nsub):
        so = sref[1] + 0x88 * k
        nm = q.pstr(*q.resolve(sref[0], so + 8))
        ns2 = q.pu32(sref[0], so + 0x2c)
        r2 = q.resolve(sref[0], so + 0x38)
        slots = [_slot(q, r2[0], r2[1] + 0x18 * i) for i in range(ns2)] if (ns2 and r2) else []
        subs[k] = dict(name=nm, sp=sref[0], so=so, ns2=ns2, r2=r2, slots=slots, inst=set())
        names[nm] = k
    il = q.resolve(pg, b + 0x30)
    for i in range(q.u64(q.pbase(pg) + b + 0x28)):
        r = q.resolve(il[0], il[1] + 8 * i)
        n = q.pu32(r[0], r[1] + 0x20); sr = q.resolve(r[0], r[1] + 0x30)
        for k in range(n):
            so = sr[1] + 0x50 * k
            nm = q.pstr(*q.resolve(sr[0], so))
            ns = q.pu32(sr[0], so + 0x10)
            if ns == 0:
                continue
            ref = q.resolve(sr[0], so + 0x18)
            isl = tuple(_slot(q, ref[0], ref[1] + 0x18 * j) for j in range(ns))
            subs[names[nm]]['inst'].add(isl)
    out = {}
    for k, s in subs.items():
        bad = [isl for isl in s['inst'] if len(isl) != s['ns2']]   # the engine copies by count only
        if bad:
            s['bad'] = bad
            out[k] = s
    return out


def fix(src, dst):
    q = Pak(src)
    d = bytearray(q.d)
    bad = scan(q)
    # fixup entry location per (srcPage, offset)
    floc = {}
    for i in range(len(q.fixup_dirs)):
        pg, off, cnt = q.fixup_dirs[i]
        if cnt <= 0:
            continue
        base = pg * 0x80000
        for j in range(cnt):
            fo = base + off + j * 8
            a = q.u16(fo); o = q.u32(fo + 4)
            floc[(a, o)] = fo
    done = []
    for k, s in sorted(bad.items()):
        insts = s['inst']
        if len(insts) != 1:
            raise ValueError('%s: instsubs disagree among themselves %r' % (s['name'], insts))
        isl = list(next(iter(insts)))
        if len(isl) > s['ns2'] or any(x not in s['slots'] for x in isl):
            raise ValueError('%s: instsub slots %r not a subset of %r' % (s['name'], isl, s['slots']))
        order = [s['slots'].index(x) for x in isl]
        order += [i for i in range(s['ns2']) if i not in order]
        rp, ro = s['r2']
        base = q.pbase(rp) + ro
        recs = [bytes(d[base + 0x18 * i: base + 0x18 * i + 0x18]) for i in range(s['ns2'])]
        dpg = []
        for i in range(s['ns2']):
            row = []
            for fo_ in (8, 0x10):
                key = (rp, ro + 0x18 * i + fo_)
                row.append(q.u16(floc[key] + 2) if key in floc else None)
            dpg.append(row)
        for ni, oi in enumerate(order):
            d[base + 0x18 * ni: base + 0x18 * ni + 0x18] = recs[oi]
            for j, fo_ in enumerate((8, 0x10)):
                key = (rp, ro + 0x18 * ni + fo_)
                if (key in floc) != (dpg[oi][j] is not None):
                    raise ValueError('%s: fixup presence differs between records' % s['name'])
                if key in floc:
                    struct.pack_into('<H', d, floc[key] + 2, dpg[oi][j])
        struct.pack_into('<I', d, q.pbase(s['sp']) + s['so'] + 0x2c, len(isl))
        done.append((s['name'], s['slots'], isl))
    open(dst, 'wb').write(bytes(d))
    left = scan(Pak(dst))
    if left:
        raise ValueError('mismatches left after fix: %d' % len(left))
    return done


if __name__ == '__main__':
    r = fix(sys.argv[1], sys.argv[2])
    print('%d submeshes fixed' % len(r))
    for x in r[:10]:
        print(' ', x)
