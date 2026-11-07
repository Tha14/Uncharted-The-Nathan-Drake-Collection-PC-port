"""Uncharted 2 VIS_INFO_1 (version 7): PS3 (32-bit BE) -> PS4 (64-bit LE).

Version 7 is the Uncharted 3 version 9 (u3mp paktools/visinfo.py, whose docstring has the full layout) without
the H table: the PS3 header ends at +0xa0 after {nG, G*}, the PS4 header at +0xd0 after {nG, aa, G*}.
Regions, nodes, leaf streams, faces, the four runtime buffers, the dictionary and G are the same.  Checked against
the retail PS4 copies of the shared multiplayer levels (u2conv/validate.py vis).  read7 / build_parts7 are the
U3 read / build_parts with those two differences and check_ps4_7 is check_ps4 for version 7 (generated from paktools/visinfo.py, GPL v2 like it).
"""
import os, struct, sys
from ps4writer import Blob
from visinfo import huff_len, _sw4, AA


def read7(p, it):
    pg, o = it.page, it.data_off
    u32 = lambda pg_, a: p.pu32(pg_, a)
    f = lambda pg_, a, n: p.d[p.pbase(pg_) + a:p.pbase(pg_) + a + n]

    def R(pg_, a):
        r = p.resolve(pg_, a)
        return r

    W = [u32(pg, o + 4 * k) for k in range(0x2a)]
    if W[0] != 7:
        raise NotImplementedError('VIS_INFO_1 version %d (Uncharted 2 uses 7)' % W[0])
    nreg, nwords, ndict, ng, nh = W[1], W[0x8c // 4], W[0x90 // 4], W[0x98 // 4], 0
    m = dict(W=W, nwords=nwords)
    # four runtime buffers
    m['bufs'] = []
    for k in range(4):
        base = 0x10 + 0x20 * k
        r = R(pg, o + base)
        words = W[base // 4 + 1: base // 4 + 8]
        m['bufs'].append((f(*r, 4 * nwords) if r else None, words))
    r = R(pg, o + 0x94)
    dic_raw = f(*r, 4 * ndict)
    dic = [struct.unpack_from('>HH', dic_raw, 4 * i) for i in range(ndict)]
    m['dict'] = dic
    # regions
    rr = R(pg, o + 8)
    regs = []
    for i in range(nreg):
        rp, ro = rr[0], rr[1] + 0x30 * i
        RW = [u32(rp, ro + 4 * k) for k in range(12)]
        npl, nfa, x = RW[0], RW[9], RW[11]
        reg = dict(aabb=f(rp, ro + 0xc, 24), npl=npl, nfa=nfa, x=x)
        q = R(rp, ro + 4)
        reg['planes'] = f(*q, 16 * npl) if q else b''
        if npl and not q:
            raise ValueError('planes')
        # faces
        q = R(rp, ro + 0x28)
        faces = []
        for j in range(nfa):
            fp, fo = q[0], q[1] + 8 * j
            nv = u32(fp, fo)
            v = R(fp, fo + 4)
            faces.append((nv, f(*v, 12 * nv) if v else None))
        reg['faces'] = faces
        # nodes: walk the tree from the root
        q = R(rp, ro + 8)
        nodes = {}
        if q:
            np_, no = q
            todo = [0]
            while todo:
                n = todo.pop()
                if n in nodes:
                    continue
                a, b, s = struct.unpack_from('>IiI', p.d, p.pbase(np_) + no + 16 * n)
                bp = R(np_, no + 16 * n + 12)
                nodes[n] = (a, b, s, bp)
                if bp is None:
                    left = struct.unpack('>i', struct.pack('>I', a))[0] >> 2
                    for c in (left, b):
                        if c >= 0:
                            todo.append(c)
            nn = max(nodes) + 1
            if sorted(nodes) != list(range(nn)):
                raise ValueError('vis node indices not contiguous')
        reg['nodes'] = [nodes[i] for i in range(len(nodes))]
        # leaf streams
        streams = {}
        for a, b, s, bp in reg['nodes']:
            if bp is not None and bp not in streams:
                raw = p.d[p.pbase(bp[0]) + bp[1]:p.pbase(bp[0]) + bp[1] + 0x10000]
                n, out = huff_len(raw, dic)
                if len(out) > 4 * nwords:
                    raise ValueError('vis leaf decodes to %d bytes, expected %d' % (len(out), 4 * nwords))
                streams[bp] = raw[:n]
        reg['streams'] = streams
        regs.append(reg)
    m['regions'] = regs
    # G
    q = R(pg, o + 0x9c)
    G = []
    for j in range(ng):
        gp, go = q[0], q[1] + 8 * j
        v = u32(gp, go)
        b = R(gp, go + 4)
        G.append((v, f(*b, 4 * nwords) if b else None))
    m['G'] = G
    m['H'] = []                 # version 7 has no H table
    return m


def build_parts7(m, limit=None):
    """-> list of blobs: [item blob, extra blobs...].  Objects are laid out in retail order (header, regions,
    H, per region planes / nodes / leaf streams / faces / verts, then the 4 buffers, dict, G, G bits), each
    16-aligned with 0xaa fill.  With `limit`, a new blob is started whenever the current one would grow past
    it (the extras become RAW_DATA items; retail splits big VIS items across pages the same way)."""
    W = m['W']
    regs = m['regions']
    objs = []                                   # (blob) in placement order

    def obj(name):
        o = Blob(name); objs.append(o); return o
    hd = obj('vis_hdr')
    regb = obj('vis_regions')
    reg_parts = []
    for ri, reg in enumerate(regs):
        parts = {}
        if reg['planes']:
            parts['planes'] = obj('vis_planes'); parts['planes'].raw(_sw4(reg['planes']))
        if reg['nodes']:
            nb = obj('vis_nodes'); parts['nodes'] = nb
            sb = {}
            for a, bb, s_, bp in reg['nodes']:
                nb.u32(a).i32(bb).u32(s_).u32(AA)
                if bp is None:
                    nb.u64(0)
                else:
                    if bp not in sb:
                        sb[bp] = Blob('vis_leaf'); sb[bp].raw(reg['streams'][bp])
                    nb.ptr(sb[bp])
            # streams in node order
            seen = set()
            for a, bb, s_, bp in reg['nodes']:
                if bp is not None and bp not in seen:
                    seen.add(bp); objs.append(sb[bp])
        if reg['faces']:
            fb = obj('vis_faces'); parts['faces'] = fb
            vbs = []
            for nv, v in reg['faces']:
                fb.u32(nv).u32(AA)
                if v is None:
                    fb.u64(0)
                else:
                    vb = Blob('vis_verts'); vb.raw(_sw4(v)); vbs.append(vb); fb.ptr(vb)
            objs.extend(vbs)
        reg_parts.append(parts)
    for reg, parts in zip(regs, reg_parts):
        regb.ptr(parts.get('planes')).ptr(parts.get('nodes')).ptr(parts.get('faces'))
        regb.raw(_sw4(reg['aabb']))
        regb.u32(reg['npl']).u32(reg['nfa']).u32(reg['x']).u32(AA)
    bufs = []
    for k in range(4):
        data = m['bufs'][k][0]
        if data is None:
            bufs.append(None); continue
        bb = obj('vis_buf'); bb.raw(_sw4(data)); bufs.append(bb)
    db = obj('vis_dict')
    for l, r in m['dict']:
        db.u16(l).u16(r)
    gb = None
    if m['G']:
        gb = obj('vis_G')
        gbits = []
        for v, bits in m['G']:
            gb.u32(v).u32(AA)
            if bits is None:
                gb.u64(0)
            else:
                x = Blob('vis_Gbits'); x.raw(bits); gbits.append(x); gb.ptr(x)
        objs.extend(gbits)
    # header
    hd.u32(7).u32(len(regs)).ptr(regb)
    hd.u32(W[3]).u32(0)
    for k in range(4):
        hd.ptr(bufs[k])
        for w in m['bufs'][k][1]:
            hd.u32(w)
        if k < 3:
            hd.u32(0)
    hd.u32(len(m['dict'])).ptr(db)
    hd.u32(len(m['G'])).u32(AA).ptr(gb)
    assert hd.pos == 0xd0
    # pack
    out = [Blob('vis_info')]
    for o in objs:
        cur = out[-1]
        if limit and len(cur.b) and len(cur.b) + 15 + len(o.b) > limit:
            out.append(Blob('vis_part'))
            cur = out[-1]
        if len(cur.b):
            cur.embed(o, 16, 0xaa)
        else:
            cur.embed(o, 1, 0xaa)
    return out


def check_ps4_7(p, it):
    """walk a PS4 VIS_INFO_1 the way vis-probe does: regions, trees (every node reachable, leaves carry
    streams), every leaf stream decodes to an end code within nWords*4 bytes, faces/planes/G resolve."""
    pg, o = it.page, it.data_off
    rd = lambda a, n: p.d[p.pbase(a[0]) + a[1]:p.pbase(a[0]) + a[1] + n]
    U = lambda a, k=0: struct.unpack_from('<I', p.d, p.pbase(a[0]) + a[1] + k)[0]
    R = lambda a, k=0: p.resolve(a[0], a[1] + k)
    h = (pg, o)
    if U(h) != 7:
        raise ValueError('version')
    nreg, nwords, ndict, ng, nh = U(h, 4), U(h, 0xb0), U(h, 0xb4), U(h, 0xc0), 0
    dr = R(h, 0xb8)
    dic = [struct.unpack_from('<HH', rd(dr, 4 * ndict), 4 * i) for i in range(ndict)]
    for l, r in dic:
        for s in (l, r):
            if s > 0x100 + ndict:
                raise ValueError('dict symbol out of range')
    for k in range(4):
        if R(h, 0x18 + 0x28 * k) is None and nwords:
            raise ValueError('buffer %d missing' % k)
    regs = R(h, 8)
    nn = nl = 0
    for i in range(nreg):
        rg = (regs[0], regs[1] + 0x40 * i)
        npl, nfa = U(rg, 0x30), U(rg, 0x34)
        if npl and R(rg, 0) is None:
            raise ValueError('planes')
        fa = R(rg, 0x10)
        for j in range(nfa):
            if R(fa, 16 * j + 8) is None:
                raise ValueError('face verts')
        nd = R(rg, 8)
        if nd is None:
            continue
        seen, todo = set(), [0]
        while todo:
            n = todo.pop()
            if n in seen:
                continue
            seen.add(n)
            a, b = struct.unpack_from('<Ii', p.d, p.pbase(nd[0]) + nd[1] + 0x18 * n)
            sp = R(nd, 0x18 * n + 0x10)
            if sp is not None:
                ln, out = huff_len(rd(sp, 0x10000), dic)
                if len(out) > 4 * nwords:
                    raise ValueError('leaf stream too long')
                nl += 1
            else:
                left = struct.unpack('<i', struct.pack('<I', a))[0] >> 2
                todo += [c for c in (left, b) if c >= 0]
        nn += len(seen)
    g = R(h, 0xc8)
    for j in range(ng):
        if R(g, 16 * j + 8) is None:
            raise ValueError('G bits')
    return '%d regions, %d nodes, %d leaves, %d words, dict %d' % (nreg, nn, nl, nwords, ndict)


def vis_blobs(p, limit=0x100000):
    """[item blob, extra blobs] for the VIS_INFO_1 of a U2 PS3 pak (None when it has none); the PS3 PVS words stay
    in their big-endian order, as on retail PS4 (see paktools/visinfo.py _maybe_swap)"""
    its = p.items_of('VIS_INFO_1')
    if not its:
        return None
    m = read7(p, its[0])
    one = build_parts7(m)
    if len(one[0].b) <= limit:
        return one
    return build_parts7(m, limit)
