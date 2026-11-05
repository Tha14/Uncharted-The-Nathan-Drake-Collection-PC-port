"""air-wall.json: strip triangles on the plane x=31.64, z 11.6..13.3, y 0..5 (main wall ends at z=12.44 on this plane)."""
from lib import *
import json, hashlib
d, O = world(PK + 'air-multi-strip.pak')
out = {}
for I, sn, s, Wd, T in O:
    tw = Wd[T]; live = ~((T[:, 0] == T[:, 1]) & (T[:, 1] == T[:, 2]))
    m = live & (np.abs(tw[:, :, 0] - 31.64) < .03).all(1) & (tw[:, :, 2] > 11.6).all(1) & (tw[:, :, 2] < 13.3).all(1) & (tw[:, :, 2].max(1) > 12.5) & (tw[:, :, 1] > -.1).all(1) & (tw[:, :, 1] < 5.1).all(1)
    for t in np.where(m)[0]:
        out[int(s['i'] + t * 6)] = [int(s['i'] + t * 6), *[int(x) for x in T[t]]]
        print(sn.split('.', 1)[1], s['mat'].split('/')[-1][:30], t, tw[t].round(2).tolist())
json.dump(dict(pak='air-multi-strip.pak', md5=hashlib.md5(d).hexdigest(), bytes=len(d), tris=sorted(out.values())), open('air-wall.json', 'w'))
print(len(out), 'triangles')
