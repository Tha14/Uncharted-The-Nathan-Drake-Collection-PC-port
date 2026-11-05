#!/usr/bin/env python3
"""PS3 -> PS4 animation package converter (Uncharted 3, pak 0x0a79).

PS3 clips are ICE-anim command-list clips (magic 0x43440107 "CD\\x01\\x07", big endian);
PS4 clips are the OrbisAnim port of the same thing (magic 0x41430104, little endian).
The conversion was derived from the 605 anim-proto-multiplayer pairs (ros3 PS3 disc pak vs
retail PS4) and reproduces every retail clip byte-for-byte (except 8 bytes of uninitialised
converter memory in the 32-byte tail pad).  See FINDINGS.md section 11.

Clip layout (both, header fields identical apart from endianness):
  +00 magic  +04 u32 hash  +08 u32 total size  +0c u16 numGroups, u16 ?  +10 u32 keyframe-bitmask
  offset (0 = none)  +14 u32 group table offset (0x30)  +18 u32 0  +1c f32 fps  +20 u16 flags
  (bit0 loop, 0x30 bitmask modes)  +22 u16 numFrames  +24 f32 1/(n-1)  +28 f32 last frame  +2c f32 1/fps
  group (16 B): u32 block offset, u16 block size, u8 numJoints, u8 numFloats, u32 frame data offset,
                u16 frame stride (bytes), u16 ?
  PS3: block = [masks (16 B per non-zero b6/b7)] [command list] [const data]
  PS4: masks are collected into one table right after the group table; block = [cmds][const data].
  Commands: u16 (argc << 8 | opcode) + argc u16 args.  "Locations" are (offset | base index):
  base 0 = command list start, base 3 = evaluation context (PS3 encodes it as 0x000c, PS4 0x0003).
  Each opcode's const-data region is re-emitted with the PS4 length rule and element swap below,
  reading the PS3 block as a buffer that is 0xff beyond its end (that is what the PS4 converter did).
  Frame data (MSB-first bit streams) is copied verbatim.

usage: anim.py <ps3.pak> <out_ps4.pak> [--stm <dir of PS3 igc-stream-*.stm>] [--check]
Streamed anims (STREAMING_BUFFER) and clip op 0x17: FINDINGS.md section 11.
"""
import argparse, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid
from ps4writer import PakWriter, Blob, make_login_and_vram_page

PAD32 = 0xaaaaaaaa
PAGE_LIMIT = 0x1b0000
PS3_MAGIC, PS4_MAGIC = 0x43440107, 0x41430104


# ------------------------------------------------------------------ clip
def _swap(b, k):
    if k == 1:
        return bytes(b)
    n = len(b) - len(b) % k
    return b''.join(b[i:i + k][::-1] for i in range(0, n, k)) + bytes(b[n:])


def _a16(x):
    return (x + 15) & ~15


def clip_header(c, ps3):
    e = '>' if ps3 else '<'
    h = dict(zip(('magic', 'hash', 'size', 'ngroups', 'u0e', 'off10', 'grpoff', 'off18', 'fps', 'flags',
                  'nframes', 'f24', 'f28', 'f2c'), struct.unpack_from(e + 'IIIHHIIIfHHfff', c, 0)))
    gs = []
    for i in range(h['ngroups']):
        cmd, bsz, b6, b7, fdo, stride, ue = struct.unpack_from(e + 'IHBBIHH', c, h['grpoff'] + 16 * i)
        gs.append(dict(cmd=cmd, bsz=bsz, b6=b6, b7=b7, fdo=fdo, stride=stride, ue=ue))
    h['groups'] = gs
    return h


def nmasks(g):
    return (1 if g['b6'] else 0) + (1 if g['b7'] else 0)


def parse_cmds(c, ps3, off, end=None):
    e = '>' if ps3 else '<'
    out = []
    while True:
        if end is not None and off + 2 > end:
            break
        w = struct.unpack_from(e + 'H', c, off)[0]
        if w & 0xff == 0:
            break
        n = w >> 8
        out.append((off, w & 0xff, list(struct.unpack_from(e + '%dH' % n, c, off + 2))))
        off += 2 + 2 * n
    return out


def clip_groups(c, ps3):
    h = clip_header(c, ps3)
    out = []
    for g in h['groups']:
        b0 = g['cmd'] + (16 * nmasks(g) if ps3 else 0)
        end = g['cmd'] + g['bsz']
        out.append((g, b0, parse_cmds(c, ps3, b0, len(c)) if b0 < end else [], end))
    return h, out


def _need(op, a):
    """PS4 byte length of the const-data region an opcode references (arg 2)."""
    n = a[0]
    m = n & 0x7ff
    if op == 0x0b: return m if n >> 15 else 1                 # u8 bit widths (per item if bit15)
    if op == 0x0c: return 4 * (n & 0x1fff) if n >> 15 else 1  # u8 bit widths, 4 comps
    if op == 0x0f: return 2 * ((m + 7) & ~7)                  # u16 dest index table (lerp vec)
    if op == 0x10: return 2 * ((m + 15) & ~15)                # u16 dest index table (slerp quat)
    if op == 0x11: return 2 * (((n & 0x1fff) + 3) & ~3)       # u16 dest index table (float lerp)
    if op == 0x15: return 14 * n                              # u16 idx[n] + f32 vec3[n]
    if op == 0x17: return 18 * n                              # u16 idx[n] + f32 quat[n]
    if op in (0x16, 0x18): return 8 * n                       # u16 idx[n] + half3 / packed quat48
    if op == 0x19: return 6 * n                               # u16 idx[n] + f32[n]
    if op == 0x1b: return 16 * m + (4 * m if n >> 15 else 4)  # 2x u64 range + u32 format
    if op == 0x1c: return 4 * m if n >> 15 else 4             # u32 format words
    if op == 0x1d: return 8 * m                               # s16 x4
    if op == 0x1e: return 16 * m                              # s16 x8
    raise KeyError('unsupported clip opcode %#x' % op)


def _xform(op, a, b):
    n = a[0]
    if op in (0x0b, 0x0c, 0x1c):
        return bytes(b)
    if op in (0x0f, 0x10, 0x11, 0x16, 0x1d, 0x1e):
        return _swap(b, 2)
    if op == 0x18:
        return _swap(b[:2 * n], 2) + bytes(b[2 * n:])
    if op == 0x19:
        return _swap(b[:2 * n], 2) + _swap(b[2 * n:], 4)
    if op in (0x15, 0x17):
        return _swap(b[:2 * n], 2) + _swap(b[2 * n:], 4)
    if op == 0x1b:
        m = n & 0x7ff
        return _swap(b[:16 * m], 8) + bytes(b[16 * m:])
    raise KeyError(op)


def _convert_block(c3, b3, l3, e3, ext):
    if not l3:
        return b'\0\0'
    cend = l3[-1][0] + 2 + 2 * len(l3[-1][2]) + 2
    refs = {}
    for o, op, a in l3:
        if op == 0x1a:          # arg 2 unused on PS4 (zeroed by the retail converter)
            continue
        if len(a) < 3:
            raise KeyError('opcode %#x with %d args' % (op, len(a)))
        refs.setdefault(a[2], []).append((op, a))
    keys = sorted(refs)
    first = keys[0] if keys else _a16(cend - b3)
    newoff = {}
    out = bytearray(first)
    for i, k in enumerate(keys):
        last = i + 1 == len(keys)
        L3 = (keys[i + 1] if not last else ext - b3) - k
        op, a = max(refs[k], key=lambda x: _need(*x))
        nd = _need(op, a)
        L4 = _a16(max(L3, nd)) if not last else nd
        src = c3[b3 + k:min(b3 + k + L4, ext)]
        if len(src) < L4:
            src = src + b'\xff' * (L4 - len(src))
        newoff[k] = len(out)
        out += _xform(op, a, src)
    cmd = bytearray()
    for o, op, a in l3:
        a = list(a)
        if a[1] == 0x000c:
            a[1] = 0x0003
        elif (a[1] & 0xf) or op not in (0x0b, 0x0c, 0x0f, 0x10, 0x11, 0x15, 0x16, 0x17, 0x18, 0x19,
                                         0x1a, 0x1b, 0x1c, 0x1d, 0x1e):
            raise KeyError('unexpected context location %#x for op %#x' % (a[1], op))
        a[2] = 0 if op == 0x1a else newoff[a[2]]
        cmd += struct.pack('<%dH' % (1 + len(a)), op | (len(a) << 8), *a)
    cmd += b'\0\0'
    assert len(cmd) <= first
    out[:len(cmd)] = cmd
    out[len(cmd):first] = _swap(c3[b3 + len(cmd):b3 + first], 2)
    return bytes(out)


def is_streamed(c3):
    return struct.unpack_from('>I', c3, 0x14)[0] == 0


def convert_clip(c3):
    """PS3 clip bytes -> PS4 clip bytes."""
    if struct.unpack_from('>I', c3, 0)[0] != PS3_MAGIC:
        raise ValueError('not a PS3 clip')
    if is_streamed(c3):
        # header-only clip of a streamed animation (the frames live in the -chunk-N items):
        # retail keeps the PS3 magic and only swaps the header fields.
        if struct.unpack_from('>I', c3, 8)[0] != 0x30:
            raise ValueError('streamed clip with a body')
        return struct.pack('<IIIHHIIIfHHfff', *struct.unpack_from('>IIIHHIIIfHHfff', c3, 0))
    h3, G3 = clip_groups(c3, True)
    gs = h3['groups']
    ng = len(gs)
    fd0 = min(g['fdo'] for g in gs)
    blocks = []
    for gi, (g3, b3, l3, e3) in enumerate(G3):
        ext = gs[gi + 1]['cmd'] if gi + 1 < ng else fd0
        blocks.append(_convert_block(c3, b3, l3, e3, ext))
    nm = sum(nmasks(g) for g in gs)
    out = bytearray(0x30 + 16 * ng + 16 * nm)
    cmdoffs = []
    for b in blocks:
        off = _a16(len(out))
        out += bytes(off - len(out))
        cmdoffs.append(off)
        out += b
    fd4 = _a16(len(out))
    out += bytes(fd4 - len(out))
    if h3['off10']:
        if h3['off10'] < fd0:
            raise ValueError('keyframe bitmask table before frame data - unsupported')
    out += c3[fd0:]
    out += bytes(16 * ng)          # retail converter pads 16 bytes per group
    hw = struct.unpack_from('>IIIHHIIIfHHfff', c3, 0)
    struct.pack_into('<IIIHHIIIfHHfff', out, 0, PS4_MAGIC, hw[1], len(out), hw[3], hw[4],
                     hw[5] + (fd4 - fd0 if hw[5] else 0), hw[6], hw[7], hw[8], hw[9], hw[10], hw[11],
                     hw[12], hw[13])
    mo = 0x30 + 16 * ng
    for gi, g in enumerate(gs):
        fdo = (g['fdo'] - fd0 + fd4) if (g['fdo'] < len(c3) or fd0 < len(c3)) else len(out)
        struct.pack_into('<IHBBIHH', out, 0x30 + gi * 16, cmdoffs[gi], len(blocks[gi]), g['b6'], g['b7'],
                         fdo, g['stride'], g['ue'])
        for k in range(nmasks(g)):
            a, b = struct.unpack_from('>QQ', c3, g['cmd'] + 16 * k)
            struct.pack_into('<QQ', out, mo, a, b)
            mo += 16
    return bytes(out)


# ------------------------------------------------------------------ ANIM item
def _item_targets(p, it):
    lo, hi = it.data_off, it.data_off + it.size
    t = set()
    for (pg, o), tp in p.ptrmap.items():
        if pg == it.page and lo <= o < hi and tp == it.page:
            v = p.pptr(pg, o)
            if lo <= v <= hi:
                t.add(v)
    return sorted(t)


def _containing_item(p, pg, off):
    for x in p.items:
        if x.page == pg and x.data_off <= off < x.data_off + x.size:
            return x
    return None


def read_ps3_anim(p, it):
    """Parse a PS3 ANIM item into a plain dict."""
    pg, b = it.page, it.data_off
    w = struct.unpack_from('>7I', p.d, p.pbase(pg) + b)
    a = dict(name=it.name, w0=w[0], w1=w[1], w4=w[4], skel=w[5])
    if w[0] != 6 or w[4] != 0:
        raise ValueError('%s: unexpected ANIM header %r' % (it.name, w))
    # clip
    r = p.resolve(pg, b + 8)
    a['clip'] = None
    if r:
        cb = p.pbase(r[0]) + r[1]
        size = struct.unpack_from('>I', p.d, cb + 8)[0]
        a['clip'] = p.d[cb:cb + size]
    # float channels (ANIM_RAWCHANNELS item)
    a['chans'] = None
    r = p.resolve(pg, b + 12)
    if r:
        cpg, co = r
        ci = _containing_item(p, cpg, co)
        T = _item_targets(p, ci) + [ci.data_off + ci.size]
        n = p.pu32(cpg, co)
        chans = []
        if n:
            sp = p.resolve(cpg, co + 4)[1]
            pp = p.resolve(cpg, co + 8)[1]
            for i in range(n):
                s = p.pu32(cpg, sp + 4 * i)
                hh = p.resolve(cpg, pp + 4 * i)[1]
                nfr, typ = struct.unpack_from('>HH', p.d, p.pbase(cpg) + hh)
                dr = p.resolve(cpg, hh + 4)
                if dr is None:
                    chans.append((s, nfr, typ, None))
                    continue
                d = dr[1]
                nxt = [t for t in T if t > d][0]
                chans.append((s, nfr, typ, p.d[p.pbase(cpg) + d:p.pbase(cpg) + nxt]))
        a['chans'] = chans
    # events
    a['events'] = None
    r = p.resolve(pg, b + 24)
    if r:
        epg, eo = r
        tsid, cnt = p.pu32(epg, eo), p.pu32(epg, eo + 4)
        ents = []
        er = p.resolve(epg, eo + 8)
        for i in range(cnt if er else 0):
            e = er[1] + 16 * i
            s, f, c = struct.unpack_from('>IfI', p.d, p.pbase(epg) + e)
            kvr = p.resolve(epg, e + 12)
            kv = []
            if kvr:
                for j in range(c):
                    if p.is_ptr(epg, kvr[1] + 8 * j) or p.is_ptr(epg, kvr[1] + 8 * j + 4):
                        raise ValueError('%s: pointer inside event key/value list' % it.name)
                    kv.append(struct.unpack_from('>II', p.d, p.pbase(epg) + kvr[1] + 8 * j))
            ents.append((s, f, c, kv if kvr else None))
        a['events'] = (tsid, cnt, ents)
    return a


def build_ps4_anim(a):
    """Blob with the PS4 ANIM item body (header, channel table, events, clip)."""
    B = Blob('anim:' + a['name'])
    clip4 = convert_clip(a['clip']) if a['clip'] is not None else None
    hdr = B.pos
    B.u32(a['w0']).u32(a['w1']).u64(0).u64(0).u64(0).u32(a['skel']).u32(PAD32).u64(0)
    fix = []          # (field pos, target pos) resolved after layout
    # channels
    if a['chans'] is not None:
        B.pad(16, 0xaa)
        ct = B.pos
        fix.append((hdr + 0x10, ct))
        n = len(a['chans'])
        B.u32(n).u32(PAD32).u64(0).u64(0).u32(PAD32).u32(PAD32)
        if n:
            B.pad(16, 0xaa)
            fix.append((ct + 8, B.pos))
            for s, _, _, _ in a['chans']:
                B.u32(s)
            B.pad(16, 0xaa)
            pp = B.pos
            fix.append((ct + 0x10, pp))
            B.raw(b'\0' * (8 * n))
            for i, (s, nfr, typ, d) in enumerate(a['chans']):
                B.pad(16, 0xaa)
                fix.append((pp + 8 * i, B.pos))
                ch = B.pos
                B.u16(nfr).u16(typ).u32(PAD32).u64(0)
                if d is not None:
                    B.pad(16, 0xaa)
                    fix.append((ch + 8, B.pos))
                    B.raw(_swap(d, 2))
    # events
    if a['events'] is not None:
        tsid, cnt, ents = a['events']
        B.pad(16, 0xaa)
        et = B.pos
        fix.append((hdr + 0x28, et))
        B.u32(tsid).u32(cnt).u64(0)
        if ents:
            B.pad(16, 0xaa)
            ep = B.pos
            fix.append((et + 8, ep))
            for s, f, c, kv in ents:
                B.u32(s).raw(struct.pack('<f', f)).u32(c).u32(PAD32).u64(0)
            for i, (s, f, c, kv) in enumerate(ents):
                if kv is None:      # (older anim.py: a pointer for every PS3 list, also empty ones)
                    continue
                B.pad(16, 0xaa)
                fix.append((ep + 24 * i + 16, B.pos))
                for k, v in kv:
                    B.u32(k).u32(v)
    if clip4 is not None:
        B.pad(16, 0xaa)
        fix.append((hdr + 8, B.pos))
        B.raw(clip4)
    for pos, tgt in fix:
        B.relocs.append((pos, B, tgt))
    return B


# ------------------------------------------------------------------ streaming
def read_stm(path):
    """PS3 igc-stream-*.stm: a sequence of blocks {16-byte tag, mini PS3 pak with one page whose
    login table lists one ANIM '<anim>-chunk-N'}.  Blocks are chunk-major (chunk 1 of every
    stream of the file, then chunk 2, ...).  Returns [(item name, anim dict)] in file order."""
    d = open(path, 'rb').read()
    out = []
    o = 0
    while o < len(d):
        if len(d) - o < 0x40:
            if d[o:].strip(b'\0'):
                raise ValueError('%s: trailing data at %#x' % (path, o))
            break
        p = Pak(d[o + 16:], platform='ps3')
        size = p.pages[-1][0] + p.pages[-1][1]
        login = [p.item_at(a, b) for a, b in p.login['entries']]
        if len(login) != 1 or login[0].type != 'ANIM':
            raise ValueError('%s: block at %#x is not a single ANIM' % (path, o))
        a = read_ps3_anim(Pak(d[o + 16:o + 16 + size], platform='ps3'), login[0])
        out.append((login[0].name, a))
        o += 16 + size
    return out


def read_ps3_sb(p, it):
    """Parse a PS3 STREAMING_BUFFER (stream set) into a dict.
    PS3 body: {u32* listCount[nLists], u32** listSids, u32** listSlot, u32 fps(30),
    u32* fileChunks[nFiles], u32 bufferSize, u32*** chunkSizes [list][anim] -> u32[chunks],
    char** fileNames, u32* fileStreams[nFiles], Runtime*}; Runtime = {u32 nLists, u32* a, 0,
    u32* b, 0 x4, Rec* recs (3 x {u32 id, 0, 0, ptr buffer, ptr u32[nLists]}), 0 x4}.
    The two buffers (rec 1/2) are bufferSize each and end the item."""
    pg, b = it.page, it.data_off
    R = lambda o: p.resolve(pg, o)
    U = lambda o: p.pu32(pg, o)
    rt = R(b + 0x24)[1]
    nl = U(rt)
    names_at = R(b + 0x1c)[1]
    first_str = R(names_at)[1]
    nf = (first_str - names_at) // 4
    names = [p.pstr(pg, R(names_at + 4 * i)[1]) for i in range(nf)]
    A = R(b)[1]
    counts = [U(A + 4 * i) for i in range(nl)]
    sids = [[U(R(R(b + 4)[1] + 4 * i)[1] + 4 * j) for j in range(counts[i])] for i in range(nl)]
    slots = [[U(R(R(b + 8)[1] + 4 * i)[1] + 4 * j) for j in range(counts[i])] for i in range(nl)]
    nch = [U(R(b + 0x10)[1] + 4 * f) for f in range(nf)]
    nstr = [U(R(b + 0x20)[1] + 4 * f) for f in range(nf)]
    D = R(b + 0x18)[1]
    tables = [[R(R(D + 4 * i)[1] + 4 * j)[1] for j in range(counts[i])] for i in range(nl)]
    recs = R(rt + 0x20)[1]
    bufs = [R(recs + 0x14 * k + 0xc) for k in range(3)]
    if bufs[0] is not None or bufs[1] is None or bufs[2] is None:
        raise ValueError('%s: unexpected stream buffer records' % it.name)
    sb = dict(name=it.name, fps=U(b + 0xc), bufsize=U(b + 0x14), names=names, nch=nch, nstr=nstr,
              counts=counts, sids=sids, slots=slots, tables_at=tables, size=it.size)
    if bufs[2][1] + sb['bufsize'] != b + it.size or bufs[1][1] + sb['bufsize'] != bufs[2][1]:
        raise ValueError('%s: stream buffers do not end the item' % it.name)
    if sum(counts) != sum(nstr):
        raise ValueError('%s: list/file stream counts differ' % it.name)
    return sb


def stm_streams(sb, stm, nch):
    """Split one stm file's blocks into rows: [chunk 1..nch] -> [anim names in file order]."""
    n = len(stm) // nch if nch else 0
    if n * nch != len(stm):
        raise ValueError('stm block count %d is not a multiple of %d chunks' % (len(stm), nch))
    rows = []
    for k in range(nch):
        row = stm[k * n:(k + 1) * n]
        for nm, _ in row:
            if not nm.endswith('-chunk-%d' % (k + 1)):
                raise ValueError('stm block %s out of order (chunk %d)' % (nm, k + 1))
        rows.append([nm for nm, _ in row])
    return rows


def build_ps4_sb(p, it, sb, file_rows, chunk_items):
    """Blob with the PS4 STREAMING_BUFFER body (layout copied from retail loading-menu-cloth and
    anim-coop-syria): every object 16-aligned, aa padding, natural 8-byte alignment of pointer
    fields.  PS4 adds a chunk table (Runtime+0x48): ptr -> ptr[nFiles] -> ptr[nChunks+1] ->
    row of item-header pointers (row 0 = chunk 0, which is resident: all null)."""
    pg = it.page
    B = Blob('sb:' + sb['name'])
    nl, nf = len(sb['counts']), len(sb['names'])
    fix = []

    def al():
        B.pad(16, 0xaa)

    def u32s(vals):
        al()
        o = B.pos
        for v in vals:
            B.u32(v)
        return o

    def ptrs(n):
        al()
        o = B.pos
        B.raw(b'\0' * (8 * n))
        return o

    hdr = B.pos
    B.raw(b'\0' * 0x18).u32(sb['fps']).u32(PAD32).u64(0).u32(sb['bufsize']).u32(PAD32)
    B.raw(b'\0' * 0x20)
    o = u32s(sb['counts']); fix.append((hdr + 0, o))
    pb = ptrs(nl); fix.append((hdr + 8, pb))
    pc = ptrs(nl); fix.append((hdr + 0x10, pc))
    for i in range(nl):
        o = u32s(sb['sids'][i]); fix.append((pb + 8 * i, o))
        o = u32s(sb['slots'][i]); fix.append((pc + 8 * i, o))
    o = u32s(sb['nch']); fix.append((hdr + 0x20, o))
    o = u32s(sb['nstr']); fix.append((hdr + 0x40, o))
    pn = ptrs(nf); fix.append((hdr + 0x38, pn))
    for f, s in enumerate(sb['names']):
        al()
        fix.append((pn + 8 * f, B.pos))
        B.cstr(s)
    pd = ptrs(nl); fix.append((hdr + 0x30, pd))
    for i in range(nl):
        pl = ptrs(sb['counts'][i]); fix.append((pd + 8 * i, pl))
        for j, toff in enumerate(sb['tables_at'][i]):
            n = sb['table_len'][i][j]
            o = u32s([p.pu32(pg, toff + 4 * k) for k in range(n)])
            fix.append((pl + 8 * j, o))
    al()
    rt = B.pos; fix.append((hdr + 0x48, rt))
    B.u32(nl).u32(PAD32).u64(0).u32(0).u32(PAD32).u64(0).u32(0).u32(PAD32).u64(0).u64(0)
    B.u32(0).u32(PAD32).u64(0).u64(0).u64(0).u32(0).u32(PAD32)
    o = u32s([0] * nl); fix.append((rt + 8, o))
    o = u32s([0] * nl); fix.append((rt + 0x18, o))
    al()
    recs = B.pos; fix.append((rt + 0x40, recs))
    for k in range(3):
        B.u32(k).u32(0).u32(0).u32(PAD32).u64(0).u64(0)
    al()
    ct = B.pos; fix.append((rt + 0x48, ct))
    B.u64(0)
    pf = ptrs(nf); fix.append((ct, pf))
    for f in range(nf):
        rows = file_rows[f]
        pr = ptrs(len(rows) + 1); fix.append((pf + 8 * f, pr))
        o = ptrs(sb['nstr'][f]); fix.append((pr, o))     # chunk 0 is resident: null row
        for k, row in enumerate(rows):
            o = ptrs(len(row)); fix.append((pr + 8 * (k + 1), o))
            for j, nm in enumerate(row):
                B.relocs.append((o + 8 * j, chunk_items[nm].data, -0x20))
    o = ptrs(nl); fix.append((recs + 0x18, o))
    for k in (1, 2):
        al()
        fix.append((recs + 0x20 * k + 0x10, B.pos))
        B.raw(b'\0' * sb['bufsize'])
    for pos, tgt in fix:
        B.relocs.append((pos, B, tgt))
    return B


# ------------------------------------------------------------------ package
class Pager:
    def __init__(self, w):
        self.w = w
        self.cur = w.page()
        self.size = 0x200

    def item(self, type_, data, name=''):
        est = _a16(len(data) + 0x40 + len(type_) + len(name)) + 16
        if self.size + est > PAGE_LIMIT and self.size > 0x200:
            self.cur = self.w.page()
            self.size = 0x200
        self.size += est
        return self.cur.item(type_, data, name=name)


def find_stm(name, stm_dirs):
    for d in stm_dirs:
        f = os.path.join(d, name + '.stm')
        if os.path.exists(f):
            return f
    raise FileNotFoundError('stream %s.stm not found in %s' % (name, ', '.join(stm_dirs) or '(no --stm dir)'))


def convert_pak(src, dst, verbose=True, stm_dirs=()):
    p = Pak(src)
    if p.platform != 'ps3':
        raise ValueError('source must be a PS3 pak')
    skip = {'ANIM_RAWCHANNELS', 'MATERIAL_TABLE_1', 'VRAM_DESC_TABLE', 'PAK_LOGIN_TABLE'}
    other = sorted({i.type for i in p.items} - skip - {'ANIM', 'ARTGROUP_DATA', 'LEVEL_PATS_1',
                                                        'PACKAGE_ITEM_LOOKUP', 'STREAMING_BUFFER'})
    if other:
        raise ValueError('unsupported item types in %s: %s' % (src, ', '.join(other)))
    w = PakWriter()
    pg = Pager(w)
    newitem = {}     # (page, off) of PS3 item header -> PS4 Item
    login3 = [p.item_at(a, b) for a, b in p.login['entries']]
    anims = [x for x in login3 if x.type == 'ANIM'] + \
            [x for x in p.items_of('ANIM') if (x.page, x.off) not in {(y.page, y.off) for y in login3}]
    for it in anims:
        a = read_ps3_anim(p, it)
        newitem[(it.page, it.off)] = pg.item('ANIM', build_ps4_anim(a), name=it.name)
    # streamed animations: chunks 1..N come from the .stm files and become ANIM items
    sbitem = None
    sbs = p.items_of('STREAMING_BUFFER')
    if len(sbs) > 1:
        raise ValueError('more than one STREAMING_BUFFER')
    nchunk = 0
    if sbs:
        sb3 = sbs[0]
        sb = read_ps3_sb(p, sb3)
        file_rows, chunk_items, file_of = [], {}, {}
        for f, nm in enumerate(sb['names']):
            stm = read_stm(find_stm(nm, stm_dirs))
            rows = stm_streams(sb, stm, sb['nch'][f])
            if rows and len(rows[0]) != sb['nstr'][f]:
                raise ValueError('%s: %d streams in stm, %d in STREAMING_BUFFER' % (nm, len(rows[0]), sb['nstr'][f]))
            for cn, a in stm:
                chunk_items[cn] = pg.item('ANIM', build_ps4_anim(a), name=cn)
            for x in (rows[0] if rows else []):
                file_of[sid(x[:-len('-chunk-1')])] = f
            file_rows.append(rows)
            nchunk += len(stm)
        sb['table_len'] = [[sb['nch'][file_of[s]] for s in l] for l in sb['sids']]
        sbitem = pg.item('STREAMING_BUFFER', build_ps4_sb(p, sb3, sb, file_rows, chunk_items), name=sb3.name)
    # artgroup
    ag3 = p.items_of('ARTGROUP_DATA')[0]
    gp, gb = ag3.page, ag3.data_off
    n = p.pu32(gp, gb)
    if p.pu32(gp, gb + 4) != 1:
        raise ValueError('artgroup +4 != 1')
    ents = p.resolve(gp, gb + 8)[1]
    targets = []
    for k in range(n):
        r = p.resolve(gp, ents + 8 * k)
        targets.append(newitem[r] if r else None)
    h2 = p.resolve(gp, gb + 0xc)[1]
    x = p.resolve(gp, h2 + 0xc)[1]
    nb = p.pu32(gp, x + 0x10)
    tbl = p.resolve(gp, x + 0x14)[1]
    buckets = []
    for i in range(nb):
        key = p.pu32(gp, tbl + 8 * i)
        r = p.resolve(gp, tbl + 8 * i + 4)
        buckets.append((key, None if r is None else (r[1] - ents) // 8))
    ab = Blob('artgroup')
    ab.u32(n).u32(1).ptr(ab, 0x50).ptr(ab, 0x30).u64(0).u64(0).u32(PAD32).u32(PAD32)
    tbl_off = 0x50 + 16 * n
    x_off = tbl_off + 16 * nb
    ab.u64(0).u32(1).u32(0x12345).ptr(ab, x_off).u32(PAD32).u32(PAD32)
    for t in targets:
        ab.ptr(t.data if t is not None else None, -0x20).u64(0)
    for key, ei in buckets:
        ab.u32(key).u32(PAD32)
        ab.ptr(ab, 0x50 + 16 * ei) if ei is not None else ab.u64(0)
    ab.u32(0).u32(PAD32).u64(0).u64(0).u32(0).u32(nb).ptr(ab, tbl_off)
    art = ag3.name
    artgroup = pg.item('ARTGROUP_DATA', ab, name=art)
    lp = Blob('levelpats')
    lp.u64(0)
    levelpats = pg.item('LEVEL_PATS_1', lp)
    pl = Blob('pkglookup')
    pl.ptr(pl, 0x20).u32(0).u32(1).ptr(pl, 0x20 + ((len(art) + 1 + 15) // 16) * 16).u32(PAD32).u32(PAD32)
    pl.cstr(art)
    pl.pad(16, 0xaa)
    pl.u32(sid(art)).u32(PAD32).ptr(artgroup.data, -0x20)
    lookup = pg.item('PACKAGE_ITEM_LOOKUP', pl)
    # login page
    lpage = w.page()
    order = []
    for x in login3:
        if x.type == 'ARTGROUP_DATA':
            order.append(artgroup)
        elif x.type == 'LEVEL_PATS_1':
            order.append(levelpats)
        elif x.type == 'ANIM':
            order.append(newitem[(x.page, x.off)])
        elif x.type == 'PACKAGE_ITEM_LOOKUP':
            order.append(lookup)
        elif x.type == 'STREAMING_BUFFER':
            order.append(sbitem)
    for extra in [artgroup, levelpats] + [newitem[(i.page, i.off)] for i in anims] + [lookup] + \
            ([sbitem] if sbitem else []):
        if extra not in order:
            order.append(extra)
    make_login_and_vram_page(w, lpage, order, [])
    data = w.build()
    open(dst, 'wb').write(data)
    if verbose:
        print('wrote %s: %d bytes, %d pages, %d anims, %d stream chunks' % (dst, len(data), len(w.pages),
                                                                             len(anims), nchunk))
    return data


def _check_anim(p4, j, a, errs):
    r = p4.resolve(j.page, j.data_off + 8)
    if a['clip'] is None:
        if r is not None:
            errs.append('%s: unexpected clip' % j.name)
        return
    cb = p4.pbase(r[0]) + r[1]
    sz = struct.unpack_from('<I', p4.d, cb + 8)[0]
    c4 = p4.d[cb:cb + sz]
    if not is_streamed(a['clip']):
        h, G = clip_groups(c4, False)
        for g, b0, l, e in G:
            for o, op, args in l:
                if op != 0x1a and args[2] + _need(op, args) > g['bsz'] + 2 and l:
                    errs.append('%s: op %#x region beyond block' % (j.name, op))
    if c4 != convert_clip(a['clip']):
        errs.append('%s: clip mismatch' % j.name)


def selfcheck(src, dst, stm_dirs=()):
    """Re-read the PS4 output and decode every clip back against the PS3 source (and the .stm
    stream chunks); check the STREAMING_BUFFER chunk table points at the right chunk items."""
    p3, p4 = Pak(src), Pak(dst)
    errs = list(p4.validate())
    a4 = {i.name: i for i in p4.items_of('ANIM')}
    for it in p3.items_of('ANIM'):
        j = a4.get(it.name)
        if j is None:
            errs.append('missing %s' % it.name)
            continue
        _check_anim(p4, j, read_ps3_anim(p3, it), errs)
    for sb3 in p3.items_of('STREAMING_BUFFER'):
        sb = read_ps3_sb(p3, sb3)
        sb4 = [x for x in p4.items_of('STREAMING_BUFFER') if x.name == sb3.name]
        if len(sb4) != 1:
            errs.append('STREAMING_BUFFER %s missing' % sb3.name)
            continue
        q, b = sb4[0].page, sb4[0].data_off
        R = lambda o: p4.resolve(q, o)
        rt = R(b + 0x48)[1]
        files = R(R(rt + 0x48)[1])[1]
        for f, nm in enumerate(sb['names']):
            stm = read_stm(find_stm(nm, stm_dirs))
            for cn, a in stm:
                j = a4.get(cn)
                if j is None:
                    errs.append('missing chunk %s' % cn)
                    continue
                _check_anim(p4, j, a, errs)
            rows = stm_streams(sb, stm, sb['nch'][f])
            ft = R(files + 8 * f)[1]
            for k in range(len(rows) + 1):
                row = R(ft + 8 * k)[1]
                for i in range(sb['nstr'][f]):
                    x = R(row + 8 * i)
                    got = p4.item_at(*x).name if x else None
                    want = rows[k - 1][i] if k else None
                    if got != want:
                        errs.append('%s chunk table [%d][%d][%d]: %s != %s' % (sb3.name, f, k, i, got, want))
        bufsz = p4.pu32(q, b + 0x28)
        recs = R(rt + 0x40)[1]
        b1, b2 = R(recs + 0x30), R(recs + 0x50)
        if b2[1] - b1[1] < bufsz or b2[1] + bufsz != b + sb4[0].size:
            errs.append('%s: stream buffers wrong' % sb3.name)
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--stm', action='append', default=[], help='directory with PS3 igc-stream-*.stm files')
    o = ap.parse_args()
    convert_pak(o.src, o.dst, stm_dirs=o.stm)
    if o.check:
        e = selfcheck(o.src, o.dst, o.stm)
        print('selfcheck:', 'OK' if not e else '; '.join(e[:10]))


if __name__ == '__main__':
    main()
