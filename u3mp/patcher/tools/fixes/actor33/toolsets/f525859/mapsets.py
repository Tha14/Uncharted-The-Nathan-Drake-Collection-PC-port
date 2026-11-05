#!/usr/bin/env python3
"""Which level paks, sky, sound banks and actors make up each PS3 1.19 MP / co-op map, where each PS3 pak
comes from, and what PS4 already has.

Sources (PS3 1.19, newest first): container trees dlc-patch18, dlc-patch13 (1.19 patch pkg), the DLC pkgs
dlc-map1..4, then the GOTY disc on the PC: goty-patch.psarc, goty-dlc.psarc, pak23.psarc.
Map level lists: net-patch18.bin (the per-mode {count, (string, sid)[]} lists of the net-levels entries),
plus the stage, the mode packs (-pak / -comp-pak / -hunter-pak) and killvis of the DC map entries
(outputs/dc/tools/maps_status.json, written by the DC agent).
usage: mapsets.py [map ...]   -> prints the plan; mapsets.plan(map) -> dict"""
import json, os, re, sys, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

TREE = '/home/claude/u3/ps3pkg/tree'
TREES = ['dlc-patch18', 'dlc-patch13', 'dlc-map1', 'dlc-map2', 'dlc-map3', 'dlc-map4']
INV = '/mnt/user-data/uploads/Uncharted The Nathan Drake Collection - U3 Multiplayer/work/inventory'
PCL = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/maps'
NET = TREE + '/dlc-patch18/build/main/bin/dc1/net-patch18.bin'
PAK23TXT = TREE + '/dlc-patch18/build/main/pak23.txt'
STATUS = '/mnt/attach/outputs/dc/tools/maps_status.json'

# map key -> (main level names that identify its net lists, extra name patterns)
MAPS = {
    'ice-cave': ['cave-multi'], 'gompa': ['gompa-multi'], 'fort': ['fort-multi'],
    'shambhala': ['shambhala-multi'], 'coop-fort': ['fort-coop-cistern'], 'warzone': ['warzone-multi'],
    'temple': ['temple-multi'], 'village': ['village-multi'], 'train': ['train-wreck-multi'],
    'graveyard': ['grave-multi'], 'oldquarter': ['old-quarter-multi'], 'oasis': ['oasis-multi'],
    'streets': ['ldn-streets-multi'], 'drydocks1': ['graveyard-drydock-anniversary'],
    'yemen': ['yemen-multi'], 'airport': ['air-multi', 'air-multi-shade'], 'cha': ['cha-multi', 'cha-multi-shade'],
    'san': ['san-upper-multi'], 'ldn': ['ldn-train-multi', 'ldn-train-multi-lit'], 'desert': ['desert-multi'],
    'syria': ['syria-multi'], 'ruins': ['ruins-multi'], 'highrise': ['highrise-multi'],
    'facility': ['facility-multi'], 'museum': ['museum-multi'],
    'coop-dig': ['dig-coop-approach'], 'coop-ldn': ['coop-ldn-victorian-station'],
    'coop-mon': ['mon-coop-church'], 'coop-syria': ['coop-syria-persistent'], 'coop-air': ['coop-airport-1'],
    'lab': ['lab-multi-01-version2'],
}
PREVIEW = {'ice-cave': 'ice-cave', 'train': 'train', 'oldquarter': 'oldquarter', 'drydocks1': 'drydocks1'}
SHARED = {'multi-all', 'coop-all', 'coop-shade-all'}


def _ps3_index():
    idx = {}
    for t in TREES:
        for f in glob.glob('%s/%s/build/main/pak23/*.pak' % (TREE, t)):
            idx.setdefault(os.path.basename(f)[:-4], ('tree', f))
    for lst, src in (('goty-patch.list', 'goty-patch.psarc'), ('goty-dlc.list', 'goty-dlc.psarc')):
        fp = os.path.join(PCL, lst)
        if os.path.exists(fp):
            for l in open(fp):
                f = l.split()
                if len(f) == 3 and '/pak23/' in f[2] and f[2].endswith('.pak'):
                    idx.setdefault(os.path.basename(f[2])[:-4], ('pc', src, int(f[1])))
    for l in open(os.path.join(INV, 'ps3-pak23-psarc.txt')):
        f = l.split()
        if len(f) == 3 and f[2].endswith('.pak'):
            idx.setdefault(f[2][:-4], ('pc', 'pak23.psarc', int(f[1])))
    return idx


def _ps4_set():
    s = set()
    for l in open(os.path.join(INV, 'ps4-files.tsv')):
        p = l.split('\t')[0]
        if '/pak23/' in p:
            s.add(os.path.basename(p)[:-4])
    return s


def _ps4_actors():
    s = set()
    for l in open(os.path.join(INV, 'ps4-files.tsv')):
        p = l.split('\t')[0]
        if '/actor33/' in p:
            s.add(os.path.basename(p)[:-4])
    return s


def _converted_actors():
    s = set()
    for f in glob.glob('/mnt/user-data/outputs/**/actor33/**/*.pak', recursive=True):
        s.add(os.path.basename(f)[:-4])
    return s


def net_lists():
    from dc import DC
    c = DC(NET)
    out = []
    for p in sorted(c.ptrs):
        t = c.ptr(p)
        n = c.u32(p - 4) if p >= 4 else 0
        if not 0 < n < 64:
            continue
        names = []
        for k in range(n):
            q = t + k * 8
            if q not in c.ptrs:
                names = None
                break
            s = c.cstr(c.ptr(q))
            if not s or not all(32 < ord(ch) < 127 for ch in s):
                names = None
                break
            names.append(s)
        if names and any('-' in s for s in names) and p < 0x9000:
            out.append(names)
    return out


def level_defs():
    """pak23.txt -> {level: {'packages': [...], 'banks': [...], 'actors': [...], 'lines': [...]}}"""
    defs = {}
    cur = None
    for l in open(PAK23TXT):
        f = l.split()
        if not f:
            continue
        if f[0] == 'level-name':
            cur = defs.setdefault(f[1], dict(packages=[], banks=[], actors=[], lines=[]))
        if cur is None:
            continue
        cur['lines'].append(l.rstrip('\n'))
        if len(f) < 2:
            continue
        if f[0] == 'package':
            cur['packages'].append(f[1])
        elif f[0] == 'sound-bank':
            cur['banks'].append(f[1])
        elif f[0] == 'actor':
            cur['actors'].append(f[1])
    return defs


def plan(key, _cache={}):
    if not _cache:
        _cache['lists'] = net_lists()
        _cache['defs'] = level_defs()
        _cache['ps3'] = _ps3_index()
        _cache['ps4'] = _ps4_set()
        _cache['ps4act'] = _ps4_actors()
        _cache['conv'] = _converted_actors()
        _cache['status'] = json.load(open(STATUS)) if os.path.exists(STATUS) else []
    L, D, P3, P4 = _cache['lists'], _cache['defs'], _cache['ps3'], _cache['ps4']
    keys = MAPS[key]
    lists = [l for l in L if any(k in l for k in keys)]
    if key == 'lab':
        lists = [l for l in L if any(x.startswith('lab-multi') for x in l)]
    levels = []
    for l in lists:
        for n in l:
            if n not in levels:
                levels.append(n)
    prev = 'net-levels/' + PREVIEW.get(key, key)
    entries = [e for e in _cache['status'] if e['preview'] == prev or
               (key == 'ldn' and e['preview'] in ('net-levels/ldn', 'net-levels/ldn-tdm')) or
               (key == 'airport' and e['preview'] == 'net-levels/airport-tdm') or   # 4f90c260 (yemen-airport-multi-pak, TDM playlist)
               (key == 'lab' and e['preview'] == 'net-levels/lab-stage')]
    stage = None
    for e in entries:
        for n in (e['pack'], e.get('killvis')):
            if n and n not in levels:
                levels.append(n)
        stage = e['stage']
    if stage and stage not in levels:
        levels.insert(0, stage)
    rows = []
    for n in levels:
        if n in SHARED:
            kind = 'shared'
        elif n.startswith('sky-'):
            kind = 'sky'
        else:
            kind = 'level'
        d = D.get(n, {})
        has_ig = any(pk == n + '-ingame' for pk in d.get('packages', []))
        src = P3.get(n)
        srci = P3.get(n + '-ingame12')
        rows.append(dict(level=n, kind=kind, on_ps4=n in P4, ig_on_ps4=(n + '-ingame12') in P4,
                         has_def=n in D, ingame=has_ig, src=src, src_ig=srci))
    banks, actors = [], []
    for r in rows:
        for b in D.get(r['level'], {}).get('banks', []):
            if b not in banks:
                banks.append(b)
        for a in D.get(r['level'], {}).get('actors', []):
            if a not in actors:
                actors.append(a)
    miss_act = [a for a in actors if a not in _cache['ps4act'] and a not in _cache['conv']]
    return dict(map=key, stage=stage, rows=rows, banks=banks, actors=actors, missing_actors=miss_act,
                entries=[e['map_id'] for e in entries])


if __name__ == '__main__':
    for k in sys.argv[1:] or MAPS:
        pl = plan(k)
        print('== %s (stage %s, DC entries %s)' % (k, pl['stage'], ' '.join(pl['entries'])))
        for r in pl['rows']:
            s = r['src']
            print('   %-36s %-6s ps4:%-5s ig:%-5s def:%-5s src:%s' % (
                r['level'], r['kind'], r['on_ps4'], r['ingame'], r['has_def'],
                ('%s %s' % (s[1].split('/build/')[0].split('/')[-1] if s[0] == 'tree' else s[1],
                            os.path.getsize(s[1]) if s[0] == 'tree' else s[2])) if s else None))
        print('   banks:', ' '.join(pl['banks']))
        print('   actors: %d, missing on PS4 and not converted: %d %s' % (
            len(pl['actors']), len(pl['missing_actors']), ' '.join(pl['missing_actors'][:40])))
