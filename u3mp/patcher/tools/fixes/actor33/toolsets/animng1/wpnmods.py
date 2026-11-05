#!/usr/bin/env python3
"""Append PS3 weapon-skin (mod) materials to a retail PS4 weapon pak.

The retail PS4 machinegun-mp / hk416-mp / p99-mp lack some DLC skins that the MP unlock table
still sells (ak47-mod:snake/nd, hk416-mod:jeremy, p99-mod:snake/nd).  Every skin is a material
variant that differs from its siblings only in one colour texture.  For each missing skin this
tool appends an alias material: a copy of a retail sibling (same shader, features, params and
effect hash) named '<dir>/<file>:<skin>,<guid>,<shader>' whose bindings point at the colour
texture converted from the PS3 pak.

Output layout (retail bytes kept verbatim, retail material/texture/effect indices unchanged):
  page 0  retail page 0; changed fields only:
            GEOMETRY_1 +0x38/+0x40 texture count/array, +0x48/+0x50 material count/array
            TEXTURE_TABLE +0 count, +8 hashes, +0x10 names, +0x18 desc pointers
            EFFECT_TABLE +0 count, +8 entries (new entries = copies of the sibling's entry,
            same EFFECT object: materials and effect-table entries pair up by index)
  page 1  retail page 1 + appended VRAM desc table (retail entries + new), header points at it
  page 2  new VRAM_DESC items and RAW_DATA arrays (retail entries copied first, then new ones)
  VRAM    retail VRAM verbatim + new textures (fully resident, top mips cut to --max-tex)

usage: wpnmods.py <ps4 retail pak> <ps3 pak> <out pak> --src <retail sibling skin> skin [skin...]
       e.g. wpnmods.py machinegun-mp.pak ps3/machinegun-mp.pak out.pak --src navarro snake nd
"""
import argparse, hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ndpak import Pak, sid
from ps3mat import read_materials
from ps4writer import PakWriter, Blob, Item
from convert_net import PS4_COOK_PREFIX, PS3_COOK_MARK
import texcodec as tc


class Writer(PakWriter):
    """vram_table_loc resolved after layout (the table is a blob appended to a RawPage)."""
    vt_blob = None
    vt_page = None

    @property
    def vram_table_loc(self):
        if self.vt_blob is None:
            return None
        return self.vt_page.index, self.vt_page.blob_abs[id(self.vt_blob)][1]

    @vram_table_loc.setter
    def vram_table_loc(self, v):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ps4')
    ap.add_argument('ps3')
    ap.add_argument('out')
    ap.add_argument('--src', required=True, help='retail sibling skin to clone (e.g. navarro)')
    ap.add_argument('--max-tex', type=int, default=256)
    ap.add_argument('--stream-flags', type=int, default=2)
    ap.add_argument('skins', nargs='+')
    a = ap.parse_args()

    R = Pak(a.ps4)
    S = Pak(a.ps3)
    assert R.platform == 'ps4' and S.platform == 'ps3'
    assert len(R.pages) == 2, 'expects a 2-page retail pak'
    g = R.items_of('GEOMETRY_1')[0]
    gb = g.data_off
    tt = R.items_of('TEXTURE_TABLE')[0]
    et = R.items_of('EFFECT_TABLE')[0]
    assert g.page == tt.page == et.page == 0
    P0 = R.pbase(0)

    def rptr(pg, off):
        """retail pointer field -> (page, offset) or None"""
        return R.resolve(pg, off)

    # ---- retail tables
    nmat = R.pu32(0, gb + 0x48)
    mpg, moff = rptr(0, gb + 0x50)
    ntex = R.pu32(0, gb + 0x38)
    tpg, toff = rptr(0, gb + 0x40)
    mats = []
    for i in range(nmat):
        o = moff + 0x48 * i
        mats.append(dict(off=o, name=R.pstr(*rptr(mpg, o))))
    src = [m for m in mats if m['name'].split(',')[0].endswith(':' + a.src)]
    assert len(src) == 1, 'sibling %r not found: %s' % (a.src, [m['name'] for m in mats])
    src = src[0]
    src_idx = mats.index(src)
    base_dir = src['name'].split(',')[0].rsplit(':', 1)[0]           # objects/weapons/ak47/ak47-mod
    shader = src['name'].rsplit(',', 1)[1]
    existing = {m['name'].split(',')[0] for m in mats}
    nbind = R.pu32(mpg, src['off'] + 0x38)
    bpg, boff = rptr(mpg, src['off'] + 0x28)
    binds = []
    for k in range(nbind):
        o = boff + 0x28 * k
        binds.append(dict(off=o, param=R.pstr(*rptr(bpg, o)), short=R.pstr(*rptr(bpg, o + 8)),
                          full=R.pstr(*rptr(bpg, o + 0x10))))
    color = [b for b in binds if b['param'] == 'g_tNdFetchColor1Map']
    assert color, 'sibling has no Color1 binding'
    src_color_full = color[0]['full']
    swap = [b for b in binds if b['full'] == src_color_full]          # Color1 (+ Overlay2)

    # retail geometry texture entries, texture table, effect table
    gtex = [toff + 0x28 * i for i in range(ntex)]
    src_gtex = [o for o in gtex if R.pstr(*rptr(tpg, o)) == src_color_full]
    assert src_gtex, 'sibling colour texture entry not found'
    n_tt = R.u64(P0 + tt.data_off)
    hpg, hoff = rptr(0, tt.data_off + 8)
    npg, noff = rptr(0, tt.data_off + 0x10)
    dpg, doff = rptr(0, tt.data_off + 0x18)
    tt_rows = []
    for i in range(n_tt):
        tt_rows.append(dict(hash=R.pu32(hpg, hoff + 4 * i), name=rptr(npg, noff + 8 * i),
                            desc=rptr(dpg, doff + 8 * i)))
    n_fx = R.pu32(0, et.data_off)
    epg, eoff = rptr(0, et.data_off + 8)
    assert n_fx == nmat, 'effect entries %d != materials %d' % (n_fx, nmat)

    # ---- PS3 skin textures
    ms3, tx3 = read_materials(S)
    tex3 = {t['path']: t for t in S.textures()}
    new = []
    for skin in a.skins:
        nm3 = [m for m in ms3 if m['name'] == base_dir + '/' + skin]
        assert nm3, 'PS3 material %s/%s not found' % (base_dir, skin)
        if base_dir + ':' + skin in existing:
            print('skip %s: retail already has it' % skin)
            continue
        cols = [tx3[t]['name'] if t is not None else None for t in nm3[0]['textures']]
        cols = [c for c in cols if c and '-color' in c]
        assert cols, 'no colour texture for %s' % skin
        t = [x for p_, x in tex3.items() if p_.endswith(cols[0])][0]
        short = t['path'].split(PS3_COOK_MARK, 1)[1] if PS3_COOK_MARK in t['path'] else t['path']
        fmt3 = tc.ps3_format(t)
        kind, bpb, comp = fmt3
        texels = S.vram_bytes(t['vramOffset'], t['vramSize'])
        tw, th, tm = t['width'], t['height'], t['mips']
        while a.max_tex and max(tw, th) > a.max_tex and tm > 1:
            bw, bh = tc.blocks(tw, th, True)
            texels = texels[bw * bh * bpb:]
            tw, th, tm = max(1, tw >> 1), max(1, th >> 1), tm - 1
        tiled = tc.ps3_to_ps4_texels(texels, tw, th, tm, bpb)
        fmt4 = {'DXT1': 0x48, 'DXT5': 0x4e, 'DXT3': 0x4b}[kind]
        assert sid(short) == t['hash'], 'texture hash scheme mismatch for %s' % short
        hx = hashlib.md5((base_dir + ':' + skin).encode()).hexdigest()
        guid = '%s-%s-%s-%s-%s' % (hx[:8], hx[8:12], hx[12:16], hx[16:20], hx[20:32])
        new.append(dict(skin=skin, short=short, hash=t['hash'], fmt=fmt4, w=tw, h=th, mips=tm, tiled=tiled,
                        name='%s:%s,%s,%s' % (base_dir, skin, guid, shader)))
        print('  %s <- %s %dx%d %s mips %d (%d bytes)' % (skin, short, tw, th, kind, tm, len(tiled)))
    if not new:
        print('nothing to add')
        return

    # ---- build
    w = Writer()
    w.flags = R.hdr['flags']
    fx0 = [(o, d) for s_, d, o in R.fixups if s_ == 0]
    fx1 = [(o, d) for s_, d, o in R.fixups if s_ == 1]
    pg0 = w.raw_page(R.d[R.pbase(0):R.pbase(0) + R.pages[0][1]], fx0)
    pg1 = w.raw_page(R.d[R.pbase(1):R.pbase(1) + R.pages[1][1]], fx1)
    pg2 = w.page()
    w.login_page = (R.hdr['loginPage'], R.hdr['loginOff'])
    rp = {0: pg0, 1: pg1}

    def P(blob, loc):
        blob.ptr(rp[loc[0]], loc[1]) if loc else blob.u64(0)

    def copy_words(blob, pg, off, size):
        """copy retail bytes [off, off+size) of page pg into blob, re-pointing pointer fields"""
        o = off
        while o < off + size:
            if R.is_ptr(pg, o):
                P(blob, rptr(pg, o))
                o += 8
            else:
                blob.raw(R.d[R.pbase(pg) + o:R.pbase(pg) + o + 4])
                o += 4

    strs = Blob('wpnstrings')
    sidx = {}

    def S_(s):
        if s not in sidx:
            sidx[s] = strs.cstr(s)
        return sidx[s]

    # VRAM: retail + new
    vram = bytearray(R.d[R.vram_base:R.vram_base + R.hdr['vramSize']])
    descs = []
    for n_ in new:
        while len(vram) % 0x100:
            vram.append(0)
        voff = len(vram)
        vram += n_['tiled']
        db = Blob('vramdesc')
        full4 = PS4_COOK_PREFIX + n_['short']
        db.u64(0).u64(voff).u32(len(n_['tiled'])).u32(n_['hash'])
        db.u32(1).u32(n_['fmt']).u32(0x010f0000 if n_['fmt'] in (0x48, 0x4b, 0x4e) else 0x01000000)
        db.u32(n_['mips']).u32(n_['w']).u32(n_['h']).u32(0).u32(a.stream_flags)
        db.ptr(db, 0x40)
        db.cstr(full4)
        it = pg2.item('VRAM_DESC', db)
        n_['desc'] = it
        n_['full_noext'] = full4[:-4] if full4.endswith('.ndb') else full4
        n_['short_noext'] = n_['short'].rsplit('/', 1)[0] if n_['short'].endswith('.ndb') else n_['short']
        descs.append(it)
    w.vram = bytes(vram)

    # VRAM desc table (appended to page 1, with an unlisted ResItem header like retail)
    vpg, voff_ = R.hdr['vramDescPage'], R.hdr['vramDescOff']
    vdata = voff_ + 0x20
    nv = R.pu32(vpg, vdata)
    vt = Blob('vramtable')
    hdr_old = R.d[R.pbase(vpg) + voff_:R.pbase(vpg) + voff_ + 0x20]
    vt.raw(hdr_old[:0x10]).u32(8 + 8 * (nv + len(descs))).raw(hdr_old[0x14:0x20])
    vt.u32(nv + len(descs)).u32(0)
    for k in range(nv):
        P(vt, rptr(vpg, vdata + 8 + 8 * k))
    for it in descs:
        vt.ptr(it.data, -0x20)
    pg1.append(vt)
    w.vt_blob, w.vt_page = vt, pg1

    # TEXTURE_TABLE: merged, sorted by hash
    rows = [dict(hash=r['hash'], name=('R', r['name']), desc=('R', r['desc'])) for r in tt_rows]
    for n_ in new:
        rows.append(dict(hash=n_['hash'], name=('N', n_), desc=('D', n_)))
    rows.sort(key=lambda r: r['hash'])
    hb, nb, dbl = Blob('tthash'), Blob('ttnames'), Blob('ttdesc')
    for r in rows:
        hb.u32(r['hash'])
        if r['name'][0] == 'R':
            P(nb, r['name'][1])
        else:
            nb.ptr(strs, S_(r['name'][1]['short']))
        if r['desc'][0] == 'R':
            P(dbl, r['desc'][1])
        else:
            dbl.ptr(r['desc'][1]['desc'].data)       # TEXTURE_TABLE points at the desc data
    # GEOMETRY_1 texture entries: retail + copies of the sibling colour entry
    gt = Blob('geotex')
    for o in gtex:
        copy_words(gt, tpg, o, 0x28)
    for n_ in new:
        st = len(gt)
        copy_words(gt, tpg, src_gtex[0], 0x28)
        gt.relocs = [r for r in gt.relocs if r[0] != st]
        gt.relocs.append((st, strs, S_(n_['full_noext'])))
        struct.pack_into('<I', gt.b, st + 8, n_['hash'])
    # materials: retail entries + aliases (private binding arrays)
    mb = Blob('materials')
    bblobs = []
    for m in mats:
        copy_words(mb, mpg, m['off'], 0x48)
    for n_ in new:
        bb = Blob('bindings-' + n_['skin'])
        for b in binds:
            st = len(bb)
            copy_words(bb, bpg, b['off'], 0x28)
            if b in swap:
                bb.relocs = [r for r in bb.relocs if r[0] not in (st + 8, st + 0x10)]
                bb.relocs.append((st + 8, strs, S_(n_['short_noext'])))
                bb.relocs.append((st + 0x10, strs, S_(n_['full_noext'])))
                struct.pack_into('<iI', bb.b, st + 0x18, -1, 0)
        bblobs.append(bb)
        st = len(mb)
        copy_words(mb, mpg, src['off'], 0x48)
        mb.relocs = [r for r in mb.relocs if r[0] not in (st, st + 0x28)]
        mb.relocs.append((st, strs, S_(n_['name'])))
        mb.relocs.append((st + 0x28, bb, 0))
    # effect table entries: retail + copies of the sibling's entry
    eb = Blob('fxentries')
    for i in range(n_fx):
        copy_words(eb, epg, eoff + 0x18 * i, 0x18)
    for n_ in new:
        copy_words(eb, epg, eoff + 0x18 * src_idx, 0x18)

    for blob in [strs, hb, nb, dbl, gt, mb, eb] + bblobs:
        pg2.item('RAW_DATA', blob)

    # retail page-0 field patches
    total_tex = ntex + len(new)
    pg0.set_u64(gb + 0x38, total_tex)
    pg0.redirect(gb + 0x40, gt)
    pg0.set_u64(gb + 0x48, nmat + len(new))
    pg0.redirect(gb + 0x50, mb)
    pg0.set_u64(tt.data_off, len(rows))
    pg0.redirect(tt.data_off + 8, hb)
    pg0.redirect(tt.data_off + 0x10, nb)
    pg0.redirect(tt.data_off + 0x18, dbl)
    pg0.set_u32(et.data_off, n_fx + len(new))
    pg0.redirect(et.data_off + 8, eb)

    out = w.build()
    open(a.out, 'wb').write(out)
    print('wrote %s: %d bytes, +%d materials (%d total), +%d textures, vram %#x' % (
        a.out, len(out), len(new), nmat + len(new), len(new), len(w.vram)))


if __name__ == '__main__':
    main()
