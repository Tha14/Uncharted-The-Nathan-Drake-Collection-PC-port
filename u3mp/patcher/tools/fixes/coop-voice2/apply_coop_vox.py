#!/usr/bin/env python3
"""Co-op adventure voice fix: copy every vox entry the co-op adventure scripts request from vox-mp.bin into the overlay's vox.bin.

Why: in a co-op adventure level the vox controller's SayRequest (PS4 0x4af5f0) rejects every scripted hero line with code 8
"no vox entry": the lookup 0x4a7350 only finds symbols of the modules vox (0x4eb9055f), vox-igc and vox-mp, and in the co-op
levels the vox-mp module is not resolvable (x028 run baseline-1793586362153677500: syr-coop-drk-* all code 8, while a vox.bin
entry was accepted in the same level). Every co-op line of every map lives in vox-mp.bin (coop_lines.pkl); vox.bin resolves.
So the entries are duplicated into vox.bin, which the vox lookup searches first.

Dry run (default): builds build/vox.bin, verifies it, prints the per-map coverage, touches nothing else.
--apply: backs up the overlay vox.bin to work/backup-coop-voice2/vox.bin (once), installs the build.
--restore: puts the backup back.
Run with: python -I apply_coop_vox.py [--apply | --restore]
"""
import os, re, shutil, struct, sys, glob, collections
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, '{{PAKTOOLS}}')
from dc import DC
P = "{{PROJECT}}/"
RET = "{{PS4}}/u3data/build/main/dc1/ps4/"
OVL = P + "work/overlay-r3/u3data/build/main/dc1/ps4/"
BAK = P + "work/backup-coop-voice2/"
VOX = 0xe3825e8d
SAY = {0x235cfa87: 'say', 0x95f6b8dd: 'wait-say', 0x159dcdd2: 'say-pre-roll'}
NETGAME = 0x3b9a067d


def served(name):
    return OVL + name if os.path.exists(OVL + name) else RET + name


def read_dc(d):
    reloc = struct.unpack_from('<Q', d, 8)[0]
    n = struct.unpack_from('<I', d, 0x14)[0]
    syms = []
    o = 0x18
    for _ in range(n):
        s, t = struct.unpack_from('<II', d, o); off = struct.unpack_from('<Q', d, o + 8)[0]; o += 16
        syms.append((s, t, off))
    cnt = struct.unpack_from('<I', d, reloc)[0]
    ptrs = set()
    for bi, b in enumerate(d[reloc + 4:reloc + 4 + cnt]):
        for k in range(8):
            if b >> k & 1:
                ptrs.add((bi * 8 + k) * 8)
    return reloc, syms, ptrs


def cstr(d, o):
    return d[o:d.index(b'\0', o)].decode('latin1')


def swap16(b):
    return bytes([b[1], b[0]])


def parse_entry(a, ap, o):
    """PS4 little-endian vox entry at o -> (raw 0x28 bytes, [(raw 0x18 block bytes, [(line name, raw 0x20 line bytes)])]).
    The only pointers are entry+0x28 (block array), block+0x10 (line array) and line+0 (name); anything else that the reloc
    bitmap marks as a pointer inside these objects aborts, so every other word is carried over verbatim."""
    u = lambda p: struct.unpack_from('<I', a, p)[0]
    q8 = lambda p: struct.unpack_from('<Q', a, p)[0]
    h = u(o)
    if o + 0x28 not in ap or any(o + k in ap for k in range(0, 0x28, 8)):
        raise ValueError('entry %08x: pointer layout' % h)
    nblocks = a[o + 0x27]
    if not nblocks:
        raise ValueError('entry %08x: no blocks' % h)
    bp = q8(o + 0x28)
    blocks = []
    for i in range(nblocks):
        q = bp + i * 0x18
        if q + 0x10 not in ap or q in ap or q + 8 in ap:
            raise ValueError('entry %08x: block %d pointer layout' % (h, i))
        count = u(q + 0xc)
        lp = q8(q + 0x10)
        lines = []
        for k in range(count):
            l = lp + k * 0x20
            if l not in ap or any(l + j in ap for j in (8, 16, 24)):
                raise ValueError('entry %08x: line %d pointer layout' % (h, k))
            lines.append((cstr(a, q8(l)), bytes(a[l:l + 0x20])))
        blocks.append((bytes(a[q:q + 0x18]), lines))
    return bytes(a[o:o + 0x28]), blocks


def build(c, entries, add):
    """apply_vo.py's writer (unchanged layout): append the entries to the DC file c, return the new file."""
    creloc, csyms, cptrs = read_dc(c)
    shift = len(add) * 16
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
    for raw, blocks in entries:
        for braw, lines in blocks:
            for nm, lraw in lines:
                strs.setdefault(nm, None)
    for s in strs:
        strs[s] = here()
        body.extend(s.encode('latin1') + b'\0')

    def putq(pos, val, isptr=True):
        struct.pack_into('<Q', body, pos - base, val)
        if isptr:
            new_ptrs.add(pos)
    for (raw, blocks), (s, t, _) in zip(entries, add):
        pad(16)
        body.extend(b'\0' * 12 + struct.pack('<I', VOX))   # 16-byte aligned object with its type word just before it
        off = here()
        body.extend(raw)
        blk_at = here() + 8
        body.extend(bytes(8))
        putq(off + 0x28, blk_at)
        for braw, lines in blocks:
            body.extend(braw)
        for i, (braw, lines) in enumerate(blocks):
            q = blk_at + i * 0x18
            la = here()
            for nm, lraw in lines:
                body.extend(lraw)
            putq(q + 0x10, la)
            for k, (nm, lraw) in enumerate(lines):
                putq(la + k * 0x20, strs[nm])
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
    if 0x20 in cptrs:
        new_ptrs |= {0x18 + i * 16 + 8 for i in range(len(out_syms))}
    nbytes = (reloc + 63) // 64
    bm = bytearray(nbytes)
    for p in new_ptrs:
        w = p // 8
        bm[w // 8] |= 1 << (w % 8)
    return bytes(hdr + tbl + body) + struct.pack('<I', nbytes) + bytes(bm)


def verify(res, old, entries, add):
    reloc, syms, ptrs = read_dc(res)
    by = {s: o for s, _, o in syms}
    assert [s for s, _, _ in syms] == sorted(s for s, _, _ in syms), 'symbols not sorted'
    r0, s0, p0 = read_dc(old)
    shift = len(add) * 16
    for s, t, o in s0:
        if o in p0:   # a symbol that starts with a pointer (vox.bin has some): it moved by the table growth
            assert struct.unpack_from('<Q', res, by[s])[0] == struct.unpack_from('<Q', old, o)[0] + shift, 'old pointer %08x' % s
        else:
            assert old[o:o + 8] == res[by[s]:by[s] + 8], 'old entry %08x changed' % s
    # every old pointer slot is still a pointer slot, shifted, with its value shifted
    for p in p0:
        if p >= 0x18 + len(s0) * 16:
            assert p + shift in ptrs and struct.unpack_from('<Q', res, p + shift)[0] == struct.unpack_from('<Q', old, p)[0] + shift, 'reloc %x' % p
    assert len(syms) == len(s0) + len(add)
    # every copied entry parses back to the same tuple (pointer slots inside the raw bytes masked: they moved)
    def mask(e):
        raw, blocks = e
        return raw, [(b[:0x10] + b'\0' * 8, [(nm, b'\0' * 8 + l[8:]) for nm, l in lines]) for b, lines in blocks]
    for e, (s, t, _) in zip(entries, add):
        assert mask(parse_entry(res, ptrs, by[s])) == mask(e), 'round trip %08x' % s
        assert struct.unpack_from('<I', res, by[s] - 4)[0] == VOX
    return by


def coop_maps():
    """(netgame index, hero rows) of every co-op adventure game in the overlay net.bin, and the script modules of all maps."""
    c = DC(served('net.bin'))
    t, o = c.bysid[NETGAME]; n = c.u32(o); e = c.ptr(o + 8)
    rows = DC(served('net.bin')); tr, orow = rows.bysid[0xbd3bb3b9]; nr = rows.u32(orow); er = rows.ptr(orow + 8)
    rowname = lambda i: rows.cstr(rows.ptr(er + i * 0x70))
    games = []
    for i in range(n):
        a = e + i * 0xd0
        if c.i32(a + 0xc) != 11:
            continue
        descs = [c.u32(c.ptr(c.ptr(a + 0x70) + k * 8)) for k in range(3)]
        games.append((i, [rowname(d) for d in descs]))
    scripts = {x for d in (RET, OVL) for x in os.listdir(d) if x.startswith('ss-coop-adv') and x.endswith('.bin')}   # no glob: the retail path has [] in it
    return games, sorted(scripts)


def script_lines(name, voxsids):
    d = open(served(name), 'rb').read(); lines = set()
    for i in range(0, len(d) - 40, 4):
        if struct.unpack_from('<I', d, i)[0] in SAY:
            for k in (8, 16, 24, 32, 40):
                w = struct.unpack_from('<I', d, i + k)[0]
                if w in voxsids:
                    lines.add(w)
    return lines


def modules_patch(newsize):
    """overlay modules.bin with the 'vox' entry's size word (+0x10, the module's .bin size; the overlay copy was regenerated
    for its bigger files) set to the new vox.bin size. Returns (bytes, old size)."""
    f = OVL + 'modules.bin'
    m = DC(f); d = bytearray(m.d); s, t, o = m.syms[0]; n = m.u32(o); arr = m.ptr(o + 8)
    for i in range(n):
        a = arr + i * 0x28
        if m.cstr(m.ptr(a)) == 'vox':
            old = m.u32(a + 0x10)
            struct.pack_into('<I', d, a + 0x10, newsize)
            return bytes(d), old
    raise SystemExit('modules.bin: no vox entry')


def main():
    apply_ = '--apply' in sys.argv[1:]; restore = '--restore' in sys.argv[1:]
    dst = OVL + 'vox.bin'
    if restore:
        for name in ('vox.bin', 'modules.bin'):
            b = BAK + name
            if not os.path.exists(b):
                raise SystemExit('no backup at ' + b)
            shutil.copy2(b, OVL + name); print('restored', OVL + name, 'from', b)
        return
    c = open(dst, 'rb').read()
    a = open(served('vox-mp.bin'), 'rb').read()
    areloc, asyms, aptrs = read_dc(a)
    creloc, csyms, cptrs = read_dc(c)
    have = {s for s, _, _ in csyms}
    srcsyms = {s: (t, o) for s, t, o in asyms if t == VOX}
    other = {}
    for f in ('vox-igc.bin',):
        for s, t, o in read_dc(open(served(f), 'rb').read())[1]:
            if t == VOX: other[s] = f
    games, scripts = coop_maps()
    print('co-op adventure games in net.bin:', [(i, h) for i, h in games])
    need = {}
    allvox = set(srcsyms) | have | set(other)
    for n in scripts:
        for s in script_lines(n, allvox):
            need.setdefault(s, set()).add(n)
    add = [(s, srcsyms[s][0], srcsyms[s][1]) for s in sorted(need) if s in srcsyms and s not in have]
    entries = [parse_entry(a, aptrs, o) for _, _, o in add]
    res = build(c, entries, add)
    by = verify(res, c, entries, add)
    os.makedirs(HERE + '/build', exist_ok=True)
    open(HERE + '/build/vox.bin', 'wb').write(res)
    print('vox.bin: %d entries -> %d (+%d from vox-mp.bin), %d -> %d bytes, written build/vox.bin' % (len(csyms), len(csyms) + len(add), len(add), len(c), len(res)))
    # per-map coverage against the result, with the speaker prefixes of the line names
    rsyms = {s for s, t, o in read_dc(res)[1] if t == VOX}
    names = {}
    for (raw, blocks), (s, t, _) in zip(entries, add):
        names[s] = blocks[0][1][0][0] if blocks and blocks[0][1] else ''
    for n in scripts:
        ls = script_lines(n, allvox)
        miss = [s for s in ls if s not in rsyms]
        pre = collections.Counter()
        for s in ls:
            m = re.search(r'-(drk|sul|chl|ctr|eln|laz|edy|fln|drz|prtb|prt)-', names.get(s, ''))
            if m: pre[m.group(1)] += 1
        print('  %-26s lines %3d  resolve in vox.bin after fix: %3d  missing: %s  speakers: %s' % (n, len(ls), len(ls) - len(miss), [hex(x) for x in miss] or 'none', dict(pre)))
    mod, oldsize = modules_patch(len(res))
    open(HERE + '/build/modules.bin', 'wb').write(mod)
    print('modules.bin: vox size word %d -> %d, written build/modules.bin' % (oldsize, len(res)))
    if not apply_:
        print('dry run: nothing installed'); return
    os.makedirs(BAK, exist_ok=True)
    for name, data in (('vox.bin', res), ('modules.bin', mod)):
        if not os.path.exists(BAK + name):
            shutil.copy2(OVL + name, BAK + name); print('backup', BAK + name)
        open(OVL + name, 'wb').write(data); print('installed', OVL + name)


if __name__ == '__main__':
    main()
