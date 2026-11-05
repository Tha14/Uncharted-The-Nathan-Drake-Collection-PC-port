#!/usr/bin/env python3
"""JOINT_HIERARCHY: PS3 (ICE "JH\\x01\\x0a" skeleton, 32-bit BE item) -> PS4 (OrbisAnim "AH\\x01\\x06", 64-bit LE).

PS3 item {u32 8, u32 nJoints, blob*, B*, names*, u32 0, D*, u32 skelSid}           (0x20)
PS4 item {u32 8, u32 nJoints, blob*, B*, names*, u64 0, D*, u32 skelSid, aa x3}    (0x40)
  names  {u32 sid, ptr char*}[n] -> {u32 sid, aa, ptr}[n], strings 16-aligned (aa fill)
  D      PS3 {3, zeros} (0x64) -> fixed PS4 template (0xb0)
  B      PS3 {arrays used by the PS3 runtime only, S* at +0x2c} -> PS4 zeros + S* at +0x50
  S      {u16* perGroup, float** perGroup, u8** perGroup} (k = group count)
  blob   header + pose + inverse bind + group tables + command list + sids + parents (see convert_blob)
Item order on PS4: header, names, strings, D, B, S tree, blob.  Verified byte-exact (FINDINGS.md 14).
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid as ndsid
from ps4writer import Blob

AA = 0xaaaaaaaa
D_TEMPLATE = bytes.fromhex(
    '03000000aaaaaaaa' + '00' * 0x4c + 'aaaaaaaa' + '00' * 0x14 + 'aaaaaaaa' + '00' * 0x34 + 'aa' * 12)
assert len(D_TEMPLATE) == 0xb0


def _sw32(b):
    return b''.join(b[i:i + 4][::-1] for i in range(0, len(b), 4))


def _sw16(b):
    return b''.join(b[i:i + 2][::-1] for i in range(0, len(b), 2))


def convert_blob(x):
    """PS3 'JH' skeleton blob -> PS4 'AH' blob"""
    h = struct.unpack_from('>24I', x, 0)
    assert h[0] == 0x4a48010a, 'not a PS3 JH blob: %08x' % h[0]
    nj = h[3] >> 16
    g0, g1 = x[0x14], x[0x15]
    groups = g0
    pose_o, inv_o, grp_o, cmd_o, sid_o, par_o = h[11], h[17], h[18], h[19], h[20], h[10]
    assert (h[4] >> 16) == nj and x[0x16:0x18] == b'\x01\x01', x[0xc:0x18].hex()
    pose = _sw32(x[pose_o:pose_o + 0x30 * nj])
    inv = _sw32(x[inv_o:inv_o + 0x30 * nj])
    grp = x[grp_o:grp_o + 8 * groups]
    csz, coff = struct.unpack_from('>II', x, cmd_o)
    cmd = bytearray(_sw16(x[coff:coff + csz]))
    loc = struct.unpack_from('<H', cmd, 4)[0]
    assert loc == 0x000c, 'command list context location %#x' % loc
    struct.pack_into('<H', cmd, 4, 0x0003)            # evaluation context: PS3 base 0xc -> PS4 base 3
    sids = _sw32(x[sid_o:sid_o + 4 * nj])
    par_len = len(x) - par_o
    assert par_len == 0x10 * nj, (par_len, nj)
    pars = _sw32(x[par_o:])

    def al(n, a=16):
        return (n + a - 1) // a * a
    o_pose = 0x50
    o_inv = o_pose + len(pose)
    o_a = o_inv + len(inv)
    o_b = o_a + 0x20
    o_c = o_b + al(8 * groups)
    o_cmd = o_c + 0x10
    o_d = o_cmd + al(len(cmd))
    d_sids = 0x20
    d_par = d_sids + al(4 * nj)
    d_size = d_par + len(pars)
    total = o_d + d_size + 0x20
    out = bytearray(total)
    struct.pack_into('<IIIHHIHHII', out, 0, 0x41480106, h[1], total, nj, nj, 0xff010000, 0, groups, groups, 1)
    struct.pack_into('<20I', out, 0, *struct.unpack_from('<20I', out, 0))
    struct.pack_into('<I', out, 0x28, o_pose)
    struct.pack_into('<IIIII', out, 0x30, o_inv, o_a, o_b, 0, o_c)
    struct.pack_into('<I', out, 0x4c, o_d)
    out[o_pose:o_pose + len(pose)] = pose
    out[o_inv:o_inv + len(inv)] = inv
    struct.pack_into('<IIHHBBBBI', out, o_a, 0, 0, 0, nj, g0, g1, 0, 1, nj)
    for g in range(groups):
        e = grp[8 * g:8 * g + 8]
        out[o_b + 8 * g:o_b + 8 * g + 8] = e[:4] + e[4:8][::-1]
    struct.pack_into('<HHI', out, o_c, 0, csz, o_cmd)
    out[o_cmd:o_cmd + len(cmd)] = cmd
    struct.pack_into('<HHI', out, o_d, 0, d_size, d_sids)
    struct.pack_into('<I', out, o_d + 0x14, d_par)
    out[o_d + d_sids:o_d + d_sids + len(sids)] = sids
    out[o_d + d_par:o_d + d_par + len(pars)] = pars
    return bytes(out)


def read_jh(p, it):
    pg, o = it.page, it.data_off
    r = p.resolve
    ver, nj = p.pu32(pg, o), p.pu32(pg, o + 4)
    ba = r(pg, o + 8)
    bb = p.pbase(ba[0]) + ba[1]
    blob = p.d[bb:bb + p.u32(bb + 8)]
    na = r(pg, o + 0x10)
    names = []
    for i in range(nj):
        s = p.pu32(na[0], na[1] + 8 * i)
        names.append((s, p.pstr(*r(na[0], na[1] + 8 * i + 4))))
    if p.pu32(pg, o + 0x14):
        raise NotImplementedError('JOINT_HIERARCHY +0x14 non-zero')
    d = r(pg, o + 0x18)
    if d:
        dd = p.d[p.pbase(d[0]) + d[1]:p.pbase(d[0]) + d[1] + 0x5c]
        if struct.unpack_from('>I', dd)[0] != 3 or any(dd[4:]):
            raise NotImplementedError('JOINT_HIERARCHY D block differs from {3, 0...}')
    B = r(pg, o + 0xc)
    S = None
    if B:
        sa = r(B[0], B[1] + 0x2c)
        if sa:
            S = read_s(p, sa, blob)
    return dict(ver=ver, nj=nj, blob=blob, names=names, d=d is not None, b=B is not None, s=S,
                sid=p.pu32(pg, o + 0x1c))


def read_s(p, sa, blob):
    """S {u16* perGroup (inline), float** , u8**}: k = group count of the skeleton blob"""
    r = p.resolve
    k = blob[0x14]
    p0, p1, p2 = r(sa[0], sa[1]), r(sa[0], sa[1] + 4), r(sa[0], sa[1] + 8)
    cnt = p.d[p.pbase(p0[0]) + p0[1]:p.pbase(p0[0]) + p0[1] + 2 * k]
    h = struct.unpack_from('>24I', blob, 0)
    grp = blob[h[18]:h[18] + 8 * k]
    groups = []
    for g in range(k):
        nf = cnt[2 * g] + cnt[2 * g + 1]
        nb = (grp[8 * g] + 1) & ~1                # joints of the group, rounded to even
        fa = r(p1[0], p1[1] + 4 * g)
        ba = r(p2[0], p2[1] + 4 * g)
        fl = p.d[p.pbase(fa[0]) + fa[1]:p.pbase(fa[0]) + fa[1] + 4 * nf]
        by = p.d[p.pbase(ba[0]) + ba[1]:p.pbase(ba[0]) + ba[1] + nb]
        groups.append((fl, by))
    return dict(cnt=cnt, groups=groups)


def build_jh(m):
    b = Blob('jointhier')
    b.u32(m['ver']).u32(m['nj'])
    rel = []                                     # (pos, target offset)

    def ptr_here(pos):
        rel.append((pos, b.pos))
    b.u64(0)                                     # +08 blob
    b.u64(0)                                     # +10 B
    b.u64(0)                                     # +18 names
    b.u64(0)                                     # +20
    b.u64(0)                                     # +28 D
    b.u32(m['sid']).u32(AA).u32(AA).u32(AA)      # +30
    # names
    ptr_here(0x18)
    ent = b.pos
    for s, _n in m['names']:
        b.u32(s).u32(AA).u64(0)
    for i, (_s, n) in enumerate(m['names']):
        ptr_here(ent + 16 * i + 8)
        b.cstr(n)
        b.pad(16, 0xaa)
    if m['d']:
        ptr_here(0x28)
        b.raw(D_TEMPLATE)
    if m['b']:
        ptr_here(0x10)
        bo = b.pos
        b.raw(struct.pack('<QIIQQIIQIIQQQQQ', 0, 0, AA, 0, 0, 0, AA, 0, 0, AA, 0, 0, 0, 0, 0))
        assert b.pos - bo == 0x60
        if m['s'] is not None:
            S = m['s']
            k = len(S['groups'])
            rel.append((bo + 0x50, b.pos))
            so = b.pos
            b.u64(0).u64(0).u64(0).u32(AA).u32(AA)
            rel.append((so, b.pos))
            b.raw(_sw16(S['cnt']))
            b.pad(16, 0xaa)
            rel.append((so + 8, b.pos))
            p1 = b.pos
            b.raw(b'\0' * 8 * k)
            b.pad(16, 0xaa)
            rel.append((so + 0x10, b.pos))
            p2 = b.pos
            b.raw(b'\0' * 8 * k)
            b.pad(16, 0xaa)
            for g, (fl, by) in enumerate(S['groups']):
                rel.append((p1 + 8 * g, b.pos))
                b.raw(_sw32(fl))
                b.pad(16, 0xaa)
                rel.append((p2 + 8 * g, b.pos))
                b.raw(by)
                b.pad(16, 0xaa)
    b.pad(16, 0xaa)
    ptr_here(0x08)
    b.raw(convert_blob(m['blob']))
    for pos, t in rel:
        b.relocs.append((pos, b, t))
    return b


def jh_blob(p, it):
    return build_jh(read_jh(p, it))


def check_pair(p3path, p4path, verbose=True):
    from fgcoll import place
    p, q = Pak(p3path), Pak(p4path)
    i3, i4 = p.items_of('JOINT_HIERARCHY')[0], q.items_of('JOINT_HIERARCHY')[0]
    b = place(jh_blob(p, i3), i4.data_off)
    ref = q.d[q.pbase(i4.page) + i4.data_off:q.pbase(i4.page) + i4.data_off + i4.size]
    nd = sum(1 for x, y in zip(b, ref) if x != y) + abs(len(b) - len(ref))
    first = next((k for k in range(min(len(b), len(ref))) if b[k] != ref[k]), None)
    if verbose:
        print('%-40s mine %#x retail %#x diff %d first %s' % (os.path.basename(p3path), len(b), len(ref), nd,
                                                             None if first is None else hex(first)))
    return nd


if __name__ == '__main__':
    if sys.argv[1] == 'check':
        check_pair(sys.argv[2], sys.argv[3])
