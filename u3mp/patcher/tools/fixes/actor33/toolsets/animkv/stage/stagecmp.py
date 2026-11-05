#!/usr/bin/env python3
"""Compare two PS4 background stage paks (converted vs retail rosetta): submeshes (counts,
indices, stream layout and values), prototypes, instances (matrix, per-instance streams),
material names per submesh, login order and item type counts.
usage: stagecmp.py <converted.pak> <retail.pak>"""
import sys, os, struct, collections
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
from ndpak import Pak
from ps4bg import geometry, streams


def protos_insts(q):
    g = q.items_of('GEOMETRY_1')[0]; pg, b = g.page, g.data_off
    cnt = lambda o: q.u64(q.pbase(pg) + b + o)
    P = {}
    pl = q.resolve(pg, b + 0x20)
    for i in range(cnt(0x18)):
        r = q.resolve(pl[0], pl[1] + 8 * i)
        nm = q.pstr(*q.resolve(r[0], r[1] + 0x10))
        nsub = q.pu32(r[0], r[1] + 4)
        fl = q.resolve(r[0], r[1] + 0x20)
        flags = q.d[q.pbase(fl[0]) + fl[1]:q.pbase(fl[0]) + fl[1] + 16] if fl else b''
        P[nm] = dict(nsub=nsub, flags=flags.hex(), cb=struct.unpack_from('<II', q.d, q.pbase(r[0]) + r[1] + 0x30))
    I = []
    il = q.resolve(pg, b + 0x30)
    for i in range(cnt(0x28)):
        r = q.resolve(il[0], il[1] + 8 * i)
        base = q.pbase(r[0]) + r[1]
        proto = q.pstr(*q.resolve(r[0], r[1] + 8))
        nsub, nj = struct.unpack_from('<II', q.d, base + 0x20)
        M = np.frombuffer(q.d[base + 0x80:base + 0xc0], '<f4').reshape(4, 4)
        bb = np.frombuffer(q.d[base + 0x60:base + 0x80], '<f4')
        subs = []
        sr = q.resolve(r[0], r[1] + 0x30)
        for k in range(nsub):
            so = sr[1] + 0x50 * k
            sn = q.pstr(*q.resolve(sr[0], so))
            ns = q.pu32(sr[0], so + 0x10)
            st = streams(q, q.resolve(sr[0], so + 0x18), ns) if ns else []
            subs.append(dict(name=sn, st=st, raw=q.d[q.pbase(sr[0]) + so + 0x20:q.pbase(sr[0]) + so + 0x30].hex()))
        I.append(dict(proto=proto, id=q.pu32(r[0], r[1] + 4), M=M, bb=bb, subs=subs, nj=nj))
    return P, I


def login_types(q):
    return [q.item_at(pg, off).type for pg, off in q.login['entries']]


def cmp(a, b, verbose=True):
    pa, pb = Pak(a), Pak(b)
    out = []
    la, lb = login_types(pa), login_types(pb)
    out.append('login order %s' % ('MATCH' if la == lb else 'DIFF %s vs %s' % (la, lb)))
    ca = collections.Counter(i.type for i in pa.items); cb = collections.Counter(i.type for i in pb.items)
    out.append('item types: ' + ', '.join('%s %d/%d' % (t, ca[t], cb[t]) for t in sorted(set(ca) | set(cb))))
    Ga, Gb = geometry(pa), geometry(pb)
    Sa = {s['name']: s for s in Ga['subs']}; Sb = {s['name']: s for s in Gb['subs']}
    out.append('submeshes %d/%d, common %d' % (len(Sa), len(Sb), len(set(Sa) & set(Sb))))
    miss = sorted(set(Sb) - set(Sa)); extra = sorted(set(Sa) - set(Sb))
    if miss: out.append('  missing: %s' % miss[:10])
    if extra: out.append('  extra: %s' % extra[:10])
    err = collections.defaultdict(list); bad = []
    for n in sorted(set(Sa) & set(Sb)):
        x, y = Sa[n], Sb[n]
        if (x['nv'], x['ni']) != (y['nv'], y['ni']):
            bad.append('%s nv/ni %d/%d vs %d/%d' % (n, x['nv'], x['ni'], y['nv'], y['ni'])); continue
        if (x['idx'] != y['idx']).any(): bad.append('%s indices differ' % n)
        if x['mat'].split(',')[0] != y['mat'].split(',')[0]:
            err['material-name-diff'].append(1)
        for which in ('streams', 'streams2'):
            sx = [(s['slot'], s['type']) for s in x[which]]; sy = [(s['slot'], s['type']) for s in y[which]]
            if sx != sy:
                bad.append('%s %s layout %s vs %s' % (n, which, sx, sy)); continue
            for s, t in zip(x[which], y[which]):
                if s['data'] is not None and t['data'] is not None:
                    err['%s slot%d' % (which, s['slot'])].append(float(np.abs(s['data'] - t['data']).max()))
        if x['raw'][0x10:0x14] != y['raw'][0x10:0x14]:
            bad.append('%s flags %s vs %s' % (n, x['raw'][0x10:0x14].hex(), y['raw'][0x10:0x14].hex()))
    Pa, Ia = protos_insts(pa); Pb, Ib = protos_insts(pb)
    out.append('prototypes %d/%d; instances %d/%d' % (len(Pa), len(Pb), len(Ia), len(Ib)))
    for n in sorted(set(Pa) & set(Pb)):
        if Pa[n] != Pb[n]: bad.append('proto %s %s vs %s' % (n, Pa[n], Pb[n]))
    if len(Ia) == len(Ib):
        for x, y in zip(Ia, Ib):
            if x['proto'] != y['proto'] or len(x['subs']) != len(y['subs']):
                bad.append('inst %s/%s mismatch' % (x['proto'], y['proto'])); continue
            err['inst matrix'].append(float(np.abs(x['M'] - y['M']).max()))
            err['inst bbox'].append(float(np.abs(x['bb'] - y['bb']).max()))
            for s, t in zip(x['subs'], y['subs']):
                lx = [(k['slot'], k['type']) for k in s['st']]; ly = [(k['slot'], k['type']) for k in t['st']]
                if sorted(lx) != sorted(ly):
                    bad.append('inst %s %s streams %s vs %s' % (x['proto'], s['name'], lx, ly)); continue
                if lx != ly:
                    err['inst stream order differs (cosmetic)'].append(0.0)
                if s['raw'] != t['raw']:
                    bad.append('instsub %s %s +0x20 %s vs %s' % (x['proto'], s['name'], s['raw'], t['raw']))
                d2 = {k['slot']: k for k in t['st']}
                for k1 in s['st']:
                    k2 = d2[k1['slot']]
                    err['inststream slot%d' % k1['slot']].append(float(np.abs(k1['data'] - k2['data']).max()))
    for k, v in sorted(err.items()):
        out.append('  %-22s n=%-4d max %.4g median %.3g' % (k, len(v), max(v), float(np.median(v))))
    out.append('%d structural differences' % len(bad))
    out += ['   ' + x for x in bad[:40]]
    if verbose:
        print('\n'.join(out))
    return bad


if __name__ == '__main__':
    cmp(sys.argv[1], sys.argv[2])
