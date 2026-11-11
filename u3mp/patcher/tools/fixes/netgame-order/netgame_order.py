#!/usr/bin/env python3
"""Recipe step netgame-order-119: the overlay net.bin netgame table (DC 0x3b9a067d) and playlists (0xc1b0be0d) in
PS3 1.19 order, so a PC netgame index is the PS3 index (crossplay with RPCS3 / PS3: match settings, votes, films and
the party messages carry the index).

  python netgame_order.py [--apply] [--verify] [--net net.bin] [--modules modules.bin] [--net18 net18.bin]

The order is the one of the player's PS3 game data net18.bin ({{PS3_DATA2}}/net18.bin), the table a PS3 1.19 game
loads when it goes online (NetInit downloads it and reloads DC module 'net').  The result has exactly PS3's netgames:
same count, order, names, maps and game types.
  * a netgame the PC table has (same name; duplicates take the next unused PC entry) is copied whole, so every PS4
    sub-structure and every earlier recipe fix stays (PS4 wave / AI lists, hero records in the coop-desc-fix form);
    its scalar fields are then overwritten with the converted PS3 values: the match rules are PS3's (score / time
    limits, Classic / Hardcore flags and loadouts: Hardcore stays Hardcore as on PS3).
  * a netgame the PC lacks is converted from PS3 like dctools/lab_reg.py (scalars by the field map, the +0x74 hero
    records as f0670de8 -> 3 x 8b617b51); any other pointer field stops the step.
  * PC-only netgames (old PS4-retail Hardcore, the lab-custom-* TDM entries, old Lab Dodgeball, ...) are dropped.
  * a PS3 map the map list lacks (net18 2026-11-07: 0x416ad15b lab-multi-35) gets a map-list entry converted like
    lab_reg.py with +0x94 = 1 (not listed): the PC has no paks for it; its netgames keep their PS3 indices.
  * playlists: PS3 order, ids, words, netgame lists with weights and pools; the PC entry of the same id is the
    template (PS4 layout, picture string) and keeps its content-pack word (+0x2c).
Append-only (dctools DCEdit): the original bytes stay, new arrays go after them; only the table headers change.
modules.bin: the size word of module 'net' (found by name) is set to the new net.bin size.
Dry run by default; --apply writes (net.bin / modules.bin backed up once to work/backup-netgame-order/).
Idempotent: exits 0 'already applied' when the table already equals net18's (ng119.check finds nothing).
--verify: only report the differences to net18 (exit 1 when there are any)."""
import argparse, collections, hashlib, json, os, shutil, struct, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[0:0] = [HERE, os.path.join(HERE, '..', 'dctools')]
from common import DC, sid                # noqa: E402  (dctools job, rendered next to this one: with_jobs)
from dcedit import DCEdit                 # noqa: E402
import ng119 as N                         # noqa: E402

PROJECT = "{{PROJECT}}"
D = PROJECT + "/work/overlay-r3/u3data/build/main/dc1/ps4/"
BK = PROJECT + "/work/backup-netgame-order/"
NET18 = "{{PS3_DATA2}}/net18.bin"
DROP_LEVELS = {'cargo-plane-static'}       # as lab_reg.py: not a PS4 level
MODULE_T = 0x61183179                      # modules.bin module list (entries 0x28: name pointer +0, .bin size +0x10)


def sha256(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def accepted_net18():
    """the net18.bin versions the recipe knows (tools/dcconv/net18-versions.json), for the log only"""
    for base in (os.environ.get('PAKTOOLS', ''), os.path.join(HERE, '..', '..', 'paktools')):
        p = os.path.join(base, '..', 'dcconv', 'net18-versions.json')
        if base and os.path.isfile(p):
            try:
                return json.load(open(p, encoding='utf-8')).get('accepted', {})
            except ValueError:
                return {}
    return {}


def module_size_slot(mod, name):
    """file offset of the u64 .bin size word of module `name` in modules.bin (by name, not a fixed offset)"""
    m = DC(mod)
    hits = []
    for s, t, o in m.syms:
        if t != MODULE_T:
            continue
        n, arr = m.u32(o), m.ptr(o + 8)
        for i in range(n):
            e = arr + i * 0x28
            if m.isptr(e) and m.cstr(m.ptr(e)) == name:
                hits.append(e + 0x10)
    if len(hits) != 1:
        raise SystemExit('modules.bin: %d entries for module %r (expected 1)' % (len(hits), name))
    return hits[0]


def build(net, b, log):
    """the rebuilt net.bin bytes (raises SystemExit on anything unexpected; nothing is written here)"""
    a = DC(net)
    if a.ps != 8 or b.ps != 4:
        raise SystemExit('expected a PS4 net.bin and a PS3 net18.bin')
    e = DCEdit(net)
    strs = {}

    def string(s):                         # an existing string of the original part, else appended (lab_reg.py)
        if s in strs:
            return strs[s]
        enc = s.encode('latin1') + b'\0'
        pos = a.d.find(enc)
        while 0 <= pos < a.reloc:
            if pos == 0 or a.d[pos - 1] == 0:
                break
            pos = a.d.find(enc, pos + 1)
        if pos < 0 or pos >= a.reloc:
            pos = e.add_string(s)
        strs[s] = pos
        return pos

    def obj(t, size):                      # PS4 object: 8-byte header [0, type]
        e.align(8); e.d += struct.pack('<II', 0, t); o = len(e.d); e.d += bytes(size); return o

    def obj4(t, w):                        # 4-byte header [type] + u32 words (hero records)
        e.align(4); e.d += struct.pack('<I', t); o = len(e.d); e.d += struct.pack('<%dI' % len(w), *w); return o

    def put(o, v):
        if (o & ~7) in e.ptrs:
            raise SystemExit('internal: scalar write over a pointer slot at %x' % o)
        struct.pack_into('<I', e.d, o, v)

    # ------------------------------------------------------------------ map list: PS3 maps the PC lacks
    om, nm, am = N.table(a, N.MAPS)
    _, nm3, am3 = N.table(b, N.MAPS)
    ids4 = {a.u32(am + i * N.MAP4) for i in range(nm)}
    new_maps = [i for i in range(nm3) if b.u32(am3 + i * N.MAP3) not in ids4]
    log['maps_added'] = []
    if new_maps:
        known = set(N.MAP_STR) | set(N.MAP_U32) | {0x30}
        na = e.alloc(N.MAP4 * (nm + len(new_maps)), 16)
        e.copy_words(am, na, N.MAP4 * nm)
        for j, i3 in enumerate(new_maps):
            src, m = am3 + i3 * N.MAP3, na + N.MAP4 * (nm + j)
            for k in range(0, N.MAP3, 4):
                if b.isptr(src + k) and k not in N.MAP_STR and k != 0x30:
                    raise SystemExit('PS3 map %08x: unexpected pointer at +%x' % (b.u32(src), k))
                if k not in known and b.u32(src + k):
                    raise SystemExit('PS3 map %08x: unexpected value %x at +%x' % (b.u32(src), b.u32(src + k), k))
            if not b.isptr(src + 0x30):
                raise SystemExit('PS3 map %08x has no sub-level list' % b.u32(src))
            l3 = b.ptr(src + 0x30)
            if b.u32(l3 - 4) != N.MAP_LIST_T:
                raise SystemExit('PS3 map %08x: sub-level list type %08x' % (b.u32(src), b.u32(l3 - 4)))
            sub = [b.cstr(b.ptr(b.ptr(l3 + 4) + 8 * k)) for k in range(b.u32(l3))]
            sub = [s for s in sub if s not in DROP_LEVELS]
            items = e.alloc(16 * len(sub), 8)
            for k, s in enumerate(sub):
                e.putptr(items + 16 * k, string(s)); struct.pack_into('<I', e.d, items + 16 * k + 8, sid(s))
            lst = obj(N.MAP_LIST_T, 16); struct.pack_into('<I', e.d, lst, len(sub)); e.putptr(lst + 8, items)
            for k3, k4 in N.MAP_U32.items():
                put(m + k4, b.u32(src + k3))
            for k3, k4 in N.MAP_STR.items():
                if b.isptr(src + k3):
                    e.putptr(m + k4, string(b.cstr(b.ptr(src + k3))))
                elif b.u32(src + k3):
                    raise SystemExit('PS3 map %08x: +%x is a value, not a string' % (b.u32(src), k3))
            e.putptr(m + 0x58, lst)
            put(m + N.MAP_NOT_LISTED, 1)
            ids4.add(b.u32(src))
            log['maps_added'].append(dict(id='%08x' % b.u32(src), ps3_slot=i3, main=b.cstr(b.ptr(src + 0x18)) if b.isptr(src + 0x18) else '',
                                          sublevels=len(sub), not_listed=1))
        e.put32(om, nm + len(new_maps)); e.putptr(om + 8, na)

    # ------------------------------------------------------------------ netgames in PS3 order
    o4, n4, ar4 = N.table(a)
    o3, n3, ar3 = N.table(b)
    ptr4 = set(N.PTRS.values())
    for i in range(n4):
        g = ar4 + i * N.NG4
        bad = [k for k in range(0, N.NG4, 8) if a.isptr(g + k) and k not in ptr4]
        if bad:
            raise SystemExit('PC netgame %d (%08x): unexpected pointer at +%s' % (i, a.u32(g), ','.join('%x' % k for k in bad)))
    pc_by = collections.defaultdict(list)
    for i in range(n4):
        pc_by[a.u32(ar4 + i * N.NG4)].append(i)
    used, plan = set(), []
    for i3 in range(n3):
        src = ar3 + i3 * N.NG3
        bad = [k for k in range(0, N.NG3, 4) if b.isptr(src + k) and k not in N.PTRS]
        if bad:
            raise SystemExit('PS3 netgame %d (%08x): unexpected pointer at +%s' % (i3, b.u32(src), ','.join('%x' % k for k in bad)))
        for k3, k4 in N.MAP.items():
            if k4 is None and b.u32(src + k3) and not (k3 in N.DROPPED_CLASSIC and b.u32(src + 0xc) == N.CLASSIC):
                raise SystemExit('PS3 netgame %d (%08x): +%x = %x has no PC field' % (i3, b.u32(src), k3, b.u32(src + k3)))
        if b.u32(src + 4) and b.u32(src + 4) not in ids4:
            raise SystemExit('PS3 netgame %d (%08x): map %08x is in no map list' % (i3, b.u32(src), b.u32(src + 4)))
        free = [i for i in pc_by.get(b.u32(src), []) if i not in used]
        if free:
            used.add(free[0]); plan.append(('pc', i3, free[0]))
        else:
            plan.append(('conv', i3, None))
    na = e.alloc(N.NG4 * len(plan), 16)
    stats, changed = collections.Counter(), collections.Counter()
    for j, (kind, i3, i4) in enumerate(plan):
        g, src = na + N.NG4 * j, ar3 + i3 * N.NG3
        if kind == 'pc':
            pc = ar4 + i4 * N.NG4
            for k3, k4 in N.PTRS.items():
                if a.isptr(pc + k4) != b.isptr(src + k3):
                    raise SystemExit('netgame %08x: pointer +%x on PC %s, PS3 +%x %s: the PC sub-structure does not match PS3'
                                     % (b.u32(src), k4, a.isptr(pc + k4), k3, b.isptr(src + k3)))
            e.copy_words(pc, g, N.NG4)
            for k3, k4 in N.MAP.items():
                if k4 is None:
                    continue
                v = b.u32(src + k3)
                if struct.unpack_from('<I', e.d, g + k4)[0] != v:
                    changed['+%02x' % k4] += 1
                    put(g + k4, v)
        else:
            if not b.isptr(src + 0x74) or [k for k in N.PTRS if k != 0x74 and b.isptr(src + k)]:
                raise SystemExit('PS3 netgame %d (%08x) is not on the PC and has pointers other than the +0x74 hero '
                                 'records: needs a sub-structure converter' % (i3, b.u32(src)))
            for k3, k4 in N.MAP.items():
                if k4 is not None:
                    put(g + k4, b.u32(src + k3))
            t3 = b.ptr(src + 0x74)
            if b.u32(t3 - 4) != N.NG_OBJ:
                raise SystemExit('PS3 netgame %08x: +0x74 object type %08x' % (b.u32(src), b.u32(t3 - 4)))
            subs = []
            for k in range(3):
                r = b.ptr(t3 + 4 * k)
                if b.u32(r - 4) != N.NG_SUB:
                    raise SystemExit('PS3 netgame %08x: hero record type %08x' % (b.u32(src), b.u32(r - 4)))
                w = [b.u32(r + 4 * x) for x in range(9)]
                if b.u32(src + 0xc) == N.COOP_ADV:   # the form coop-desc-fix leaves (u8[4] in file byte order)
                    w[0] = int.from_bytes(w[0].to_bytes(4, 'little'), 'big')
                subs.append(obj4(N.NG_SUB, w))
            t4 = obj(N.NG_OBJ, 24)
            for k in range(3):
                e.putptr(t4 + 8 * k, subs[k])
            e.putptr(g + 0x70, t4)
        stats[kind] += 1
    e.put32(o4, len(plan)); e.putptr(o4 + 8, na)
    dropped = [i for i in range(n4) if i not in used]
    log['netgames'] = dict(total=len(plan), ps3=n3, pc_before=n4, copied_from_pc=stats['pc'], converted=stats['conv'],
                           pc_only_dropped=len(dropped))
    log['ps3_values_changed_fields'] = dict(sorted(changed.items()))
    log['pc_only_dropped'] = ['%d:%08x' % (i, a.u32(ar4 + i * N.NG4)) for i in dropped]
    log['converted'] = ['%d:%08x' % (i3, b.u32(ar3 + i3 * N.NG3)) for k, i3, _ in plan if k == 'conv']
    log['indices_over_255'] = sum(1 for _ in plan[256:])

    # ------------------------------------------------------------------ playlists in PS3 order
    op4, np4, ap4 = N.table(a, N.PL)
    op3, np3, ap3 = N.table(b, N.PL)
    pc_pl = {a.u32(ap4 + i * N.PL4): ap4 + i * N.PL4 for i in range(np4)}
    names = {b.u32(ar3 + i * N.NG3) for i in range(n3)}
    npl = e.alloc(N.PL4 * np3, 16)
    plog = []
    for j in range(np3):
        m3, m = ap3 + j * N.PL3, npl + N.PL4 * j
        pid = b.u32(m3)
        tpl = pc_pl.get(pid)
        if tpl is None:
            raise SystemExit('PS3 playlist %08x has no PC entry to take the PS4 layout from' % pid)
        p3 = {k for k in range(0, N.PL3, 4) if b.isptr(m3 + k)}
        p4 = {k for k in range(0, N.PL4, 8) if a.isptr(tpl + k)}
        if not p3 <= N.PL_PTRS3 or 0x2c not in p3 or not p4 <= N.PL_PTRS4 or 0x30 not in p4:
            raise SystemExit('playlist %08x: unexpected pointers PS3 %s PC %s' % (pid, sorted(p3), sorted(p4)))
        l3 = b.ptr(m3 + 0x2c)
        if b.u32(l3 - 4) != N.PL_LIST_T:
            raise SystemExit('PS3 playlist %08x: list type %08x' % (pid, b.u32(l3 - 4)))
        e.copy_words(tpl, m, N.PL4)
        for k3, k4 in N.PL_MAP.items():
            if k4 not in N.PL_KEEP_PC:
                put(m + k4, b.u32(m3 + k3))
        n, it3 = b.u32(l3), b.ptr(l3 + 4)
        items = [(b.u32(it3 + 8 * k), b.u32(it3 + 8 * k + 4)) for k in range(n)]
        it = e.alloc(max(8 * n, 8), 8)       # plain array, no type header
        for k, (s, w) in enumerate(items):
            struct.pack_into('<II', e.d, it + 8 * k, s, w)
        lst = obj(N.PL_LIST_T, 16); struct.pack_into('<I', e.d, lst, n); e.putptr(lst + 8, it); e.putptr(m + 0x30, lst)
        if 0x38 in p3:
            q3 = b.ptr(m3 + 0x38)
            if b.u32(q3 - 4) != N.PL_POOL_T:
                raise SystemExit('PS3 playlist %08x: pool type %08x' % (pid, b.u32(q3 - 4)))
            pn, pit3 = b.u32(q3), b.ptr(q3 + 4)
            pit = e.alloc(max(4 * pn, 8), 8)
            for k in range(pn):
                struct.pack_into('<I', e.d, pit + 4 * k, b.u32(pit3 + 4 * k))
            pool = obj(N.PL_POOL_T, 16); struct.pack_into('<I', e.d, pool, pn); e.putptr(pool + 8, pit); e.putptr(m + 0x40, pool)
        elif 0x40 in p4:                      # PS3 has no pool here: none on the PC either
            e.ptrs.discard(m + 0x40); struct.pack_into('<Q', e.d, m + 0x40, 0)
        missing = [s for s, _ in items if s not in names]
        if missing:
            raise SystemExit('PS3 playlist %08x names netgames PS3 does not have: %s' % (pid, ['%08x' % s for s in missing]))
        plog.append(dict(id='%08x' % pid, netgames=n, pool=b.u32(b.ptr(m3 + 0x38)) if 0x38 in p3 else 0))
    e.put32(op4, np3); e.putptr(op4 + 8, npl)
    log['playlists'] = plog
    out = e.build()
    # nothing but the table headers (and the reloc offset) may differ in the original part
    allowed = set(range(8, 16))
    for o, ln in e.patched:
        allowed.update(range(o, o + ln))
    bad = [i for i in range(a.reloc) if out[i] != a.d[i] and i not in allowed]
    if bad:
        raise SystemExit('internal: original bytes changed at %s' % [hex(x) for x in bad[:8]])
    lost = [p for p in a.ptrs if p not in e.ptrs]
    if lost:
        raise SystemExit('internal: pointer bits lost at %s' % [hex(x) for x in lost[:8]])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--verify', action='store_true', help='only list the differences to net18.bin')
    ap.add_argument('--net', default=D + 'net.bin')
    ap.add_argument('--modules', default=D + 'modules.bin')
    ap.add_argument('--net18', default=NET18)
    ap.add_argument('--backup', default=BK)
    A = ap.parse_args()
    if not os.path.isfile(A.net18):
        raise SystemExit('PS3 net18.bin not found: %s' % A.net18)
    h = sha256(A.net18)
    acc = accepted_net18()
    print('net18.bin %s sha256 %s (%s)' % (A.net18, h, acc.get(h, 'not in tools/dcconv/net18-versions.json') if acc else 'version list not found'))
    b = DC(A.net18)
    errs = N.check(DC(A.net), b)
    if A.verify:
        for x in errs[:60]:
            print('  ' + x)
        print('differences to net18:', len(errs))
        return 1 if errs else 0
    if not errs:
        print('already applied: the netgame table and playlists equal net18.bin (%d netgames)' % N.table(b)[1])
        return 0
    print('%d difference(s) to net18.bin, first: %s' % (len(errs), errs[0]))
    log = collections.OrderedDict(net18=A.net18, net18_sha256=h, net_before=dict(size=os.path.getsize(A.net), sha256=sha256(A.net)))
    out = build(A.net, b, log)
    slot = module_size_slot(A.modules, 'net')
    mod = bytearray(open(A.modules, 'rb').read())
    old = struct.unpack_from('<Q', mod, slot)[0]
    if old != os.path.getsize(A.net):
        raise SystemExit('modules.bin: module net size word %d (at %#x) is not the net.bin size %d' % (old, slot, os.path.getsize(A.net)))
    struct.pack_into('<Q', mod, slot, len(out))
    with tempfile.TemporaryDirectory() as td:   # the result must load and equal net18 before anything is written
        p = os.path.join(td, 'net.bin'); open(p, 'wb').write(out)
        c = DC(p)
        left = N.check(c, b)
        if left:
            for x in left[:20]:
                print('  ERROR ' + x)
            raise SystemExit('the rebuilt table still differs from net18.bin in %d place(s); nothing written' % len(left))
    log['net_after'] = dict(size=len(out), sha256=hashlib.sha256(out).hexdigest())
    log['modules_net_size_slot'] = '%#x' % slot
    s = {k: v for k, v in log.items() if k not in ('converted', 'pc_only_dropped', 'playlists')}
    print(json.dumps(s, indent=1))
    print('dropped PC-only netgames:', ' '.join(log['pc_only_dropped']))
    if not A.apply:
        print('dry run: net.bin %d -> %d, modules.bin net size %d -> %d; nothing written' % (old, len(out), old, len(out)))
        return 0
    os.makedirs(A.backup, exist_ok=True)
    for src, name in ((A.net, 'net.bin'), (A.modules, 'modules.bin')):
        if not os.path.exists(os.path.join(A.backup, name)):
            shutil.copy2(src, os.path.join(A.backup, name))
    for dst, data in ((A.net, out), (A.modules, bytes(mod))):
        open(dst + '.tmp', 'wb').write(data)
        os.replace(dst + '.tmp', dst)
    json.dump(log, open(os.path.join(A.backup, 'netgame_order.log.json'), 'w'), indent=1)
    print('applied: net.bin %d -> %d (%d netgames, PS3 order), modules.bin net size %d -> %d' % (old, len(out), N.table(b)[1], old, len(out)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
