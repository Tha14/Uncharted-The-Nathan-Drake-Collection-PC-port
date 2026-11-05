import sys, os, hashlib
sys.path.insert(0, os.path.expanduser('~/mnt/outputs/la'))
from psarcmod import Psarc
src, out = sys.argv[1], sys.argv[2]
want = set(sys.argv[3:])
p = Psarc(src)
for i, n in enumerate(p.names, 1):
    b = os.path.basename(n)
    if b in want:
        d = p.read(i)
        open(os.path.join(out, b), 'wb').write(d)
        print(b, len(d), hashlib.md5(d).hexdigest())
