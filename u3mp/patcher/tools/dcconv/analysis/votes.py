import sys, collections
sys.path.insert(0, sys.argv[1])
import dcconv as C
L = C.Learner(); key = sys.argv[3]; rels = [int(x, 16) for x in sys.argv[4].split(',')]
pairs = list(C.read_pairs(sys.argv[2]))[:int(sys.argv[5]) if len(sys.argv) > 5 else 200]
for p3, p4 in pairs:
    L.add_pair(open(p3, 'rb').read(), open(p4, 'rb').read())
k = L.K.get(key)
print('n', k['n'], 'bad', k['bad'], k['kinds'], 'span', sorted(k['span'].items())[:6])
for r in rels:
    v = k['units'].get(r)
    print(hex(r), 'feas', dict(v[0]), 'uniq', dict(v[1]), 'kinds', dict(v[2]))
sh = k['shapes'].get(sys.argv[6] if len(sys.argv) > 6 else '20:10,14')
if sh:
    for r in rels:
        v = sh.get(r)
        if v: print('shape', hex(r), 'feas', dict(v[0]), 'uniq', dict(v[1]))
