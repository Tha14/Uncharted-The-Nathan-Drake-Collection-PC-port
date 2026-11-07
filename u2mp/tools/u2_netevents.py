"""u2_netevents.py - NetEvent type table of Uncharted 2 multiplayer, PS3 1.10 against PS4 1.02
(read-only; prints addresses, ids and short strings only).

Both builds create net events in one factory (CreateNetEventByType): a switch over the event
type 0..79 that allocates the object and stores its vtable. PS3 1.10: 0x47e550, jump table
right after the bctr. PS4 1.02: 0x6180f0, jump table = the lea before 'movsxd rcx,[rax+4*rdi]'.
For every type this tool reads the vtable each case stores (the last one: the base
constructor stores the NetEvent base vtable first; a case whose derived vtable is stored in
shared code falls back to the free vtable that follows the previous type's one), then pairs the vtable slots of
the same type on both sides (same source, same class layout), and names the type from the
PS3 debug name table (kNpNetEvent* strings, pointer array at 0xd70c20, read by 0x468ed8).
Slot meaning (from the PS3 bodies): 0 dtor, 1 deleting dtor, 2 Write(BitStream),
3 Read(BitStream), 4 Apply/Process, 5 shared base method, 6 GetSize-like (ret 0).

Usage:
  python u2_netevents.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> <PS4 eboot.elf> [--tsv OUT]
Output: one line per type: id, name, PS3 vtable, PS4 vtable, slot count, then per slot
PS3 method -> PS4 method (state). --tsv writes the slot pairs (work/match/netevent-slots.tsv
by default) which u2_match_report.py merges as method 'evtable'.
"""
import bisect, collections, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import u2_func_match as U

PS3_FACTORY, PS3_TABLE = 0x47e550, 0x47e5a0
PS4_FACTORY, PS4_TABLE = 0x6180f0, 0x619ad4
NTYPES = 80
NAME0 = bytes(1) + b"kNpNetEventStartConnection" + bytes(1)


def ps3_types(an, r3):
    starts = {run[0][0]: k for k, run in enumerate(r3)}
    rs = sorted((s, v) for s, v, k in an.refs if k == 'a')
    keys = [x[0] for x in rs]
    end = an.func_end(PS3_FACTORY)
    out = {}
    tg = {}
    for t in range(NTYPES):
        off = struct.unpack('>i', an.elf.bytes(PS3_TABLE + 4 * t, 4))[0]
        tg[t] = PS3_TABLE + off
    bounds = sorted(set(tg.values()) | {end})
    for t, tgt in tg.items():
        if not (PS3_FACTORY <= tgt < end):
            continue
        lim = bounds[bounds.index(tgt) + 1]
        i = bisect.bisect_left(keys, tgt)
        while i < len(rs) and rs[i][0] < lim:      # the derived vtable is stored last
            v = rs[i][1]
            if v in starts:
                out[t] = (tgt, v, starts[v])
            i += 1
    # a case that stores its vtable in shared code: take the free run of the same stride
    # between the neighbouring types' vtables (vtables follow the declaration order)
    usedv = set(v[1] for v in out.values())
    for t in range(NTYPES):
        if t in out and out[t][1] != 0xd70bf8:
            continue
        prev = max((out[u][1] for u in out if u < t and out[u][1] != 0xd70bf8), default=None)
        if prev and prev + 0x30 in starts and prev + 0x30 not in usedv:
            out[t] = (tg[t], prev + 0x30, starts[prev + 0x30])
            usedv.add(prev + 0x30)
    return out


def type_names(an):
    """the PS3 debug name table (kNpNetEventStartConnection ... kNumNpNetEvents): a pointer
    array found through the slot that holds the first name"""
    e = an.elf
    o = e.d.find(NAME0)
    if o < 0:
        return {}
    va = None
    for v, off, fs, ms, fl in e.segs:
        if off <= o + 1 < off + fs:
            va = v + o + 1 - off
    slots = an.ptrs_to(va)
    out = {}
    if slots:
        for i in range(NTYPES):
            w = e.u32(slots[0] + 4 * i)
            st = e.cstr(w, 60) if w and e.off(w) else None
            if not st or not st.startswith('kNp'):
                break
            out[i] = st.replace('kNpNetEvent', 'NetEvent').replace('kNpEvent', 'NetEvent')
    return out


def ps4_types(cg, r4):
    starts = {run[0][0]: k for k, run in enumerate(r4)}
    d, b = cg.d, cg.bias
    end = cg.func_end(PS4_FACTORY)
    out = {}
    for t in range(NTYPES):
        off = struct.unpack_from('<i', d, PS4_TABLE + b + 4 * t)[0]
        tgt = PS4_TABLE + off
        if not (PS4_FACTORY <= tgt < end):
            continue
        i = bisect.bisect_left(cg.rips, (tgt, 0, 0))
        while i < len(cg.rips) and cg.rips[i][0] < tgt + 0x100:
            v = cg.rips[i][1]
            if v in starts:
                out[t] = (tgt, v, starts[v])
                break
            i += 1
    return out


def main(argv):
    argv = list(argv)
    tsv = os.path.join(U.OUT, 'netevent-slots.tsv')
    if '--tsv' in argv:
        i = argv.index('--tsv'); tsv = argv[i + 1]; del argv[i:i + 2]
    if len(argv) < 4:
        print(__doc__)
        return 1
    mt = U.load(argv[1], argv[2], argv[3])
    an, cg = mt.an, mt.cg
    cg.starts_set = set(cg.starts)
    r3, i3 = U.vtables3(an)
    r4, i4 = U.vtables4(cg)
    s3, k3, c3 = U.ps3_features(an)
    t3, t4 = ps3_types(an, r3), ps4_types(cg, r4)
    enum_names = type_names(an)
    rows = []
    for t in range(NTYPES):
        a, b = t3.get(t), t4.get(t)
        name = enum_names.get(t, '?')
        if name == '?' and a:
            cnt = collections.Counter()
            for sa, f in r3[a[2]]:
                for s in s3.get(f, ()):
                    for m in re.finditer(r'\b(NetEvent[A-Z]\w+)', s):
                        cnt[m.group(1)] += 1
            if cnt:
                name = cnt.most_common(1)[0][0]
        print('%2d %-36s ps3 vt %s  ps4 vt %s  slots %s/%s' % (
            t, name, '%x' % a[1] if a else '-', '%x' % b[1] if b else '-',
            len(r3[a[2]]) if a else 0, len(r4[b[2]]) if b else 0))
        if a and b:
            A, B = r3[a[2]], r4[b[2]]
            for k in range(max(len(A), len(B))):
                f = A[k][1] if k < len(A) else None
                g = B[k][1] if k < len(B) else None
                st = U.ps4_state(mt, g) if g else '-'
                print('     %2d %s -> %s %s' % (k, '%x' % f if f else '-', '%x' % g if g else '-', st))
                rows.append((t, name, k, f, g, st))
    os.makedirs(os.path.dirname(tsv), exist_ok=True)
    with open(tsv, 'w') as fo:
        fo.write('type\tname\tslot\tps3\tps4\tps4_state\n')
        for t, name, k, f, g, st in rows:
            fo.write('%d\t%s\t%d\t%s\t%s\t%s\n' % (t, name, k, '%x' % f if f else '', '%x' % g if g else '', st))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
