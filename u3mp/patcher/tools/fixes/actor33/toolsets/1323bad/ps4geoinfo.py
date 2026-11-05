"""Summarise a PS4 actor pak's GEOMETRY_1: materials, submeshes (streams, skin), prototypes
(+0x30/+0x34 cbuffer capacities), FG sections (skeleton sid / hierarchy id).
usage: ps4geoinfo.py <ps4.pak>..."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak


def info(p, out=sys.stdout):
    w = out.write
    for g in p.items_of('GEOMETRY_1'):
        pg, b = g.page, g.data_off
        nsub = p.u64(p.pbase(pg) + b + 8); sr = p.resolve(pg, b + 0x10)
        nproto = p.u64(p.pbase(pg) + b + 0x18); pr = p.resolve(pg, b + 0x20)
        ntex = p.u64(p.pbase(pg) + b + 0x38)
        nmat = p.u64(p.pbase(pg) + b + 0x48); mr = p.resolve(pg, b + 0x50)
        w('GEOMETRY_1 v%#x flags %#x: %d submeshes, %d prototypes, %d textures, %d materials\n' % (
            p.pu32(pg, b), p.pu32(pg, b + 4), nsub, nproto, ntex, nmat))
        for i in range(nmat):
            o = mr[1] + 0x48 * i
            w('  mat %d %s | %s\n' % (i, p.pstr(*p.resolve(mr[0], o)), p.pstr(*p.resolve(mr[0], o + 8))))
        for i in range(nsub):
            o = sr[1] + 0x88 * i
            nm = p.pstr(*p.resolve(sr[0], o + 8))
            flags = p.pu32(sr[0], o + 0x14)
            nv, ni, ns = struct.unpack_from('<III', p.d, p.pbase(sr[0]) + o + 0x20)
            st = p.resolve(sr[0], o + 0x30)
            fmts = []
            for k in range(ns):
                so = st[1] + 0x18 * k
                f = p.resolve(st[0], so + 8)
                fb = p.d[p.pbase(f[0]) + f[1]:p.pbase(f[0]) + f[1] + 4]
                fmts.append('%d/%d/%#x' % (fb[0], p.pu32(st[0], so) >> 16, fb[3]))
            mat = p.pstr(*p.resolve(sr[0], o + 0x48))
            nj = p.pu32(sr[0], o + 0x58)
            sk = p.resolve(sr[0], o + 0x60)
            w('  sub %d %s flags %#x nv %d ni %d streams [%s] njoints %d skin %s mat %s\n' % (
                i, nm, flags, nv, ni, ' '.join(fmts), nj, 'yes' if sk else 'no', mat.split(',')[0]))
        for i in range(nproto):
            r = p.resolve(pr[0], pr[1] + 8 * i)
            a, c = struct.unpack_from('<II', p.d, p.pbase(r[0]) + r[1] + 0x30)
            w('  proto %d %s nsub %d cb3 %d cb4 %d\n' % (i, p.pstr(*p.resolve(r[0], r[1] + 0x10)),
                                                       p.pu32(r[0], r[1] + 4), a, c))
    for it in p.items_of('FOREGROUND_SECTION_2'):
        b = p.pbase(it.page) + it.data_off
        skel, hier = struct.unpack_from('<II', p.d, b + 8)
        w('  FG %s skel %#x hier %#x lod %g proto %s\n' % (it.name, skel, hier, struct.unpack_from('<f', p.d, b + 0x18)[0], p.cstr(b + 0x1c)))
    for it in p.items_of('JOINT_HIERARCHY'):
        b = p.pbase(it.page) + it.data_off
        w('  JOINT_HIERARCHY %s size %#x words %s\n' % (it.name, it.size, p.d[b:b + 0x40].hex()))


if __name__ == '__main__':
    for f in sys.argv[1:]:
        p = Pak(f)
        print('==', os.path.basename(f))
        info(p)
