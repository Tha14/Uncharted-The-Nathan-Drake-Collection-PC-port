#!/usr/bin/env python3
"""build_treasures.py <ps4 net.bin> <ps3 1.19 net.bin> <out> <log.json>
*net-treasures* 0xb114fe00 (PS3 0x4c -> PS4 0x68) and *net-treasure-sets* 0xf1f296cb (12 bytes, same
layout). PS4 = the first 154 / 59 PS3 entries. PS3 1.19 entries whose treasure actor exists are
appended in PS3 order; sets are appended unless every treasure of the set was left out.
Treasure field map (derived from the 154 shared entries): 0->0, char* 4->8, 8->0x10, 0xc->0x18,
0x10..0x30 -> 0x1c..0x3c, char* 0x34->0x40, 0x38->0x48, char* 0x3c->0x50, 0x40->0x58, char* 0x44->0x60."""
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import DC
from dcedit import DCEdit
T, TS = 0xb114fe00, 0xf1f296cb
U32 = [(0, 0), (8, 0x10), (0xc, 0x18)] + [(k, k + 0xc) for k in range(0x10, 0x34, 4)] + [(0x38, 0x48), (0x40, 0x58)]
PTR = [(4, 8), (0x34, 0x40), (0x3c, 0x50), (0x44, 0x60)]

def main(p4, p3, out, logp):
    a = DC(p4); b = DC(p3); e = DCEdit(p4)
    av = set(l.strip() for l in open('/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/dc2/avail_actors.txt'))
    strs = {}
    def string(t):
        if t in strs: return strs[t]
        enc = t.encode('latin1') + b'\0'; d = a.d; pos = d.find(enc)
        while 0 <= pos < a.reloc and pos > 0 and d[pos - 1] != 0: pos = d.find(enc, pos + 1)
        if pos < 0 or pos >= a.reloc or pos < 0x18 + 16 * len(a.syms): pos = e.add_string(t)
        strs[t] = pos; return pos
    t, o4 = a.bysid[T]; n4 = a.u32(o4); r4 = a.ptr(o4 + 8)
    t, o3 = b.bysid[T]; n3 = b.u32(o3); r3 = b.ptr(o3 + 4)
    names4 = [a.cstr(a.ptr(r4 + i * 0x68 + 8)) for i in range(n4)]
    names3 = [b.cstr(b.ptr(r3 + i * 0x4c + 4)) for i in range(n3)]
    assert names4 == names3[:n4]
    add, skip = [], []
    for i in range(n4, n3):
        (add if names3[i] in av else skip).append(i)
    na = e.alloc(0x68 * (n4 + len(add)), 16); e.copy_words(r4, na, 0x68 * n4)
    log = {'treasures': [], 'treasures_skipped': [names3[i] for i in skip]}
    for k, i in enumerate(add):
        src = r3 + i * 0x4c; dst = na + 0x68 * (n4 + k)
        for k3, k4 in U32: struct.pack_into('<I', e.d, dst + k4, b.u32(src + k3))
        for k3, k4 in PTR:
            assert b.isptr(src + k3); e.putptr(dst + k4, string(b.cstr(b.ptr(src + k3))))
        assert b.u32(src + 0x48) == 0
        log['treasures'].append(dict(index=n4 + k, ps3_index=i, name=names3[i], set='%08x' % b.u32(src + 0x20)))
    e.put32(o4, n4 + len(add)); e.putptr(o4 + 8, na)
    # sets
    t, s4o = a.bysid[TS]; m4 = a.u32(s4o); q4 = a.ptr(s4o + 8)
    t, s3o = b.bysid[TS]; m3 = b.u32(s3o); q3 = b.ptr(s3o + 4)
    sets4 = [tuple(a.u32(q4 + 12 * i + k) for k in (0, 4, 8)) for i in range(m4)]
    sets3 = [tuple(b.u32(q3 + 12 * i + k) for k in (0, 4, 8)) for i in range(m3)]
    assert sets4 == sets3[:m4]
    member = {}
    for i in range(n3): member.setdefault(b.u32(r3 + i * 0x4c + 0x20), []).append(i)
    kept = set(range(n4)) | set(add)
    sadd, sskip = [], []
    for i in range(m4, m3):
        mem = member.get(sets3[i][0], [])
        (sadd if (not mem or any(x in kept for x in mem)) else sskip).append(i)
    ns = e.alloc(12 * (m4 + len(sadd)), 8)
    e.d[ns:ns + 12 * m4] = a.d[q4:q4 + 12 * m4]
    for k, i in enumerate(sadd): struct.pack_into('<III', e.d, ns + 12 * (m4 + k), *sets3[i])
    e.put32(s4o, m4 + len(sadd)); e.putptr(s4o + 8, ns)
    log['sets'] = [dict(index=m4 + k, ps3_index=i, set='%08x' % sets3[i][0], text='%08x' % sets3[i][1], n=sets3[i][2]) for k, i in enumerate(sadd)]
    log['sets_skipped'] = [dict(ps3_index=i, set='%08x' % sets3[i][0], members=[names3[x] for x in member.get(sets3[i][0], [])]) for i in sskip]
    log['patched_fields'] = [(o4, 4), (o4 + 8, 8), (s4o, 4), (s4o + 8, 8)]
    open(out, 'wb').write(e.build()); json.dump(log, open(logp, 'w'), indent=1)
    print('treasures +%d (skipped %d), sets +%d (skipped %d)' % (len(add), len(skip), len(sadd), len(sskip)))

if __name__ == '__main__':
    main(*sys.argv[1:5])
