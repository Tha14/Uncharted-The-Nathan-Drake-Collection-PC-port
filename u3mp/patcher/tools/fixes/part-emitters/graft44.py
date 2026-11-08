"""graft44: copy PS4 particle effects (symbol + full object closure) from one PS4 dc2 particle module into another.
No format conversion: source and destination are both retail PS4 data. Objects = spans between pointer targets (with the
8-byte {0, type} header in front when present); pointers inside copied spans are remapped; the symbol table is rebuilt
sorted (dc2lib.build; the retail files round-trip byte-exact through dc2lib).
part-emitters copy (batch45 gompa water effects): works on bytes. graft(dst_bytes, items, sources) with
items = [(sid, source name)] and sources = {source name: dc2 bytes}; returns the new dc2 bytes. The algorithm is the
batch45 one unchanged (sources are grafted in the order they first appear in items)."""
import os, sys, struct, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dc2lib


class Mod:
    def __init__(self, path_or_bytes):
        self.c = dc2lib.load(path_or_bytes)
        c = self.c
        self.T = sorted(set(c.ptr(p) for p in c.ptrs if c.data0 <= c.ptr(p) < c.reloc) | {o for _, _, o in c.syms})
    def end(self, t):
        k = bisect.bisect_right(self.T, t); return self.T[k] if k < len(self.T) else self.c.reloc
    def start(self, t):
        k = bisect.bisect_right(self.T, t) - 1; return self.T[k]
    def hdr(self, t):
        c = self.c
        return 8 if (t % 8 == 0 and t >= 8 and (t - 8) not in c.ptrs and (t - 4) not in c.ptrs and c.u32(t - 4) and not any(c.d[t - 8:t - 4])) else 0
    def closure(self, roots):
        c = self.c; seen = set(); st = [self.start(r) for r in roots]
        while st:
            t = st.pop()
            if t in seen or t >= c.reloc: continue
            seen.add(t)
            for q in range(t, self.end(t)):
                if q in c.ptrs:
                    x = c.ptr(q)
                    if x < c.reloc: st.append(self.start(x))
        return sorted(seen)


def graft(dst, items, sources, log=None):
    a = Mod(dst).c
    d = bytearray(a.d[:a.reloc]); ptrs = {p for p in a.ptrs if p >= a.data0}; syms = list(a.syms)
    have = {s for s, _, _ in syms}
    bysrc = {}
    for s, p in items: bysrc.setdefault(p, []).append(s)
    for p, sids in bysrc.items():
        m = Mod(sources[p]); c = m.c
        sids = [s for s in sids if s not in have]
        roots = [c.bysid[s][1] for s in sids]
        spans = m.closure(roots); newaddr = {}
        for t in spans:
            h = m.hdr(t); lo = t - h; e = m.end(t)
            while len(d) % 16 != lo % 16: d.append(0)
            base = len(d); d += c.d[lo:e]; newaddr[t] = base + h
        starts = sorted(newaddr)
        def remap(x):
            k = bisect.bisect_right(starts, x) - 1
            assert k >= 0 and x < m.end(starts[k]), hex(x)
            return newaddr[starts[k]] + (x - starts[k])
        for t in spans:
            h = m.hdr(t); e = m.end(t); nb = newaddr[t]
            for q in range(t - h, e):
                if q in c.ptrs:
                    x = c.ptr(q)
                    assert x < c.reloc, ('ptr past reloc', hex(q))
                    struct.pack_into('<Q', d, nb + (q - t), remap(x)); ptrs.add(nb + (q - t))
        for s in sids:
            ty, o = c.bysid[s]; syms.append((s, ty, remap(o))); have.add(s)
        if log is not None: log.append('graft44 from %s: %d symbols, %d objects' % (p, len(sids), len(spans)))
    return dc2lib.build(d, ptrs, syms, len(a.syms))
