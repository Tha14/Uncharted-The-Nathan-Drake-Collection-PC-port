"""Level part of maps-batch/tools/build_paks.py (2026-11-01): the converted pieces of Lab maps 34 / 34b / 37.
usage: build_level_paks.py <paktools> <labsrc/pak> <tmp dir> <out dir>   (PYTHONPATH: paktools, paktools/stage, lib)
Writes lab-multi-34-pak-ingame12.pak, lab34-spawners-ingame12.pak, lab-multi-34-pak.pak, lab-multi-37-pak.pak.
(lab34-spawners.pak = empty level, lab34-stage(-ingame12).pak = copies of lab-multi-14: plan.tsv rows 'empty'/'copy';
the box / stadium actors of build_paks.py are actor33 and not built here.)"""
import os
import sys

PT, SRC, TMP, P = sys.argv[1:5]
sys.path[:0] = [PT, os.path.join(PT, 'stage'), os.path.dirname(os.path.abspath(__file__))]
os.environ['PAKTOOLS'] = PT
from ndpak import Pak  # noqa: E402
from ps4writer import PakWriter, make_login_and_vram_page  # noqa: E402
import slicepak, ps4ingame, collision  # noqa: E402,E401

for d in (P, TMP):
    os.makedirs(d, exist_ok=True)


def havok_pak(src, dst):
    q = Pak(src)
    blobs = collision.havok_bg_blobs(q)
    assert len(blobs) == 1, len(blobs)
    w = PakWriter()
    pg = w.page()
    it = pg.item('COLLISION_DATA_HAVOK_BG', blobs[0])
    make_login_and_vram_page(w, w.page(), [it], [])
    open(dst, 'wb').write(w.build())
    return os.path.getsize(dst)


ps4ingame.IGNORE_OTHERS = True
ps4ingame.NEUTRALIZE_ARTS = {'sd-pitch', 'sd-structure'}       # no PS3 file holds these arts (markers, nothing spawns)
ig34 = os.path.join(SRC, 'lab-multi-34-pak-ingame12.pak')
a34, b34 = os.path.join(TMP, '34a-ingame12.pak'), os.path.join(TMP, '34b-ingame12.pak')
slicepak.slice_pages(ig34, 2, a34)
slicepak.slice_pages(ig34, 3, b34)
ps4ingame.convert(a34, os.path.join(P, 'lab-multi-34-pak-ingame12.pak'))
ps4ingame.convert(b34, os.path.join(P, 'lab34-spawners-ingame12.pak'))
ps4ingame.NEUTRALIZE_ARTS = set()
print('havok 34', havok_pak(a34, os.path.join(P, 'lab-multi-34-pak.pak')))
print('havok 37', havok_pak(os.path.join(SRC, 'lab-multi-37-pak-ingame12.pak'), os.path.join(P, 'lab-multi-37-pak.pak')))
