#!/usr/bin/env python3
"""Integrity check of a converted PS4 background stage pak (what the loader resolves):
  * ps4check (header/pages/fixups, VRAM descs, TEXTURE_TABLE, EFFECT_TABLE, every material
    binding -> geometry texture -> VRAM_DESC, every material effect hash -> EFFECT_TABLE),
    tolerating LIGHT_TABLE_1 and env cube maps (VRAM type 2) which retail stages also carry
  * login order == retail rosetta stage (san-upper-multi-stage); item types within the retail set
  * pages < 0x1c0000, VRAM directly after the last page
  * GEOMETRY_1: every submesh material in the material table, stream sizes == nv*stride,
    indices < nv, prototype submesh names -> submeshes, instance prototype / instsub names resolve,
    per-instance stream sizes == nv*stride
  * lightmaps: a stage that carries lightmaps/<stage>/... binds every g_tLightMap* to them
usage: stagecheck.py <pak> [...]   (exit 1 on problems)"""
import os, sys, struct
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
from ndpak import Pak
import ps4check
from ps4bg import geometry
from stagecmp import protos_insts, login_types

RETAIL_LOGIN = ['LEVEL_INFO_1', 'DEBUG_INFO_1', 'LEVEL_PATS_1', 'EFFECT_TABLE', 'TEXTURE_TABLE', 'GEOMETRY_1',
                'LIGHT_TABLE_1', 'ATTACHED_FG_MAPPING', 'TAG_FLOAT']
TYPES = {'TEXTURE_TABLE', 'VRAM_DESC', 'RAW_DATA', 'EFFECT_TABLE', 'EFFECT', 'GEOMETRY_1', 'LEVEL_INFO_1',
         'LEVEL_PATS_1', 'DEBUG_INFO_1', 'ATTACHED_FG_MAPPING', 'TAG_FLOAT', 'LIGHT_TABLE_1',
         'COLLISION_DATA_HAVOK_BG'}


def check(path, quiet=False):
    errs, info, _ = ps4check.check(path, quiet=True)
    p = Pak(path)
    stage = os.path.basename(path)[:-4]
    texs = {t['path']: t for t in p.textures()}
    errs = [e for e in errs if e not in ('unknown item type LIGHT_TABLE_1',
                                         'unknown item type COLLISION_DATA_HAVOK_BG')]
    keep = []
    for i, e in enumerate(errs):
        if e.startswith('tiled size') or e == 'type 2':
            m = [t for t in texs.values() if t['path'] in e]
            if e == 'type 2' or (m and m[0]['type'] == 2):
                continue                      # cube map: 6 faces (retail stages carry them too)
        keep.append(e)
    errs = keep
    lt = login_types(p)
    if lt != RETAIL_LOGIN and lt != RETAIL_LOGIN[:-1] + ['COLLISION_DATA_HAVOK_BG', 'TAG_FLOAT']:
        errs.append('login order %s' % lt)   # retail ldn-train-stage: collision before TAG_FLOAT
    for it in p.items_of('COLLISION_DATA_HAVOK_BG'):
        import collision
        b = p.pbase(it.page) + it.data_off
        tr = p.resolve(it.page, it.data_off + 0x70)
        if tr is None:
            errs.append('COLLISION_DATA_HAVOK_BG without trailer')
            continue
        pfr = p.resolve(tr[0], tr[1] + 8)
        size = p.u32(p.pbase(tr[0]) + tr[1] + 0x10)
        pf = p.d[p.pbase(pfr[0]) + pfr[1]:p.pbase(pfr[0]) + pfr[1] + size]
        if (p.pbase(pfr[0]) + pfr[1]) % 16:
            errs.append('collision packfile not 16-aligned')
        try:
            r = collision.decode_bvcm(pf)
            if r['bad']:
                errs.append('collision tree containment failures: %d' % r['bad'])
            info.append('collision: %d triangles, %d sections' % (sum(len(s) for s in r['tris']), r['nsec']))
        except Exception as ex:
            errs.append('collision packfile unreadable: %r' % ex)
    for it in p.items:
        if it.type not in TYPES:
            errs.append('item type %s not in retail stages' % it.type)
    for i, (po, ps, _) in enumerate(p.pages):
        if ps > 0x200000:
            errs.append('page %d too large %#x' % (i, ps))
    if p.vram_base != p.pages[-1][0] + p.pages[-1][1]:
        errs.append('VRAM base not at end of pages')
    G = geometry(p)
    g = p.items_of('GEOMETRY_1')[0]; pg, b = g.page, g.data_off
    if (G['ver'], G['flags']) != (0x10, 0):
        errs.append('GEOMETRY_1 ver/flags %#x/%d' % (G['ver'], G['flags']))
    nmat = p.u64(p.pbase(pg) + b + 0x48); mp, mo = p.resolve(pg, b + 0x50)
    mats = {}
    for i in range(nmat):
        o = mo + 0x48 * i
        nm = p.pstr(*p.resolve(mp, o))
        nb = p.pu32(mp, o + 0x38); br = p.resolve(mp, o + 0x28)
        mats[nm] = [(p.pstr(*p.resolve(br[0], br[1] + 0x28 * k)), p.pstr(*p.resolve(br[0], br[1] + 0x28 * k + 0x10)))
                    for k in range(nb if br else 0)]
    subs = {s['name']: s for s in G['subs']}
    for s in G['subs']:
        if s['mat'] not in mats:
            errs.append('submesh %s material %s missing' % (s['name'], s['mat']))
        if len(s['idx']) and int(s['idx'].max()) >= s['nv']:
            errs.append('submesh %s index out of range' % s['name'])
        for st in s['streams'] + s['streams2']:
            if st['size'] != st['stride'] * s['nv']:
                errs.append('submesh %s stream slot %d size %#x != nv*stride' % (s['name'], st['slot'], st['size']))
    P, I = protos_insts(p)
    pl = p.resolve(pg, b + 0x20)
    for i in range(p.u64(p.pbase(pg) + b + 0x18)):
        r = p.resolve(pl[0], pl[1] + 8 * i)
        ns = p.pu32(r[0], r[1] + 4); sn = p.resolve(r[0], r[1] + 0x18)
        for k in range(ns):
            nm = p.pstr(*p.resolve(sn[0], sn[1] + 0x10 * k))
            if nm not in subs:
                errs.append('prototype submesh name %s unresolved' % nm)
    for ins in I:
        if ins['proto'] not in P:
            errs.append('instance prototype %s missing' % ins['proto'])
        for s in ins['subs']:
            if s['name'] not in subs:
                errs.append('instsub %s unresolved' % s['name']); continue
            for st in s['st']:
                if st['size'] != st['stride'] * subs[s['name']]['nv']:
                    errs.append('instsub %s slot %d size mismatch' % (s['name'], st['slot']))
    lms = [t for t in texs if '/lightmaps/%s/' % stage in t]
    nlm = 0
    if lms:
        for nm, bl in mats.items():
            for par, full in bl:
                if par.startswith('g_tLightMap'):
                    nlm += 1
                    if '/lightmaps/%s/' % stage not in full:
                        errs.append('material %s %s -> %s (not the stage lightmap)' % (nm[:50], par, full[-60:]))
    info.append('%d submeshes, %d prototypes, %d instances, %d materials, %d textures, %d lightmap bindings, '
                '%d pages, %d bytes' % (len(subs), len(P), len(I), len(mats), len(texs), nlm, len(p.pages), len(p.d)))
    if not quiet:
        print(path)
        for l in info:
            if not l.startswith('EFFECT_TABLE'):
                print('  ' + l)
        print('  RESULT: %s' % ('OK' if not errs else '%d problems' % len(errs)))
        for e in errs[:30]:
            print('   - ' + e)
    return errs, info


if __name__ == '__main__':
    bad = 0
    for f in sys.argv[1:]:
        e, _ = check(f)
        bad += bool(e)
    sys.exit(1 if bad else 0)
