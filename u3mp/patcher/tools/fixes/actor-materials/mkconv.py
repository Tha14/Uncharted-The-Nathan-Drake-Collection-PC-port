"""Generate ps4actor_am.py from the paktools ps4actor.py. Every change is an asserted text replacement."""
import os
T = '{{PAKTOOLS}}/ps4actor.py'
s = open(T, encoding='utf8').read()
R = [
    # paktools folder on the path even though this file lives elsewhere
    ("sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))\n",
     "sys.path.insert(0, '{{PAKTOOLS}}')\nMAX_TEX = 0\n"),
    # repair: submesh array may be on another page than GEOMETRY_1
    ("    sm = q.resolve(g.page, g.data_off + 0x10)[1]\n    njoints = q.pu32(g.page, sm + 0x58)\n",
     "    sp, sm = q.resolve(g.page, g.data_off + 0x10)\n    njoints = q.pu32(sp, sm + 0x58)\n"),
    # real PS3 material -> texture table (ps3mat) instead of the name guess
    ("    texs = [t for t in src.textures()]\n",
     "    texs = [t for t in src.textures()]\n"
     "    sys.path.insert(0, %r)\n"
     "    from ps3mat_am import read_materials\n" % os.path.dirname(os.path.abspath(__file__)) +
     "    _m, _t = read_materials(src)\n"
     "    MATTEX = {m['name']: {_t[i]['hash'] for i in m['textures'] if i is not None} for m in _m}\n"),
    ("        stem = mname.rsplit('/', 1)[0].rsplit('/', 1)[-1]          # .../<file>/<mat>\n",
     "        _own = [e for e in ps3tex if e['hash'] in MATTEX.get(mname, ())]\n"
     "        if _own:\n"
     "            # the first passes list only part of a material's set: add siblings (x-color -> x-normal, x-spec-mask)\n"
     "            _base = set()\n"
     "            for e in _own:\n"
     "                if e['kind']:\n"
     "                    _base.add(e['short'].split('.tga')[0].split(e['kind'])[0].rsplit('/', 1)[-1])\n"
     "            for e in ps3tex:\n"
     "                if e not in _own and e['kind'] and e['short'].split('.tga')[0].split(e['kind'])[0].rsplit('/', 1)[-1] in _base:\n"
     "                    _own.append(e)\n"
     "            return _own\n"
     "        stem = mname.rsplit('/', 1)[0].rsplit('/', 1)[-1]          # .../<file>/<mat>\n"),
    # texture cap: drop top mips of 2D PS3 textures
    ("        tiled = tc.ps3_to_ps4_texels(texels, t['width'], t['height'], t['mips'], bpb)\n",
     "        tw, th, tm_ = t['width'], t['height'], t['mips']\n"
     "        while MAX_TEX and t['type'] == 1 and max(tw, th) > MAX_TEX and tm_ > 1:\n"
     "            bw_, bh_ = tc.blocks(tw, th, True)\n"
     "            texels = texels[bw_ * bh_ * bpb:]\n"
     "            tw, th, tm_ = max(1, tw // 2), max(1, th // 2), tm_ - 1\n"
     "        tiled = tc.ps3_to_ps4_texels(texels, tw, th, tm_, bpb)\n"),
    ("lazy=(short, t['hash'], fmt4, t['width'], t['height'], t['mips'], tiled)",
     "lazy=(short, t['hash'], fmt4, tw, th, tm_, tiled)"),
    # a material with known PS3 textures must not borrow another character's map for a kind it lacks
    ("                e = c[0] if c else (tmpl_texture(tfull) or flat_texture(want[0]))\n",
     "                e = c[0] if c else ((flat_texture(want[0]) if MATTEX.get(mn) else None) or tmpl_texture(tfull) or flat_texture(want[0]))\n"),
    # texture kinds this copy lacks: hair alpha, and the Marisa eye/teeth naming (x_c.dds, x_n.dds)
    ("    ('-detail-normal', ('g_tNdFetchDetail3Map', 'g_tNdFetchDetail1Map', 'g_tNdFetchDetail2Map')),\n",
     "    ('-trans', ('g_tNdFetchHairAlphaMap',)),\n    ('-detail-normal', ('g_tNdFetchDetail3Map', 'g_tNdFetchDetail1Map', 'g_tNdFetchDetail2Map')),\n"),
    ("            return suf\n    return None\n",
     "            return suf\n    if base.endswith(('_c', '_clr')):\n        return '-color'\n    if base.endswith(('_n', '_nmt')):\n        return '-normal'\n    return None\n"),
    ("'-tint-mask': (0, 0, 0), '-detail-normal': (128, 128, 255)}[kind]",
     "'-tint-mask': (0, 0, 0), '-detail-normal': (128, 128, 255), '-trans': (255, 255, 255)}[kind]"),
    # colour fallback must be sRGB (crashcheck V3)
    ("        e = add_texture(short, sid(short), 0x47, 64, 64, mips, tiled)\n",
     "        e = add_texture(short, sid(short), 0x48 if kind == '-color' else 0x47, 64, 64, mips, tiled)\n"),
]
for a, b in R:
    assert s.count(a) == 1, a
    s = s.replace(a, b)
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ps4actor_am.py'), 'w', encoding='utf8').write(s)
print('ok', len(R))
