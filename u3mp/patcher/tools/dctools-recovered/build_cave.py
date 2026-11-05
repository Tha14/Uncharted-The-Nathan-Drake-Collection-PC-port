#!/usr/bin/env python3
"""build_cave.py <ps4 net.bin in> <ps3 1.19 net.bin> <out net.bin> <log.json>
Adds the DLC map Cave (PS3 *net-maps* entry 42, id 0x3869aa4e) to the PS4 net.bin:
  *net-maps*   0xdec97a4   entry appended (index 27)        PS3 0x58 -> PS4 0x98
  *net-games*  0x3b9a067d  6 netgames appended (89..94)       PS3 0xe4 -> PS4 0xd0
  *playlists*  0xc1b0be0d  netgame lists of 6 PS4 playlists get the Cave netgame appended
Conversion rules are derived from the 27 maps / 78 netgames / 9 playlists both builds share
(see CHANGES.md). Bluepoint rules followed:
  * map +0x14 (PS3 flag bits: 1/4/16 map packs, 64 hunter, 128 co-op) is 0 on every PS4 entry -> 0
  * PS4 competitive maps carry exactly modes 0,14,15,1,2,19 -> Cave mode 10 (0x85e4ff87,
    elimination playlist only), lab-class 0xbc8a121b and the classic-weapon (0xd932420c,
    a PS3-only weapon set) mode-19 0xc619236b are not added.
  * PS4 'classic' lists the mode-19 netgame that uses *online-basic-weapons* (PS3 form
    +0x5c 0x63e35446, +0x64 1) in place of PS3's 0xd932420c one -> Cave 0x831087b6 goes there
    with PS3's classic weight (10)."""
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import DC, sid
from dcedit import DCEdit

MAPS, NG, PL = 0xdec97a4, 0x3b9a067d, 0xc1b0be0d
CAVE = 0x3869aa4e
NETGAMES = [0x824b5bcc, 0xb033f301, 0xc1e921b8, 0xb42ce4f5, 0x49b1b2ed, 0x831087b6]
PL_EXTRA = {0xaac6efd8: [(0x831087b6, 10)]}          # classic (see docstring)
NG_OBJ = 0xf0670de8; NG_SUB = 0x8b617b51; LIST_T = 0xd14c6ffc


class Conv:
    def __init__(self, a, b, e):
        self.a, self.b, self.e = a, b, e
        self.strs = {}

    def string(self, s):
        """reuse a whole NUL-terminated string from the PS4 file or one already added"""
        if s in self.strs: return self.strs[s]
        enc = s.encode('latin1') + b'\0'
        d = self.a.d; pos = d.find(enc)
        while pos >= 0 and pos < self.a.reloc:
            if pos == 0 or d[pos - 1] == 0: break
            pos = d.find(enc, pos + 1)
        if pos < 0 or pos >= self.a.reloc:
            pos = self.e.add_string(s)
        self.strs[s] = pos
        return pos

    def obj(self, typ, size, ptrs):
        """allocate a typed object: [u32 0][u32 type] then 8-aligned body (pointer objects)"""
        e = self.e
        e.align(8); e.d += struct.pack('<II', 0, typ)
        o = len(e.d); e.d += bytes(size)
        return o

    def obj4(self, typ, words):
        e = self.e
        e.align(4); e.d += struct.pack('<I', typ)
        o = len(e.d); e.d += struct.pack('<%dI' % len(words), *words)
        return o


def table(c, S):
    t, o = c.bysid[S]
    return o, c.u32(o), c.ptr(o + c.ps)


def main(p4, p3, out, logp):
    a = DC(p4); b = DC(p3); e = DCEdit(p4); cv = Conv(a, b, e)
    log = {}
    # ---------------------------------------------------------------- map entry
    o4, n4, arr4 = table(a, MAPS); o3, n3, arr3 = table(b, MAPS)
    src = next(arr3 + i * 0x58 for i in range(n3) if b.u32(arr3 + i * 0x58) == CAVE)
    assert not any(a.u32(arr4 + i * 0x98) == CAVE for i in range(n4))
    # sub-level list
    l3 = b.ptr(src + 0x30); assert b.u32(l3 - 4) == LIST_T
    ln = b.u32(l3); li = b.ptr(l3 + 4)
    items = e.alloc(16 * ln, 8)
    subl = []
    for k in range(ln):
        s = b.cstr(b.ptr(li + 8 * k)); h = b.u32(li + 8 * k + 4); assert h == sid(s), s
        e.putptr(items + 16 * k, cv.string(s)); struct.pack_into('<I', e.d, items + 16 * k + 8, h)
        subl.append(s)
    lst = cv.obj(LIST_T, 16, True)
    struct.pack_into('<I', e.d, lst, ln); e.putptr(lst + 8, items)
    na = e.alloc(0x98 * (n4 + 1), 16)
    e.copy_words(arr4, na, 0x98 * n4)
    m = na + 0x98 * n4
    def u(k3, k4): struct.pack_into('<I', e.d, m + k4, b.u32(src + k3))
    def p(k3, k4):
        if b.isptr(src + k3): e.putptr(m + k4, cv.string(b.cstr(b.ptr(src + k3))))
        else: assert b.u32(src + k3) == 0
    u(0, 0); u(4, 4); u(8, 8)
    p(0xc, 0x10); p(0x10, 0x18)
    flags3 = b.u32(src + 0x14)                       # -> 0 (PS4 convention)
    p(0x18, 0x28); u(0x1c, 0x30); p(0x20, 0x38); u(0x24, 0x40); p(0x28, 0x48); u(0x2c, 0x50)
    e.putptr(m + 0x58, lst)
    assert b.u32(src + 0x34) == 0 and not b.isptr(src + 0x34)
    u(0x38, 0x68); u(0x3c, 0x6c)
    p(0x40, 0x70); u(0x44, 0x78); p(0x48, 0x80); u(0x4c, 0x88); u(0x50, 0x90); u(0x54, 0x94)
    e.put32(o4, n4 + 1); e.putptr(o4 + 8, na)
    log['map'] = dict(index=n4, id='%08x' % CAVE, ps3_entry='%#x' % src, ps3_flags=flags3, ps4_flags=0,
                      preview=b.cstr(b.ptr(src + 0xc)), main=b.cstr(b.ptr(src + 0x18)),
                      killvis=b.cstr(b.ptr(src + 0x20)), pak=b.cstr(b.ptr(src + 0x28)),
                      sublevels=subl, world=b.cstr(b.ptr(src + 0x40)), stage=b.cstr(b.ptr(src + 0x48)))
    # ---------------------------------------------------------------- netgames
    o4, n4, arr4 = table(a, NG); o3, n3, arr3 = table(b, NG)
    idx3 = {b.u32(arr3 + i * 0xe4): i for i in range(n3)}
    na = e.alloc(0xd0 * (n4 + len(NETGAMES)), 16)
    e.copy_words(arr4, na, 0xd0 * n4)
    log['netgames'] = []
    for j, s in enumerate(NETGAMES):
        src = arr3 + idx3[s] * 0xe4; g = na + 0xd0 * (n4 + j)
        assert b.u32(src + 4) == CAVE
        for k3 in range(0, 0xe4, 4):
            if b.isptr(src + k3) and k3 != 0x74: raise SystemExit('unhandled ptr %x in %08x' % (k3, s))
        for k in range(0, 0x48, 4): struct.pack_into('<I', e.d, g + k, b.u32(src + k))
        for k3, k4 in ((0x50, 0x58), (0x54, 0x5c), (0x58, 0x60), (0x5c, 0x64), (0x60, 0x68), (0x64, 0x6c),
                       (0x78, 0x78), (0x7c, 0x7c)) + tuple((k, k + 4) for k in range(0x84, 0xbc, 4)):
            struct.pack_into('<I', e.d, g + k4, b.u32(src + k3))
        dropped = {hex(k): b.u32(src + k) for k in (0x68, 0x6c, 0x70) + tuple(range(0xc4, 0xe4, 4)) if b.u32(src + k)}
        assert not dropped, (hex(s), dropped)
        # +0x74 triple of 8b617b51 records
        t3 = b.ptr(src + 0x74); assert b.u32(t3 - 4) == NG_OBJ
        subs = []
        for k in range(3):
            r = b.ptr(t3 + 4 * k); assert b.u32(r - 4) == NG_SUB
            subs.append(cv.obj4(NG_SUB, [b.u32(r + 4 * w) for w in range(9)]))
        t4 = cv.obj(NG_OBJ, 24, True)
        for k in range(3): e.putptr(t4 + 8 * k, subs[k])
        e.putptr(g + 0x70, t4)
        log['netgames'].append(dict(index=n4 + j, sid='%08x' % s, ps3_index=idx3[s], mode=b.u32(src + 0xc),
                                    weapons='%08x' % b.u32(src + 0x2c)))
    e.put32(o4, n4 + len(NETGAMES)); e.putptr(o4 + 8, na)
    # ---------------------------------------------------------------- playlists
    o4, n4, arr4 = table(a, PL); o3, n3, arr3 = table(b, PL)
    p3 = {b.u32(arr3 + i * 0x5c): arr3 + i * 0x5c for i in range(n3)}
    log['playlists'] = []
    want = set(NETGAMES)
    for i in range(n4):
        pe = arr4 + i * 0x68; ps = a.u32(pe)
        add = []
        if ps in p3:
            l = b.ptr(p3[ps] + 0x2c)
            for k in range(b.u32(l)):
                s, w = b.u32(b.ptr(l + 4) + 8 * k), b.u32(b.ptr(l + 4) + 8 * k + 4)
                if s in want: add.append((s, w))
        add += PL_EXTRA.get(ps, [])
        if not add: continue
        l4 = a.ptr(pe + 0x30); ln = a.u32(l4); li = a.ptr(l4 + 8)
        ni = e.alloc(8 * (ln + len(add)), 8)
        e.d[ni:ni + 8 * ln] = a.d[li:li + 8 * ln]
        for k, (s, w) in enumerate(add):
            struct.pack_into('<II', e.d, ni + 8 * (ln + k), s, w)
        e.put32(l4, ln + len(add)); e.putptr(l4 + 8, ni)
        log['playlists'].append(dict(index=i, name=a.cstr(a.ptr(pe + 0x10)), list_obj='%#x' % l4,
                                     old_count=ln, added=[('%08x' % s, w) for s, w in add]))
    open(out, 'wb').write(e.build())
    json.dump(log, open(logp, 'w'), indent=1)
    print(json.dumps(log, indent=1)[:3000])

if __name__ == '__main__':
    main(*sys.argv[1:5])
