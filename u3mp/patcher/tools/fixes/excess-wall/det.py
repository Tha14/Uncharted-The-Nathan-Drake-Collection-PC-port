#!/usr/bin/env python3
"""Excess-geometry detector (read-only). usage: det.py [--strip-from PAK] [main ...]; writes det/<main>.json.
Voxel version: a sample is "far" when none of the 27 cells (h=0.25 m) around it holds collision (guarantees > 0.25 m).
Excess sample = on a drawn companion triangle, far from all BG collision of the map set, far from main-level draw,
and 0..2.5 m above a walkable collision floor that has main-level draw within about 1 m."""
from lib import *
import json, glob, hashlib, time
H = 0.25; STEP = 0.2
SKIP = re.compile(r'grass|tree|palm|foliage|canopy|bush|plant|ivy|vine|leaf|decal|cable|wire|rope|water|wave|foam|fog|smoke|cloud|sky|flow|snow|dust|shaft|glow|flare|fire|sun|ray|net|blocker|flag|cloth|cobweb|debris|rubble|chain|glass|shadow', re.I)
OFF = 1 << 20


def key(a):
    return (a[..., 0] + OFF) * (1 << 42) + (a[..., 1] + OFF) * (1 << 21) + (a[..., 2] + OFF)


def occupancy(tris, step, h, maxn=4000):
    """unique voxel keys touched by triangles; big triangles are batched so memory stays bounded"""
    e1 = tris[:, 1] - tris[:, 0]; e2 = tris[:, 2] - tris[:, 0]
    L = np.max([np.linalg.norm(e1, axis=1), np.linalg.norm(e2, axis=1), np.linalg.norm(tris[:, 2] - tris[:, 1], axis=1)], axis=0)
    N = np.clip(np.ceil(L / step).astype(int), 1, maxn); out = []
    for n in np.unique(N):
        idx = np.where(N == n)[0]
        u, v = np.meshgrid(np.arange(n + 1, dtype=np.float32), np.arange(n + 1, dtype=np.float32)); m = u + v <= n; u = u[m] / n; v = v[m] / n
        bs = max(1, int(3e6 / len(u)))
        for b in range(0, len(idx), bs):
            ii = idx[b:b + bs]
            p = tris[ii, 0][:, None, :] + e1[ii][:, None, :] * u[None, :, None] + e2[ii][:, None, :] * v[None, :, None]
            out.append(np.unique(key(np.floor(p.reshape(-1, 3) / h).astype(np.int64))))
        if sum(len(o) for o in out) > 2e7:
            out = [np.unique(np.concatenate(out))]
    return np.unique(np.concatenate(out))


def has(U, k):
    pos = np.minimum(np.searchsorted(U, k), len(U) - 1)
    return U[pos] == k


def near27(U, q):
    """q integer cells (n,3): any of the 27 neighbours in U"""
    hit = np.zeros(len(q), bool)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            for dz in (-1, 0, 1):
                hit |= has(U, key(q + np.array([dx, dy, dz])))
    return hit


def unkey(k):
    return np.stack([k // (1 << 42) - OFF, (k // (1 << 21)) % (1 << 21) - OFF, k % (1 << 21) - OFF], -1)


def floors(CT, main_occ_1m):
    """dilated column keys (0.5 m): a sample cell is playable when its key is in the result"""
    n = np.cross(CT[:, 1] - CT[:, 0], CT[:, 2] - CT[:, 0]); l = np.maximum(np.linalg.norm(n, axis=1), 1e-9)
    wk = CT[np.abs(n[:, 1]) / l > 0.7]
    F = occupancy(wk, 0.45, 0.5)
    c = unkey(F); ok = near27(main_occ_1m, np.floor((c + .5) * .5).astype(np.int64))
    c = c[ok]; out = []
    for a in (-1, 0, 1):
        for b in (-1, 0, 1):
            for h_ in range(-1, 6):
                out.append(key(c + np.array([a, h_, b])))
    return np.unique(np.concatenate(out))


def companions(main):
    fs = [os.path.basename(f) for f in glob.glob(glob.escape(PK) + '/*.pak')]
    c = [f for f in fs if f.startswith(main + '-') and 'ingame' not in f and not re.search(r'-shade|-sound|-tc\d|\.pak\.pak|hidden', f)]
    if main == 'shambhala-multi':
        c += [f for f in fs if f.startswith('sham-multi') and 'ingame' not in f]
    return sorted(c)


def run(main, over={}, tag=''):
    t0 = time.time(); comps = companions(main); print(main, comps, flush=True)
    path = lambda f: over.get(f, PK + f)
    dm, MO = world(path(main + '.pak'))
    if not MO:
        print(' no geometry'); return
    CT = [c for c in [coll(path(main + '.pak'))] + [coll(path(c)) for c in comps] if len(c)]
    if not CT:
        print(' no collision'); return
    CT = np.vstack(CT); mt = []
    for I, sn, s, Wd, T in MO:
        if 'shadowblocker' in s['mat']:
            continue
        l = ~((T[:, 0] == T[:, 1]) & (T[:, 1] == T[:, 2]))
        if l.any():
            mt.append(Wd[T[l]])
    mt = np.vstack(mt)
    UC = occupancy(CT, STEP, H); UM = occupancy(mt, STEP, H); UM1 = occupancy(mt, 0.9, 1.0)
    F = floors(CT, UM1); print(' coll', len(CT), 'main', len(mt), 'floor', len(F), '%.0fs' % (time.time() - t0), flush=True)
    hits = []
    for pak, ismain in [(c, False) for c in comps]:
        d, O = (dm, MO) if ismain else world(path(pak))
        if not O:
            continue
        uses = {}
        for I, sn, s, Wd, T in O:
            uses[sn] = uses.get(sn, 0) + 1
        rows = []
        for I, sn, s, Wd, T in O:
            mat = s['mat']
            if SKIP.search(mat):
                continue
            ix = np.where(~((T[:, 0] == T[:, 1]) | (T[:, 1] == T[:, 2]) | (T[:, 0] == T[:, 2])))[0]
            if not len(ix):
                continue
            tw = Wd[T[ix]]; n = np.cross(tw[:, 1] - tw[:, 0], tw[:, 2] - tw[:, 0]); a = np.linalg.norm(n, axis=1) / 2
            e = np.max([np.linalg.norm(tw[:, i] - tw[:, (i + 1) % 3], axis=1) for i in range(3)], axis=0)
            keep = (a > 0.02) & (np.abs(n[:, 1]) / np.maximum(2 * a, 1e-9) < 0.3)   # walls only
            ix = ix[keep]; tw = tw[keep]; a = a[keep]; n = n[keep]
            if not len(ix):
                continue
            P, o = sample(tw, STEP, 120)
            pl = has(F, key(np.floor(P / .5).astype(np.int64)))
            if not pl.any():
                continue
            q = np.floor(P[pl] / H).astype(np.int64)
            nc = near27(UC, q); nm = np.zeros(len(q), bool) if ismain else near27(UM, q)
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
        hits.append(dict(pak=pak, md5=hashlib.md5(d).hexdigest(), bytes=len(d), main=ismain, rows=rows))
    os.makedirs('det', exist_ok=True); json.dump(hits, open('det/%s%s.json' % (main, tag), 'w'))


if __name__ == '__main__':
    a = sys.argv[1:]; over = {}; tag = ''
    if a and a[0] == '--strip-from':                # positive control: use the pre-fix strip pak
        over = {'air-multi-strip.pak': a[1]}; tag = '.control'; a = a[2:]
    ms = a or sorted(os.path.basename(f)[:-4] for f in glob.glob(glob.escape(PK) + '/*-multi.pak') if not re.search(r'sky-|grave-water', os.path.basename(f)))
    for m in ms:
        try:
            run(m, over, tag)
        except Exception as ex:
            import traceback; traceback.print_exc(); print('ERR', m, repr(ex), flush=True)
