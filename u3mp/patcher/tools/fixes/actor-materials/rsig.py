#!/usr/bin/env python3
"""Mesh signatures of PS4 actor paks: per FG section (LODs excluded) the unique rounded visible vertex positions, plus
skeleton, hierarchy, joint table hash, material names. rsig.py builds sig_retail.pkl and sig_targets.pkl."""
import sys, os, re, struct, pickle, hashlib, multiprocessing as mp
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import numpy as np
T = '{{PAKTOOLS}}'; sys.path.insert(0, T)
from ndpak import Pak
PR = '{{PROJECT}}'
RET = '{{PS4}}/u3data/build/main/actor33/ps4'
GRID = 0.004


def hashes(pos):
    k = np.round(pos / GRID).astype(np.int64)
    return np.unique(k[:, 0] * 2000003 * 2000003 + k[:, 1] * 2000003 + k[:, 2])


def sig(f):
    try:
        q = Pak(f)
        gs = q.items_of('GEOMETRY_1'); fgs = q.items_of('FOREGROUND_SECTION_2')
        if not gs or not fgs:
            return None
        g = gs[0]; pg, b = g.page, g.data_off
        ns = q.pu32(pg, b + 8); r = q.resolve(pg, b + 0x10)
        sub = {}
        for i in range(ns):
            o = r[1] + 0x88 * i
            nm = q.pstr(*q.resolve(r[0], o + 8)); x = q.resolve(r[0], o + 0x48); mat = q.pstr(*x).split(',')[0] if x else ''
            nv = q.pu32(r[0], o + 0x20); nst = q.pu32(r[0], o + 0x28); st = q.resolve(r[0], o + 0x30); pos = None
            for k in range(nst if st else 0):
                so = st[1] + 0x18 * k; w = q.pu32(st[0], so); stride = w >> 16
                fm = q.resolve(st[0], so + 8); da = q.resolve(st[0], so + 0x10)
                if fm and da and q.d[q.pbase(fm[0]) + fm[1]] == 0 and stride >= 12:
                    base = q.pbase(da[0]) + da[1]
                    a = np.frombuffer(q.d, dtype='<f4', count=nv * stride // 4, offset=base).reshape(nv, stride // 4)[:, :3]
                    pos = a; break
            sub[nm] = (mat, nv, pos)
        pl = q.resolve(pg, b + 0x20); protos = {}
        for i in range(q.pu32(pg, b + 0x18)):
            x = q.resolve(pl[0], pl[1] + 8 * i); n = q.pu32(x[0], x[1] + 4); sn = q.resolve(x[0], x[1] + 0x18)
            protos[q.pstr(*q.resolve(x[0], x[1] + 0x10))] = [q.pstr(*q.resolve(sn[0], sn[1] + 0x10 * k)) for k in range(n)]
        units = {}
        for it in fgs:
            if re.search(r'-lod\d+$', it.name):
                continue
            pn = q.pstr(*q.resolve(it.page, it.data_off))
            ps = [sub[s][2] for s in protos.get(pn, []) if s in sub and 'shadowblocker' not in sub[s][0] and sub[s][2] is not None]
            if ps:
                units[it.name] = (hashes(np.concatenate(ps)), sorted({sub[s][0] for s in protos.get(pn, []) if s in sub and 'shadowblocker' not in sub[s][0]}))
        it = fgs[0]; skel, hier = struct.unpack_from('<II', q.d, q.pbase(it.page) + it.data_off + 8)
        jr = q.resolve(it.page, it.data_off + 0x10); jh = ''
        if jr:
            ji = q.item_at(jr[0], jr[1] - 0x20); jh = hashlib.sha1(q.d[q.pbase(jr[0]) + jr[1]:q.pbase(jr[0]) + jr[1] + ji.size]).hexdigest()[:8]
        return dict(file=f, name=os.path.basename(f)[:-4], skel=skel, hier=hier, jh=jh, units=units, nfg=len(fgs),
                    arts=[i.name for i in q.items_of('ARTGROUP_DATA')], vram=q.u32(0x20),
                    own_skel=bool(q.items_of('JOINT_HIERARCHY')), anim=len(q.items_of('ANIM')))
    except Exception as e:
        return dict(file=f, name=os.path.basename(f)[:-4], err=repr(e)[:120])


def collect(dirs):
    seen, out = set(), []
    for d in dirs:
        for x in sorted(os.listdir(d)):
            if x.endswith('.pak') and x not in seen:
                seen.add(x); out.append(d + '/' + x)
    return out


if __name__ == '__main__':
    A = PR + '/work/%s/u3data/build/main/actor33/ps4'
    with mp.Pool(10) as P:
        r = [x for x in P.map(sig, collect([RET]), 16) if x]
        pickle.dump(r, open(HERE + '/sig_retail.pkl', 'wb')); print('retail', len(r), 'errs', [x['name'] for x in r if 'err' in x][:10], [x['err'] for x in r if 'err' in x][:2])
        t = [x for x in P.map(sig, collect([A % 'overlay-r3', A % 'overlay-port']), 16) if x]
        pickle.dump(t, open(HERE + '/sig_targets.pkl', 'wb')); print('targets', len(t), 'errs', [x['name'] for x in t if 'err' in x][:10], [x['err'] for x in t if 'err' in x][:2])
