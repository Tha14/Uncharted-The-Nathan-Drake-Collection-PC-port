import os, sys, hashlib, collections
P = sys.argv[1]; O = sys.argv[2]; L = sys.argv[3]
sha = lambda p: hashlib.sha256(open(p, 'rb').read()).hexdigest()
C = collections.Counter(); bad = []
for l in open(L, encoding='utf-8'):
    if l.startswith('#'): continue
    ov, rel, layer, want, st = l.rstrip('\n').split('\t')
    o = O + '/work/' + ov + '/u3data/build/main/' + rel
    ref = P + '/work/' + ov + '/u3data/build/main/' + rel
    if not os.path.exists(o): C['absent-' + st] += 1; continue
    s = sha(o)
    C['bin=current-overlay' if s == sha(ref) else 'bin=prefix-ref' if s == want else 'bin-differs'] += 1
    if s != sha(ref) and s != want: bad.append(rel)
    d = o[:-4] + '.dci'
    if os.path.exists(d):
        C['dci-identical' if open(d, 'rb').read() == open(ref[:-4] + '.dci', 'rb').read() else 'dci-differs'] += 1
print(dict(C)); print(bad[:10])
