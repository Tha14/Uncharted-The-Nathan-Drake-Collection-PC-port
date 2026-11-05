#!/usr/bin/env python3
"""Build every missing PS4 pak of one MP / co-op map (mapsets.plan), in three phases:
  req <map>      write work/<map>/<level>.plan.json + .req.json for every BG level (mkplan) and a
                 fetch list of PS3 paks that live only on the PC (goty psarcs / pak23.psarc)
  build <map>    convert: BG levels via ps4stage (needs work/<map>/<level>.bundle.json from fxextract on the
                 PC), empty levels (killvis = VIS_CHEAT only, -pak/-npc paks) as the 248-byte empty PS4 pak,
                 every -ingame12 via ps4ingame; run validate / stagecheck / ps4verify; write report.json
  publish <map> <batch>   copy to /mnt/user-data/outputs/stages/<batch>/u3data/build/main/pak23/ps4 + READY rows
PS3 sources: mapsets._ps3_index (container trees, else PCDIR/<map>/ fetched from the PC)."""
import json, os, shutil, struct, sys, time, traceback
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, 'stage'))
import mapsets
from ndpak import Pak

WORK = os.environ.get('MAPBUILD_WORK', '/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/maps')
PCDIR = WORK + '/_ps3'
RES = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/stages'
EMPTY = os.path.join(HERE, 'data', 'empty-level.pak')
OUTROOT = '/mnt/user-data/outputs/stages'
BG_TYPES = {'BACKGROUND_SECTION'}
EMPTY_OK = {'VIS_CHEAT', 'PAK_LOGIN_TABLE', 'VRAM_DESC_TABLE'}


def src_path(key, name, s):
    if s is None:
        return None
    if s[0] == 'tree':
        return s[1]
    import glob
    p = os.path.join(PCDIR, key, name + '.pak')
    if os.path.exists(p):
        return p
    g = glob.glob(os.path.join(PCDIR, '*', name + '.pak'))
    return g[0] if g else None


def todo(key, skip_shared=()):
    pl = mapsets.plan(key)
    rows = []
    for r in pl['rows']:
        if r['on_ps4'] and (r['ig_on_ps4'] or not r['ingame']):
            continue
        if r['kind'] == 'shared' and r['level'] in skip_shared:
            continue
        rows.append(r)
    return pl, rows


def kind_of(path):
    p = Pak(path)
    ts = {i.type for i in p.items} - {'RAW_DATA', 'VRAM_DESC'}
    if ts & BG_TYPES and not p.items_of('PM_COLLECTION_2') and not p.items_of('WATER_DATA'):
        return 'nogeo', p                   # no prototypes and no water: empty PS4 level
    # water-only levels (WATER_DATA, no prototypes) convert like a BG level: ps4stage writes the water surface,
    # a dummy prototype for its material and the collision (retail grave-water layout)
    if ts & BG_TYPES:
        return 'bg', p
    if ts <= EMPTY_OK:
        return 'empty', p
    return 'other:' + ','.join(sorted(ts - EMPTY_OK)), p


def cmd_req(key, skip_shared=()):
    import mkplan
    pl, rows = todo(key, skip_shared)
    wd = os.path.join(WORK, key); os.makedirs(wd, exist_ok=True)
    fetch, reqs = [], []
    for r in rows:
        for nm, s in ((r['level'], r['src']), (r['level'] + '-ingame12', r['src_ig'])):
            if s and s[0] == 'pc' and not src_path(key, nm, s):
                fetch.append((s[1], nm + '.pak', s[2]))
    for r in rows:
        sp = src_path(key, r['level'], r['src'])
        if sp is None:
            continue
        k, p = kind_of(sp)
        if k != 'bg':
            continue
        planf = os.path.join(wd, r['level'] + '.plan.json')
        if not os.path.exists(planf):
            P = mkplan.plan(sp, (), RES)
            json.dump(P, open(planf, 'w'), indent=1)
            req = dict(mats=P['requests']['mats'], tex=P['requests']['tex'],
                       techs=sorted({m[0] for m in P['requests']['mats']}))
            json.dump(req, open(os.path.join(wd, r['level'] + '.req.json'), 'w'))
        if not os.path.exists(os.path.join(wd, r['level'] + '.bundle.json')):
            reqs.append(r['level'])
    json.dump(dict(fetch=fetch, reqs=reqs), open(os.path.join(wd, 'req.json'), 'w'), indent=1)
    return fetch, reqs


def check_pak(path, bg):
    import ps4verify, stagecheck
    errs = []
    e, notes = ps4verify.verify(path, quiet=True)
    errs += [x for x in e if 'ARTGROUP_DATA items' not in x]      # level paks carry no artgroup
    if bg:
        e2, info = stagecheck.check(path, quiet=True)
        errs += e2
    return errs


def cmd_build(key, skip_shared=(), only=None):
    import ps4stage, ps4ingame
    ps4ingame.IGNORE_OTHERS = True
    pl, rows = todo(key, skip_shared)
    wd = os.path.join(WORK, key); out = os.path.join(wd, 'out'); os.makedirs(out, exist_ok=True)
    rep = dict(map=key, levels=[], banks=pl['banks'], missing_actors=pl['missing_actors'], stage=pl['stage'],
               entries=pl['entries'])
    for r in rows:
        if only and r['level'] not in only:
            continue
        lv = r['level']
        ent = dict(level=lv, kind=r['kind'], paks=[])
        try:
            sp = src_path(key, lv, r['src'])
            if not r['on_ps4']:
                if sp is None:
                    raise RuntimeError('no PS3 source for %s' % lv)
                k, p = kind_of(sp)
                op = os.path.join(out, lv + '.pak')
                t0 = time.time()
                if k == 'bg':
                    planf = os.path.join(wd, lv + '.plan.json'); bf = os.path.join(wd, lv + '.bundle.json')
                    P = json.load(open(planf)); B = json.load(open(bf))
                    bad = [m for m in B['mats'] if 'err' in m or not m.get('rec')]
                    if bad:
                        raise RuntimeError('bundle errors %s' % bad[:2])
                    data, srep = ps4stage.convert(sp, P, B, op, verbose=False)
                    import paramfix                     # neutralise foreign template tints/scroll/uv (stage/paramfix.py)
                    srep['paramfix'] = len(paramfix.fix(op, P))
                    import cubefix                      # PS3 cubes ps4stage could not convert natively
                    srep['cubefix'] = cubefix.fix(op, sp)
                    srep['seconds'] = round(time.time() - t0, 1)
                    json.dump(srep, open(os.path.join(wd, lv + '.report.json'), 'w'), indent=1)
                    note = '%d materials (%d approximated), %d textures, vram %.1fMB%s%s' % (
                        srep['materials'], len(srep['approx']), srep['textures'], srep['vram'] / 1e6,
                        ', VIS %dKB' % (srep['vis'] // 1024) if srep.get('vis') else '',
                        ', %d collision items' % srep.get('collision_items', 0) if srep.get('collision_items') else '')
                    dropped = sorted({i.type for i in p.items} - {'RAW_DATA', 'VRAM_DESC', 'PAK_LOGIN_TABLE',
                        'VRAM_DESC_TABLE', 'BACKGROUND_SECTION', 'MATERIAL_TABLE_1', 'NAME_TABLE_1',
                        'SPHERE_TREE_NODE_1', 'INSTANCE_TAGS_1', 'PM_COLLECTION_2', 'LIGHT_TABLE_1',
                        'COLLISION_DATA_HAVOK_BG', 'VIS_INFO_1', 'DEBUG_INFO_1', 'LEVEL_INFO_1', 'LEVEL_PATS_1',
                        'ATTACHED_FG_MAPPING'})
                    if dropped:
                        note += '; not converted: ' + ','.join(dropped)
                elif k == 'nogeo':
                    shutil.copyfile(EMPTY, op)
                    note = ('PLACEHOLDER: PS3 level has no geometry, only %s; written as an empty PS4 level so the map '
                            'loads (WATER_DATA conversion pending: water surface / swim volume missing)' %
                            ','.join(sorted({i.type for i in p.items} & {'WATER_DATA', 'COLLISION_DATA_HAVOK_BG',
                                                                          'LIGHT_TABLE_1'})))
                elif k == 'empty':
                    shutil.copyfile(EMPTY, op)
                    note = 'empty PS4 level pak (PS3 %s; retail ships these as stubs)' % (
                        'VIS_CHEAT only' if p.items_of('VIS_CHEAT') else 'empty')
                else:
                    raise RuntimeError('unsupported level content %s' % k)
                errs = check_pak(op, k == 'bg')
                ent['paks'].append(dict(name=lv + '.pak', size=os.path.getsize(op), ps3=os.path.getsize(sp),
                                        errs=errs, note=note, src=r['src'][1] if r['src'][0] == 'pc' else
                                        r['src'][1].split('/build/')[0].split('/')[-1]))
            if r['ingame'] and not r['ig_on_ps4']:
                ip = src_path(key, lv + '-ingame12', r['src_ig'])
                if ip is None:
                    raise RuntimeError('no PS3 ingame12 for %s' % lv)
                q = Pak(ip)
                handled = set(['TAG_INT', 'SPLINE_TABLE', 'FEATURE_DB_2', 'NAV_MESH_1', 'LEVEL_INFO_1',
                               'SPAWNER_GROUP', 'REGION_TREE', 'PAK_LOGIN_TABLE', 'VRAM_DESC_TABLE'])
                dropped = sorted({i.type for i in q.items} - handled)
                op = os.path.join(out, lv + '-ingame12.pak')
                ps4ingame.convert(ip, op)
                errs = check_pak(op, False)
                ts = {}
                for i in Pak(op).items:
                    ts[i.type] = ts.get(i.type, 0) + 1
                note = ' '.join('%s%s' % (t, ('x%d' % n) if n > 1 else '') for t, n in sorted(ts.items()))
                if dropped:
                    note += '; dropped: ' + ','.join(dropped)
                ent['paks'].append(dict(name=lv + '-ingame12.pak', size=os.path.getsize(op), ps3=os.path.getsize(ip),
                                        errs=errs, note=note))
        except Exception as ex:
            ent['error'] = repr(ex)
            ent['trace'] = traceback.format_exc()[-1500:]
        rep['levels'].append(ent)
        print('%-34s %s' % (lv, ent.get('error') or ' '.join('%s %.2fMB %s' % (x['name'], x['size'] / 1e6,
              'OK' if not x['errs'] else x['errs'][:2]) for x in ent['paks'])), flush=True)
    rf = os.path.join(wd, 'report.json')
    if only and os.path.exists(rf):
        old = json.load(open(rf))
        new = {e['level']: e for e in rep['levels']}
        rep['levels'] = [new.pop(e['level'], e) for e in old['levels']] + list(new.values())
    json.dump(rep, open(rf, 'w'), indent=1)
    return rep


def dc_needs(key, rep):
    pl = mapsets.plan(key)
    D = mapsets._cache_defs() if hasattr(mapsets, '_cache_defs') else mapsets.level_defs()
    lv = [r['level'] for r in pl['rows']]
    pk = sorted({os.path.basename(r['src'][1]).split('.')[0] if r['src'] and r['src'][0] == 'pc' else
                 (r['src'][1].split('/build/')[0].split('/')[-1] if r['src'] else '?') for r in pl['rows']
                 if r['kind'] != 'shared'})
    s = ('DC: done by the map agent in outputs/dc3 (dcmap.py; see dc3 CHANGES.md and tools/logs/%s.dcmap.json). '
         'Reference (%s): pak23_ps4.txt level definitions (copy the PS3 1.19 pak23.txt blocks: level-name/level-id/'
         'package <level>[-ingame]/sound-bank/actor lines) for: %s; map list entries: %s (stage %s); net games: the '
         'PS3-only netgames of those map ids; DLC pack / source: %s; sound banks referenced: %s (PS3 SCREAM banks are '
         'big-endian with PS3 audio; PS4 banks are LE re-encodes, not converted - reuse PS4 copies where retail '
         'has them); levels already on PS4: %s' % (
             key, key, ' '.join(lv), ' '.join(pl['entries']) or '(none)', pl['stage'], ','.join(pk),
             ' '.join(pl['banks']) or '-', ' '.join(r['level'] for r in pl['rows'] if r['on_ps4']) or 'none'))
    return s


def cmd_publish(key, batch, note=''):
    wd = os.path.join(WORK, key)
    rep = json.load(open(os.path.join(wd, 'report.json')))
    od = os.path.join(OUTROOT, batch, 'u3data', 'build', 'main', 'pak23', 'ps4')
    os.makedirs(od, exist_ok=True)
    rows = []
    total = ps3t = 0
    for e in rep['levels']:
        if e.get('error'):
            rows.append('%s\t%s\t-\tFAILED\t%s' % (batch, e['level'], e['error']))
            continue
        for x in e['paks']:
            if x['errs']:
                raise SystemExit('%s has errors %s' % (x['name'], x['errs'][:3]))
            shutil.copyfile(os.path.join(wd, 'out', x['name']), os.path.join(od, x['name']))
            total += x['size']; ps3t += x['ps3']
            chk = 'validate OK, ps4verify OK' + (', stagecheck OK' if 'materials' in x['note'] else '')
            rows.append('%s\t%s\t%s\t%s, size %.2fMB (PS3 %.2fMB)\t%s' % (
                batch, x['name'][:-4], x.get('src', e['kind']), chk, x['size'] / 1e6, x['ps3'] / 1e6, x['note']))
    rows.append('%s\tTOTAL %s set\t%s\t%d paks, %.1fMB (PS3 %.1fMB)\t%s %s' % (
        batch, key, rep['stage'], sum(len(e['paks']) for e in rep['levels']), total / 1e6, ps3t / 1e6,
        note, dc_needs(key, rep)))
    rf = os.path.join(OUTROOT, 'READY.tsv')
    keep = [l for l in open(rf) if not l.startswith(batch + '\t')] if os.path.exists(rf) else []
    with open(rf, 'w') as f:
        f.writelines(keep)
        for r in rows:
            f.write(r + '\n')
    return rows, total


if __name__ == '__main__':
    c, key = sys.argv[1], sys.argv[2]
    skip = tuple(os.environ.get('MAPBUILD_SKIP', '').split(',')) if os.environ.get('MAPBUILD_SKIP') else ()
    if c == 'req':
        f, r = cmd_req(key, skip)
        print('fetch', f); print('reqs', r)
    elif c == 'build':
        cmd_build(key, skip, sys.argv[3:] or None)
    elif c == 'publish':
        rows, total = cmd_publish(key, sys.argv[3], ' '.join(sys.argv[4:]))
        print('\n'.join(rows)); print('total %.1fMB' % (total / 1e6))
