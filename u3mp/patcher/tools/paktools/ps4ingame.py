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
CONVERT_NAV = True
# Debris-family objects (SimpleBreakable, DebrisObject, RockFall, SlidingSand: their 'needs anim data' vfunc
# 0x578440 reads spawner tag 0x5d4729de) only get their anim data block (object +0x148) when that tag is set,
# yet the per-frame update (0x10bd000 -> 0x10b39e0) dereferences it as soon as an anim layer plays.  An art
# group with an ANIM ('idle') plays it, so on PS4 such a spawner without the tag crashes (Gompa,
# gompa-destruct-rail-2x1: read of 0 at 0x10b3b7f).  Retail PS4 sets the tag (int 1) on its animated
# objects; the converter adds it to debris-family spawners whose art has an ANIM item.
# Spawner int tag 0x66c6d3fe != 0 makes object init call vfunc 0x168 (anim-control DisableAsyncUpdate, sets
# +0x228 bit 2), which skips the anim data block (+0x148); the per-frame update still calls vfunc 0x1b8
# (0x10b39e0 / 0x10bc8a0) once an anim layer plays and that dereferences +0x148 -> null read at 0x10b3b7f.
# Only PS3 gompa-multi sets it (20 banners/lanterns with ANIM 'idle'); every retail PS4 spawner of an animated
# art has it = 0, so the converter writes 0 for spawners whose art has an ANIM item.
NO_ASYNC_TAG = 0x66c6d3fe
NEUTRALIZE_ARTS = set()      # debugging only: spawners of these arts become type-0 markers (nothing spawns)
DEBRIS_TYPES = {0x8b7d6f1a, 0x775bef07, 0x035f6332, 0x69de5580}
ANIM_DATA_TAG = 0x5d4729de
_ANIM_ARTS = None


def anim_arts():
    global _ANIM_ARTS
    if _ANIM_ARTS is None:
        import glob, json, os
        here = os.path.dirname(os.path.abspath(__file__))
        s = set(json.load(open(os.path.join(here, 'data', 'ps4_anim_actors.json'))))
        cf = os.path.join(here, 'data', 'conv_anim_actors.json')     # our converted animated actors (portable copy)
        if os.path.exists(cf):
            s |= set(json.load(open(cf)))
        for f in glob.glob('/mnt/user-data/outputs/**/actor33/**/*.pak', recursive=True):
            try:
                if Pak(f).items_of('ANIM'):
                    s.add(os.path.basename(f)[:-4])
            except Exception:
                pass
        _ANIM_ARTS = s
    return _ANIM_ARTS      # NAV_MESH_1 (navmesh.py) and FEATURE_DB_2 (fgcoll trailer3), byte-exact on retail pairs


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

    # ---- SPLINE_TABLE (retail pair cha-forest-drop-ingame12: byte-exact)
    def spline_table(self, it, root):
        """PS3 {u32 n, n x rec(0x30)} -> PS4 {u32 n, aa pad to 16, n x rec(0x60)}
        rec PS3 {sid, ptr 0, sid2, u32, u32 nA, ptr A{u32 x3}[nA], u32 nPts, ptr pts vec4[nPts], u32 nSamp,
                 ptr samples f32[nSamp*nPts], ptr lens f32[nPts], ptr extra}
        PS4 {sid, aa, ptr 0, sid2, u32, u32 nA, aa, ptr A{u32 x3, 0x12345}, nPts, aa, ptr pts, nSamp, aa,
             ptr samples f32[nSamp*nSeg], ptr lens f32[nSeg], ptr extra} = 0x58; nSeg = nPts-1 (PS3 pads the
             last segment with zeros) unless the last PS3 segment is non-zero (closed spline).
        extra {u32 nVal, u32 nTags, ptr tags{sid, type, ptr data u32[nVal]}[nTags]} -> {nVal, nTags, ptr},
             tags 16 bytes."""
        a = (it.page, it.data_off)
        n = self.u32(a)
        root.u32(n).u32(PADW).u32(PADW).u32(PADW)
        recs = []
        for k in range(n):
            r = (a[0], a[1] + 4 + 0x30 * k)
            W = [self.u32(r, 4 * i) for i in range(12)]
            recs.append((r, W))
        for r, W in recs:
            nA, nP, nS = W[4], W[6], W[8]
            root.u32(W[0]).u32(PADW)
            par = self.ptr(r, 4)                  # owning spawner (cargo-plane-in-air-2), else null
            self.ptr_or_null(root, self.spawner(par) if par is not None else None)
            root.u32(W[2]).u32(W[3]).u32(nA).u32(PADW)
            A = self.ptr(r, 0x14)                 # tags {key, type, value | string*} like region tags
            self.ptr_or_null(root, self.region_tags(A, nA) if A is not None else None)
            P = self.ptr(r, 0x1c)
            pb = None
            if P is not None:
                pb = self.new(P, 'splPts')
                pb.raw(struct.pack('<%df' % (4 * nP), *struct.unpack('>%df' % (4 * nP), self.raw(P, 0, 16 * nP))))
            root.u32(nP).u32(PADW)
            self.ptr_or_null(root, pb)
            S = self.ptr(r, 0x24)
            L = self.ptr(r, 0x28)
            nseg = nP
            if S is not None and nP:
                last = self.raw(S, 4 * nS * (nP - 1), 4 * nS)
                if not any(last):                 # the PS3 lens word past the end may be garbage
                    nseg = nP - 1
            sb = lb = None
            if S is not None:
                sb = self.new(S, 'splSamp')
                sb.raw(struct.pack('<%dI' % (nS * nseg), *struct.unpack('>%dI' % (nS * nseg), self.raw(S, 0, 4 * nS * nseg))))
            root.u32(nS).u32(PADW)
            self.ptr_or_null(root, sb)
            if L is not None:
                lb = self.new(L, 'splLen')
                lb.raw(struct.pack('<%dI' % nseg, *struct.unpack('>%dI' % nseg, self.raw(L, 0, 4 * nseg))))
            self.ptr_or_null(root, lb)
            X = self.ptr(r, 0x2c)
            xb = None
            if X is not None:
                xb = self.new(X, 'splX')
                nv, nt = self.u32(X), self.u32(X, 4)
                xb.u32(nv).u32(nt)
                T = self.ptr(X, 8)
                tb = None
                if T is not None:
                    tb = self.new(T, 'splXtags')
                    datas = []
                    for j in range(nt):
                        q = (T[0], T[1] + 12 * j)
                        ty = self.u32(q, 4)
                        tb.u32(self.u32(q)).u32(ty)
                        D = self.ptr(q, 8)
                        datas.append((tb, tb.pos, D))
                        tb.u64(0)
                    for j, (blob, pos, D) in enumerate(datas):
                        if D is None:
                            continue
                        if self.u32((T[0], T[1] + 12 * j), 4) == 3:
                            # string tag (highrise-multi-pak): per-vertex string pointers, widened to 64-bit like
                            # every other PS4 string pointer slot
                            db = self.new(D, 'splXstr')
                            for k in range(nv):
                                self.ptr_or_null(db, self.string(self.ptr(D, 4 * k)))
                        else:
                            db = self.new(D, 'splXdata')
                            db.raw(struct.pack('<%dI' % nv, *struct.unpack('>%dI' % nv, self.raw(D, 0, 4 * nv))))
                        blob.relocs.append((pos, db, 0))
                self.ptr_or_null(xb, tb)
            self.ptr_or_null(root, xb)           # record stride 0x58 (cargo-plane-in-air-2: 3 records)

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
        _art = self.ptr(a, 52)
        _artn = self.p.pstr(*_art) if _art else None
        b.u32(0 if _artn in NEUTRALIZE_ARTS else self.u32(a, 48)).u32(PADW)   # bisect builds: type 0 = marker
        self.ptr_or_null(b, self.string(self.ptr(a, 52)))
        b.u32(rev32(self.u32(a, 56))).u32(PADW)
        g = self.ptr(a, 60)
        b.ptr(self.memo[g]) if g is not None else b.u64(0)
        par = self.ptr(a, 64)
        self.ptr_or_null(b, self.spawner(par) if par is not None else None)
        art = self.ptr(a, 52)
        artname = self.p.pstr(*art) if art else None
        add = None
        if self.u32(a, 48) in DEBRIS_TYPES and artname in anim_arts():
            add = (ANIM_DATA_TAG, 1)
            self.added_tags = getattr(self, 'added_tags', []) + [artname]
        zero = None
        if artname in anim_arts() and self.int_tag(self.ptr(a, 68), NO_ASYNC_TAG):
            zero = NO_ASYNC_TAG
            self.zeroed_tags = getattr(self, 'zeroed_tags', []) + [artname]
        self.ptr_or_null(b, self.spawner_tags(self.ptr(a, 68), add=add, zero=zero))
        for o in (72, 76, 80):
            assert self.ptr(a, o) is None and self.u32(a, o) == 0, ('spawner field', hex(a[1]), o)
            b.u64(0)
        return b

    def int_tag(self, a, key):
        """value of int tag `key` in PS3 spawner tag list a (None when absent)"""
        if a is None:
            return None
        tab, n = self.ptr(a, 0), self.u32(a, 4)
        for k in range(n):
            if self.u32(tab, 8 * k) == key:
                v = self.ptr(tab, 8 * k + 4)
                if v is None or self.u16(v, 0) != 1:
                    return None
                d = self.ptr(v, 4)
                return self.u32(d, 0) if d is not None else None
        return None

    def spawner_tags(self, a, add=None, zero=None):
        if add is None and zero is None:
            if a is None:
                return None
            if a in self.memo:
                return self.memo[a]
        ents = []
        if a is not None:
            tab, n, z = self.ptr(a, 0), self.u32(a, 4), self.u32(a, 8)
            assert z == 0
            ents = [(self.u32(tab, 8 * k), self.ptr(tab, 8 * k + 4)) for k in range(n)]
        if add is not None and any(k == add[0] for k, _ in ents):
            add = None
        fresh = add is not None or zero is not None
        b = self.new(None if fresh else a, 'tags')
        if add is not None:
            ents = sorted(ents + [(add[0], ('int', add[1]))], key=lambda e: e[0])   # binary-searched by key
        if zero is not None:
            ents = [(k, ('int', 0) if k == zero else v) for k, v in ents]
        if not ents:
            # PS3 keeps a real (empty) array behind an empty table; the PS4 MP spawn filter (0x72fbc0) binary-searches
            # and then reads data[0] without a count check -> never write data = null: point at one terminator entry
            # whose key sorts above every tag (retail writes null, but only on spawners that filter never reaches)
            tb = self.new(None, 'tagterm')
            tb.u32(0xffffffff).u32(PADW).u64(0)
            b.ptr(tb)
        else:
            tb = self.new(tab if (a is not None and not fresh) else None, 'tagtab')
            for key, v in ents:
                if isinstance(v, tuple) and v and v[0] == 'int':
                    vb = self.new(None, 'tagval'); db = self.new(None, 'tagdata')
                    db.u32(v[1]); vb.u16(1).u16(4).u32(PADW).ptr(db)
                    tb.u32(key).u32(PADW).ptr(vb)
                else:
                    tb.u32(key).u32(PADW).ptr(self.tag_value(v))
            b.ptr(tb)
        b.u32(len(ents)).u32(PADW)
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
        # bytecode is stored little-endian on both, 4-byte instructions {op, a, b, c}.  PS4 registers are
        # 64-bit: retail rewrites LoadStaticI32Imm (0x3f) as LoadStaticU32Imm (0x44) so symbol ids are
        # zero-extended (opcode names from the PS4 disassembler 0x11f6060; 3 retail ingame12 pairs)
        cd = bytearray(self.raw(code, 0, ncode))
        for i in range(0, len(cd) - 3, 4):
            if cd[i] == 0x3f:
                cd[i] = 0x44
        cb.raw(bytes(cd))
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
    order = ['TAG_INT', 'SPLINE_TABLE', 'FEATURE_DB_2', 'NAV_MESH_1', 'LEVEL_INFO_1', 'LEVEL_OFFSET_1', 'SPAWNER_GROUP',
             'REGION_TREE']
    if not CONVERT_NAV:
        order = [t for t in order if t not in ('FEATURE_DB_2', 'NAV_MESH_1')]
    items = {}
    extra = []          # self-contained items (nav meshes, feature db): (type, ps3 item, blob)
    # convert spawners / regions first: splines may point at a spawner (memo shares the blob)
    for t in sorted(order, key=lambda t: {'SPAWNER_GROUP': 0, 'REGION_TREE': 1}.get(t, 2)):
        for it in p.items_of(t):
            root = Blob(t)
            c.blocks = []
            if t == 'TAG_INT':
                root.u32(c.u32((it.page, it.data_off)))
            elif t == 'LEVEL_INFO_1':
                assert it.size == 4, 'LEVEL_INFO_1 size %#x' % it.size
                root.u32(c.u32((it.page, it.data_off)))
            elif t == 'LEVEL_OFFSET_1':
                # level placement offset, 3 floats; PS4 login 0x10e3e5f reads them at item data +0/+4/+8 and moves
                # the whole level (background included) by it -- dropping it left co-op London's sub-levels at
                # the origin
                assert it.size == 12, 'LEVEL_OFFSET_1 size %#x' % it.size
                for k in range(3):
                    root.raw(p.d[p.pbase(it.page) + it.data_off + 4 * k:p.pbase(it.page) + it.data_off + 4 * k + 4][::-1])
            elif t == 'SPLINE_TABLE':
                if it.size == 4 and c.u32((it.page, it.data_off)) == 0:
                    root.u32(0)
                elif SKIP_SPLINES:
                    root.u32(0)
                else:
                    c.spline_table(it, root)
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
