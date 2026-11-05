#!/usr/bin/env python3
"""build_palettes.py <ps4 net.bin in> <ps3 1.19 net.bin> <out net.bin> <log.json>
Replace every PS4 colour palette (symbol type 0x290349e3: {u32 count, u32 0, ptr float[count][3]})
whose PS3 1.19 version differs with the 1.19 data. The symbol value object keeps its offset; only
count and ptr change. The new float array is appended; the old one stays (unreferenced)."""
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import DC, PAL
from dcedit import DCEdit

def pal_floats(c, o):
    n = c.u32(o); q = c.ptr(o + c.ps)
    return n, [c.u32(q + 4 * k) for k in range(3 * n)], q

def keyed(c):
    # a few sids are defined twice (e.g. 0x5bc1dff9 on every build): pair them by occurrence order
    r, seen = {}, {}
    for s, t, o in c.syms:
        if t != PAL: continue
        k = seen.get(s, 0); seen[s] = k + 1
        r[(s, k)] = o
    return r

def main(p4, p3, out, logp):
    a = DC(p4); b = DC(p3); e = DCEdit(p4)
    b3 = keyed(b)
    log = []
    for (s, k), o in keyed(a).items():
        if (s, k) not in b3:
            log.append(dict(sid='%08x' % s, action='kept (no PS3 1.19 symbol)')); continue
        n4, f4, q4 = pal_floats(a, o)
        n3, f3, _ = pal_floats(b, b3[(s, k)])
        if f4 == f3:
            log.append(dict(sid='%08x' % s, action='kept (identical)', count=n4)); continue
        # references to the old float array other than this symbol's ptr?
        refs = [p for p in a.ptrs if a.ptr(p) == q4 and p != o + 8]
        na = e.alloc(4 * len(f3), 16)
        struct.pack_into('<%dI' % len(f3), e.d, na, *f3)
        e.put32(o, n3)
        e.putptr(o + 8, na)
        rows4 = [tuple(f4[3*i:3*i+3]) for i in range(n4)]
        rows3 = [tuple(f3[3*i:3*i+3]) for i in range(n3)]
        changed = [i for i in range(min(n4, n3)) if rows4[i] != rows3[i]]
        log.append(dict(sid='%08x' % s, occurrence=k, action='replaced', old_count=n4, new_count=n3,
                        overwritten_slots=changed, other_refs_to_old_array=[hex(x) for x in refs],
                        sym_value='%#x' % o, new_array='%#x' % na))
    open(out, 'wb').write(e.build())
    json.dump(log, open(logp, 'w'), indent=1)
    r = [x for x in log if x['action'] == 'replaced']
    print('palettes', len(log), 'replaced', len(r), 'with overwritten slots', sum(1 for x in r if x['overwritten_slots']))

if __name__ == '__main__':
    main(*sys.argv[1:5])
