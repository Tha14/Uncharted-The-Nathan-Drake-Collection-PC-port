#!/usr/bin/env python3
"""Apply the Lab maps 34 / 34b / 37 modes and the 24th Classic map to work/overlay-r3.

  python apply_maps.py            apply to the overlay, then verify
  python apply_maps.py --verify   verify an already-applied overlay
  python apply_maps.py --sim DIR  apply to a scratch copy (only net.bin, modules.bin, pak23_ps4.txt and the
                                  copied paks go to DIR; the overlay is only read)

Starts from the CURRENT overlay files (append-only). Originals go to work/backup-maps once.
Needs net18.bin (PS3 records). Stdlib only: lib/ holds copies of dc.py, dclib.py, common.py, dcedit.py, ndpak.py.
Refuses to run twice. If net.bin is later rebuilt (lab_reg.py, classic.py, sk7.py start from their own backups),
this result is lost: restore from backup or run this script again afterwards.
Recipe 1.14: a table in PS3 1.19 order (step netgame-order-119, dctools/ng119.py) already has these netgames and
playlists as on PS3: --dc-only exits 0 'already applied', and verify() checks it against net18.bin instead of the
PC-order rules (<= 256 netgames, unique names, 24 Classic, Lab playlist 14).
"""
import argparse, json, os, re, shutil, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[0:0] = [os.path.join(HERE, 'lib'), os.path.join(HERE, '..', 'dctools')]   # lib/ (old layout) or the dctools job
from common import DC, sid
from dcedit import DCEdit
from ndpak import Pak
try:
    import ng119                    # dctools job (recipe 1.14); missing in the old lib/ layout
except ImportError:
    ng119 = None
WORK = "{{PROJECT}}/work/"
OVER = WORK + "overlay-r3/u3data/build/main/"
RETAIL = "{{PS4}}/u3data/build/main/"
NET18 = "{{PS3_DATA2}}/net18.bin"
BK = WORK + "backup-maps/"
FILES = HERE + '/files/'
MAPS, NG, PL = 0xdec97a4, 0x3b9a067d, 0xc1b0be0d
LIST_T, NG_OBJ, NG_SUB = 0xd14c6ffc, 0xf0670de8, 0x8b617b51
NET_SIZE_OFF = 192352
PL_LAB, PL_CLASSIC = 0x5242014b, 0xaac6efd8
CLASSIC_NEW, CLASSIC_MAP, CLASSIC_TWIN3, CLASSIC_TWIN4 = 0xebdaa7b7, 0xdd1b3f0f, 0xc4e6ad46, 0x43109556
LAB_MODES = [(0x7eeec759, 0x17d1af73), (0xcaffc253, 0xc77d7f07), (0xe5592a17, 0x195b0985)]
LAB_PLAYLIST = [(0xe5592a17, 10), (0xcaffc253, 10)]     # PS3 Lab playlist; 0x7eeec759 is not in it on PS3
TS = ['texturedict1/chunk-41-tstrm', 'texturedict1/chunk-41-tstrm-1', 'texturedict1/chunk-42-tstrm', 'texturedict1/chunk-43-tstrm',
      'texturedict1/common-tstrm', 'texturedict1/common-tstrm-1', 'texturedict1/common-tstrm-2', 'texturedict1/common-tstrm-3',
      'texturedict1/common-tstrm-4']
SUB34 = ['lab-multi-34-pak', 'lab34-stage', 'lab34-spawners', 'sky-village-multi', 'multi-all']
SUBLISTS = {0x17d1af73: SUB34 + ['chat-obj-pak', 'yemen-sound'], 0xc77d7f07: SUB34 + ['cha-sound-forest', 'yemen-sound']}
MAN = json.load(open(FILES + 'manifest.json')) if os.path.exists(FILES + 'manifest.json') else None   # from tools/build_paks.py


def table(c, s):
    o = c.bysid[s][1]
    return o, c.u32(o), c.ptr(o + c.ps)


def build_net(cur):
    b = DC(NET18); a = DC(cur); e = DCEdit(cur); strs = {}

    def string(s):
        if s in strs: return strs[s]
        enc = s.encode('latin1') + b'\0'; d = a.d; pos = d.find(enc)
        while 0 <= pos < a.reloc:
            if pos == 0 or d[pos - 1] == 0: break
            pos = d.find(enc, pos + 1)
        if pos < 0 or pos >= a.reloc: pos = e.add_string(s)
        strs[s] = pos; return pos

    def obj(t, size):
        e.align(8); e.d += struct.pack('<II', 0, t); o = len(e.d); e.d += bytes(size); return o

    def obj4(t, w):
        e.align(4); e.d += struct.pack('<I', t); o = len(e.d); e.d += struct.pack('<%dI' % len(w), *w); return o

    # map sub-level lists: PS3 reused campaign level names for the Stadium pieces; PS4 has the real campaign
    # data under those names, so the pieces live in new levels (lab34-*).  Map 37 keeps its PS3 list.
    _, nm, am = table(a, MAPS); idx = {a.u32(am + i * 0x98): i for i in range(nm)}
    for mid, sub in SUBLISTS.items():
        items = e.alloc(16 * len(sub), 8)
        for k, s in enumerate(sub):
            e.putptr(items + 16 * k, string(s)); struct.pack_into('<I', e.d, items + 16 * k + 8, sid(s))
        lst = obj(LIST_T, 16); struct.pack_into('<I', e.d, lst, len(sub)); e.putptr(lst + 8, items)
        e.putptr(am + idx[mid] * 0x98 + 0x58, lst)
    # netgames
    o4, n4, ar4 = table(a, NG); _, n3, ar3 = table(b, NG)
    ids4 = {a.u32(ar4 + i * 0xd0): i for i in range(n4)}
    idx3 = {}
    for i in range(n3): idx3.setdefault(b.u32(ar3 + i * 0xe4), i)
    if CLASSIC_NEW in ids4: raise SystemExit('already applied: %08x is in net.bin' % CLASSIC_NEW)
    new = len(LAB_MODES) + 1
    assert n4 + new <= 256, 'netgame index limit'
    na = e.alloc(0xd0 * (n4 + new), 16); e.copy_words(ar4, na, 0xd0 * n4)
    for j, (s, mp) in enumerate(LAB_MODES):           # same field map as dctools/lab_reg.py
        src = ar3 + idx3[s] * 0xe4; g = na + 0xd0 * (n4 + j)
        assert b.u32(src + 4) == mp and mp in idx
        for k3 in range(0, 0xe4, 4):
            if b.isptr(src + k3) and k3 != 0x74: raise SystemExit('ptr %x' % k3)
        for k in range(0, 0x48, 4): struct.pack_into('<I', e.d, g + k, b.u32(src + k))
        for k3, k4 in ((0x50, 0x58), (0x54, 0x5c), (0x58, 0x60), (0x5c, 0x64), (0x60, 0x68), (0x64, 0x6c), (0x78, 0x78), (0x7c, 0x7c)) + tuple((k, k + 4) for k in range(0x84, 0xbc, 4)):
            struct.pack_into('<I', e.d, g + k4, b.u32(src + k3))
        dropped = {hex(k): b.u32(src + k) for k in (0x6c, 0x70) + tuple(range(0xc4, 0xe4, 4)) if b.u32(src + k)}; assert not dropped, dropped
        t3 = b.ptr(src + 0x74); assert b.u32(t3 - 4) == NG_OBJ; subs = []
        for k in range(3):
            r = b.ptr(t3 + 4 * k); assert b.u32(r - 4) == NG_SUB; subs.append(obj4(NG_SUB, [b.u32(r + 4 * w) for w in range(9)]))
        t4 = obj(NG_OBJ, 24)
        for k in range(3): e.putptr(t4 + 8 * k, subs[k])
        e.putptr(g + 0x70, t4)
    # 24th Classic: clone of Classic Graveyard (same PS3 fields apart from id and map)
    twin3, cl3 = ar3 + idx3[CLASSIC_TWIN3] * 0xe4, ar3 + idx3[CLASSIC_NEW] * 0xe4
    for k in range(8, 0xe4, 4):
        if k != 0x74: assert b.u32(twin3 + k) == b.u32(cl3 + k), 'classic differs from twin at %x' % k
    r1, r2 = b.ptr(twin3 + 0x74), b.ptr(cl3 + 0x74)
    for k in range(3):
        p1, p2 = b.ptr(r1 + 4 * k), b.ptr(r2 + 4 * k)
        assert [b.u32(p1 + 4 * w) for w in range(9)] == [b.u32(p2 + 4 * w) for w in range(9)], 'classic sub table differs'
    twin4 = ar4 + ids4[CLASSIC_TWIN4] * 0xd0; g = na + 0xd0 * (n4 + len(LAB_MODES))
    assert a.u32(twin4 + 0xc) == 0x13 and a.u32(twin4 + 0x2c) == 0xd932420c and a.u32(twin4 + 0x64) == 0x1ad223b4
    e.copy_words(twin4, g, 0xd0)
    struct.pack_into('<II', e.d, g, CLASSIC_NEW, CLASSIC_MAP)
    assert CLASSIC_MAP in idx
    e.put32(o4, n4 + new); e.putptr(o4 + 8, na)
    # playlists
    _, np4, ap4 = table(a, PL); _, np3, ap3 = table(b, PL)
    weight = None
    for i in range(np3):
        m3 = ap3 + i * 0x5c
        if b.u32(m3) == PL_CLASSIC:
            l3 = b.ptr(m3 + 0x2c); it3 = b.ptr(l3 + 4)
            weight = [b.u32(it3 + k * 8 + 4) for k in range(b.u32(l3)) if b.u32(it3 + k * 8) == CLASSIC_NEW][0]
    assert weight is not None

    def extend(pid, add):
        for i in range(np4):
            m = ap4 + i * 0x68
            if a.u32(m) == pid:
                l = a.ptr(m + 0x30); n = a.u32(l); it = a.ptr(l + 8)
                old = [(a.u32(it + k * 8), a.u32(it + k * 8 + 4)) for k in range(n)]
                assert not set(s for s, _ in add) & set(s for s, _ in old)
                pl = old + add; ni = e.alloc(8 * len(pl), 8)
                for k, (s, w) in enumerate(pl): struct.pack_into('<II', e.d, ni + 8 * k, s, w)
                ll = obj(LIST_T, 16); struct.pack_into('<I', e.d, ll, len(pl)); e.putptr(ll + 8, ni)
                e.putptr(m + 0x30, ll); return len(pl)
        raise SystemExit('no playlist %08x' % pid)
    nl, nc = extend(PL_LAB, LAB_PLAYLIST), extend(PL_CLASSIC, [(CLASSIC_NEW, weight)])
    print('playlists: Lab', nl, 'Classic', nc, 'weight', weight, 'netgames', n4 + new)
    return e.build(), sorted(set(e.patched))


def build_text(raw):
    assert raw.count(b'\r\n') == raw.count(b'\n')
    t = raw.decode('latin1')
    for n in ('lab34-stage', 'lab34-spawners'): assert 'level-name %s\r\n' % n not in t
    old = 'actor mixer-map-middle\r\nactor mixer-map-side\r\n'; assert t.count(old) == 1
    t = t.replace(old, ''.join('actor %s\r\n' % n for n in MAN['boxes']))
    new = []
    for lv, pk, ac in (('lab34-stage', ['lab34-stage', 'lab34-stage-ingame'], []),
                       ('lab34-spawners', ['lab34-spawners-ingame'], MAN['sd'])):
        new.append('\r\n'.join(['level-name ' + lv, 'level-id ' + lv] + ['package ' + p for p in pk] + ['actor ' + x for x in ac] + ['package-ts ' + s for s in TS]))
    return (t + ('' if t.endswith('\r\n') else '\r\n') + '\r\n'.join(new) + '\r\n').encode('latin1')


def paks():
    out = []
    for sub in ('actor33/ps4', 'pak23/ps4'):
        for f in sorted(os.listdir(FILES + sub)):
            out.append(sub + '/' + f)
    return out


def blocks(t):
    t = t.replace('\r', ''); B = {}
    for x in re.split(r'\n(?=level-name )', t.rstrip('\n')):
        B[re.search(r'^level-id (\S+)', x, re.M).group(1)] = x
    return B


def exists(root, rel):
    return any(os.path.exists(d + rel) for d in (root, OVER, RETAIL))


def ps3_order(net):
    """the netgame table is in PS3 1.19 order (made by netgame-order-119): leave it alone"""
    return ng119 is not None and ng119.is_ps3_order(net, NET18, DC)


def verify(root, base=None, patched=()):
    err = []; net = root + 'dc1/ps4/net.bin'; c = DC(net); d = c.d
    ps3 = ps3_order(net)
    size = struct.unpack_from('<Q', open(root + 'dc1/ps4/modules.bin', 'rb').read(), NET_SIZE_OFF)[0]
    if size != len(d): err.append('modules.bin net size %d != %d' % (size, len(d)))
    n = struct.unpack_from('<I', d, c.reloc)[0]
    if c.reloc + 4 + n != len(d): err.append('net.bin container size')
    for p in c.ptrs:
        t = c.ptr(p)
        if not 0 < t < c.reloc: err.append('pointer %x -> %x' % (p, t)); break
    _, ng, ag = table(c, NG); ids = [c.u32(ag + i * 0xd0) for i in range(ng)]
    cl = [i for i in range(ng) if c.u32(ag + i * 0xd0 + 0xc) == 0x13]
    if ps3:
        # PS3 1.19 order: 271 netgames, a duplicate name (ba32a788), new- and old-form Classic, PS3 playlists
        if os.path.isfile(NET18):
            err += ['PS3 order: ' + x for x in ng119.check(c, DC(NET18))]
    else:
        if ng > 256: err.append('netgames > 256')
        if len(ids) != len(set(ids)): err.append('duplicate netgame ids')
        if len(cl) != 24: err.append('classic count %d' % len(cl))
        for i in cl:
            g = ag + i * 0xd0
            if (c.u32(g + 0x2c), c.u32(g + 0x64), c.u32(g + 0x68), c.u32(g + 0x6c)) != (0xd932420c, 0x1ad223b4, 1, 0): err.append('classic fields %08x' % c.u32(g))
    for s, mp in LAB_MODES:
        if s not in ids or c.u32(ag + ids.index(s) * 0xd0 + 4) != mp: err.append('mode %08x' % s)
    _, np, ap = table(c, PL)
    for i in range(np):
        m = ap + i * 0x68; l = c.ptr(m + 0x30)
        for k in range(c.u32(l)):
            if c.u32(c.ptr(l + 8) + k * 8) not in ids: err.append('playlist %08x -> unknown netgame' % c.u32(m))
        if not ps3 and c.u32(m) == PL_CLASSIC and c.u32(l) != 24: err.append('Classic playlist %d' % c.u32(l))
        if not ps3 and c.u32(m) == PL_LAB and c.u32(l) != 14: err.append('Lab playlist %d' % c.u32(l))
    types = {}
    for s in ids: types.setdefault(c.u32(ag + ids.index(s) * 0xd0 + 0xc), []).append(s)
    print('netgames per type over the 32 custom-list slots:', {hex(k): len(v) for k, v in types.items() if len(v) > 32})
    B = blocks(open(root + 'pak23_ps4.txt', 'rb').read().decode('latin1'))
    _, nm, am = table(c, MAPS); maps = {c.u32(am + i * 0x98): am + i * 0x98 for i in range(nm)}
    allact = {}
    for mid, arts in ((0x17d1af73, ['34a', '34b']), (0xc77d7f07, ['34a', '34b']), (0x195b0985, ['37']), (CLASSIC_MAP, [])):
        l = c.ptr(maps[mid] + 0x58); it = c.ptr(l + 8); act = set()
        for k in range(c.u32(l)):
            s = c.cstr(c.ptr(it + 16 * k))
            if c.u32(it + 16 * k + 8) != sid(s): err.append('sid %s' % s)
            if s not in B: err.append('level %s has no block' % s); continue
            act |= set(re.findall(r'^actor (\S+)', B[s], re.M))
            for pk in re.findall(r'^package (\S+)', B[s], re.M):
                f = 'pak23/ps4/' + (pk[:-7] + '-ingame12' if pk.endswith('-ingame') else pk) + '.pak'
                if not exists(root, f): err.append('missing ' + f)
            if not exists(root, 'pak23/ps4/%s.pak' % s): err.append('missing level pak ' + s)
        for x in act:
            if not exists(root, 'actor33/ps4/%s.pak' % x): err.append('missing actor %s (map %08x)' % (x, mid))
        for g in arts:
            for art in MAN['arts'][g]:
                if art in ('sd-pitch', 'sd-structure'): continue
                if art not in act: err.append('map %08x: spawner art %s is in no listed actor' % (mid, art))
    for rel in paks():
        p = root + rel
        if not os.path.exists(p): err.append('not copied ' + rel); continue
        q = Pak(p); v = q.validate()
        if v: err.append('%s: %s' % (rel, v[:2]))
        if open(p, 'rb').read() != open(FILES + rel, 'rb').read(): err.append('differs from batch: ' + rel)
    if base:
        a = DC(base); allowed = set()
        for o, ln in patched: allowed.update(range(o, o + ln))
        allowed.update(range(8, 16))
        bad = [i for i in range(a.reloc) if a.d[i] != d[i] and i not in allowed]
        if bad: err.append('original net.bin bytes changed at %s' % [hex(x) for x in bad[:8]])
        if [p for p in a.ptrs if p not in c.ptrs]: err.append('pointer bits lost')
        if not all(a.syms[k] == c.syms[k] for k in range(len(a.syms))): err.append('symbols changed')
    print('VERIFY', 'FAILED' if err else 'OK', '| net.bin', len(d), '| paks', len(paks()), '| netgames', ng, '| classic', len(cl))
    for x in err[:40]: print('  ERROR', x)
    return not err


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--sim'); ap.add_argument('--verify', action='store_true')
    ap.add_argument('--dc-only', action='store_true', help='only net.bin + modules.bin (recipe step maps-batch-dc); '
                    'the paks and pak23_ps4.txt need files/ from tools/build_paks.py')
    ap.add_argument('--files-only', action='store_true', help='only the 55 paks + pak23_ps4.txt (recipe step maps-batch-files), '
                    'after --dc-only')
    a = ap.parse_args()
    root = (a.sim.rstrip('/\\').replace('\\', '/') + '/') if a.sim else OVER
    if a.verify:
        sys.exit(0 if verify(root) else 1)
    # recipe 1.1: idempotent; --dc-only does the net.bin / modules.bin part alone
    if a.dc_only and ps3_order(root + 'dc1/ps4/net.bin'):
        print('already applied: the netgame table is in PS3 1.19 order (netgame-order-119)'); sys.exit(0)
    c = DC(root + 'dc1/ps4/net.bin'); _, n4, ar4 = table(c, NG)
    have_dc = any(c.u32(ar4 + i * 0xd0) == CLASSIC_NEW for i in range(n4))
    have_txt = 'level-name lab34-stage\r\n' in open(root + 'pak23_ps4.txt', 'rb').read().decode('latin1')
    if have_dc and (a.dc_only or have_txt):
        print('already applied: %08x is in net.bin' % CLASSIC_NEW); sys.exit(0)
    if a.files_only:
        if not have_dc: raise SystemExit('run apply_maps.py --dc-only first')
        if MAN is None: raise SystemExit('missing %smanifest.json: run tools/build_paks.py first' % FILES)
        txt = build_text(open(root + 'pak23_ps4.txt', 'rb').read())
        for x in ['pak23_ps4.txt'] + paks():
            src = (root if os.path.exists(root + x) else OVER) + x
            if os.path.exists(src):
                t_ = BK + x; os.makedirs(os.path.dirname(t_), exist_ok=True)
                if not os.path.exists(t_): shutil.copy2(src, t_)
        kept = 0
        for rel in paks():
            if os.path.exists(root + rel) and (rel.startswith('actor33/') or open(root + rel, 'rb').read() == open(FILES + rel, 'rb').read()):
                kept += 1; continue        # actor paks belong to convert-actors-r3 (step 070); never overwrite them here
            os.makedirs(os.path.dirname(root + rel), exist_ok=True); shutil.copy2(FILES + rel, root + rel)
        print('kept %d existing file(s)' % kept)
        open(root + 'pak23_ps4.txt', 'wb').write(txt)
        print('applied (files): %d paks copied, pak23_ps4.txt +lab34-stage/+lab34-spawners' % len(paks()))
        sys.exit(0 if verify(root) else 1)
    if a.dc_only:
        net, patched = build_net(root + 'dc1/ps4/net.bin')
        mod = bytearray(open(root + 'dc1/ps4/modules.bin', 'rb').read())
        old = struct.unpack_from('<Q', mod, NET_SIZE_OFF)[0]
        if old != os.path.getsize(root + 'dc1/ps4/net.bin'): raise SystemExit('modules.bin net size slot does not match net.bin')
        os.makedirs(BK + 'dc1/ps4/', exist_ok=True)
        for x in ('net.bin', 'modules.bin'):
            if not os.path.exists(BK + 'dc1/ps4/' + x): shutil.copy2(root + 'dc1/ps4/' + x, BK + 'dc1/ps4/' + x)
        open(root + 'dc1/ps4/net.bin', 'wb').write(net)
        struct.pack_into('<Q', mod, NET_SIZE_OFF, len(net)); open(root + 'dc1/ps4/modules.bin', 'wb').write(mod)
        print('applied (dc only): net.bin %d -> %d' % (old, len(net))); sys.exit(0)
    if have_dc:
        raise SystemExit('net.bin already has the maps-batch netgames but pak23_ps4.txt lacks lab34-stage: restore '
                         'net.bin and modules.bin from %sdc1/ps4 and run the full step' % BK)
    if MAN is None: raise SystemExit('missing %smanifest.json: run tools/build_paks.py first (or use --dc-only)' % FILES)
    bk = (root + '_backup/') if a.sim else BK
    stamp = bk + 'applied.json'
    if os.path.exists(stamp): raise SystemExit('already applied (%s). Restore from %s first.' % (stamp, bk))
    for x in ('dc1/ps4/net.bin', 'dc1/ps4/modules.bin', 'pak23_ps4.txt'):
        os.makedirs(os.path.dirname(root + x), exist_ok=True)
        if a.sim: shutil.copy2(OVER + x, root + x)
    os.makedirs(bk, exist_ok=True)
    cur = root + 'dc1/ps4/net.bin'
    net, patched = build_net(cur)           # raises before anything is written if the data is not as expected
    mod = bytearray(open(root + 'dc1/ps4/modules.bin', 'rb').read())
    old = struct.unpack_from('<Q', mod, NET_SIZE_OFF)[0]
    if old != os.path.getsize(cur): raise SystemExit('modules.bin holds net size %d, file is %d' % (old, os.path.getsize(cur)))
    txt = build_text(open(root + 'pak23_ps4.txt', 'rb').read())
    for x in ['dc1/ps4/net.bin', 'dc1/ps4/modules.bin', 'pak23_ps4.txt'] + paks():
        src = (root if os.path.exists(root + x) else OVER) + x
        if os.path.exists(src):
            t = bk + x; os.makedirs(os.path.dirname(t), exist_ok=True)
            if not os.path.exists(t): shutil.copy2(src, t)
    for rel in paks():
        os.makedirs(os.path.dirname(root + rel), exist_ok=True); shutil.copy2(FILES + rel, root + rel)
    open(root + 'pak23_ps4.txt', 'wb').write(txt)
    open(cur, 'wb').write(net)
    struct.pack_into('<Q', mod, NET_SIZE_OFF, len(net)); open(root + 'dc1/ps4/modules.bin', 'wb').write(mod)
    json.dump(dict(net_old=old, net_new=len(net)), open(stamp, 'w'))
    print('applied: net.bin %d -> %d, %d paks copied, backup in %s' % (old, len(net), len(paks()), bk))
    ok = verify(root, bk + 'dc1/ps4/net.bin', patched)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
