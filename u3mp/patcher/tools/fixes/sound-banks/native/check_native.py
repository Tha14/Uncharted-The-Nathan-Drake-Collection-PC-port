"""Compare native/vagenc.dll with the pure-Python encoder on random and edge-case signals (byte-identical or exit 1)."""
import os, random, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import vagenc
assert vagenc.encode is not vagenc.encode_py, 'native/vagenc.dll did not load'
rnd = random.Random(1027)
cases = [[], [0], [32767] * 57, [-32768] * 57, [rnd.randint(-32768, 32767) for _ in range(5000)]]
for n in (1, 27, 28, 29, 280, 4411):
    cases.append([int(12000 * __import__('math').sin(i / (3 + n % 7))) + rnd.randint(-300, 300) for i in range(n)])
bad = 0
for i, c in enumerate(cases):
    for loop in (None, (0, len(c)) if c else None, (56, len(c)) if len(c) > 60 else None):
        if vagenc.encode(c, loop) != vagenc.encode_py(c, loop):
            bad += 1; print('DIFF case', i, 'loop', loop)
print('native == python on %d cases' % len(cases) if not bad else '%d differences' % bad)
sys.exit(1 if bad else 0)
