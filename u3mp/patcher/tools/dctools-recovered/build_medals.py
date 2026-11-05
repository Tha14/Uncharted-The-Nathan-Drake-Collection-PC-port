#!/usr/bin/env python3
"""build_medals.py <ps4 medals.bin> <ps3 1.19 medals.bin> <out> <log.json>
Medal table 0x980d6543 (PS4 0x48/entry, PS3 0x40/entry; PS4 = PS3 with the image char* widened
to 8 bytes and one trailing u32 0). PS4 retail = PS3 1.13 data plus Bluepoint edits.
 * 1.19 balance changes are applied to the matching entries (fields: +0x04 description text,
   +0x0c required count, +0x10 flags, +0x20 points). Two PS4-specific values are kept: hold-this
   flags and tricky-boy display index (Bluepoint edits, both differ from every PS3 version).
 * The 7 PS3 1.19 medals missing on PS4 are appended (PS3 order). Their +0x14 (PS4 award byte
   index) is -1 on PS3, +0x28 (medal-count slot) is 103/160..165 (< 0x100, the bound every PS4
   reader checks: user+0x250+(0x3d+i/2)*4 u16 counters).
The table value object keeps its offset; count/ptr point at a new array = retail entries copied
(with pointer bits) + appended entries."""
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from medals_common import DC, MED, rows
from dcedit import DCEdit

KEEP_PS4 = {(0xd5b6ab45, 4), }   # filled below by name
FIELDS = {1: 0x04, 3: 0x0c, 4: 0x10, 7: 0x20}

def main(p4, p3, out, logp):
    a = DC(p4); b = DC(p3); e = DCEdit(p4)
    o4, arr4, r4 = rows(a); o3, arr3, r3 = rows(b)
    n4 = len(r4)
    keep = {('net-medals/hold-this', 4), ('net-medals/tricky-boy', 9)}
    d3 = {x[0]: x for x in r3}
    sids4 = {x[0] for x in r4}
    new = [x for x in r3 if x[0] not in sids4]
    na = e.alloc(0x48 * (n4 + len(new)), 16)
    e.copy_words(arr4, na, 0x48 * n4)
    log = {'changed': [], 'added': []}
    for i, x in enumerate(r4):
        y = d3.get(x[0])
        if not y: continue
        for k, off in FIELDS.items():
            if x[k] != y[k] and (x[6], k) not in keep:
                e.put32(na + i * 0x48 + off, y[k])
                log['changed'].append(dict(index=i, medal=x[6], field='+%#x' % off, ps4=x[k], ps319=y[k]))
        for k in range(16):
            if x[k] != y[k] and k not in FIELDS and k != 5 and (x[6], k) not in keep:
                raise SystemExit('unexpected difference %s field %d' % (x[6], k))
    strs = {}
    for i, y in enumerate(new):
        ent = na + (n4 + i) * 0x48
        j = r3.index(y); src = arr3 + j * 0x40
        for k in range(0, 0x18, 4):
            struct.pack_into('<I', e.d, ent + k, b.u32(src + k))
        s = b.cstr(b.ptr(src + 0x18))
        pos = a.d.find(s.encode() + b'\0')
        while pos > 0 and a.d[pos - 1] != 0:          # need a whole string
            pos = a.d.find(s.encode() + b'\0', pos + 1)
        if pos <= 0 or pos >= a.reloc:
            if s not in strs: strs[s] = e.add_string(s)
            pos = strs[s]
        e.putptr(ent + 0x18, pos)
        for k in range(0x1c, 0x40, 4):
            struct.pack_into('<I', e.d, ent + k + 4, b.u32(src + k))
        log['added'].append(dict(index=n4 + i, ps3_index=j, sid='%08x' % y[0], image=s,
                                 count_slot=y[9], award_index=y[5]))
    e.put32(o4, n4 + len(new)); e.putptr(o4 + 8, na)
    open(out, 'wb').write(e.build())
    json.dump(log, open(logp, 'w'), indent=1)
    print('changed fields', len(log['changed']), 'added', len(new))

if __name__ == '__main__':
    main(*sys.argv[1:5])
