#!/usr/bin/env python3
"""Make the provenance recipes (dcrecipe.py) of every overlay DC file the converter does not reproduce exactly.

  python make_recipes.py <project> <recipe dir> <ps4 game> <ps3 disc USRDIR> <ps3 update USRDIR> <ps3 data2 USRDIR> <out.json.gz>

Targets: the manifest's dc1 / dc2 files of overlay-r3 / r2 / port, minus the files convert-dc1 writes exactly
(fixes/dcconv/dc1-convert.tsv 'identical' rows and their .dci) and skel-retargets.bin (airport-igc3).
Sources of a target: the PS4 retail file of the same path and every PS3 layer of it (dc2/part-module/x = PS3 dc1/x);
when more than 64 bytes stay literal, the PS4 / PS3 net, net-always-loaded, menu, modules, medals and weapon modules
are added. Each recipe is applied once right away (self-check). Writes one gzip'd json {overlay/path: recipe}."""
import gzip, hashlib, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dcrecipe
from ps3src import Index

P, RD, PS4, DISC, UPD, DATA2, OUT = sys.argv[1:8]
MAIN4 = PS4 + '/u3data/build/main/'
idx = Index(DISC, UPD, DATA2)
conv = set()
for l in open(os.path.join(HERE, '..', 'fixes', 'dcconv', 'dc1-convert.tsv'), encoding='utf-8'):
    if l.startswith('#'): continue
    ov, rel, layer, want, st = l.rstrip('\n').split('\t')
    if st == 'identical':
        conv.add((ov, rel))
        if '/ss-multiplayer/' in '/' + rel: conv.add((ov, rel[:-4] + '.dci'))
EXTRA = ['dc1/ps4/net.bin', 'dc1/ps4/net-always-loaded.bin', 'dc1/ps4/menu.bin', 'dc1/ps4/modules.bin', 'dc1/ps4/medals.bin',
         'dc1/ps4/weapon-art.bin', 'dc1/ps4/weapon-gameplay.bin', 'dc1/ps4/net-weapon-gameplay.bin']


def ps3rel(rel):
    r = rel.replace('dc2/part-module/', 'dc1/ps4/')
    for suf in ('.bak-coopvo', '.bak-coopdesc', '.bak-hairkey'):
        if r.endswith(suf): r = r[:-len(suf)]
    return r


def family(rel):
    """related modules whose data the cloud merges copied: other voice modules for vox*, all particle modules for dc2"""
    b = os.path.basename(ps3rel(rel))
    if b.startswith('vox'):
        return ['dc1/ps4/' + x for x in ('vox.bin', 'vox-mp.bin', 'vox-igc.bin', 'vox-coop.bin')]
    if b in ('net.bin', 'weapon-art.bin'):
        # skin material names 'path:name,guid,prime-shader.fx' come from the PS4 retail weapon actor paks
        return ['actor33/ps4/' + w for w in ('ak74-mp.pak', 'dragunov-mp.pak', 'fn-fal-mp.pak', 'galil-mp.pak', 'hk416-mp.pak',
                'machinegun-mp.pak', 'p99-mp.pak', 'turret-gatling-tripod-mp.pak', 'gaz-van-grenade-launcher.pak')]
    if rel.startswith('dc2/'):
        names = set(n for n in idx.names() if os.path.basename(n).startswith('part-') and n.endswith('.bin'))
        if os.path.isdir(MAIN4 + 'dc2/part-module'):
            names |= {'dc2/part-module/' + f for f in os.listdir(MAIN4 + 'dc2/part-module') if f.endswith('.bin')}
        return sorted(names)
    return []


def sources_for(rel, extra=False):
    s = {}
    rels = [rel] + family(rel) + (EXTRA if extra else [])
    for r in rels:
        r4 = r
        for suf in ('.bak-coopvo', '.bak-coopdesc', '.bak-hairkey'):
            if r4.endswith(suf): r4 = r4[:-len(suf)]
        if os.path.exists(MAIN4 + r4): s['ps4:' + r4] = open(MAIN4 + r4, 'rb').read()
        for ly, _, _ in idx.layers(ps3rel(r)):
            s['ps3/%s:%s' % (ly, ps3rel(r))] = idx.read(ps3rel(r), ly)
    return s


ONLY = os.environ.get('ONLY')
out = json.load(gzip.open(OUT, 'rt', encoding='utf-8')) if ONLY and os.path.exists(OUT) else {}
tot = dict(files=0, bytes=0, literal=0)
rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(RD, 'manifest.tsv'), encoding='utf-8')][1:]
for ov, path, size, want, _ in rows:
    if '/dc1/' not in path and '/dc2/' not in path: continue
    if ov not in ('overlay-r3', 'overlay-r2', 'overlay-port'): continue
    rel = path.split('u3data/build/main/')[1]
    if (ov, rel) in conv or rel.endswith('skel-retargets.bin'): continue
    if ONLY and not __import__('re').search(ONLY, rel): continue
    t = open(P + '/work/' + ov + '/' + path, 'rb').read()
    if hashlib.sha256(t).hexdigest() != want: print('SKIP (overlay differs from manifest)', ov, rel); continue
    src = sources_for(rel)
    rec = dcrecipe.make(t, src)
    if rec['literal_bytes'] > 64 and '/ss-multiplayer/' not in '/' + rel:
        src = sources_for(rel, True); rec = dcrecipe.make(t, src)
    dcrecipe.apply(rec, src)                         # raises if not exact
    rec['used'] = sorted({op[1] for op in rec['ops'] if op[0] == 'c'})
    rec['sources'] = {k: v for k, v in rec['sources'].items() if k in rec['used']}
    out[ov + '/' + rel] = rec
    tot['files'] += 1; tot['bytes'] += len(t); tot['literal'] += rec['literal_bytes']
    print('%-12s %-55s %8d lit %5d ops %6d' % (ov, rel, len(t), rec['literal_bytes'], len(rec['ops'])), flush=True)
with gzip.open(OUT, 'wt', encoding='utf-8') as f: json.dump(out, f, separators=(',', ':'))
print('TOTAL', tot, 'size', os.path.getsize(OUT))
