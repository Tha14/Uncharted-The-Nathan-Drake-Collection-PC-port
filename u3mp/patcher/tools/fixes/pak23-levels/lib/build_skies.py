#!/usr/bin/env python3
"""build_skies.py: rebuild the batch57 (+ batch89 GCM remap) sky level paks of overlay-r3 from the PS3 sky paks.

Re-implements the cloud's aga/maps/b57/skyplan.py + buildsky.py (agent A, 2026-10-28) with local paths, run with the
cloud converter as it ended (recovered converter/paktools, GCM remap included = batch89) plus one fix (paktools-sky.patch):
  1. plan  = mkplan.plan(PS3 sky pak, res)        (the cloud used its req7 plans; only the exact non-sky material matters)
  2. sky entries re-pointed to retail sky-clouds.fx templates with a PS3 texture per binding (skyplan MAP)
  3. bundle = ONE shared fxextract bundle of all 9 templates in MAP order (sky.req.json, tex=[]), plus per level the
     exact non-sky materials (sky-drydock's seabed) with the technique table of the release-maps-3 pak (REL_TECHS)
  4. ps4stage.convert + paramfix.fix              (buildsky; no cubefix)
usage: build_skies.py PAKTOOLS PS3_DIR PS4_PAK23_DIR RES_DIR OUT_DIR [level ...] [--ref REF_DIR]
  PAKTOOLS   converter tree: paktools-sky (next to this script)
  PS3_DIR    PS3 sky paks: <PS3_DIR>/<archive>/<level>.pak (archive priority as plan.tsv) or <PS3_DIR>/<level>.pak
  PS4_PAK23_DIR  retail PS4 u3data/build/main/pak23/ps4 (TextureDictionaries.csv is found from it, or $U3_TEXDICT)
  RES_DIR    mkplan survey (ps4mats.jsonl.gz, ps4tex.tsv; levels.py res_dir())   OUT_DIR  output paks + work files
  --ref DIR  compare with DIR/<level>.pak (sha256)
Runs at below-normal priority (Windows)."""
import sys, os, json, re, hashlib, subprocess, io, contextlib, traceback

if os.name == 'nt':
    import ctypes
    ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)

args = [a for a in sys.argv[1:]]
REF = None
if '--ref' in args:
    i = args.index('--ref'); REF = args[i + 1]; del args[i:i + 2]
PT, PS3, PS4, RES, OUT = args[:5]
LEVELS = args[5:] or ['sky-drydock', 'sky-fort-coop-b', 'sky-fort-multi', 'sky-gompa-mp', 'sky-grave-multi',
                      'sky-ldn-streets-multi', 'sky-oasis-multi', 'sky-yemen-mp']
sys.path[:0] = [PT, os.path.join(PT, 'stage')]
os.environ['PYTHONPATH'] = PT + os.pathsep + os.path.join(PT, 'stage')
if not os.environ.get('U3_TEXDICT'):      # PS4 texture dictionary (path of PS3-only textures that retail also has)
    os.environ['U3_TEXDICT'] = os.path.normpath(os.path.join(PS4, '..', '..', 'texturedict1', 'ps4', 'TextureDictionaries.csv'))
from ndpak import Pak
import mkplan

T = dict(SUN=('sky-chateau-multi.pak', 'levels/sky/-big3/sky-chateau-multi:default'),
         PLAIN=('sky-highrise-multi.pak', 'levels/sky/-big3/mp-coop/sky-highrise-multi:default'),
         UNDER=('sky-grave-03-docks.pak', 'levels/sky/-big3/sky-grave-03-docks-undersky:default'),
         DIG=('sky-dig-coop.pak', 'levels/sky/-big3/mp-coop/sky-dig-coop:default'),
         LW=('sky-london-wide.pak', 'levels/sky/-big3/sky-london-wide:default'),
         LWM=('sky-london-wide.pak', 'levels/sky/-big3/sky-london-wide-moon:default'),
         GR=('sky-grave-03-docks.pak', 'levels/sky/-big3/sky-grave-03-docks:default'),
         HC=('sky-horse-chase.pak', 'levels/sky/-big3/sky-horse-chase:default'),
         SYN=('sky-syria.pak', 'levels/sky/-big3/sky-syria-night:default'))
S, R, CM, CM2, CC, CC2, F, FL, DM, MC, MM = ('g_tSkyColorMap', 'g_tNdFetchSkyColorReflectionMap', 'g_tNdFetchCloudMaskMap',
    'g_tNdFetchCloudMask2Map', 'g_tNdFetchCloudColorMap', 'g_tNdFetchCloudColor2Map', 'g_tFadeMaskMap',
    'g_tNdFetchFlowTextureMap', 'g_tNdDistortionMaskMap', 'g_tNdFetchMoonColorMap', 'g_tNdFetchMoonMaskMap')
CYL, NOI = 'g_tNdFetchCylinderCloudMaskMap', 'g_tNdFetchUvNoiseColorMap'
LWB = {S: 0, CM: 1, CM2: 2, F: 3, R: 4}
MAP = {('sky-air-multi', 'sky-airport-multi'): ('SUN', {S: 0, R: 1}),
       ('sky-cave-multi-2', 'sky-cave-multi'): ('LW', LWB),
       ('sky-fort-coop-b', 'sky-fort-coop-b'): ('SUN', {S: 0, R: 1}),
       ('sky-fort-multi', 'sky-fort-multi'): ('DIG', {S: 0, CC: 1, CM: 2, CC2: 1, CM2: 3, F: 4, R: 5}),
       ('sky-gompa-mp', 'sky-gompa-multi-3'): ('LWM', {S: 0, CM: 1, CM2: 2, F: 3, R: 4, MC: 5, MM: 6}),
       ('sky-gompa-mp', 'sky-gompa-multi-moon'): ('LWM', {S: 0, CM: 1, CM2: 2, F: 3, MC: 4, MM: 5, R: 6}),
       ('sky-grave-multi', 'sky-grave-multi'): ('GR', {S: 0, CM: 1, CM2: 2, F: 3, FL: 4, R: 5}),
       ('sky-grave-multi', 'sky-grave-multi-undersky'): ('UNDER', {S: 0, R: 1}),
       ('sky-ldn-streets-multi', 'sky-ldn-streets-multi'): ('LW', LWB),
       ('sky-mon-coop-b', 'sky-mon-coop-b'): ('SUN', {S: 0, R: 1}),
       ('sky-oasis-multi', 'sky-oasis-multi'): ('HC', {S: 0, CM: 1, CM2: 1, F: 2, FL: 3, DM: 4, R: 5}),
       ('sky-ruins-multi', 'sky-ruins-multi'): ('PLAIN', {S: 0, R: 1}),
       ('sky-sham-multi', 'sky-sham-multi'): ('LW', LWB),
       ('sky-temple-multi', 'sky-temple-multi'): ('SUN', {S: 0, R: 1}),
       ('sky-train-wreck-multi', 'sky-train-wreck-multi'): ('LW', LWB),
       ('sky-village-multi', 'sky-village-multi-2'): ('LW', LWB),
       ('sky-warzone-multi', 'sky-warzone-multi'): ('LW', LWB),
       ('sky-yemen-mp', 'sky-yemen-multi'): ('DIG', {S: 0, CC: 1, CM: 2, CC2: 1, CM2: 3, F: 4, R: 6}),
       ('sky-drydock', 'sky-drydock-special'): ('GR', {S: 0, CM: 1, CM2: 2, F: 3, FL: 4, R: 5}),
       ('sky-old-quarter', 'sky-old-quarter'): ('SYN', {CM: 0, CM2: 1, F: 2, CYL: 3, NOI: 4, R: 5})}

ARCH = ['dlc-patch18', 'dlc-patch13', 'dlc-map1', 'dlc-map2', 'dlc-map3', 'dlc-map4', 'goty-patch', 'goty-dlc', 'pak23']


def ps3_src(lv):
    for a in ARCH:
        f = os.path.join(PS3, a, lv + '.pak')
        if os.path.exists(f):
            return f
    f = os.path.join(PS3, lv + '.pak')
    if os.path.exists(f):
        return f
    raise SystemExit('PS3 source %s not found under %s' % (lv, PS3))


def md5(f):
    return hashlib.md5(open(f, 'rb').read()).hexdigest()


def sha256(f):
    return hashlib.sha256(open(f, 'rb').read()).hexdigest()


def skyplan(lv, sp, P):
    p = Pak(sp)
    m3, t3 = mkplan.read_materials(p)
    rep = {}
    for (lv_, tail), (tk, bm) in MAP.items():
        if lv_ != lv:
            continue
        m = [x for x in m3 if x['name'].split('/')[-2] == tail][0]
        sh = [re.sub(r'/[0-9A-F]{16}\.ndb(\.error)?$', '', mkplan.short_of(t3[i]['name'])) for i in m['textures']]
        pe = P['mats'][m['name']]
        pak, key = T[tk]
        pe.update(key=key, pak=pak, exact=False, bindmap={b: sh[i] for b, i in bm.items() if i < len(sh)})
        for k_ in ('name', 'binds', 'roles', 'cls'):
            pe.pop(k_, None)
        rep[tail] = dict(template=key, textures=sh, bindmap=pe['bindmap'])
    return rep


def fxextract(req, stem):
    rq, bf = stem + '.req.json', stem + '.bundle.json'
    json.dump(req, open(rq, 'w'), indent=1)
    subprocess.run([sys.executable, os.path.join(PT, 'stage', 'fxextract.py'), PS4, rq, bf], check=True,
                   stdout=subprocess.DEVNULL)
    B = json.load(open(bf))
    bad = [m['key'] for m in B['mats'] if 'err' in m or not m.get('rec')]
    if bad:
        raise RuntimeError('bundle errors %s' % bad)
    return B


# buildsky took the level's exact (non-sky) materials from the release-maps-3 pak itself (fxextract REL <lv>.pak), so
# their technique (EFFECT string) table is that pak's table: the 10-27 v1 build = techs(v1 sky template donor) + the
# exact material's own pak techs (ps4stage: base pak first, then new strings). The v1 donor of sky-drydock's sky
# material was ldn-atwood-station.pak (found by search over the retail pak23: the only table that gives the reference).
REL_TECHS = {'sky-drydock': 'ldn-atwood-station.pak'}


def rel_techs(X, donor):
    """X (fxextract bundle of retail exact materials) -> as if extracted from the release-maps-3 pak: techs of each
    material's pak = U(techs(donor), techs(pak)); the EFFECT string indices of the closure re-indexed to that table."""
    import fxremap
    D = fxextract(dict(mats=[], tex=[], techs=[donor]), os.path.join(OUT, 'work', 'reltechs.' + donor[:-4]))['techs'][donor]
    for m in X['mats']:
        src = X['techs'][m['pak']]
        T = list(D) + [s for s in src if s not in D]
        mp = {i: T.index(s) for i, s in enumerate(src)}
        it = m['rec']['fx'][0]
        ptrs = {pp[0]: (pp[2] if pp[1] == 0 else None) for pp in it['ptrs']}
        it['data'] = fxremap.remap(bytes.fromhex(it['data']), ptrs, mp).hex()
        X['techs'][m['pak']] = T


_SB = None


def sky_bundle(wd):
    """skyplan's sky.req.json (templates in MAP order, techs = their paks, no tex) -> fxextract -> sky.bundle.json"""
    global _SB
    if _SB is None:
        req = dict(mats=[], tex=[], techs=[])
        for (lv_, tail), (tk, bm) in MAP.items():
            pak, key = T[tk]
            if [pak, key] not in req['mats']:
                req['mats'].append([pak, key])
            if pak not in req['techs']:
                req['techs'].append(pak)
        _SB = fxextract(req, os.path.join(wd, 'sky'))
    return _SB


def build(lv):
    import ps4stage, paramfix, crashcheck
    sp = ps3_src(lv)
    wd = os.path.join(OUT, 'work'); os.makedirs(wd, exist_ok=True)
    with contextlib.redirect_stdout(io.StringIO()):
        P = mkplan.plan(sp, (), RES, verbose=False) if 'verbose' in mkplan.plan.__code__.co_varnames else mkplan.plan(sp, (), RES)
    rep = dict(src=sp, src_md5=md5(sp), sky=skyplan(lv, sp, P))
    json.dump(P, open(os.path.join(wd, lv + '.plan.json'), 'w'), indent=1)
    # buildsky: B = the ONE shared sky bundle (all templates, skyplan order); retail textures are looked up by path in
    # bundle order, so the first template carrying a texture decides which retail copy is used
    B = json.loads(json.dumps(sky_bundle(wd)))
    need = [v for v in P['mats'].values() if v.get('bindmap') is None]
    if need:                # the other (exact) materials of the level (drydock's seabed): from their retail pak
        X = fxextract(dict(mats=[[v['pak'], v['key']] for v in need], tex=[], techs=sorted({v['pak'] for v in need})),
                      os.path.join(wd, lv + '.x'))
        if lv in REL_TECHS:
            rel_techs(X, REL_TECHS[lv])
        B['techs'].update(X.get('techs') or {})
        B['mats'] += X['mats']
        B['tex'] = B.get('tex', []) + X.get('tex', [])
    op = os.path.join(OUT, lv + '.pak')
    with contextlib.redirect_stdout(io.StringIO()):
        d, srep = ps4stage.convert(sp, P, B, op, verbose=False)
        npf = len(paramfix.fix(op, P))
        e, _ = crashcheck.check(op)
    rep.update(paramfix=npf, crash=e, size=os.path.getsize(op), md5=md5(op), sha256=sha256(op))
    if REF and os.path.exists(os.path.join(REF, lv + '.pak')):
        rf = os.path.join(REF, lv + '.pak')
        rep.update(ref_md5=md5(rf), ref_sha256=sha256(rf), ref_size=os.path.getsize(rf), identical=sha256(rf) == rep['sha256'])
    return rep


if __name__ == '__main__':
    os.makedirs(OUT, exist_ok=True)
    rp = os.path.join(OUT, 'build_skies.json')
    allrep = json.load(open(rp)) if os.path.exists(rp) else {}
    for lv in LEVELS:
        try:
            allrep[lv] = build(lv)
        except Exception as ex:
            allrep[lv] = dict(error=repr(ex)); traceback.print_exc()
        r = allrep[lv]
        print(lv, {k: r.get(k) for k in ('md5', 'ref_md5', 'identical', 'size', 'ref_size', 'paramfix', 'crash', 'error')}, flush=True)
        json.dump(allrep, open(rp, 'w'), indent=1, default=str)
