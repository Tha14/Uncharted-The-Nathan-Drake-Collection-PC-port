#!/usr/bin/env python3
"""apply_mapcolor2.py - restore PS3 blend/stripes on map materials using effects ALREADY in each pak.
 class Zflat : PS3 blends from normal-map blue, PS4 BlendMap is the flat mask -> append grey BC1 of that blue, rebind BlendMap.
 class Zvc   : same source, PS4 effect is vertex-colour only -> also retarget to a sibling effect (blend + spec + env).
 class stripe: PS3 uv2 overlay dropped -> retarget to the overlay effect already in the pak, bind the overlay.
 Default = dry run (builds in memory, overlay untouched). --out DIR writes built paks there. --apply backs up to
 work/backup-mapcolor2 then writes the overlay. --revert restores. --concrete-stripes also does concrete -hazard/-red
 (restores the stripes but loses their gloss and tint3).
 The lightmap-flag fix (instsub +0x20, backup-mapcolor/manifest.json) is asserted unchanged."""
import sys, os, json, re, struct, hashlib, shutil, subprocess, pickle, gzip, collections
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from lib import *
from ndpak import sid
from ps4writer import PakWriter, Blob
import texcodec as tc, bc1inv
from psarc_lib import Psarc
from sm import subs
from fxin import inputs
PFX = 'd:/dev/CP9/Data/U3/CookData/ND/build/texture2/'
BK = W + 'backup-mapcolor2/'
CAP = 256
BLENDP = 'BLEND_FACTOR_USE_BLEND_MAP_AND_VERTEX_COLOR'
INV = 'BLEND_FACTOR_INVERT_BLEND_MAP'


def md5b(b): return hashlib.md5(b).hexdigest()


def blueblocks(lin, bpb):
    """linear DXT1/DXT5 colour blocks -> DXT1 blocks with colour = grey(blue), same convention as retail '-blend' maps"""
    out = bytearray(len(lin) // bpb * 8)
    g = lambda c: (c & 31) << 11 | (((c & 31) << 1) | ((c & 31) >> 4)) << 5 | (c & 31)
    for n in range(len(lin) // bpb):
        c0, c1, ix = struct.unpack_from('<HHI', lin, n * bpb + (8 if bpb == 16 else 0))
        a, b = g(c0), g(c1)
        if c0 > c1 or bpb == 16:            # 4-colour block: must keep a > b
            if a < b:
                a, b = b, a; ix ^= 0x55555555
            elif a == b:
                if a == 0: a = 1
                else: b = a - 1
                ix = 0
        elif a > b:                          # 3-colour block: must keep a <= b
            a, b = b, a
            ix = sum(({0: 1, 1: 0, 2: 2, 3: 3}[(ix >> (2 * k)) & 3]) << (2 * k) for k in range(16))
        struct.pack_into('<HHI', out, n * 8, a, b, ix)
    return bytes(out)


class P3:
    idx = pickle.load(open(os.path.join(HERE, 'p3idx.pkl'), 'rb')); arcs = {}

    @classmethod
    def pak(c, name):
        a, i, n = c.idx[name][-1]
        return Pak(c.arcs.setdefault(a, Psarc(a)).read(i))


def ps3_tex(p, base):
    for t in p.textures():
        if t['path'].split('/')[-2] == base:
            return t


def chain(p, t):
    kind, bpb, comp = tc.ps3_format(t)
    if not comp or kind not in ('DXT1', 'DXT5'):
        raise ValueError('format ' + kind)
    d = p.vram_bytes(t['vramOffset'], t['vramSize']); w, h, m = t['width'], t['height'], t['mips']
    while max(w, h) > CAP and m > 1:
        d = d[tc.ps3_linear_size(w, h, 1, bpb):]; w, h, m = max(1, w >> 1), max(1, h >> 1), m - 1
    return d[:tc.ps3_linear_size(w, h, m, bpb)], w, h, m, kind, bpb


def plan(concrete):
    R = [json.loads(l) for l in open(os.path.join(HERE, 'rows.jsonl'))]
    by = collections.defaultdict(list)
    O = {x['pak']: x for x in map(json.loads, gzip.open(os.path.join(HERE, 'ov.jsonl.gz'), 'rt')) if 'mats' in x}
    for r in R:
        s = r['src']; m = next(x for x in O[r['pak']]['mats'] if x['n'].split(',')[0] == r['mat'])
        ov = [d for d in r['dropped'] if d[2] == 'color' and re.search('hazard|striped|decal', d[1])]
        if s and s[2] == 'z' and not r['shared']:
            nrm = (s[1] if s[1] and 'normal' in s[1] else None) or next((b[1].split('/')[-1] for b in m['b'] if 'Normal1' in b[0]), None)
            blend = next((b[1] for b in m['b'] if 'BlendMap' in b[0]), None)
            if nrm and r['vc_only']:
                by[r['pak']].append(dict(cls='Zvc', mat=r['mat'], nrm=nrm, row=r))
            elif nrm and blend and 'generated/u3mp/flat' in blend:
                by[r['pak']].append(dict(cls='Zflat', mat=r['mat'], nrm=nrm, row=r))
        if ov and r['blendmap'] and re.search(r'-(hazard|striped)\b|multi-red', r['mat']) and (concrete or 'concrete' not in r['mat']):
            by[r['pak']].append(dict(cls='stripe', mat=r['mat'], ovl=ov[0][1], row=r))
    return by


def build(pak, rows):
    q = Pak(open(OV + pak, 'rb').read()); p3 = P3.pak(pak)
    M = mats4(q); byn = {m['n'].split(',')[0]: m for m in M}; SU = subs(q); FX = inputs(q)
    log = []; w = PakWriter(); w.flags = q.hdr['flags']; byp = {}
    for a, b, off in q.fixups:
        byp.setdefault(a, []).append((off, b))
    raws = [w.raw_page(q.d[po:po + ps], byp.get(pi, [])) for pi, (po, ps, _u) in enumerate(q.pages)]
    newp = w.page(); strs = Blob('strs2'); vram = bytearray(q.d[q.vram_base:])
    TB = {t['path'].split('/')[-2]: t for t in q.textures()}
    new = {}

    def addtex(short, texels, fmt, mips, w_, h_, cls):
        if short in new:
            return new[short]
        hx = '%016X' % (sid(short) & 0xffffffffffffffff); sn = short + '/' + hx + '.ndb'; pn = PFX + sn; hsh = sid(sn)
        while len(vram) % 0x100:
            vram.append(0)
        voff = len(vram); vram.extend(texels)
        d = bytearray(0x38); srgb = fmt in (0x48, 0x4e)
        struct.pack_into('<QQIIIIIIIIII', d, 0, 0, voff, len(texels), hsh, 1, fmt, 0x010f0000 if srgb else 0x01000000, mips, w_, h_, 0, 3)
        db = Blob('vd'); db.raw(bytes(d)); db.ptr(db, 0x40); db.cstr(pn); nb = Blob('tn'); nb.cstr(sn)
        ni = newp.item('RAW_DATA', nb); di = newp.item('VRAM_DESC', db)
        new[short] = dict(short=short, full=pn[:-4], hash=hsh, desc=di, name=ni, fmt=fmt, cls=cls)
        return new[short]

    def blendtex(nrm, invert):
        t3 = ps3_tex(p3, nrm)
        if t3 is None:
            raise ValueError('no PS3 normal ' + nrm)
        lin, w_, h_, m_, kind, bpb = chain(p3, t3)
        gb = blueblocks(lin, bpb)
        if invert:
            gb = bc1inv.invert(gb)
        return addtex('art/generated/u3mp/zblend/' + nrm[:-4] + ('-zblendinv.tga' if invert else '-zblend.tga'),
                      tc.ps3_to_ps4_texels(gb, w_, h_, m_, 8), 0x47, m_, w_, h_, 4)

    def donor_ok(d, mat):
        need = {a[1] for a in FX[d['h']]} - {15}
        for sig, _n in SU.get(mat, []):
            have = {e[0] for st in sig for e in st}
            if not need <= have:
                return False, sorted(need - have)
        return True, []

    edits = []
    for r in rows:
        m = byn.get(r['mat'])
        if m is None:
            log.append(('skip', r['mat'], 'not in pak')); continue
        try:
            if r['cls'] == 'Zflat':
                e = blendtex(r['nrm'], INV in m['f'])
                edits.append((m, None, {'g_tNdFetchBlendMap': e}, {})); log.append(('Zflat', r['mat'], e['short']))
            elif r['cls'] == 'Zvc':
                stem, var = r['mat'].split(':')
                sib = [x for x in M if x['n'].startswith(stem + '-') and x['n'].split(',')[0].endswith(':' + var) and BLENDP in x['f']
                       and 'FINAL_COLOR_ADD_SPECULAR_LIGHT' in x['f'] and donor_ok(x, r['mat'])[0]]
                if not sib:
                    log.append(('skip', r['mat'], 'no sibling effect with compatible streams')); continue
                d = sib[0]; e = blendtex(r['nrm'], INV in d['f'])
                pv = {k: v for k, v in r['row']['ps3p'].items() if not k.startswith('g_specular')}  # sibling's spec already converted
                edits.append((m, d, {'g_tNdFetchBlendMap': e}, pv)); log.append(('Zvc', r['mat'], d['h'], e['short']))
            else:
                d = next((x for x in M if all(f in x['f'] for f in ('COLOR_3_APPLY_OVERLAY_TEXTURE', 'COLOR_3_OVERLAY_USE_UV2',
                          'COLOR_3_OVERLAY_MASK_OVERLAY_TEXTURE_A', 'COLOR_1_BLEND_COLOR_2', 'BLEND_FACTOR_SOFTNESS', 'COLOR_2_APPLY_COLOR_TINT', BLENDP))
                          and (INV in x['f']) == (INV in m['f'])), None)
                if d is None:
                    log.append(('skip', r['mat'], 'no overlay effect of this polarity in pak')); continue
                ok, miss = donor_ok(d, r['mat'])
                if not ok:
                    log.append(('skip', r['mat'], 'streams lack %s' % miss)); continue
                ot = TB.get(r['ovl'])
                if ot is None:
                    t3 = ps3_tex(p3, r['ovl']); lin, w_, h_, m_, kind, bpb = chain(p3, t3)
                    if kind != 'DXT5' or (t3['gcmRemapCtl'] & 0xffff) != 0xaae4:
                        log.append(('skip', r['mat'], 'overlay has no plain alpha')); continue
                    oe = addtex('art/generated/u3mp/zblend/' + r['ovl'], tc.ps3_to_ps4_texels(lin, w_, h_, m_, 16), 0x4e, m_, w_, h_, 5)
                else:
                    oe = dict(full=ot['path'][:-4], short=ot['path'].split('texture2/')[1].rsplit('/', 1)[0])
                wh = addtex('art/generated/u3mp/flat-white-u3mc.tga', tc.ps3_to_ps4_texels(struct.pack('<HHI', 0xffff, 0xfffe, 0), 4, 4, 1, 8), 0x47, 1, 4, 4, 4)
                pv = dict(r['row']['ps3p']); pv.update(g_uvOffsetU3=[0.0], g_uvOffsetV3=[0.0], g_uvScale3=[1.0])
                edits.append((m, d, {'g_tNdFetchOverlay3Map': oe, 'g_tNdFetchColorTintMask2Map': wh}, pv)); log.append(('stripe', r['mat'], d['h'], r['ovl']))
        except ValueError as ex:
            log.append(('skip', r['mat'], str(ex)))
    if not edits:
        return None, log
    vitem = None
    g = q.items_of('GEOMETRY_1')[0]; gpg, gb = g.page, g.data_off
    if new:
        tt = q.items_of('TEXTURE_TABLE')[0]; tp, tb = tt.page, tt.data_off; n0 = q.u64(q.pbase(tp) + tb)
        hp = q.resolve(tp, tb + 8); npp = q.resolve(tp, tb + 0x10); dp = q.resolve(tp, tb + 0x18); old = []
        for i in range(n0):
            old.append(dict(hash=q.pu32(hp[0], hp[1] + 4 * i), name=q.resolve(npp[0], npp[1] + 8 * i), desc=q.resolve(dp[0], dp[1] + 8 * i)))
        allt = sorted([('o', o) for o in old] + [('n', e) for e in new.values()], key=lambda x: x[1]['hash'])
        hb, nb, db = Blob('h2'), Blob('n2'), Blob('d2')
        for k, e in allt:
            hb.u32(e['hash'])
            if k == 'o':
                nb.ptr(raws[e['name'][0]], e['name'][1]); db.ptr(raws[e['desc'][0]], e['desc'][1])
            else:
                nb.ptr(e['name'].data); db.ptr(e['desc'].data)
        for bl in (hb, nb, db):
            newp.item('RAW_DATA', bl)
        raws[tp].set_u64(tb, len(allt)); raws[tp].redirect(tb + 8, hb); raws[tp].redirect(tb + 0x10, nb); raws[tp].redirect(tb + 0x18, db)
        vmap = {(pg, off + 0x20): (pg, off) for pg, off in q.vram_desc_table()}
        vb = Blob('vt2'); vb.u32(len(allt)).u32(0)
        for k, e in allt:
            if k == 'o':
                pg, off = vmap[tuple(e['desc'])]; vb.ptr(raws[pg], off)
            else:
                vb.ptr(e['desc'].data, -0x20)
        vitem = newp.item('VRAM_DESC_TABLE', vb, listed=False)
        ntex = q.pu32(gpg, gb + 0x38); gtp = q.resolve(gpg, gb + 0x40)
        geob = Blob('gt2')
        for i in range(ntex):
            e = gtp[1] + 0x28 * i; s = q.resolve(gtp[0], e); geob.ptr(raws[s[0]], s[1]); geob.raw(q.d[q.pbase(gtp[0]) + e + 8:q.pbase(gtp[0]) + e + 0x28])
        for e in new.values():
            geob.ptr(strs, strs.cstr(e['full'])); geob.raw(struct.pack('<IIIiIIQ', e['hash'], 0x400, 0x101, -1, 0, e['cls'], 0))
        newp.item('RAW_DATA', geob); raws[gpg].set_u32(gb + 0x38, ntex + len(new)); raws[gpg].redirect(gb + 0x40, geob)
    mp, mo = q.resolve(gpg, gb + 0x50)
    for m, d, ov, pv in edits:
        o = mo + m['i'] * 0x48; src = d or m; so = mo + src['i'] * 0x48
        if d is not None:
            fr = q.resolve(mp, so + 0x10); hr = q.resolve(mp, so + 0x18)
            raws[mp].redirect(o + 0x10, raws[fr[0]], fr[1]); raws[mp].redirect(o + 0x18, raws[hr[0]], hr[1])
            pr = q.resolve(mp, so + 0x20); pb = Blob('par2'); vb_ = Blob('val2')
            old_p = {x[0]: x[2] for x in m['p']}
            for k, (nm, cnt, val) in enumerate(d['p']):
                po = pr[1] + 0x18 * k; nr = q.resolve(pr[0], po)
                v = pv.get(nm) or old_p.get(nm)
                if v is None:
                    v = [1.0] * 4 if re.match(r'g_color(Tint|Brightness)|g_uvScale', nm) else val
                vo = vb_.pos; vb_.raw(struct.pack('<4f', *(list(v) + [0.0] * 4)[:4]))
                pb.ptr(raws[nr[0]], nr[1]); pb.ptr(vb_, vo); pb.raw(q.d[q.pbase(pr[0]) + po + 0x10:q.pbase(pr[0]) + po + 0x18])
            newp.item('RAW_DATA', vb_); newp.item('RAW_DATA', pb)
            raws[mp].redirect(o + 0x20, pb)
            raws[mp].data[o + 0x30:o + 0x40] = q.d[q.pbase(mp) + so + 0x30:q.pbase(mp) + so + 0x40]
        br = q.resolve(mp, so + 0x28); bb = Blob('bnd2'); ot = q.resolve(mp, o + 0x28)
        tgt = {m['b'][k][0]: ot[1] + 0x28 * k for k in range(len(m['b']))}
        for k, (nm, short) in enumerate(src['b']):
            e = br[1] + 0x28 * k; nr = q.resolve(br[0], e); bb.ptr(raws[nr[0]], nr[1])
            if nm in ov:
                bb.ptr(strs, strs.cstr(ov[nm]['short'])); bb.ptr(strs, strs.cstr(ov[nm]['full']))
            else:
                pg2, e2 = (ot[0], tgt[nm]) if nm in tgt else (br[0], e)
                a = q.resolve(pg2, e2 + 8); c = q.resolve(pg2, e2 + 0x10)
                bb.ptr(raws[a[0]], a[1]); bb.ptr(raws[c[0]], c[1])
            bb.raw(q.d[q.pbase(br[0]) + e + 0x18:q.pbase(br[0]) + e + 0x28])
        newp.item('RAW_DATA', bb); raws[mp].redirect(o + 0x28, bb)
    newp.item('RAW_DATA', strs)
    w.login_page = (q.hdr['loginPage'], q.hdr['loginOff']); w.vram_table_loc = (q.hdr['vramDescPage'], q.hdr['vramDescOff']); w.vram = bytes(vram)
    w.build()
    if vitem is not None:
        w.vram_table_loc = (newp.index, vitem.off)
    data = w.build()
    rem = newp.size % 16
    if rem:
        strs.raw(b'\0' * (16 - rem)); data = w.build(); assert newp.size % 16 == 0
    return data, log


def lm_locked(pak, data):
    man = json.load(open(W + 'backup-mapcolor/manifest.json')).get(pak)
    if not man:
        return True
    o, n = Pak(OV + pak), Pak(data)
    def at(p, q, off):
        for (po, ps, _), (po2, _s, _u) in zip(p.pages, q.pages):
            if po <= off < po + ps:
                return p.d[off:off + 4], q.d[po2 + off - po:po2 + off - po + 4]
        raise ValueError(off)
    return all(x == y and struct.unpack('<I', y)[0] & 0x100 == 0 for x, y in (at(o, n, e['off']) for e in man['edits']))


def main():
    a = sys.argv[1:]; apply = '--apply' in a; rev = '--revert' in a; conc = '--concrete-stripes' in a
    out = a[a.index('--out') + 1] if '--out' in a else None
    if (apply or rev) and any(l.split()[:1] == ['uncharted.exe'] for l in subprocess.run(['tasklist'], capture_output=True, text=True).stdout.lower().splitlines()):
        sys.exit('game running: stop it first')
    if rev:
        for f in os.listdir(BK):
            if f.endswith('.pak'):
                shutil.copy2(BK + f, OV + f); print('restored', f)
        return
    res = {}
    for pak, rows in sorted(plan(conc).items()):
        data, log = build(pak, rows)
        for l in log:
            print(pak, *l)
        if not data:
            continue
        if not lm_locked(pak, data):             # recipe 1.7.0: a pak whose base is not the recorded state is skipped, not fatal
            print(pak, 'SKIP: lightmap-flag lock fails (base pak differs from the recorded state)'); continue
        orig = open(OV + pak, 'rb').read()
        res[pak] = dict(md5_before=md5b(orig), md5_after=md5b(data), edits=[l for l in log if l[0] != 'skip'], grew=len(data) - len(orig))
        print(pak, 'OK +%d bytes' % res[pak]['grew'])
        if out:
            os.makedirs(out, exist_ok=True); open(os.path.join(out, pak), 'wb').write(data)
        if apply:
            os.makedirs(BK, exist_ok=True); shutil.copy2(OV + pak, BK + pak)
            open(OV + pak + '.new', 'wb').write(data); os.replace(OV + pak + '.new', OV + pak)
    json.dump(res, open(os.path.join(HERE, 'plan-result.json' + ('.concrete' if conc else '')), 'w'), indent=1)
    if apply:
        json.dump(res, open(BK + 'manifest.json', 'w'), indent=1)
    print('APPLIED' if apply else 'DRY RUN: overlay not written')


main()
