#!/usr/bin/env python3
"""mapjob.py <map> <batch> [note]: build (mapbuild) with adaptive texture caps so the whole set stays under
BUDGET bytes (retail Sanctuary set ~75 MB), publish to stages/<batch>, run the dc3 DC update (dcmap.py).
Caps tried in order (PS3-only textures px, lightmaps px): default (256, 4096), (128, 1024), (64, 1024), (64, 512)."""
import json, os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
import mapbuild
BUDGET = 75e6
CAPS = [(256, 4096), (128, 1024), (64, 1024), (64, 512)]
SKIP = 'multi-all,coop-all,coop-shade-all'


def total(key):
    r = json.load(open(os.path.join(mapbuild.WORK, key, 'report.json')))
    return sum(p['size'] for l in r['levels'] for p in l['paks']), r


def run(key, batch, note=''):
    env = dict(os.environ, MAPBUILD_SKIP=SKIP)
    used = None
    for ci, (t, lm) in enumerate(CAPS):
        env['U3_STAGE_MAX_TEX'] = str(t); env['U3_STAGE_MAX_LM'] = str(lm)
        if ci == 0:
            args = []
        else:
            r = json.load(open(os.path.join(mapbuild.WORK, key, 'report.json')))
            args = [l['level'] for l in r['levels'] if any('materials' in p['note'] for p in l['paks'])]
        out = subprocess.run([sys.executable, os.path.join(HERE, 'mapbuild.py'), 'build', key] + args, env=env,
                             capture_output=True, text=True)
        tot, r = total(key)
        errs = [l['level'] for l in r['levels'] if l.get('error')]
        print('%s caps %s: %.1f MB, errors %s' % (key, (t, lm), tot / 1e6, errs), flush=True)
        used = (t, lm)
        if errs or tot <= BUDGET:
            break
    capnote = '' if used == CAPS[0] else (' Size cap applied to the BG levels: PS3-only textures <= %d px, lightmaps <= %d px.' % used)
    rows, tot = mapbuild.cmd_publish(key, batch, note + capnote)
    dc = subprocess.run([sys.executable, '/mnt/user-data/outputs/dc3/tools/dcmap.py', key], capture_output=True, text=True)
    if dc.returncode:
        print(dc.stdout[-2000:], dc.stderr[-2000:]); raise SystemExit('dcmap failed')
    d = json.load(open('/mnt/user-data/outputs/dc3/tools/logs/%s.dcmap.json' % key))
    print(json.dumps(dict(total=tot, caps=used, dc=dict(entries=d['entries'], skipped=d['skipped'],
          map_index=d.get('map_index'), netgames=d.get('netgames'), levels_added=d['levels_added'],
          missing_actors=d['missing_actors'], verify=d.get('verify_net'))), indent=0))
    return tot, used, d


if __name__ == '__main__':
    run(sys.argv[1], sys.argv[2], ' '.join(sys.argv[3:]))


def match_set(key):
    """bytes of the paks a competitive match loads (levels of the dc3 map entries: main, killvis, pak, sub-levels,
    stage, + their ingame12)"""
    r = json.load(open(os.path.join(mapbuild.WORK, key, 'report.json')))
    lg = json.load(open('/mnt/user-data/outputs/dc3/tools/logs/%s.net.json' % key))
    lv = set()
    for m in lg['maps']:
        lv |= {m['main'], m['killvis'], m['pak'], m['stage']} | set(m['sublevels'])
    sizes = {p['name'][:-4]: p['size'] for l in r['levels'] for p in l['paks']}
    tot = sum(v for k, v in sizes.items() if k in lv or (k.endswith('-ingame12') and k[:-9] in lv))
    shared = [x for x in lv if x not in sizes and x + '-ingame12' not in sizes]
    return tot, sorted(lv), shared
