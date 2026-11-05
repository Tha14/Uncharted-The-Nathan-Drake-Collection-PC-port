#!/usr/bin/env python3
"""Excess-geometry detector. usage: detect.py [main ...] ; writes det/<main>.json. Reads paks only.
Excess sample = on a drawn companion triangle, >R from all BG collision of the map set, >R from main-level draw,
and within 2.4 m above a walkable collision floor that has main-level draw on it."""
from lib import *
import json, glob, hashlib, time
R = 0.35; CELL = 3.0; STEP = 0.2
SKIP = re.compile(r'grass|tree|palm|foliage|canopy|bush|plant|ivy|vine|leaf|decal|cable|wire|rope|water|wave|foam|fog|smoke|cloud|sky|flow|snow|dust|shaft|glow|flare|fire|sun|ray|net|blocker|flag|cloth|cobweb|debris|rubble|chain|glass|shadow', re.I)


def closest(P, A, B, C):
    """P (n,3), A/B/C (1,m,3) -> distance matrix (n,m)"""
    P = P[:, None, :]; ab = B - A; ac = C - A; ap = P - A
    d00 = (ab * ab).sum(-1); d01 = (ab * ac).sum(-1); d11 = (ac * ac).sum(-1); d20 = (ap * ab).sum(-1); d21 = (ap * ac).sum(-1)
    den = d00 * d11 - d01 * d01; den = np.where(np.abs(den) < 1e-12, 1e-12, den)
    v = (d11 * d20 - d01 * d21) / den; w = (d00 * d21 - d01 * d20) / den
    inside = (v >= 0) & (w >= 0) & (v + w <= 1)
    dist = np.linalg.norm(ap - (ab * v[..., None] + ac * w[..., None]), axis=-1)

    def seg(a, b):
        t = np.clip((((P - a) * (b - a)).sum(-1)) / np.maximum(((b - a) ** 2).sum(-1), 1e-12), 0, 1)
        return np.linalg.norm(P - (a + (b - a) * t[..., None]), axis=-1)
    edge = np.minimum(np.minimum(seg(A, B), seg(B, C)), seg(C, A))
    return np.where(inside, np.minimum(dist, edge), edge)


class Grid:
    def __init__(s, tris):
        s.t = tris; s.c = {}
        lo = np.floor((tris.min(1) - R) / CELL).astype(int); hi = np.floor((tris.max(1) + R) / CELL).astype(int)
        for i in range(len(tris)):
            for x in range(lo[i, 0], hi[i, 0] + 1):
                for z in range(lo[i, 2], hi[i, 2] + 1):
                    s.c.setdefault((x, z), []).append(i)

    def near(s, pts):
        out = np.zeros(len(pts), bool)
        q = np.floor(pts / CELL).astype(int)[:, [0, 2]]
        u, inv = np.unique(q, axis=0, return_inverse=True); inv = inv.ravel()
        order = np.argsort(inv, kind='stable'); ends = np.searchsorted(inv[order], np.arange(len(u) + 1))
        for k, c in enumerate(u.tolist()):
            ids = s.c.get(tuple(c))
            if not ids:
                continue
            ix = order[ends[k]:ends[k + 1]]; T = s.t[ids]
            for b in range(0, len(ix), 1500):
                j = ix[b:b + 1500]
                pl = pts[j].min(0) - R; ph = pts[j].max(0) + R
                sel = np.where(((T.max(1) >= pl) & (T.min(1) <= ph)).all(1))[0]
                for b2 in range(0, len(sel), 800):
                    tt = T[sel[b2:b2 + 800]]
                    out[j] |= (closest(pts[j], tt[:, 0][None], tt[:, 1][None], tt[:, 2][None]) < R).any(1)
        return out


def floors(coll_tris, main_pts):
    n = np.cross(coll_tris[:, 1] - coll_tris[:, 0], coll_tris[:, 2] - coll_tris[:, 0]); l = np.maximum(np.linalg.norm(n, axis=1), 1e-9)
    wk = coll_tris[np.abs(n[:, 1]) / l > 0.7]
    P, _ = sample(wk, 0.5, 800)
    M = set(map(tuple, np.floor(main_pts).astype(int).tolist()))
    u, i = np.unique(np.floor(P).astype(int), axis=0, return_inverse=True); i = i.ravel(); ok = np.zeros(len(u), bool)
    for k, c in enumerate(u.tolist()):
        ok[k] = any((c[0] + a, c[1] + b, c[2] + d) in M for a in (-1, 0, 1) for b in (-1, 0, 1) for d in (-1, 0, 1))
    return set(map(tuple, np.floor(P[ok[i]] / .5).astype(int).tolist()))


def playable(pts, F):
    u, i = np.unique(np.floor(pts / .5).astype(int), axis=0, return_inverse=True); i = i.ravel(); ok = np.zeros(len(u), bool)
    for k, (x, y, z) in enumerate(u.tolist()):
        ok[k] = any((x + a, y - h, z + b) in F for a in (-1, 0, 1) for b in (-1, 0, 1) for h in range(-1, 6))
    return ok[i]


def companions(main):
    fs = [os.path.basename(f) for f in glob.glob(glob.escape(PK) + '/*.pak')]
    c = [f for f in fs if f.startswith(main + '-') and 'ingame' not in f and not re.search(r'-shade|-sound|-tc\d|\.pak\.pak|hidden', f)]
    if main == 'shambhala-multi':
        c += [f for f in fs if f.startswith('sham-multi') and 'ingame' not in f]
    return sorted(c)


def run(main):
    t0 = time.time(); comps = companions(main); print(main, comps, flush=True)
    dm, MO = world(PK + main + '.pak')
    CT = [coll(PK + main + '.pak')] + [coll(PK + c) for c in comps]; CT = np.vstack([c for c in CT if len(c)])
    mt = []
    for I, sn, s, Wd, T in MO:
        if 'shadowblocker' in s['mat']:
            continue
        l = ~((T[:, 0] == T[:, 1]) & (T[:, 1] == T[:, 2]))
        if l.any():
            mt.append(Wd[T[l]])
    mt = np.vstack(mt); mp_, _ = sample(mt, 1.0, 200)
    F = floors(CT, mp_); print(' coll', len(CT), 'main', len(mt), 'floor keys', len(F), flush=True)
    GC = Grid(CT); GM = Grid(mt); hits = []
    for pak, ismain in [(c, False) for c in comps] + [(main + '.pak', True)]:
        d, O = (dm, MO) if ismain else world(PK + pak)
        if not O:
            continue
        uses = {}
        for I, sn, s, Wd, T in O:
            uses[sn] = uses.get(sn, 0) + 1
        md5 = hashlib.md5(d).hexdigest(); rows = []
        for I, sn, s, Wd, T in O:
            mat = s['mat']
            if SKIP.search(mat):
                continue
            live = ~((T[:, 0] == T[:, 1]) | (T[:, 1] == T[:, 2]) | (T[:, 0] == T[:, 2]))
            ix = np.where(live)[0]
            if not len(ix):
                continue
            tw = Wd[T[ix]]; n = np.cross(tw[:, 1] - tw[:, 0], tw[:, 2] - tw[:, 0]); a = np.linalg.norm(n, axis=1) / 2
            e = np.max([np.linalg.norm(tw[:, i] - tw[:, (i + 1) % 3], axis=1) for i in range(3)], axis=0)
            keep = e > 4.0 if ismain else a > 0.02       # main pass: only coarse shell-like triangles
            ix = ix[keep]; tw = tw[keep]; a = a[keep]; n = n[keep]
            if not len(ix):
                continue
            P, o = sample(tw, STEP, 120)
            pl = playable(P, F)
            if not pl.any():
                continue
            nc = GC.near(P[pl]); nm = np.zeros(pl.sum(), bool) if ismain else GM.near(P[pl])
            ex = np.zeros(len(P), bool); ho = np.zeros(len(P), bool); idx = np.where(pl)[0]
            ex[idx] = (~nc) & (~nm); ho[idx] = nc & (~nm)
            ne = np.bincount(o, minlength=len(tw)); ee = np.bincount(o, ex, minlength=len(tw)); hh = np.bincount(o, ho, minlength=len(tw))
            for k in np.where(ee > 0)[0]:
                fe = ee[k] / ne[k]
                if fe < .1 or a[k] * fe < .03:
                    continue
                ny = abs(n[k, 1]) / max(np.linalg.norm(n[k]), 1e-9)
                rows.append(dict(pak=pak, inst=I['i'], sub=sn.split('.', 1)[1], mat=mat.split('/')[-1][:44], uses=uses[sn], t=int(ix[k]),
                                 off=int(s['i'] + ix[k] * 6), idx=[int(x) for x in T[ix[k]]], c=tw[k].mean(0).round(2).tolist(),
                                 lo=tw[k].min(0).round(2).tolist(), hi=tw[k].max(0).round(2).tolist(), area=float(round(a[k], 3)),
                                 fex=float(round(fe, 3)), fhole=float(round(hh[k] / ne[k], 3)), ny=float(round(ny, 2))))
        print(' ', pak, 'tris', len(rows), '%.0fs' % (time.time() - t0), flush=True)
        hits.append(dict(pak=pak, md5=md5, bytes=len(d), main=ismain, rows=rows))
    os.makedirs('det', exist_ok=True); json.dump(hits, open('det/%s.json' % main, 'w'))


if __name__ == '__main__':
    ms = sys.argv[1:] or sorted(os.path.basename(f)[:-4] for f in glob.glob(glob.escape(PK) + '/*-multi.pak') if not re.search(r'sky-|grave-water', os.path.basename(f)))
    for m in ms:
        try:
            run(m)
        except Exception as ex:
            import traceback; traceback.print_exc(); print('ERR', m, repr(ex), flush=True)
