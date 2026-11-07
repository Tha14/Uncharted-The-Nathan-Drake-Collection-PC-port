"""The Uncharted 3 PS3 -> PS4 pak converters (u3mp/patcher/tools/paktools, GPL v2) set up for Uncharted 2.

U2 paks are the same container (0x0a79, pak22 / actor27) and mostly the same item formats as U3 (pak23 / actor33).
What differs, found on the PS3/PS4 pairs of the shared multiplayer levels and skins (IMPORT.md):
  * texture cook prefix of the PS4 build: .../Data/U2/CookData/... instead of U3
  * LIGHT_TABLE_1: older 0x40 / 0x68 header layout (u2light.py)
  * VIS_INFO_1: version 7, no H table (u2vis.py)
  * level data packages are '<level>-ingame10.pak' (U3: -ingame12)
  * foreground actors: PM_COLLECTION_2 geometry instead of PM_FG_PROTOTYPES (u2actor.py)
Import this module before any paktools module is used; it puts paktools on sys.path and patches those spots.
Nothing in u3mp is modified on disk.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def find_paktools():
    """u3mp/patcher/tools/paktools of the launcher package (u2mp and u3mp sit side by side), or U2MP_PAKTOOLS"""
    cands = [os.environ.get('U2MP_PAKTOOLS', ''),
             os.path.join(HERE, '..', '..', '..', 'u3mp', 'patcher', 'tools', 'paktools'),
             os.path.join(HERE, '..', 'tools', 'paktools')]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, 'ndpak.py')):
            return os.path.abspath(c)
    raise RuntimeError('the Uncharted 3 converter (u3mp/patcher/tools/paktools) was not found next to u2mp; '
                       'set U2MP_PAKTOOLS')


PAKTOOLS = find_paktools()
for _p in (os.path.join(PAKTOOLS, 'stage'), PAKTOOLS, HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)

U2_COOK_PREFIX = 'd:/dev/CP9/Data/U2/CookData/ND/build/texture2/'

# Big U2 level paks (the DLC main maps, retail ones too) have more fixups than fit in 512 KiB: their fixup
# directory entries {page, offset, count} then name page 1, 2 ... of the header block, i.e. 0x80000-byte chunks of
# it (facility-multi: (0, 0x370, 65426), (1, 0, 65536), (2, 0, 2490) end exactly at hdrSize).  The PS4 writer puts
# all fixups in one table at chunk 0, as retail PS4 does (warzone-multi: 110026 entries).
_NDPAK_EDITS = [
    ("            base = 0 if pg == 0 else None\n"
     "            if base is None:\n"
     "                raise ValueError('fixup directory in a non-header page not supported (%d)' % pg)\n",
     "            base = pg * 0x80000\n"),
]

# retail U2 collision items keep their data 128-aligned in the page (vertex / index / MOPP blocks are 128-aligned
# inside the item): the page writer honours Blob.data_align (u2coll sets 0x80)
_WRITER_EDITS = [
    ("                while len(out) % 16:\n"
     "                    out.append(PAD)\n"
     "                x.off = len(out)\n",
     "                while len(out) % 16 or (len(out) + 0x20) % getattr(x.data, 'data_align', 16):\n"
     "                    out.append(PAD)\n"
     "                x.off = len(out)\n"),
]

INGAME = '-ingame10'

_done = False


def setup():
    """apply the U2 differences to the loaded paktools modules (idempotent)"""
    global _done
    if _done:
        return
    import convert_net
    convert_net.PS4_COOK_PREFIX = U2_COOK_PREFIX
    import visinfo
    import u2vis
    visinfo.vis_blobs = u2vis.vis_blobs
    visinfo.check_ps4 = u2vis.check_ps4_7
    _load_patched('mkplan', _MKPLAN_EDITS)
    ps4stage = _load_patched('ps4stage', _STAGE_EDITS)
    import u2light
    ps4stage.PS4_PREFIX = U2_COOK_PREFIX
    ps4stage.convert_light_table = u2light.convert_light_table
    ps4stage.visinfo = visinfo
    import collision
    import u2coll
    collision.havok_bg_blobs = u2coll.havok_bg_blobs      # U2 version 3 layout (u2coll.py)
    import waterdata
    import u2water
    for _n in ('objects', 'materials', 'water_blob'):   # U2 WATER_DATA version 3 (u2water.py)
        setattr(waterdata, _n, getattr(u2water, _n))
    try:
        import ps4actor
        ps4actor.PS4_COOK_PREFIX = U2_COOK_PREFIX
    except ImportError:          # numpy missing: actors are reported by the caller
        pass
    _done = True


# retail U2 PS4 stage paks have no ATTACHED_FG_MAPPING and no TAG_FLOAT (U3 retail stages do): not written
_STAGE_EDITS = [
    # the unnamed facility-multi material (see _MKPLAN_EDITS) gets a PS4 key of the usual 'dir:variant' form
    ("            k3 = n.rsplit('/', 1)[0] + ':' + n.rsplit('/', 1)[1]\n",
     "            k3 = n.rsplit('/', 1)[0] + ':' + n.rsplit('/', 1)[1] if '/' in n else 'levels/u2-unnamed:%s' % (n or 'material')\n"),
    ("    afm = P.item('ATTACHED_FG_MAPPING', af)\n", "    afm = None\n"),
    ("    tagf = P.item('TAG_FLOAT', tf)\n", "    tagf = None\n"),
    # login order of retail U2 stages (village-multi-wide, sky-village-multi): the small items, EFFECT_TABLE,
    # TEXTURE_TABLE, LEVEL_OFFSET_1, VIS_INFO_1 before GEOMETRY_1, LIGHT_TABLE_1, then collision / water
    ("    w.login_targets = [level_info] + ([loff] if loff is not None else []) + [debug_info, level_pats, et_item, "
     "ttab, geo] + ([vis] if vis is not None else []) + \\\n"
     "        ([light] if light is not None else []) + [afm] + colls + ([water] if water is not None else []) + [tagf]\n",
     "    w.login_targets = [level_info, level_pats, debug_info, et_item, ttab] + ([loff] if loff is not None else []) + "
     "([vis] if vis is not None else []) + [geo] + ([light] if light is not None else []) + colls + "
     "([water] if water is not None else [])\n"),
]
U2_LOGIN = ['LEVEL_INFO_1', 'LEVEL_PATS_1', 'DEBUG_INFO_1', 'EFFECT_TABLE', 'TEXTURE_TABLE', 'LEVEL_OFFSET_1',
            'VIS_INFO_1', 'GEOMETRY_1', 'LIGHT_TABLE_1', 'COLLISION_DATA_HAVOK_BG', 'WATER_DATA']


# mkplan's fallback (a PS3 material with no same-named PS4 material) only considers U3 'prime-shader.fx' materials.
# Retail U2 PS4 level materials mostly keep their own shaders (blend-color-bump-specular.fx, ...), and its few
# prime-shader materials are characters (e.g. dead-explorer skin): U2 considers every background material instead
# (GEOMETRY_1 flag bit 0 = foreground geometry is left out).
_MKPLAN_EDITS = [
    # facility-multi has one PM_COLLECTION_2 group with an empty material name: planned like any material without
    # a retail twin (closest background material) instead of failing on the missing 'dir/variant' split
    ("    d, v = ps3name.rsplit('/', 1)\n",
     "    if '/' not in ps3name:\n        return ps3name\n    d, v = ps3name.rsplit('/', 1)\n"),
    ("                if pk in exclude or m['s'] != 'prime-shader.fx':\n",
     "                if pk in exclude or (m.get('fl', 0) & 1) or m['s'] in ('hair.fx', 'eye.fx', 'skin.fx'):\n"),
]


def _load_patched(name, edits, folder='stage'):
    """paktools/<folder>/<name>.py with U2 edits, installed as the module <name>"""
    import types
    if name in sys.modules and getattr(sys.modules[name], 'U2', False):
        return sys.modules[name]
    path = os.path.join(PAKTOOLS, folder, name + '.py')
    src = open(path, encoding='utf-8').read()
    for old, new in edits:
        if src.count(old) != 1:
            raise RuntimeError('%s.py changed: edit point not found once: %r' % (name, old[:60]))
        src = src.replace(old, new)
    mod = types.ModuleType(name)
    mod.__file__ = path
    mod.U2 = True
    sys.modules[name] = mod
    exec(compile(src, path, 'exec'), mod.__dict__)
    return mod


def paktools_env():
    """environment for paktools subprocesses (fxextract runs as its own process)"""
    env = dict(os.environ)
    env['PYTHONPATH'] = os.pathsep.join([os.path.join(PAKTOOLS, 'stage'), PAKTOOLS, HERE] +
                                        ([env['PYTHONPATH']] if env.get('PYTHONPATH') else []))
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    return env


# ndpak must be the patched one before any paktools module does 'from ndpak import Pak'
_load_patched('ndpak', _NDPAK_EDITS, folder='')
_load_patched('ps4writer', _WRITER_EDITS, folder='')
