"""Convert the Lab pieces for maps 34 / 34b / 37 into maps-batch/files (overlay is only read).
Run with the numpy venv: pv/Scripts/python.exe -I build_paks.py <paktools copy with rebuilt hk tables> <scratchpad>/"""
import sys, os, json, shutil, struct, collections, traceback
PT, S = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [PT, PT + '/stage', HERE]; os.environ['PAKTOOLS'] = PT
from ndpak import Pak
from ps4writer import PakWriter, make_login_and_vram_page
import slicepak, assetbatch, ps4verify, ps4check, crashcheck, ps4ingame, collision, sg
MB = S + 'maps-batch/'; SRC = S + 'labsrc/pak/'; TMP = MB + '_tmp/src/'
A = MB + 'files/actor33/ps4/'; P = MB + 'files/pak23/ps4/'
for d in (A, P, TMP): os.makedirs(d, exist_ok=True)
OV = "{{PROJECT}}/work/overlay-r3/u3data/build/main/"
RA = "{{PS4}}/u3data/build/main/actor33/ps4/"
TMPL = RA + 'crate-wood-1x1x1-a.pak'
rows = []; fail = []


def cerrs(path):
    e, _, _ = ps4check.check(path, quiet=True)
    return [x for x in e if not x.startswith('unknown item type') and not (x.startswith('tiled size') and 'env-reflection' in x)]


def actor(name, src):
    dst = A + name + '.pak'
    ok, msg, cap = assetbatch.convert((name + '.pak', 'lab', src, 'own', TMPL, {}), dst)
    if not ok:
        fail.append((name, msg)); print('FAIL', name, msg); return
    errs, _ = ps4verify.verify(dst, TMPL, name, quiet=True)
    ce = cerrs(dst); cc = crashcheck.check(dst)
    cc = cc[0] if isinstance(cc, tuple) else cc
    good = not errs and not ce and not cc
    rows.append((name, os.path.getsize(dst), cap, 'OK' if good else 'BAD', str(errs)[:90], str(ce)[:90], str(cc)[:90], msg[:70]))
    print('OK ' if good else 'BAD', name, os.path.getsize(dst), cap, errs[:1], ce[:1], cc[:1] if cc else '', msg[:50])
    if not good: fail.append((name, errs, ce, cc))


# ---- boxes (standalone in lab-trench-v1-7); mixer-map-side is byte-identical to lt-box20
USED = ['lt-box%02d' % i for i in list(range(21)) + [31, 32, 33, 36, 37]]
for n in USED: actor(n, SRC + n + '.pak')
# ---- Stadium art groups: PS3 hid them inside packs named after campaign actors
GROUPS = {'anim-col-square-tail': 3, 'anim-col-thrown-out': 3, 'anim-london-pub': 4, 'anim-london-pub-bathroom': 4,
          'block-20x20x30': 3, 'cruise-ship-ballroom-90-lod': 4, 'lab-multi-34-pak-ingame12': 1}
sd = []
for f in GROUPS:
    q = Pak(SRC + f + '.pak'); names = slicepak.arts(q); assert len(names) == GROUPS[f], (f, names)
    for a in names:
        t = TMP + a + '.pak'; open(t, 'wb').write(slicepak.slice_bytes(q.path, a)); sd.append(a); actor(a, t)


# ---- level pieces
def havok_pak(src, dst):
    q = Pak(src); blobs = collision.havok_bg_blobs(q)
    assert len(blobs) == 1, len(blobs)
    w = PakWriter(); pg = w.page(); it = pg.item('COLLISION_DATA_HAVOK_BG', blobs[0])
    make_login_and_vram_page(w, w.page(), [it], [])
    open(dst, 'wb').write(w.build()); return os.path.getsize(dst)


def page_slice(src, keep_page, dst_src):
    """PS3 ingame pak holding the stadium + spawner sub-packages: keep one sub-package's pages"""
    return slicepak.slice_pages(src, keep_page, dst_src)


ps4ingame.IGNORE_OTHERS = True
ps4ingame.NEUTRALIZE_ARTS = {'sd-pitch', 'sd-structure'}       # no PS3 file holds these arts (markers, nothing spawns)
ig34 = SRC + 'lab-multi-34-pak-ingame12.pak'
a34, b34 = TMP + '34a-ingame12.pak', TMP + '34b-ingame12.pak'
slicepak.slice_pages(ig34, 2, a34); slicepak.slice_pages(ig34, 3, b34)
ps4ingame.convert(a34, P + 'lab-multi-34-pak-ingame12.pak')
ps4ingame.convert(b34, P + 'lab34-spawners-ingame12.pak')
ps4ingame.NEUTRALIZE_ARTS = set()
print('havok 34', havok_pak(a34, P + 'lab-multi-34-pak.pak'))
print('havok 37', havok_pak(SRC + 'lab-multi-37-pak-ingame12.pak', P + 'lab-multi-37-pak.pak'))
shutil.copy2(OV + 'pak23/ps4/lab-multi-37-pak.pak' if False else OV + 'pak23/ps4/lab-multi-01-pak.pak', P + 'lab34-spawners.pak')   # 248-byte empty pak
for x, y in (('lab-multi-14.pak', 'lab34-stage.pak'), ('lab-multi-14-ingame12.pak', 'lab34-stage-ingame12.pak')):
    shutil.copy2(OV + 'pak23/ps4/' + x, P + y)
for f in sorted(os.listdir(P)):
    q = Pak(P + f); v = q.validate(); cc = crashcheck.check(P + f); cc = cc[0] if isinstance(cc, tuple) else cc
    print('pak', f, os.path.getsize(P + f), dict(collections.Counter(i.type for i in q.items)), 'validate', v[:2], 'crashcheck', cc[:3] if cc else 'OK')
    if v or cc: fail.append((f, v, cc))
# spawner arts per sub-package
arts = {}
for k, pth in (('34a', a34), ('34b', b34), ('37', SRC + 'lab-multi-37-pak-ingame12.pak')):
    q = Pak(pth); c = collections.Counter()
    for it, (pg, o) in sg.spawners(q):
        r = q.resolve(pg, o + 52); c[q.pstr(*r) if r else ''] += 1
    arts[k] = {x: n for x, n in c.items() if x}
man = dict(boxes=USED, sd=sd, arts=arts, rows=rows, fail=[str(x)[:400] for x in fail])
json.dump(man, open(MB + 'files/manifest.json', 'w'), indent=1)
print('actors', len(rows), 'fail', len(fail)); print(arts)
