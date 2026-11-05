#!/usr/bin/env python3
"""Fort co-op voice fix: add the 84 vox entries PS4 vox-mp.bin lacks, converted from PS3 1.19.

Dry run (default): builds coop-vo/build/vox-mp.bin, verifies it, touches nothing else.
--apply: backs up any overlay vox-mp.bin to vox-mp.bin.bak-coopvo, then installs the build.
Run with: python -I apply_vo.py [--apply]
"""
import os, re, shutil, struct, sys, glob

PAK = "{{TRANSFER_TOOLS}}"
sys.path.insert(0, PAK)
from psarc_lib import Psarc

HERE = os.path.dirname(os.path.abspath(__file__))
RET = "{{PS4}}/u3data/build/main/dc1/ps4/"
WORK = "{{PROJECT}}/work/"
OVL = WORK + "overlay-r3/u3data/build/main/"
PS3DLC = "{{PS3_UPDATE}}/dlc-patch18.psarc"
GOTY = "{{PS3_DISC}}/goty-speech-english.psarc"
GOTY = "{{PS3_DISC}}/goty-speech-english.psarc"
VOX = 0xe3825e8d


def read_dc(d, be):
    e = '>' if be else '<'
    ps = 4 if be else 8
    reloc = struct.unpack_from(e + ('I' if be else 'Q'), d, 8)[0]
    n = struct.unpack_from(e + 'I', d, 0x14)[0]
    syms, o = [], 0x18
    for _ in range(n):
        if be:
            s, t, off = struct.unpack_from('>III', d, o); o += 12
        else:
            s, t = struct.unpack_from('<II', d, o); off = struct.unpack_from('<Q', d, o + 8)[0]; o += 16
        syms.append((s, t, off))
    cnt = struct.unpack_from(e + 'I', d, reloc)[0]
    ptrs = set()
    for bi, b in enumerate(d[reloc + 4:reloc + 4 + cnt]):
        for k in range(8):
            if b >> k & 1:
                ptrs.add((bi * 8 + (7 - k if be else k)) * ps)
    return reloc, syms, ptrs


def cstr(d, o):
    return d[o:d.index(b'\0', o)].decode('latin1')


def conv_entry(a, ap, o):
    """PS3 big-endian vox entry at o -> dict of PS4 values; raises on anything unexpected."""
    u = lambda p: struct.unpack_from('>I', a, p)[0]
    h = u(o)
    b4 = a[o + 4:o + 8]
    if u(o + 8) or u(o + 0xc) or u(o + 0x10) or u(o + 0x18):
        raise ValueError('entry %08x has fields this converter does not handle' % h)
    flags = u(o + 0x14)
    tag = u(o + 0x1c)
    meta = a[o + 0x20:o + 0x24]
    nblocks = meta[3]
    if not nblocks or o + 0x24 not in ap:
        raise ValueError('entry %08x: no blocks' % h)
    blocks = []
    bp = u(o + 0x24)
    for i in range(nblocks):
        q = bp + i * 0x18
        sid_, zero, bf, count, lp = (u(q), u(q + 4), a[q + 8:q + 0xc], u(q + 0xc), u(q + 0x10))
        if zero or (q + 0x10) not in ap:
            raise ValueError('entry %08x: block %d layout' % (h, i))
        lines = []
        for k in range(count):
            l = lp + k * 0x18
            if l not in ap or u(l + 4) or u(l + 8) or u(l + 0xc):
                raise ValueError('entry %08x: line %d layout' % (h, k))
            lines.append((cstr(a, u(l)), u(l + 0x10), a[l + 0x14:l + 0x18]))
        blocks.append((sid_, bf, lines))
    return h, b4, flags, tag, meta, blocks


def swap16(b):
    return bytes([b[1], b[0]])


def build(c, a):
    creloc, csyms, cptrs = read_dc(c, False)
    areloc, asyms, aptrs = read_dc(a, True)
    have = {s for s, _, _ in csyms}
    add = [(s, t, o) for s, t, o in asyms if s not in have]
    bad = [hex(t) for _, t, _ in add if t != VOX]
    if bad:
        raise SystemExit('non-vox symbols to add: %r' % bad)
    entries = [conv_entry(a, aptrs, o) for _, _, o in add]

    shift = len(add) * 16
    if shift % 16:
        raise SystemExit('shift alignment')
    tbl_old_end = 0x18 + len(csyms) * 16
    body = bytearray(c[tbl_old_end:creloc])
    new_ptrs = set()
    for p in cptrs:
        if p >= tbl_old_end:
            v = struct.unpack_from('<Q', c, p)[0]
            struct.pack_into('<Q', body, p - tbl_old_end, v + shift)
            new_ptrs.add(p + shift)
    base = tbl_old_end + shift
    out_syms = [(s, t, o + shift) for s, t, o in csyms]

    def here():
        return base + len(body)

    def pad(n):
        while here() % n:
            body.append(0)

    strs = {}
    for e in entries:
        for blk in e[5]:
            for ln in blk[2]:
                strs.setdefault(ln[0], None)
    for s in strs:
        strs[s] = here()
        body.extend(s.encode('latin1') + b'\0')

    def putq(pos, val, isptr=True):
        struct.pack_into('<Q', body, pos - base, val)
        if isptr:
            new_ptrs.add(pos)

    for e, (s, t, _) in zip(entries, add):
        h, b4, flags, tag, meta, blocks = e
        pad(16)
        body.extend(b'\0' * 12 + struct.pack('<I', VOX))  # 16 byte aligned object with type just before it
        off = here()
        body.extend(bytes(0x28))
        struct.pack_into('<I', body, off - base, h)
        body[off - base + 4:off - base + 8] = swap16(b4[:2]) + b4[2:4]
        struct.pack_into('<I', body, off - base + 0x18, flags)
        struct.pack_into('<I', body, off - base + 0x20, tag)
        body[off - base + 0x24:off - base + 0x28] = swap16(meta[:2]) + meta[2:4]
        blk_at = here() + 8
        body.extend(bytes(8))
        putq(off + 0x28, blk_at)
        body.extend(bytes(0x18 * len(blocks)))
        for i, (bs, bf, lines) in enumerate(blocks):
            q = blk_at + i * 0x18
            struct.pack_into('<I', body, q - base, bs)
            body[q - base + 8:q - base + 0xc] = swap16(bf[:2])[::1] + bf[2:4] if False else bytes([bf[1], bf[0]]) + bf[2:4]
            struct.pack_into('<I', body, q - base + 0xc, len(lines))
            la = here()
            body.extend(bytes(0x20 * len(lines)))
            putq(q + 0x10, la)
            for k, (nm, ls, lf) in enumerate(lines):
                l = la + k * 0x20
                putq(l, strs[nm])
                struct.pack_into('<I', body, l + 0x18 - base, ls)
                body[l + 0x1c - base:l + 0x20 - base] = swap16(lf[:2]) + lf[2:4]
        out_syms.append((s, t, off))

    out_syms.sort(key=lambda x: x[0])
    while here() % 16:
        body.append(0)
    reloc = here()
    hdr = bytearray(c[:0x18])
    struct.pack_into('<Q', hdr, 8, reloc)
    struct.pack_into('<I', hdr, 0x14, len(out_syms))
    tbl = bytearray()
    for s, t, o in out_syms:
        tbl += struct.pack('<IIQ', s, t, o)
    # symbol table slots are pointer slots in the retail bitmap too, when it marks them
    if 0x20 in cptrs:
        new_ptrs |= {0x18 + i * 16 + 8 for i in range(len(out_syms))}
    nbytes = (reloc + 63) // 64
    bm = bytearray(nbytes)
    for p in new_ptrs:
        w = p // 8
        bm[w // 8] |= 1 << (w % 8)
    res = bytes(hdr + tbl + body) + struct.pack('<I', nbytes) + bytes(bm)
    return res, add, entries


def verify(res, retail, entries):
    reloc, syms, ptrs = read_dc(res, False)
    by = {s: o for s, _, o in syms}
    assert [s for s, _, _ in syms] == sorted(s for s, _, _ in syms), 'symbols not sorted'
    # old entries: same relative content and strings
    r0, s0, p0 = read_dc(retail, False)
    bad = 0
    for s, t, o in s0:
        n = by[s]
        if retail[o:o + 8] != res[n:n + 8]:
            bad += 1
    assert not bad, 'old entries changed: %d' % bad
    lines = []
    for e in entries:
        o = by[e[0]]
        assert struct.unpack_from('<I', res, o - 4)[0] == VOX
        blk = struct.unpack_from('<Q', res, o + 0x28)[0]
        for i, b in enumerate(e[5]):
            q = blk + i * 0x18
            assert struct.unpack_from('<I', res, q)[0] == b[0]
            la = struct.unpack_from('<Q', res, q + 0x10)[0]
            for k, ln in enumerate(b[2]):
                nm = cstr(res, struct.unpack_from('<Q', res, la + k * 0x20)[0])
                assert nm == ln[0]
                lines.append(nm)
    return by, lines


def main():
    apply_ = '--apply' in sys.argv[1:]
    c = open(RET + 'vox-mp.bin', 'rb').read()
    p = Psarc(PS3DLC)
    a = None
    for i, n in enumerate(p.names):
        if n.endswith('dc1/vox-mp.bin'):
            a = p.read(i + 1)
    if a is None:
        raise SystemExit('PS3 vox-mp.bin not found')
    res, add, entries = build(c, a)
    by, lines = verify(res, c, entries)
    ov = OVL + 'sound1/streams/speech/english/'
    missing = [l for l in lines if not os.path.exists(ov + l + '.xvag')]
    in_ps4 = {os.path.basename(n)[:-5] for n in
              Psarc(RET.replace('dc1/ps4/', 'sound1/streams/speech/english_ps4.psarc')).names}
    goty = Psarc(GOTY)
    gidx = {os.path.basename(n)[:-5]: i for i, n in enumerate(goty.names)}
    copy = [l for l in missing if l not in in_ps4]
    unavailable = [l for l in copy if l not in gidx]
    print('added vox entries:', len(add), 'lines:', len(lines), 'size', len(c), '->', len(res))
    print('xvags to copy from goty-speech-english:', len(copy), 'not obtainable:', unavailable)
    if unavailable:
        raise SystemExit('stopping: a line has no source file')
    for l in copy:
        b = goty.read(gidx[l] + 1)
        if b[:4] != b'XVAG':
            raise SystemExit('not XVAG: ' + l)
        if apply_:
            open(ov + l + '.xvag', 'wb').write(b)
    out = os.path.join(HERE, 'build')
    os.makedirs(out, exist_ok=True)
    dst = os.path.join(out, 'vox-mp.bin')
    open(dst, 'wb').write(res)
    print('built', dst)
    target = OVL + 'dc1/ps4/vox-mp.bin'
    if not apply_:
        print('DRY RUN: would install to', target)
        return
    if os.path.exists(target):
        shutil.copy2(target, target + '.bak-coopvo')
    shutil.copy2(dst, target)
    print('installed', target)


if __name__ == '__main__':
    main()
