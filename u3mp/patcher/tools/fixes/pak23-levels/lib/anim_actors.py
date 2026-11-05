"""data/ps4_anim_actors.json for ps4ingame: names of the retail actor33 paks that carry an ANIM item
(scratchpad lab/aa.py of 2026-10-31; the converted-actor half is shipped as data/conv_anim_actors.json, the 167 names
the 2026-10-31 overlays had). usage: anim_actors.py <paktools> <PS4 build/main/actor33/ps4> <out.json>"""
import json
import os
import sys

sys.path.insert(0, sys.argv[1])
from ndpak import Pak  # noqa: E402

R = sys.argv[2]
s = set()
for f in os.listdir(R):
    if f.endswith('.pak'):
        try:
            if Pak(os.path.join(R, f)).items_of('ANIM'):
                s.add(f[:-4])
        except Exception:
            pass
json.dump(sorted(s), open(sys.argv[3], 'w'))
print('retail animated actors', len(s))
