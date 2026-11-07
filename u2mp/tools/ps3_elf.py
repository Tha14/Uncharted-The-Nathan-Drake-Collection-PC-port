"""ps3_elf.py - read-only static analysis helpers for a decrypted PS3 PPU EBOOT.elf
(PowerPC64 big-endian, 32-bit pointers, OPD/TOC descriptors).

Used by ps3_map.py. Nothing here writes to the game files; caches go to an output
directory chosen by the caller (default: <repo>/work/ps3-110, which is git-ignored).

What it builds:
  - LOAD segments, vaddr <-> file offset
  - import libraries from .lib.stub (sceLibStub, 0x2c bytes each), their NIDs and
    the stub code addresses that jump through each function slot; NIDs are named by
    hashing candidate names (SHA-1(name + suffix), first 4 bytes, big-endian)
  - function starts: OPD entries + direct bl targets
  - a linear decode of the text with simple per-function constant propagation
    (lis/addis/addi/ori/lwz/mr), giving: call sites (bl), computed addresses
    (string/data/OPD references), 32-bit immediates (string ids), and memory
    accesses with a resolved base (globals)
"""
import bisect, hashlib, os, pickle, re, struct

NID_SUFFIX = bytes.fromhex('6759659904250490566427499489741A')


def nid_of(name):
    return struct.unpack('<I', hashlib.sha1(name.encode() + NID_SUFFIX).digest()[:4])[0]


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


class Elf:
    def __init__(self, path):
        self.path = path
        self.d = open(path, 'rb').read()
        d = self.d
        assert d[:4] == b'\x7fELF' and d[4] == 2 and d[5] == 2, 'need ELF64 big-endian'
        self.entry = struct.unpack_from('>Q', d, 0x18)[0]
        phoff, = struct.unpack_from('>Q', d, 0x20)
        shoff, = struct.unpack_from('>Q', d, 0x28)
        phnum, = struct.unpack_from('>H', d, 0x38)
        shnum, = struct.unpack_from('>H', d, 0x3c)
        self.segs = []
        for i in range(phnum):
            t, fl, off, va, pa, fs, ms, al = struct.unpack_from('>IIQQQQQQ', d, phoff + i * 56)
            if t == 1 and fs:
                self.segs.append((va, off, fs, ms, fl))
        self.secs = []
        for i in range(shnum):
            nm, t, fl, va, off, sz = struct.unpack_from('>IIQQQQ', d, shoff + i * 64)
            self.secs.append((i, t, fl, va, off, sz))

    def off(self, va):
        for s, o, fs, ms, fl in self.segs:
            if s <= va < s + fs:
                return o + va - s
        return None

    def u32(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from('>I', self.d, o)[0]

    def bytes(self, va, n):
        o = self.off(va)
        return None if o is None else self.d[o:o + n]

    def cstr(self, va, maxlen=200):
        o = self.off(va)
        if o is None:
            return None
        e = self.d.find(b'\0', o, o + maxlen)
        if e < 0:
            return None
        s = self.d[o:e]
        if len(s) < 1 or any(c < 0x20 or c > 0x7e for c in s if c not in (9, 10)):
            return None
        return s.decode('latin1')

    def text_range(self):
        # largest executable section
        best = max((s for s in self.secs if s[2] & 4), key=lambda s: s[5])
        return best[3], best[3] + best[5]

    def sec_containing(self, va):
        for s in self.secs:
            if s[1] == 1 and s[3] <= va < s[3] + s[5]:
                return s
        return None


# ---------------------------------------------------------------- imports
def parse_imports(elf, names):
    """names: dict nid -> name. Returns list of libs:
    {'lib','stub_entry','funcs':[{'nid','name','slot','stub'}]}"""
    # find .lib.stub: section whose entries look like sceLibStub (first byte 0x2c)
    libs = []
    cand = None
    for s in elf.secs:
        i, t, fl, va, off, sz = s
        if t == 1 and sz and sz % 0x2c == 0 and elf.d[off] == 0x2c and elf.d[off + 0x2c if sz > 0x2c else off] == 0x2c:
            ok = True
            for k in range(0, sz, 0x2c):
                if elf.d[off + k] != 0x2c:
                    ok = False
                    break
            if ok:
                cand = s
                break
    if not cand:
        return libs
    _, _, _, va, off, sz = cand
    slot2fn = {}
    for k in range(0, sz, 0x2c):
        e = elf.d[off + k: off + k + 0x2c]
        nfunc, = struct.unpack_from('>H', e, 6)
        nm_p, fnid_p, ftab_p = struct.unpack_from('>III', e, 0x10)
        lib = {'lib': elf.cstr(nm_p) or '?', 'stub_entry': va + k, 'funcs': []}
        for j in range(nfunc):
            nid = elf.u32(fnid_p + 4 * j)
            f = {'nid': nid, 'name': names.get(nid, 'nid_%08x' % nid), 'slot': ftab_p + 4 * j, 'stub': None}
            lib['funcs'].append(f)
            slot2fn[ftab_p + 4 * j] = f
        libs.append(lib)
    # stub code: li/lis r12; oris/addis r12 hi; lwz r12, lo(r12) -> slot; ... bctr
    for s in elf.secs:
        i, t, fl, sva, soff, ssz = s
        if not (fl & 4) or ssz > 0x100000:
            continue
        for p in range(0, ssz - 8, 4):
            w0, w1, w2 = struct.unpack_from('>III', elf.d, soff + p)
            # li r12,0 ; oris r12,r12,hi ; lwz r12,lo(r12)
            if w0 >> 16 == 0x3980 and (w1 >> 16) == 0x658c and (w2 >> 16) == 0x818c:
                slot = ((w1 & 0xffff) << 16) + sx16(w2 & 0xffff) + sx16(w0 & 0xffff)
                slot &= 0xffffffff
                if slot in slot2fn:
                    slot2fn[slot]['stub'] = sva + p
            elif w0 >> 16 == 0x3d80 and (w1 >> 16) == 0x818c:  # lis r12,hi ; lwz r12,lo(r12)
                slot = (((w0 & 0xffff) << 16) + sx16(w1 & 0xffff)) & 0xffffffff
                if slot in slot2fn and slot2fn[slot]['stub'] is None:
                    slot2fn[slot]['stub'] = sva + p
    return libs


def nid_names_from_binary(path, extra=()):
    """Collect identifier-like strings from a binary (e.g. rpcs3.exe, which registers
    every HLE function by name) and hash them into a nid -> name map."""
    data = open(path, 'rb').read() if path else b''
    names = set(extra)
    for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_]{2,63}', data):
        names.add(m.group().decode())
    return {nid_of(n): n for n in names}


# ---------------------------------------------------------------- decode
LOGICAL31 = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 58, 922, 954, 986, 27, 539, 794,
             826, 827}
NOWRITE31 = {0, 32, 4, 68, 151, 215, 407, 183, 247, 439, 663, 695, 727, 759, 983, 149, 181, 662, 918, 231,
             135, 167, 199, 487, 391, 455, 519, 647, 54, 86, 246, 278, 1014, 598, 467, 144, 854, 150, 214,
             982, 966, 0x3ac}
UPDATE31 = {183, 247, 439, 695, 759, 181, 55, 119, 311, 375, 567, 631, 53, 373}


class Analysis:
    VERSION = 6

    def __init__(self, elf):
        self.elf = elf

    def build(self):
        e = self.elf
        t0, t1 = self.text_lo, self.text_hi = e.text_range()
        # OPD: section of (func,toc) pairs where func in text and toc constant
        self.opd = {}
        self.toc = None
        for s in e.secs:
            i, t, fl, va, off, sz = s
            if t != 1 or sz < 64 or sz % 8:
                continue
            good = 0
            tocs = {}
            for k in range(0, min(sz, 800), 8):
                f, tc = struct.unpack_from('>II', e.d, off + k)
                if t0 <= f < t1:
                    good += 1
                    tocs[tc] = tocs.get(tc, 0) + 1
            if good >= 90:
                self.toc = max(tocs, key=tocs.get)
                for k in range(0, sz, 8):
                    f, tc = struct.unpack_from('>II', e.d, off + k)
                    if t0 <= f < t1:
                        self.opd[va + k] = f
                self.opd_range = (va, va + sz)
                break
        starts = set(self.opd.values())
        o0 = e.off(t0)
        n = (t1 - t0) // 4
        words = struct.unpack_from('>%dI' % n, e.d, o0)
        calls = []      # (site, target)
        for idx, w in enumerate(words):
            if w >> 26 == 18 and (w & 3) == 1:
                li = w & 0x03fffffc
                if li & 0x02000000:
                    li -= 0x04000000
                tgt = t0 + idx * 4 + li
                calls.append((t0 + idx * 4, tgt))
                starts.add(tgt)
        self.starts = sorted(x for x in starts if t0 <= x < t1)
        self.calls = calls
        # constant propagation
        refs = []       # (site, value, kind) kind: 'a' addr computed, 'm' mem access base+d, 'k' 32-bit const
        icalls = []     # (site, opd) indirect call via function descriptor loaded from known address
        si = 0
        nxt = self.starts[0] if self.starts else t1
        regs = {}
        ctr = None
        r12 = None
        start_set = set(self.starts)
        TOC = self.toc
        for idx, w in enumerate(words):
            a = t0 + idx * 4
            if a in start_set:
                regs = {2: TOC}
                ctr = None
            op = w >> 26
            if op == 18:
                if w & 1:
                    for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                        regs.pop(r, None)
                    regs[2] = TOC
                continue
            rd = (w >> 21) & 31
            ra = (w >> 16) & 31
            imm = w & 0xffff
            if op == 15:   # addis
                if ra == 0:
                    v = (imm << 16) & 0xffffffff
                    regs[rd] = v
                elif ra in regs:
                    regs[rd] = (regs[ra] + (sx16(imm) << 16)) & 0xffffffff
                else:
                    regs.pop(rd, None)
            elif op == 14:  # addi
                if ra == 0:
                    regs[rd] = sx16(imm) & 0xffffffff
                elif ra in regs:
                    v = (regs[ra] + sx16(imm)) & 0xffffffff
                    regs[rd] = v
                    refs.append((a, v, 'a'))
                else:
                    regs.pop(rd, None)
            elif op == 24 or op == 25:  # ori/oris rA = rS | imm
                rs, rA = rd, ra
                if rs in regs:
                    v = regs[rs] | (imm if op == 24 else imm << 16)
                    regs[rA] = v
                    if v > 0xffff and (op == 24 or v & 0xffff):
                        refs.append((a, v, 'k'))
                else:
                    regs.pop(rA, None)
            elif op in (32, 34, 40, 42, 48, 50):  # lwz lbz lhz lha lfs lfd
                if ra != 0 and ra in regs:
                    ea = (regs[ra] + sx16(imm)) & 0xffffffff
                    refs.append((a, ea, 'm'))
                    if op == 32:
                        v = e.u32(ea) if (ea < 0xd60000 or self._ro(ea)) else None
                        if v is not None:
                            regs[rd] = v
                            refs.append((a, v, 'a'))
                        else:
                            regs.pop(rd, None)
                    elif op in (34, 40, 42):
                        regs.pop(rd, None)
                elif op in (32, 34, 40, 42):
                    regs.pop(rd, None)
            elif op in (36, 38, 44, 52, 54, 37, 33):  # stw stb sth stfs stfd stwu lwzu
                if ra != 0 and ra in regs:
                    refs.append((a, (regs[ra] + sx16(imm)) & 0xffffffff, 'm'))
                if op == 33:
                    regs.pop(rd, None)
                if op in (37, 33):
                    regs.pop(ra, None)
            elif op == 31:
                xo = (w >> 1) & 0x3ff
                rb = (w >> 11) & 31
                if xo == 444:  # or rA,rS,rB
                    if rd == rb and rd in regs:
                        regs[ra] = regs[rd]
                    else:
                        regs.pop(ra, None)
                elif xo == 467 and ((w >> 11) & 0x3ff) == 0x120:  # mtctr
                    ctr = regs.get(rd)
                elif xo in LOGICAL31:       # write rA, rS is a source
                    regs.pop(ra, None)
                elif xo in NOWRITE31:       # compares, stores, cache ops, traps
                    if xo in UPDATE31:
                        regs.pop(ra, None)
                else:                       # arithmetic, loads, mfspr: write rD
                    regs.pop(rd, None)
                    if xo in UPDATE31:
                        regs.pop(ra, None)
            elif op == 19:
                xo = (w >> 1) & 0x3ff
                if xo == 528 and (w & 1):  # bctrl
                    if ctr is not None:
                        icalls.append((a, ctr))
                    for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                        regs.pop(r, None)
                    regs[2] = TOC
            elif op in (58,):  # ld
                if rd == 2:
                    regs[2] = TOC
                elif ra == 1 and rd >= 14:
                    pass    # epilogue restore of a callee-saved register; code after blr keeps the prologue values
                else:
                    regs.pop(rd, None)
            elif op in (62,):
                pass
            elif op in (20, 21, 23, 26, 27, 28, 29, 7, 8, 12, 13, 30):
                # rlwimi/rlwinm/rlwnm/andi./andis. write rA ; mulli/subfic/addic write rD; rld* write rA
                if op in (20, 21, 23, 26, 27, 28, 29, 30):
                    regs.pop(ra, None)
                else:
                    regs.pop(rd, None)
            elif op in (39, 45, 53, 55):    # stores with update
                regs.pop(ra, None)
            elif op in (35, 41, 43, 49, 51):  # loads with update
                regs.pop(ra, None)
                if op in (35, 41, 43):
                    regs.pop(rd, None)
            elif op in (11, 10, 16, 17, 46, 47, 4, 59, 63):
                if op in (46,):
                    for r in range(rd, 32):
                        regs.pop(r, None)
            else:
                regs.pop(rd, None)
        self.refs = refs
        self.icalls = icalls
        self.words_base = t0

    def _ro(self, ea):
        # treat the whole first two LOAD segments as readable; RW data values are the
        # initial image values (fine for pointer pools, which are never rewritten)
        return self.elf.off(ea) is not None

    # -------------------------------------------------------------- queries
    def func_of(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        return self.starts[i] if i >= 0 else None

    def func_end(self, f):
        i = bisect.bisect_right(self.starts, f)
        return self.starts[i] if i < len(self.starts) else self.text_hi

    def index(self):
        self.callers = {}
        for s, t in self.calls:
            self.callers.setdefault(t, []).append(s)
        for s, o in self.icalls:
            f = self.opd.get(o)
            if f:
                self.callers.setdefault(f, []).append(s)
        self.byval = {}
        for s, v, k in self.refs:
            self.byval.setdefault(v, []).append((s, k))
        self.opd_of = {}
        for o, f in self.opd.items():
            self.opd_of.setdefault(f, []).append(o)
        self._ptrs = None

    def ptrs_to(self, v):
        """aligned data words (outside text and OPD) equal to v: pointer pools, vtables, tables"""
        if self._ptrs is None:
            import struct as S
            self._ptrs = {}
            e = self.elf
            for va, off, fs, ms, fl in e.segs:
                n = fs // 4
                ws = S.unpack_from('>%dI' % n, e.d, off)
                for i, w in enumerate(ws):
                    a = va + 4 * i
                    if self.text_lo <= a < self.text_hi or self.opd_range[0] <= a < self.opd_range[1]:
                        continue
                    if 0x10000 <= w < 0x1100000:
                        self._ptrs.setdefault(w, []).append(a)
        return self._ptrs.get(v, [])

    def sites(self, v, depth=1):
        """code sites for v, directly or through pointer slots (depth levels)"""
        out = [(s, k, None) for s, k in self.byval.get(v, [])]
        if depth:
            for slot in self.ptrs_to(v):
                for s, k in self.byval.get(slot, []):
                    out.append((s, k, slot))
        return out


def load(elf_path, cache_dir):
    os.makedirs(cache_dir, exist_ok=True)
    h = hashlib.sha256(open(elf_path, 'rb').read()).hexdigest()
    cp = os.path.join(cache_dir, 'analysis-%s-v%d.pickle' % (h[:16], Analysis.VERSION))
    e = Elf(elf_path)
    if os.path.exists(cp):
        an = pickle.load(open(cp, 'rb'))
        an.elf = e
    else:
        an = Analysis(e)
        an.build()
        an.elf = None
        pickle.dump(an, open(cp, 'wb'))
        an.elf = e
    an.sha256 = h
    an.index()
    return an
