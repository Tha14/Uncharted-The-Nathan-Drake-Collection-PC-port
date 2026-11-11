"""dctools/ng119.py: the PS3 1.19 netgame table (net.bin DC 0x3b9a067d) next to the PC one: field maps and checks.

Shared by the recipe step netgame-order-119 (tools/fixes/netgame-order/netgame_order.py), which rebuilds the overlay
net.bin netgame and playlist tables in the order of the player's PS3 DATA2 net18.bin, and by the older net.bin steps
(classic.py, maps-batch/apply_maps.py), which must leave such a table alone when they run again.

Field map PS3 1.19 netgame (0xe4 bytes, BE, 4-byte pointers) -> PS4 (0xd0, LE, 8-byte pointers): see
u3mp-project/scratch/netgame-order/ANALYSIS.md section 1 (verified on all shared entries)."""
import os
import struct

NG, MAPS, PL = 0x3b9a067d, 0x0dec97a4, 0xc1b0be0d
NG3, NG4 = 0xe4, 0xd0          # netgame entry size PS3 1.19 / PS4
MAP3, MAP4 = 0x58, 0x98        # map list entry
PL3, PL4 = 0x5c, 0x68          # playlist entry
NG_OBJ, NG_SUB = 0xf0670de8, 0x8b617b51      # +0x74/+0x70 hero-record object, its 3 records {9 u32}
PL_LIST_T, PL_POOL_T = 0xea8e8c7e, 0x7b085cda  # playlist netgame list / pool object types (retail and PS3)
MAP_LIST_T = 0xd14c6ffc                        # map sub-level list object type
COOP_ADV = 11                                  # game type Co-op Adventure (coop-desc-fix byte order, see below)
CLASSIC = 0x13

# PS3 netgame scalar word -> PS4 offset; None = no PS4 slot (dropped; see DROPPED_OK)
MAP = {k: k for k in range(0, 0x48, 4)}
MAP.update({0x50: 0x58, 0x54: 0x5c, 0x58: 0x60, 0x5c: 0x64, 0x60: None, 0x64: 0x6c, 0x68: 0x68, 0x6c: None,
            0x70: None, 0x78: 0x78, 0x7c: 0x7c})
MAP.update({k: k + 4 for k in range(0x84, 0xbc, 4)})
MAP.update({k: None for k in range(0xc4, 0xe4, 4)})
# PS3 netgame pointer slot -> PS4 pointer slot
PTRS = {0x48: 0x48, 0x4c: 0x50, 0x74: 0x70, 0x80: 0x80, 0xbc: 0xc0, 0xc0: 0xc8}
# PS3 1.19 words with no PS4 slot: must be zero, except +0x6c/+0x70 (1 on the new-form Classic netgames, type 0x13)
DROPPED_CLASSIC = (0x6c, 0x70)
# PS3 playlist (0x5c) -> PS4 (0x68) scalar words; +0x28 -> +0x2c is the content-pack word, kept from the PC entry
PL_MAP = {0x0: 0x0, 0x4: 0x4, 0x8: 0x8, 0xc: 0xc, 0x14: 0x18, 0x18: 0x1c, 0x1c: 0x20, 0x20: 0x24, 0x24: 0x28,
          0x30: 0x38, 0x34: 0x3c}
PL_MAP.update({k: k + 0xc for k in range(0x3c, 0x5c, 4)})
PL_KEEP_PC = {0x2c}
PL_PTRS3, PL_PTRS4 = {0x10, 0x2c, 0x38}, {0x10, 0x30, 0x40}   # picture string, netgame list, pool
# PS3 map entry pointer slot -> PS4 slot (strings; +0x30 the sub-level list -> +0x58), as dctools/lab_reg.py
MAP_STR = {0xc: 0x10, 0x10: 0x18, 0x18: 0x28, 0x20: 0x38, 0x28: 0x48, 0x40: 0x70, 0x48: 0x80}
MAP_U32 = {0x0: 0x0, 0x4: 0x4, 0x8: 0x8, 0x1c: 0x30, 0x24: 0x40, 0x2c: 0x50, 0x38: 0x68, 0x3c: 0x6c, 0x44: 0x78,
           0x4c: 0x88, 0x50: 0x90, 0x54: 0x94}
MAP_NOT_LISTED = 0x94          # PS4 map entry word: 1 = not listed (a PS3 map whose paks the PC does not have)


def table(c, s=NG):
    """(header offset, count, array offset) of a DC table symbol (count u32, then a pointer)."""
    o = c.bysid[s][1]
    return o, c.u32(o), c.ptr(o + c.ps)


def hero_records(c, t, step):
    return [[c.u32(c.ptr(t + step * j) + 4 * w) for w in range(9)] for j in range(3)]


def reversed_word0(rec):
    """the record form coop-desc-fix leaves on Co-op Adventure netgames: word 0 (u8[4]) in PS3 byte order"""
    return [[int.from_bytes(x[0].to_bytes(4, 'little'), 'big')] + x[1:] for x in rec]


def order_matches(c, b):
    """True when the PC table c has exactly the PS3 table b's netgames: same count, and at every index the same
    name, map and game type."""
    _, n, ar = table(c)
    _, n3, ar3 = table(b)
    if n != n3:
        return False
    for i in range(n):
        g, s = ar + i * NG4, ar3 + i * NG3
        if any(c.u32(g + k) != b.u32(s + k) for k in (0, 4, 0xc)):
            return False
    return True


def is_ps3_order(net_path, net18_path, DC):
    """Is the net.bin netgame table already in PS3 1.19 order (made by netgame-order-119)?  With the player's
    net18.bin: exact (order_matches).  Without it: the table has more than 256 netgames, which the steps before
    netgame-order-119 never make (maps-batch keeps the PC table at <= 256; PS3 1.19 has 271)."""
    c = DC(net_path)
    if net18_path and os.path.isfile(net18_path):
        return order_matches(c, DC(net18_path))
    return table(c)[1] > 256


def check(c, b):
    """Every difference between the PC net.bin c and the table it must have after netgame-order-119 (PS3 net18 b):
    netgame index identity (name, map, type), scalar fields = converted PS3 values, pointer slots present like on
    PS3, co-op hero records = PS3 (or the coop-desc-fix form on Co-op Adventure), every PS3 map in the map list,
    playlists = PS3 (ids, order, scalar words, netgame lists with weights, pools).  [] = nothing to do."""
    err = []
    d = c.d
    n = struct.unpack_from('<I', d, c.reloc)[0]
    if c.reloc + 4 + n != len(d):
        err.append('container size')
    for p in c.ptrs:
        if not 0 < c.ptr(p) < c.reloc:
            err.append('pointer %x -> %x' % (p, c.ptr(p)))
            break
    _, N, ar = table(c)
    _, n3, ar3 = table(b)
    if N != n3:
        err.append('netgame count %d, PS3 %d' % (N, n3))
    for i in range(min(N, n3)):
        g, s = ar + i * NG4, ar3 + i * NG3
        if any(c.u32(g + k) != b.u32(s + k) for k in (0, 4, 0xc)):
            err.append('index %d: %08x map %08x type %d, PS3 %08x map %08x type %d' % (
                i, c.u32(g), c.u32(g + 4), c.u32(g + 0xc), b.u32(s), b.u32(s + 4), b.u32(s + 0xc)))
            continue
        for k3, k4 in MAP.items():
            if k4 is not None and c.u32(g + k4) != b.u32(s + k3):
                err.append('index %d %08x: +%x = %x, PS3 +%x = %x' % (i, c.u32(g), k4, c.u32(g + k4), k3, b.u32(s + k3)))
        for k3, k4 in PTRS.items():
            if c.isptr(g + k4) != b.isptr(s + k3):
                err.append('index %d %08x: pointer +%x present %s, PS3 +%x %s' % (i, c.u32(g), k4, c.isptr(g + k4), k3, b.isptr(s + k3)))
        if c.isptr(g + 0x70) and b.isptr(s + 0x74):
            r4, r3 = hero_records(c, c.ptr(g + 0x70), 8), hero_records(b, b.ptr(s + 0x74), 4)
            if r4 != r3 and not (b.u32(s + 0xc) == COOP_ADV and r4 == reversed_word0(r3)):
                err.append('index %d %08x: co-op hero records differ from PS3' % (i, c.u32(g)))
    _, nm, am = table(c, MAPS)
    maps = {c.u32(am + i * MAP4) for i in range(nm)}
    _, nm3, am3 = table(b, MAPS)
    for i in range(nm3):
        if b.u32(am3 + i * MAP3) not in maps:
            err.append('PS3 map %08x is not in the map list' % b.u32(am3 + i * MAP3))
    for i in range(N):
        m = c.u32(ar + i * NG4 + 4)
        if m and m not in maps:
            err.append('netgame %d map %08x is not in the map list' % (i, m))
    names = {c.u32(ar + i * NG4) for i in range(N)}
    _, npl, apl = table(c, PL)
    _, np3, ap3 = table(b, PL)
    if npl != np3:
        err.append('playlist count %d, PS3 %d' % (npl, np3))
    for j in range(min(npl, np3)):
        m, m3 = apl + j * PL4, ap3 + j * PL3
        if c.u32(m) != b.u32(m3):
            err.append('playlist %d: %08x, PS3 %08x' % (j, c.u32(m), b.u32(m3)))
            continue
        for k3, k4 in PL_MAP.items():
            if k4 not in PL_KEEP_PC and c.u32(m + k4) != b.u32(m3 + k3):
                err.append('playlist %08x +%x' % (c.u32(m), k4))
        l, l3 = c.ptr(m + 0x30), b.ptr(m3 + 0x2c)
        L = [(c.u32(c.ptr(l + 8) + 8 * k), c.u32(c.ptr(l + 8) + 8 * k + 4)) for k in range(c.u32(l))]
        L3 = [(b.u32(b.ptr(l3 + 4) + 8 * k), b.u32(b.ptr(l3 + 4) + 8 * k + 4)) for k in range(b.u32(l3))]
        if L != L3:
            err.append('playlist %08x netgame list' % c.u32(m))
        if any(s not in names for s, _ in L):
            err.append('playlist %08x names a netgame that is not in the table' % c.u32(m))
        if c.isptr(m + 0x40) != b.isptr(m3 + 0x38):
            err.append('playlist %08x pool presence' % c.u32(m))
        elif b.isptr(m3 + 0x38):
            p, p3 = c.ptr(m + 0x40), b.ptr(m3 + 0x38)
            if [c.u32(c.ptr(p + 8) + 4 * k) for k in range(c.u32(p))] != [b.u32(b.ptr(p3 + 4) + 4 * k) for k in range(b.u32(p3))]:
                err.append('playlist %08x pool' % c.u32(m))
    return err
