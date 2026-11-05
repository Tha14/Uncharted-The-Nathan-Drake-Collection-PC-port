#!/usr/bin/env python3
"""mapb.py <map> [...]   MAPS-B driver for the competitive base / GOTY maps (PS3 sources only on the PC).
Per map (mapsets.plan rows, shared multi-all / coop-all / coop-shade-all skipped):
  main / ingame12 with source 'pc'   -> converted on the PC by pcbuild.py, staged to STAGED/build/<map>/
  main / ingame12 with source 'tree' -> converted here: BG via ps4stage (bundle from STAGED/bundle/<map>/ or
                                        WORK/<map>/), killvis via vischeat, other empty levels -> empty PS4 pak,
                                        ingame12 via ps4ingame
Then crashcheck + ps4verify (+ stagecheck for BG) on every pak, size total vs BUDGET, publish to
OUT/<map>/u3data/build/main/pak23/ps4/ and write OUT/<map>/dc.json + pak23.add.txt (level blocks to add to
pak23_ps4.txt, computed against the current dc3 files without touching them; MAPS-A merges).
Missing staged inputs are listed as 'pending' (rerun when they land)."""
import glob, json, os, shutil, subprocess, sys, time, traceback
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, 'stage'))
import mapsets, mapbuild
from ndpak import Pak

STAGED = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/maps'
WORK = '/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/mapsb'
OUT = '/mnt/user-data/outputs/maps-b'
REQ7 = '/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/vis/req7'
SHARED = {'multi-all', 'coop-all', 'coop-shade-all'}
BUDGET = 130e6        # v7: full-res maps; warning only (heap: 135 x 2 MB pages, lobby ~28 used)
DC3 = '/mnt/user-data/outputs/dc3/u3data/build/main'
DC2 = '/mnt/user-data/outputs/dc2/u3data/build/main'
P3TXT = '/home/claude/u3/ps3pkg/tree/dlc-patch18/build/main/pak23.txt'


def cur(rel):
    p = os.path.join(DC3, rel)
    return p if os.path.exists(p) else os.path.join(DC2, rel)


def staged(key, name):
    for d in (os.path.join(WORK, key, 'pc'), os.path.join(STAGED, 'build7', key), os.path.join(STAGED, 'build', key),
              os.path.join(OUT, key, 'u3data/build/main/pak23/ps4')):   # local rebuilds, v7, v6, then the published copy (non-BG levels)
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    return None


def bundle(key, lv):
    for d in (os.path.join(STAGED, 'bundle7', key), os.path.join(STAGED, 'bundle', key), os.path.join(WORK, key), os.path.join(mapbuild.WORK, key)):
        p = os.path.join(d, lv + '.bundle.json')
        if os.path.exists(p):
            return p
    return None


def plan_file(key, lv):
    for d in (os.path.join(REQ7, key), os.path.join(STAGED, 'req7', key), os.path.join(WORK, key), os.path.join(mapbuild.WORK, key), os.path.join(STAGED, 'req', key)):
        p = os.path.join(d, lv + '.plan.json')
        if os.path.exists(p):
            return p
    return None


def pc_caps(key):
    f = os.path.join(STAGED, 'build', key, 'report.json')
    try:
        return json.load(open(f)).get('caps')
    except Exception:
        return None


def entry_sets(key):
    """PS3 1.19 net-map entries of the map: {id: [levels loaded by that entry]} (stage, main pak, killvis, sub-levels).
    A shade entry swaps the main BG level for the -shade one, so the budget is per entry, not per map."""
    sys.path.insert(0, '/mnt/user-data/outputs/dc3/tools')
    import build_map
    from common import DC
    b = DC('/home/claude/u3/ps3pkg/tree/dlc-patch18/build/main/bin/dc1/net.bin')
    o3, n3, arr3 = build_map.table(b, build_map.MAPS)
    out = {}
    for mid in mapsets.plan(key)['entries']:
        m = int(mid, 16)
        src = next((arr3 + i * 0x58 for i in range(n3) if b.u32(arr3 + i * 0x58) == m), None)
        if src is None:
            continue
        cs = lambda k: b.cstr(b.ptr(src + k)) if b.isptr(src + k) else ''
        L = b.ptr(src + 0x30); n = b.u32(L); li = b.ptr(L + 4)
        lv = [cs(0x48), cs(0x18), cs(0x20)] + [b.cstr(b.ptr(li + 8 * k)) for k in range(n)]
        out[mid] = [x for x in lv if x and x not in SHARED]
    return out


def conv_main_tree(key, lv, sp, op):
    import ps4stage, vischeat
    k, p = mapbuild.kind_of(sp)
    if k == 'bg':
        pf, bf = plan_file(key, lv), bundle(key, lv)
        if not pf:
            import mkplan
            P = mkplan.plan(sp, (), mapbuild.RES)
            os.makedirs(os.path.join(WORK, key), exist_ok=True)
            pf = os.path.join(WORK, key, lv + '.plan.json'); json.dump(P, open(pf, 'w'), indent=1)
            json.dump(dict(mats=P['requests']['mats'], tex=P['requests']['tex'],
                           techs=sorted({m[0] for m in P['requests']['mats']})),
                      open(os.path.join(WORK, key, lv + '.req.json'), 'w'))
        if not bf:
            return 'pending', 'bundle %s.bundle.json (fxextract on the PC; req in %s)' % (lv, os.path.join(WORK, key))
        P = json.load(open(pf)); B = json.load(open(bf))
        # v7: no texture caps (full PS3 resolution); the old per-map PC caps are ignored
        bad = [m for m in B['mats'] if 'err' in m or not m.get('rec')]
        if bad:
            raise RuntimeError('bundle errors %s' % bad[:2])
        d, r = ps4stage.convert(sp, P, B, op, verbose=False)
        if r.get('capped'):
            raise RuntimeError('V4 %d textures capped: %s' % (len(r['capped']), r['capped'][:3]))
        return 'ok', '%d materials, %d textures (BG)' % (r['materials'], r['textures'])
    if k == 'empty' and p.items_of('VIS_CHEAT'):
        vischeat.convert(sp, op)
        return 'ok', 'VIS_CHEAT converted (vischeat)'
    if k in ('empty', 'nogeo'):
        shutil.copyfile(mapbuild.EMPTY, op)
        return 'ok', 'empty PS4 level (PS3 %s)' % k
    raise RuntimeError('unsupported level content ' + k)


def run(key):
    import ps4ingame, crashcheck
    ps4ingame.IGNORE_OTHERS = True
    pl, rows = mapbuild.todo(key)
    wd = os.path.join(WORK, key); od = os.path.join(wd, 'out'); os.makedirs(od, exist_ok=True)
    rep = dict(map=key, stage=pl['stage'], entries=pl['entries'], levels=[], pending=[], errors=[])
    for r in rows:
        lv = r['level']
        if lv in SHARED:
            continue
        ent = dict(level=lv, paks=[])
        for part, src, on4, name in (('main', r['src'], r['on_ps4'], lv + '.pak'),
                                     ('ig', r['src_ig'], r['ig_on_ps4'] or not r['ingame'], lv + '-ingame12.pak')):
            if on4 or src is None:
                continue
            op = os.path.join(od, name)
            try:
                if src[0] == 'pc':
                    sp = staged(key, name)
                    if not sp:
                        rep['pending'].append('PC build %s' % name); continue
                    shutil.copyfile(sp, op); note = 'PC build (pcbuild.py)'
                else:
                    sp = src[1]
                    if part == 'main':
                        st, note = conv_main_tree(key, lv, sp, op)
                        if st == 'pending':
                            rep['pending'].append(note); continue
                    else:
                        ps4ingame.convert(sp, op); note = 'ps4ingame (container tree)'
                e, info = crashcheck.check(op)
                e += mapbuild.check_pak(op, 'BG' in note or 'materials' in note)
                ent['paks'].append(dict(name=name, size=os.path.getsize(op), errs=e, note=note))
                if e:
                    rep['errors'].append('%s: %s' % (name, e[:3]))
            except Exception as ex:
                rep['errors'].append('%s: %r' % (name, ex))
                ent.setdefault('trace', []).append(traceback.format_exc()[-1200:])
        rep['levels'].append(ent)
    rep['total'] = sum(p['size'] for e in rep['levels'] for p in e['paks'])
    size = {p['name'][:-4]: p['size'] for e in rep['levels'] for p in e['paks']}
    rep['entries_mb'] = {}
    for mid, lvs in entry_sets(key).items():
        rep['entries_mb'][mid] = round(sum(size.get(l, 0) + size.get(l + '-ingame12', 0) for l in lvs) / 1e6, 1)
    rep['in_match_mb'] = max(rep['entries_mb'].values()) if rep['entries_mb'] else round(rep['total'] / 1e6, 1)
    rep['complete'] = not rep['pending'] and not rep['errors']
    rep['over_budget'] = rep['in_match_mb'] * 1e6 > BUDGET
    json.dump(rep, open(os.path.join(wd, 'report.json'), 'w'), indent=1)
    return rep


def dc_entry(key, rep):
    """pak23_ps4.txt blocks for the map's new levels (against the current dc3 files, nothing installed)."""
    sys.path.insert(0, '/mnt/user-data/outputs/dc3/tools')
    import build_map_levels
    pl = mapsets.plan(key)
    levels = [r['level'] for r in pl['rows'] if r['level'] not in SHARED and not r['on_ps4']]
    wd = os.path.join(WORK, key)
    # levels the PS3 1.19 pak23.txt does not define (DLC-tree packages ship their own <level>.txt beside the pak):
    # append those blocks to a private copy of the PS3 list so build_map_levels can use them
    p3 = P3TXT
    extra = []
    for r in pl['rows']:
        if r['level'] in levels and not r['has_def'] and r['src'] and r['src'][0] == 'tree':
            tx = r['src'][1][:-4] + '.txt'
            if os.path.exists(tx):
                body = open(tx, 'rb').read().decode('latin1').replace('\r', '').strip('\n')
                extra.append('level-name %s\n%s' % (r['level'], body))
    if extra:
        os.makedirs(wd, exist_ok=True)
        p3 = os.path.join(wd, 'pak23-ps3-plus.txt')
        base = open(P3TXT, 'rb').read().decode('latin1').replace('\r', '').rstrip('\n')
        open(p3, 'wb').write(('\n'.join([base] + extra) + '\n').encode('latin1'))
    ll = build_map_levels.main(cur('pak23_ps4.txt'), p3, cur('dc1/ps4/modules.bin'), os.path.join(wd, 'pak23.tmp'),
                               os.path.join(wd, 'levels.json'), *levels)
    add = '\r\n'.join(l for a in ll['added'] for l in a['lines']) + '\r\n'
    return dict(map=key, stage=pl['stage'], net_map_entries=pl['entries'],
                net_map_entries_note='base-game map entries and competitive netgames already exist in PS4 net.bin; '
                                     'shade (type 8) / hunter (17/18) netgames are MAPS-A (ngconv)',
                levels_added=[dict(level=a['level'], before=a['before'], dropped=a['dropped']) for a in ll['added']],
                levels_already=ll['already'], missing_actors=ll['missing_actors'],
                banks=pl['banks'], total_bytes=rep['total'], in_match_mb_per_entry=rep['entries_mb'],
                over_budget=rep['over_budget']), add


def publish(key, rep):
    dd = os.path.join(OUT, key, 'u3data', 'build', 'main', 'pak23', 'ps4'); os.makedirs(dd, exist_ok=True)
    for e in rep['levels']:
        for p in e['paks']:
            if not p['errs']:
                shutil.copyfile(os.path.join(WORK, key, 'out', p['name']), os.path.join(dd, p['name']))
    dc, add = dc_entry(key, rep)
    json.dump(dc, open(os.path.join(OUT, key, 'dc.json'), 'w'), indent=1)
    open(os.path.join(OUT, key, 'pak23.add.txt'), 'wb').write(add.encode('latin1'))
    json.dump(rep, open(os.path.join(OUT, key, 'report.json'), 'w'), indent=1)


if __name__ == '__main__':
    for k in sys.argv[1:]:
        rep = run(k)
        print('%-11s %d paks %.1f MB, per entry %s, complete=%s over=%s pending=%d errors=%s' % (
            k, sum(len(e['paks']) for e in rep['levels']), rep['total'] / 1e6, rep['entries_mb'], rep['complete'],
            rep['over_budget'], len(rep['pending']), rep['errors'][:3]), flush=True)
        if rep['complete']:                    # over-budget maps are published too, flagged in report/dc.json
            publish(k, rep)
            print('  published', os.path.join(OUT, k), flush=True)
