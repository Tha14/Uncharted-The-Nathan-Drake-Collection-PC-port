"""Extract the Lab level sources (as on 2026-10-31, scratchpad xl.py): usage xl.py <ps3 update USRDIR> <ps3 DATA2 USRDIR> <out labsrc>"""
import sys, os, re
U, D2, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
here = os.path.dirname(os.path.abspath(__file__))
from psarc_lib import Psarc
D = os.path.join(D2, 'build', 'main') + '/'
for k in ('pak', 'dds', 'txt'):
    os.makedirs(os.path.join(OUT, k), exist_ok=True)
ar = [os.path.join(U, 'dlc-patch13.psarc'), D + 'lab-multi-01-7.psarc', D + 'actor-parts-7.psarc', D + 'lab-trench-v1-7.psarc']
tot = 0
for a in ar:
    p = Psarc(a)
    for i, n in enumerate(p.names):
        x = os.path.basename(n)
        if re.search(r'^(lab|lt-box|mixer-map|chat-obj|cha-sound|cruise-ship|col-anim|ldn-pub|anim-|block-)', x) and x.endswith('.pak') or re.search(r'^lab', x) and x.endswith(('.dds', '.txt')):
            k = 'pak' if x.endswith('.pak') else 'dds' if x.endswith('.dds') else 'txt'
            open(os.path.join(OUT, k, x), 'wb').write(p.read(i + 1)); tot += 1
print('extracted', tot)
