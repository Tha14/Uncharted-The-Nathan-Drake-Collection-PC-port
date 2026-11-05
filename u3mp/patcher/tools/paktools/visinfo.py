#!/usr/bin/env python3
"""VIS_INFO_1 (occlusion / PVS): PS3 (32-bit BE) -> PS4 (64-bit LE).  See FINDINGS.md section 16.

Reader on PS4: login 0x10e34f0 (case sid 0x215902ef, stores the item at background+0x160 -> +0xf8 when
version == 9), vis-probe.cpp 0xf22810 -> decode 0xf223e0, tree walk 0xf237e0.

PS3 header (0xa8):                       PS4 header (0xe0):
  +00 u32 version 9                        +00 version, +04 nRegions
  +04 u32 nRegions                         +08 regions*            (nRegions x 0x40)
  +08 regions* (stride 0x30)               +10 u32 (PS3 +0c), +14 0
  +0c u32 0                                +18/+40/+68/+90 4 x {ptr bits, u32 x7}, then u32 0
  +10/+30/+50/+70 4 x {ptr bits, u32 x7}     (runtime bit buffers, nWords u32 each; nWords = E word 7)
  +90 nDict  +94 dict*                     +b4 nDict  +b8 dict*  +c0 nG (+aa)  +c8 G*  +d0 H*  +d8 nH (+aa)
  +98 nG  +9c G*  +a0 H*  +a4 nH
Region PS3 0x30: {nPlanes, planes*, nodes*, aabb f32[6], nFaces, faces*, u32 x}
       PS4 0x40: {planes*, nodes*, faces*, aabb f32[6], nPlanes, nFaces, x, aa}
Plane: vec4.  Node PS3 {u32 a, i32 b, f32 split, bits*} -> PS4 {a, b, split, aa, bits*} (0x18).
  a = left<<2 | axis, b = right child; a leaf has a bits pointer (a = -4, b = -1).
Face PS3 {u32 nVerts, verts*} -> PS4 {nVerts, aa, verts*}; verts are vec3 floats.
Dict: u16 pairs {left, right} = a Huffman tree, root = last entry; symbol < 0x100 is a literal byte,
  0x100 = end of stream, 0x101 + i = dict entry i.  Leaf bit streams are read MSB first; PS4 keeps
  exactly the bytes up to the end-of-stream code (decoded output = nWords*4 bytes).
G: nG x {u32, bits*} -> {u32, aa, bits*}; bits = nWords*4 raw bytes.  H: nH x {sid, region*} ->
  {sid, aa, null} (retail leaves the pointer null).
PS4 object order (retail cha-entrance-fire-low): header, regions, H, then per region planes, nodes,
leaf streams (node order, 16-aligned, 0xaa fill), faces, verts; then B, C, D, E, dict, G, G bits.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps4writer import Blob

AA = 0xaaaaaaaa


def _sw4(x):
    return b''.join(x[i:i + 4][::-1] for i in range(0, len(x), 4))


def huff_len(stream, dic, nout=None):
    """number of bytes of `stream` consumed up to the end-of-stream code; also the decoded bytes"""
    nd = len(dic)
    root = (dic[nd - 1][0], dic[nd - 1][1])
    l, r = root
    out = bytearray()
    for i, byte in enumerate(stream):
        m = 0x80
        while m:
            s = r if byte & m else l
            m >>= 1
            if s < 0x100:
                out.append(s)
                l, r = root
            elif s == 0x100:
                return i + 1, bytes(out)
            else:
                l, r = dic[s - 0x101]
    raise ValueError('vis stream without end code (%d bytes, %d decoded)' % (len(stream), len(out)))


def read(p, it):
    pg, o = it.page, it.data_off
    u32 = lambda pg_, a: p.pu32(pg_, a)
    f = lambda pg_, a, n: p.d[p.pbase(pg_) + a:p.pbase(pg_) + a + n]

    def R(pg_, a):
        r = p.resolve(pg_, a)
        return r

    W = [u32(pg, o + 4 * k) for k in range(0x2a)]
    if W[0] != 9:
        raise NotImplementedError('VIS_INFO_1 version %d' % W[0])
    nreg, nwords, ndict, ng, nh = W[1], W[0x8c // 4], W[0x90 // 4], W[0x98 // 4], W[0xa4 // 4]
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
    q = R(pg, o + 0xa0)
    m['H'] = [u32(q[0], q[1] + 8 * j) for j in range(nh)]
    return m


def build_parts(m, limit=None):
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
    hb = obj('vis_H')
    for sid in m['H']:
        hb.u32(sid).u32(AA).u64(0)
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
    hd.u32(9).u32(len(regs)).ptr(regb)
    hd.u32(W[3]).u32(0)
    for k in range(4):
        hd.ptr(bufs[k])
        for w in m['bufs'][k][1]:
            hd.u32(w)
        if k < 3:
            hd.u32(0)
    hd.u32(len(m['dict'])).ptr(db)
    hd.u32(len(m['G'])).u32(AA).ptr(gb).ptr(hb)
    hd.u32(len(m['H'])).u32(AA)
    assert hd.pos == 0xe0
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


def build(m):
    return build_parts(m)[0]


def check_ps4(p, it):
    """walk a PS4 VIS_INFO_1 the way vis-probe does: regions, trees (every node reachable, leaves carry
    streams), every leaf stream decodes to an end code within nWords*4 bytes, faces/planes/G resolve."""
    pg, o = it.page, it.data_off
    rd = lambda a, n: p.d[p.pbase(a[0]) + a[1]:p.pbase(a[0]) + a[1] + n]
    U = lambda a, k=0: struct.unpack_from('<I', p.d, p.pbase(a[0]) + a[1] + k)[0]
    R = lambda a, k=0: p.resolve(a[0], a[1] + k)
    h = (pg, o)
    if U(h) != 9:
        raise ValueError('version')
    nreg, nwords, ndict, ng, nh = U(h, 4), U(h, 0xb0), U(h, 0xb4), U(h, 0xc0), U(h, 0xd8)
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
    """[item blob, extra blobs] for the first VIS_INFO_1 of a PS3 pak (None when it has none)"""
    its = p.items_of('VIS_INFO_1')
    if not its:
        return None
    m = _maybe_swap(read(p, its[0]))
    one = build_parts(m)
    if len(one[0].b) <= limit:
        return one
    return build_parts(m, limit)


# ---------------------------------------------------------------------------------------------------------
# PVS bit layout (agent O, 2026-10-28).  The decoded leaf / G bit sets are arrays of u32 words in which
# bit k = (word[k >> 5] >> (k & 31)) & 1 indexes instance id k (and k+1+j = submesh j; ids come from PS3
# instance x8 & 0xffff).  PS3 stores the words big-endian.  The PS4 runtime decodes leaf bytes in order and
# ANDs them into an all-ones byte buffer (vis-probe 0xf223e0), and mesh-gpu-inst.cpp (0xe9530a) tests
# `bt idx, dword[buf + (idx >> 5) * 4]` = little-endian words.  Copying PS3 streams verbatim therefore
# scrambles the culling (bit k of PS3 lands on bit k ^ 0x18): about half of every room's instances were
# culled from inside the room (Fort "floating slabs / missing buildings", white voids).  Self-visibility
# check (b87o/viscoh4.py, 60 points, Fort): PS3 layout 100%, verbatim-on-PS4 53%.
# Fix: decode each leaf, pad to whole words with 0xff (bytes past the stream stay 1 in the runtime buffer),
# byte-swap every u32, re-encode.  The Huffman dict is reused when it codes every needed byte, else rebuilt.

def _codes(dic):
    nd = len(dic); out = {}
    stack = [(0x101 + nd - 1, '')]
    while stack:
        s, c = stack.pop()
        if s <= 0x100:
            out[s] = c
        else:
            l, r = dic[s - 0x101]
            stack.append((l, c + '0')); stack.append((r, c + '1'))
    return out


def _huffman(freq):
    """byte/EOS frequencies -> dict [(left, right)], root last (the format the decoder walks)"""
    import heapq
    h = [(f, i, s) for i, (s, f) in enumerate(sorted(freq.items())) if f]
    heapq.heapify(h); dic = []; k = len(h)
    if len(h) == 1:
        f, _, s = h[0]; dic.append((s, s)); return dic
    while len(h) > 1:
        f1, _, a = heapq.heappop(h); f2, _, b = heapq.heappop(h)
        dic.append((a, b)); k += 1
        heapq.heappush(h, (f1 + f2, k, 0x101 + len(dic) - 1))
    return dic


def _encode(data, codes):
    bits = ''.join(codes[b] for b in data) + codes[0x100]
    bits += '0' * (-len(bits) % 8)
    return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))


def swap_bits(m):
    """in place: PS3 big-endian PVS words -> PS4 little-endian (leaf streams, G bit sets)"""
    dic = m['dict']; nw = m['nwords']
    dec = {}
    for reg in m['regions']:
        for bp, raw in reg['streams'].items():
            _, out = huff_len(raw + b'\0' * 8, dic)
            out = out + b'\xff' * (-len(out) % 4)
            dec[(id(reg), bp)] = _sw4(out)[:4 * nw]
    codes = _codes(dic)
    need = set(b for v in dec.values() for b in v) | {0x100}
    if not need <= set(codes):
        freq = {}
        for v in dec.values():
            for b in v: freq[b] = freq.get(b, 0) + 1
            freq[0x100] = freq.get(0x100, 0) + 1
        dic = _huffman(freq); m['dict'] = dic; codes = _codes(dic)
        m['vis_dict_rebuilt'] = True
    for reg in m['regions']:
        for bp in list(reg['streams']):
            reg['streams'][bp] = _encode(dec[(id(reg), bp)], codes)
    m['G'] = [(v, _sw4(b) if b is not None else None) for v, b in m['G']]
    m['vis_swapped'] = True
    return m


# agent P (2026-10-29): DEFAULT OFF.  Bluepoint's own retail PS4 paks keep the PS3 big-endian word layout verbatim
# (aga/maps/b87o/bitunion.py on retail airport-3b-low: BE puts the never-visible bits exactly on the unused tail 112-127 and gives
# instance/submesh pairs equal counts; LE would make instance 98 invisible from all 10600 leaves; cha-entrance-fire-low the
# same), and the PS4 runtime has a movbe (big-endian) PVS test at 0xea130a on the same instance vis id (+0xd4).  Converting
# verbatim = retail format.  The Fort lobby stage, which has no VIS_INFO_1 at all, shows the same floating pieces, so the
# Fort symptoms are not occlusion (they are hole-making templates, fixed by mkplan v2.2 / batch82).  U3_VIS_SWAP=1 re-enables.
def _maybe_swap(m):
    return swap_bits(m) if os.environ.get('U3_VIS_SWAP', '0') == '1' else m


def vis_blob(p, it=None):
    if it is None:
        its = p.items_of('VIS_INFO_1')
        if not its:
            return None
        it = its[0]
    return build(_maybe_swap(read(p, it)))


if __name__ == '__main__':
    from ndpak import Pak
    p = Pak(sys.argv[1])
    for it in p.items_of('VIS_INFO_1'):
        m = read(p, it)
        b = build(m)
        print('%s: %d regions, %d words, dict %d, %d nodes, %d bytes' % (
            sys.argv[1], len(m['regions']), m['nwords'], len(m['dict']),
            sum(len(r['nodes']) for r in m['regions']), len(b.b)))
        if len(sys.argv) > 2:
            q = Pak(sys.argv[2])
            ri = q.items_of('VIS_INFO_1')[0]
            ref = q.d[q.pbase(ri.page) + ri.data_off:q.pbase(ri.page) + ri.data_off + ri.size]
            mine = bytearray(b.b)
            for pos, t, off in b.relocs:
                rt, ro = t.root()
                mine[pos:pos + 8] = struct.pack('<Q', ro + off)
            refb = bytearray(ref)
            for o in range(0, len(refb) - 7, 8):
                if q.is_ptr(ri.page, ri.data_off + o):
                    refb[o:o + 8] = struct.pack('<Q', q.pptr(ri.page, ri.data_off + o) - ri.data_off)
            print('retail %d bytes, mine %d, identical=%s' % (len(refb), len(mine), bytes(refb) == bytes(mine)))
            if bytes(refb) != bytes(mine):
                d = [i for i in range(min(len(refb), len(mine))) if refb[i] != mine[i]]
                print('first diffs', [hex(x) for x in d[:20]])


def read_ps4(p, it):
    """PS4 VIS_INFO_1 -> same dict as read() plus 'layout': [(name, page, off)] of every object"""
    pg, o = it.page, it.data_off
    rd = lambda a, n: p.d[p.pbase(a[0]) + a[1]:p.pbase(a[0]) + a[1] + n]
    U = lambda a, k=0: struct.unpack_from('<I', p.d, p.pbase(a[0]) + a[1] + k)[0]
    R = lambda a, k=0: p.resolve(a[0], a[1] + k)
    h = (pg, o)
    lay = []
    nreg, nwords, ndict, ng, nh = U(h, 4), U(h, 0xb0), U(h, 0xb4), U(h, 0xc0), U(h, 0xd8)
    m = dict(nwords=nwords)
    m['W'] = [U(h, 4 * k) for k in range(0x38)]
    dr = R(h, 0xb8); lay.append(('dict',) + dr)
    m['dict'] = [struct.unpack_from('<HH', rd(dr, 4 * ndict), 4 * i) for i in range(ndict)]
    m['bufs'] = []
    for k in range(4):
        r = R(h, 0x18 + 0x28 * k)
        if r: lay.append(('buf%d' % k,) + r)
        m['bufs'].append((_sw4(rd(r, 4 * nwords)) if r else None, [U(h, 0x20 + 0x28 * k + 4 * j) for j in range(7)]))
    regs = R(h, 8); lay.append(('regions',) + regs)
    hh = R(h, 0xd0); lay.append(('H',) + hh)
    m['H'] = [U(hh, 16 * j) for j in range(nh)]
    m['Hptr'] = [R(hh, 16 * j + 8) for j in range(nh)]
    m['regions'] = []
    for i in range(nreg):
        rg = (regs[0], regs[1] + 0x40 * i)
        reg = dict(aabb=_sw4(rd(rg, 24) if False else rd((rg[0], rg[1] + 0x18), 24)), npl=U(rg, 0x30),
                   nfa=U(rg, 0x34), x=U(rg, 0x38))
        pl = R(rg, 0)
        if pl: lay.append(('r%d.planes' % i,) + pl)
        reg['planes'] = _sw4(rd(pl, 16 * reg['npl'])) if pl else b''
        fa = R(rg, 0x10)
        faces = []
        if fa:
            lay.append(('r%d.faces' % i,) + fa)
            for j in range(reg['nfa']):
                v = R(fa, 16 * j + 8); nv = U(fa, 16 * j)
                if v: lay.append(('r%d.verts%d' % (i, j),) + v)
                faces.append((nv, _sw4(rd(v, 12 * nv)) if v else None))
        reg['faces'] = faces
        nd = R(rg, 8)
        nodes, streams = [], {}
        if nd:
            lay.append(('r%d.nodes' % i,) + nd)
            seen, todo = set(), [0]
            while todo:
                n = todo.pop()
                if n in seen: continue
                seen.add(n)
                a, b = struct.unpack_from('<Ii', p.d, p.pbase(nd[0]) + nd[1] + 0x18 * n)
                if R(nd, 0x18 * n + 0x10) is None:
                    left = struct.unpack('<i', struct.pack('<I', a))[0] >> 2
                    todo += [c for c in (left, b) if c >= 0]
            for n in range(max(seen) + 1):
                a, b, s = struct.unpack_from('<IiI', p.d, p.pbase(nd[0]) + nd[1] + 0x18 * n)
                sp = R(nd, 0x18 * n + 0x10)
                if sp is not None and sp not in streams:
                    lay.append(('r%d.leaf%d' % (i, n),) + sp)
                    ln, _ = huff_len(rd(sp, 0x10000), m['dict'])
                    streams[sp] = rd(sp, ln)
                nodes.append((a, b, s, sp))
        reg['nodes'] = nodes
        reg['streams'] = streams
        m['regions'].append(reg)
    g = R(h, 0xc8)
    m['G'] = []
    if g:
        lay.append(('G',) + g)
        for j in range(ng):
            b = R(g, 16 * j + 8)
            if b: lay.append(('Gbits%d' % j,) + b)
            m['G'].append((U(g, 16 * j), rd(b, 4 * nwords) if b else None))
    m['layout'] = lay
    return m


def compare(ps3pak, ps4pak):
    from ndpak import Pak
    a, b = Pak(ps3pak), Pak(ps4pak)
    A = read(a, a.items_of('VIS_INFO_1')[0])
    B = read_ps4(b, b.items_of('VIS_INFO_1')[0])
    out = []
    out.append('regions %d/%d words %d/%d dict %s H %s G %s' % (
        len(A['regions']), len(B['regions']), A['nwords'], B['nwords'], A['dict'] == B['dict'], A['H'] == B['H'],
        [x[0] for x in A['G']] == [x[0] for x in B['G']] and [x[1] for x in A['G']] == [x[1] for x in B['G']]))
    out.append('H ptrs retail: %s' % (['null' if x is None else 'set' for x in B['Hptr']][:8]))
    out.append('bufs %s' % [A['bufs'][k][1] == B['bufs'][k][1] and A['bufs'][k][0] == B['bufs'][k][0] for k in range(4)])
    for i, (ra, rb) in enumerate(zip(A['regions'], B['regions'])):
        sa = [ra['streams'][n[3]] if n[3] else None for n in ra['nodes']]
        sb = [rb['streams'][n[3]] if n[3] else None for n in rb['nodes']]
        na = [n[:3] for n in ra['nodes']]; nb = [n[:3] for n in rb['nodes']]
        out.append('r%d: aabb %s planes %s faces %s nodes %s (%d/%d) streams %s x %s/%s' % (
            i, ra['aabb'] == rb['aabb'], ra['planes'] == rb['planes'], ra['faces'] == rb['faces'], na == nb,
            len(na), len(nb), sa == sb, ra['x'], rb['x']))
    # layout: offsets relative to the first object
    lay = B['layout']
    base = min((pg, off) for _, pg, off in lay)
    out.append('retail layout (sorted): ' + ' '.join('%s@%d:%x' % (n, pg, off) for n, pg, off in sorted(lay, key=lambda x: (x[1], x[2]))[:60]))
    return '\n'.join(out)


def layout_match(ps3pak, ps4pak):
    """byte-level check: rebuild from PS3, then compare every object's offset (relative to the item data)
    and bytes with retail, pointers compared by target offset"""
    from ndpak import Pak
    a, b = Pak(ps3pak), Pak(ps4pak)
    it = b.items_of('VIS_INFO_1')[0]
    blob = build(read(a, a.items_of('VIS_INFO_1')[0]))
    mine = bytearray(blob.b)
    for pos, t, off in blob.relocs:
        rt, ro = t.root()
        mine[pos:pos + 8] = struct.pack('<Q', ro + off)
    base = b.pbase(it.page) + it.data_off
    ref = bytearray(b.d[base:base + len(mine)])
    bad = 0
    for o in range(0, len(ref) - 7, 8):
        if b.is_ptr(it.page, it.data_off + o):
            dp, do = b.resolve(it.page, it.data_off + o)
            if dp != it.page:
                bad += 1; continue
            ref[o:o + 8] = struct.pack('<Q', do - it.data_off)
    diffs = [i for i in range(len(mine)) if mine[i] != ref[i]]
    return 'mine %d bytes, retail item %d, %d differing bytes, %d cross-page ptrs, first %s' % (
        len(mine), it.size, len(diffs), bad, [hex(x) for x in diffs[:8]])
