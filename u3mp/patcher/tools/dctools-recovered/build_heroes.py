#!/usr/bin/env python3
"""build_heroes.py <ps4 net.bin> <ps3 1.19 net.bin> <ps4 modules.bin> <out net.bin> <out modules.bin> <log.json>

Ports the PS3 1.19 custom-character data whose actor paks exist (retail actor33, converted overlay
paks, or art groups the retail net.bin already names):
 A. 27 existing part lists (type 0x2a8027cf) that 1.19 extended: PS3-only entries appended in PS3 order
    (PS4 entries keep their index; parts are addressed by index within the slot's first list).
 B. Hero slot 0 (hero +0x28 = customization slot 0, accessories) restored on the heroes where PS4 has
    none and PS3 1.19 has one; hero +0x48 (type 0x5652eec7) restored where PS4 has none.
 C. Hero body list (+0x18/+0x20) set back to the PS3 1.19 list where Bluepoint swapped it (needed for
    the hair-under-accessory matching, 0x9952a0 cases 6/7/10).
 D. The 22 PS3 heroes missing on PS4 appended to the hero table 0xbd3bb3b9 (57 -> 79).
 E. New symbols for every PS3 part list / palette these reference that PS4 lacks (symbol table grows,
    file relaid out by dcsyms.add_symbols); their sids appended to modules.bin's net export list.
 F. Unlock entries (cat 1 = part sid, cat 2 = hero sid2) for the added items, except DLC packs >= 256.
Entries whose art group is not available are left out of new/extended lists (logged)."""
import sys, os, struct, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import DC, sid
from dcedit import DCEdit
from dcsyms import add_symbols
from heroes_common import hero_rows
from parts_common import part_syms, available, entry3
import unlocks

H, PT, PAL, SLOT, S56, S44, S4E = 0xbd3bb3b9, 0x2a8027cf, 0x290349e3, 0x88e3fe54, 0x5652eec7, 0x4494d781, 0x4e512ad9


class B:
    def __init__(s, a, b, e):
        s.a, s.b, s.e = a, b, e; s.strs = {}; s.newsyms = []   # (sid, type, value off)
    def string(s, t):
        if t in s.strs: return s.strs[t]
        enc = t.encode('latin1') + b'\0'; d = s.a.d; pos = d.find(enc)
        while 0 <= pos < s.a.reloc and pos > 0 and d[pos - 1] != 0: pos = d.find(enc, pos + 1)
        if pos < 0 or pos >= s.a.reloc: pos = s.e.add_string(t)
        s.strs[t] = pos; return pos
    def obj(s, typ, size):
        e = s.e; e.align(8); e.d += struct.pack('<II', 0, typ); o = len(e.d); e.d += bytes(size); return o
    # ---- converters
    def part_entry(s, dst, e3):
        b, e = s.b, s.e
        struct.pack_into('<II', e.d, dst, b.u32(e3), 0)
        e.putptr(dst + 8, s.string(b.cstr(b.ptr(e3 + 4))))
        struct.pack_into('<II', e.d, dst + 0x10, b.u32(e3 + 8), 0)
        for k3, k4 in ((0xc, 0x18), (0x10, 0x1c), (0x14, 0x20), (0x18, 0x24), (0x1c, 0x28), (0x20, 0x2c), (0x24, 0x30), (0x28, 0x34)):
            struct.pack_into('<I', e.d, dst + k4, b.u32(e3 + k3))
    def u32list(s, words):
        arr = s.e.alloc(4 * max(1, len(words)), 8)
        for i, w in enumerate(words): struct.pack_into('<I', s.e.d, arr + 4 * i, w)
        return arr
    def slot(s, o3):        # 0x88e3fe54 {count, ptr u32[]}
        b = s.b; n = b.u32(o3); q = b.ptr(o3 + 4)
        arr = s.u32list([b.u32(q + 4 * i) for i in range(n)])
        o = s.obj(SLOT, 16); struct.pack_into('<I', s.e.d, o, n); s.e.putptr(o + 8, arr); return o
    def ptrcount(s, typ, o3):   # 0x5652eec7 / 0x4e512ad9 {ptr u32[], count}
        b = s.b; n = b.u32(o3 + 4); q = b.ptr(o3)
        arr = s.u32list([b.u32(q + 4 * i) for i in range(n)])
        o = s.obj(typ, 16); s.e.putptr(o, arr); struct.pack_into('<I', s.e.d, o + 8, n); return o
    def o4494(s, o3):
        b = s.b; o = s.obj(S44, 0x20)
        s.e.putptr(o, s.string(b.cstr(b.ptr(o3))))
        for k3, k4 in ((4, 8), (8, 0x10), (0xc, 0x14), (0x10, 0x18), (0x14, 0x1c)): struct.pack_into('<I', s.e.d, o + k4, b.u32(o3 + k3))
        return o
    def palette(s, o3):
        b = s.b; n = b.u32(o3); q = b.ptr(o3 + 4)
        arr = s.u32list([b.u32(q + 4 * k) for k in range(3 * n)])
        o = s.obj(PAL, 16); struct.pack_into('<I', s.e.d, o, n); s.e.putptr(o + 8, arr); return o


def main(p4, p3, mod4, out, outmod, logp):
    a = DC(p4); b = DC(p3); e = DCEdit(p4); B_ = B(a, b, e); av = available()
    log = {'extended': [], 'new_parts': [], 'new_palettes': [], 'slot0': [], 's56': [], 'body': [], 'heroes': [], 'excluded': []}
    p4s = part_syms(a); p3s = part_syms(b)
    syms4 = {x[0] for x in a.syms}
    need_parts, need_pal = set(), set()
    ADDED = set()      # part entry sids added by this run (appended or in new lists)
    def use_entry(e3, where):
        art = b.cstr(b.ptr(e3 + 4))
        if art not in av:
            log['excluded'].append(dict(list=where, sid='%08x' % b.u32(e3), art=art)); return False
        pal = b.u32(e3 + 0x20)
        if pal and pal not in syms4: need_pal.add(pal)
        return True
    def need_list(ps):
        if ps not in syms4: need_parts.add(ps)
    # ---- A: extend lists
    for ps, (o4, q4, es4) in p4s.items():
        s4 = [a.u32(x) for x in es4]
        s3 = [b.u32(x) for x in p3s[ps][2]]
        if s4 == s3 or not all(x in s3 for x in s4): continue
        if s3[:len(s4)] != s4 and ps != 0x23f313bc: continue          # Bluepoint-edited lists stay
        add = [x for x in p3s[ps][2] if b.u32(x) not in s4 and use_entry(x, '%08x' % ps)]
        if not add: continue
        n4 = len(es4); na = e.alloc(0x38 * (n4 + len(add)), 8)
        e.copy_words(q4, na, 0x38 * n4)
        for k, x in enumerate(add): B_.part_entry(na + 0x38 * (n4 + k), x); ADDED.add(b.u32(x))
        e.put32(o4, n4 + len(add)); e.putptr(o4 + 8, na)
        log['extended'].append(dict(sym='%08x' % ps, old=n4, added=[b.cstr(b.ptr(x + 4)) for x in add]))
    # ---- heroes
    r4 = hero_rows(a); r3 = hero_rows(b); d3 = {r['sid']: r for r in r3}; s4h = {r['sid'] for r in r4}
    newh = [y for y in r3 if y['sid'] not in s4h]
    t, oh = a.bysid[H]; nh = a.u32(oh); ah = a.ptr(oh + 8)
    na = e.alloc(0x70 * (nh + len(newh)), 16); e.copy_words(ah, na, 0x70 * nh)
    HE = lambda r: na + 0x70 * r['i']          # entry in the new array
    for r in r4:
        y = d3[r['sid']]
        # B: slot 0 (+0x28 / PS3 +0x18)
        if r['slots'][2] is None and isinstance(y['slots'][2], dict):
            for ps in y['slots'][2]['parts']: need_list(ps)
            o = B_.slot(y['slots'][2]['obj']); e.putptr(HE(r) + 0x28, o)
            log['slot0'].append(dict(hero=r['name'], lists=['%08x' % p for p in y['slots'][2]['parts']]))
        # 0x5652eec7 at +0x48 / PS3 +0x28
        if r['slots'][6] is None and isinstance(y['slots'][6], tuple):
            o3 = b.ptr(y['e'] + 0x28); o = B_.ptrcount(S56, o3); e.putptr(HE(r) + 0x48, o)
            q = b.ptr(o3); refs = [b.u32(q + 4 * i) for i in range(b.u32(o3 + 4))]
            for ps in refs:
                if ps in p3s: need_list(ps)
            log['s56'].append(dict(hero=r['name'], refs=['%08x' % x for x in refs]))
        # C: body lists swapped by Bluepoint
        for k, off in ((0, 0x18), (1, 0x20)):
            sa, sb = r['slots'][k], y['slots'][k]
            if isinstance(sa, dict) and isinstance(sb, dict) and sa['parts'] != sb['parts']:
                for ps in sb['parts']: need_list(ps)
                o = B_.slot(sb['obj']); e.putptr(HE(r) + off, o)
                log['body'].append(dict(hero=r['name'], slot_off='%#x' % off, ps4=['%08x' % p for p in sa['parts']], ps319=['%08x' % p for p in sb['parts']]))
    # D: new heroes
    for j, y in enumerate(newh):
        dst = na + 0x70 * (nh + j); src = y['e']
        e.putptr(dst, B_.string(b.cstr(b.ptr(src))))
        for k3, k4 in ((4, 8), (8, 0x10), (0xc, 0x14), (0x30, 0x58)): struct.pack_into('<I', e.d, dst + k4, b.u32(src + k3))
        for k in range(7):
            so = src + 0x10 + 4 * k
            if not b.isptr(so): continue
            o3 = b.ptr(so); typ = b.u32(o3 - 4)
            if typ == SLOT:
                for i in range(b.u32(o3)): need_list(b.u32(b.ptr(o3 + 4) + 4 * i))
                o = B_.slot(o3)
            elif typ == S56:
                q = b.ptr(o3)
                for i in range(b.u32(o3 + 4)):
                    if b.u32(q + 4 * i) in p3s: need_list(b.u32(q + 4 * i))
                o = B_.ptrcount(S56, o3)
            else: raise SystemExit('hero slot type %x' % typ)
            e.putptr(dst + 0x18 + 8 * k, o)
        e.putptr(dst + 0x50, B_.string(b.cstr(b.ptr(src + 0x2c))))
        assert b.u32(b.ptr(src + 0x34) - 4) == S44
        e.putptr(dst + 0x60, B_.o4494(b.ptr(src + 0x34)))
        if b.isptr(src + 0x38):
            o3 = b.ptr(src + 0x38); assert b.u32(o3 - 4) == S4E
            q = b.ptr(o3)
            for i in range(b.u32(o3 + 4)):
                if b.u32(q + 4 * i) in p3s: need_list(b.u32(q + 4 * i))
            e.putptr(dst + 0x68, B_.ptrcount(S4E, o3))
        log['heroes'].append(dict(index=nh + j, name=y['name'], sid='%08x' % y['sid'], sid2='%08x' % y['sid2']))
    e.put32(oh, nh + len(newh)); e.putptr(oh + 8, na)
    # E: new part lists (closure over palettes)
    added_part_sids = set(int(x['sid'], 16) for x in [])
    for ps in sorted(need_parts):
        if ps not in p3s: raise SystemExit('referenced list %08x not a PS3 part list' % ps)
        o3, q3, es3 = p3s[ps]
        keep = [x for x in es3 if use_entry(x, '%08x' % ps)]
        arr = e.alloc(0x38 * max(1, len(keep)), 8)
        for k, x in enumerate(keep): B_.part_entry(arr + 0x38 * k, x); ADDED.add(b.u32(x))
        o = B_.obj(PT, 16); struct.pack_into('<I', e.d, o, len(keep)); e.putptr(o + 8, arr)
        B_.newsyms.append((ps, PT, o))
        log['new_parts'].append(dict(sym='%08x' % ps, entries=[b.cstr(b.ptr(x + 4)) for x in keep], dropped=len(es3) - len(keep)))
    b_pal = {}
    for s_, t_, o_ in b.syms:
        if t_ == PAL: b_pal.setdefault(s_, o_)
    for ps in sorted(need_pal):
        o = B_.palette(b_pal[ps]); B_.newsyms.append((ps, PAL, o)); log['new_palettes'].append('%08x' % ps)
    # F: unlocks for added part entries / heroes
    added_parts = set()
    for x in log['extended']: pass
    def entry_sids_added():
        r = set()
        for ps in need_parts:
            for x in p3s[ps][2]: r.add(b.u32(x))
        for ps, (o4, q4, es4) in p4s.items():
            for x in p3s.get(ps, (0, 0, []))[2]: r.add(b.u32(x))
        return r
    psids = ADDED - {a.u32(x) for _, (o4, q4, es4) in p4s.items() for x in es4}; hsids2 = {y['sid2'] for y in newh}
    have = unlocks.ps4_keys(a)[3]; ids4 = {k[2] for k in have}
    excluded_arts = {int(x['sid'], 16) for x in log['excluded']}
    addu = {1: [], 2: []}; skipped = []
    for y in unlocks.ps3_unlocks(b):
        if (y['cat'], y['sid'], y['id']) in have: continue
        ok = (y['cat'] == 1 and y['sid'] in psids and y['sid'] not in excluded_arts) or (y['cat'] == 2 and y['sid'] in hsids2)
        if not ok: continue
        if unlocks.dlc_blocked(y): skipped.append(dict(id='%#x' % y['id'], cat=y['cat'], sid='%08x' % y['sid'], conds=y['conds'])); continue
        pass  # PS3 itself reuses some ids (0xbd, 0xbf, 0x36d-0x370)
        addu[y['cat']].append(y)
    added, positions = unlocks.insert_multi(a, e, addu)
    log['unlock_added'] = added; log['unlock_positions'] = {str(k): v for k, v in positions.items()}
    log['unlock_skipped_dlc'] = skipped
    data = e.build()
    open(out + '.pre', 'wb').write(data)          # before the symbol-table relayout (for verification)
    log['patched_fields'] = sorted(set(e.patched))
    data2, delta, mv = add_symbols(data, B_.newsyms)
    open(out, 'wb').write(data2)
    # modules.bin: append the new sids to module 'net' export list
    m = DCEdit(mod4); M = DC(mod4)
    s_, t_, o_ = M.syms[0]; nmod = M.u32(o_); arr = M.ptr(o_ + 8)
    for i in range(nmod):
        ent = arr + i * 0x28
        if M.cstr(M.ptr(ent)) == 'net': break
    lo = M.ptr(ent + 0x20); ln = M.u32(lo); lq = M.ptr(lo + 8)
    nl = m.alloc(4 * (ln + len(B_.newsyms)), 8)
    m.d[nl:nl + 4 * ln] = M.d[lq:lq + 4 * ln]
    for k, (sx, tx, ox) in enumerate(B_.newsyms): struct.pack_into('<I', m.d, nl + 4 * (ln + k), sx)
    m.put32(lo, ln + len(B_.newsyms)); m.putptr(lo + 8, nl)
    struct.pack_into('<Q', m.d, ent + 0x10, len(data2))
    open(outmod, 'wb').write(m.build())
    log['modules'] = dict(net_entry='%#x' % ent, export_list='%#x' % lo, old=ln, new=ln + len(B_.newsyms), net_size=len(data2))
    log['new_symbols'] = ['%08x %08x' % (sx, tx) for sx, tx, ox in B_.newsyms]
    log['relayout_delta'] = delta
    json.dump(log, open(logp, 'w'), indent=1)
    print('extended %d lists, slot0 %d, s56 %d, body %d, heroes +%d, new parts %d, new palettes %d, excluded entries %d, unlocks %s, skipped %d, net %d B' % (
        len(log['extended']), len(log['slot0']), len(log['s56']), len(log['body']), len(newh), len(log['new_parts']),
        len(log['new_palettes']), len(log['excluded']), len(log['unlock_added']), len(skipped), len(data2)))


def insert_from_bytes(e, add, cat):
    """unlocks.insert on the current state of e (table may already have been replaced in this run)"""
    tmp = e.build()
    a = DC.__new__(DC)
    import io
    path = '/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/dc2/out/_tmp_unlock.bin'
    open(path, 'wb').write(tmp); a = DC(path)
    return unlocks.insert(a, e, add, cat)

if __name__ == '__main__':
    main(*sys.argv[1:7])
