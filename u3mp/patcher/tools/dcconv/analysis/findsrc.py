"""which other files hold the literal residue of a recipe? usage: findsrc.py recipes.json.gz key [dirs...]"""
import gzip, json, os, sys, collections
R = json.load(gzip.open(sys.argv[1], 'rt'))
rec = R[sys.argv[2]]
chunks = [bytes.fromhex(o[1]) for o in rec['ops'] if o[0] == 'l']
chunks = [c for c in chunks if len(c) >= 6 and c.count(0) < len(c) // 2]
print('chunks', len(chunks), sum(map(len, chunks)))
hits = collections.Counter(); cov = collections.Counter()
for d in sys.argv[3:]:
    for dp, dn, fn in os.walk(d):
        for f in fn:
            p = os.path.join(dp, f)
            try: b = open(p, 'rb').read()
            except OSError: continue
            sw = bytearray(len(b))
            for i in range(0, len(b) - 3, 4): sw[i:i + 4] = b[i:i + 4][::-1]
            sw = bytes(sw)
            n = sum(len(c) for c in chunks if c in b or c in sw)
            if n: cov[p] = n
for p, n in cov.most_common(15): print(n, p)
