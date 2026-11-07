"""Menu images the PS4 net.pak lacks, from the PS3 net.pak: a small PS4 package 'net-u2mp.pak'.

The PS4 Collection's net.pak (251 materials) dropped what only the PS3 online menus used: the
Leaderboards / Trophies mode buttons (general/net/mode-buttons/leaderboards, .../trophies), the DLC map
photos and a few more. The PS3 net.pak (newest copy: dlc-patch7) has them. Only the materials whose
PS3 id (sid of the '/' name) is not already a PS4 material are converted, with the textures they use;
everything the PS4 net.pak has stays the PS4 one (the import rule).

Package format: the Uncharted 3 net converter (paktools/convert_net.py: TEXTURE_TABLE, VRAM_DESCs,
the effect of the PS4 menu.pak, GEOMETRY_1 materials named with the PS3 names). U2 differs only in the
PS3 MATERIAL_TABLE_1 (two tables, one per section; texture hashes in a shared array on another page;
no texture name array: names come from the VRAM_DESCs), read here.

In game, gow3_pc/src/u2mp/menuimages.cpp requests the package 'net-u2mp' next to 'net' and its image
lookup falls back to this package's material table.
"""
import os

import u2paktools

u2paktools.setup()
from ndpak import Pak, sid  # noqa: E402

OUT_NAME = 'net-u2mp.pak'
VERSION = 3      # bump when the materials or their formats change: the import converts again
# PS3 images taken even though PS4 has its own: the PS4 Collection redrew the Multiplayer page's mode
# buttons (other style, larger); next to the PS3-only Leaderboards / Trophies buttons the set looked
# mixed. The user asked for the PS3 look there. The runtime looks net-u2mp up first, so these win.
PREFER_PS3 = {'general/net/mode-buttons/matchmaking', 'general/net/mode-buttons/customgame',
              'general/net/mode-buttons/cinema'}


def read_materials_u2(p):
    """(materials, textures) as paktools/ps3mat.read_materials returns them, for U2 PS3 paks"""
    assert p.platform == 'ps3'
    vram = {t['hash']: t for t in p.textures()}
    out, hashes, texh = [], [], None
    for it in p.items_of('MATERIAL_TABLE_1'):
        pg, b = it.page, it.data_off
        if p.pu32(pg, b) != 0x79:
            raise ValueError('unsupported MATERIAL_TABLE_1 version %#x' % p.pu32(pg, b))
        nmat = p.pu32(pg, b + 0xc)
        th = p.resolve(pg, b + 0x18)
        ma = p.resolve(pg, b + 0x20)
        if texh is None:
            texh = th
        elif th != texh:
            raise ValueError('material tables with different texture hash arrays')
        for i in range(nmat):
            mp, mo = p.resolve(ma[0], ma[1] + 4 * i)
            name = p.pstr(*p.resolve(mp, mo))
            if p.pu32(mp, mo + 0x18) != sid(name):
                raise ValueError('material id mismatch: %s' % name)
            rec = dict(name=name, sid=sid(name), textures=[])
            r = p.resolve(mp, mo + 0x10)
            node = p.resolve(*r) if r else None
            bb = p.resolve(node[0], node[1] + 0x10) if node else None
            if bb:
                for k in range(p.pu32(bb[0], bb[1] + 4)):
                    rr = p.resolve(bb[0], bb[1] + 0x50 + 0x10 * k)
                    ok = rr and rr[0] == texh[0] and rr[1] >= texh[1] and (rr[1] - texh[1]) % 4 == 0
                    rec['textures'].append((rr[1] - texh[1]) // 4 if ok else None)
            out.append(rec)
    n = max([t for m in out for t in m['textures'] if t is not None] + [-1]) + 1
    for i in range(n):
        h = p.pu32(texh[0], texh[1] + 4 * i)
        t = vram.get(h)
        hashes.append(dict(hash=h, name=t['path'] if t else '<%08x>' % h))
    return out, hashes


def ps4_material_ids(ps4_net):
    """ids of the PS4 net.pak materials, as the menus ask for them (PS3 form: ':' read as '/')"""
    q = Pak(ps4_net)
    g = q.items_of('GEOMETRY_1')[0]
    pg, b = g.page, g.data_off
    n = q.pu32(pg, b + 0x48)
    mo = q.resolve(pg, b + 0x50)[1]
    ids = set()
    for i in range(n):
        nm = q.pstr(pg, q.resolve(pg, mo + i * 0x48)[1]).split(',')[0]
        ids.add(sid(nm.replace(':', '/')))
    return ids


def missing(ps3_net, ps4_net):
    mats, tex = read_materials_u2(Pak(ps3_net))
    have = ps4_material_ids(ps4_net)
    return [m for m in mats if (m['sid'] not in have or m['name'] in PREFER_PS3) and
            any(t is not None for t in m['textures'])], tex


# The U3 converter takes one effect for every material (from the PS4 menu.pak). U2's net materials use
# several (same shader hud/textured-2d.fx, one effect per image set): the reference is the PS4 net.pak
# itself and each material gets the effect of a PS4 material of its set.
_CONVERT_EDITS = [
    ("    shader, effect_hash, eparams = effect_hash_for(ref, EFFECTS[effect])\n",
     "    shader, effect_hash, eparams = effect_hash_for(ref, EFFECTS.get(effect, effect))\n"),
    ("        matb.ptr(strs, S(effect_hash)).u64(0).ptr(bindb, boff)\n",
     "        matb.ptr(strs, S(MATERIAL_EFFECT.get(m['name'], effect_hash))).u64(0).ptr(bindb, boff)\n"),
]


def _converter():
    cn = u2paktools._load_patched('convert_net', _CONVERT_EDITS, folder='')
    cn.PS4_COOK_PREFIX = u2paktools.U2_COOK_PREFIX
    return cn


def ps4_effects(ps4_net):
    """{set: (PS4 material name, effect hash)}, the most used effect of each PS4 net image set"""
    from collections import Counter
    q = Pak(ps4_net)
    g = q.items_of('GEOMETRY_1')[0]
    pg, b = g.page, g.data_off
    mo = q.resolve(pg, b + 0x50)[1]
    count, first = Counter(), {}
    for i in range(q.pu32(pg, b + 0x48)):
        m = mo + i * 0x48
        nm = q.pstr(pg, q.resolve(pg, m)[1]).split(',')[0]
        eh = q.pstr(pg, q.resolve(pg, m + 0x18)[1])
        key = (nm.split(':')[0], eh)
        count[key] += 1
        first.setdefault(key, nm)
    out = {}
    for (st, eh), _ in count.most_common():
        out.setdefault(st, (first[(st, eh)], eh))
    return out


def convert(ps3_net, ps4_net, out, log=print):
    """write out (net-u2mp.pak) with the PS3 net materials PS4 lacks; returns their names"""
    convert_net = _converter()
    keep, tex = missing(ps3_net, ps4_net)
    if not keep:
        log('net images: the PS4 net.pak has every PS3 net material')
        return []
    effects = ps4_effects(ps4_net)
    default = effects.get('general/net/player-icons') or next(iter(effects.values()))
    convert_net.MATERIAL_EFFECT = {m['name']: effects.get(m['name'].rsplit('/', 1)[0], default)[1] for m in keep}
    # the kept materials' textures only, renumbered
    used = sorted({t for m in keep for t in m['textures'] if t is not None})
    remap = {old: new for new, old in enumerate(used)}
    mats = [dict(m, textures=[remap.get(t) if t is not None else None for t in m['textures']]) for m in keep]
    mtex = [dict(tex[i]) for i in used]
    want = {t['hash'] for t in mtex}
    # A few PS3 descriptors (the DLC map photo placeholders 'maps/dig-*') share one path under several
    # hashes; the converter needs hash == sid(name), so those get a unique name (hash appended) and its id.
    rename = {}
    for t in Pak(ps3_net).textures():
        short = t['path'].split(convert_net.PS3_COOK_MARK, 1)[-1]
        if t['hash'] in want and sid(short) != t['hash']:
            new = short[:-4] + '-%08x.ndb' % t['hash'] if short.endswith('.ndb') else short + '-%08x' % t['hash']
            rename[t['hash']] = (t['path'][:len(t['path']) - len(short)] + new, sid(new))
    for t in mtex:
        if t['hash'] in rename:
            t['name'], t['hash'] = rename[t['hash']]

    class Src(Pak):
        def textures(self):
            out = []
            for t in super().textures():
                if t['hash'] in want:
                    if t['hash'] in rename:
                        t = dict(t, path=rename[t['hash']][0], hash=rename[t['hash']][1])
                    out.append(t)
            return out

    saved = convert_net.Pak, convert_net.read_materials

    def pak(path):
        return Src(path) if os.path.abspath(path) == os.path.abspath(ps3_net) else saved[0](path)
    convert_net.Pak = pak
    convert_net.read_materials = lambda p: (mats, mtex)
    try:
        r = convert_net.convert(ps3_net, ps4_net, out + '.tmp', effect=default[0],
                                    srgb_color=False, verbose=False)   # PS4 net images are UNORM (0x47): sRGB drew them darker
    finally:
        convert_net.Pak, convert_net.read_materials = saved
    os.replace(out + '.tmp', out)
    log('net images: %s: %d materials, %d textures, %d bytes' % (os.path.basename(out), len(mats) - len(r['skipped']),
                                                                 len(r['textures']), r['size']))
    return [m['name'] for m in mats]
