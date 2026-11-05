"""Exact strip triangles of the Airstrip excess wall, and what the main level draws on the same plane."""
from lib import *
import json, hashlib
for n in ('air-multi-strip', 'air-multi', 'air-multi-wide', 'air-multi-stage'):
    d, O = world(PK + n + '.pak')
    print('==', n, hashlib.md5(d).hexdigest(), len(d)); uses = {}
    for I, sn, s, Wd, T in O:
        uses[sn] = uses.get(sn, 0) + 1
    rows = []
    for I, sn, s, Wd, T in O:
        tw = Wd[T]; live = ~((T[:, 0] == T[:, 1]) & (T[:, 1] == T[:, 2]))
        # anything drawn in the 0.86 m beyond the main wall end, any orientation
        lo = tw.min(1); hi = tw.max(1)
        m = live & (hi[:, 2] > 12.6) & (lo[:, 2] < 13.4) & (lo[:, 0] < 32.2) & (hi[:, 0] > 31.0) & (lo[:, 1] < 5.2) & (hi[:, 1] > -.1) & ((hi - lo).max(1) < 20)
        for t in np.where(m)[0]:
            rows.append((sn.split('.', 1)[1], s['mat'].split('/')[-1][:30], uses[sn], int(t), hex(s['i'] + int(t) * 6), [int(x) for x in T[t]], tw[t].round(2).tolist()))
    for r in rows:
        print(r)
    if n == 'air-multi':
        # main wall extent on the same plane
        zs = []
        for I, sn, s, Wd, T in O:
            if 'shadowblocker' in s['mat']:
                continue
            tw = Wd[T]; m = (np.abs(tw[:, :, 0] - 31.64) < .05).all(1) & (tw[:, :, 2].max(1) < 16) & (tw[:, :, 2].min(1) > 4)
            if m.any():
                zs.append((sn.split('.', 1)[1], s['mat'].split('/')[-1][:30], float(tw[m][:, :, 2].min()), float(tw[m][:, :, 2].max())))
        print('main plane x=31.64:', zs)
