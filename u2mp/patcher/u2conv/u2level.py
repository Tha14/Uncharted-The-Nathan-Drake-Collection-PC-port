"""Uncharted 2 level paks (pak22): PS3 -> PS4.

Two kinds, told apart by their items:
  * background (stage) paks: PM_COLLECTION_2 / BACKGROUND_SECTION geometry, materials, textures, LIGHT_TABLE_1,
    VIS_INFO_1 -> the U3 stage pipeline (paktools/stage: mkplan -> fxextract -> ps4stage), with the U2 light and
    vis tables (u2light, u2vis) and the U2 cook prefix.  Materials and compiled effects are taken from retail PS4
    U2 paks: a material with the same name when one exists (exact), else the closest retail material
    (fallback, see mkplan).  The level's own PS4 copy is never used (it is missing: that is why it is imported).
  * data paks ('-ingame10', '-npc'): spawners, regions, splines, feature db -> u2ingame.
Background collision (u2coll), water (u2water), nav meshes (u2ingame -> u2nav) and paks with more than 512 KiB of
fixups (u2paktools ndpak edit) are converted; CAMERA_TABLE_1 (village-multi only) is left out as before.
"""
import json
import os
import subprocess
import sys

import u2paktools

u2paktools.setup()
from ndpak import Pak  # noqa: E402

STAGE_TYPES = {'PM_COLLECTION_2', 'BACKGROUND_SECTION'}
DATA_TYPES = {'SPAWNER_GROUP', 'REGION_TREE', 'FEATURE_DB_2', 'SPLINE_TABLE', 'NAV_MESH_1', 'TAG_INT'}
BLOCKERS = {}       # collision (u2coll), water (u2water), nav meshes (u2nav) and big fixup tables are converted now


class Unsupported(Exception):
    pass


def kind(p):
    types = {i.type for i in p.items}
    if types & STAGE_TYPES:
        return 'stage'
    if types & DATA_TYPES or types <= {'LEVEL_INFO_1', 'PAK_LOGIN_TABLE', 'VRAM_DESC_TABLE', 'TAG_INT',
                                        'SPLINE_TABLE', 'LEVEL_OFFSET_1', 'DEBUG_INFO_1', 'LEVEL_PATS_1'}:
        return 'data'
    return 'other'


def build_res(ps4_pak_dir, res_dir, log=print):
    """the retail PS4 material survey + texture index mkplan reads (paktools/fixes/pak23-levels/lib/mkres.py, for
    pak22 / -ingame10): built once per PS4 game (a few minutes)"""
    import gzip
    done = os.path.join(res_dir, 'ok.json')
    stamp = dict(dir=os.path.abspath(ps4_pak_dir), n=len(os.listdir(ps4_pak_dir)))
    if os.path.exists(done):
        try:
            if json.load(open(done)) == stamp:
                return res_dir
        except ValueError:
            pass
    os.makedirs(res_dir, exist_ok=True)
    import struct
    log('level survey: reading the retail PS4 level paks (materials, textures) ...')
    mats_f = gzip.open(os.path.join(res_dir, 'ps4mats.jsonl.gz'), 'wt')
    tex_f = open(os.path.join(res_dir, 'ps4tex.tsv'), 'w', encoding='utf-8')
    n = e = 0
    for fn in sorted(os.listdir(ps4_pak_dir)):
        if not fn.endswith('.pak') or fn.endswith(u2paktools.INGAME + '.pak'):
            continue
        try:
            q = Pak(os.path.join(ps4_pak_dir, fn))
            mats = []
            for g in q.items_of('GEOMETRY_1'):
                pg, b = g.page, g.data_off
                flags = q.pu32(pg, b + 4)
                cnt = q.u64(q.pbase(pg) + b + 0x48)
                r = q.resolve(pg, b + 0x50)
                if not r:
                    continue
                mp, mo = r
                for i in range(cnt):
                    o = mo + i * 0x48
                    S = lambda off: q.pstr(*q.resolve(mp, o + off)) if q.resolve(mp, o + off) else None  # noqa: E731
                    nf, npar, nb, x3 = struct.unpack_from('<4I', q.d, q.pbase(mp) + o + 0x30)
                    feats, binds = [], []
                    fr = q.resolve(mp, o + 0x10)
                    for k in range(nf if fr else 0):
                        feats.append(q.pstr(*q.resolve(fr[0], fr[1] + 8 * k)))
                    br = q.resolve(mp, o + 0x28)
                    for k in range(nb if br else 0):
                        bo = br[1] + 0x28 * k
                        binds.append((q.pstr(*q.resolve(br[0], bo)), q.pstr(*q.resolve(br[0], bo + 8))))
                    mats.append(dict(n=S(0), s=S(8), h=S(0x18), f=feats, b=binds, fl=flags))
            mats_f.write(json.dumps(dict(pak=fn, size=len(q.d), nfx=len(q.items_of('EFFECT')), mats=mats)) + '\n')
            for x in q.textures():
                tex_f.write('\t'.join(map(str, [fn, x.get('path', ''), x.get('width', 0), x.get('height', 0),
                                                x.get('format', 0), x.get('mips', 0), x.get('vramOffset', 0),
                                                x.get('vramSize', 0), 0, 0, 0])) + '\n')
            n += 1
        except Exception as ex:          # retail paks the reader does not handle: left out of the survey
            e += 1
            mats_f.write(json.dumps(dict(pak=fn, err=str(ex))) + '\n')
    mats_f.close()
    tex_f.close()
    json.dump(stamp, open(done, 'w'))
    log('level survey: %d paks (%d unreadable)' % (n, e))
    return res_dir


def convert_stage(src, out, ps4_pak_dir, res_dir, work, log=print):
    """one background pak -> out; returns a report dict (approximations, check result)"""
    import mkplan
    import ps4stage
    import stagecheck
    name = os.path.basename(out)[:-4]
    os.makedirs(work, exist_ok=True)
    planf = os.path.join(work, name + '.plan.json')
    reqf = os.path.join(work, name + '.req.json')
    bundf = os.path.join(work, name + '.bundle.json')
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        pl = mkplan.plan(src, (name + '.pak',), res_dir)
    json.dump(pl, open(planf, 'w'), indent=1)
    req = dict(mats=pl['requests']['mats'], tex=pl['requests']['tex'],
               techs=sorted({m[0] for m in pl['requests']['mats']}))
    json.dump(req, open(reqf, 'w'))
    fx = os.path.join(u2paktools.PAKTOOLS, 'stage', 'fxextract.py')
    r = subprocess.run([sys.executable, fx, ps4_pak_dir, reqf, bundf + '.tmp'], env=u2paktools.paktools_env(),
                       capture_output=True, text=True, encoding='utf-8', errors='replace',
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    if r.returncode:
        raise RuntimeError('fxextract: ' + (r.stderr or r.stdout)[-400:])
    os.replace(bundf + '.tmp', bundf)
    bundle = json.load(open(bundf))
    errs = [m for m in bundle['mats'] if 'err' in m or not m.get('rec')]
    if errs:
        raise RuntimeError('effect bundle errors: %s' % str(errs[:2])[:300])
    with contextlib.redirect_stdout(io.StringIO()):
        data, rep = ps4stage.convert(src, pl, bundle, out + '.tmp', verbose=False)
    e, info = stagecheck.check(out + '.tmp', quiet=True)
    # stagecheck expects the U3 retail login order; U2 retail logs in VIS_INFO_1 before GEOMETRY_1 (u2paktools)
    # stagecheck checks the U3 Havok packfile collision; U2 keeps its own layout (u2coll.check_ps4)
    e = [x for x in e if not x.startswith('login order') and 'COLLISION_DATA_HAVOK_BG without trailer' not in x] +         login_order_problems(out + '.tmp') + collision_problems(out + '.tmp')
    plan_lines = [l for l in buf.getvalue().splitlines() if l.strip()]
    rep = dict(approx=rep.get('approx', []), check=e, info=info[:12], plan=plan_lines[:1])
    if e:
        os.remove(out + '.tmp')
        raise RuntimeError('stagecheck: %s' % '; '.join(e[:4]))
    os.replace(out + '.tmp', out)
    return rep


def collision_problems(path):
    import u2coll
    q = Pak(path)
    return ['collision: %s' % x for it in q.items_of('COLLISION_DATA_HAVOK_BG') for x in u2coll.check_ps4(q, it)]


def login_order_problems(path):
    from stagecmp import login_types
    lt = login_types(Pak(path))
    want = [t for t in u2paktools.U2_LOGIN for _ in range(lt.count(t))]
    return [] if lt == want else ['U2 login order %s' % lt]


def convert(src, out, ps4_pak_dir, res_dir, work, drop_navmesh=False, log=print):
    """-> (kind, notes).  Raises Unsupported for what has no converter, RuntimeError when a check fails."""
    try:
        p = Pak(src)
    except ValueError as ex:
        raise Unsupported('pak layout: %s' % ex)
    types = {i.type for i in p.items}
    k = kind(p)
    for t, why in BLOCKERS.items():
        if t in types:
            raise Unsupported(why)
    if k == 'stage':
        if types & {'SPAWNER_GROUP', 'REGION_TREE', 'FEATURE_DB_2'}:
            raise Unsupported('stage pak with level data items (%s)' % ', '.join(sorted(types & DATA_TYPES)))
        rep = convert_stage(src, out, ps4_pak_dir, res_dir, work, log=log)
        notes = rep['plan'] + ['%d approximations' % len(rep['approx'])]
        return k, notes
    if k == 'data':
        import u2ingame
        try:
            notes = u2ingame.convert(src, out + '.tmp', drop_navmesh=drop_navmesh)
        except u2ingame.NavMeshUnsupported as ex:
            raise Unsupported(str(ex))
        Pak(out + '.tmp')                    # re-read: a broken writer result fails here
        os.replace(out + '.tmp', out)
        return k, notes
    raise Unsupported('unknown level pak kind (items: %s)' % ', '.join(sorted(types)))
