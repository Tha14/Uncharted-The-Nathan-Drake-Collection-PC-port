"""provenance.py: classify every manifest file by hash against retail PS4 (files + psarc entries), PS3 archives/loose files,
backups, pre-session transfer batches and stage-assets. Writes provenance.tsv and prints per-family stats."""
import csv, collections, glob, os
man = list(csv.DictReader(open('manifest.tsv', encoding='utf-8'), delimiter='\t'))
idx = collections.defaultdict(list)   # sha -> [(class, source)]
def add(f, cls, keyfn):
    if not os.path.exists(f): return
    for r in csv.DictReader(open(f, encoding='utf-8'), delimiter='\t'):
        h = r.get('sha256', '')
        if len(h) == 64: idx[h].append((cls, keyfn(r)))
def addpsarc(f, cls):
    if not os.path.exists(f): return
    for line in open(f, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) == 4 and len(p[3]) == 64: idx[p[3]].append((cls, os.path.basename(p[0]) + '::' + p[1]))
add('retail-ps4-hashes.tsv', 'ps4-retail', lambda r: r['path'])
add('retail-ps4-u2data-sound.tsv', 'ps4-retail', lambda r: r['path'])
addpsarc('retail-ps4-psarc-entries.tsv', 'ps4-retail-psarc')
addpsarc('ps3-hashes/u3-pak23.tsv', 'ps3-u3-disc-psarc')
addpsarc('ps3-hashes/u3-actor-bin.tsv', 'ps3-u3-disc-psarc')
addpsarc('ps3-hashes/u3-goty.tsv', 'ps3-u3-goty-psarc')
addpsarc('ps3-hashes/u3-streams.tsv', 'ps3-u3-streams-psarc')
addpsarc('ps3-hashes/u3-rpcs3.tsv', 'ps3-u3-patch-dlc-psarc')
addpsarc('ps3-hashes/u2.tsv', 'ps3-u2-psarc')
add('ps3-hashes/u3-disc-loose.tsv', 'ps3-u3-disc-loose', lambda r: r['path'])
add('ps3-hashes/rpcs3-loose.tsv', 'ps3-rpcs3-loose', lambda r: r['path'])
add('backup-hashes-work.tsv', 'backup', lambda r: 'work/' + r['path'])
add('backup-hashes-d.tsv', 'backup', lambda r: 'D-backups/' + r['path'])
add('transfer-hashes.tsv', 'transfer-batch', lambda r: r['path'])
add('stage-assets-hashes.tsv', 'stage-assets', lambda r: r['path'])
rpath = {r['path']: r['sha256'] for r in csv.DictReader(open('retail-ps4-hashes.tsv', encoding='utf-8'), delimiter='\t')}
PRIO = ['ps4-retail', 'ps4-retail-psarc', 'ps3-u3-disc-loose', 'ps3-u3-disc-psarc', 'ps3-u3-goty-psarc', 'ps3-u3-streams-psarc',
        'ps3-u3-patch-dlc-psarc', 'ps3-rpcs3-loose', 'ps3-u2-psarc', 'transfer-batch', 'stage-assets', 'backup']
stats = collections.Counter(); rows = []
for m in man:
    fam = m['overlay'] + ':' + '/'.join(m['path'].split('/')[2:5])
    hits = idx.get(m['sha256'], [])
    best = None
    for c in PRIO:
        s = [x for x in hits if x[0] == c]
        if s: best = (c, s[0][1]); break
    if best is None:
        cls = 'derived-or-converted' + ('(retail path exists)' if m['path'] in rpath else '')
    else:
        cls = best[0]
    stats[(fam, cls)] += 1
    rows.append([m['overlay'], m['path'], m['size'], m['sha256'], cls, best[1] if best else '', ';'.join(f'{c}:{s}' for c, s in hits[:4])])
with open('provenance.tsv', 'w', encoding='utf-8') as f:
    f.write('overlay\tpath\tsize\tsha256\tclass\tbest_source\tall_matches\n')
    for r in rows: f.write('\t'.join(map(str, r)) + '\n')
for k, v in sorted(stats.items()): print(f'{v:6d}  {k[0]:45s} {k[1]}')
tot = collections.Counter(r[4] for r in rows); print(); [print(f'{v:6d} {k}') for k, v in tot.most_common()]
