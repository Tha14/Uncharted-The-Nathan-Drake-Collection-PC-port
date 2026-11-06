#!/usr/bin/env python3
"""append102.py (fixer W, batch102-flatslots): texture slots the stage converter bound to its generated black flat-mask although
the PS3 material carries the texture for that job (the converter's role rules did not recognise its name):
  A  g_tNdFetchBlendMap          <- the PS3 '-height' / '-blend<n>' map (retail binds '-height' maps in BlendMap too)
  B  g_tNdFetchColorTintMask3Map <- the PS3 '-blend' map (layer-3 tint mask; retail tint masks are '-mask' / '-blend' maps)
  C  g_tNdFetchIncandescenceMaskMap <- the PS3 '-inc' / '-incan' / '-incand' map (lamps that glow on PS3)
Appends the PS3 texture (DXT1 -> PS4 tiled BC1 UNORM, data/*.bin, 1:1 texels) and repoints only the listed bindings.

usage:  python3 append102.py <work dir holding overlay-r3> <backup dir> [--check | --dry | --revert]
  --check   build every pak in memory and compare with SWAP.tsv md5_after (nothing is written)
  --dry     list what would be done
  --revert  copy the backups back
Every pak is md5-locked (SWAP.tsv md5_before = the PC files after batch101 / batch101m as installed in v6.44).

What changes in a pak (all original pages are kept byte for byte except the pointer/count words listed; one page is appended):
  - new page: one VRAM_DESC + name RAW_DATA per added texture, the rebuilt TEXTURE_TABLE arrays (hash-sorted), a new
    VRAM_DESC_TABLE (the header points at it), the GEOMETRY_1 texture array with the new entries, the new strings;
  - TEXTURE_TABLE +0 count, +8/+0x10/+0x18 array pointers; GEOMETRY_1 +0x38 count, +0x40 array pointer;
  - each target binding: +8 short / +0x10 full string pointers;
  - VRAM: the texels appended (256-byte aligned)."""
import sys, os, struct, json, hashlib, shutil
here = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.join(here, 'tools'))
from ndpak import Pak, sid
from ps4writer import PakWriter, Blob

PFX = 'd:/dev/CP9/Data/U3/CookData/ND/build/texture2/'
FMT_BC1_SRGB = 0x48


def md5b(b): return hashlib.md5(b).hexdigest()


def mats(q):
    g = q.items_of('GEOMETRY_1')[0]; pg, b = g.page, g.data_off
    n = q.u64(q.pbase(pg) + b + 0x48); mp, mo = q.resolve(pg, b + 0x50); out = []
    for i in range(n):
        o = mo + i * 0x48
        name = q.pstr(*q.resolve(mp, o))
        nf, npar, nb, _x = struct.unpack_from('<4I', q.d, q.pbase(mp) + o + 0x30)
        fr = q.resolve(mp, o + 0x10); feats = [q.pstr(*q.resolve(fr[0], fr[1] + 8 * k)) for k in range(nf if fr else 0)]
        br = q.resolve(mp, o + 0x28); binds = []
        for k in range(nb if br else 0):
            e = br[1] + 0x28 * k
            binds.append(dict(pname=q.pstr(*q.resolve(br[0], e)), short=q.pstr(*q.resolve(br[0], e + 8)), page=br[0], off=e))
        out.append(dict(name=name, feats=feats, binds=binds))
    return out


def build(src, targets, T, datadir):
    q = Pak(src)
    # target bindings
    todo = []   # (binding page, off, texkey)
    for m in mats(q):
        full = m['name'].split(',')[0]
        tg = {t['slot']: t for t in targets if t['mat'] == full}
        if not tg: continue
        for bd in m['binds']:
            t = tg.get(bd['pname'])
            if t and bd['short'].startswith('art/generated/u3mp/flat-'):
                todo.append((bd['page'], bd['off'], t['tex'], full + '|' + bd['pname']))
    want = sorted({k for _p, _o, k, _b in todo})
    if not todo: return None, 'no target binding'
    # writer with every original page raw
    w = PakWriter(); w.flags = q.hdr['flags']
    byp = {}
    for a, b, off in q.fixups: byp.setdefault(a, []).append((off, b))
    raws = [w.raw_page(q.d[po:po + ps], byp.get(pi, [])) for pi, (po, ps, _u) in enumerate(q.pages)]
    newp = w.page()
    strs = Blob('strs102')
    vram = bytearray(q.d[q.vram_base:])
    new = []
    tt0 = q.items_of('TEXTURE_TABLE')[0]; hp0 = q.resolve(tt0.page, tt0.data_off + 8)
    have = {q.pu32(hp0[0], hp0[1] + 4 * i) for i in range(q.u64(q.pbase(tt0.page) + tt0.data_off))}
    reuse = []
    for key in want:
        t = T[key]
        if sid(t['short'] + '/' + t['hx'] + '.ndb') in have:
            reuse.append(dict(key=key, full=PFX + t['short'] + '/' + t['hx'], short=t['short'])); continue
        short_name = t['short'] + '/' + t['hx'] + '.ndb'          # TEXTURE_TABLE name (retail: short incl. hash)
        path_ndb = PFX + short_name
        hsh = sid(short_name)
        texels = open(os.path.join(datadir, t['file']), 'rb').read()
        assert len(texels) == t['size']
        while len(vram) % 0x100: vram.append(0)
        voff = len(vram); vram += texels
        d = bytearray(0x38)
        struct.pack_into('<QQIIIIIIIIII', d, 0, 0, voff, len(texels), hsh, 1, t['fmt'], 0x01000000, t['mips'], t['w'], t['h'], 0, 3)
        db = Blob('vramdesc'); db.raw(bytes(d)); db.ptr(db, 0x40); db.cstr(path_ndb)
        nb = Blob('texname'); nb.cstr(short_name)
        name_item = newp.item('RAW_DATA', nb)
        desc_item = newp.item('VRAM_DESC', db)
        new.append(dict(key=key, hash=hsh, desc=desc_item, name=name_item, full=path_ndb[:-4], short=t['short'], fmt=t['fmt']))
    # TEXTURE_TABLE: rebuild the three arrays, hash-sorted
    tt = q.items_of('TEXTURE_TABLE')[0]; tp, tb = tt.page, tt.data_off
    n0 = q.u64(q.pbase(tp) + tb)
    hp = q.resolve(tp, tb + 8); npp = q.resolve(tp, tb + 0x10); dp = q.resolve(tp, tb + 0x18)
    old = []
    for i in range(n0):
        h = q.pu32(hp[0], hp[1] + 4 * i)
        nm = q.resolve(npp[0], npp[1] + 8 * i); de = q.resolve(dp[0], dp[1] + 8 * i)
        old.append(dict(hash=h, name=nm, desc=de))
    hashes = {o['hash'] for o in old}
    for e in new: assert e['hash'] not in hashes, ('texture already present', e['key'])
    allt = sorted([('o', o) for o in old] + [('n', e) for e in new], key=lambda x: x[1]['hash'])
    hashb, namesb, descb = Blob('tthash102'), Blob('ttnames102'), Blob('ttdesc102')
    for kind, e in allt:
        hashb.u32(e['hash'])
        if kind == 'o':
            namesb.ptr(raws[e['name'][0]], e['name'][1]); descb.ptr(raws[e['desc'][0]], e['desc'][1])
        else:
            namesb.ptr(e['name'].data); descb.ptr(e['desc'].data)
    for bl in (hashb, namesb, descb): newp.item('RAW_DATA', bl)
    raws[tp].set_u64(tb, len(allt))
    raws[tp].redirect(tb + 8, hashb); raws[tp].redirect(tb + 0x10, namesb); raws[tp].redirect(tb + 0x18, descb)
    # VRAM_DESC_TABLE: new one (same order), header points at it
    vt = q.vram_desc_table()          # (page, item header off) per desc, in table order
    vmap = {}
    for (pg, off) in vt:
        vmap[(pg, off + 0x20)] = (pg, off)
    vb = Blob('vramtable102'); vb.u32(len(allt)).u32(0)
    for kind, e in allt:
        if kind == 'o':
            pg, off = vmap[tuple(e['desc'])]; vb.ptr(raws[pg], off)
        else:
            vb.ptr(e['desc'].data, -0x20)
    vitem = newp.item('VRAM_DESC_TABLE', vb, listed=False)
    # GEOMETRY_1 texture array
    g = q.items_of('GEOMETRY_1')[0]; gpg, gb = g.page, g.data_off
    ntex = q.pu32(gpg, gb + 0x38); gtp = q.resolve(gpg, gb + 0x40)
    geob = Blob('geotex102')
    for i in range(ntex):
        e = gtp[1] + 0x28 * i
        s = q.resolve(gtp[0], e)
        geob.ptr(raws[s[0]], s[1]); geob.raw(q.d[q.pbase(gtp[0]) + e + 8:q.pbase(gtp[0]) + e + 0x28])
    for e in new:
        geob.ptr(strs, strs.cstr(e['full']))
        geob.raw(struct.pack('<IIIiIIQ', e['hash'], 0x400, 0x101, -1, 0, 4, 0))
    newp.item('RAW_DATA', geob)
    raws[gpg].set_u32(gb + 0x38, ntex + len(new))
    raws[gpg].redirect(gb + 0x40, geob)
    # bindings
    so = {}
    for e in new + reuse:
        so[e['key']] = (strs.cstr(e['short']), strs.cstr(e['full']))
    for pg, off, key, _b in todo:
        s_off, f_off = so[key]
        raws[pg].redirect(off + 8, strs, s_off); raws[pg].redirect(off + 0x10, strs, f_off)
    sitem = newp.item('RAW_DATA', strs)
    w.login_page = (q.hdr['loginPage'], q.hdr['loginOff'])
    w.vram_table_loc = (q.hdr['vramDescPage'], q.hdr['vramDescOff'])
    w.vram = bytes(vram)
    w.build()
    w.vram_table_loc = (newp.index, vitem.off)
    data = w.build()
    rem = newp.size % 16
    if rem:
        strs.raw(b'\0' * (16 - rem)); data = w.build()
        assert newp.size % 16 == 0
    # verify
    q2 = Pak(data)
    assert q2.d[q2.vram_base:q2.vram_base + len(q.d) - q.vram_base] == q.d[q.vram_base:]
    T2 = {t['path']: t for t in q2.textures()}
    for e in new:
        t2 = T2[e['full'] + '.ndb']
        assert t2['hash'] == e['hash'] and t2['format'] == e['fmt']
    keys = {b_ for _p, _o, _k, b_ in todo}
    nfix = 0
    for m in mats(q2):
        full = m['name'].split(',')[0]
        for bd in m['binds']:
            if full + '|' + bd['pname'] in keys:
                assert not bd['short'].startswith('art/generated/u3mp/flat-'); nfix += 1
    assert nfix == len(todo), (nfix, len(todo))
    for pi, (po, ps, _u) in enumerate(q.pages):
        a = q.d[po:po + ps]; b2 = q2.d[q2.pages[pi][0]:q2.pages[pi][0] + q2.pages[pi][1]]
        assert len(a) == len(b2)
    return data, dict(bindings=len(todo), added=len(new), reused=len(reuse))


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]; mode = ([x for x in sys.argv[1:] if x.startswith('--')] or ['--apply'])[0]
    work, bak = a[0], a[1]
    D = json.load(open(os.path.join(here, 'data', 'targets.json')))
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(here, 'SWAP.tsv')) if l.strip() and not l.startswith('pak\t')]
    if mode != '--check': os.makedirs(bak, exist_ok=True)
    ok = skip = 0
    for r in rows:
        pak, ov, m0, m1 = r[:4]
        dst = os.path.join(work, 'overlay-' + ov, 'u3data', 'build', 'main', 'pak23', 'ps4', pak)
        bk = os.path.join(bak, 'overlay-' + ov + '__' + pak)
        if mode == '--revert':
            if os.path.exists(bk): shutil.copyfile(bk, dst); print('reverted', pak); ok += 1
            continue
        if not os.path.exists(dst): print('MISSING', pak); skip += 1; continue
        src = open(dst, 'rb').read(); m = md5b(src)
        if m == m1: print('already', pak); ok += 1; continue
        if m != m0: print('SKIP (md5 changed)', pak, m); skip += 1; continue
        data, rep = build(src, D['targets'][pak], D['textures'], os.path.join(here, 'data'))
        if data is None: print('NOTHING', pak, rep); skip += 1; continue
        if md5b(data) != m1: print('MD5 MISMATCH (not written)', pak, md5b(data)); skip += 1; continue
        if mode in ('--check', '--dry'): print('ok-would-patch', pak, rep); ok += 1; continue
        if not os.path.exists(bk): shutil.copyfile(dst, bk)
        tmp = dst + '.b102tmp'; open(tmp, 'wb').write(data); os.replace(tmp, dst)
        if md5b(open(dst, 'rb').read()) != m1: print('MD5 MISMATCH after write, restoring', pak); shutil.copyfile(bk, dst); skip += 1; continue
        print('patched', pak, rep); ok += 1
    print('ok', ok, 'skipped', skip)


if __name__ == '__main__': main()
