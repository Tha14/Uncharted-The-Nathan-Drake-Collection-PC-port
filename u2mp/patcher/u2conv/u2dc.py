"""Uncharted 2 DC modules (script data, dc1/*.bin), PS3 -> PS4, on the U3 schema-learning converter
(u3mp/patcher/tools/dcconv/dcconv.py), loaded as a private copy; nothing in u3mp is modified.

U2 module header (differs from U3, whose symbol table sits right after a 0x18-byte header with a type per entry):
  PS3 'DC00' BE:  +0 magic, +4 u32 1, +8 u32 reloc, +0xc u32 0, +0x10 u32 root ptr (relocated slot),
                  +0x14 type header word 0x916aa605, +0x18 root {u32 1, u32 count, {u32 sid, u32 ptr}[count]}
  PS4 '00CD' LE:  +0 magic, +4 u32 1, +8 u64 reloc, +0x10 u64 root ptr (slot), +0x18 [u32 0][u32 0x916aa605],
                  +0x20 root {u32 1, u32 count, {u32 sid, u32 0, u64 ptr}[count]}
  then the object data, then at reloc: PS3 {u32 count, u32 slot offset[count]} (+ zero padding), PS4 {u32 byte
  count, bitmap: bit k of byte i marks the 8-byte pointer slot (8i + k) * 8}.
The PS4 loader (U2 1.02 0xa19a00) checks '00CD' and 16-byte alignment, adds the base to every slot and returns
base+0x10; the game then binds each root entry (sid -> ptr) into the script symbol table (0xa16d70).

The U3 converter is used as is with a U2 header parser: every symbol gets its sid as 'type', so an object that a
symbol names is keyed by that symbol (a module that defines the same symbol as a learned one - net-patch9 vs net.bin
- reuses its exact layout); other objects are keyed by their type header or the pointer path, as in U3. The output
is written in U3 layout and moved 0x10 bytes up into the U2 header (alignment is kept: 0x18+16n and 0x28+16n are
equal mod 16).

  python u2dc.py learn   <ps3 base> <ps4 game> <schema.json> [net-patch9.bin]   pairs: PS3 dc1.psarc dc1/<x>.bin +
                         PS4 dc1/ps4/<x>.bin (graph-differing pairs and net-patch9~net.bin learned partially)
  python u2dc.py test    <ps3 base> <ps4 game> <schema.json> [net-patch9.bin]   convert every pair, compare with PS4
  python u2dc.py convert <schema.json> <ps3 module.bin> <out.bin>
  python u2dc.py info    <module.bin>                                 header, symbols
No game bytes are stored; the schema holds layouts (offsets / transforms) and the opcode map only.
"""
import collections
import hashlib
import importlib.util
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
VERSION = 'u2dc1'          # bump when the schema or the conversion changes
ROOT_TYPE = 0x916aa605     # type header of the root table


def find_dcconv():
    cands = [os.environ.get('U2MP_DCCONV', ''),
             os.path.join(HERE, '..', '..', '..', 'u3mp', 'patcher', 'tools', 'dcconv'),
             os.path.join(HERE, '..', 'tools', 'dcconv')]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, 'dcconv.py')):
            return os.path.abspath(os.path.join(c, 'dcconv.py'))
    raise RuntimeError('the Uncharted 3 DC converter (u3mp/patcher/tools/dcconv/dcconv.py) was not found next to '
                       'u2mp; set U2MP_DCCONV')


# U3 PS4 writes a type header as 8 bytes [0][type]; U2 PS4 keeps the 4-byte [type] right before the body (the body
# alignment decides any padding before it). Text edits of the private copy (each must apply exactly once).
_HDR4_EDITS = [
    ("    s4 = t4 - 8 if hdr is not None else t4\n", "    s4 = t4 - 4 if hdr is not None else t4\n"),
    ("        if b.d[s4:s4 + 8] != b'\\0\\0\\0\\0' + struct.pack('<I', hdr): return None\n",
     "        if b.d[s4:s4 + 4] != struct.pack('<I', hdr): return None\n"),
    ("        last = max(last, r4 + (8 if ('P' in ks or 'T' in ks) else 4))\n",
     "        last = max(last, r4 + (8 if 'P' in ks else 4))\n"),
    ("    hl4 = 8 if hl else 0\n    conv = lambda", "    hl4 = 4 if hl else 0\n    conv = lambda"),
    ("ok = t4 is not None and t4 >= 8 and b.d[t4 - 8:t4] == b'\\0\\0\\0\\0' + struct.pack('<I', h)",
     "ok = t4 is not None and t4 >= 4 and b.d[t4 - 4:t4] == struct.pack('<I', h)"),
    ("starts4.append(None if t4 is None else (t4 - 8 if ob[2] is not None else t4))",
     "starts4.append(None if t4 is None else (t4 - 4 if ob[2] is not None else t4))"),
    ("s4 = ev['s4'] + (8 if ob[2] is not None else 0); al_ = 1", "s4 = ev['s4'] + (4 if ob[2] is not None else 0); al_ = 1"),
    ("if hdr is not None: out.append([0, 0, 'T']); r4 = 8; o += 4", "if hdr is not None: out.append([0, 0, 'T']); r4 = 4; o += 4"),
    ("                    hl4 = 8 if hl else 0\n                    units = ([[0, 0, 'T']]",
     "                    hl4 = 4 if hl else 0\n                    units = ([[0, 0, 'T']]"),
    ("fills = []; hl4 = 8 if hl else 0; o4 = hl4", "fills = []; hl4 = 4 if hl else 0; o4 = hl4"),
    ("hd = 8 if pl['hdr'] is not None else 0", "hd = 4 if pl['hdr'] is not None else 0"),
    ("hl3 = 4 if pl['hdr'] is not None else 0; hl4 = 8 if pl['hdr'] is not None else 0",
     "hl3 = 4 if pl['hdr'] is not None else 0; hl4 = 4 if pl['hdr'] is not None else 0"),
    ("u[1] + (8 if u[2] in ('P', 'T', 'Q') else 4)", "u[1] + (8 if u[2] in ('P', 'Q') else 4)"),
    ("                struct.pack_into('<II', out, s4, 0, pl['hdr'])\n",
     "                struct.pack_into('<I', out, s4, pl['hdr'])\n"),
    ("                hl4 = 8 if pl['hdr'] is not None else 0\n                out[s4 + hl4",
     "                hl4 = 4 if pl['hdr'] is not None else 0\n                out[s4 + hl4"),
    ("                    struct.pack_into('<II', out, o4, 0, struct.unpack('>I', w)[0])\n",
     "                    struct.pack_into('<I', out, o4, struct.unpack('>I', w)[0])\n"),
    # a pointer right after a 4-byte header: the body (not the header) must be 8-aligned
    ("            if any(u[2] in 'PQ' and u[1] % 8 == 0 for u in units):\n",
     "            if any(u[2] in 'PQ' and (u[1] - (4 if pl['hdr'] is not None else 0)) % 8 == 0 for u in units):\n"),
    ("            r4 = al(r4, 8); out.append([o - s3, r4, 'P']); r4 += 8\n",
     "            hb = 4 if hdr is not None else 0; r4 = al(r4 - hb, 8) + hb; out.append([o - s3, r4, 'P']); r4 += 8\n"),
    # generic containers ('array' holds {count, elements inline} of any element type, 'map'): their layout depends
    # on where they hang, so they are keyed by type + the field that refers to them
    ("            objs[i][2] = cand[i]; keys[i] = 'H%08x' % cand[i]\n",
     "            objs[i][2] = cand[i]; keys[i] = ('H%08x' % cand[i]) if cand[i] not in GENERIC_HDRS "
     "else ('H%08x/%s' % (cand[i], fk))\n"),
]
GENERIC_HDRS = {0x1ed2e0a2,   # sid('array')
                0x16094dd2}   # sid('map')


def _load_dcconv():
    path = find_dcconv()
    src = open(path, encoding='utf-8').read()
    for old, new in _HDR4_EDITS:
        if src.count(old) != 1:
            raise RuntimeError('u2dc: dcconv.py changed, edit not applicable: %r' % old[:60])
        src = src.replace(old, new)
    spec = importlib.util.spec_from_loader('u2mp_dcconv', loader=None, origin=path)
    mod = importlib.util.module_from_spec(spec)
    mod.__file__ = path
    mod.GENERIC_HDRS = GENERIC_HDRS
    exec(compile(src, path, 'exec'), mod.__dict__)
    return mod


D = _load_dcconv()


class U2Mod(D.Mod):
    """dcconv.Mod for the U2 header (see the module doc)"""

    def __init__(self, d):
        self.d = d = bytes(d)
        if d[:4] == b'DC00':
            self.be, self.ps, e = True, 4, '>'
            self.reloc = struct.unpack_from('>I', d, 8)[0]
            self.root = struct.unpack_from('>I', d, 0x10)[0]
        elif d[:4] == b'00CD':
            self.be, self.ps, e = False, 8, '<'
            self.reloc = struct.unpack_from('<Q', d, 8)[0]
            self.root = struct.unpack_from('<Q', d, 0x10)[0]
        else:
            raise ValueError('not a DC module')
        self.e = e
        if not (0x14 <= self.root < self.reloc <= len(d) - 4):
            raise ValueError('bad U2 DC header (root %#x reloc %#x size %#x)' % (self.root, self.reloc, len(d)))
        self.root_hdr = struct.unpack_from(e + 'I', d, self.root - 4)[0]
        self.root_word0, n = struct.unpack_from(e + 'II', d, self.root)
        es = 4 + self.ps if self.ps == 4 else 16
        self.syms = []
        for i in range(n):
            o = self.root + 8 + i * es
            s = struct.unpack_from(e + 'I', d, o)[0]
            v = struct.unpack_from(e + ('I' if self.ps == 4 else 'Q'), d, o + (4 if self.ps == 4 else 8))[0]
            self.syms.append((s, s, v))          # 'type' = the sid itself: symbol-specific layout keys
        self.data_start = self.root + 8 + n * es
        cnt = struct.unpack_from(e + 'I', d, self.reloc)[0]
        if self.be:              # PS3: a list of slot offsets (then zero padding)
            if self.reloc + 4 + 4 * cnt > len(d):
                raise ValueError('relocation list past the end of the module')
            self.slots = set(struct.unpack_from('>%dI' % cnt, d, self.reloc + 4))
            self.tail = d[self.reloc + 4 + 4 * cnt:]
        else:                    # PS4: a bitmap, bit k of byte i = slot (8 * i + k) * 8
            bm = d[self.reloc + 4:self.reloc + 4 + cnt]
            self.tail = d[self.reloc + 4 + cnt:]
            self.slots = set()
            for bi, b in enumerate(bm):
                if not b:
                    continue
                for k in range(8):
                    if b >> k & 1:
                        self.slots.add((bi * 8 + k) * 8)
        self.dslots = sorted(s for s in self.slots if s >= self.data_start)


D.Mod = U2Mod          # the private copy parses U2 headers everywhere (Learner, Converter)


def u3_to_u2(out3, m3):
    """dcconv output (U3 header + 16-byte symbol entries at 0x18) -> U2 PS4 module (0x10 bytes further up)"""
    n = len(m3.syms)
    reloc3 = struct.unpack_from('<Q', out3, 8)[0]
    nb3 = struct.unpack_from('<I', out3, reloc3)[0]
    bm3 = out3[reloc3 + 4:reloc3 + 4 + nb3]
    ds3 = 0x18 + 16 * n
    slots3 = [(bi * 8 + k) * 8 for bi, b in enumerate(bm3) if b for k in range(8) if b >> k & 1]
    delta = 0x10
    reloc = reloc3 + delta
    out = bytearray(reloc)
    out[0:4] = b'00CD'
    struct.pack_into('<IQQII', out, 4, 1, reloc, 0x20, 0, m3.root_hdr)
    struct.pack_into('<II', out, 0x20, m3.root_word0, n)
    slots = [0x10]
    for i in range(n):
        s, _t, v = struct.unpack_from('<IIQ', out3, 0x18 + 16 * i)
        o = 0x28 + 16 * i
        struct.pack_into('<IIQ', out, o, s, 0, v + delta if v else 0)
        slots.append(o + 8)
    out[ds3 + delta:reloc] = out3[ds3:reloc3]
    for p in slots3:
        if p < ds3:
            continue                                          # symbol entry slots: written above
        q = p + delta
        v = struct.unpack_from('<Q', out, q)[0]
        struct.pack_into('<Q', out, q, v + delta if v else 0)
        slots.append(q)
    nb = (reloc + 63) // 64
    bm = bytearray(nb)
    for p in slots:
        w = p // 8
        bm[w // 8] |= 1 << (w % 8)
    out += struct.pack('<I', nb) + bm
    return bytes(out)


# ------------------------------------------------------------------------------------------- learn / convert


def pairs(ps3, ps4_dc_dir):
    """[(name, ps3 entry, ps4 path)]: every PS3 dc1/<x>.bin (newest source) with a PS4 dc1/ps4/<x>.bin"""
    out = []
    for f in sorted(os.listdir(ps4_dc_dir)):
        if not f.endswith('.bin'):
            continue
        c = ps3.candidates('dc1/' + f)
        if c:
            out.append((f, c[0], os.path.join(ps4_dc_dir, f)))
    return out


def partial_anchors(a, b):
    """PS3->PS4 offset map for modules whose graphs differ (PS4 built from another PS3 version): walk every symbol
    both define in parallel, pairing an object's pointer slots in order when both have the same number, and map
    the objects / slots reached that way. Unmapped objects are left out (not learned)."""
    ta = sorted(set(v for _, _, v in a.syms) | set(a.ptr(s) for s in a.dslots))
    tb = sorted(set(v for _, _, v in b.syms) | set(b.ptr(s) for s in b.dslots))
    import bisect

    def slots_in(m, tg, t):
        i = bisect.bisect_right(tg, t)
        e = tg[i] if i < len(tg) else m.reloc
        return m.dslots[bisect.bisect_left(m.dslots, t):bisect.bisect_left(m.dslots, e)]
    sb = {s: v for s, _, v in b.syms}
    M = {}
    bad = set()
    stack = [(v, sb[s]) for s, _, v in a.syms if s in sb]
    while stack:
        x, y = stack.pop()
        if not (a.data_start <= x < a.reloc and b.data_start <= y < b.reloc):
            continue
        if x in M:
            if M[x] != y:
                bad.add(x)
            continue
        M[x] = y
        s3, s4 = slots_in(a, ta, x), slots_in(b, tb, y)
        if len(s3) != len(s4):
            continue
        for p, q in zip(s3, s4):
            if p in M and M[p] != q:
                bad.add(p)
            M[p] = q
            stack.append((a.ptr(p), b.ptr(q)))
    for x in bad:
        M.pop(x, None)
    M[a.data_start] = b.data_start
    M[a.reloc] = b.reloc
    return M


def add_partial(L, d3, d4):
    """Learner.add_pair for a pair whose graphs differ: only objects mapped by partial_anchors whose layout aligns
    (same contents) are learned; the others are not counted as failures."""
    a, b = U2Mod(d3), U2Mod(d4)
    M = partial_anchors(a, b)

    def decide(i, fk, h):
        t4 = M.get(objs_t[i])
        ok = t4 is not None and t4 >= 4 and b.d[t4 - 4:t4] == struct.pack('<I', h)
        if t4 is not None:
            for k_ in {fk, D.tailkey(fk, 1) or fk}:
                L.hdrtab.setdefault(k_, [0, 0])[0 if ok else 1] += 1
        return ok
    objs_t = sorted(set(v for _, _, v in a.syms) | set(a.ptr(s) for s in a.dslots))
    objs_t = [x for x in objs_t if a.data_start <= x < a.reloc]
    objs, keys, fks = D.graph(a, decide)
    starts4 = [None if M.get(ob[0]) is None else (M[ob[0]] - 4 if ob[2] is not None else M[ob[0]]) for ob in objs]
    pref = [p for p, _ in L.prefixes.most_common(8)]
    n = 0
    for i, ob in enumerate(objs):
        nx = starts4[i + 1] if i + 1 < len(objs) else b.reloc
        if starts4[i] is None or nx is None or nx < starts4[i]:
            continue
        ev = D.align_object(a, b, M, ob, nx, pref)
        if ev is None:
            continue
        if ob[2] is not None:
            L.hdrs[ob[2]] += 1
        for ks in [x for x in (keys[i], D.tailkey(keys[i], 2), D.tailkey(keys[i], 1)) if x]:
            L._one(ks, ev, ob, a, b, M, nx)
        n += 1
    L.stats['partial-pairs'] += 1
    L.stats['partial-obj-ok'] += n
    return n


def learn(pair_list, log=print, extra=()):
    """pair_list: [(name, PS3 entry, PS4 path)]; extra: [(name, PS3 bytes, PS4 bytes)] learned as partial pairs
    (e.g. the PS3 server net-patch9 against the PS4 net.bin: the PS4 tables are a later version of it)"""
    L = D.Learner()
    for name, e3, p4 in pair_list:
        try:
            d3, d4 = e3.read(), open(p4, 'rb').read()
            if not L.add_pair(d3, d4):
                add_partial(L, d3, d4)
        except Exception as ex:
            L.stats['pair-error'] += 1
            log('u2dc: %s: %s: %s' % (name, type(ex).__name__, ex))
    for name, d3, d4 in extra:
        try:
            log('u2dc: %s: %d objects learned (partial pair)' % (name, add_partial(L, d3, d4)))
        except Exception as ex:
            L.stats['pair-error'] += 1
            log('u2dc: %s: %s: %s' % (name, type(ex).__name__, ex))
    S = L.finish()
    S.update(version=2, u2dc=VERSION, pairs=len(pair_list), stats=dict(L.stats))
    return S


class _Conv(D.Converter):
    """PS3 U2 objects carry the zero padding up to the next object (and to the 16-aligned end of the data); PS4 drops
    it. An object of a fixed-size key (one learned size) that is longer by < 16 zero bytes is cut back to that size
    before its layout is extended."""

    def plan(self, m, rep):
        plans = super().plan(m, rep)
        for pl in plans:
            ent = self.S.get(pl['key'])
            if not ent or pl.get('kind') not in ('units', 'tmpl') or ent.get('period') or len(ent.get('span', {})) != 1:
                continue
            n = ent.get('n3max', 0)
            s3, e3 = pl['s3'], pl['e3']
            if n and 0 < e3 - s3 - n < 16 and not any(m.d[s3 + n:e3]):
                ref = ent.get('units') or []
                if pl['kind'] == 'units' and ref and all(r in {u[0] for u in ref} for r in range(0, n - 3, 4)):
                    pl['units'] = [list(u) for u in ref if u[0] < n]
                    pl['e3'] = s3 + n
                    rep['pad-trimmed'] += 1
        return plans


class Converter:
    def __init__(self, schema):
        self.C = _Conv(schema)

    def convert(self, d3, rep=None):
        """PS3 U2 module bytes -> (PS4 U2 module bytes, report Counter)"""
        rep = rep if rep is not None else collections.Counter()
        m3 = U2Mod(d3)
        if not m3.be:
            raise ValueError('input is not a PS3 module')
        out3, rep = self.C.convert(d3, rep)
        return u3_to_u2(out3, m3), rep


def net_extra(ps4_dc_dir, net_patch):
    """the partial training pair (PS3 net-patch9, PS4 net.bin) or []"""
    p4 = os.path.join(ps4_dc_dir, 'net.bin')
    if net_patch is None or not os.path.isfile(p4):
        return []
    return [('net-patch9.bin~net.bin', net_patch, open(p4, 'rb').read())]


def schema_for(ps3, ps4_dc_dir, cache_dir, log=print, net_patch=None):
    """the learned schema, cached in cache_dir by the inputs' names / sizes and VERSION. net_patch: PS3 net-patch9
    bytes, also learned against the PS4 net.bin (partial pair)"""
    pl = pairs(ps3, ps4_dc_dir)
    extra = net_extra(ps4_dc_dir, net_patch)
    h = hashlib.sha1(VERSION.encode())
    for name, e3, p4 in pl:
        h.update(('%s %d %d;' % (name, e3.size, os.path.getsize(p4))).encode())
    for name, d3, d4 in extra:
        h.update(('%s %s %d;' % (name, hashlib.sha1(d3).hexdigest(), len(d4))).encode())
    os.makedirs(cache_dir, exist_ok=True)
    path = os.path.join(cache_dir, 'u2dc-schema-%s.json' % h.hexdigest()[:12])
    if os.path.isfile(path):
        try:
            return json.load(open(path)), pl
        except ValueError:
            pass
    log('u2dc: learning the DC layout schema from %d PS3/PS4 module pairs' % (len(pl) + len(extra)))
    S = learn(pl, log=log, extra=extra)
    log('u2dc: %d keys, %s' % (len(S['keys']), S['stats']))
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(S, f, separators=(',', ':'))
    os.replace(tmp, path)
    return S, pl


def test(conv, pl, log=print):
    """convert every pair's PS3 module and compare with the PS4 one -> {'identical': n, 'different': n, ...}"""
    tot = collections.Counter()
    for name, e3, p4 in pl:
        want = open(p4, 'rb').read()
        try:
            got, rep = conv.convert(e3.read())
        except Exception as ex:
            tot['error'] += 1
            log('ERROR %s %s: %s' % (name, type(ex).__name__, ex))
            continue
        if got == want:
            tot['identical'] += 1
        else:
            tot['different'] += 1
            n = min(len(got), len(want))
            fd = next((i for i in range(n) if got[i] != want[i]), n)
            log('DIFF %-36s %7d %7d first@%x %s' % (name, len(got), len(want), fd,
                                                   {k: v for k, v in rep.items() if v}))
    return dict(tot)


def check_module(data):
    """sanity check of a converted PS4 module: header, root, symbols and every pointer inside the data"""
    m = U2Mod(data)
    if m.be:
        raise ValueError('not a PS4 module')
    if m.root != 0x20 or m.root_hdr != ROOT_TYPE:
        raise ValueError('unexpected root %#x / type %#x' % (m.root, m.root_hdr))
    bad = [s for s in m.slots if s >= m.reloc or not (s == 0x10 or m.root <= m.ptr(s) < m.reloc or m.ptr(s) == 0)]
    if bad:
        raise ValueError('%d pointer slots outside the module' % len(bad))
    sids = [s for s, _, _ in m.syms]
    if sids != sorted(sids):
        raise ValueError('symbol table is not sorted')
    return dict(symbols=len(sids), pointers=len(m.slots), size=len(data))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == 'info':
        m = U2Mod(open(argv[2], 'rb').read())
        print('%s reloc %#x root %#x type %#x word0 %d symbols %d data %#x slots %d' % (
            'PS3' if m.be else 'PS4', m.reloc, m.root, m.root_hdr, m.root_word0, len(m.syms), m.data_start,
            len(m.slots)))
        for s, _, v in m.syms:
            print('  %08x -> %#x' % (s, v))
        return 0
    if cmd in ('learn', 'test'):
        sys.path.insert(0, HERE)
        import sources
        ps3 = sources.PS3(argv[2], None)
        ps4 = sources.PS4(argv[3])
        dc = os.path.join(ps4.main, 'dc1', 'ps4')
        pl = pairs(ps3, dc)
        np9 = open(argv[5], 'rb').read() if len(argv) > 5 else None
        if cmd == 'learn' or not os.path.isfile(argv[4]):
            S = learn(pl, extra=net_extra(dc, np9))
            json.dump(S, open(argv[4], 'w'), separators=(',', ':'))
            print('learned %d keys from %d pairs %s' % (len(S['keys']), len(pl), S['stats']))
            if cmd == 'learn':
                return 0
        r = test(Converter(json.load(open(argv[4]))), pl)
        print(r, 'match rate %.1f%%' % (100.0 * r.get('identical', 0) / max(1, len(pl))))
        return 0
    if cmd == 'convert':
        conv = Converter(json.load(open(argv[2])))
        out, rep = conv.convert(open(argv[3], 'rb').read())
        print(check_module(out), {k: v for k, v in rep.items() if v})
        with open(argv[4], 'wb') as f:
            f.write(out)
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
