"""Uncharted 2 multiplayer skins built from the PS4 Collection's own remastered campaign actor (the "twin").

Most MP skins (actor27/mp-hero-*.pak) are, on PS3, the campaign character of the same name plus the MP setup. Found on
the PS3 pairs (mp-hero-X against costume-hero-X / hero-X / lieutenant ...) and on the 15 retail PS4 pairs:

  * the MP pak = the campaign actor's mesh as LOD0, plus three lower LODs (FOREGROUND_SECTION_2 '<name>-lod1..3'
    with their own prototypes; PS3 also a PM_COLLECTION_2 'mesh.collision.<name>-lodN' each), three TAG_FLOAT
    'lod_distance' (6 / 15 / 20 or 30 m) and one TAG_VEC4 'vis_sphere' + TAG_INT 'vis_joint_index' per LOD, an
    ARTGROUP_DATA that lists the four FG sections. Same skeleton (0x27e57f43), same item types otherwise; no hit or
    collision data beyond the per-LOD 'mesh.collision' collections (PS4 drops those for both).
  * retail PS4 mp-hero LOD0 = the PS4 campaign actor's mesh: same submeshes, materials, index buffers, positions and
    UVs; normals / tangents differ by <= 0.0015 (half-float rounding of a separate cook), a few submeshes have
    2-8 more vertices (seam splits). Materials: identical records (name, shader, parameters, bindings). Textures:
    identical descriptors (hash, format, size, streaming flags); the in-pak low mips differ only in tile padding.
  * MP variants that only re-texture the campaign mesh (chloe-heist, lazarevic, schafer, udf ...) share the PS3 index
    and vertex data block for block but use other materials on some submeshes.

So a skin is built as: the PS3 MP skin converted with the PS4 twin as template (u2actor: LOD1-3, FG sections, tags,
artgroup, MP-only materials), then every LOD0 submesh whose PS3 blocks equal a twin submesh (same triangles and
skinning, positions within 1 mm, UVs within 0.002; or the same surface in another vertex order) gets the twin's PS4
buffers (vertex streams, index buffer, skinning) - the remastered mesh - and every material the twin has by name gets
the twin's own record (the converter writes a synthetic GUID name and PS3-case texture names), plus the twin textures
and GEOMETRY_1 texture entries those records bind. Submeshes and materials the twin lacks stay converted.

  python u2twin.py build <ps3 mp-hero.pak> <ps3 twin.pak> <ps4 twin.pak> <out.pak>
  python u2twin.py validate --ps4 <PS4 game> --ps3 <PS3 base> [--ps3-dlc <DLC>] --work <dir>
"""
import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import u2paktools  # noqa: E402

u2paktools.setup()
from ndpak import Pak  # noqa: E402
from ps4writer import PakWriter, Blob  # noqa: E402
import u2actor  # noqa: E402

# PS4 campaign actor whose mesh the MP skin uses (from the PS3 data: identical LOD0 index/vertex blocks), names only.
# The build checks every submesh again; a skin whose twin shares nothing goes the PS3 way.
TWINS = {
    'mp-hero': 'costume-hero-nd', 'mp-hero-year-1': 'costume-hero-nd', 'mp-hero-frosty': 'costume-hero-nd',
    'mp-hero-nd': 'costume-hero-nd', 'mp-hero-nd-frosty': 'costume-hero-nd',
    'mp-hero-chloe': 'costume-hero-chloe', 'mp-hero-chloe-heist': 'costume-hero-chloe',
    'mp-hero-chloe-variation': 'costume-hero-chloe', 'mp-hero-chloe-winter': 'costume-hero-chloe-winter',
    'mp-hero-dead-explorer': 'costume-hero-dead-explorer', 'mp-hero-divesuit': 'costume-hero-divesuit',
    'mp-hero-doughnut': 'costume-hero-doughnut', 'mp-hero-eddie': 'costume-hero-eddie',
    'mp-hero-elena': 'costume-hero-elena', 'mp-hero-elena-reporter': 'costume-hero-elena-reporter',
    'mp-hero-elena-winter': 'costume-hero-elena-winter', 'mp-hero-flynn': 'costume-hero-flynn',
    'mp-hero-flynn-frosty': 'costume-hero-flynn-winter', 'mp-hero-flynn-heist': 'costume-hero-flynn-heist',
    'mp-hero-flynn-winter': 'costume-hero-flynn-winter', 'mp-hero-heist': 'hero-heist', 'mp-hero-jeff': 'costume-hero-jeff',
    'mp-hero-lazarevic': 'costume-hero-lazarevic', 'mp-hero-lieutenant': 'costume-hero-lieutenant',
    'mp-hero-merc': 'costume-hero-merc', 'mp-hero-navarro': 'costume-hero-navarro', 'mp-hero-parka': 'hero-parka',
    'mp-hero-pirate': 'costume-hero-pirate', 'mp-hero-pirate-nd': 'costume-hero-pirate-nd',
    'mp-hero-red': 'costume-hero-red', 'mp-hero-rescuer': 'costume-hero-rescuer', 'mp-hero-roman': 'costume-hero-roman',
    'mp-hero-schafer': 'costume-hero-schafer', 'mp-hero-skeleton-1': 'costume-hero-skeleton-1',
    'mp-hero-skeleton-1-glow': 'costume-hero-skeleton-1', 'mp-hero-skeleton-1-platinum': 'costume-hero-skeleton-1',
    'mp-hero-skeleton-ghengis': 'costume-hero-skeleton-ghengis', 'mp-hero-skeleton-marco': 'costume-hero-skeleton-marco',
    'mp-hero-sol-med-b-1a': 'costume-hero-sol-med-b-1a', 'mp-hero-sol-med-b-1a-v2': 'costume-hero-sol-med-b-1a-v2',
    'mp-hero-sol-med-b-1b': 'costume-hero-sol-med-b-1b', 'mp-hero-sol-med-b-2a': 'costume-hero-sol-med-b-2a',
    'mp-hero-sol-med-b-2b': 'costume-hero-sol-med-b-2b', 'mp-hero-sol-med-b-3a': 'costume-hero-sol-med-b-3a',
    'mp-hero-sullivan': 'costume-hero-sullivan', 'mp-hero-sullivan-blue': 'costume-hero-sullivan',
    'mp-hero-sullivan-red': 'costume-hero-sullivan', 'mp-hero-sullivan-winter': 'costume-hero-sullivan',
    'mp-hero-udf': 'costume-hero-udf',
}
# a twin build is used when at least one LOD0 submesh comes from the PS4 twin: on the retail pairs every swapped
# submesh equals retail (index buffer, positions, UVs) except where retail's own cook split 2-8 more seam vertices,
# and the rest of the skin is the PS3 conversion it would be anyway
SUB = 0x88          # PS4 submesh record
PAGE_LIMIT = 0x1c0000


class NoTwin(Exception):
    pass


# ------------------------------------------------------------------ PS3 side: LOD0 blocks
def _lod0_fg(p):
    fgs = p.items_of('FOREGROUND_SECTION_2')
    plain = [i for i in fgs if not re.search(r'-lod\d+$', i.name)]
    return (plain or fgs)[0]


def ps3_lod0_blocks(p):
    """per block of the PS3 LOD0 prototype: Block (exact key = vertex count + triangles + skin; positions and UVs
    for a tolerance check; vertex-order-free digest; material; triangle count)"""
    import ps3geom
    mod = u2actor.module()
    it = _lod0_fg(p)
    pr = p.resolve(it.page, it.data_off)
    mats = mod.ps3_block_materials(p, pr)
    nsub = p.u16(p.pbase(pr[0]) + pr[1] + 6)
    tpg, tbl = p.resolve(pr[0], pr[1] + 0x14)
    out = []
    for k in range(nsub):
        r = p.resolve(tpg, tbl + 4 * k)
        b = ps3geom.decode_block(p, r[0], r[1])
        key = hashlib.md5(repr((b['nverts'], b['tris'], b.get('skin'))).encode()).hexdigest()
        out.append(Block(key, b['pos'], b.get('uv0'), loose_digest(b), mats[k], len(b['tris'])))
    return out


class Block:
    __slots__ = ('key', 'pos', 'uv', 'loose', 'mat', 'ntris')

    def __init__(self, key, pos, uv, loose, mat, ntris):
        self.key, self.pos, self.uv, self.loose, self.mat, self.ntris = key, pos, uv, loose, mat, ntris

    def same(self, o):
        """the same mesh block: identical triangles and skinning, positions within 1 mm and UVs within 0.002 (the
        PS3 cooks of an MP skin and its campaign twin quantise separately: sullivan differs by 2.5e-4 / 6e-4)"""
        import numpy as np
        if self.key != o.key or (self.uv is None) != (o.uv is None):
            return False
        if np.abs(self.pos - o.pos).max(initial=0) > 1e-3:
            return False
        return self.uv is None or np.abs(self.uv - o.uv).max(initial=0) <= 2e-3


def loose_digest(b):
    """the same surface whatever the vertex / triangle order of the export: the sorted triangles as (position, UV)
    corners rounded to 1e-4. PS3 MP skins and their campaign twins were often exported separately (other vertex
    order), while retail PS4 uses the campaign mesh for both (mp-hero-divesuit 13/14, navarro 12/12 submeshes)."""
    import numpy as np
    cols = [b['pos']] + ([b['uv0']] if 'uv0' in b else [])
    v = np.round(np.hstack(cols), 4)
    rows = [tuple(x) for x in v.tolist()]
    tris = sorted(tuple(sorted((rows[a], rows[c], rows[d]))) for a, c, d in b['tris'])
    return hashlib.md5(repr(tris).encode()).hexdigest()


# ------------------------------------------------------------------ PS4 side
def geometry(q):
    """GEOMETRY_1 of a PS4 pak: submesh records, prototypes, materials"""
    g = q.items_of('GEOMETRY_1')[0]
    pg, b = g.page, g.data_off
    nsub = q.pu32(pg, b + 8)
    sp, so = q.resolve(pg, b + 0x10)
    npro = q.pu32(pg, b + 0x18)
    lp, lo = q.resolve(pg, b + 0x20)
    subs = {}
    for i in range(nsub):
        o = so + i * SUB
        nm = q.pstr(*q.resolve(sp, o + 8))
        subs[nm] = dict(page=sp, off=o, nv=q.pu32(sp, o + 0x20), ni=q.pu32(sp, o + 0x24),
                        mat=q.pstr(*q.resolve(sp, o + 0x48)), first=int(re.search(r'Submesh(\d+)$', nm).group(1)))
    protos = {}
    for i in range(npro):
        pp = q.resolve(lp, lo + 8 * i)
        n = q.pu32(pp[0], pp[1] + 4)
        name = q.pstr(*q.resolve(pp[0], pp[1] + 0x10))
        snp = q.resolve(pp[0], pp[1] + 0x18)
        protos[name] = dict(page=pp[0], off=pp[1],
                            subs=[q.pstr(*q.resolve(snp[0], snp[1] + 0x10 * k)) for k in range(n)])
    nm_ = q.pu32(pg, b + 0x48)
    mp_, mo = q.resolve(pg, b + 0x50)
    mats = []
    for i in range(nm_):
        o = mo + i * 0x48
        mats.append(dict(page=mp_, off=o, name=q.pstr(*q.resolve(mp_, o))))
    fg = {}
    for it in q.items_of('FOREGROUND_SECTION_2'):
        base = q.pbase(it.page) + it.data_off
        fg[it.name] = dict(proto=q.cstr(base + 0x1c), lod=struct.unpack_from('<f', q.d, base + 0x18)[0])
    return dict(subs=subs, protos=protos, mats=mats, fg=fg)


def lod0_proto(geo):
    names = [n for n in geo['fg'] if not re.search(r'-lod\d+$', n)]
    return geo['fg'][names[0]]['proto']


def mat_key(name):
    return name.split(',')[0]


def groups(blocks, firsts):
    """block digests grouped as the PS4 submeshes (first block numbers) group them"""
    fs = sorted(firsts)
    out = {}
    for i, f in enumerate(fs):
        end = fs[i + 1] if i + 1 < len(fs) else len(blocks)
        out[f] = blocks[f:end]
    return out


# ------------------------------------------------------------------ raw pak editing
class Graph:
    """deep copies of pointer targets of a PS4 pak into new Blobs (one per source item, memoised)"""

    def __init__(self, q):
        self.q = q
        self.memo = {}
        self.items = {}
        for it in q.items:
            self.items.setdefault(it.page, []).append((it.data_off, it.data_off + it.size))
        for v in self.items.values():
            v.sort()
        self.fix = {}
        for a, b, off in q.fixups:
            self.fix.setdefault(a, {})[off] = b
        self.blobs = []

    def _region(self, pg, off):
        for lo, hi in self.items.get(pg, []):
            if lo <= off < hi or (lo == off == hi):
                return lo, hi
        raise ValueError('pointer target p%d+%#x is not inside an item' % (pg, off))

    def ptr(self, pg, off):
        """(Blob, offset in it) holding a copy of the data at pg:off"""
        lo, hi = self._region(pg, off)
        key = (pg, lo)
        if key not in self.memo:
            b = Blob('twin')
            b.raw(self.q.d[self.q.pbase(pg) + lo:self.q.pbase(pg) + hi])
            self.memo[key] = b
            self.blobs.append(b)
            for o, dp in sorted(self.fix.get(pg, {}).items()):
                if lo <= o < hi:
                    tb, toff = self.ptr(dp, self.q.pptr(pg, o))
                    b.relocs.append((o - lo, tb, toff))
        return self.memo[key], off - lo


def _pages_of(q):
    pages = [bytearray(q.d[po:po + ps]) for po, ps, _ in q.pages]
    fix = [dict() for _ in q.pages]
    for a, b, off in q.fixups:
        fix[a][off] = b
    return pages, fix


def fix_artgroup(L, pages, fix):
    """retail (and PS3) artgroups key FG sections as sid('geo.' + name); the converter keys U2 FG sections (PS3 type
    FOREGROUND_SECTION) as sid(name). Re-key and re-hash the buckets (linear probing). Returns the keys changed."""
    from ndpak import sid
    remap = {sid(it.name): sid('geo.' + it.name) for it in L.items_of('FOREGROUND_SECTION_2')}
    n = 0
    for it in L.items_of('ARTGROUP_DATA'):
        pg, b = it.page, it.data_off
        d = pages[pg]
        # bucket entries {u32 key, u32 0xaaaaaaaa, ptr}: nothing to do when no key is a converter FG key (a converter
        # that already writes the retail keys / layout)
        keys = [struct.unpack_from('<I', d, o)[0] for o in range(b, b + it.size - 15, 8)
                if struct.unpack_from('<I', d, o + 4)[0] == 0xaaaaaaaa and (o + 8) in fix[pg]]
        if not any(k in remap for k in keys) or fix[pg].get(b + 0x40) != pg:
            continue
        xo = struct.unpack_from('<Q', d, b + 0x40)[0]
        nb = struct.unpack_from('<I', d, xo + 0x1c)[0]
        tbl = struct.unpack_from('<Q', d, xo + 0x20)[0]
        ents = []
        for k in range(nb):
            o = tbl + 16 * k
            key = struct.unpack_from('<I', d, o)[0]
            if (o + 8) in fix[pg]:
                if key in remap:
                    n += 1
                ents.append((remap.get(key, key), struct.unpack_from('<Q', d, o + 8)[0]))
                del fix[pg][o + 8]
            struct.pack_into('<IIQ', d, o, 0, 0xaaaaaaaa, 0)
        slots = [None] * nb
        for key, ptr in ents:
            bi = key % nb
            while slots[bi] is not None:
                bi = (bi + 1) % nb
            slots[bi] = (key, ptr)
        for k, s in enumerate(slots):
            if s:
                o = tbl + 16 * k
                struct.pack_into('<IIQ', d, o, s[0], 0xaaaaaaaa, s[1])
                fix[pg][o + 8] = pg
    return n


def rekey_artgroup(path):
    """fix_artgroup on a converted pak in place (the PS3-path skins have the same converter keys); returns the
    number of keys changed (0: nothing written)"""
    q = Pak(path)
    pages, fix = _pages_of(q)
    n = fix_artgroup(q, pages, fix)
    if n:
        w = PakWriter()
        for i in range(len(pages)):
            w.raw_page(bytes(pages[i]), sorted(fix[i].items()))
        w.login_page = (q.hdr['loginPage'], q.hdr['loginOff'])
        w.vram_table_loc = (q.hdr['vramDescPage'], q.hdr['vramDescOff'])
        w.vram = q.d[q.vram_base:]
        data = w.build()
        with open(path, 'wb') as f:
            f.write(data)
    return n


def build(ps3_mp, ps3_twin, ps4_twin, out_path, work=None, log=None, use_loose=True):
    """the MP skin from the PS4 twin; returns notes (dict). Raises NoTwin when the twin shares no LOD0 submesh."""
    log = log or (lambda *_: None)
    work = work or os.path.dirname(os.path.abspath(out_path))
    src = u2actor.U2Pak(ps3_mp)
    tw3 = u2actor.U2Pak(ps3_twin)
    C = Pak(ps4_twin)
    if u2actor.skeleton(src) != u2actor.skeleton(C):
        raise NoTwin('skeleton %08x != twin %08x' % (u2actor.skeleton(src), u2actor.skeleton(C)))
    mp_blocks = ps3_lod0_blocks(src)
    tw_blocks = ps3_lod0_blocks(tw3)
    cg = geometry(C)
    c0 = cg['protos'][lod0_proto(cg)]['subs']
    c_first = {cg['subs'][n]['first']: n for n in c0}
    if max(c_first) >= len(tw_blocks):
        raise NoTwin('PS4 twin submeshes do not follow the PS3 twin blocks')
    tw_groups = groups(tw_blocks, c_first)
    # sanity: PS4 twin index counts = PS3 twin triangles x 3
    bad = [f for f, g in tw_groups.items() if cg['subs'][c_first[f]]['ni'] != 3 * sum(x.ntris for x in g)]
    if len(bad) > len(c_first) // 4:
        raise NoTwin('PS4 twin %s is not the PS3 twin mesh (%d of %d submeshes differ)' % (
            os.path.basename(ps4_twin), len(bad), len(c_first)))
    by_loose = {}
    for f, g in tw_groups.items():
        if f not in bad:
            by_loose.setdefault(tuple(sorted(x.loose for x in g)), f)

    # 1. the PS3 skin converted with the twin as template
    lpath = out_path + '.conv'
    u2actor.convert(ps3_mp, ps4_twin, lpath)
    L = Pak(lpath)
    lg = geometry(L)
    l0 = lg['protos'][lod0_proto(lg)]['subs']
    l_first = {lg['subs'][n]['first']: n for n in l0}
    mp_groups = groups(mp_blocks, l_first)
    swap = {}                                   # L submesh name -> C submesh name
    loose = 0
    for f, g in mp_groups.items():
        cf = next((tf for tf, tg in tw_groups.items() if tf not in bad and len(tg) == len(g) and
                   all(a.same(b) for a, b in zip(g, tg))), None)
        if cf is None and use_loose:
            cf = by_loose.get(tuple(sorted(x.loose for x in g)))
            loose += cf is not None
        if cf is not None and c_first[cf] not in swap.values():
            swap[l_first[f]] = c_first[cf]
    tris = sum(x.ntris for x in mp_blocks)
    tris_swapped = sum(sum(x.ntris for x in mp_groups[f]) for f in l_first if l_first[f] in swap)
    share = tris_swapped / float(tris or 1)
    cmats = {mat_key(m['name']): m for m in cg['mats']}
    notes = dict(twin=os.path.basename(ps4_twin)[:-4], submeshes=len(l0), swapped=len(swap), loose=loose,
                 share=round(share, 3),
                 materials=len(lg['mats']), twin_materials=sum(1 for m in lg['mats'] if mat_key(m['name']) in cmats))
    if not swap:
        os.remove(lpath)
        raise NoTwin('no LOD0 submesh of %s is in %s' % (os.path.basename(ps3_mp), notes['twin']))

    # 2. raw edit of L: LOD0 buffers from C, twin material names, LOD0 prototype words
    pages, fix = _pages_of(L)
    g = Graph(C)
    redirect = []                               # (page, off, blob, boff)

    def put_ptr(pg, off, blob, boff=0):
        fix[pg].pop(off, None)
        redirect.append((pg, off, blob, boff))

    for ln, cn in swap.items():
        ls, cs = lg['subs'][ln], cg['subs'][cn]
        lp_, lo_ = ls['page'], ls['off']
        cp_, co_ = cs['page'], cs['off']
        crec = C.d[C.pbase(cp_) + co_:C.pbase(cp_) + co_ + SUB]
        cfix = {o - co_: d for o, d in g.fix.get(cp_, {}).items() if co_ <= o < co_ + SUB}
        lfix = {o - lo_: d for o, d in fix[lp_].items() if lo_ <= o < lo_ + SUB}
        rec = pages[lp_]
        for o in range(0, SUB, 4):
            if o in (8, 0xc, 0x48, 0x4c):        # name, material: L's
                continue
            if any(x <= o < x + 8 for x in list(cfix) + list(lfix)):
                continue
            rec[lo_ + o:lo_ + o + 4] = crec[o:o + 4]
        for o in sorted(set(cfix) | set(lfix)):
            if o in (8, 0x48):
                continue
            if o in cfix:
                tb, toff = g.ptr(cfix[o], struct.unpack_from('<Q', crec, o)[0])
                put_ptr(lp_, lo_ + o, tb, toff)
            else:
                fix[lp_].pop(lo_ + o, None)
                rec[lo_ + o:lo_ + o + 8] = crec[o:o + 8]
    # LOD0 prototype words +0x30 (retail: 4,2 / 3,2 ...; the converter writes 2,1). +0x30 is the scope-3 cbuffer
    # capacity (u2actor.fix_cbuffer_capacity raises it again after the build)
    lpr = lg['protos'][lod0_proto(lg)]
    cpr = cg['protos'][lod0_proto(cg)]
    if len(swap) == len(l0):
        pages[lpr['page']][lpr['off'] + 0x30:lpr['off'] + 0x38] = \
            C.d[C.pbase(cpr['page']) + cpr['off'] + 0x30:C.pbase(cpr['page']) + cpr['off'] + 0x38]
    notes['artgroup_keys_fixed'] = fix_artgroup(L, pages, fix)
    # twin materials: the converter's record (template copy, synthetic GUID name, PS3-case texture names) is replaced
    # by the twin's own record (0x48 bytes + everything it points at); submeshes (+0x48) name it by that string
    MREC = 0x48
    import ps4actor
    cbinds = {m['name']: [x[2] for x in m['binds']] for m in ps4actor.tmpl_materials(C)}
    noff = {}
    bound = set()
    for m in lg['mats']:
        cm = cmats.get(mat_key(m['name']))
        if cm is None:
            continue
        bound.update(cbinds[cm['name']])
        crec = C.d[C.pbase(cm['page']) + cm['off']:C.pbase(cm['page']) + cm['off'] + MREC]
        cfix = {o - cm['off']: d for o, d in g.fix.get(cm['page'], {}).items() if cm['off'] <= o < cm['off'] + MREC}
        rec = pages[m['page']]
        for o in list(fix[m['page']]):
            if m['off'] <= o < m['off'] + MREC:
                del fix[m['page']][o]
        rec[m['off']:m['off'] + MREC] = crec
        for o, dp in cfix.items():
            tb, toff = g.ptr(dp, struct.unpack_from('<Q', crec, o)[0])
            put_ptr(m['page'], m['off'] + o, tb, toff)
            if o == 0:
                noff[m['name']] = (tb, toff)
    for n, s_ in lg['subs'].items():
        if s_['mat'] in noff:
            put_ptr(s_['page'], s_['off'] + 0x48, *noff[s_['mat']])
    # twin textures those materials bind that the converter did not carry over (its template lookup misses retail
    # names such as 'default-mask.tga/<hash>.ndb.error'): copied with their in-pak mips
    ltex = {t['path'] for t in L.textures()}
    lstem = {_stem(x) for x in ltex}
    add_tex = [t for t in C.textures() if _stem(t['path']) in bound and _stem(t['path']) not in lstem]
    notes['textures_added'] = len(add_tex)
    # GEOMETRY_1 texture entries (+0x38 count, +0x40 array of 0x28 {ptr full name, u32 hash, ...}): every material
    # binding's full name must be listed (the game asserts "Could not find texture"); the twin's entries for its
    # bindings are added to the converter's
    from ndpak import sid
    lgeo, cgeo = L.items_of('GEOMETRY_1')[0], C.items_of('GEOMETRY_1')[0]

    def tex_entries(q, it):
        n = q.pu32(it.page, it.data_off + 0x38)
        r = q.resolve(it.page, it.data_off + 0x40)
        return [(r[0], r[1] + 0x28 * k, q.pstr(*q.resolve(r[0], r[1] + 0x28 * k))) for k in range(n)] if r else []
    lent = tex_entries(L, lgeo)
    have = {sid(x[2]) for x in lent}
    want = {sid(f) for f in bound}
    cent = [e for e in tex_entries(C, cgeo) if sid(e[2]) in want and sid(e[2]) not in have]
    raw_refs = []
    if cent:
        arr = Blob('geotextures')
        for p_, o_, _n in lent:
            raw_refs.append((arr, arr.pos, p_))
            arr.relocs.append((arr.pos, None, L.pptr(p_, o_)))
            arr.u64(0)
            arr.raw(L.d[L.pbase(p_) + o_ + 8:L.pbase(p_) + o_ + 0x28])
        for p_, o_, _n in cent:
            tb, toff = g.ptr(*C.resolve(p_, o_))
            arr.ptr(tb, toff)
            arr.raw(C.d[C.pbase(p_) + o_ + 8:C.pbase(p_) + o_ + 0x28])
        g.blobs.append(arr)
        struct.pack_into('<Q', pages[lgeo.page], lgeo.data_off + 0x38, len(lent) + len(cent))
        put_ptr(lgeo.page, lgeo.data_off + 0x40, arr, 0)
    notes['geometry_textures_added'] = len(cent)
    notes = _write(L, C, g, pages, fix, redirect, add_tex, out_path, lpath, notes, raw_refs)
    # +0x30 = scope-3 cbuffer capacity: the twin's LOD0 value above is the retail one for the twin's materials,
    # but the skin also keeps converted materials, and LOD1-3 keep the converter's 2 (u2actor; PORT-RENDER.md)
    notes['cbuffer_capacity_raised'] = u2actor.fix_cbuffer_capacity(out_path)
    return notes


def _stem(path):
    return re.sub(r'(\.ndb)?(\.error)?$', '', path)


class _Ref:
    """a login / vram table target inside a raw page"""

    def __init__(self, page, off):
        self.page, self.off = page, off


def _write(L, C, g, pages, fix, redirect, add_tex, out_path, lpath, notes, raw_refs=()):
    """L's data pages (raw, edited), new pages with the copied twin data, then a rebuilt login / vram-table page"""
    lh = L.hdr
    last = len(pages) - 1
    assert lh['loginPage'] == last and lh['vramDescPage'] == last, 'converter layout changed (login page)'
    assert not any(d == last for f in fix[:last] for d in f.values()), 'pointer into the login page'
    vram = bytearray(L.d[L.vram_base:])
    w = PakWriter()
    raws = [w.raw_page(bytes(pages[i]), sorted(fix[i].items())) for i in range(last)]
    for pg_, off, blob, boff in redirect:
        raws[pg_].redirect(off, blob, boff)
    for blob, pos, pg_ in raw_refs:             # Blob pointers into L's raw pages
        blob.relocs = [(p_, raws[pg_] if p_ == pos and t_ is None else t_, o_) for p_, t_, o_ in blob.relocs]
    # new VRAM_DESC items (twin descriptor + its low mips appended to the VRAM data)
    new_desc = []
    for t in add_tex:
        it = C.item_at(t['page'], t['off'])
        base = C.pbase(it.page) + it.data_off
        db = Blob('vramdesc')
        db.raw(C.d[base:base + it.size])
        while len(vram) % 0x100:
            vram.append(0)
        struct.pack_into('<Q', db.b, 8, len(vram))
        vram += C.vram_bytes(t['vramOffset'], t['vramSize'])
        db.relocs.append((0x38, db, C.pptr(it.page, it.data_off + 0x38) - it.data_off))
        name = Blob('texname')
        name.cstr(t['path'].split(u2paktools.U2_COOK_PREFIX, 1)[-1])
        new_desc.append((t, db, name))
    # new pages
    blobs = [('RAW_DATA', b) for b in g.blobs]
    for t, db, name in new_desc:
        blobs += [('VRAM_DESC', db), ('RAW_DATA', name)]
    size = PAGE_LIMIT
    pg = None
    for ty, b in blobs:
        if size + len(b) + 0x40 > PAGE_LIMIT:
            pg = w.page()
            size = 0x200
        pg.item(ty, b)
        size += len(b) + 0x40
    li = L.item_at(lh['loginPage'], lh['loginOff'])
    lb = L.pbase(last) + li.data_off
    login = [(L.u32(lb + 8 + 8 * k), L.u32(lb + 12 + 8 * k)) for k in range(L.u32(lb))]
    targets = [_Ref(raws[p_], o_) for p_, o_ in login]
    if new_desc:
        # TEXTURE_TABLE rebuilt with the added textures (sorted by hash, as the converter writes it)
        if pg is None or size > PAGE_LIMIT - 0x4000:
            pg = w.page()
        tt = L.items_of('TEXTURE_TABLE')[0]
        tb = L.pbase(tt.page) + tt.data_off
        n = L.u64(tb)
        hp, np_ = L.resolve(tt.page, tt.data_off + 8), L.resolve(tt.page, tt.data_off + 0x10)
        ents = []
        for k in range(n):
            ents.append((L.u32(L.pbase(hp[0]) + hp[1] + 4 * k), (None, L.resolve(np_[0], np_[1] + 8 * k)),
                         (None, L.resolve(tt.page, tt.data_off + 0x20 + 8 * k))))
        for t, db, name in new_desc:
            ents.append((t['hash'], (name, None), (db, None)))
        ents.sort(key=lambda e: e[0])
        hb, nb_, tbb = Blob('tthash'), Blob('ttnames'), Blob('texturetable')
        for h, nm, de in ents:
            hb.u32(h)
            if nm[0] is not None:
                nb_.ptr(nm[0])
            else:
                nb_.ptr(raws[nm[1][0]], nm[1][1])
        hi, ni = pg.item('RAW_DATA', hb), pg.item('RAW_DATA', nb_)
        tbb.u64(len(ents)).ptr(hi.data).ptr(ni.data).ptr(tbb, 0x20)
        for h, nm, de in ents:
            if de[0] is not None:
                tbb.ptr(de[0])
            else:
                tbb.ptr(raws[de[1][0]], de[1][1])
        ttab = pg.item('TEXTURE_TABLE', tbb)
        # the old table stays as unreferenced bytes: its page entry is dropped so the pak lists one TEXTURE_TABLE
        _drop_entry(raws[tt.page].data, tt.off)
        targets = [ttab if L.item_at(p_, o_).type == 'TEXTURE_TABLE' else t_ for (p_, o_), t_ in zip(login, targets)]
    # login / vram table page (as the converter lays it out: unlisted PAK_LOGIN_TABLE + VRAM_DESC_TABLE)
    lp = w.page()
    lgb = Blob('login')
    lgb.u32(len(targets)).u32(0).raw(b'\0' * (8 * len(targets)))
    w.login_item = lp.item('PAK_LOGIN_TABLE', lgb, listed=False)
    w.login_targets = targets
    vt = L.vram_desc_table()
    vb = Blob('vramtable')
    vb.u32(len(vt) + len(new_desc)).u32(0)
    for p_, o_ in vt:
        vb.ptr(raws[p_], o_)
    for t, db, name in new_desc:
        vb.ptr(db, -0x20)
    w.vram_table_item = lp.item('VRAM_DESC_TABLE', vb, listed=False)
    w.vram = bytes(vram)
    data = w.build()
    with open(out_path, 'wb') as f:
        f.write(data)
    os.remove(lpath)
    errs = Pak(out_path).validate()
    if errs:
        raise RuntimeError('twin build: ' + '; '.join(errs[:3]))
    notes['size'] = len(data)
    return notes


def _drop_entry(page, item_off):
    """remove the page entry of the item at item_off (the last entry takes its slot)"""
    n = struct.unpack_from('<H', page, 0x12)[0]
    for j in range(n):
        if struct.unpack_from('<I', page, 0x14 + 16 * j + 8)[0] == item_off:
            page[0x14 + 16 * j:0x14 + 16 * j + 16] = page[0x14 + 16 * (n - 1):0x14 + 16 * n]
            page[0x14 + 16 * (n - 1):0x14 + 16 * n] = b'\0' * 16
            struct.pack_into('<H', page, 0x12, n - 1)
            return True
    return False


# ------------------------------------------------------------------ comparison with retail
def compare(built, retail):
    """what of a built skin matches the retail PS4 pak of the same name"""
    a, b = Pak(built), Pak(retail)
    ga, gb = geometry(a), geometry(b)

    def subdata(q, s):
        pg, o = s['page'], s['off']
        ns = q.pu32(pg, o + 0x28)
        st = q.resolve(pg, o + 0x30)
        streams = {}
        for k in range(ns):
            so = st[1] + 0x18 * k
            ln = q.pu32(st[0], so + 4)
            fm = q.resolve(st[0], so + 8)
            slot = q.d[q.pbase(fm[0]) + fm[1]]
            dr = q.resolve(st[0], so + 0x10)
            streams[slot] = q.d[q.pbase(dr[0]) + dr[1]:q.pbase(dr[0]) + dr[1] + ln]
        ib = q.resolve(pg, o + 0x40)
        idx = q.d[q.pbase(ib[0]) + ib[1]:q.pbase(ib[0]) + ib[1] + 2 * s['ni']]
        return streams, idx

    r = {}
    import numpy as np
    pa, pb = ga['protos'][lod0_proto(ga)]['subs'], gb['protos'][lod0_proto(gb)]['subs']
    free = list(pb)
    same_ib = same_pos = near_pos = same_uv = same_mat = same_n = paired = 0
    for n in pa:
        s = ga['subs'][n]
        # pair with the retail submesh of the same material and index count, else the same index count
        t = next((x for x in free if mat_key(gb['subs'][x]['mat']) == mat_key(s['mat']) and
                  gb['subs'][x]['ni'] == s['ni']), None) or             next((x for x in free if gb['subs'][x]['ni'] == s['ni']), None)
        if t is None:
            continue
        free.remove(t)
        paired += 1
        t = gb['subs'][t]
        same_mat += mat_key(s['mat']) == mat_key(t['mat'])
        same_n += s['nv'] == t['nv']
        sa, ia = subdata(a, s)
        sb, ib = subdata(b, t)
        same_ib += ia == ib
        same_pos += sa.get(0) == sb.get(0)
        same_uv += sa.get(1) == sb.get(1)
        if s['nv'] == t['nv'] and 0 in sa and 0 in sb:
            x = np.frombuffer(sa[0], '<f4').reshape(-1, 3)
            y = np.frombuffer(sb[0], '<f4').reshape(-1, 3)
            d = np.abs(x - y).max(initial=0)
            if d > 1e-3:                         # same vertices in another order
                d = np.abs(x[np.lexsort(np.round(x, 2).T)] - y[np.lexsort(np.round(y, 2).T)]).max(initial=0)
            near_pos += bool(d <= 1e-3)
    r['lod0'] = dict(submeshes='%d/%d' % (len(pa), len(pb)), paired=paired, material=same_mat, vertex_count=same_n,
                     index=same_ib, positions=same_pos, positions_1mm=near_pos, uv=same_uv)
    lods = {}
    for q, gg, tag in ((a, ga, 'built'), (b, gb, 'retail')):
        for fn, f in sorted(gg['fg'].items()):
            pr = gg['protos'][f['proto']]
            lods.setdefault(re.sub(r'^.*?(-lod\d+)?$', lambda m: m.group(1) or 'lod0', fn), {})[tag] = (
                len(pr['subs']), sum(gg['subs'][s]['ni'] for s in pr['subs']), f['lod'])
    r['lods'] = lods
    ma = {m['name'] for m in ga['mats']}
    mb = {m['name'] for m in gb['mats']}
    r['materials'] = dict(built=len(ma), retail=len(mb), same_name=len(ma & mb))

    def mrec(q, mats):
        import ps4actor
        return {m['name']: m for m in ps4actor.tmpl_materials(q)}
    ra, rb = mrec(a, ga), mrec(b, gb)
    r['materials']['same_record'] = sum(1 for k in ra if k in rb and ra[k] == rb[k])
    ta = {t['path']: (t['hash'], t['format'], t['width'], t['height'], t['mips'], t['unk34']) for t in a.textures()}
    tb = {t['path']: (t['hash'], t['format'], t['width'], t['height'], t['mips'], t['unk34']) for t in b.textures()}
    r['textures'] = dict(built=len(ta), retail=len(tb), same=sum(1 for k in ta if tb.get(k) == ta[k]))

    def tags(q):
        out = []
        for it in q.items:
            if it.type.startswith('TAG'):
                base = q.pbase(it.page) + it.data_off
                out.append((it.type, it.name, q.d[base:base + it.size]))
        return sorted(out)
    r['tags_equal'] = tags(a) == tags(b)
    r['fg_equal'] = sorted((k, round(v['lod'], 2)) for k, v in ga['fg'].items()) == \
        sorted((k, round(v['lod'], 2)) for k, v in gb['fg'].items())

    def artgroup_keys(q):
        """bucket keys: 16-byte {u32 key, u32 0xaaaaaaaa, ptr} entries (layout independent)"""
        it = q.items_of('ARTGROUP_DATA')[0]
        base = q.pbase(it.page) + it.data_off
        keys = []
        for o in range(0, it.size - 15, 8):
            k, pad = struct.unpack_from('<II', q.d, base + o)
            if k and pad == 0xaaaaaaaa and q.is_ptr(it.page, it.data_off + o + 8):
                keys.append(k)
        return sorted(keys), q.pu32(it.page, it.data_off), it.name
    ka, kb = artgroup_keys(a), artgroup_keys(b)
    r['artgroup_equal'] = ka == kb
    import collections
    r['types_equal'] = collections.Counter(i.type for i in a.items if i.type != 'RAW_DATA') == \
        collections.Counter(i.type for i in b.items if i.type != 'RAW_DATA')
    r['login_types'] = [a.item_at(p, o).type for p, o in a.login['entries']] == \
        [b.item_at(p, o).type for p, o in b.login['entries']]
    return r


def validate(ps3, ps4, work, log=print, check=None):
    """build the retail PS4 mp-hero skins that have a PS4 twin from their PS3 files and compare with retail"""
    import json
    os.makedirs(work, exist_ok=True)
    res = {}
    for name in sorted(TWINS):
        twin = TWINS[name]
        ret = os.path.join(ps4.actor_dir, name + '.pak')
        c4 = os.path.join(ps4.actor_dir, twin + '.pak')
        if not (os.path.isfile(ret) and os.path.isfile(c4)):
            continue
        paths = []
        for k in (name, twin):
            c = ps3.candidates('actor27/%s.pak' % k)
            p = os.path.join(work, 'ps3-' + k + '.pak')
            if c and not os.path.exists(p):
                with open(p, 'wb') as f:
                    f.write(c[0].read())
            paths.append(p)
        out = os.path.join(work, name + '.pak')
        try:
            notes = build(paths[0], paths[1], c4, out)
            r = dict(notes=notes, cmp=compare(out, ret))
            if check:
                r['ps4check'] = check(out)
        except NoTwin as e:
            r = dict(no_twin=str(e))
        res[name] = r
        log('%s %s' % (name, json.dumps(r)))
    return res


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd')
    b = sub.add_parser('build')
    b.add_argument('ps3_mp')
    b.add_argument('ps3_twin')
    b.add_argument('ps4_twin')
    b.add_argument('out')
    v = sub.add_parser('validate')
    v.add_argument('--ps4', required=True)
    v.add_argument('--ps3', required=True)
    v.add_argument('--ps3-dlc')
    v.add_argument('--work', required=True)
    v.add_argument('--retail', help='compare a built pak with this retail pak only (with build)')
    a = ap.parse_args()
    if a.cmd == 'build':
        print(build(a.ps3_mp, a.ps3_twin, a.ps4_twin, a.out))
    elif a.cmd == 'validate':
        import json
        import sources
        r = validate(sources.PS3(a.ps3, a.ps3_dlc, log=print), sources.PS4(a.ps4), a.work, check=u2actor.check)
        with open(os.path.join(a.work, 'twin-validation.json'), 'w') as f:
            json.dump(r, f, indent=1)


if __name__ == '__main__':
    main()
