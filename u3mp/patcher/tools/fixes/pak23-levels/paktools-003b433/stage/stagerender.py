#!/usr/bin/env python3
"""Quick visual check of a PS4 background stage pak: flat-shaded orthographic renders (top and
front) of every instance's submeshes (instance matrix applied, skinned prototypes posed with their
joint matrices).  usage: stagerender.py <pak> <out.png>"""
import os, sys, struct
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection
from ndpak import Pak
from ps4bg import geometry


def world_tris(q):
    G = geometry(q); subs = {s['name']: s for s in G['subs']}
    g = q.items_of('GEOMETRY_1')[0]; pg, b = g.page, g.data_off
    il = q.resolve(pg, b + 0x30)
    out = []
    for i in range(q.u64(q.pbase(pg) + b + 0x28)):
        r = q.resolve(il[0], il[1] + 8 * i); base = q.pbase(r[0]) + r[1]
        M = np.frombuffer(q.d[base + 0x80:base + 0xc0], '<f4').reshape(4, 4).astype(float)
        nsub, nj = struct.unpack_from('<II', q.d, base + 0x20)
        J = None
        if nj:
            jr = q.resolve(r[0], r[1] + 0x38); jb = q.pbase(jr[0]) + jr[1]
            J = np.frombuffer(q.d[jb:jb + 0x30 * nj], '<f4').reshape(nj, 3, 4).astype(float)
        sr = q.resolve(r[0], r[1] + 0x30)
        for k in range(nsub):
            nm = q.pstr(*q.resolve(sr[0], sr[1] + 0x50 * k))
            s = subs[nm]
            if 'shadowblocker' in s['mat'] or 'collision' in s['mat']:
                continue
            pos = s['streams'][0]['data'][:, :3]
            if J is not None:
                sp, so = s['ref']; sk = q.resolve(sp, so + 0x60)
                if sk:
                    tot = q.u32(q.pbase(sk[0]) + sk[1] + 4)
                    rd = lambda o, dt, n: (lambda rr: np.frombuffer(q.d[q.pbase(rr[0]) + rr[1]:q.pbase(rr[0]) + rr[1] + n * np.dtype(dt).itemsize], dt))(q.resolve(sk[0], sk[1] + o))
                    cnt = rd(8, '<u2', s['nv']); jn = rd(0x18, '<u2', tot); wt = rd(0x20, '<f4', tot)
                    vi = np.repeat(np.arange(len(pos)), cnt)
                    c = np.einsum('tij,tj->ti', J[jn], np.c_[pos, np.ones(len(pos))][vi]) * wt[:, None]
                    pos = np.zeros((len(pos), 3)); np.add.at(pos, vi, c)
            wp = (np.c_[pos, np.ones(len(pos))] @ M)[:, :3]
            out.append(wp[s['idx'].reshape(-1, 3)])
    return np.concatenate(out) if out else np.zeros((0, 3, 3))


def render(path, png):
    T = world_tris(Pak(path))
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    light = np.abs(n @ np.array([0.4, 0.8, 0.45]))
    fig, ax = plt.subplots(1, 2, figsize=(16, 8))
    for a, (i, j, title) in zip(ax, ((0, 2, 'top (x,z)'), (0, 1, 'front (x,y)'))):
        depth = T[:, :, 3 - i - j].mean(1)
        order = np.argsort(depth if title.startswith('top') else -depth)
        if title.startswith('top'):
            order = np.argsort(T[:, :, 1].mean(1))
        pc = PolyCollection(T[order][:, :, [i, j]], facecolors=plt.cm.gray(0.15 + 0.8 * light[order]), edgecolors='none')
        a.add_collection(pc); a.autoscale(); a.set_aspect('equal'); a.set_title('%s %s' % (os.path.basename(path), title))
    fig.tight_layout(); fig.savefig(png, dpi=70); plt.close(fig)
    return len(T)


if __name__ == '__main__':
    print(render(sys.argv[1], sys.argv[2]), 'triangles')
