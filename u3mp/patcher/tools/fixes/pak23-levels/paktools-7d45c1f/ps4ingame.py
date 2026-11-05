"""PS3 -> PS4 converter for Uncharted 3 '-ingame12' level data packages.

Supported items: TAG_INT, LEVEL_INFO_1 (empty), SPLINE_TABLE (empty), SPAWNER_GROUP, REGION_TREE,
NAV_MESH_1 (navmesh.py), FEATURE_DB_2 (same layout as the FG-collision trailer3, fgcoll.py).
Layouts were derived from PS3/PS4 rosetta pairs (san-upper-multi-stage, graveyard-obj-pak,
ldn-multi-comp-pak, yemen-airport-multi-pak, world-multiplayer ...):

General rules PS3 (BE, 4-byte pointers) -> PS4 (LE, 8-byte pointers):
  * pointers grow to 8 bytes and are 8-aligned; every separately pointed-to block is 16-aligned;
  * a {ptr, u32 count} list with count 0 becomes a null pointer;
  * 32-bit flag words are bitfields: PS4 value = bit-reversed PS3 value;
  * spawner positions store w = 0 on PS4 (1.0 on PS3); BVH boxes keep w;
  * 'u32 value' slots in tag records are padded with the 0x00012345 filler on PS4.

SPAWNER_GROUP  PS3 {ptr 0, ptr spawners[], u32 n, ptr sorted{u32 sid, ptr}[], u32 n}
Spawner (84 -> 144): pos f4, quat f4, ptr name, u32 sid, ptr, ptr, u32 x, ptr art, u32 flags,
  ptr group, ptr parent, ptr tags{ptr entries{u32 key, ptr value}[], u32 n, u32}, ptr, ptr, ptr
  value {u16 type, u16 size, ptr data}: type 1 int, 4 float, 13 sid (4 bytes), 8 vec4 (16),
  5 string (data = ptr to the string, size = strlen)
REGION_TREE    PS3 {ptr grid, ptr regions..., 0}
Grid (56/52): box min f4, max f4, u32 nx, ny, nz, ptr cells[nx*ny*nz+1]
  cell {ptr groups{u32 sid, ptr list{ptr regions[], u32 n}}[], u32 n}
Region (40 -> 80): ptr name, u32 sid, u16 x, u16 y, ptr A{u32,u32,u32,ptr,ptr}, ptr spawner,
  ptr C, ptr shape, u16 ntags, u16 z, ptr script, ptr tags{u32 key, u32 type, u32 value}[ntags]
  (type 3 = string pointer)
Shape: {ptr, ptr, u32, u16, u16} + floats (block extent)
Script (region trigger lambda): {u32 sid, ptr X, u32 0, u32 0};
  X {ptr code, ptr consts, u32 hash, u32 size, u32 0, code bytes (unchanged)..., consts[] (4 -> 8 bytes)}
"""
import struct, sys
from ndpak import Pak
from ps4writer import PakWriter, Blob, make_login_and_vram_page

FILLER = 0x12345
PADW = 0xaaaaaaaa   # PS4 struct padding fill
SKIP_SPLINES = False
IGNORE_OTHERS = False
CONVERT_NAV = True      # NAV_MESH_1 (navmesh.py) and FEATURE_DB_2 (fgcoll trailer3), byte-exact on retail pairs


def rev32(x):
    return int('{:032b}'.format(x)[::-1], 2)


class Conv:
    def __init__(self, src):
        self.p = src
        self.memo = {}          # (page, off) -> Blob
        self.blocks = []        # blobs to embed into the current item
        self.cuts = {}          # page -> sorted pointer targets (block extents)
        pt = {}
        for (pg, o), tp in src.ptrmap.items():
            pt.setdefault(tp, set()).add(src.pptr(pg, o))
        for it in src.items:
            pt.setdefault(it.page, set()).update({it.off, it.data_off, it.data_off + it.size})
        self.cuts = {k: sorted(v) for k, v in pt.items()}

    # ---- PS3 reads
    def u32(self, a, o=0): return self.p.pu32(a[0], a[1] + o)
    def u16(self, a, o=0): return struct.unpack_from('>H', self.p.d, self.p.pbase(a[0]) + a[1] + o)[0]
    def f32(self, a, o=0): return struct.unpack_from('>f', self.p.d, self.p.pbase(a[0]) + a[1] + o)[0]
    def raw(self, a, o, n): b = self.p.pbase(a[0]) + a[1] + o; return self.p.d[b:b + n]
    def ptr(self, a, o=0): return self.p.resolve(a[0], a[1] + o)

    def extent(self, a):
        """bytes from a to the next pointer target / item boundary on its page"""
        import bisect
        c = self.cuts[a[0]]
        i = bisect.bisect_right(c, a[1])
        return (c[i] if i < len(c) else len(self.p.d)) - a[1]

    def new(self, a, name=''):
        b = Blob(name)
        if a is not None:
            self.memo[a] = b
        self.blocks.append(b)
        return b

    # ---- generic pieces
    def string(self, a):
        if a is None:
            return None
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'str')
        b.cstr(self.p.pstr(*a))
        return b

    def ptr_or_null(self, blob, target):
        blob.ptr(target) if target is not None else blob.u64(0)

    # ---- SPAWNER_GROUP
    def spawner_group(self, it, root):
        a = (it.page, it.data_off)
        self.memo[a] = root
        assert self.ptr(a, 0) is None and self.u32(a, 0) == 0
        arr, n = self.ptr(a, 4), self.u32(a, 8)
        srt, n2 = self.ptr(a, 12), self.u32(a, 16)
        sp = [self.spawner(self.ptr(arr, 4 * k)) for k in range(n)]
        root.u64(0)
        ab = self.new(arr, 'sp[]')
        for b in sp:
            ab.ptr(b)
        root.ptr(ab).u32(n).u32(PADW)
        sb = self.new(srt, 'sorted')
        for k in range(n2):
            sb.u32(self.u32(srt, 8 * k)).u32(PADW).ptr(self.spawner(self.ptr(srt, 8 * k + 4)))
        root.ptr(sb).u32(n2).u32(PADW)

    def spawner(self, a):
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'spawner')
        x, y, z = (self.f32(a, 4 * i) for i in range(3))
        b.raw(struct.pack('<4f', x, y, z, 0.0))
        b.raw(struct.pack('<4f', *(self.f32(a, 16 + 4 * i) for i in range(4))))
        self.ptr_or_null(b, self.string(self.ptr(a, 32)))
        b.u32(self.u32(a, 36)).u32(PADW)
        for o in (40, 44):
            assert self.ptr(a, o) is None and self.u32(a, o) == 0, ('spawner field', hex(a[1]), o)
            b.u64(0)
        b.u32(self.u32(a, 48)).u32(PADW)
        self.ptr_or_null(b, self.string(self.ptr(a, 52)))
        b.u32(rev32(self.u32(a, 56))).u32(PADW)
        g = self.ptr(a, 60)
        b.ptr(self.memo[g]) if g is not None else b.u64(0)
        par = self.ptr(a, 64)
        self.ptr_or_null(b, self.spawner(par) if par is not None else None)
        self.ptr_or_null(b, self.spawner_tags(self.ptr(a, 68)))
        for o in (72, 76, 80):
            assert self.ptr(a, o) is None and self.u32(a, o) == 0, ('spawner field', hex(a[1]), o)
            b.u64(0)
        return b

    def spawner_tags(self, a):
        if a is None:
            return None
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'tags')
        tab, n, z = self.ptr(a, 0), self.u32(a, 4), self.u32(a, 8)
        if n == 0:
            b.u64(0)
        else:
            tb = self.new(tab, 'tagtab')
            for k in range(n):
                tb.u32(self.u32(tab, 8 * k)).u32(PADW).ptr(self.tag_value(self.ptr(tab, 8 * k + 4)))
            b.ptr(tb)
        assert z == 0
        b.u32(n).u32(PADW)
        return b

    def tag_value(self, a):
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'tagval')
        ty, sz = self.u16(a, 0), self.u16(a, 2)
        d = self.ptr(a, 4)
        b.u16(ty).u16(sz).u32(PADW)
        if d is None:
            b.u64(0)
            return b
        db = self.new(d, 'tagdata')
        if ty in (1, 4, 13):
            assert sz == 4
            db.u32(self.u32(d))
        elif ty == 8:
            assert sz == 16
            db.raw(struct.pack('<4f', *(self.f32(d, 4 * i) for i in range(4))))
        elif ty == 5:
            s = self.ptr(d, 0)
            self.ptr_or_null(db, self.string(s))
        else:
            raise ValueError('spawner tag type %d' % ty)
        b.ptr(db)
        return b

    # ---- REGION_TREE
    def region_tree(self, it, root):
        a = (it.page, it.data_off)
        self.memo[a] = root
        grid = self.ptr(a, 0)
        regs = []
        k = 4
        while self.ptr(a, k) is not None:
            regs.append(self.ptr(a, k))
            k += 4
        gb = self.grid(grid) if grid is not None else None
        rbs = [self.region(r) for r in regs]
        self.ptr_or_null(root, gb)
        for r in rbs:
            root.ptr(r)
        root.u64(0)

    def grid(self, a):
        b = self.new(a, 'grid')
        b.raw(struct.pack('<8f', *(self.f32(a, 4 * i) for i in range(8))))
        nx, ny, nz = self.u32(a, 32), self.u32(a, 36), self.u32(a, 40)
        b.u32(nx).u32(ny).u32(nz).u32(PADW)
        for k in range(nx * ny * nz + 1):
            self.ptr_or_null(b, self.cell(self.ptr(a, 44 + 4 * k)))
        return b

    def cell(self, a):
        if a is None:
            return None
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'cell')
        g, n = self.ptr(a, 0), self.u32(a, 4)
        if n == 0:
            b.u64(0)
        else:
            gb = self.new(g, 'groups')
            for k in range(n):
                gb.u32(self.u32(g, 8 * k)).u32(PADW).ptr(self.reglist(self.ptr(g, 8 * k + 4)))
            b.ptr(gb)
        b.u32(n).u32(PADW)
        return b

    def reglist(self, a):
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'reglist')
        r, n = self.ptr(a, 0), self.u32(a, 4)
        if n == 0:
            b.u64(0)
        else:
            rb = self.new(r, 'regptrs')
            for k in range(n):
                rb.ptr(self.region(self.ptr(r, 4 * k)))
            b.ptr(rb)
        b.u32(n).u32(PADW)
        return b

    def region(self, a):
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'region')
        self.ptr_or_null(b, self.string(self.ptr(a, 0)))
        b.u32(self.u32(a, 4)).u16(self.u16(a, 8)).u16(self.u16(a, 10))
        self.ptr_or_null(b, self.region_a(self.ptr(a, 12)))
        sp = self.ptr(a, 16)
        self.ptr_or_null(b, self.spawner(sp) if sp is not None else None)
        assert self.ptr(a, 20) is None and self.u32(a, 20) == 0, ('region C', hex(a[1]))
        b.u64(0)
        self.ptr_or_null(b, self.shape(self.ptr(a, 24)))
        ntags = self.u16(a, 28)
        b.u16(ntags).u16(self.u16(a, 30)).u32(PADW)
        sc = self.ptr(a, 32)
        self.ptr_or_null(b, self.script(sc) if sc is not None else None)
        t = self.ptr(a, 36)
        self.ptr_or_null(b, self.region_tags(t, ntags) if t is not None else None)
        return b

    def region_a(self, a):
        if a is None:
            return None
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'regA')
        b.u32(self.u32(a, 0)).u32(self.u32(a, 4)).u32(self.u32(a, 8)).u32(PADW)
        for o in (12, 16):
            assert self.ptr(a, o) is None and self.u32(a, o) == 0, ('regionA ptr', hex(a[1]))
            b.u64(0)
        return b

    def shape(self, a):
        if a is None:
            return None
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'shape')
        nxt = self.ptr(a, 0)                      # shapes of one region form a linked list
        self.ptr_or_null(b, self.shape(nxt) if nxt is not None else None)
        assert self.ptr(a, 4) is None and self.u32(a, 4) == 0, ('shape ptr', hex(a[1]))
        b.u64(0)
        b.u32(self.u32(a, 8)).u16(self.u16(a, 12)).u16(self.u16(a, 14)).u32(PADW).u32(PADW)
        n = self.extent(a) - 16
        assert n % 4 == 0
        b.raw(struct.pack('<%dI' % (n // 4), *struct.unpack('>%dI' % (n // 4), self.raw(a, 16, n))))
        return b

    def region_tags(self, a, n):
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'rtags')
        for k in range(n):
            o = 12 * k
            key, ty = self.u32(a, o), self.u32(a, o + 4)
            b.u32(key).u32(ty)
            if ty == 3:
                self.ptr_or_null(b, self.string(self.ptr(a, o + 8)))
            else:
                assert not self.p.is_ptr(a[0], a[1] + o + 8), ('region tag ptr', ty)
                b.u32(self.u32(a, o + 8)).u32(FILLER)
        return b

    def script(self, a):
        if a in self.memo:
            return self.memo[a]
        b = self.new(a, 'script')
        # {u32 sid, ptr lambda} + padding: the lambda usually follows directly (12 or 16 bytes on)
        x = self.ptr(a, 4)
        b.u32(self.u32(a, 0)).u32(PADW)
        xb = self.new(x, 'lambda')
        code, consts = self.ptr(x, 0), self.ptr(x, 4)
        assert code == (x[0], x[1] + 20), 'lambda layout'
        ncode = consts[1] - code[1]
        cb = self.new(code, 'code')
        cb.raw(self.raw(code, 0, ncode))          # bytecode is stored little-endian on both
        nconst = self.extent(consts) // 4
        kb = self.new(consts, 'consts')
        for k in range(nconst):
            if self.p.is_ptr(consts[0], consts[1] + 4 * k):
                self.ptr_or_null(kb, self.string(self.ptr(consts, 4 * k)))
            else:
                kb.u32(self.u32(consts, 4 * k)).u32(0)
        xb.ptr(cb).ptr(kb).u32(self.u32(x, 8)).u32(self.u32(x, 12)).u32(self.u32(x, 16)).u32(PADW)
        b.ptr(xb)
        return b


def convert(src_path, out_path):
    p = Pak(src_path)
    assert p.platform == 'ps3'
    c = Conv(p)
    w = PakWriter()
    page = w.page()
    order = ['TAG_INT', 'SPLINE_TABLE', 'FEATURE_DB_2', 'NAV_MESH_1', 'LEVEL_INFO_1', 'SPAWNER_GROUP',
             'REGION_TREE']
    if not CONVERT_NAV:
        order = [t for t in order if t not in ('FEATURE_DB_2', 'NAV_MESH_1')]
    items = {}
    extra = []          # self-contained items (nav meshes, feature db): (type, ps3 item, blob)
    for t in order:
        for it in p.items_of(t):
            root = Blob(t)
            c.blocks = []
            if t == 'TAG_INT':
                root.u32(c.u32((it.page, it.data_off)))
            elif t == 'LEVEL_INFO_1':
                assert it.size == 4, 'LEVEL_INFO_1 size %#x' % it.size
                root.u32(c.u32((it.page, it.data_off)))
            elif t == 'SPLINE_TABLE':
                if it.size == 4 and c.u32((it.page, it.data_off)) == 0:
                    root.u32(0)
                elif SKIP_SPLINES:
                    root.u32(0)
                else:
                    raise NotImplementedError('SPLINE_TABLE with splines')
            elif t == 'SPAWNER_GROUP':
                c.spawner_group(it, root)
            elif t == 'REGION_TREE':
                c.region_tree(it, root)
            elif t == 'NAV_MESH_1':
                import navmesh
                extra.append((t, it, navmesh.nav_blob(p, it)))
                continue
            elif t == 'FEATURE_DB_2':
                import fgcoll
                fw = fgcoll.W()
                fgcoll.w_tr3(fw, fgcoll.read_tr3(fgcoll.R(p), (it.page, it.data_off)))
                fw.pad(8, 0)
                extra.append((t, it, fw.b))
                continue
            items[t] = (it, root, list(c.blocks))
    # spawners referenced by regions were created while converting REGION_TREE: they belong to the
    # SPAWNER_GROUP blocks (memo shares them), so embed each blob only once, in the item that made it
    done = set()
    made = []
    for t, it, blob in extra:
        made.append(page.item(t, blob, name=it.name))
    for t in order:
        if t not in items:
            continue
        it, root, blocks = items[t]
        for b in blocks:
            if id(b) in done:
                continue
            done.add(id(b))
            root.embed(b, 16, 0xaa)
        root.pad(4, 0xaa)
        made.append(page.item(t, root, name=it.name))
    others = [i.type for i in p.items if i.type not in order + ['PAK_LOGIN_TABLE', 'VRAM_DESC_TABLE']]
    assert not others or IGNORE_OTHERS, others
    p1 = w.page()
    login = sorted(made, key=lambda i: i.type)
    make_login_and_vram_page(w, p1, login, [])
    data = w.build()
    open(out_path, 'wb').write(data)
    return data


if __name__ == '__main__':
    convert(sys.argv[1], sys.argv[2])
