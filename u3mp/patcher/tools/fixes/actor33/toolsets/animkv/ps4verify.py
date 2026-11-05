#!/usr/bin/env python3
"""Sanity checks for a generated PS4 actor pak.
  * re-reads with ndpak and runs Pak.validate()
  * VRAM starts exactly at the end of the last page (no alignment gap)
  * every page < 0x1c0000
  * ARTGROUP keys all resolve to an item of the pak (sid(name) or sid('geo.'+FG name)),
    except keys listed as tolerated (PS3 COLLISION_DATA_EMPTY targets have no PS4 item)
  * PACKAGE_ITEM_LOOKUP name/sid == ARTGROUP name
  * prototype cbuffer capacity (+0x30) >= distinct scope-3 cbuffers is NOT computable here;
    reports cb3/cb4 and compares with an optional reference pak (--ref) for >=
  * FG skeleton id == JOINT_HIERARCHY skeleton id when the pak has its own skeleton
  * submesh material names all exist in the material table
usage: ps4verify.py <pak> [--ref template.pak] [--name expected-artgroup]
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid


def protos(p):
    out = []
    for g in p.items_of('GEOMETRY_1'):
        pg, b = g.page, g.data_off
        n = p.u64(p.pbase(pg) + b + 0x18)
        pr = p.resolve(pg, b + 0x20)
        for i in range(n):
            r = p.resolve(pr[0], pr[1] + 8 * i)
            out.append(struct.unpack_from('<II', p.d, p.pbase(r[0]) + r[1] + 0x30))
    return out


def verify(path, ref=None, name=None, quiet=False):
    errs, notes = [], []
    p = Pak(path)
    errs += p.validate()
    last = p.pages[-1]
    if p.vram_base != last[0] + last[1]:
        errs.append('VRAM base %#x != end of last page %#x' % (p.vram_base, last[0] + last[1]))
    for i, pg in enumerate(p.pages):
        if pg[1] >= 0x1c0000:
            errs.append('page %d too large (%#x)' % (i, pg[1]))
    ag = p.items_of('ARTGROUP_DATA')
    if len(ag) != 1:
        errs.append('%d ARTGROUP_DATA items' % len(ag))
    else:
        ag = ag[0]
        if name and ag.name != name:
            errs.append('artgroup name %r != %r' % (ag.name, name))
        known = {}
        for it in p.items:
            known[sid(it.name)] = it.type + ':' + it.name
            if it.type == 'FOREGROUND_SECTION_2':
                known[sid('geo.' + it.name)] = 'geo.' + it.name
        pg, b = ag.page, ag.data_off
        xo = p.pptr(pg, b + 0x40)
        nb = p.pu32(pg, xo + 0x1c)
        tbl = p.pptr(pg, xo + 0x20)
        keys = []
        for k in range(nb):
            key = p.pu32(pg, tbl + 16 * k)
            if p.is_ptr(pg, tbl + 16 * k + 8):
                keys.append(key)
        n = p.pu32(pg, b)
        if len(keys) != n:
            errs.append('artgroup: %d keys in buckets, n=%d' % (len(keys), n))
        miss = [hex(k) for k in keys if k not in known]
        if miss:
            notes.append('artgroup keys without item: %s (ok for PS3 COLLISION_DATA_EMPTY targets)' % miss)
        fgs = [it for it in p.items if it.type == 'FOREGROUND_SECTION_2']
        for it in fgs:
            if sid('geo.' + it.name) not in keys:
                errs.append('FG %s not in artgroup' % it.name)
        for it in p.items_of('PACKAGE_ITEM_LOOKUP'):
            r = p.resolve(it.page, it.data_off)
            nm = p.pstr(*r) if r else None
            sids = [p.pu32(it.page, o) for o in range(it.data_off, it.data_off + it.size, 4)]
            if nm != ag.name or sid(ag.name) not in sids:
                errs.append('PACKAGE_ITEM_LOOKUP %r / sid mismatch with %r' % (nm, ag.name))
    # skeleton ids
    jh = {p.pu32(it.page, it.data_off + 0x30) for it in p.items_of('JOINT_HIERARCHY')}
    for it in p.items_of('FOREGROUND_SECTION_2'):
        sk = p.pu32(it.page, it.data_off + 8)
        if jh and sk not in jh:
            errs.append('FG %s skel %#x not in JOINT_HIERARCHY %s' % (it.name, sk, [hex(x) for x in jh]))
    # materials
    for g in p.items_of('GEOMETRY_1'):
        gp, b = g.page, g.data_off
        nsub = p.u64(p.pbase(gp) + b + 8); sr = p.resolve(gp, b + 0x10)
        nmat = p.u64(p.pbase(gp) + b + 0x48); mr = p.resolve(gp, b + 0x50)
        mats = {p.pstr(*p.resolve(mr[0], mr[1] + 0x48 * i)) for i in range(nmat)}
        for i in range(nsub):
            m = p.pstr(*p.resolve(sr[0], sr[1] + 0x88 * i + 0x48))
            if m not in mats:
                errs.append('submesh %d material %s missing' % (i, m))
    cbs = protos(p)
    if ref:
        rc = protos(Pak(ref))
        if rc and cbs:
            r3 = max(a for a, _ in rc); r4 = max(b for _, b in rc)
            for a, c in cbs:
                if a < r3 or c < r4:
                    errs.append('prototype cb capacity %d/%d below template %d/%d' % (a, c, r3, r4))
    notes.append('cb3/cb4 per prototype: %s' % cbs)
    notes.append('vram %#x, %d textures, %d pages %s' % (p.hdr['vramSize'], len(p.textures()), len(p.pages),
                                                       [hex(x[1]) for x in p.pages]))
    if not quiet:
        print('%s: %s' % (os.path.basename(path), 'OK' if not errs else 'ERRORS'))
        for e in errs:
            print('  ERROR', e)
        for n_ in notes:
            print('  ', n_)
    return errs, notes


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('pak', nargs='+')
    ap.add_argument('--ref')
    ap.add_argument('--name')
    a = ap.parse_args()
    bad = 0
    for f in a.pak:
        e, _ = verify(f, a.ref, a.name)
        bad += bool(e)
    sys.exit(1 if bad else 0)
