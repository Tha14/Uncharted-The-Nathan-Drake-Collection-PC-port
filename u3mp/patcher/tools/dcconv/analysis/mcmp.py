import sys, os, hashlib, collections
man, out = sys.argv[1], sys.argv[2]
C = collections.Counter(); bad = []
for l in list(open(man, encoding='utf-8'))[1:]:
    ov, path, size, want, _ = l.rstrip('\n').split('\t')
    if ov not in ('overlay-r3', 'overlay-r2', 'overlay-port') or ('/dc1/' not in path and '/dc2/' not in path): continue
    p = out + '/work/' + ov + '/' + path
    if not os.path.exists(p): C['missing'] += 1; bad.append('missing ' + ov + '/' + path); continue
    ok = hashlib.sha256(open(p, 'rb').read()).hexdigest() == want
    C['identical' if ok else 'different'] += 1
    if not ok: bad.append('different ' + ov + '/' + path)
print(dict(C)); print('\n'.join(bad[:20]))
