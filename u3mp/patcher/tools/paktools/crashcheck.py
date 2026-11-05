#!/usr/bin/env python3
"""Static crash checker for converted PS4 paks: every engine-side crash class hit at runtime so far, plus
the load-time limits the engine asserts on.  Run it on every pak before publishing.

GEOMETRY_1 (BG stages / map levels)
  G1 stream record attribute count 1..kMaxAttrDefs (4)      Create 0xead300 (vertex-stream.cpp:146/148)
  G2 stream stride <= kMaxStride (0x1000), size/stride <= kMaxVertexes (0x10000), size == nv*stride
  G3 instsub stream count == submesh ns2 (or 0): 0xe9a490 copies <submesh ns2> records from the instsub
     array -> reads past it otherwise (Shambhala / Warzone lobby crash, ns2fix.py)
  G4 main + per-instance streams of a submesh < kMaxVertexStreams (32)   AddStream (vertex-stream.h:0x90)
  G5 indices < nv
  G6 prototype scope-3 cbuffer capacity (+0x30) >= distinct material feature sets + 2: 0xe86b60 allocates
     exactly that many slots and 0xe86f10 appends one per distinct per-instance cbuffer without a bounds
     check -> heap overrun (Highrise kMaxAttrDefs assert in 0xe9a490, Museum fault 0x1281ac6; cb3fix.py)
  W1 WATER_DATA objects walked at the engine stride 0x108 (0x127d067): water type
     index (+0x0c) < 30 (0x9518-byte type table) for every object (0x110 stride -> Museum fault 0x1281ac6)
Havok BG collision (any pak)
  H1 hkpBvCompressedMeshShape section word +0x4c low byte == numPacked (+0x58)   Cave crash 0x12fbd17 (bvfix)
Spawners (-ingame12 paks)
  S1 spawner of an animated art (ANIM item) must not set int tag 0x66c6d3fe (DisableAsyncUpdate -> anim data
     +0x148 never allocated -> null read 0x10b3b7f; Gompa banners)
  T1 a spawner tag table {data, count} never has data = null (empty table -> one 0xffffffff terminator entry)
  S2 debris-family spawner (SimpleBreakable/DebrisObject/RockFall/SlidingSand) of an animated art needs int
     tag 0x5d4729de = 1 (else +0x148 never allocated; Gompa rails)
Visual (GEOMETRY_1 materials; errors V*, the map must look like PS3)
  V1 every material texture binding resolves to a texture of the pak (no missing texture -> see-through/black)
  V2 no vertex-animated wind variant (GLOBAL_PIVOT_WIND*, BP_TREE_ANIM, ND_TREE_ANIM) on a non-foliage material
     (retail winds only real foliage, e.g. CoB reeds; on floors/walls it makes them sway)
  V3 '-color'/'-diffuse' maps sRGB (BC 0x48/0x4b/0x4e), normal maps linear (retail keeps spec masks sRGB)
  V4 (info) resident textures below 128 px; real caps are reported by ps4stage ('capped') and mapb fails on them
Level
  L1 NAV_MESH_1 items per level <= kMaxNavMeshesPerLevel (150)   (0x10f2ac5)
  L2 pak structure: ndpak validate (pages, fixups in range)
usage: crashcheck.py <pak|dir> [...] [--json out.json]   (exit 1 when any pak fails)"""
import glob, json, os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, 'stage'))
from ndpak import Pak
from mkplan import FOLIAGE, GROUNDISH
import cb3fix

K_MAX_ATTR, K_MAX_STRIDE, K_MAX_VTX, K_MAX_STREAMS, K_MAX_NAV = 4, 0x1000, 0x10000, 32, 150


def _streams(q, ref, n, nv, where, errs):
    if n and not ref:
        errs.append('G1 %s: %d streams, null pointer' % (where, n))
        return
    for i in range(n):
        b = q.pbase(ref[0]) + ref[1] + 0x18 * i
        na, stride, size = struct.unpack_from('<HHI', q.d, b)
        if not 1 <= na <= K_MAX_ATTR:
            errs.append('G1 %s stream %d: %d attributes' % (where, i, na))
        if stride == 0 or stride > K_MAX_STRIDE:
            errs.append('G2 %s stream %d: stride %#x' % (where, i, stride))
            continue
        if size // stride > K_MAX_VTX:
            errs.append('G2 %s stream %d: %d vertices' % (where, i, size // stride))
        if nv is not None and size != nv * stride:
            errs.append('G2 %s stream %d: size %#x != nv %d * stride %d' % (where, i, size, nv, stride))


def check_geometry(q, errs):
    for g in q.items_of('GEOMETRY_1'):
        pg, b = g.page, g.data_off
        nsub = q.u64(q.pbase(pg) + b + 8); sref = q.resolve(pg, b + 0x10)
        subs = {}
        for k in range(nsub):
            so = sref[1] + 0x88 * k
            nmr = q.resolve(sref[0], so + 8)
            nm = q.pstr(*nmr) if nmr else 'sub%d' % k
            nv, ni, ns, ns2 = struct.unpack_from('<4I', q.d, q.pbase(sref[0]) + so + 0x20)
            subs[nm] = (nv, ns2, ns)
            _streams(q, q.resolve(sref[0], so + 0x30), ns, nv, nm, errs)
            _streams(q, q.resolve(sref[0], so + 0x38), ns2, nv, nm + ' inst-default', errs)
            if ns + ns2 >= K_MAX_STREAMS:
                errs.append('G4 %s: %d streams' % (nm, ns + ns2))
            ib = q.resolve(sref[0], so + 0x40)
            if ni and ib:
                o = q.pbase(ib[0]) + ib[1]
                import numpy as np
                idx = np.frombuffer(q.d[o:o + 2 * ni], '<u2')
                if len(idx) and int(idx.max()) >= nv:
                    errs.append('G5 %s: index %d >= nv %d' % (nm, int(idx.max()), nv))
        for _o, cap, nfs, pn, dummy in cb3fix.prototypes(q):
            if cap < nfs + 2 and not dummy:
                errs.append('G6 %s: scope-3 cbuffer capacity %d < %d feature sets + 2' % (pn, cap, nfs))
        il = q.resolve(pg, b + 0x30)
        for i in range(q.u64(q.pbase(pg) + b + 0x28)):
            r = q.resolve(il[0], il[1] + 8 * i)
            n = q.pu32(r[0], r[1] + 0x20); sr = q.resolve(r[0], r[1] + 0x30)
            for k in range(n):
                so = sr[1] + 0x50 * k
                nm = q.pstr(*q.resolve(sr[0], so))
                ns = q.pu32(sr[0], so + 0x10)
                if nm not in subs:
                    errs.append('G3 instance %d sub %d: submesh %s missing' % (i, k, nm))
                    continue
                nv, ns2, nsm = subs[nm]
                if ns and ns != ns2:
                    errs.append('G3 instance %d %s: instsub %d streams != submesh ns2 %d' % (i, nm, ns, ns2))
                _streams(q, q.resolve(sr[0], so + 0x18), ns, nv, 'instance %d %s' % (i, nm), errs)


def check_water(q, errs):
    for it in q.items_of('WATER_DATA'):
        n = q.pu32(it.page, it.data_off + 4); r = q.resolve(it.page, it.data_off + 8)
        for k in range(n if r else 0):
            t, kind = q.pu32(r[0], r[1] + 0x108 * k + 0xc), q.pu32(r[0], r[1] + 0x108 * k + 0x14)
            if t >= 30:
                errs.append('W1 water object %d/%d: type %#x kind %#x (stride 0x108)' % (k, n, t, kind))


def check_collision(d, errs):
    import bvfix
    npf, ns, nb = bvfix.scan(bytearray(d))
    if nb:
        errs.append('H1 %d of %d collision sections with the bad shared-vertex byte' % (nb, ns))
    return npf


def check_spawners(q, errs):
    import ps4ingame
    from sg import spawners, spawner_info
    arts = ps4ingame.anim_arts()
    try:
        sps = spawners(q)
    except Exception:
        return 0
    n = 0
    ps = q.ptrsize
    for it, (pg, o) in sps:
        n += 1
        b = q.pbase(pg)
        if ps == 8:
            tt = q.resolve(pg, o + 0x68)
            if tt is not None and q.resolve(tt[0], tt[1]) is None:
                nm = q.resolve(pg, o + 0x20)
                errs.append('T1 spawner %s: tag table with data = null (MP spawn filter 0x72fbc0 reads data[0])'
                            % (q.pstr(*nm) if nm else hex(o)))
        typ = q.u32(b + o + (48 if ps == 4 else 0x40))
        ar = q.resolve(pg, o + (52 if ps == 4 else 0x48))
        art = q.pstr(*ar) if ar else None
        if not art or art not in arts:
            continue
        tags = spawner_info(q, (pg, o))['tags']

        def ival(k):
            if k not in tags:
                return None
            ty, sz, dp = tags[k]
            return q.u32(q.pbase(dp[0]) + dp[1]) if dp else None
        if ival(ps4ingame.NO_ASYNC_TAG):
            errs.append('S1 spawner of animated art %s sets tag 0x66c6d3fe' % art)
        if typ in ps4ingame.DEBRIS_TYPES and ival(ps4ingame.ANIM_DATA_TAG) != 1:
            errs.append('S2 debris spawner of animated art %s without tag 0x5d4729de=1' % art)
    return n


SRGB_FMTS = {0x48, 0x4b, 0x4e}
LIN_FMTS = {0x47, 0x4a, 0x4d}
WIND = ('GLOBAL_PIVOT_WIND', 'BP_TREE_ANIM', 'ND_TREE_ANIM')


def check_visual(q, errs, info):
    g = q.items_of('GEOMETRY_1')
    if not g:
        return
    g = g[0]
    pg, b = g.page, g.data_off
    S = lambda a, o: q.pstr(*q.resolve(a, o)) if q.resolve(a, o) else ''
    # textures of the pak: full path (no .ndb) -> (fmt, w, h, flag)
    tex = {}
    for it in q.items_of('VRAM_DESC'):
        db = q.pbase(it.page) + it.data_off
        fmt, = struct.unpack_from('<I', q.d, db + 0x1c)
        w, h = struct.unpack_from('<II', q.d, db + 0x28)
        flag, = struct.unpack_from('<I', q.d, db + 0x34)
        ttype, = struct.unpack_from('<I', q.d, db + 0x18)
        r = q.resolve(it.page, it.data_off + 0x38)
        path = q.pstr(*r) if r else ''
        tex[path[:-4] if path.endswith('.ndb') else path] = (fmt, w, h, flag, ttype)
    nmat = q.u64(q.pbase(pg) + b + 0x48)
    mr = q.resolve(pg, b + 0x50)
    nb_missing = nwind = nsrgb = 0
    for i in range(nmat if mr else 0):
        mp, mo = mr[0], mr[1] + 0x48 * i
        name = S(mp, mo).split(',')[0]
        nf, npar, nbind = struct.unpack_from('<3I', q.d, q.pbase(mp) + mo + 0x30)
        fr = q.resolve(mp, mo + 0x10)
        feats = [q.pstr(*q.resolve(fr[0], fr[1] + 8 * k)) for k in range(nf if fr else 0)]
        leafname = re.search(FOLIAGE, name.lower()) and not re.search(GROUNDISH, name.lower().rsplit('/', 2)[-2] if name.count('/') > 1 else name.lower())
        if any(w_ in f for f in feats for w_ in WIND) and not leafname:
            nwind += 1
            errs.append('V2 wind variant on non-foliage %s' % name)
        br = q.resolve(mp, mo + 0x28)
        for k in range(nbind if br else 0):
            bo = br[1] + 0x28 * k
            pname, full = S(br[0], bo), S(br[0], bo + 0x10)
            t = tex.get(full)
            if t is None and 'LightMap' not in pname and 'Environment' not in pname:
                nb_missing += 1
                if nb_missing <= 5:
                    errs.append('V1 %s %s -> %s not in pak' % (name, pname, full.rsplit('/', 2)[-2] if '/' in full else full))
                continue
            if t is None:
                continue
            if 'Environment' in pname and t[4] != 2:
                errs.append('V5 environment map bound as a 2D texture (PS3 cube converted flat): %s' % (full.rsplit('/', 2)[-2] if '/' in full else full))
            low = full.lower()
            leaf = full.rsplit('/', 2)[-2] if full.count('/') > 1 else full
            if t[3] != 3:
                continue                      # retail (streamed) textures keep retail's encoding
            if re.search(r'-(color|diffuse)\.tga$', leaf.lower()) and t[0] in LIN_FMTS:
                nsrgb += 1; errs.append('V3 colour texture linear: %s' % leaf)
            if re.search(r'-normals?\.tga$', leaf.lower()) and t[0] in SRGB_FMTS:
                nsrgb += 1; errs.append('V3 normal map sRGB: %s' % leaf)
    if nb_missing > 5:
        errs.append('V1 ... %d missing bindings in total' % nb_missing)
    ncap = 0
    for full, (fmt, w, h, flag, _t) in tex.items():
        low = full.lower()
        if flag == 3 and max(w, h) < 128 and not any(k in low for k in ('flat', 'default', 'lightmap', 'missing', '/sky')):
            ncap += 1
    # V4 is informational here (a PS3 texture can be natively small); a real cap shows in the converter report
    info['visual'] = dict(materials=nmat, missing_bindings=nb_missing, wind=nwind, srgb=nsrgb, capped=ncap,
                          textures=len(tex))


def check(path):
    errs = []
    info = {}
    try:
        q = Pak(path)
    except Exception as ex:
        return ['L2 unreadable: %r' % ex], info
    try:
        v = q.validate() if hasattr(q, 'validate') else []
        errs += ['L2 ' + e for e in (v or [])]
    except Exception as ex:
        errs.append('L2 validate: %r' % ex)
    if q.platform != 'ps4':
        return ['L2 not a PS4 pak'], info
    try:
        check_geometry(q, errs)
    except Exception as ex:
        errs.append('G? geometry walk failed: %r' % ex)
    try:
        check_visual(q, errs, info)
    except Exception as ex:
        errs.append('V? material walk failed: %r' % ex)
    try:
        check_water(q, errs)
    except Exception as ex:
        errs.append('W? water walk failed: %r' % ex)
    info['collision_packfiles'] = check_collision(q.d, errs)
    nav = len(q.items_of('NAV_MESH_1'))
    if nav > K_MAX_NAV:
        errs.append('L1 %d NAV_MESH_1 items' % nav)
    if os.path.basename(path).endswith('-ingame12.pak'):
        try:
            info['spawners'] = check_spawners(q, errs)
        except Exception as ex:
            errs.append('S? spawner walk failed: %r' % ex)
    return errs, info


def expand(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += sorted(glob.glob(os.path.join(glob.escape(a), '**', '*.pak'), recursive=True))
        else:
            out.append(a)
    return out


if __name__ == '__main__':
    args = sys.argv[1:]
    jout = None
    if '--json' in args:
        i = args.index('--json'); jout = args[i + 1]; del args[i:i + 2]
    res = {}
    nbad = 0
    for f in expand(args):
        errs, info = check(f)
        res[f] = dict(errors=errs, **info)
        if errs:
            nbad += 1
            print('FAIL %s' % f)
            for e in errs[:8]:
                print('   ', e)
            if len(errs) > 8:
                print('    ... %d more' % (len(errs) - 8))
    print('%d paks checked, %d fail' % (len(res), nbad))
    if jout:
        json.dump(res, open(jout, 'w'), indent=1)
    sys.exit(1 if nbad else 0)


# ---------------------------------------------------------------- FIDELITY (PS3 source vs PS4 output, MAPS-A)
FIDELITY_ITEMS = ('VIS_INFO_1', 'COLLISION_DATA_HAVOK_BG', 'LIGHT_TABLE_1', 'WATER_DATA', 'LEVEL_OFFSET_1',
                  'SPAWNER_GROUP', 'REGION_TREE', 'NAV_MESH_1', 'FEATURE_DB_2')


def fidelity(ps3_path, ps4_path):
    """1:1 object completeness of one converted level: [] when the PS4 pak keeps every PS3 object.
    F1 BG prototypes / instances / LOD0 triangles differ; F2 a PS3 item type of FIDELITY_ITEMS is missing;
    F3 spawners lost (by name); F4 water objects differ."""
    import collections
    from ndpak import Pak
    import fidelity as FD
    errs = []
    p3, p4 = Pak(ps3_path), Pak(ps4_path)
    t3 = {i.type for i in p3.items}; t4 = {i.type for i in p4.items}
    for ty in FIDELITY_ITEMS:
        if ty in t3 and ty not in t4:
            errs.append('F2 %s missing' % ty)
    if 'PM_COLLECTION_2' in t3:
        c3 = FD.bg_counts_ps3(p3); c4 = FD.bg_counts_ps4(p4)
        if 'WATER_DATA' in t3 and 'WATER_DATA' in t4:
            import waterdata
            nw = len(waterdata.materials(p3))         # one dummy water-material prototype / instance / triangle each
            c4['protos'] -= nw; c4['insts'] -= nw; c4['tris'] -= nw
        for k in c3:
            if c3[k] != c4[k]:
                errs.append('F1 BG %s %d -> %d' % (k, c3[k], c4[k]))
    if 'SPAWNER_GROUP' in t3:
        n3 = collections.Counter(n for n, a in FD.spawners_of(p3)); n4 = collections.Counter(n for n, a in FD.spawners_of(p4))
        lost = list((n3 - n4).elements())
        if lost:
            errs.append('F3 %d spawners lost (%s)' % (len(lost), ', '.join(lost[:4])))
    if 'WATER_DATA' in t3 and 'WATER_DATA' in t4:
        w3 = p3.pu32(p3.items_of('WATER_DATA')[0].page, p3.items_of('WATER_DATA')[0].data_off + 4)
        w4 = p4.pu32(p4.items_of('WATER_DATA')[0].page, p4.items_of('WATER_DATA')[0].data_off + 4)
        if w3 != w4:
            errs.append('F4 water objects %d -> %d' % (w3, w4))
    return errs
