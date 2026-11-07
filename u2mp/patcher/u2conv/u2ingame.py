"""Uncharted 2 '-ingame10' / '-npc' level data packages: PS3 -> PS4 with the U3 converter paktools/ps4ingame.py.

Differences to U3 (found on the shared multiplayer pairs village-multi-ingame10, stage-multi-ingame10,
gompa-multi-npc-ingame10, sky-village-multi-ingame10; check: validate.py ingame):
  * region scripts: U2 regions point at two {u32 sid, ptr lambda} entries (U3: one entry plus zeros);
    lambda = {ptr body, u32 sid} (PS4: {ptr body, pad}); body = {ptr code, ptr consts, u32 hash, u32 tag}
    (PS4: {ptr code, ptr consts, u32 hash, pad x3}); the code bytes are copied, a trailing word equal to the
    body's tag becomes padding on PS4; constants widen from 4 to 8 bytes as in U3.
  * NAV_MESH_1: U2 layout (no 'NavM' header), converted by u2nav.py (installed as navmesh.nav_blob).
  * the animated-actor list ps4ingame consults (spawner tag 0x66c6d3fe) comes from the U2 PS4 actor27 folder.
  * FEATURE_DB_2: U2 has two element arrays and two trees (U3: three and three).
      PS3 0x1c {ptr A, nA, ptr B, nB, u32 u, ptr T0, ptr T1}
      PS4 0x40 {ptr A, nA, aa, ptr B, nB, aa, u32 u, 0, ptr T0, ptr T1, aa aa}
      A elements 0x40 (words 14, 15 are u16 pairs, as U3), B elements 0x34 (U3 0x38; word 12 a u16 pair);
      tree nodes as U3 (fgcoll read_node / w_node).  The Havok tables fgcoll imports are not needed.
"""
import os
import sys
import types

import u2paktools

u2paktools.setup()
if 'hkconvert' not in sys.modules:          # fgcoll imports it at module level; FEATURE_DB_2 never calls it
    try:
        import hkconvert  # noqa: F401
    except Exception:
        sys.modules['hkconvert'] = types.ModuleType('hkconvert')
import ps4ingame  # noqa: E402
import fgcoll  # noqa: E402

U2_ELEM = [(0x40, (14, 15)), (0x34, (12,))]


def read_fdb2(r, a):
    t = dict(arrs=[], u=r.u32(a, 0x10), trees=[])
    for k, (sz, _) in enumerate(U2_ELEM):
        cnt = r.u32(a, 8 * k + 4)
        t['arrs'].append((cnt, r.raw(r.ptr(a, 8 * k), 0, sz * cnt) if cnt else b''))
    for o in (0x14, 0x18):
        x = r.ptr(a, o)
        t['trees'].append(fgcoll.read_node(r, x) if x else None)
    return t


def write_fdb2(w, t):
    w.pad()
    o = w.reserve(0x40)
    for k in range(2):
        w.put(o + 0x10 * k + 0xc, 'I', fgcoll.AA)
    w.put(o + 0x20, 'I', t['u'])
    w.put(o + 0x38, 'II', fgcoll.AA, fgcoll.AA)
    for k, (cnt, data) in enumerate(t['arrs']):
        w.put(o + 0x10 * k + 8, 'I', cnt)
        if cnt:
            a = w.pos
            sz, pairs = U2_ELEM[k]
            for i in range(cnt):
                w.b.raw(fgcoll.swap_words(data[sz * i:sz * (i + 1)], pairs))
            w.pad()
            w.ptr_at(o + 0x10 * k, a)
    for k, tr in enumerate(t['trees']):
        if tr is not None:
            w.ptr_at(o + 0x28 + 8 * k, fgcoll.w_node(w, tr))
    return o


fgcoll.read_tr3 = read_fdb2         # ps4ingame's FEATURE_DB_2 branch calls these two
fgcoll.w_tr3 = write_fdb2

PADW = ps4ingame.PADW


def _script(self, a):
    if a in self.memo:
        return self.memo[a]
    b = self.new(a, 'script')
    for k in range(2):
        sid = self.u32(a, 8 * k)
        x = self.ptr(a, 8 * k + 4)
        b.u32(sid).u32(PADW)
        self.ptr_or_null(b, _lambda(self, x) if x is not None else None)
    return b


def _lambda(self, x):
    if x in self.memo:
        return self.memo[x]
    lb = self.new(x, 'lambda')
    body = self.ptr(x, 0)
    self.ptr_or_null(lb, _body(self, body) if body is not None else None)
    lb.u32(PADW).u32(PADW)
    return lb


def _body(self, y):
    if y in self.memo:
        return self.memo[y]
    bb = self.new(y, 'lambda-body')
    code, consts = self.ptr(y, 0), self.ptr(y, 4)
    h, tag = self.u32(y, 8), self.u32(y, 12)
    cbk = None
    if code is not None:
        end = consts[1] if consts is not None and consts[0] == code[0] and consts[1] > code[1] else None
        n = (end - code[1]) if end is not None else self.extent(code)
        cd = bytearray(self.raw(code, 0, n))
        if len(cd) >= 4 and int.from_bytes(cd[-4:], 'big') == tag:
            cd[-4:] = b'\xaa' * 4
        cbk = self.new(code, 'code')
        cbk.raw(bytes(cd))
    kb = None
    if consts is not None:
        kb = self.new(consts, 'consts')
        for k in range(self.extent(consts) // 4):
            if self.p.is_ptr(consts[0], consts[1] + 4 * k):
                self.ptr_or_null(kb, self.string(self.ptr(consts, 4 * k)))
            else:
                kb.u32(self.u32(consts, 4 * k)).u32(0)
    self.ptr_or_null(bb, cbk)
    self.ptr_or_null(bb, kb)
    bb.u32(h).u32(PADW).u32(PADW).u32(PADW)
    return bb


ps4ingame.Conv.script = _script


def set_anim_arts(names):
    """names of PS4 actor paks that carry ANIM items (see ps4ingame.NO_ASYNC_TAG)"""
    ps4ingame._ANIM_ARTS = set(names)


def anim_arts_from(actor_dir, cache=None):
    """scan the PS4 actor27/ps4 folder for paks with ANIM items (cached as a name list)"""
    import json
    if cache and os.path.exists(cache):
        try:
            return json.load(open(cache))
        except ValueError:
            pass
    from ndpak import Pak
    out = []
    for f in sorted(os.listdir(actor_dir)):
        if f.endswith('.pak'):
            try:
                if Pak(os.path.join(actor_dir, f)).items_of('ANIM'):
                    out.append(f[:-4])
            except Exception:
                pass
    if cache:
        json.dump(out, open(cache, 'w'))
    return out


from u2nav import NavMeshUnsupported  # noqa: E402,F401  (raised for nav mesh layouts u2nav does not know)


def convert(src, dst, drop_navmesh=False):
    """-> list of notes.  NAV_MESH_1 items are converted by u2nav (NavMeshUnsupported on an unknown layout);
    drop_navmesh leaves them out instead."""
    import navmesh
    import u2nav
    from ndpak import Pak
    if ps4ingame._ANIM_ARTS is None:
        ps4ingame._ANIM_ARTS = set()
    navmesh.nav_blob = u2nav.nav_blob
    notes = []
    p = Pak(src)
    nnav = len(p.items_of('NAV_MESH_1'))
    if nnav and drop_navmesh:
        class NoNavPak(Pak):
            def items_of(self, t):
                return [] if t == 'NAV_MESH_1' else super().items_of(t)
        old = ps4ingame.Pak
        ps4ingame.Pak = NoNavPak
        try:
            ps4ingame.convert(src, dst)
        finally:
            ps4ingame.Pak = old
        notes.append('%d NAV_MESH_1 left out (drop_navmesh)' % nnav)
        return notes
    ps4ingame.convert(src, dst)
    return notes
