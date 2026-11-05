"""Dump LIGHT_TABLE_1 headers, sun record and local lights of PS3 source / overlay / retail paks."""
import sys, struct
from llib import *


def table(p):
    its = p.items_of('LIGHT_TABLE_1')
    if not its: return None
    it = its[0]; pg, b = it.page, it.data_off; base = p.pbase(pg)
    ps3 = p.ptrsize == 4
    H = 0x60 if ps3 else 0xa0
    hdr = [p.pu32(pg, b + o) for o in range(0, H, 4)]
    out = {'hdr': hdr, 'size': it.size}
    if ps3:
        cnt8, nlocal, n18, n1c = hdr[2], hdr[4], hdr[6], hdr[7]
        sun = p.resolve(pg, b + 0x34); loc = p.resolve(pg, b + 0x30); rec = p.resolve(pg, b + 0x28)
        E = '>'
    else:
        cnt8, nlocal, n18, n1c = hdr[4], hdr[6], hdr[8], hdr[9]
        sun = p.resolve(pg, b + 0x48); loc = p.resolve(pg, b + 0x40); rec = p.resolve(pg, b + 0x30)
        E = '<'
    out['counts'] = dict(rec=cnt8, local=nlocal, gobo=n18, probes=n1c)

    def words(r, n, stride):
        o = p.pbase(r[0]) + r[1]
        res = []
        for k in range(n):
            w = [struct.unpack_from(E + 'f', p.d, o + k * stride + x)[0] for x in range(0, stride, 4)]
            raw = p.d[o + k * stride + 0x30:o + k * stride + 0x34]
            res.append((w, raw))
        return res
    out['sun'] = words(sun, 1, 0x3c)[0] if sun else None
    out['local'] = words(loc, nlocal, 0x48) if loc and nlocal else []
    out['rec'] = words(rec, cnt8, 0x3c) if rec and cnt8 else []
    return out


def show(tag, p):
    t = table(p)
    if t is None: print(tag, 'no LIGHT_TABLE_1'); return
    print('==', tag, 'size', hex(t['size']), t['counts'])
    print('  hdr', [hex(x) for x in t['hdr']])
    if t['sun']:
        w, raw = t['sun']
        print('  sun', ' '.join('%.4g' % x for x in w[:12]), '| raw+30', raw.hex(), '|', ' '.join('%.4g' % x for x in w[13:]))
    for k, (w, raw) in enumerate(t['local'][:int(os.environ.get('NLOC', '6'))]):
        print('  loc%d' % k, ' '.join('%.4g' % x for x in w[:12]), '| raw', raw.hex(), '|', ' '.join('%.4g' % x for x in w[13:]))
    for k, (w, raw) in enumerate(t['rec'][:4]):
        print('  rec%d' % k, ' '.join('%.4g' % x for x in w[:12]), '| raw', raw.hex(), '|', ' '.join('%.4g' % x for x in w[13:]))
    return t


if __name__ == '__main__':
    for a in sys.argv[1:]:
        kind, name = a.split(':', 1)
        if kind == 'ps3':
            print('ps3 src', ps3_find(name)[:1]); p = Pak(ps3_read(name))
        elif kind == 'ov': p = Pak(OV + name)
        elif kind == 'rt': p = Pak(RT + name)
        show(a, p)
