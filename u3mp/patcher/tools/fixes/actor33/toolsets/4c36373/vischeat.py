"""PS3 -> PS4 converter for Uncharted 3 '-killvis' level packages (item VIS_CHEAT, type sid 0x4d3bdd98).

VIS_CHEAT is the multiplayer out-of-bounds voxel grid. PS3 logs it in (login handler f_898c90) into the global
0x12d5864 and only the local net player update f_363ed4 reads it, through f_74615c/f_745b64: when all 8 grid
corners around (player + (0, 1, 0)) are non-zero voxels, a 5 s timer starts and the player dies when it runs out.
PS4 kept the login/logout handlers (0x10e43c9 / 0x10e4d9e: global 0x1b3ae78 = item data, set only while null)
but no reader, and ships every killvis pak as an empty 244-byte stub. The U3 LAN helper ports the reader
(u3_lan_profiles.c vis_cheat_corners); this module writes the item in the layout that reader expects.

PS3 layout (BE, 32-bit pointers)                         PS4 layout written here (LE, 64-bit pointers)
 +00 u32 type (1 = no origin, 2 = with origin)            +00 u32 type
 +04 i32 min x, max x, min y, max y, min z, max z         +04 same (cells, max exclusive)
 +1c u8 axis0, axis1, axis2, pad                          +1c same
 +20 f32 scale x, y, z, pad                               +20 same
 type 2: +30 f32 origin x, y, z, pad                      type 2: +30 same
 +H   ptr runs   (H = 0x40 type 2, 0x30 type 1)           +H     ptr runs
 +H+4 u32 slice count (= dim[axis2])                      +H+8   u32 slice count, u32 0
 +H+8 ptr slices[count]                                   +H+0x10 ptr slices[count]
 runs:  {u32 n, ptr rows[n]}                              runs:  {u32 n, u32 0, ptr rows[n]}
 slice: {u16 n, u16 entry[n]}  entry = rep << 14 | row    slice: {u16 n, u16 entry[n]} (LE)
 row:   u8 value, then u8 run lengths (value toggles after each run unless the length is 255) - bytes unchanged
Slices and rows shared by several pointers stay shared.

usage: vischeat.py <ps3 killvis.pak> <out ps4 killvis.pak> [--check]
"""
import struct, sys, os
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, '/home/claude/u3/paktools')
from ndpak import Pak
from ps4writer import PakWriter, Blob, make_login_and_vram_page


class Src:
    def __init__(self, p, it):
        self.p, self.pg, self.it = p, it.page, it
        self.hb = p.pbase(it.page)

    def u8(self, o): return self.p.d[self.hb + o]
    def u16(self, o): return self.p.u16(self.hb + o)
    def u32(self, o): return self.p.u32(self.hb + o)
    def i32(self, o): return self.p.i32(self.hb + o)
    def f32b(self, o): return self.p.d[self.hb + o:self.hb + o + 4][::-1]   # BE float -> LE bytes

    def ptr(self, o):
        r = self.p.resolve(self.pg, o)
        if r is None:
            return None
        if r[0] != self.pg:
            raise ValueError('VIS_CHEAT pointer to another page')
        return r[1]


def row_len(s, o, rowlen):
    """bytes of one RLE row starting at o (value byte + run bytes covering rowlen)"""
    n, left = 1, rowlen
    while left > 0:
        c = s.u8(o + n)
        n += 1
        left -= c
    if left != 0:
        raise ValueError('row overruns its length')
    return n


def convert_item(s):
    g = s.it.data_off
    t = s.u32(g)
    if t not in (1, 2):
        raise ValueError('VIS_CHEAT type %d' % t)
    H = 0x40 if t == 2 else 0x30
    mn = [s.i32(g + 4 + 8 * i) for i in range(3)]
    mx = [s.i32(g + 8 + 8 * i) for i in range(3)]
    ax = [s.u8(g + 0x1c + i) for i in range(3)]
    dim = [mx[i] - mn[i] for i in range(3)]
    rowlen = dim[ax[0]]
    runs = s.ptr(g + H)
    nsl = s.u32(g + H + 4)
    if nsl != dim[ax[2]]:
        raise ValueError('slice count %d != dim %d' % (nsl, dim[ax[2]]))
    root = Blob('VIS_CHEAT')
    root.u32(t)
    for i in range(6):
        root.i32(s.i32(g + 4 + 4 * i))
    root.raw(bytes(ax) + b'\0')
    for i in range(3):
        root.raw(s.f32b(g + 0x20 + 4 * i))
    root.u32(0)
    if t == 2:
        for i in range(3):
            root.raw(s.f32b(g + 0x30 + 4 * i))
        root.u32(0)
    # runs table
    nrows = s.u32(runs)
    rows_blob = Blob('rows')
    row_at = {}
    rt = Blob('runs')
    rt.u32(nrows).u32(0)
    for i in range(nrows):
        ro = s.ptr(runs + 4 + 4 * i)
        if ro not in row_at:
            n = row_len(s, ro, rowlen)
            row_at[ro] = rows_blob.pos
            rows_blob.raw(s.p.d[s.hb + ro:s.hb + ro + n])
        rt.ptr(rows_blob, row_at[ro])
    # slices
    sl_blob = Blob('slices')
    sl_at = {}
    slice_ptrs = []
    for k in range(nsl):
        so = s.ptr(g + H + 8 + 4 * k)
        if so not in sl_at:
            n = s.u16(so)
            rows = 0
            sl_blob.pad(2)
            sl_at[so] = sl_blob.pos
            sl_blob.u16(n)
            for j in range(n):
                e = s.u16(so + 2 + 2 * j)
                if (e & 0x3fff) >= nrows:
                    raise ValueError('slice entry row %d >= %d' % (e & 0x3fff, nrows))
                rows += (e >> 14) + 1
                sl_blob.u16(e)
            if rows != dim[ax[1]]:
                raise ValueError('slice %d decodes %d rows, want %d' % (k, rows, dim[ax[1]]))
        slice_ptrs.append(sl_at[so])
    root.ptr(rt, 0)
    root.u32(nsl).u32(0)
    for o in slice_ptrs:
        root.ptr(sl_blob, o)
    root.embed(rt, 16)
    root.embed(sl_blob, 16)
    root.embed(rows_blob, 16)
    root.pad(16)
    return root, dict(type=t, min=mn, max=mx, axes=ax, rows=nrows, slices=nsl, unique_slices=len(sl_at),
                      bytes=len(root))


def convert(src_path, out_path):
    p = Pak(src_path)
    if p.platform != 'ps3':
        raise ValueError('expected a PS3 pak')
    its = p.items_of('VIS_CHEAT')
    others = [i.type for i in p.items if i.type not in ('VIS_CHEAT', 'PAK_LOGIN_TABLE', 'VRAM_DESC_TABLE')]
    if others:
        raise ValueError('unexpected items %r' % others)
    w = PakWriter()
    page = w.page()
    made, info = [], []
    for it in its:
        root, inf = convert_item(Src(p, it))
        made.append(page.item('VIS_CHEAT', root, name=it.name))
        info.append(inf)
    p1 = w.page()
    make_login_and_vram_page(w, p1, made, [])
    data = w.build()
    open(out_path, 'wb').write(data)
    return data, info


# ---- reference decoder of the PS4 layout (same algorithm as PS3 f_745b64 / the helper's vis_cell) ----
def ps4_query(pak, x, y, z):
    it = pak.items_of('VIS_CHEAT')[0]
    hb = pak.pbase(it.page)
    g = it.data_off
    u8 = lambda o: pak.d[hb + o]
    u16 = lambda o: pak.u16(hb + o)
    u32 = lambda o: pak.u32(hb + o)
    i32 = lambda o: pak.i32(hb + o)
    f32 = lambda o: pak.f32(hb + o)
    ptr = lambda o: pak.resolve(it.page, o)[1]
    t = u32(g)
    H = 0x40 if t == 2 else 0x30
    sc = [f32(g + 0x20 + 4 * i) for i in range(3)]
    org = [f32(g + 0x30 + 4 * i) for i in range(3)] if t == 2 else [0.0] * 3
    mn = [i32(g + 4 + 8 * i) for i in range(3)]
    mx = [i32(g + 8 + 8 * i) for i in range(3)]
    c = [int((v - o) * s) for v, o, s in zip((x, y, z), org, sc)]
    for i in range(3):
        if not mn[i] <= c[i] < mx[i]:
            return 2
    idx = [c[i] - mn[i] for i in range(3)]
    dim = [mx[i] - mn[i] for i in range(3)]
    ax = [u8(g + 0x1c + i) for i in range(3)]
    runs = ptr(g + H)
    sl = ptr(g + H + 0x10 + 8 * idx[ax[2]])
    want_row, want_col = idx[ax[1]], idx[ax[0]]
    r = 0
    for j in range(u16(sl)):
        e = u16(sl + 2 + 2 * j)
        rep = e >> 14
        if r <= want_row <= r + rep:
            ro = ptr(runs + 8 + 8 * (e & 0x3fff))
            v = u8(ro)
            o = ro + 1
            pos = 0
            while True:
                n = u8(o)
                if want_col < pos + n:
                    return 1 if v == 0 else 0
                pos += n
                o += 1
                if n != 255:
                    v ^= 0xff
        r += rep + 1
    return 1


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    data, info = convert(src, dst)
    print(dst, len(data), info)
    if '--check' in sys.argv:
        sys.path.insert(0, HERE)
        from vis3 import G, query
        import random
        g3 = G(src)
        p4 = Pak(dst)
        it = p4.items_of('VIS_CHEAT')[0]
        b = p4.pbase(it.page) + it.data_off
        mn = [p4.i32(b + 4 + 8 * i) for i in range(3)]
        mx = [p4.i32(b + 8 + 8 * i) for i in range(3)]
        rnd = random.Random(1)
        bad = 0
        for _ in range(20000):
            pt = [rnd.uniform(mn[i] - 3, mx[i] + 3) for i in range(3)]
            a, bq = query(g3, *pt), ps4_query(p4, *pt)
            bad += a != bq
        print('check: 20000 random points, mismatches', bad)
