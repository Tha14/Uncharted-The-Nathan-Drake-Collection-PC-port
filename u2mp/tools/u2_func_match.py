"""u2_func_match.py - match PS3 1.10 functions of Uncharted 2 to their PS4 1.02 counterparts
(read-only; prints addresses, names and short strings only).

Usage:
  python u2_func_match.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> <PS4 eboot.elf> build
  python u2_func_match.py ... show <ps3hex> [...]     match, method, PS4 state for PS3 functions
  python u2_func_match.py ... natives [regex]         script natives (C name, DC name, PS3 fn, PS4 fn)
  python u2_func_match.py ... stats

Matching passes (each result keeps the method that found it):
  native   script natives: PS3 registration calls (C name string such as "DcNetIsHost" and the
           OPD loaded just before) against the PS4 registration nodes (lea fn; ...; lea name).
  str      a string referenced by exactly one function on each side.
  sid      a 32-bit constant (string id, event type hash, magic) used by exactly one function
           on each side; values below 0x10000 and obvious masks are ignored.
  strset   the full set of strings of a PS3 function equals the set of exactly one PS4 function.
  vtable   PS3 vtables (runs of OPD pointers in data) aligned with PS4 vtables (runs of
           R_X86_64_RELATIVE slots into text) when two or more matched functions agree on the
           pair and the slot offset; then every slot pair is matched.
  callee   for a matched pair, ordered direct callees are aligned between matched anchors; a gap
           of equal length (<= 3) on both sides is paired in order (sizes must be plausible).
  caller   the only unmatched caller on each side of a matched pair whose caller sets agree.
  menucb   menu control callbacks paired by control id (work/ps3-110/menu-callbacks.txt).
  strfuzzy best Jaccard >= 0.5 over rare strings (<= 3 functions per side), unique best.
  score    weighted Jaccard (idf) over strings, constants, float constants, matched callees and
           callers; >= 0.45, unique best by 0.1 on both sides.
  inlined  (not one-to-one) an unmatched PS3 function whose distinctive features (>= 60 %
           idf-weighted) are inside the PS4 counterpart of its caller or caller's caller, or of
           a callee / a callee's PS4 caller (the PS4 compiler merged them).
  order    two consecutive matched pairs that enclose the same number (<= 4) of functions on
           both sides, all with plausible sizes: paired in address order.
The passes repeat until nothing changes. Conflicts (one PS4 function claimed twice) keep the
first claim. Results go to <repo>/work/match/ (git-ignored).
PS4 state (from ps4_cg.py): alive = referenced by a data slot or by code of a function that is
not dead; dead = no reference left after removing references from dead functions (fixpoint);
stub(...) = trivial body.
"""
import bisect, collections, os, pickle, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps3_elf as P
import ps3_map as M
import ps4_cg as C

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(REPO, 'work', 'match')
PRINT = set(range(0x20, 0x7f)) | {9, 10}
PS3_REG = 0x14d470          # script native registration (obj, fn slot, "DcName", "dc-name", "ScriptFn")


def ps3_features(an):
    e = an.elf
    strs = collections.defaultdict(set)
    consts = collections.defaultdict(set)
    cache = {}
    for s, v, k in an.refs:
        f = an.func_of(s)
        if k == 'a':
            if v not in cache:
                st = None
                o = e.off(v)
                if o is not None and v > an.text_hi and (o == 0 or e.d[o - 1] == 0):
                    z = e.d.find(b'\0', o, o + 300)
                    if z - o >= 5 and all(c in PRINT for c in e.d[o:z]):
                        st = e.d[o:z].decode('latin1')
                cache[v] = st
            if cache[v]:
                strs[f].add(cache[v])
        elif k == 'k':
            consts[f].add(v)
    calls = collections.defaultdict(list)
    for s, t in an.calls:
        calls[an.func_of(s)].append((s, t))
    for s, o in an.icalls:
        t = an.opd.get(o)
        if t:
            calls[an.func_of(s)].append((s, t))
    for f in calls:
        calls[f] = [t for s, t in sorted(calls[f])]
    return strs, consts, calls


def ps4_features(cg):
    d, b = cg.d, cg.bias
    strs = collections.defaultdict(set)
    cache = {}
    for s, t, k in cg.rips:
        if t < C.TEXT_END:
            continue
        if t not in cache:
            o = t + b
            st = None
            if 0 < o < len(d) and d[o - 1] == 0:
                z = d.find(b'\0', o, o + 300)
                if z - o >= 5 and all(c in PRINT for c in d[o:z]):
                    st = d[o:z].decode('latin1')
            cache[t] = st
        if cache[t]:
            strs[cg.func_of(s)].add(cache[t])
    consts = collections.defaultdict(set)
    for s, v in cg.imms:
        consts[cg.func_of(s)].add(v)
    calls = collections.defaultdict(list)
    for s, t in cg.calls:
        calls[cg.func_of(s)].append(t)
    for s, t, u in cg.jmps:
        if u:
            f = cg.func_of(s)
            if not (f <= t < cg.func_end(f)):
                calls[f].append(t)
    return strs, consts, calls


def ps3_natives(an):
    """(C name, dc name, fn) from calls to the registration routine"""
    e = an.elf
    rs = sorted((s, v) for s, v, k in an.refs if k == 'a')
    keys = [x[0] for x in rs]
    out = []
    sites = sorted(s for s, t in an.calls if t == PS3_REG)
    for s in sites:
        i = bisect.bisect_left(keys, s)
        j = i - 1
        cname = dname = fn = None
        while j >= 0 and rs[j][0] > s - 0x50:
            v = rs[j][1]
            if fn is None and v in an.opd and an.opd_range[0] <= v < an.opd_range[1]:
                fn = an.opd[v]
            st = e.cstr(v, 80) if e.off(v) else None
            if st and re.match(r'^D[Cc][A-Z]\w+$', st) and cname is None:
                cname = st
            elif st and re.match(r'^[a-z0-9][a-z0-9\-\?\!\*\>\<\=/]*$', st) and dname is None and len(st) > 2:
                dname = st
            j -= 1
        if cname and fn:
            out.append((cname, dname, fn))
    return out


def ps4_natives(cg, strs4_cache=None):
    d, b = cg.d, cg.bias
    out = []
    rips = cg.rips
    for i, (s, t, k) in enumerate(rips):
        if t < C.TEXT_END:
            continue
        o = t + b
        if d[o:o + 2] not in (b'Dc', b'DC') or d[o - 1] != 0:
            continue
        z = d.find(b'\0', o, o + 100)
        st = d[o:z].decode('latin1')
        if not re.match(r'^D[Cc][A-Z]\w+$', st):
            continue
        fn = None
        for j in range(i - 1, max(i - 6, 0), -1):
            s2, t2, k2 = rips[j]
            if s2 < s - 0x40:
                break
            if t2 < C.TEXT_END and k2 == 0 and t2 in cg.starts_set:
                fn = t2
                break
        dname = None
        for j in range(i + 1, min(i + 4, len(rips))):
            s2, t2, k2 = rips[j]
            if t2 >= C.TEXT_END:
                o2 = t2 + b
                z2 = d.find(b'\0', o2, o2 + 100)
                c = d[o2:z2].decode('latin1')
                if re.match(r'^[a-z0-9][a-z0-9\-\?\!\*\>\<\=/]*$', c):
                    dname = c
                break
        if fn:
            out.append((st, dname, fn))
    return out


def vtables3(an):
    """PS3: runs of consecutive data words that hold OPD addresses -> {fn: [(run, idx)]}"""
    e = an.elf
    runs = []
    for va, off, fs, ms, fl in e.segs:
        import struct
        n = fs // 4
        ws = struct.unpack_from('>%dI' % n, e.d, off)
        cur = []
        for i, w in enumerate(ws):
            a = va + 4 * i
            if an.opd_range[0] <= a < an.opd_range[1] or an.text_lo <= a < an.text_hi:
                if cur:
                    runs.append(cur); cur = []
                continue
            if w in an.opd:
                cur.append((a, an.opd[w]))
            else:
                if len(cur) >= 2:
                    runs.append(cur)
                cur = []
        if len(cur) >= 2:
            runs.append(cur)
    idx = collections.defaultdict(list)
    for r, run in enumerate(runs):
        for i, (a, f) in enumerate(run):
            idx[f].append((r, i))
    return runs, idx


def vtables4(cg):
    slots = sorted((s, t) for s, t in cg.rel.items() if 0 < t < C.TEXT_END)
    runs, cur = [], []
    for s, t in slots:
        if cur and s == cur[-1][0] + 8:
            cur.append((s, t))
        else:
            if len(cur) >= 2:
                runs.append(cur)
            cur = [(s, t)]
    if len(cur) >= 2:
        runs.append(cur)
    idx = collections.defaultdict(list)
    for r, run in enumerate(runs):
        for i, (a, f) in enumerate(run):
            idx[f].append((r, i))
    return runs, idx


def ps3_floats(an):
    """float constants loaded with lfs/lfd from a resolved address, per function"""
    import struct
    e = an.elf
    out = collections.defaultdict(set)
    for s, v, k in an.refs:
        if k != 'm':
            continue
        w = e.u32(s)
        if w is None or (w >> 26) not in (48, 50):
            continue
        o = e.off(v)
        if o is None:
            continue
        x = struct.unpack_from('>f', e.d, o)[0] if (w >> 26) == 48 else struct.unpack_from('>d', e.d, o)[0]
        if x == x and abs(x) < 1e12:
            out[an.func_of(s)].add('%.5g' % x)
    return out


def ps4_floats(cg):
    import struct
    d, b = cg.d, cg.bias
    out = collections.defaultdict(set)
    for s, t, k in cg.rips:
        if k or t < C.TEXT_END:
            continue
        o = s + b
        b0 = d[o]
        if b0 in (0x66,) or 0x40 <= b0 <= 0x4f:
            b0 = d[o + 1]; o += 1
        pp = None
        if b0 == 0xc5:
            pp = d[o + 1] & 3
        elif b0 == 0xc4:
            pp = d[o + 2] & 3
        elif b0 in (0xf3, 0xf2) and d[o + 1] in (0x0f,) or (b0 in (0xf3, 0xf2) and 0x40 <= d[o + 1] <= 0x4f):
            pp = 2 if b0 == 0xf3 else 3
        if pp not in (2, 3):
            continue
        to = t + b
        x = struct.unpack_from('<f', d, to)[0] if pp == 2 else struct.unpack_from('<d', d, to)[0]
        if x == x and abs(x) < 1e12:
            out[cg.func_of(s)].add('%.5g' % x)
    return out


def score_pass(m, used, claim, s3, s4, k3, k4, fl3, fl4, c3, c4, callers3, callers4, plausible):
    """weighted-Jaccard similarity over strings, constants, floats, matched callees/callers"""
    import math
    def feats3(f):
        F = set('S' + x for x in s3.get(f, ())) | set('K%x' % x for x in k3.get(f, ()) if x >= 0x10000)
        F |= set('F' + x for x in fl3.get(f, ()))
        F |= set('C%x' % m[t][0] for t in c3.get(f, ()) if t in m)
        F |= set('R%x' % m[t][0] for t in callers3.get(f, ()) if t in m)
        return F
    inv_used = {g: f for f, (g, h) in m.items()}
    def feats4(g):
        F = set('S' + x for x in s4.get(g, ())) | set('K%x' % x for x in k4.get(g, ()) if x >= 0x10000)
        F |= set('F' + x for x in fl4.get(g, ()))
        F |= set('C%x' % t for t in c4.get(g, ()) if t in inv_used)
        F |= set('R%x' % t for t in callers4.get(g, ()) if t in inv_used)
        return F
    all3 = set(s3) | set(fl3) | set(c3) | set(callers3)
    all4 = set(s4) | set(fl4) | set(c4) | set(callers4)
    F4 = {g: feats4(g) for g in all4 if g not in used}
    df = collections.Counter()
    inv = collections.defaultdict(list)
    for g, F in F4.items():
        for x in F:
            df[x] += 1
            inv[x].append(g)
    idf = lambda x: math.log(1 + 2000.0 / (1 + df.get(x, 0)))
    best3 = {}
    for f in all3:
        if f in m:
            continue
        F = feats3(f)
        if len(F) < 2:
            continue
        cand = set()
        for x in F:
            if 0 < df.get(x, 0) <= 40:
                cand.update(inv[x])
        sc = []
        for g in cand:
            G = F4[g]
            inter = sum(idf(x) for x in F & G)
            uni = sum(idf(x) for x in F | G)
            if uni and plausible(f, g):
                sc.append((inter / uni, g))
        sc.sort(reverse=True)
        if sc and sc[0][0] >= 0.45 and (len(sc) == 1 or sc[0][0] - sc[1][0] >= 0.1):
            best3[f] = sc[0]
    byg = collections.defaultdict(list)
    for f, (v, g) in best3.items():
        byg[g].append((v, f))
    n = 0
    for g, lst in byg.items():
        lst.sort(reverse=True)
        if len(lst) == 1 or lst[0][0] - lst[1][0] >= 0.1:
            if claim(lst[0][1], g, 'score:%.2f' % lst[0][0]):
                n += 1
    return n


def inline_pass(m, s3, s4, k3, k4, fl3, fl4, c3, c4, callers3, callers4=None):
    """unmatched PS3 f whose distinctive features sit inside the PS4 counterpart of a caller
    (or a caller's caller): f was inlined there. Returns {f: (g, containment)}"""
    import math
    df = collections.Counter()
    for dct, pre in ((s4, 'S'), (fl4, 'F')):
        for g, xs in dct.items():
            for x in xs:
                df[pre + x] += 1
    for g, xs in k4.items():
        for x in xs:
            df['K%x' % x] += 1
    inv = {g: f for f, (g, h) in m.items()}
    for g, ts in c4.items():
        for t in set(ts):
            if t in inv:
                df['C%x' % t] += 1
    idf = lambda x: math.log(1 + 2000.0 / (1 + df.get(x, 0)))
    def f3(f):
        F = set('S' + x for x in s3.get(f, ())) | set('K%x' % x for x in k3.get(f, ()) if x >= 0x10000)
        F |= set('F' + x for x in fl3.get(f, ()))
        F |= set('C%x' % m[t][0] for t in c3.get(f, ()) if t in m)
        return F
    def f4(g):
        F = set('S' + x for x in s4.get(g, ())) | set('K%x' % x for x in k4.get(g, ()) if x >= 0x10000)
        F |= set('F' + x for x in fl4.get(g, ()))
        F |= set('C%x' % t for t in c4.get(g, ()))
        return F
    out = {}
    for f in list(callers3.keys()) + list(s3.keys()):
        if f in m or f in out:
            continue
        F = f3(f)
        if len(F) < 2:
            continue
        hosts = set()
        for p in callers3.get(f, ()):
            if p in m:
                hosts.add(m[p][0])
            for q in callers3.get(p, ()):
                if q in m:
                    hosts.add(m[q][0])
        for t in c3.get(f, ()):
            if t in m:
                hosts.add(m[t][0])
                for q in (callers4 or {}).get(m[t][0], ()):
                    hosts.add(q)
        best = None
        for g in hosts:
            G = f4(g)
            sh = F & G
            if len(sh) < 2:
                continue
            c = sum(idf(x) for x in sh) / sum(idf(x) for x in F)
            if c >= 0.6 and (best is None or c > best[1]):
                best = (g, c)
        if best:
            out[f] = best
    return out


def dead_set(cg):
    """fixpoint: functions with no reference except from dead functions"""
    refs = collections.defaultdict(set)    # target fn -> referencing fns (code)
    for k, lst in ((0, [(s, t) for s, t in cg.calls]), (1, [(s, t) for s, t, u in cg.jmps if u]),
                   (2, [(s, t) for s, t, kk in cg.rips])):
        for s, t in lst:
            if t in cg.starts_set:
                f = cg.func_of(s)
                if f != t:
                    refs[t].add(f)
    slotted = set(t for t in cg.rel.values())
    dead = set(f for f in cg.starts if f not in slotted and not refs.get(f))
    changed = True
    while changed:
        changed = False
        for f in cg.starts:
            if f in dead or f in slotted:
                continue
            if refs.get(f) and refs[f] <= dead:
                dead.add(f)
                changed = True
    return dead


class Matcher:
    def __init__(self, ps3, img, elf):
        self.an = P.load(ps3, M.DEF_CACHE)
        self.cg = C.CG(img, elf)
        self.cg.starts_set = set(self.cg.starts)

    def build(self):
        an, cg = self.an, self.cg
        s3, k3, c3 = ps3_features(an)
        s4, k4, c4 = ps4_features(cg)
        self.s3, self.s4 = s3, s4
        m = {}      # ps3 -> (ps4, method)
        used = {}

        def claim(f, g, how):
            if f in m or g in used or f is None or g is None:
                return False
            m[f] = (g, how)
            used[g] = f
            return True
        # natives
        n3 = {c: fn for c, dn, fn in ps3_natives(an)}
        n4 = {c: fn for c, dn, fn in ps4_natives(cg)}
        self.nat3, self.nat4 = ps3_natives(an), ps4_natives(cg)
        for c in n3:
            if c in n4:
                claim(n3[c], n4[c], 'native:' + c)
        # unique strings
        by3, by4 = collections.defaultdict(set), collections.defaultdict(set)
        for f, ss in s3.items():
            for s in ss:
                by3[s].add(f)
        for g, ss in s4.items():
            for s in ss:
                by4[s].add(g)
        for s in sorted(by3):
            if len(by3[s]) == 1 and len(by4.get(s, ())) == 1:
                claim(next(iter(by3[s])), next(iter(by4[s])), 'str:' + s[:40])
        # unique constants
        bk3, bk4 = collections.defaultdict(set), collections.defaultdict(set)
        for f, ks in k3.items():
            for k in ks:
                bk3[k].add(f)
        for g, ks in k4.items():
            for k in ks:
                bk4[k].add(g)

        def interesting(v):
            if v < 0x10000 or v >= 0xffff0000 or (v & 0xffff) == 0 or v in (0x7fffffff, 0x80000000):
                return False
            if bin(v).count('1') <= 3:
                return False
            return True
        for k in sorted(bk3):
            if interesting(k) and len(bk3[k]) == 1 and len(bk4.get(k, ())) == 1:
                claim(next(iter(bk3[k])), next(iter(bk4[k])), 'sid:%08x' % k)
        # string sets
        set4 = collections.defaultdict(list)
        for g, ss in s4.items():
            if len(ss) >= 2:
                set4[frozenset(ss)].append(g)
        for f, ss in s3.items():
            if len(ss) >= 2 and f not in m:
                gs = set4.get(frozenset(ss), [])
                if len(gs) == 1:
                    claim(f, gs[0], 'strset')
        # menu control callbacks paired by control id (u2_menu_callbacks.py output)
        mc = os.path.join(REPO, 'work', 'ps3-110', 'menu-callbacks.txt')
        if os.path.exists(mc):
            for line in open(mc):
                p = line.split()
                if '|' in p and not line.startswith('#'):
                    i = p.index('|')
                    try:
                        f, g = int(p[i - 2], 16), int(p[i + 2], 16)
                    except (ValueError, IndexError):
                        continue
                    claim(f, cg.func_of(g), 'menucb')
        # fuzzy string sets: rare strings (<= 3 functions per side), best Jaccard >= 0.5, unique
        for f, ss in s3.items():
            if f in m:
                continue
            rare = [x for x in ss if len(by3[x]) <= 3 and 0 < len(by4.get(x, ())) <= 3]
            if not rare:
                continue
            cand = collections.Counter()
            for x in rare:
                for g in by4[x]:
                    cand[g] += 1
            sc = sorted(((len(ss & s4[g]) / len(ss | s4[g]), g) for g in cand if g not in used), reverse=True)
            if sc and sc[0][0] >= 0.5 and (len(sc) == 1 or sc[1][0] < sc[0][0]):
                claim(f, sc[0][1], 'strfuzzy')
        # propagation
        r3, i3 = vtables3(an)
        r4, i4 = vtables4(cg)
        size3 = lambda f: an.func_end(f) - f
        size4 = lambda g: cg.func_end(g) - g

        def plausible(f, g):
            a, b = size3(f), size4(g)
            return 0.25 <= (b + 16) / (a + 16) <= 4.0
        callers3 = collections.defaultdict(set)
        for f, lst in c3.items():
            for t in lst:
                callers3[t].add(f)
        callers4 = collections.defaultdict(set)
        for g, lst in c4.items():
            for t in lst:
                callers4[t].add(g)
        done_vt = set()
        fl3, fl4 = ps3_floats(an), ps4_floats(cg)
        for outer in range(8):
          before_outer = len(m)
          for rnd in range(12):
            before = len(m)
            # vtables
            votes = collections.Counter()
            for f, (g, how) in list(m.items()):
                a3, a4 = i3.get(f, []), i4.get(g, [])
                if len(a3) > 4 or len(a4) > 4:
                    continue
                for ra, ia in a3:
                    for rb, ib in a4:
                        votes[(ra, rb, ia - ib)] += 1
            for (ra, rb, dl), n in votes.items():
                if n < 2 or (ra, rb) in done_vt:
                    continue
                done_vt.add((ra, rb))
                for ia, (sa, f) in enumerate(r3[ra]):
                    ib = ia - dl
                    if 0 <= ib < len(r4[rb]):
                        g = r4[rb][ib][1]
                        if len(i3.get(f, [])) <= 3 and len(i4.get(g, [])) <= 3 and plausible(f, g):
                            claim(f, g, 'vtable')
            # callees
            for f, (g, how) in list(m.items()):
                A = [t for t in c3.get(f, [])]
                B = [t for t in c4.get(g, [])]
                if not A or not B:
                    continue
                # anchors: positions where A[i] maps to B[j]
                pos4 = collections.defaultdict(list)
                for j, t in enumerate(B):
                    pos4[t].append(j)
                anchors = [(-1, -1)]
                lj = -1
                for i, t in enumerate(A):
                    if t in m:
                        js = [j for j in pos4.get(m[t][0], []) if j > lj]
                        if js:
                            anchors.append((i, js[0]))
                            lj = js[0]
                anchors.append((len(A), len(B)))
                for (i0, j0), (i1, j1) in zip(anchors, anchors[1:]):
                    ga, gb = A[i0 + 1:i1], B[j0 + 1:j1]
                    if len(ga) == len(gb) and 0 < len(ga) <= 3:
                        for x, y in zip(ga, gb):
                            if x not in m and y not in used and plausible(x, y):
                                claim(x, y, 'callee')
            # unique unmatched caller
            for f, (g, how) in list(m.items()):
                ua = [x for x in callers3.get(f, ()) if x not in m]
                ub = [y for y in callers4.get(g, ()) if y not in used]
                if len(ua) == 1 and len(ub) == 1 and len(callers3[f]) == len(callers4[g]):
                    if plausible(ua[0], ub[0]):
                        claim(ua[0], ub[0], 'caller')
            # address order: equal-sized gaps between consecutive anchors
            pairs = sorted((f, g) for f, (g, how) in m.items())
            for (f0, g0), (f1, g1) in zip(pairs, pairs[1:]):
                if not (0 < g1 - g0 < 0x8000 and 0 < f1 - f0 < 0x8000):
                    continue
                i0, i1 = bisect.bisect_right(an.starts, f0), bisect.bisect_left(an.starts, f1)
                j0, j1 = bisect.bisect_right(cg.starts, g0), bisect.bisect_left(cg.starts, g1)
                ga, gb = an.starts[i0:i1], cg.starts[j0:j1]
                if len(ga) == len(gb) and 0 < len(ga) <= 4:
                    if all(plausible(x, y) for x, y in zip(ga, gb)):
                        for x, y in zip(ga, gb):
                            claim(x, y, 'order')
            if len(m) == before:
                break
          n = score_pass(m, used, claim, s3, s4, k3, k4, fl3, fl4, c3, c4, callers3, callers4, plausible)
          if len(m) == before_outer:
            break
        self.m = m
        self.inl = inline_pass(m, s3, s4, k3, k4, fl3, fl4, c3, c4, callers3, callers4)
        self.dead = dead_set(cg)
        os.makedirs(OUT, exist_ok=True)
        pickle.dump({'m': m, 'inl': self.inl, 'dead': self.dead, 'nat3': self.nat3, 'nat4': self.nat4,
                     's4strs': set(s for ss in s4.values() for s in ss)},
                    open(os.path.join(OUT, 'match.pickle'), 'wb'))
        return m


def load(ps3, img, elf):
    mt = Matcher(ps3, img, elf)
    p = os.path.join(OUT, 'match.pickle')
    if os.path.exists(p):
        d = pickle.load(open(p, 'rb'))
        mt.m, mt.dead, mt.nat3, mt.nat4, mt.s4all = d['m'], d['dead'], d['nat3'], d['nat4'], d['s4strs']
        mt.inl = d.get('inl', {})
    else:
        mt.build()
        mt.s4all = set(s for ss in mt.s4.values() for s in ss)
    return mt


def ps4_state(mt, g):
    k = mt.cg.body_kind(g)
    if k.startswith('stub'):
        return k
    return 'dead' if g in mt.dead else 'alive'


def main(argv):
    if len(argv) < 5:
        print(__doc__)
        return 1
    ps3, img, elf, cmd = argv[1:5]
    args = argv[5:]
    if cmd == 'build':
        mt = Matcher(ps3, img, elf)
        m = mt.build()
        c = collections.Counter(h.split(':')[0] for g, h in m.values())
        print(len(m), dict(c))
        return 0
    mt = load(ps3, img, elf)
    if cmd == 'stats':
        c = collections.Counter(h.split(':')[0] for g, h in mt.m.values())
        print(len(mt.m), dict(c), 'dead', len(mt.dead))
    elif cmd == 'show':
        for h in args:
            f = mt.an.func_of(int(h, 16))
            r = mt.m.get(f)
            if r:
                print('%x -> %x %s %s' % (f, r[0], r[1], ps4_state(mt, r[0])))
            elif f in mt.inl:
                g, c = mt.inl[f]
                print('%x -> inlined into %x (%.2f) %s' % (f, g, c, ps4_state(mt, g)))
            else:
                print('%x -> -' % f)
    elif cmd == 'natives':
        rx = re.compile(args[0] if args else '.')
        n4 = {c: (d, f) for c, d, f in mt.nat4}
        for c, d, f in sorted(mt.nat3):
            if rx.search(c):
                g = n4.get(c)
                print('%-40s %-36s %08x %s' % (c, d, f, ('%x %s' % (g[1], ps4_state(mt, g[1]))) if g else '-'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
