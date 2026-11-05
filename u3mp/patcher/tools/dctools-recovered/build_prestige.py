#!/usr/bin/env python3
"""build_prestige.py <ps4 net.bin> <ps3 1.19 net.bin> <out> <log.json>
*net-prestige-costs* 0x4d466631 (type 0x53d93c40 {count, ptr float[]}): PS4 4 multipliers
(1.0 1.15 1.3 1.45), PS3 1.19 6 (+1.75, 2.25). The store price code (0x7bca00/0x7bcf20) indexes the
array with the prestige level user+0x1860 and no bound check, so prestige 4/5 read past the PS4 array."""
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import DC, sid
from dcedit import DCEdit
S = 0x4d466631
def main(p4, p3, out, logp):
    a = DC(p4); b = DC(p3); e = DCEdit(p4)
    t, o4 = a.bysid[S]; n4 = a.u32(o4); q4 = a.ptr(o4 + 8)
    t, o3 = b.bysid[S]; n3 = b.u32(o3); q3 = b.ptr(o3 + 4)
    v4 = [a.u32(q4 + 4 * i) for i in range(n4)]; v3 = [b.u32(q3 + 4 * i) for i in range(n3)]
    assert v3[:n4] == v4
    na = e.alloc(4 * n3, 8)
    for i, v in enumerate(v3): struct.pack_into('<I', e.d, na + 4 * i, v)
    e.put32(o4, n3); e.putptr(o4 + 8, na)
    open(out, 'wb').write(e.build())
    json.dump(dict(old=[struct.unpack('<f', struct.pack('<I', v))[0] for v in v4], new=[struct.unpack('<f', struct.pack('<I', v))[0] for v in v3],
                   patched_fields=[(o4, 4), (o4 + 8, 8)]), open(logp, 'w'), indent=1)
    print('prestige', n4, '->', n3)
if __name__ == '__main__':
    main(*sys.argv[1:5])
