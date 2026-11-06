"""make_default_lightmaps.py - write default_lightmaps.json (the two retail default lightmap textures ps4stage.py
binds to unlit props) from the player's own PS4 game, so no texture data ships with the recipe.

  python make_default_lightmaps.py <PS4 u3data/build/main/pak23/ps4> <out.json>

Runs fxextract.py (next to this file) on the retail grave-02-hollowhulk-front-low.pak for the two textures; same output as the file
the converter used to carry.  ps4stage.py reads <its folder>/default_lightmaps.json."""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PAK = 'grave-02-hollowhulk-front-low.pak'   # retail; its two copies give the recorded texels byte for byte
LABEL = 'fort-multi-stage.pak'  # the pak the converter recorded them from (a converted map): its name and VRAM placement are kept
PATHS = ('d:/dev/CP9/Data/U3/CookData/ND/build/texture2/art/general/textures/default/default-lightmap0.tga/121EDD38E3116400.ndb',
         'd:/dev/CP9/Data/U3/CookData/ND/build/texture2/art/general/textures/default/default-lightmap1.tga/B1C390E18A2F0DF2.ndb')
# VRAM_DESC placement words as recorded in that map (u32 at +0x08, u16 at +0x38); the rest of the descriptor and the
# texels come from the player's retail pak
PLACE = {PATHS[0]: (0x0b7000, 0x3700), PATHS[1]: (0x0b8800, 0x3870)}


def make(pak_dir, out):
    with tempfile.TemporaryDirectory() as t:
        req = os.path.join(t, 'req.json')
        json.dump(dict(mats=[], tex=[[PAK, p] for p in PATHS], techs=[]), open(req, 'w'))
        res = os.path.join(t, 'out.json')
        env = dict(os.environ, PYTHONPATH=os.pathsep.join([os.path.dirname(HERE), HERE]))
        subprocess.run([sys.executable, os.path.join(HERE, 'fxextract.py'), pak_dir, req, res], check=True,
                       env=env, stdout=subprocess.DEVNULL)
        d = json.load(open(res))
    bad = [t for t in d['tex'] if 'desc' not in t]
    for t in d['tex']:
        t['pak'] = LABEL
        if 'desc' in t:
            b = bytearray.fromhex(t['desc'])
            b[8:12] = PLACE[t['path']][0].to_bytes(4, 'little')
            b[0x38:0x3a] = PLACE[t['path']][1].to_bytes(2, 'little')
            t['desc'] = b.hex()
    if bad:
        raise SystemExit('default lightmaps not found in %s: %s' % (PAK, bad))
    tmp = out + '.tmp'
    json.dump(d, open(tmp, 'w'))
    os.replace(tmp, out)


if __name__ == '__main__':
    make(sys.argv[1], sys.argv[2])
