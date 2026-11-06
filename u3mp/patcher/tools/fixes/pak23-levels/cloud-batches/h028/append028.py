#!/usr/bin/env python3
"""append028.py (fixer H, h028-spec-masks): PS3 spec masks the stage converter dropped (white / blown-out reflections).
  * rebind rows: the template's spec-colour map slot (g_tNdFetchColor4Map / g_tNdFetchMask4Map / g_tNdFetchColor5Map) held
    the ALBEDO; it is repointed at the PS3 '-spec(-mask)' texture, appended to the pak (data/*.bin, PS3 texels re-tiled).
    Retail binds '-spec' maps in these slots (Mask4Map 155/182, Color4Map 26/38 over the uploaded retail paks).
  * scale rows (template without a spec map): g_specularBrightness / g_specularEnvironmentStrength /
    g_specularSecondLobeBrightness x mean(PS3 mask channel), in place, 4 bytes each, old value checked.
usage:  python3 append028.py <work dir holding overlay-r3/r2/port/mp> <backup dir> [--check | --dry | --revert | --scan]
--scan prints md5 + the in-memory md5_after of every target pak (used to build SWAP.tsv).
Every pak is md5-locked (SWAP.tsv md5_before = the PC file as read 2026-10-30, effective overlay r3>r2>port>mp)."""
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
        pr = q.resolve(mp, o + 0x20); params = []
        for k in range(npar if pr else 0):
            e = pr[1] + 0x18 * k
            params.append(dict(pname=q.pstr(*q.resolve(pr[0], e)), val=q.resolve(pr[0], e + 8)))
        out.append(dict(name=name, feats=feats, binds=binds, params=params))
    return out


def build(src, targets, T, datadir):
    q = Pak(src)
    todo = []   # (binding page, off, texkey, label)
    want = {}
    pfix = []   # (page, off, value, label)
    allm = mats(q)
    refc = {}
    for m in allm:
        for pa in m['params']: refc[pa['val']] = refc.get(pa['val'], 0) + 1
    for m in allm:
        full = m['name'].split(',')[0]
        tg = {t['slot']: t for t in targets['tex'] if t['mat'] == full}
        for bd in m['binds']:
            if bd['pname'] not in tg: continue
            key = tg[bd['pname']]['key']; want[key] = T[key]
            todo.append((bd['page'], bd['off'], key, full + '|' + bd['pname']))
        for pt in [x for x in targets['param'] if x['mat'] == full]:
            pa = [x for x in m['params'] if x['pname'] == pt['pname']]
            if len(pa) != 1 or not pa[0]['val']: return None, 'param not found: %s %s' % (full, pt['pname'])
            v = pa[0]['val']
            if refc[v] != 1: return None, 'param value shared: %s' % full
            cur = struct.unpack_from('<f', q.d, q.pbase(v[0]) + v[1] + 4 * pt['comp'])[0]
            if abs(cur - pt['old']) > 1e-3 * max(1.0, abs(pt['old'])): return None, 'param %s %s is %r, expected %r' % (full, pt['pname'], cur, pt['old'])
            pfix.append((v[0], v[1] + 4 * pt['comp'], pt['value'], full + '|' + pt['pname']))
    ntg = len(targets['tex']) + len(targets['param'])
    if len(todo) + len(pfix) != ntg: return None, 'found %d of %d targets' % (len(todo) + len(pfix), ntg)
    if not todo:                      # softness only: 4-byte in-place writes, nothing moves
        data = bytearray(src)
        for pg, off, val, _l in pfix:
            struct.pack_into('<f', data, q.pbase(pg) + off, val)
        data = bytes(data)
        q2 = Pak(data); n = 0
        for m in mats(q2):
            full = m['name'].split(',')[0]
            for pa in m['params']:
                for pg, off, val, lab in pfix:
                    if lab == full + '|' + pa['pname']:
                        assert abs(struct.unpack_from('<f', q2.d, q2.pbase(pa['val'][0]) + off)[0] - val) < 1e-3; n += 1
        assert n == len(pfix)
        assert sum(a != b for a, b in zip(src, data)) <= 4 * len(pfix)
        return data, dict(bindings=0, added=0, params=len(pfix))
    # writer with every original page raw
    w = PakWriter(); w.flags = q.hdr['flags']
    byp = {}
    for a, b, off in q.fixups: byp.setdefault(a, []).append((off, b))
    raws = [w.raw_page(q.d[po:po + ps], byp.get(pi, [])) for pi, (po, ps, _u) in enumerate(q.pages)]
    newp = w.page()
    strs = Blob('strs028')
    vram = bytearray(q.d[q.vram_base:])
    new = []
    reuse = []
    for key in sorted(want):
        tm = want[key]
        nshort = tm['short']
        hx = '%016X' % (sid(nshort) & 0xffffffffffffffff)
        short_name = nshort + '/' + hx + '.ndb'
        path_ndb = PFX + short_name
        hsh = sid(short_name)
        texels = open(os.path.join(datadir, tm['file']), 'rb').read()
        assert md5b(texels) == tm['md5'], ('data file changed', tm['file'])
        while len(vram) % 0x100: vram.append(0)
        voff = len(vram); vram += texels
        d = bytearray(0x38)
        struct.pack_into('<QQIIIIIIIIII', d, 0, 0, voff, len(texels), hsh, 1, tm['fmt'], 0x010f0000 if tm['fmt'] in (0x48, 0x4b, 0x4e) else 0x01000000,
                         tm['mips'], tm['w'], tm['h'], 0, 3)
        db = Blob('vramdesc'); db.raw(bytes(d)); db.ptr(db, 0x40); db.cstr(path_ndb)
        nb = Blob('texname'); nb.cstr(short_name)
        name_item = newp.item('RAW_DATA', nb)
        desc_item = newp.item('VRAM_DESC', db)
        new.append(dict(key=key, hash=hsh, desc=desc_item, name=name_item, full=path_ndb[:-4], short=nshort, fmt=tm['fmt']))
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
    hashb, namesb, descb = Blob('tthash028'), Blob('ttnames028'), Blob('ttdesc028')
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
    vb = Blob('vramtable028'); vb.u32(len(allt)).u32(0)
    for kind, e in allt:
        if kind == 'o':
            pg, off = vmap[tuple(e['desc'])]; vb.ptr(raws[pg], off)
        else:
            vb.ptr(e['desc'].data, -0x20)
    vitem = newp.item('VRAM_DESC_TABLE', vb, listed=False)
    # GEOMETRY_1 texture array
    g = q.items_of('GEOMETRY_1')[0]; gpg, gb = g.page, g.data_off
    ntex = q.pu32(gpg, gb + 0x38); gtp = q.resolve(gpg, gb + 0x40)
    geob = Blob('geotex028')
    for i in range(ntex):
        e = gtp[1] + 0x28 * i
        s = q.resolve(gtp[0], e)
        geob.ptr(raws[s[0]], s[1]); geob.raw(q.d[q.pbase(gtp[0]) + e + 8:q.pbase(gtp[0]) + e + 0x28])
    for e in new:
        geob.ptr(strs, strs.cstr(e['full']))
        geob.raw(struct.pack('<IIIiIIQ', e['hash'], 0x400, 0x101, -1, 0, {0x47: 4, 0x48: 4, 0x4a: 5, 0x4b: 5, 0x4d: 5, 0x4e: 5}[e['fmt']], 0))
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
    for pg, off, val, _l in pfix:
        raws[pg].set_u32(off, struct.unpack('<I', struct.pack('<f', val))[0])
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
                assert bd['short'].startswith('art/generated/u3mp/h028/'); nfix += 1
        for pa in m['params']:
            for pg, off, val, lab in pfix:
                if lab == full + '|' + pa['pname']:
                    assert abs(struct.unpack_from('<f', q2.d, q2.pbase(pa['val'][0]) + off)[0] - val) < 1e-3; nfix += 1
    assert nfix == len(todo) + len(pfix), (nfix, len(todo), len(pfix))
    for pi, (po, ps, _u) in enumerate(q.pages):
        a = q.d[po:po + ps]; b2 = q2.d[q2.pages[pi][0]:q2.pages[pi][0] + q2.pages[pi][1]]
        assert len(a) == len(b2)
    return data, dict(bindings=len(todo), added=len(new), params=len(pfix))


def main():
    a = [x for x in sys.argv[1:] if not x.startswith('--')]; mode = ([x for x in sys.argv[1:] if x.startswith('--')] or ['--apply'])[0]
    work, bak = a[0], a[1]
    D = json.load(open(os.path.join(here, 'data', 'targets.json')))
    if mode == '--scan':
        rows = [[pk, 'r3', '', '', '', '', '', '', ''] for pk in sorted(D['targets'])]
    else:
        rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(here, 'SWAP.tsv')) if l.strip() and not l.startswith('pak\t')]
    if mode not in ('--check', '--scan'): os.makedirs(bak, exist_ok=True)
    ok = skip = 0
    bypak = {}
    for r in rows:
        bypak.setdefault((r[0], r[1]), []).append(r)
    for (pak, ov), states in bypak.items():
        dst = os.path.join(work, 'overlay-' + ov, 'u3data', 'build', 'main', 'pak23', 'ps4', pak)
        if mode == '--scan':
            for ov2 in ('r3', 'r2', 'port', 'mp'):
                d2 = os.path.join(work, 'overlay-' + ov2, 'u3data', 'build', 'main', 'pak23', 'ps4', pak)
                if os.path.exists(d2): break
            src = open(d2, 'rb').read(); m = md5b(src)
            data, rep = build(src, D['targets'][pak], D['textures'], os.path.join(here, 'data'))
            print('SCAN', pak, ov2, m, md5b(data) if data else 'FAIL', json.dumps(rep)); continue
        bk = os.path.join(bak, 'overlay-' + ov + '__' + pak)
        if mode == '--revert':
            if os.path.exists(bk): shutil.copyfile(bk, dst); print('reverted', pak); ok += 1
            continue
        if not os.path.exists(dst): print('MISSING', pak); skip += 1; continue
        src = open(dst, 'rb').read(); m = md5b(src)
        if any(m == r[3] for r in states): print('already', pak); ok += 1; continue
        st = [r for r in states if r[2] == m]
        if not st: print('SKIP (md5 changed)', pak, m); skip += 1; continue
        m1 = st[0][3]; label = st[0][8] if len(st[0]) > 8 else ''
        data, rep = build(src, D['targets'][pak], D['textures'], os.path.join(here, 'data'))
        if data is None: print('NOTHING', pak, rep); skip += 1; continue
        if md5b(data) != m1: print('MD5 MISMATCH (not written)', pak, md5b(data)); skip += 1; continue
        if mode in ('--check', '--dry'): print('ok-would-patch', pak, '[%s]' % label, rep); ok += 1; continue
        if os.path.exists(bk) and md5b(open(bk, 'rb').read()) != m: os.replace(bk, bk + '.old')
        if not os.path.exists(bk): shutil.copyfile(dst, bk)
        tmp = dst + '.h028tmp'; open(tmp, 'wb').write(data); os.replace(tmp, dst)
        if md5b(open(dst, 'rb').read()) != m1: print('MD5 MISMATCH after write, restoring', pak); shutil.copyfile(bk, dst); skip += 1; continue
        print('patched', pak, '[%s]' % label, rep); ok += 1
    print('ok', ok, 'skipped', skip)


if __name__ == '__main__': main()
