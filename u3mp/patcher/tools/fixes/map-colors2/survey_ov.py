#!/usr/bin/env python3
"""Recipe helper (2026-11-06): ov.jsonl.gz = the material survey of the current overlay-r3 BG paks (scratch survey.py of the
11-01 session, overlay part only); detect.py then derives rows.jsonl from it and the PS3 paks.  Run in this folder."""
import gzip, os, time, json
from lib import *
with gzip.open('ov.jsonl.gz', 'wt') as f:
    for fn in sorted(os.listdir(OV)):
        if not fn.endswith('.pak') or fn.endswith('-ingame12.pak'):
            continue
        try:
            q = Pak(open(OV + fn, 'rb').read()); m = mats4(q)
            f.write(json.dumps(dict(pak=fn, size=len(q.d), nfx=len(q.items_of('EFFECT')), mats=m)) + '\n')
        except Exception as e:
            f.write(json.dumps(dict(pak=fn, err=repr(e))) + '\n')
print('ov.jsonl.gz written')
