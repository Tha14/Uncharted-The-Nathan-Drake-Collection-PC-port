#!/usr/bin/env python3
"""Provenance recipes: rebuild a file byte for byte from the user's own game files.

Some overlay files were made by cloud scripts that are lost (the merged PS4 DC modules: net.bin "netfix", menu.bin,
modules.bin, medals, vox*, ...; and DC conversions the rewritten converter does not reproduce exactly). Their bytes
are, almost word for word, bytes of the PS4 retail module or of the PS3 1.19 module (byte-swapped words, swapped
halves, raw strings), placed in a new layout. A recipe records only that derivation:

  * COPY ops: (source, transform, source offset, length) - transform raw / w (32-bit swap) / h (16-bit halves);
  * pointer slots: (offset, value) - values are offsets inside the rebuilt file (structure, not content), and the
    relocation bitmap is recomputed from them;
  * ZERO runs, and LIT bytes for the few bytes no source holds (counted and listed by `make`).

The sources are named (e.g. ps4:dc1/ps4/net.bin, ps3/patch18:dc1/ps4/net.bin) and checked by sha256 when the recipe
is applied, so a recipe only rebuilds from the same inputs; the rebuilt file is checked against the target sha256.

  python dcrecipe.py make  <target> <out.json> NAME=PATH [NAME=PATH ...]
  python dcrecipe.py apply <recipe.json> <out> NAME=PATH [NAME=PATH ...]
Stdlib only."""
import collections, hashlib, json, struct, sys

K = 8


def sha(b): return hashlib.sha256(b).hexdigest()


_OPMAP = None


def opmap():
    """PS3 -> PS4 script opcode renumbering (learned by dcconv; byte 0 of each bytecode word)"""
    global _OPMAP
    if _OPMAP is None:
        import os
        p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'schema-oldconv.json')
        _OPMAP = {int(k): v for k, v in json.load(open(p))['opmap'].items()}
    return _OPMAP


def xform(buf, x):
    if x == 'r': return bytes(buf)
    if x == 'o':
        out = bytearray(buf); om = opmap()
        for i in range(0, len(out) - 3, 4): out[i] = om.get(out[i], out[i])
        return bytes(out)
    out = bytearray(len(buf))
    n = len(buf) - len(buf) % 4
    if x == 'w':
        for i in range(0, n, 4): out[i:i + 4] = buf[i:i + 4][::-1]
    elif x == 'h':
        for i in range(0, n, 4): out[i:i + 4] = buf[i + 1:i + 2] + buf[i:i + 1] + buf[i + 3:i + 4] + buf[i + 2:i + 3]
    out[n:] = buf[n:]
    return bytes(out)


def dc_slots(d):
    """pointer slots of a PS4 DC module ('00CD'), or None (then the file is treated as plain bytes)"""
    if d[:4] != b'00CD' or len(d) < 0x18: return None
    reloc = struct.unpack_from('<Q', d, 8)[0]
    if reloc + 4 > len(d): return None
    n = struct.unpack_from('<I', d, reloc)[0]
    if reloc + 4 + n != len(d): return None
    bm = d[reloc + 4:]; s = []
    for bi, b in enumerate(bm):
        if b:
            for k in range(8):
                if b >> k & 1: s.append((bi * 8 + k) * 8)
    return reloc, s


def make(target, sources, log=print):
    d = target
    ds = dc_slots(d)
    body_end = ds[0] if ds else len(d)
    slots = ds[1] if ds else []
    wild = bytearray(len(d))
    for s in slots: wild[s:s + 8] = b'\1' * 8
    bufs = []                        # (name, xform, bytes)
    for name, data in sources.items():
        for x in ('r' if name.endswith('.pak') else 'rwho'):       # level/actor paks: little-endian strings only
            bufs.append((name, x, xform(data, x)))
    idx = collections.defaultdict(list); idx4 = {}
    for bi, (name, x, b) in enumerate(bufs):
        step = 1 if x == 'r' else 4                     # 'o' (bytecode) is word-aligned like 'w'
        for i in range(0, len(b) - K + 1, step):
            key = b[i:i + K]
            if key.count(0) == K: continue
            lst = idx[key]
            if len(lst) < 24: lst.append((bi, i))
        for i in range(0, len(b) - 3, step):
            k4 = b[i:i + 4]
            if k4 not in idx4 and any(k4): idx4[k4] = (bi, i)
    ops = []; lit = 0; i = 0
    stat = collections.Counter()

    def ext(bi, j, i):
        b = bufs[bi][2]; n = 0
        while i + n < body_end and j + n < len(b) and (wild[i + n] or d[i + n] == b[j + n]): n += 1
        return n
    while i < body_end:
        if wild[i]:
            j = i
            while j < body_end and wild[j]: j += 1
            ops.append(['z', j - i]); i = j; continue             # slot bytes: filled from the pointer list
        if d[i] == 0:
            j = i
            while j < body_end and d[j] == 0 and not wild[j]: j += 1
            if j - i >= 4 or j == body_end:
                ops.append(['z', j - i]); i = j; continue
        best = None
        key = bytes(d[i:i + K])
        cands = idx.get(key, []) if not any(wild[i:i + K]) else []
        for bi, j in cands:
            n = ext(bi, j, i)
            if best is None or n > best[2]: best = (bi, j, n)
        if best is None:
            # short match: 4-byte aligned word from a word-swapped source, or raw bytes
            hit = idx4.get(bytes(d[i:i + 4])) if not any(wild[i:i + 4]) else None
            if hit is not None:
                best = (hit[0], hit[1], ext(hit[0], hit[1], i))
        if best is not None and best[2] >= 4:
            bi, j, n = best
            name, x, _ = bufs[bi]
            if ops and ops[-1][0] == 'c' and ops[-1][1] == name and ops[-1][2] == x and ops[-1][3] + ops[-1][4] == j:
                ops[-1][4] += n
            else:
                ops.append(['c', name, x, j, n])
            stat[name + '/' + x] += n; i += n
        elif d[i] == 0:                                          # short zero run (padding)
            if ops and ops[-1][0] == 'z': ops[-1][1] += 1
            else: ops.append(['z', 1])
            i += 1
        else:
            if ops and ops[-1][0] == 'l': ops[-1][1] += d[i:i + 1].hex()
            else: ops.append(['l', d[i:i + 1].hex()])
            lit += 1; i += 1
    rec = dict(version=1, size=len(d), sha256=sha(d), body_end=body_end, dc=ds is not None,
               slots=[[s, struct.unpack_from('<Q', d, s)[0]] for s in slots],
               sources={n: sha(b) for n, b in sources.items()}, ops=ops, literal_bytes=lit, stat=dict(stat))
    if not ds:
        pass
    return rec


def apply(rec, sources):
    for n, h in rec['sources'].items():
        if n not in sources: raise SystemExit('missing source ' + n)
        if sha(sources[n]) != h: raise SystemExit('source %s differs from the one the recipe was made from' % n)
    cache = {}
    out = bytearray()
    for op in rec['ops']:
        if op[0] == 'z': out += bytes(op[1])
        elif op[0] == 'l': out += bytes.fromhex(op[1])
        else:
            _, name, x, j, n = op
            if (name, x) not in cache: cache[(name, x)] = xform(sources[name], x)
            out += cache[(name, x)][j:j + n]
    for s, v in rec['slots']: struct.pack_into('<Q', out, s, v)
    if rec['dc']:
        reloc = rec['body_end']
        nb = (reloc + 63) // 64; bm = bytearray(nb)
        for s, _ in rec['slots']:
            w = s // 8; bm[w // 8] |= 1 << (w % 8)
        out += struct.pack('<I', nb) + bm
    out = bytes(out)
    if len(out) != rec['size'] or sha(out) != rec['sha256']: raise SystemExit('rebuilt file differs from the recipe target')
    return out


def parse_srcs(args):
    s = {}
    for a in args:
        n, p = a.split('=', 1); s[n] = open(p, 'rb').read()
    return s


def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    if argv[1] == 'make':
        t = open(argv[2], 'rb').read(); rec = make(t, parse_srcs(argv[4:]))
        try:
            apply(rec, parse_srcs(argv[4:]))
        except SystemExit as ex:
            print('SELF-CHECK FAILED', ex); return 1
        json.dump(rec, open(argv[3], 'w'), separators=(',', ':'))
        print('%s: %d bytes, %d ops, %d slots, literal %d, %s' % (argv[2].split('/')[-1], rec['size'], len(rec['ops']), len(rec['slots']), rec['literal_bytes'], rec['stat']))
        return 0
    if argv[1] == 'apply':
        rec = json.load(open(argv[2])); out = apply(rec, parse_srcs(argv[4:]))
        open(argv[3], 'wb').write(out); print('rebuilt', argv[3], len(out)); return 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
