#!/usr/bin/env python3
"""PS3 -> PS4 Naughty Dog DC module converter (Uncharted 3), schema learned from the game's own pairs.

The PS3 module ('DC00', big-endian, 32-bit pointers) and the PS4 one ('00CD', little-endian, 64-bit pointers)
hold the same object graph in the same order; only the layout of each object changes (pointers widen to 8 bytes
and 8-align, type headers [type] become [0][type], structs with pointers are 8-aligned / padded, u16 fields and
byte arrays keep their order, u64 fields swap as a whole, script bytecode keeps its bytes but some opcodes are
renumbered, source-path strings lose their 'u3final/big3/' prefix). The file carries no type information, so the
layouts are learned from Bluepoint's own conversion: every PS3 disc module whose PS4 Collection counterpart has
the same pointer graph is aligned unit by unit, and the result is kept per object *key*:
    ('H', type-sid)                 object with a type header (the word before it)
    ('S', symbol type)              value of a symbol-table entry
    (parent key, field offset)      any other object, keyed by the first pointer slot that refers to it
For each key the schema keeps, per PS3 4-byte unit, the PS4 offset and the byte transform (W sw32 / H sw16 /
R raw / Q u64 / P pointer / T header), or a raw kind (str / code / blob), plus the start alignment and end
rounding. Converting walks the PS3 objects in order and lays every object out from its key's schema (periodic
extension for arrays longer than any learned one; a generic fallback otherwise, counted in the report).

  python dcconv.py learn  <pairs.tsv> <schema.json>          pairs.tsv: ps3-path<TAB>ps4-path per line
  python dcconv.py convert <schema.json> <ps3.bin> <out.bin>
  python dcconv.py test   <schema.json> <pairs.tsv> [--diff]  convert each ps3 file, compare with the ps4 file

Stdlib only. Inputs are read, never executed."""
import bisect, collections, json, os, struct, sys

# ----------------------------------------------------------------------------------------------- PS3 / PS4 parse


class Mod:
    def __init__(self, d):
        self.d = d = bytes(d)
        if d[:4] == b'DC00':
            self.be, self.ps, e = True, 4, '>'
            self.reloc = struct.unpack_from('>I', d, 8)[0]
        elif d[:4] == b'00CD':
            self.be, self.ps, e = False, 8, '<'
            self.reloc = struct.unpack_from('<Q', d, 8)[0]
        else:
            raise ValueError('not a DC module')
        self.e = e
        n = struct.unpack_from(e + 'I', d, 0x14)[0]
        es = 8 + self.ps
        self.syms = []
        for i in range(n):
            o = 0x18 + i * es
            s, t = struct.unpack_from(e + 'II', d, o)
            v = struct.unpack_from(e + ('I' if self.ps == 4 else 'Q'), d, o + 8)[0]
            self.syms.append((s, t, v))
        self.data_start = 0x18 + n * es
        cnt = struct.unpack_from(e + 'I', d, self.reloc)[0]
        bm = d[self.reloc + 4:self.reloc + 4 + cnt]
        self.tail = d[self.reloc + 4 + cnt:]
        self.slots = set()
        for bi, b in enumerate(bm):
            if not b: continue
            for k in range(8):
                if (b >> (7 - k) & 1) if self.be else (b >> k & 1):
                    self.slots.add((bi * 8 + k) * self.ps)
        self.dslots = sorted(s for s in self.slots if s >= self.data_start)

    def u32(self, o): return struct.unpack_from(self.e + 'I', self.d, o)[0]

    def ptr(self, o): return struct.unpack_from(self.e + ('I' if self.ps == 4 else 'Q'), self.d, o)[0]


# ----------------------------------------------------------------------------------------------- objects and keys


def region(ob):
    """PS3 byte range of an object including its header word"""
    return (ob[0] - 4 if ob[2] is not None else ob[0]), ob[1]


def objects(m, hdr_ok=None):
    """PS3 objects: regions starting at every pointer target / symbol value; a type header (the word before a
    target that is not a pointer slot) is attached to its object when hdr_ok(target, word) says so (training: the
    PS4 file shows [0][word] there; converting: the word is a learned header type)."""
    tg = set(v for _, _, v in m.syms) | set(m.ptr(s) for s in m.dslots)
    tg = sorted(t for t in tg if m.data_start <= t < m.reloc)
    objs = [[t, tg[i + 1] if i + 1 < len(tg) else m.reloc, None] for i, t in enumerate(tg)]
    for i, ob in enumerate(objs):
        t = ob[0]
        if t % 4 == 0 and t - 4 >= m.data_start and (t - 4) not in m.slots and (i == 0 or objs[i - 1][0] < t - 4):
            h = m.u32(t - 4)
            if h > 0xffff and (hdr_ok is None or hdr_ok(t, h)):
                ob[2] = h
    for i in range(len(objs) - 1):                 # an object ends where the next one's header starts
        objs[i][1] = region(objs[i + 1])[0]
    if objs and region(objs[0])[0] > m.data_start:
        objs.insert(0, [m.data_start, region(objs[0])[0], None])
    return objs


def obj_keys(m, objs):
    """keys of already-decided objects (kept for the debugging tools)"""
    return [k for k in graph(m, lambda i, fk, h: objs[i][2] is not None if i < len(objs) else False)[1]], None


def tailkey(k, n=2):
    """generalised key: the anchor (header / symbol type) + the last n field steps: 'H45c51f9f.20.8.10' ->
    'H45c51f9f~8.10' (n=2). Learned next to the full key; used when the full path was never seen."""
    parts = k.split('.')
    if len(parts) <= n + 1: return None
    return parts[0] + '~' + '.'.join(parts[-n:])


def graph(m, decide):
    """objects + keys. Header decisions are made in BFS order from the field that refers to an object:
    decide(index, field_key, header_word) -> bool. Returns (objs, keys, field_keys)."""
    tg = set(v for _, _, v in m.syms) | set(m.ptr(s) for s in m.dslots)
    tg = sorted(t_ for t_ in tg if m.data_start <= t_ < m.reloc)
    objs = [[t_, tg[i + 1] if i + 1 < len(tg) else m.reloc, None] for i, t_ in enumerate(tg)]
    cand = [None] * len(objs)
    for i, ob in enumerate(objs):
        t_ = ob[0]
        if t_ % 4 == 0 and t_ - 4 >= m.data_start and (t_ - 4) not in m.slots and (i == 0 or objs[i - 1][0] < t_ - 4):
            h = m.u32(t_ - 4)
            if h > 0xffff: cand[i] = h
    start = {ob[0]: i for i, ob in enumerate(objs)}
    slots_by_obj = collections.defaultdict(list)
    for s in m.dslots:
        k = bisect.bisect_right(tg, s) - 1
        if k >= 0: slots_by_obj[k].append(s)
    keys = [None] * len(objs); fks = [None] * len(objs)

    def visit(i, fk):
        fks[i] = fk
        if cand[i] is not None and decide(i, fk, cand[i]):
            objs[i][2] = cand[i]; keys[i] = 'H%08x' % cand[i]
        else:
            keys[i] = fk
    q = collections.deque()
    for s, ty, v in m.syms:
        i = start.get(v)
        if i is not None and keys[i] is None:
            visit(i, 'S%08x' % ty); q.append(i)
    while q:
        i = q.popleft()
        for s in slots_by_obj[i]:
            c = start.get(m.ptr(s))
            if c is None or keys[c] is not None: continue
            visit(c, '%s.%x' % (keys[i], s - objs[i][0])); q.append(c)
    for i in range(len(objs)):
        if keys[i] is None: visit(i, 'O')
    for i in range(len(objs) - 1):
        objs[i][1] = region(objs[i + 1])[0]
    if objs and region(objs[0])[0] > m.data_start:
        objs.insert(0, [m.data_start, region(objs[0])[0], None]); keys.insert(0, 'O'); fks.insert(0, 'O')
    return objs, keys, fks


# ----------------------------------------------------------------------------------------------- transforms

def sw32(b): return b[::-1]


def sw16(b): return b[1::-1] + b[3:1:-1]


TRANS = {'W': sw32, 'H': sw16, 'R': lambda b: b}


def as_string(body):
    """body = one NUL-terminated printable string + zero padding -> the string with its NUL, else None"""
    z = body.find(b'\0')
    if z <= 0 or any(body[z:]): return None
    s = body[:z]
    if not all(32 <= c < 127 or c in (9, 10, 13) for c in s): return None
    return body[:z + 1]


def strip_prefix(s, prefixes):
    for p in prefixes:
        if s.startswith(p): return s[len(p):]
    return s


# ----------------------------------------------------------------------------------------------- pair alignment


def anchors(a, b):
    """PS3->PS4 offset map from pointer slots, pointer targets and symbol values (longest monotone subset)"""
    if len(a.dslots) != len(b.dslots) or len(a.syms) != len(b.syms): return None
    if [s for s, _, _ in a.syms] != [s for s, _, _ in b.syms]: return None
    pairs = set(zip(a.dslots, b.dslots))
    for x, y in zip(a.dslots, b.dslots): pairs.add((a.ptr(x), b.ptr(y)))
    for (s, t, v), (s2, t2, v2) in zip(a.syms, b.syms): pairs.add((v, v2))
    pairs.add((a.data_start, b.data_start)); pairs.add((a.reloc, b.reloc))
    M = {}; bad = set()
    for x, y in sorted(pairs):
        if x in M and M[x] != y: bad.add(x)
        M[x] = y
    for x in bad: del M[x]
    xs = sorted(M); ys = [M[x] for x in xs]
    if all(ys[i] <= ys[i + 1] for i in range(len(ys) - 1)): return M
    tails, tidx, prev = [], [], [-1] * len(xs)          # PS4 may pool strings elsewhere: longest monotone subset
    for i, y in enumerate(ys):
        k = bisect.bisect_right(tails, y)
        if k == len(tails): tails.append(y); tidx.append(i)
        else: tails[k] = y; tidx[k] = i
        prev[i] = tidx[k - 1] if k else -1
    keep = set(); i = tidx[-1]
    while i >= 0: keep.add(xs[i]); i = prev[i]
    return {x: M[x] for x in xs if x in keep}


def seg_feasible(W, G, base4, tailmax=None):
    """W: PS3 4-byte words, G: PS4 bytes of the segment (base4 = its absolute PS4 offset).
    Returns per word {rel position: kinds} over every complete alignment, or None."""
    n, L = len(W), len(G)
    if L % 4: return None
    zero = [G[p:p + 4] == b'\0\0\0\0' for p in range(0, L, 4)]
    zt = [True] * (L // 4 + 1)
    for k in range(L // 4 - 1, -1, -1): zt[k] = zt[k + 1] and zero[k]
    if 4 * n > L: return None

    def moves(i, p):
        out = []
        if i < n and p + 4 <= L:
            g = G[p:p + 4]
            for kd, f in TRANS.items():
                if f(W[i]) == g: out.append((kd, i + 1, p + 4))
            if i + 1 < n and p + 8 <= L and (base4 + p) % 8 == 0 and (W[i] + W[i + 1])[::-1] == G[p:p + 8]:
                out.append(('Q', i + 2, p + 8))
        if p + 4 <= L and zero[p // 4] and (L - p - 4) >= 4 * (n - i):
            out.append(('gap', i, p + 4))
        return out
    F = {(0, 0)}; order = [(0, 0)]; edges = {}
    k = 0
    while k < len(order):
        st = order[k]; k += 1
        es = moves(*st); edges[st] = es
        for kd, i2, p2 in es:
            if (i2, p2) not in F:
                F.add((i2, p2)); order.append((i2, p2))
        if len(order) > 300000: return None
    ends = [st for st in F if st[0] == n and zt[st[1] // 4] and (tailmax is None or L - st[1] < tailmax)]
    if not ends: return None
    rev = collections.defaultdict(list)
    for st, es in edges.items():
        for kd, i2, p2 in es: rev[(i2, p2)].append(st)
    B = set(ends); stack = list(ends)
    while stack:
        t = stack.pop()
        for s in rev[t]:
            if s not in B: B.add(s); stack.append(s)
    res = [collections.defaultdict(set) for _ in range(n)]
    for st in B:
        i, p = st
        for kd, i2, p2 in edges.get(st, ()):
            if (i2, p2) in B and kd != 'gap':
                if kd == 'Q':
                    res[i][p].add('Q'); res[i + 1][p + 4].add('q')
                else:
                    res[i][p].add(kd)
    return res


def raw_evidence(a, b, body3, t4, e4, prefixes):
    """classify a slot-free object body: ('str', prefix) / ('code', op pairs) / ('blob',) or None"""
    s = as_string(body3)
    if s is not None:
        for p in [b''] + prefixes:
            if s.startswith(p) and b.d[t4:t4 + len(s) - len(p)] == s[len(p):] and not any(b.d[t4 + len(s) - len(p):e4]):
                return ('str', p, len(s) - len(p))
        # find a new prefix: PS4 holds a suffix of the PS3 string
        for k in range(1, len(s) - 1):
            if s[k - 1:k] == b'/' and b.d[t4:t4 + len(s) - k] == s[k:] and not any(b.d[t4 + len(s) - k:e4]):
                return ('str', s[:k], len(s) - k)
        z = b.d.find(b'\0', t4)                       # PS4 has another text here (retail debug strings differ)
        if 0 <= z < e4 and not any(b.d[z:e4]) and all(32 <= c < 127 for c in b.d[t4:z]):
            return ('str', None, z + 1 - t4)
        return None
    n = len(body3)
    if n % 4 == 0 and n and e4 - t4 >= n:
        g = b.d[t4:t4 + n]
        if g == body3 and not any(b.d[t4 + n:e4]): return ('blob', n)
        if all(g[k + 1:k + 4] == body3[k + 1:k + 4] for k in range(0, n, 4)) and not any(b.d[t4 + n:e4]):
            ops = [(body3[k], g[k]) for k in range(0, n, 4)]
            if any(x != y for x, y in ops) or all(body3[k + 1:k + 4] != b'\0\0\0' for k in range(0, n, 4)):
                return ('code', ops, n)
    elif n and e4 - t4 >= n and b.d[t4:t4 + n] == body3 and not any(b.d[t4 + n:e4]):
        return ('blob', n)
    return None


DEL = -(1 << 30)          # position of a PS3 word the PS4 file does not have
ZP = -(1 << 29)           # vote key: instances where the word was zero and kept


def align_object(a, b, M, ob, e4, prefixes, allow_del=False):
    """layout evidence of one object (or None): dict(s4, e4, end4, kind, units / raw info)"""
    s3, e3 = region(ob); hdr = ob[2]
    t4 = M.get(ob[0])
    if t4 is None or e4 is None: return None
    s4 = t4 - 8 if hdr is not None else t4
    if e4 < t4: return None
    units = []
    o = s3
    if hdr is not None:
        if b.d[s4:s4 + 8] != b'\0\0\0\0' + struct.pack('<I', hdr): return None
        units.append((0, {0: {'T'}})); o += 4
    slots = [x for x in a.dslots if o <= x < e3]
    body3 = a.d[o:e3]
    if not slots:
        r = raw_evidence(a, b, body3, t4, e4, prefixes)
        if r is not None and (r[0] != 'blob' or (len(body3) % 4) or o % 4):
            return dict(s4=s4, e4=e4, kind=r[0], raw=r, end4=t4 + r[-1], units=units)
    if (e3 - o) % 4 or o % 4:
        r = raw_evidence(a, b, body3, t4, e4, prefixes) if not slots else None
        if r is None: return None
        return dict(s4=s4, e4=e4, kind=r[0], raw=r, end4=t4 + r[-1], units=units)
    pos = o; p4 = t4
    for mk in slots + [e3]:
        W = [a.d[x:x + 4] for x in range(pos, mk, 4)]
        end4 = e4 if mk == e3 else M.get(mk)
        if end4 is None or end4 < p4: return None
        G = b.d[p4:end4]
        if W:
            tm = None
            if mk == e3:                        # trailing padding is bounded by the next object's alignment
                tm = 4 if end4 % 8 else 8 if end4 % 16 else 16
            f = seg_feasible(W, G, p4, tm)
            ndel = 0
            if f is None and mk == e3 and allow_del:
                # the old cloud converter dropped trailing zero words (the lost option-default slot): learn it
                k = len(W)
                while k and W[k - 1] == b'\0\0\0\0': k -= 1
                for keep in range(len(W) - 1, k - 1, -1):
                    f = seg_feasible(W[:keep], G, p4, tm)
                    if f is not None: ndel = len(W) - keep; break
            if f is None:
                if not slots and mk == e3:
                    r = raw_evidence(a, b, body3, t4, e4, prefixes)
                    if r is not None: return dict(s4=s4, e4=e4, kind=r[0], raw=r, end4=t4 + r[-1], units=units)
                return None
            for i, d in enumerate(f):
                units.append((pos - s3 + 4 * i, {p4 - s4 + r: ks for r, ks in d.items()}))
            for i in range(len(f), len(f) + ndel):
                units.append((pos - s3 + 4 * i, {DEL: {'D'}}))
        elif any(G):
            return None
        if mk == e3: break
        if end4 % 8: return None
        units.append((mk - s3, {end4 - s4: {'P'}}))
        pos = mk + 4; p4 = end4 + 8
    last = 0
    for rel3, d in units:
        r4 = min(d); ks = d[r4]
        if r4 == DEL: continue
        last = max(last, r4 + (8 if ('P' in ks or 'T' in ks) else 4))
    return dict(s4=s4, e4=e4, kind='units', units=units, end4=s4 + last)


# ----------------------------------------------------------------------------------------------- learning


def template_dp(insts, band=256):
    """fixed-size objects whose PS4 form inserts fields PS3 does not have (render settings): align all instances
    at once. insts: [(W words, G bytes)], same len(W) and len(G). A PS3 word matches a PS4 position when one
    transform fits in every instance; a PS4 word that is the same in every instance may be a constant fill;
    a PS3 word may be dropped. Returns (units [(i, p, kind)], fills [(p, bytes)]) or None."""
    import heapq
    n = len(insts[0][0]); L = len(insts[0][1])
    if L % 4: return None
    sigG = [tuple(g[p:p + 4] for _, g in insts) for p in range(0, L, 4)]
    const = [len(set(s)) == 1 for s in sigG]
    sigW = {kd: [tuple(fn(w[i]) for w, _ in insts) for i in range(n)] for kd, fn in TRANS.items()}
    best = {(0, 0): 0}; back = {}; pq = [(0, 0, 0)]
    while pq:
        c, i, q = heapq.heappop(pq)
        if best.get((i, q), 1 << 60) < c: continue
        if i == n and q == L // 4: break
        moves = []
        if i < n and q < L // 4:
            for kd in 'WHR':
                if sigW[kd][i] == sigG[q]: moves.append((c, i + 1, q + 1, kd)); break
        if q < L // 4 and const[q]: moves.append((c + 1, i, q + 1, 'F'))
        if i < n: moves.append((c + 3, i + 1, q, 'D'))
        for c2, i2, q2, kd in moves:
            if abs(q2 - i2) > band // 4: continue
            if c2 < best.get((i2, q2), 1 << 60):
                best[(i2, q2)] = c2; back[(i2, q2)] = (i, q, kd); heapq.heappush(pq, (c2, i2, q2))
    st = (n, L // 4)
    if st not in back and st != (0, 0): return None
    units, fills = [], []
    while st != (0, 0):
        i, q, kd = back[st]
        if kd == 'F': fills.append((4 * q, sigG[q][0]))
        elif kd == 'D': units.append((i, -1, 'D'))
        else: units.append((i, 4 * q, kd))
        st = (i, q)
    return units[::-1], fills[::-1]


def build_template(fl):
    """array-of-structs template: PS3 element of w words -> PS4 stride S; the last element is S + c bytes.
    fl: [(W words, G bytes, t4, hl)] of failing instances of one key."""
    import math
    hl = fl[0][3]
    fl = [x for x in fl if x[3] == hl]
    g = 0
    for W, G, _, _ in fl: g = math.gcd(g, len(W))
    if not g: return None
    sizes = collections.Counter((len(W), len(G)) for W, G, _, _ in fl)
    if g < 8 and len(sizes) > 1:
        # separate objects of a few fixed sizes (e.g. render-settings elements): one template per size
        grp = {}
        for (nw, nl), cnt in sizes.items():
            insts = [(W, G) for W, G, _, _ in fl if len(W) == nw and len(G) == nl]
            head = lambda W, G: sum(1 for i in range(min(16, len(W))) if sw32(W[i]) == G[4 * i:4 * i + 4])
            good = [x for x in insts if head(*x) >= 12]
            if len(good) * 2 > len(insts): insts = good        # drop instances that are not conversions of this PS3 data
            r = template_dp(insts)
            if r is None: continue
            grp['%d' % nw] = dict(units=[[4 * i, p if kd != 'D' else -1, kd] for i, p, kd in r[0]],
                                  fills=[[p, v.hex()] for p, v in r[1]], size=nl)
        if not grp: return None
        al_ = 16
        for _, _, t4, _ in fl:
            while t4 % al_: al_ //= 2
        return dict(n=len(fl), bad=0, kind='tmpl', align=al_, endround=1, hl=hl, w3=0, groups=grp)
    ks_ = [(len(W) // g, len(G)) for W, G, _, _ in fl]
    kset = sorted(set(k for k, _ in ks_))
    if len(kset) >= 2:
        (k1, l1) = min(ks_); (k2, l2) = max(ks_)
        if (l2 - l1) % (k2 - k1): return None
        Sd = (l2 - l1) // (k2 - k1); c = l1 - k1 * Sd
        if not all(L == c + k * Sd for k, L in ks_): return None
    else:
        k = kset[0]
        if k != 1: return None
        Sd = None; c = None
    inner, last = [], []
    for W, G, _, _ in fl:
        k = len(W) // g
        if Sd is None:
            last.append((W, G)); continue
        for j in range(k - 1): inner.append((W[j * g:(j + 1) * g], G[j * Sd:(j + 1) * Sd]))
        last.append((W[(k - 1) * g:], G[(k - 1) * Sd:]))
    if len({len(G) for _, G in last}) != 1: return None
    rl = template_dp(last)
    if rl is None: return None
    ri = template_dp(inner) if inner else None
    if inner and ri is None: return None
    al_ = 16
    for _, _, t4, _ in fl:
        while t4 % al_: al_ //= 2
    hl4 = 8 if hl else 0
    conv = lambda r: ([[4 * i, p if kd != 'D' else -1, kd] for i, p, kd in r[0]], [[p, v.hex()] for p, v in r[1]])
    ul, fl_ = conv(rl)
    ent = dict(n=len(fl), bad=0, kind='tmpl', align=al_, endround=1, hl=hl, w3=4 * g,
               last=dict(units=ul, fills=fl_, size=len(last[0][1])))
    if ri is not None:
        ui, fi = conv(ri); ent['inner'] = dict(units=ui, fills=fi, size=Sd)
    return ent


def find_period(k, ks):
    """array keys: (o3, p3, p4) when the spans grow linearly in whole elements (o3 = header word)"""
    sp = sorted(k['span'].items())
    if len(sp) < 3: return None
    o3 = 4 if ks.startswith('H') and '.' not in ks else 0
    ns = [n - o3 for n, _ in sp if n - o3 > 0]
    cands = collections.Counter(y - x for x, y in zip(ns, ns[1:]) if y - x >= 8)
    best = None
    for g, _ in cands.most_common(6):
        pts = [((n - o3) // g, s) for n, s in sp if n - o3 > 0 and (n - o3) % g == 0]
        if len(pts) < 3: continue
        (m1, s1), (m2, s2) = pts[0], pts[-1]
        if m2 == m1: continue
        p4 = int(round((s2 - s1) / (m2 - m1) / 4.0)) * 4
        if p4 <= 0 or not all(abs((s - s1) - (m - m1) * p4) <= 8 for m, s in pts): continue
        if best is None or len(pts) > best[0]: best = (len(pts), (o3, g, p4))
    return best[1] if best else None


def fold(U, o3, p3, p4):
    """merge the votes of every array element into the first one"""
    F = {}
    for rel3, (feas, uniq, kc) in U.items():
        if rel3 < o3:
            F[rel3] = [collections.Counter(feas), collections.Counter(uniq), collections.Counter(kc)]; continue
        m = (rel3 - o3) // p3; r = o3 + (rel3 - o3) % p3; sh = m * p4
        v = F.setdefault(r, [collections.Counter(), collections.Counter(), collections.Counter()])
        for x, c in feas.items(): v[0][x if x == DEL else x - sh] += c
        for x, c in uniq.items(): v[1][x if x in (DEL, ZP) else x - sh] += c
        for (x, kd), c in kc.items(): v[2][(x if x == DEL else x - sh, kd)] += c
    return F


class Learner:
    def __init__(self):
        self.K = {}
        self.stats = collections.Counter()
        self.ops = collections.defaultdict(collections.Counter)
        self.prefixes = collections.Counter()
        self.hdrs = collections.Counter(); self.nohdr = collections.Counter(); self.hdrtab = {}; self.fails = {}

    def add_pair(self, d3, d4, allow_del=False):
        a, b = Mod(d3), Mod(d4)
        M = anchors(a, b)
        if M is None: self.stats['pair-graph-differs'] += 1; return False

        def hdr_ok(_, h, t):
            t4 = M.get(t)
            ok = t4 is not None and t4 >= 8 and b.d[t4 - 8:t4] == b'\0\0\0\0' + struct.pack('<I', h)
            if t4 is not None and not ok: self.nohdr[h] += 1
            return ok
        def decide(i, fk, h):
            ok = hdr_ok(None, h, objs_t[i])
            for k_ in {fk, tailkey(fk, 1) or fk}:
                self.hdrtab.setdefault(k_, [0, 0])[0 if ok else 1] += 1
            return ok
        objs_t = sorted(set(v for _, _, v in a.syms) | set(a.ptr(s) for s in a.dslots))
        objs_t = [x for x in objs_t if a.data_start <= x < a.reloc]
        objs, keys, fks = graph(a, decide)
        starts4 = []
        for ob in objs:
            t4 = M.get(ob[0])
            starts4.append(None if t4 is None else (t4 - 8 if ob[2] is not None else t4))
        self.stats['pairs'] += 1
        pref = [p for p, _ in self.prefixes.most_common(8)]
        for i, ob in enumerate(objs):
            if ob[2] is not None: self.hdrs[ob[2]] += 1
            nx = starts4[i + 1] if i + 1 < len(objs) else b.reloc
            if nx is None:
                j = i + 1
                while j < len(objs) and starts4[j] is None: j += 1
                nx = starts4[j] if j < len(objs) else None
                if nx is not None and objs[i][0] in M and nx > M[objs[i][0]]:
                    pass
                nx = None if j != i + 1 else nx
            ev = align_object(a, b, M, ob, nx, pref, allow_del)
            for ks in [x for x in (keys[i], tailkey(keys[i], 2), tailkey(keys[i], 1)) if x]:
                self._one(ks, ev, ob, a, b, M, nx)
        return True

    def _one(self, ks, ev, ob, a, b, M, nx):
        k = self.K.setdefault(ks, dict(n=0, bad=0, kinds=collections.Counter(), units={}, align=64, endr=[], n3max=0, span={}))
        if ev is None:
            self.stats['obj-unaligned'] += 1; k['bad'] += 1
            s3_, e3_ = region(ob); hl = 4 if ob[2] is not None else 0
            t4_ = M.get(ob[0])
            if t4_ is not None and nx is not None and (e3_ - s3_ - hl) % 4 == 0 and (s3_ + hl) % 4 == 0 \
                    and not any(s3_ <= x < e3_ for x in a.dslots[bisect.bisect_left(a.dslots, s3_):bisect.bisect_left(a.dslots, e3_)]):
                fl = self.fails.setdefault(ks, [])
                if len(fl) < 64:
                    fl.append(([a.d[x:x + 4] for x in range(s3_ + hl, e3_, 4)], b.d[t4_:nx], t4_, hl))
            return
        self.stats['obj-ok'] += 1
        k['n'] += 1; k['kinds'][ev['kind']] += 1
        s4 = ev['s4'] + (8 if ob[2] is not None else 0); al_ = 1      # alignment of the object body
        while al_ < 16 and s4 % (al_ * 2) == 0: al_ *= 2
        k['align'] = min(k['align'], al_)
        k['n3max'] = max(k['n3max'], region(ob)[1] - region(ob)[0])
        k['endr'].append((ev['end4'], ev['e4']))
        if ev['kind'] == 'str':
            if ev['raw'][1]: self.prefixes[ev['raw'][1]] += 1
        elif ev['kind'] == 'code':
            for x, y in ev['raw'][1]: self.ops[x][y] += 1
        elif ev['kind'] == 'units':
            s3, e3 = region(ob)
            k['span'][e3 - s3] = min(k['span'].get(e3 - s3, 1 << 30), ev['e4'] - ev['s4'])
            shape = '%x:' % (e3 - s3) + ','.join('%x' % (x - s3) for x in a.dslots[bisect.bisect_left(a.dslots, s3):bisect.bisect_left(a.dslots, e3)])
            sh = k.setdefault('shapes', {}).setdefault(shape, {})
            for U in (k['units'], sh):
                for rel3, d in ev['units']:
                    v = U.setdefault(rel3, [collections.Counter(), collections.Counter(), collections.Counter()])
                    for r, kset in d.items():
                        v[0][r] += 1                      # feasible in this instance
                        if len(d) == 1: v[1][r] += 1      # the only position in this instance
                        for kk in kset: v[2][(r, kk)] += 1
                    allk = set().union(*d.values())
                    if 'D' not in allk and {'W', 'R'} <= allk: v[1][ZP] += 1   # a zero word that was kept
        if ev['kind'] != 'units':
            s3, e3 = region(ob)
            k['span'][e3 - s3] = min(k['span'].get(e3 - s3, 1 << 30), ev['e4'] - ev['s4'])

    def finish(self):
        S = {}
        for ks, k in self.K.items():
            if k['n'] == 0: continue
            kind = k['kinds'].most_common(1)[0][0]
            if kind == 'blob' and k['kinds'].get('units'): kind = 'units'
            ent = dict(n=k['n'], bad=k['bad'], align=k['align'], kind=kind, n3max=k['n3max'])
            if kind == 'units':
                def fin(U, prior=None):
                    units = []
                    pr = {u[0]: u[1] for u in prior} if prior else {}
                    for rel3 in sorted(U):
                        feas, uniq, kc = U[rel3]
                        nd = feas.get(DEL, 0)
                        cand = [r for r in feas if r not in (DEL, ZP)]
                        if not cand: units.append([rel3, 0, 'W', 1]); continue
                        r4 = max(cand, key=lambda r: (feas[r], uniq[r], pr.get(rel3) == r, -r))
                        kinds = {kk: c for (r, kk), c in kc.items() if r == r4}
                        pri = 'PTWHRQq'
                        kd = max(kinds, key=lambda c: (c in 'PT', kinds[c], -pri.index(c))) if kinds else 'W'
                        units.append([rel3, r4, kd] + ([1] if nd and 2 * nd >= uniq.get(ZP, 0) else []))
                    # a u64 half without its partner becomes a plain word
                    for i, u in enumerate(units):
                        if u[2] == 'Q' and not (i + 1 < len(units) and units[i + 1][0] == u[0] + 4 and units[i + 1][1] == u[1] + 4):
                            u[2] = 'W'
                        if u[2] == 'q' and not (i and units[i - 1][2] == 'Q'): u[2] = 'W'
                        if u[2] == 'Q' and i + 1 < len(units): units[i + 1][2] = 'q'
                    return units
                per = find_period(k, ks)
                if per:
                    o3, p3, p4 = per
                    ent['period'] = [o3, p3, p4]
                    ent['units'] = fin(fold(k['units'], o3, p3, p4))
                else:
                    ent['units'] = fin(k['units'])
                shp = {}
                full = ent['units']
                if per:
                    o3, p3, p4 = per; base = {u[0]: u for u in full}; full = [u for u in full if u[0] < o3]
                    for r in range(o3, k['n3max'], 4):
                        v = base.get(o3 + (r - o3) % p3)
                        if v: full.append([r, v[1] + (r - o3) // p3 * p4, v[2]])
                for shape, U in k.get('shapes', {}).items():
                    u = fin(U, full)
                    if u != full[:len(u)]: shp[shape] = u
                if shp: ent['shapes'] = shp
            if k['span']: ent['span'] = {'%x' % n3: v for n3, v in sorted(k['span'].items())}
            er = 1
            for c in (2, 4, 8, 16):
                if all(al(e, c) <= nx for e, nx in k['endr']): er = c
                else: break
            ent['endround'] = er
            S[ks] = ent
        for ks, fl in self.fails.items():
            if ks in S and S[ks]['n'] >= len(fl): continue
            if '~' in ks or len(fl) < 2: continue                 # templates only for full keys seen twice
            r = build_template(fl)
            if r is None: self.stats['template-failed'] += 1; continue
            S[ks] = r
            self.stats['templates'] += 1
        opmap = {}
        for x, c in self.ops.items():
            y, n = c.most_common(1)[0]
            opmap['%d' % x] = y
        return dict(keys=S, opmap=opmap, prefixes=[p.decode('latin1') for p, _ in self.prefixes.most_common()],
                    hdrs=sorted(h for h, n in self.hdrs.items() if n >= 2 and n >= 4 * self.nohdr.get(h, 0)),
                    hdrtab={k: v for k, v in self.hdrtab.items() if v[0] != v[1]})


# ----------------------------------------------------------------------------------------------- conversion


def al(x, a): return (x + a - 1) // a * a


def periodic_units(units, n3):
    """extend a learned unit list to cover n3 PS3 bytes by repeating its tail period"""
    if not units: return None
    have = units[-1][0] + 4
    idx = {u[0]: u for u in units}
    for p in range(4, have, 4):
        ok = True; d4 = None; seen = 0
        for u in units:
            if u[0] < have - 2 * p: continue
            v = idx.get(u[0] - p)
            if v is None: continue
            if v[2] != u[2]: ok = False; break
            dd = u[1] - v[1]
            if d4 is None: d4 = dd
            elif dd != d4: ok = False; break
            seen += 1
        if ok and d4 and seen:
            out = list(units); r = have
            while r < n3:
                v = idx.get(r - p)
                if v is None: return None
                u = [r, v[1] + d4, v[2]]; out.append(u); idx[r] = u; r += 4
            return out
    return None


def generic_units(m, s3, e3, hdr):
    out = []; r4 = 0; o = s3
    if hdr is not None: out.append([0, 0, 'T']); r4 = 8; o += 4
    while o + 4 <= e3:
        if o in m.slots:
            r4 = al(r4, 8); out.append([o - s3, r4, 'P']); r4 += 8
        else:
            out.append([o - s3, r4, 'W']); r4 += 4
        o += 4
    return out


class Converter:
    def __init__(self, schema):
        """schema: one learned schema or a list of them, highest priority first (a key's whole entry comes from the
        first schema that has it; opcode maps, prefixes and header types are merged)"""
        schemas = schema if isinstance(schema, list) else [schema]
        self.S = {}; self.opmap = {}; pref = []; self.hdrs = set(); self.hdrtab = {}
        for s in reversed(schemas):
            self.S.update(s['keys'])
            self.opmap.update({int(k): v for k, v in s.get('opmap', {}).items()})
            pref = [p for p in s.get('prefixes', []) if p not in pref] + pref
            self.hdrs |= set(s.get('hdrs', []))
            self.hdrtab.update(s.get('hdrtab', {}))
        self.prefixes = [p.encode('latin1') for p in pref]

    def plan(self, m, rep):
        def decide(i, fk, h):
            for k_ in (fk, tailkey(fk, 1)):
                v = self.hdrtab.get(k_) if k_ else None
                if v is not None: return v[0] > v[1]
            return h in self.hdrs
        objs, keys, fks = graph(m, decide)
        plans = []
        for i, ob in enumerate(objs):
            s3, e3 = region(ob); ks = keys[i]; ent = self.S.get(ks)
            if ent is None or ent.get('bad', 0) > ent.get('n', 0):
                for g in (tailkey(ks, 2), tailkey(ks, 1)):
                    e2 = self.S.get(g) if g else None
                    if e2 is not None and e2.get('n', 0) >= e2.get('bad', 0): ent = e2; rep['general-key'] += 1; break
            hl = 4 if ob[2] is not None else 0
            body = m.d[s3 + hl:e3]
            has_slot = any(s3 <= x < e3 for x in m.dslots[bisect.bisect_left(m.dslots, s3):bisect.bisect_left(m.dslots, e3)])
            pl = dict(s3=s3, e3=e3, hdr=ob[2], key=ks)
            if ks == 'O' and not any(m.d[s3:e3]):
                rep['lead-pad-dropped'] += 1; continue          # PS3 alignment padding before the first object
            if ent is None:
                rep['key-unknown'] += 1
                rep['unknown-keys'] = rep.get('unknown-keys', 0)
                s = as_string(body) if not has_slot else None
                if s is not None:
                    pl.update(kind='str', align=1, endround=1)
                else:
                    pl.update(kind='units', align=8, endround=8, units=generic_units(m, s3, e3, ob[2]))
                plans.append(pl); continue
            pl.update(align=ent['align'], endround=ent['endround'], span=ent.get('span', {}).get('%x' % (e3 - s3)))
            kind = ent['kind']
            if kind in ('str', 'code', 'blob') and not has_slot:
                if kind == 'str' and as_string(body) is None: kind = 'blob'
                if kind == 'code' and as_string(body) is not None: kind = 'str'
                pl['kind'] = kind; plans.append(pl); continue
            if kind == 'tmpl' and 'hl' not in ent: kind = 'units'
            if kind == 'tmpl':
                nb = e3 - s3 - ent['hl']; w3 = ent['w3']
                gp = ent.get('groups', {}).get('%d' % (nb // 4))
                if gp is not None and ent['hl'] == hl:
                    hl4 = 8 if hl else 0
                    units = ([[0, 0, 'T']] if hl else []) + [[hl + u[0], (hl4 + u[1]) if u[2] != 'D' else -1, u[2]] for u in gp['units']]
                    fills = [[hl4 + p, v] for p, v in gp['fills']]
                    pl.update(kind='tmpl', units=units, fills=fills, size4=hl4 + gp['size']); plans.append(pl); continue
                if w3 and ent['hl'] == hl and nb > 0 and nb % w3 == 0 and (nb // w3 == 1 or 'inner' in ent):
                    k = nb // w3; units = [[0, 0, 'T']] if hl else []; fills = []; hl4 = 8 if hl else 0; o4 = hl4
                    for j in range(k):
                        part = ent['inner'] if j < k - 1 else ent['last']
                        units += [[hl + j * w3 + u[0], (o4 + u[1]) if u[2] != 'D' else -1, u[2]] for u in part['units']]
                        fills += [[o4 + p, v] for p, v in part['fills']]
                        o4 += part['size']
                    pl.update(kind='tmpl', units=units, fills=fills, size4=o4); plans.append(pl); continue
                rep['template-size-differs'] += 1
            units = ent.get('units') or []
            n3 = e3 - s3
            if ent.get('shapes'):
                shape = '%x:' % n3 + ','.join('%x' % (x - s3) for x in m.dslots[bisect.bisect_left(m.dslots, s3):bisect.bisect_left(m.dslots, e3)])
                if shape in ent['shapes']: units = ent['shapes'][shape]; rep['shape-hit'] += 1
            have = {u[0] for u in units}
            if ent.get('period') and not all(r in have for r in range(0, n3 - 3, 4)):
                o3, p3, p4 = ent['period']
                base = {u[0]: u for u in units}
                ext = [u for u in units if u[0] < o3]
                for r in range(o3, n3 - 3, 4):
                    m_ = (r - o3) // p3; v = base.get(o3 + (r - o3) % p3)
                    if v is None: ext = None; break
                    ext.append([r, v[1] + m_ * p4, v[2]])
                if ext is not None: units = ext; have = {u[0] for u in units}; rep['period-used'] += 1
            if not all(r in have for r in range(0, n3 - 3, 4)):
                ext = periodic_units(units, n3)
                if ext is None or not all(r in {u[0] for u in ext} for r in range(0, n3 - 3, 4)):
                    rep['layout-extend-failed'] += 1; ext = generic_units(m, s3, e3, ob[2])
                else:
                    rep['layout-extended'] += 1
                units = ext
            units = [u for u in units if u[0] < n3]
            if any((s3 + u[0]) in m.slots and u[2] != 'P' for u in units):
                rep['slot-not-ptr'] += 1; units = generic_units(m, s3, e3, ob[2])
            if any(u[2] in 'PQ' and u[1] % 8 == 0 for u in units):
                pl['align'] = max(pl['align'], 8)
            pl.update(kind='units', units=units); plans.append(pl)
        return plans

    def convert(self, d3, rep=None):
        m = Mod(d3)
        if not m.be: raise ValueError('input is not a PS3 module')
        rep = rep if rep is not None else collections.Counter()
        plans = self.plan(m, rep); self.last_plans = plans
        n = len(m.syms)
        prev_end = 0x18 + 16 * n
        M = {}
        for pl in plans:
            hd = 8 if pl['hdr'] is not None else 0
            s4 = al(prev_end + hd, pl['align']) - hd; pl['s4'] = s4
            hl3 = 4 if pl['hdr'] is not None else 0; hl4 = 8 if pl['hdr'] is not None else 0
            body = m.d[pl['s3'] + hl3:pl['e3']]
            if pl['kind'] == 'str':
                s = strip_prefix(body, self.prefixes); pl['bytes'] = s      # PS3 zero padding is kept (old converter)
                end = s4 + hl4 + len(s)
            elif pl['kind'] == 'code':
                c = bytearray(body)
                for k in range(0, len(c) - 3, 4):
                    c[k] = self.opmap.get(c[k], c[k])
                pl['bytes'] = bytes(c); end = s4 + hl4 + len(c)
            elif pl['kind'] == 'blob':
                pl['bytes'] = body; end = s4 + hl4 + len(body)
            elif pl['kind'] == 'tmpl':
                end = s4 + pl['size4']
            else:
                end = s4 + max([u[1] + (8 if u[2] in ('P', 'T', 'Q') else 4) for u in pl['units']
                                if not (len(u) > 3 and m.d[pl['s3'] + u[0]:pl['s3'] + u[0] + 4] == b'\0\0\0\0')] or [hl4])
            M[pl['s3'] + hl3] = s4 + hl4
            pl['end'] = end
            sp = pl.get('span')
            mode = os.environ.get('DCCONV_END', 'span')
            prev_end = end if mode == 'none' else al(end, pl['endround'])
            if mode == 'span' and pl['kind'] != 'str' and sp is not None and s4 + sp > prev_end:
                prev_end = s4 + sp
        reloc = al(prev_end, 8)
        out = bytearray(reloc)
        out[0:4] = b'00CD'
        struct.pack_into('<IQII', out, 4, 1, reloc, struct.unpack_from('>I', m.d, 0x10)[0], n)
        slots4 = []
        for i, (s, t, v) in enumerate(m.syms):
            o = 0x18 + 16 * i
            if v not in M: rep['sym-target-missing'] += 1
            struct.pack_into('<IIQ', out, o, s, t, M.get(v, 0)); slots4.append(o + 8)
        for pl in plans:
            s3, s4 = pl['s3'], pl['s4']
            if pl['hdr'] is not None and pl['kind'] != 'units':
                struct.pack_into('<II', out, s4, 0, pl['hdr'])
            if pl['kind'] == 'tmpl':
                for p, v in pl['fills']: out[s4 + p:s4 + p + 4] = bytes.fromhex(v)
            if pl['kind'] not in ('units', 'tmpl'):
                hl4 = 8 if pl['hdr'] is not None else 0
                out[s4 + hl4:s4 + hl4 + len(pl['bytes'])] = pl['bytes']; continue
            for u in pl['units']:
                rel3, rel4, kd = u[:3]
                o3 = s3 + rel3; o4 = s4 + rel4; w = m.d[o3:o3 + 4]
                if len(u) > 3 and w == b'\0\0\0\0': continue      # zero word the old converter dropped
                if kd == 'D': continue
                if kd == 'T':
                    struct.pack_into('<II', out, o4, 0, struct.unpack('>I', w)[0])
                elif kd == 'P':
                    if o3 in m.slots:
                        t4 = M.get(m.ptr(o3))
                        if t4 is None: rep['ptr-target-missing'] += 1; t4 = 0
                        struct.pack_into('<Q', out, o4, t4); slots4.append(o4)
                    else:
                        struct.pack_into('<Q', out, o4, struct.unpack('>I', w)[0])
                elif kd == 'Q':
                    out[o4:o4 + 8] = m.d[o3:o3 + 8][::-1]
                elif kd == 'q':
                    pass
                else:
                    out[o4:o4 + 4] = TRANS.get(kd, sw32)(w)
        nb = (reloc + 63) // 64
        bm = bytearray(nb)
        for p in slots4:
            w = p // 8; bm[w // 8] |= 1 << (w % 8)
        out += struct.pack('<I', nb) + bm
        return bytes(out), rep


# ----------------------------------------------------------------------------------------------- CLI


def read_pairs(p):
    for line in open(p, encoding='utf-8'):
        line = line.rstrip('\n')
        if line and not line.startswith('#'):
            yield line.split('\t')[:2]


def learn(pairs, out=None, log=print, allow_del=False):
    L = Learner()
    for p3, p4 in pairs:
        try: L.add_pair(open(p3, 'rb').read(), open(p4, 'rb').read(), allow_del)
        except Exception as ex: L.stats['pair-error'] += 1; log('error %s %s' % (p3, ex))
    S = L.finish(); S['version'] = 2; S['pairs'] = len(pairs); S['stats'] = dict(L.stats)
    if out: json.dump(S, open(out, 'w'), separators=(',', ':'))
    log('learned %d keys from %d pairs %s' % (len(S['keys']), len(pairs), dict(L.stats)))
    return S


def first_diff(a, b):
    n = min(len(a), len(b))
    return next((i for i in range(n) if a[i] != b[i]), n)


def main(argv):
    if len(argv) < 2: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'learn':
        learn(list(read_pairs(argv[2])), argv[3], allow_del='--old-converter' in argv); return 0
    if cmd == 'convert':
        C = Converter([json.load(open(x)) for x in argv[2].split(',')])
        d, rep = C.convert(open(argv[3], 'rb').read())
        open(argv[4], 'wb').write(d); print('wrote', argv[4], len(d), dict(rep)); return 0
    if cmd == 'test':
        C = Converter([json.load(open(x)) for x in argv[2].split(',')]); tot = collections.Counter()
        for p3, p4 in read_pairs(argv[3]):
            rep = collections.Counter()
            try: d, rep = C.convert(open(p3, 'rb').read(), rep)
            except Exception as ex: print('ERROR', p3, repr(ex)); tot['error'] += 1; continue
            want = open(p4, 'rb').read()
            tot['identical' if d == want else 'different'] += 1
            if d != want:
                print('DIFF %-50s %7d %7d first@%x %s' % (os.path.basename(p4), len(d), len(want), first_diff(d, want), dict(rep)))
        print(dict(tot)); return 0
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
