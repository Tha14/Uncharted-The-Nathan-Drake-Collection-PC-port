"""Extract one psarc entry. usage: extract_one.py <archive.psarc> <entry index (1-based, ps3index.tsv)> <out>"""
import os
import sys
from psarc_lib import Psarc

a, i, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
os.makedirs(os.path.dirname(out), exist_ok=True)
open(out + '.tmp', 'wb').write(Psarc(a).read(i))
os.replace(out + '.tmp', out)
