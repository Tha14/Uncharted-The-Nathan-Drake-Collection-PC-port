"""Uncharted 2 PS3 foreground actor (skins, props) -> PS4, built on the U3 converter paktools/ps4actor.py.

U2 PS3 actors carry the same prototype / geometry-block format as U3 (decoded by ps3geom), but under older item
names (found on the PS3/PS4 pairs mp-hero-elena, mp-hero-merc, ...):
  U3 PS3 FOREGROUND_SECTION_2 {ptr prototype, ...}   = U2 PS3 FOREGROUND_SECTION (0x10: ptr prototype, 0, sid, f32 lod)
  U3 PS3 PM_FG_PROTOTYPES {n, ptr prototype[n]}      = U2 PS3 unnamed PM_COLLECTION_2 items {1, ptr prototype}, one
                                                       per prototype (named 'mesh.collision.*' ones are collision)
The PS4 output uses the PS4 names (FOREGROUND_SECTION_2, GEOMETRY_1) as retail U2 PS4 does.
ps4actor.convert is loaded from its own source with the PM_FG_PROTOTYPES read replaced (asserted to match), so
the U3 file stays untouched; everything else (texture tiling, materials from a PS4 template, skinning) is U3's.
"""
import os
import sys
import types

import u2paktools

u2paktools.setup()
from ndpak import Pak  # noqa: E402

_OLD = """    pm = src.items_of('PM_FG_PROTOTYPES')[0]
    for k in range(src.pu32(pm.page, pm.data_off)):
        proto_list.append(src.resolve(pm.page, pm.data_off + 4 + 4 * k))
"""
_NEW = """    for pm in src.u2_fg_collections():
        proto_list.append(src.resolve(pm.page, pm.data_off + 4))
"""
# retail U2 PS4 skins carry the second UV set (slot 12, half2) and vertex colour (slot 4, half4) where the PS3 blocks
# have them (attributes 0x12 / 0x04); the U3 converter writes slots 0-3 only
_EDITS = [
    # submesh flags: U3 sets 0x10000 on all but the last LOD; every retail U2 PS4 skin submesh has 0
    ("""            flags = 0x01000100 if sb else (0 if last_lod else 0x00010000)""",
     """            flags = 0x01000100 if sb else 0"""),
    ("""    FMT = {0: (0, 0, 0x0c, 0x06), 1: (1, 0, 0x04, 0x22), 2: (2, 0, 0x08, 0x0a), 3: (3, 0, 0x08, 0x0a)}""",
     """    FMT = {0: (0, 0, 0x0c, 0x06), 1: (1, 0, 0x04, 0x22), 2: (2, 0, 0x08, 0x0a), 3: (3, 0, 0x08, 0x0a),
           12: (12, 0, 0x04, 0x22), 4: (4, 0, 0x08, 0x0a)}"""),
    ("""                streams.append((1, 4, f16(uv).astype('<f2').tobytes()))
""",
     """                streams.append((1, 4, f16(uv).astype('<f2').tobytes()))
            if all('uv1' in b for b in s['blocks']):
                uv = np.concatenate([b['uv1'] for b in s['blocks']])
                streams.append((12, 4, f16(uv).astype('<f2').tobytes()))
"""),
    ("""                streams.append((3, 8, f16(tg).astype('<f2').tobytes()))
""",
     """                streams.append((3, 8, f16(tg).astype('<f2').tobytes()))
            if all('color0' in b for b in s['blocks']):
                c = np.concatenate([b['color0'] for b in s['blocks']])
                c = (c + 1.0) / 2.0 if c.min() < 0 else c
                if c.shape[1] == 3:
                    c = np.concatenate([c, np.ones((nv, 1))], 1)
                streams.append((4, 8, f16(c).astype('<f2').tobytes()))
"""),
]


class U2Pak(Pak):
    """a U2 PS3 pak seen with U3 item names.  U2 FOREGROUND_SECTION / PM_COLLECTION_2 point at a 0x20-byte LOD
    record {ptr prototype, ptr (zeros), 0, 2, ...}; U3 points at the prototype itself, so resolving those two
    pointer slots returns the inner prototype."""

    def _parse(self):
        super()._parse()
        self._u2_inner = {}
        if self.platform != 'ps3':
            return
        for i in self.items:
            slot = None
            if i.type == 'FOREGROUND_SECTION':
                slot = (i.page, i.data_off)
            elif i.type == 'PM_COLLECTION_2' and not i.name and self.pu32(i.page, i.data_off) == 1:
                slot = (i.page, i.data_off + 4)
            if slot is None:
                continue
            rec = Pak.resolve(self, *slot)
            inner = Pak.resolve(self, *rec) if rec else None
            if inner is not None:
                self._u2_inner[slot] = inner

    def resolve(self, page, off):
        r = self._u2_inner.get((page, off)) if hasattr(self, '_u2_inner') else None
        return r if r is not None else Pak.resolve(self, page, off)

    def items_of(self, t):
        if t == 'FOREGROUND_SECTION_2' and self.platform == 'ps3':
            return [i for i in self.items if i.type in ('FOREGROUND_SECTION', 'FOREGROUND_SECTION_2')]
        return super().items_of(t)

    def u2_fg_collections(self):
        """unnamed PM_COLLECTION_2 {u32 1, ptr prototype} items that point at a FOREGROUND_SECTION prototype"""
        fg = {self.resolve(i.page, i.data_off) for i in self.items_of('FOREGROUND_SECTION_2')}
        out = []
        for i in self.items:
            if i.type == 'PM_COLLECTION_2' and not i.name and self.pu32(i.page, i.data_off) == 1:
                if self.resolve(i.page, i.data_off + 4) in fg:
                    out.append(i)
        return out


def _load():
    path = os.path.join(u2paktools.PAKTOOLS, 'ps4actor.py')
    src = open(path, encoding='utf-8').read()
    if src.count(_OLD) != 1:
        raise RuntimeError('ps4actor.py changed: the PM_FG_PROTOTYPES read was not found once')
    src = src.replace(_OLD, _NEW)
    for old, new in _EDITS:
        if src.count(old) != 1:
            raise RuntimeError('ps4actor.py changed: an edit point was not found once')
        src = src.replace(old, new)
    mod = types.ModuleType('u2_ps4actor')
    mod.__file__ = path
    exec(compile(src, path, 'exec'), mod.__dict__)
    mod.PS4_COOK_PREFIX = u2paktools.U2_COOK_PREFIX
    mod.Pak = U2Pak
    return mod


_mod = None


def module():
    global _mod
    if _mod is None:
        _mod = _load()
    return _mod


def convert(ps3_path, tmpl_path, out_path, **kw):
    """ps4actor.convert for a U2 PS3 actor pak; tmpl_path = a retail PS4 actor27 pak with a similar skeleton"""
    kw.setdefault('verbose', False)
    return module().convert(ps3_path, tmpl_path, out_path, **kw)


# ------------------------------------------------------------------ what the converter takes, templates, checks
SKIN_TYPES = {'ARTGROUP_DATA', 'FOREGROUND_SECTION', 'LEVEL_PATS_1', 'MATERIAL_TABLE_1', 'PACKAGE_ITEM_LOOKUP',
              'PAK_LOGIN_TABLE', 'PM_COLLECTION_2', 'TAG_FLOAT', 'TAG_INT', 'TAG_VEC4', 'VRAM_DESC', 'VRAM_DESC_TABLE'}
WHY = {'JOINT_HIERARCHY': 'own skeleton (JOINT_HIERARCHY)', 'SKELETON_FLIPDATA': 'own skeleton (SKELETON_FLIPDATA)',
       'ANIM': 'animations (U2 PS3 ANIM)', 'ANIM_RAWCHANNELS': 'animations (U2 PS3 ANIM)',
       'COLLISION_DATA_FOREGROUND': 'collision (COLLISION_DATA_FOREGROUND)',
       'COLLISION_DATA_BACKGROUND': 'collision (COLLISION_DATA_BACKGROUND)',
       'COLLISION_HAVOK_JOINTED': 'Havok jointed collision', 'SOUND_BANK_TABLE': 'SOUND_BANK_TABLE'}


def unsupported(p):
    """why a U2 PS3 actor pak is outside what this converter reproduces ('' when it is a plain skinned mesh)"""
    types = {i.type for i in p.items}
    if 'FOREGROUND_SECTION' not in types:
        return 'no FOREGROUND_SECTION (items: %s)' % ', '.join(sorted(types))
    extra = sorted(types - SKIN_TYPES)
    if extra:
        return 'unsupported items: ' + ', '.join(sorted({WHY.get(t, t) for t in extra}))
    return ''


def skeleton(p):
    """skeleton sid of the first FG section (PS3 FOREGROUND_SECTION +8, PS4 FOREGROUND_SECTION_2 +8)"""
    import struct
    if p.platform == 'ps3':
        it = [i for i in p.items if i.type in ('FOREGROUND_SECTION', 'FOREGROUND_SECTION_2')][0]
        return p.pu32(it.page, it.data_off + 8)
    it = p.items_of('FOREGROUND_SECTION_2')[0]
    return struct.unpack_from('<I', p.d, p.pbase(it.page) + it.data_off + 8)[0]


def ps3_material_names(p):
    names = set()
    u = U2Pak(p.d) if not isinstance(p, U2Pak) else p
    mod = module()
    for pr in [u.resolve(i.page, i.data_off + 4) for i in u.u2_fg_collections()]:
        try:
            names.update(mod.ps3_block_materials(u, pr))
        except Exception:
            pass
    return names


_TCACHE = {}


def template_index(actor_dir):
    """retail PS4 actor27 paks with an FG section: {name: (skeleton sid, material keys, size)} (cached per dir)"""
    if actor_dir in _TCACHE:
        return _TCACHE[actor_dir]
    out = {}
    for f in sorted(os.listdir(actor_dir)):
        if not (f.startswith('mp-hero') and f.endswith('.pak')):
            continue
        try:
            q = Pak(os.path.join(actor_dir, f))
            if not q.items_of('FOREGROUND_SECTION_2'):
                continue
            mats = {m['name'].split(',')[0] for m in module().tmpl_materials(q)}
            out[f[:-4]] = (skeleton(q), mats, len(q.d))
        except Exception:
            pass
    _TCACHE[actor_dir] = out
    return out


def choose_template(ps3_path, actor_dir, exclude=()):
    """the retail PS4 mp-hero pak with the same skeleton that shares most material names with the PS3 actor
    (PS3 'dir/file/variant' = PS4 'dir/file:variant'); ties: longest common name prefix, then smaller pak"""
    src = U2Pak(ps3_path)
    sk = skeleton(src)
    keys = {n.rsplit('/', 1)[0] + ':' + n.rsplit('/', 1)[1] for n in ps3_material_names(src) if '/' in n}
    base = os.path.basename(ps3_path)[:-4]
    best = None
    for name, (s4, mats, size) in template_index(actor_dir).items():
        if s4 != sk or name in exclude:
            continue
        pre = len(os.path.commonprefix([name, base]))
        score = (len(keys & mats), pre, -size)
        if best is None or score > best[0]:
            best = (score, name)
    if best is None:
        raise RuntimeError('no retail PS4 actor with skeleton %08x' % sk)
    return os.path.join(actor_dir, best[1] + '.pak'), best[0][0], len(keys)


def check(path):
    """ps4check problems that retail U2 PS4 skins do not show too ([] = fine)"""
    import ps4check
    errs, info, _ = ps4check.check(path, quiet=True)
    return [e for e in errs if not e.startswith('unknown item type') and not
            (e.startswith('tiled size') and '-env-reflection' in e)]
