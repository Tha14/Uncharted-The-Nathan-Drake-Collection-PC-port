"""Uncharted 2 actor pak pieces around the geometry: ARTGROUP_DATA (U2 layout), own skeleton, the items props carry
(JOINT_HIERARCHY, ANIM, COLLISION_DATA_FOREGROUND, SOUND_BANK_TABLE) and the login order.  u2actor hooks these into
the U3 ps4actor converter.

ARTGROUP_DATA (found on the 1124 PS3/PS4 actor27 pairs: 1109 rebuilt byte for byte, the rest are character bases):
  PS3  {u32 n, u32 1, ptr entries {ptr item, 0}[n], ptr H, ptr B, ...}; H {0, 1, 0x12345, ptr X};
       X {f32 -1, f32 1, 0, 0, u32 nb, 0, 0, buckets {u32 key, ptr entry}[nb] inline}
       B {ptr name, u32 cnt, ptr L}; L {u32 sid, u32 m, ptr M}[cnt]; M {u32 sid, f32, u32 k, ptr u32[2 (k-1)]}[m]
  PS4  {u32 n, u32 1, ptr entries (null when n = 0), ptr H, ptr B, u64 0, u32 0, aa} (0x30)
       H {u64 0, u32 1, u32 0x12345, ptr X, aa aa}; entries {ptr item header, u64 0}[n]
       B {ptr name, u32 cnt, aa, ptr L, aa aa}, name (16-aligned), L {u32 sid, u32 m, ptr M}[cnt], per L entry
       M {u32 sid, f32, u32 k - 1, aa, ptr data}[m] then each data (u32 words), X {0, aa, 0, 0, 0, nb},
       buckets {u32 key, aa, ptr entry}[nb] inline.
  Bucket keys: sid('geo.' + name) for FG sections, sid(name) for the other entries (also entries whose item PS4
  drops), inserted in entry order at key % nb with linear probing (PS3 lists no key for the first FG section).
  The U3 converter writes the U3 layout (separate bucket table, no B); U2 PS4 reads the table inline.
SOUND_BANK_TABLE  PS3 {u32 n, ptr name[n]}; PS4 {u32 n, aa, ptr name[n]} + the names.
Login order (retail props): ARTGROUP_DATA / LEVEL_PATS_1 (PS3 order), SOUND_BANK_TABLE, EFFECT_TABLE, TEXTURE_TABLE,
  GEOMETRY_1, JOINT_HIERARCHY, ANIM*, FOREGROUND_SECTION_2*, COLLISION_DATA_FOREGROUND*, PACKAGE_ITEM_LOOKUP, TAG_*.
"""
import struct

import u2paktools

u2paktools.setup()
from ndpak import sid  # noqa: E402
from ps4writer import Blob  # noqa: E402

PAD32 = 0xaaaaaaaa
FG_TYPES = ('FOREGROUND_SECTION', 'FOREGROUND_SECTION_2')


# ------------------------------------------------------------------ ARTGROUP_DATA
def read_artgroup(p):
    """U2 PS3 ARTGROUP_DATA -> (name, targets [Item | None], nb, attach lists)"""
    it = p.items_of('ARTGROUP_DATA')[0]
    pg, b = it.page, it.data_off
    n = p.pu32(pg, b)
    if p.pu32(pg, b + 4) != 1:
        raise NotImplementedError('ARTGROUP_DATA +4 != 1')
    ents = p.resolve(pg, b + 8)
    targets = []
    for k in range(n):
        r = p.resolve(ents[0], ents[1] + 8 * k)
        targets.append(p.item_at(*r) if r else None)
    h2 = p.resolve(pg, b + 0xc)
    x = p.resolve(h2[0], h2[1] + 0xc)
    nb = p.pu32(x[0], x[1] + 0x10)
    lists = []
    bb = p.resolve(pg, b + 0x10)
    if bb:
        lr = p.resolve(bb[0], bb[1] + 8)
        for i in range(p.pu32(bb[0], bb[1] + 4)):
            s, m = p.pu32(lr[0], lr[1] + 12 * i), p.pu32(lr[0], lr[1] + 12 * i + 4)
            mr = p.resolve(lr[0], lr[1] + 12 * i + 8)
            ms = []
            for j in range(m):
                o = p.pbase(mr[0]) + mr[1] + 16 * j
                s2, k = struct.unpack_from('>II', p.d, o)[0], struct.unpack_from('>I', p.d, o + 8)[0]
                k4 = max(k - 1, 0)
                dr = p.resolve(mr[0], mr[1] + 16 * j + 12)
                d = p.d[p.pbase(dr[0]) + dr[1]:][:8 * k4] if (dr and k4) else b''
                ms.append((s2, p.d[o + 4:o + 8][::-1], k4, b''.join(d[q:q + 4][::-1] for q in range(0, len(d), 4))))
            lists.append((s, ms))
    return it.name, targets, nb, lists


def keys_for(targets, ren=lambda s: s):
    return [None if t is None else sid(('geo.' + ren(t.name)) if t.type in FG_TYPES else ren(t.name))
            for t in targets]


def hash_buckets(keys, nb):
    bk = [(0, None)] * nb
    used = [False] * nb
    for k, key in enumerate(keys):
        if key is None:
            continue
        i = key % nb
        while used[i]:
            i = (i + 1) % nb
        used[i] = True
        bk[i] = (key, k)
    return bk


def build_artgroup(name, entries, keys, nb, lists):
    """PS4 ARTGROUP_DATA blob; entries: PS4 items (their headers are referenced) or None"""
    n = len(entries)
    bk = hash_buckets(keys, nb)
    ab = Blob('artgroup')
    fix = []
    ents, nameblk = 0x50, 0x50 + 16 * n
    ab.u32(n).u32(1).u64(0).ptr(ab, 0x30).ptr(ab, nameblk).u64(0).u32(0).u32(PAD32)
    if n:
        fix.append((8, ents))
    ab.u64(0).u32(1).u32(0x12345).u64(0).u32(PAD32).u32(PAD32)
    for e in entries:
        if e is not None:
            ab.ptr(e.data, -0x20).u64(0)
        else:
            ab.u64(0).u64(0)
    ab.u64(0).u32(len(lists)).u32(PAD32).u64(0).u32(PAD32).u32(PAD32)
    fix.append((nameblk, ab.pos))
    ab.cstr(name)
    ab.pad(16, 0xaa)
    if lists:
        fix.append((nameblk + 0x10, ab.pos))
        lo = ab.pos
        for s, ms in lists:
            ab.u32(s).u32(len(ms)).u64(0)
        for i, (s, ms) in enumerate(lists):
            ab.pad(16, 0xaa)
            fix.append((lo + 16 * i + 8, ab.pos))
            mo = ab.pos
            for s2, f, k4, d in ms:
                ab.u32(s2).raw(f).u32(k4).u32(PAD32).u64(0)
            for j, (s2, f, k4, d) in enumerate(ms):
                if k4:
                    ab.pad(16, 0xaa)
                    fix.append((mo + 24 * j + 16, ab.pos))
                    ab.raw(d)
        ab.pad(16, 0xaa)
    x = ab.pos
    fix.append((0x40, x))
    ab.u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb)
    for key, ei in bk:
        ab.u32(key).u32(PAD32)
        if ei is None:
            ab.u64(0)
        else:
            ab.ptr(ab, ents + 16 * ei)
    for pos, t in fix:
        ab.relocs.append((pos, ab, t))
    return ab


# ------------------------------------------------------------------ own skeleton, extra items
def skeleton(src, tmpl_info):
    """(skeleton sid, hierarchy id, joint-name table, joint count) for the FG sections: a prop with its own
    JOINT_HIERARCHY uses it (retail: FG +8 = PS3 FG +8, +0xc = skeleton blob +4, names = the joint sids); otherwise
    the template's (skins: the shared MP hero skeleton)"""
    jh = src.items_of('JOINT_HIERARCHY')
    if not jh:
        return tmpl_info
    import u2skel
    m = u2skel.read_jh(src, jh[0])
    fg = [i for i in src.items if i.type in FG_TYPES][0]
    hier = struct.unpack_from('>I', m['blob'], 4)[0]
    names = struct.pack('<%dI' % m['nj'], *[s for s, _n in m['names']])
    return src.pu32(fg.page, fg.data_off + 8), hier, names, m['nj']


def sound_bank_blob(p, it):
    n = p.pu32(it.page, it.data_off)
    names = [p.pstr(*p.resolve(it.page, it.data_off + 4 + 4 * k)) for k in range(n)]
    b = Blob('soundbanks')
    b.u32(n).u32(PAD32)
    tab = b.pos
    b.raw(b'\0' * (8 * n))
    for k, nm in enumerate(names):
        b.relocs.append((tab + 8 * k, b, b.pos))
        b.cstr(nm)
    return b, names


def extra_items(src, page, targets, ren=lambda s: s):
    """convert the items a prop carries besides its geometry; returns {(type, ps4 name): item} and the list of
    (type, item) in creation order"""
    import u2anim
    import u2actorcoll
    import u2skel
    made = {}
    order = []
    sbt = src.items_of('SOUND_BANK_TABLE')
    if sbt:
        blob, _names = sound_bank_blob(src, sbt[0])
        it = page.item('SOUND_BANK_TABLE', blob)
        order.append(('SOUND_BANK_TABLE', it))
    for t in targets:
        if t is None or t.type not in ('JOINT_HIERARCHY', 'ANIM', 'COLLISION_DATA_FOREGROUND', 'COLLISION_HAVOK_JOINTED'):
            continue
        if t.type == 'JOINT_HIERARCHY':
            blob = u2skel.jh_blob(src, t)
        elif t.type == 'ANIM':
            a = u2anim.read_ps3_anim(src, t)
            if a['clip'] is not None and u2anim._anim.is_streamed(a['clip']):
                raise NotImplementedError('streamed clip %s' % t.name)
            blob = u2anim.build_ps4_anim(a)
        elif t.type == 'COLLISION_HAVOK_JOINTED':
            blob = u2actorcoll.havok_jointed_blob(src, t)
        else:
            blob = u2actorcoll.fg_blob(src, t)
        it = page.item(t.type, blob, name=ren(t.name))
        made[(t.type, t.name)] = it
        order.append((t.type, it))
    for t in src.items_of('COLLISION_HAVOK_JOINTED'):        # loose ragdolls: logged in, not in the artgroup
        if ('COLLISION_HAVOK_JOINTED', t.name) not in made:
            it = page.item(t.type, u2actorcoll.havok_jointed_blob(src, t), name=ren(t.name))
            made[(t.type, t.name)] = it
            order.append((t.type, it))
    return made, order


def artgroup_entries(targets, made):
    return [made.get((t.type, t.name)) if t is not None else None for t in targets]


LOGIN_ORDER = ['SOUND_BANK_TABLE', 'EFFECT_TABLE', 'TEXTURE_TABLE', 'GEOMETRY_1', 'JOINT_HIERARCHY', 'ANIM',
               'FOREGROUND_SECTION_2', 'COLLISION_DATA_FOREGROUND', 'COLLISION_HAVOK_JOINTED', 'PACKAGE_ITEM_LOOKUP']


def login_order(src, by_type, tag_items):
    """retail U2 login order: ARTGROUP_DATA / LEVEL_PATS_1 in the PS3 order, then LOGIN_ORDER, then the tags"""
    first = []
    for pg, off in src.login['entries']:
        t = src.item_at(pg, off).type
        if t in ('ARTGROUP_DATA', 'LEVEL_PATS_1') and t not in first:
            first.append(t)
    for t in ('ARTGROUP_DATA', 'LEVEL_PATS_1'):
        if t not in first:
            first.append(t)
    out = []
    for t in first + LOGIN_ORDER:
        out += by_type.get(t, [])
    return out + list(tag_items)


# ------------------------------------------------------------------ LEVEL_PATS_1, PACKAGE_ITEM_LOOKUP, names
def level_pats(src):
    """LEVEL_PATS_1: the PS3 u64 (big-endian) as little-endian (retail keeps the PS3 value; U3 writes 0)"""
    it = src.items_of('LEVEL_PATS_1')
    return src.d[src.pbase(it[0].page) + it[0].data_off:][:8][::-1] if it else bytes(8)


def lookup_blob(art, artgroup):
    """PACKAGE_ITEM_LOOKUP, U2 layout: {ptr name, u32 0, u32 n, {u32 sid, aa, ptr item header}[n] inline, name}
    (U3: a pointer to the entries after the 16-aligned name)"""
    pl = Blob('pkglookup')
    pl.ptr(pl, 0x20).u32(0).u32(1)
    pl.u32(sid(art)).u32(PAD32).ptr(artgroup.data, -0x20)
    pl.cstr(art)
    return pl


def proto_name(pi, art):
    return 'Prototype%d_%s' % (pi, art.replace('-', '_'))


def submesh_name(art, pi, first):
    return '%s_Prototype%d_Submesh%d' % (art.replace('-', '_'), pi, first)
