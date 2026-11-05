#!/usr/bin/env python3
"""Rename a PS4 actor package (artgroup, FG sections, prototypes, submeshes, collision, skel
item names) without touching anything else: every page is copied verbatim, renamed strings are
appended to the end of the page that held the original, and the pointers to them are redirected.

Used for MP twins whose PS3 data equals a PS4 campaign actor (e.g. propane-tank-mp = propane-tank,
mp-hero-skel-bones-glow = mp-hero-skel-bones): the result is pure retail PS4 data.

Renamed: any string WITHOUT '/' in which OLD occurs as a whole token (followed by end, '.', or
'-lod'); asset paths (materials, textures, effects) are left alone.  Also patched:
PACKAGE_ITEM_LOOKUP sid, ARTGROUP_DATA bucket keys (re-hashed), and the skeleton id (JOINT_HIERARCHY
+0x30, FOREGROUND_SECTION_2 +8, ANIM skeleton id fields) when --skel-id is given.

usage: ps4rename.py <ps4.pak> <out.pak> OLD NEW [--drop-tag NAME] [--skel-id HEX]
"""
import argparse, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid
from ps4writer import PakWriter


def rename_pak(src, out, old, new, skel_id=None, verbose=True):
    p = Pak(src)
    assert p.platform == 'ps4'
    tok = re.compile(r'(?<![A-Za-z0-9-])' + re.escape(old) + r'(?=$|\.|-lod)')

    def ren(s):
        if '/' in s or ',' in s:
            return None
        t = tok.sub(new, s)
        return t if t != s else None

    pages = []
    for pi, (po, ps, _) in enumerate(p.pages):
        pages.append(bytearray(p.d[po:po + ps]))
    fix = {pi: {} for pi in range(len(p.pages))}      # page -> {off: dstPage}
    for a, b, off in p.fixups:
        fix[a][off] = b
    appended = {}                                      # (page, newstring) -> offset

    def put(pg, s):
        key = (pg, s)
        if key not in appended:
            d = pages[pg]
            while len(d) % 16:
                d.append(0)
            appended[key] = len(d)
            d += s.encode() + b'\0'
        return appended[key]

    def cstr(pg, off):
        d = pages[pg]
        e = d.find(b'\0', off)
        try:
            return d[off:e].decode('ascii')
        except UnicodeDecodeError:
            return None

    sid_map = {}
    changed = []
    # 1. item header names and page entry names (page relative, rebased by the loader)
    for pi in range(len(p.pages)):
        d = pages[pi]
        n = struct.unpack_from('<H', d, 0x12)[0]
        for j in range(n):
            eo = 0x14 + 16 * j
            nm, io = struct.unpack_from('<QI', d, eo)
            s = cstr(pi, nm)
            t = ren(s) if s else None
            if t:
                no = put(pi, t)
                struct.pack_into('<Q', d, eo, no)
                hn = struct.unpack_from('<Q', d, io)[0]
                hs = cstr(pi, hn)
                ht = ren(hs) if hs else None
                if ht:
                    struct.pack_into('<Q', d, io, put(pi, ht))
                sid_map[sid(s)] = sid(t)
                changed.append(('item', s, t))
    # 2. pointer fields (fixups) that point at a renamable C string (not inside sound bank
    #    tables: a bank keeps the campaign name, e.g. propane-tank-mp plays bank 'propane-tank')
    skip = [(it.page, it.data_off, it.data_off + it.size) for it in p.items
            if it.type in ('SOUND_BANK_TABLE', 'VRAM_DESC', 'TEXTURE_TABLE', 'EFFECT', 'EFFECT_TABLE')]
    for pi in range(len(p.pages)):
        for off, dp in sorted(fix[pi].items()):
            if any(pg == pi and lo <= off < hi for pg, lo, hi in skip):
                continue
            v = struct.unpack_from('<Q', pages[pi], off)[0]
            if v >= len(p.d) or v >= p.pages[dp][1]:
                continue
            s = cstr(dp, v)
            if not s or len(s) < len(old):
                continue
            t = ren(s)
            if t:
                struct.pack_into('<Q', pages[pi], off, put(dp, t))
                sid_map.setdefault(sid(s), sid(t))
                changed.append(('ptr', s, t))
    # artgroup-derived keys: 'geo.' + FG name
    for it in p.items:
        t = ren(it.name)
        if t:
            sid_map[sid('geo.' + it.name)] = sid('geo.' + t)
            sid_map[sid(it.name)] = sid(t)
    # 3. PACKAGE_ITEM_LOOKUP sid(s)
    for it in p.items_of('PACKAGE_ITEM_LOOKUP'):
        d = pages[it.page]
        for o in range(it.data_off, it.data_off + it.size - 3, 4):
            k = struct.unpack_from('<I', d, o)[0]
            if k in sid_map and k == sid(old):
                struct.pack_into('<I', d, o, sid_map[k])
                changed.append(('lookup-sid', hex(k), hex(sid_map[k])))
    # 4. ARTGROUP bucket keys, re-hashed with linear probing
    for it in p.items_of('ARTGROUP_DATA'):
        pg, b = it.page, it.data_off
        d = pages[pg]
        x = fix[pg].get(b + 0x40)
        xo = struct.unpack_from('<Q', d, b + 0x40)[0]
        assert x == pg
        nb = struct.unpack_from('<I', d, xo + 0x1c)[0]
        tbl = struct.unpack_from('<Q', d, xo + 0x20)[0]
        ents = []
        for k in range(nb):
            o = tbl + 16 * k
            key = struct.unpack_from('<I', d, o)[0]
            if (o + 8) in fix[pg]:
                ents.append((sid_map.get(key, key), struct.unpack_from('<Q', d, o + 8)[0]))
                del fix[pg][o + 8]
            struct.pack_into('<IIQ', d, o, 0, 0xaaaaaaaa, 0)
        slots = [None] * nb
        for key, ptr in ents:
            bi = key % nb
            while slots[bi] is not None:
                bi = (bi + 1) % nb
            slots[bi] = (key, ptr)
        for k, s in enumerate(slots):
            if s:
                o = tbl + 16 * k
                struct.pack_into('<IIQ', d, o, s[0], 0xaaaaaaaa, s[1])
                fix[pg][o + 8] = pg
        changed.append(('artgroup', len(ents), nb))
    # 5. skeleton id
    if skel_id is not None:
        olds = set()
        for it in p.items_of('JOINT_HIERARCHY'):
            o = it.data_off + 0x30
            olds.add(struct.unpack_from('<I', pages[it.page], o)[0])
            struct.pack_into('<I', pages[it.page], o, skel_id)
        for it in p.items_of('FOREGROUND_SECTION_2'):
            o = it.data_off + 8
            if struct.unpack_from('<I', pages[it.page], o)[0] in olds:
                struct.pack_into('<I', pages[it.page], o, skel_id)
        for it in p.items_of('ANIM'):
            for o in range(it.data_off, it.data_off + it.size - 3, 4):
                if struct.unpack_from('<I', pages[it.page], o)[0] in olds:
                    struct.pack_into('<I', pages[it.page], o, skel_id)
        changed.append(('skel', [hex(x) for x in olds], hex(skel_id)))
    # leftover raw occurrences (not reached through a pointer) are reported, not patched
    left = []
    for pi in range(len(p.pages)):
        for m in re.finditer(re.escape(old.encode()), bytes(pages[pi])):
            left.append((pi, m.start()))
    # page sizes
    w = PakWriter()
    for pi in range(len(p.pages)):
        d = pages[pi]
        while len(d) % 16 and pi != len(p.pages) - 1:
            d.append(0)
        struct.pack_into('<I', d, 0xc, len(d))
        w.raw_page(bytes(d), sorted(fix[pi].items()))
    w.login_page = (p.hdr['loginPage'], p.hdr['loginOff'])
    w.vram_table_loc = (p.hdr['vramDescPage'], p.hdr['vramDescOff'])
    w.vram = p.d[p.vram_base:]
    data = w.build()
    open(out, 'wb').write(data)
    if verbose:
        seen = set()
        for c in changed:
            if c not in seen:
                seen.add(c)
                print('  %s: %s -> %s' % c)
        print('  raw occurrences of %r left (old strings, now unreferenced or inline): %d' % (old, len(left)))
        print('wrote %s (%d bytes)' % (out, len(data)))
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('old')
    ap.add_argument('new')
    ap.add_argument('--skel-id', type=lambda s: int(s, 16))
    a = ap.parse_args()
    rename_pak(a.src, a.out, a.old, a.new, skel_id=a.skel_id)


if __name__ == '__main__':
    main()
