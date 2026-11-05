#!/usr/bin/env python3
"""build_emblems.py <ps4 net.bin> <ps3 1.19 net.bin> <out> <log.json>
*net-emblem-layers-parts* images 0x03ffae77: PS3 1.19 has 79 entries, PS4 67 (frames 0xc7f8567c and
shapes/backgrounds 0xe2311588 are identical, 28/50). The 12 patch images are appended (indices 67..78).
Entry PS3 {sid, char* name, ptr drip} 0xc -> PS4 {sid, 0, char*, ptr} 0x18; drip object type
0x937276d5 {u32 0x200, u32 n, ptr u32[n][3]} (same on both). Unlock entries (category 9): the five
with condition type 7 are added (PS4 evaluator supports type 7); the seven gated by DLC packs
530/532/533 (>= 256, outside the PS4 owned-pack array) are left out = unlocked (previous agent's rule)."""
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import DC
from dcedit import DCEdit
import unlocks
IMAGES, DRIP = 0x03ffae77, 0x937276d5

def main(p4, p3, out, logp):
    a = DC(p4); b = DC(p3); e = DCEdit(p4); log = {}
    t, o4 = a.bysid[IMAGES]; n4 = a.u32(o4); arr4 = a.ptr(o4 + 8)
    t, o3 = b.bysid[IMAGES]; n3 = b.u32(o3); arr3 = b.ptr(o3 + 4)
    s4 = [a.u32(arr4 + i * 0x18) for i in range(n4)]
    assert s4 == [b.u32(arr3 + i * 0xc) for i in range(n4)]
    new = list(range(n4, n3))
    na = e.alloc(0x18 * n3, 16); e.copy_words(arr4, na, 0x18 * n4)
    log['images'] = []; added_sids = []
    for k, i in enumerate(new):
        src = arr3 + i * 0xc; dst = na + 0x18 * i
        name = b.cstr(b.ptr(src + 4)); struct.pack_into('<II', e.d, dst, b.u32(src), 0)
        e.putptr(dst + 8, e.add_string(name))
        d3 = b.ptr(src + 8); assert b.u32(d3 - 4) == DRIP
        cnt = b.u32(d3 + 4); q3 = b.ptr(d3 + 8)
        arr = e.alloc(12 * cnt, 8)
        for w in range(3 * cnt): struct.pack_into('<I', e.d, arr + 4 * w, b.u32(q3 + 4 * w))
        e.align(8); e.d += struct.pack('<II', 0, DRIP); obj = len(e.d); e.d += bytes(16)
        struct.pack_into('<II', e.d, obj, b.u32(d3), cnt); e.putptr(obj + 8, arr)
        e.putptr(dst + 0x10, obj)
        added_sids.append(b.u32(src))
        log['images'].append(dict(index=i, sid='%08x' % b.u32(src), name=name, drip_points=cnt))
    e.put32(o4, n3); e.putptr(o4 + 8, na)
    # unlocks
    have = unlocks.ps4_keys(a)[3]
    add, skipped = [], []
    for y in unlocks.ps3_unlocks(b):
        if y['cat'] != 9 or y['sid'] not in added_sids or (9, y['sid'], y['id']) in have: continue
        (skipped if unlocks.dlc_blocked(y) else add).append(y)
    added, last, f1, f2 = unlocks.insert(a, e, add, 9)
    log['unlock_added'] = added; log['unlock_insert_after'] = last
    log['unlock_skipped'] = [dict(id='%#x' % y['id'], sid='%08x' % y['sid'], conds=y['conds'], reason='DLC pack >= 256') for y in skipped]
    log['patched_fields'] = [(o4, 4), (o4 + 8, 8), f1, f2]
    open(out, 'wb').write(e.build()); json.dump(log, open(logp, 'w'), indent=1)
    print('images +%d, unlocks +%d, skipped %d' % (len(new), len(added), len(skipped)))

if __name__ == '__main__':
    main(*sys.argv[1:5])
