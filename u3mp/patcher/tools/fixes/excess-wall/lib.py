import sys, struct, re, os
import numpy as np
sys.path.insert(0, '{{PAKTOOLS}}')
from ndpak import Pak
import collision as CL
W = '{{PROJECT}}/work/'
PK = W + 'overlay-r3/u3data/build/main/pak23/ps4/'


def load(f):
    d = open(f, 'rb').read(); P = Pak(d)
    g = [i for i in P.items if i.type == 'GEOMETRY_1']
    if not g:
        return None
    it = g[0]; b = P.pbase(it.page) + it.data_off
    ns = struct.unpack_from('<I', d, b + 8)[0]; tp, to = P.resolve(it.page, it.data_off + 0x10); pb = P.pbase(tp); SM = {}
    for i in range(ns):
        o = to + i * 0x88; nm = P.pstr(*P.resolve(tp, o + 8)) or ''; w = struct.unpack_from('<34I', d, pb + o)
        mat = (P.pstr(*P.resolve(tp, o + 0x48)) if P.is_ptr(tp, o + 0x48) else '') or ''
        st = P.resolve(tp, o + 0x30); sq = P.pbase(st[0]) + st[1]; stride = struct.unpack_from('<H', d, sq + 2)[0]
        da = P.resolve(st[0], st[1] + 16); vb = P.pbase(da[0]) + da[1]; ia = P.resolve(tp, o + 0x40); ib = P.pbase(ia[0]) + ia[1]
        SM[nm] = dict(name=nm, mat=mat.split(',')[0], nv=w[8], ni=w[9], v=vb, s=stride, i=ib)
    npr = struct.unpack_from('<I', d, b + 0x18)[0]; pp, po = P.resolve(it.page, it.data_off + 0x20); PR = []
    for i in range(npr):
        t = P.resolve(pp, po + i * 8); q = P.pbase(t[0]) + t[1]; c = struct.unpack_from('<I', d, q + 4)[0]; L = []
        if c:
            l = P.resolve(t[0], t[1] + 0x18)
            for j in range(c):
                L.append(P.pstr(*P.resolve(l[0], l[1] + j * 0x10)) or '')
        PR.append(dict(subs=L))
    ni = struct.unpack_from('<I', d, b + 0x28)[0]; pp, po = P.resolve(it.page, it.data_off + 0x30); IN = []
    for i in range(ni):
        t = P.resolve(pp, po + i * 8); q = P.pbase(t[0]) + t[1]; nm = P.pstr(*P.resolve(t[0], t[1] + 8)) or ''; mm = re.match(r'Prototype(\d+)', nm)
        if not mm:
            continue
        IN.append(dict(i=i, p=int(mm.group(1)), m=struct.unpack_from('<16f', d, q + 0x80)))
    return d, P, SM, PR, IN


def world(path):
    r = load(path)
    if not r:
        return b'', []
    d, P, SM, PR, IN = r; out = []; cache = {}
    for I in IN:
        m = np.array(I['m']).reshape(4, 4)
        for sn in PR[I['p']]['subs']:
            s = SM.get(sn)
            if not s or not s['nv']:
                continue
            if sn not in cache:
                V = np.ndarray((s['nv'],), dtype=np.dtype({'names': ['x', 'y', 'z'], 'formats': ['<f4'] * 3, 'offsets': [0, 4, 8], 'itemsize': s['s']}), buffer=d, offset=s['v'])
                V = np.c_[V['x'], V['y'], V['z']].astype(float)
                T = np.frombuffer(d, dtype='<u2', count=s['ni'] - s['ni'] % 3, offset=s['i']).astype(np.int64).reshape(-1, 3)
                cache[sn] = (V, T)
            V, T = cache[sn]; Wd = (np.c_[V, np.ones(len(V))] @ m)[:, :3]
            out.append((I, sn, s, Wd, T))
    return d, out


def coll(path):
    c = os.path.dirname(os.path.abspath(__file__)) + '/cache'; os.makedirs(c, exist_ok=True)
    st = os.stat(path); k = c + '/%s_%d_%d.npy' % (os.path.basename(path), st.st_size, int(st.st_mtime))
    if os.path.exists(k):
        return np.load(k)
    p = Pak(path); out = []
    for it in p.items_of('COLLISION_DATA_HAVOK_BG'):
        tr = p.resolve(it.page, it.data_off + 0x70)
        if tr is None:
            continue
        pfr = p.resolve(tr[0], tr[1] + 8); size = p.u32(p.pbase(tr[0]) + tr[1] + 0x10)
        r = CL.decode_bvcm(p.d[p.pbase(pfr[0]) + pfr[1]:p.pbase(pfr[0]) + pfr[1] + size])
        for s in r['tris']:
            if s:
                out.append(np.array(s))
    a = np.vstack(out) if out else np.zeros((0, 3, 3))
    np.save(k, a); return a


def sample(tris, step=0.25, maxn=40):
    P = []; O = []
    e1 = tris[:, 1] - tris[:, 0]; e2 = tris[:, 2] - tris[:, 0]
    L = np.maximum(np.linalg.norm(e1, axis=1), np.maximum(np.linalg.norm(e2, axis=1), np.linalg.norm(tris[:, 2] - tris[:, 1], axis=1)))
    N = np.clip(np.ceil(L / step).astype(int), 1, maxn)
    for n in np.unique(N):
        idx = np.where(N == n)[0]
        u, v = np.meshgrid(np.arange(n + 1), np.arange(n + 1)); m = u + v <= n; u = u[m] / n; v = v[m] / n
        for b in range(0, len(idx), 2000):
            ii = idx[b:b + 2000]
            pts = tris[ii, 0][:, None, :] + e1[ii][:, None, :] * u[None, :, None] + e2[ii][:, None, :] * v[None, :, None]
            P.append(pts.reshape(-1, 3)); O.append(np.repeat(ii, len(u)))
    return np.vstack(P), np.concatenate(O)
