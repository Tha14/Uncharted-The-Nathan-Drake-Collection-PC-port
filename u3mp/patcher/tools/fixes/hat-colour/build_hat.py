#!/usr/bin/env python3
"""Rebuild one converted actor pak from its PS3 source with a retail PS4 template, the actor-materials way
(ps4actor_am + cubefix + cb3fix), into out/<name>.pak, then gate it against the pak the game loads now.
  build_hat.py NAME TEMPLATE [CAP]      (CAP = top texture size, default 256)
Writes only inside this folder (ps3/, out/)."""
import sys, os, io, json, hashlib, contextlib
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE)
AM = os.path.join(HERE, '..', 'actor-materials'); sys.path.insert(0, AM); sys.dont_write_bytecode = True
from lib import *
import ps4actor_am as A, ps3geom, ps4check, ps4verify, crashcheck
from ps3mat_am import read_materials
from build import role, pool, kinds_of_binds, BUDGET
os.chdir(HERE)   # build.py chdirs to actor-materials on import
sys.path.insert(0, T + '/stage')
import cubefix, cb3fix


def errs(f):
    with contextlib.redirect_stdout(io.StringIO()):
        e, _, _ = ps4check.check(f, quiet=True)
    # 'type 2' = a cube environment map copied from the retail template (retail paks report it too)
    return [x for x in e if not x.startswith('unknown item type') and not (x.startswith('tiled size') and 'env-reflection' in x) and x != 'type 2']


def fg(q):
    f = A.tmpl_fg_info(q)
    return f[0], f[1], f[3], hashlib.sha1(f[2]).hexdigest()[:8], sorted(i.name for i in q.items_of('FOREGROUND_SECTION_2'))


def geo(q):
    g = q.items_of('GEOMETRY_1')[0]; pg, b = g.page, g.data_off; r = q.resolve(pg, b + 0x10); c = set(); nv = ni = 0
    for i in range(q.pu32(pg, b + 8)):
        o = r[1] + 0x88 * i; c.add(q.pstr(*q.resolve(r[0], o + 0x48)).split(',')[0]); nv += q.pu32(r[0], o + 0x20); ni += q.pu32(r[0], o + 0x24)
    return c, q.pu32(pg, b + 8), nv, ni


def ps3_tint(name):
    """{PS3 material key: has tint mask} for the used, non-shadowblocker materials of ps3/<name>.pak (written by build)"""
    p = Pak(f'ps3/{name}.pak'); used = set()
    for v in ps3geom.prototypes(p).values():
        used |= set(A.ps3_block_materials(p, v['proto']))
    mats, texs = read_materials(p)
    r = {m['name'].rsplit('/', 1)[0] + ':' + m['name'].rsplit('/', 1)[1]: any('tint-mask' in texs[i]['name'] for i in m['textures'] if i is not None)
         for m in mats if m['name'] in used and 'shadowblocker' not in m['name']}
    r['__n__'] = len(used)            # every used PS3 material, shadowblocker included
    return r


def tint_wrong(name, tint_new):
    """PS3 materials whose rebuilt PS4 material has the tint features but no mask, or a mask but no tint features"""
    want = ps3_tint(name); want.pop('__n__', None); out = []
    for k, w in want.items():
        t = tint_new.get(k)
        if t is None: out.append(k + ' missing')
        elif w and not (t[0] and t[1]): out.append(k + ' lost')
        elif not w and t[0]: out.append(k + ' extra')
    return out


def tintinfo(q):
    out = {}
    for m in A.tmpl_materials(q):
        out[m['name'].split(',')[0]] = ('COLOR_1_APPLY_INSTANCE_TINT' in m['feats'], [b[1].split('/')[-1] for b in m['binds'] if b[0] == 'g_tNdFetchColorTintMask1Map'])
    return out


def build(name, tname, cap=256, verbose=True):
    A.MAX_TEX = cap
    s = S3(); src, d = s.get(name)
    if not d:
        raise SystemExit('no PS3 source for ' + name)
    os.makedirs('ps3', exist_ok=True); os.makedirs('out', exist_ok=True)
    p3 = f'ps3/{name}.pak'; open(p3, 'wb').write(d)
    tpl = f'{RET}/{tname}.pak'
    p = Pak(d); tq = Pak(tpl); tm = A.tmpl_materials(tq)
    used = set()
    for v in ps3geom.prototypes(p).values():
        used |= set(A.ps3_block_materials(p, v['proto']))
    mats, texs = read_materials(p)
    real = {m['name']: [texs[i]['name'] for i in m['textures'] if i is not None] for m in mats}
    choice, report = {}, []
    keys = {m['name'].split(',')[0]: m for m in tm}
    for mn in sorted(used):
        key = mn.rsplit('/', 1)[0] + ':' + mn.rsplit('/', 1)[1]
        if 'shadowblocker' in mn:
            report.append((mn, 'shadowblocker', 'shadowblocker')); continue
        if key in keys:
            report.append((mn, 'exact', key)); continue
        r = role(mn); c = pool(tm, r); warn = ''
        if not c:
            c = pool(tm, 'cloth'); warn = ' NO-SLOT-FOR-' + r
        ks = {A.tex_kind(x.split('art/')[-1]) for x in real.get(mn, [])} - {None}
        # the tint mask decides: a PS3 material with one must get a template that reads it, one without must not
        want_tint = '-tint-mask' in ks
        c2 = [m for m in c if ('COLOR_1_APPLY_INSTANCE_TINT' in m['feats']) == want_tint] or c
        # overlay/UV3 variants need a second UV set the converted mesh does not have
        # (HAT_NO_OVERLAY_FILTER=1 reproduces the paks built before this filter existed: recipe jobs-hats.tsv)
        if os.environ.get('HAT_NO_OVERLAY_FILTER') != '1':
            c2 = [m for m in c2 if not any('OVERLAY' in f or 'UV_3' in f for f in m['feats'])] or c2
        best = max(c2, key=lambda m: len(ks & kinds_of_binds(A, m)) - len(kinds_of_binds(A, m) - ks))
        choice[mn] = best['name']
        report.append((mn, r + warn, best['name'].split(',')[0], sorted(ks), 'tint' if 'COLOR_1_APPLY_INSTANCE_TINT' in best['feats'] else 'no-tint'))
    dst = f'out/{name}.pak'
    with contextlib.redirect_stdout(io.StringIO()):
        A.convert(p3, tpl, dst, mat_choice=choice, verbose=False)
    q_ = Pak(dst); end = q_.pages[-1][0] + q_.pages[-1][1]
    if end != q_.vram_base:
        raw = open(dst, 'rb').read(); open(dst, 'wb').write(raw[:end] + raw[q_.vram_base:])
    cf = cubefix.fix(dst, p3)
    n, raised = cb3fix.fix(dst, dst)
    meta = dict(src=src, template=tname, cap=cap, roles=report, cubes=list(cf or []), cb3=raised, vram=Pak(dst).u32(0x20))
    json.dump(meta, open(f'out/{name}.json', 'w'), indent=0)
    if verbose:
        for r in report: print('  ', r)
    return dst, meta


def gate(name, dst, verbose=True):
    """compare out/<name>.pak with the pak the game loads now; returns (list of problems, info)"""
    oldf, lay = find('actor33', name); old = Pak(oldf); q = Pak(dst)
    eo, en = errs(oldf), errs(dst); vn, _ = ps4verify.verify(dst, name=name, quiet=True); cn, _ = crashcheck.check(dst); co, _ = crashcheck.check(oldf)
    mo, so, vo, io_ = geo(old); mn, sn, vn_, in_ = geo(q)
    bad = []
    if len(en) > len(eo): bad.append('ps4check %d>%d %s' % (len(en), len(eo), en[:3]))
    if cn: bad.append('crash ' + cn[0][:100])
    if vn: bad.append('verify ' + vn[0][:100])
    if fg(q) != fg(old): bad.append('joints/fg differ %s %s' % (fg(q)[:4], fg(old)[:4]))
    # the old mp-hero-sullivan-part-glasses-a was converted from the disc pak (341 verts); patch13 has 380: the newer source wins
    if (sn, vn_, in_) != (so, vo, io_) and name not in ('mp-hero-sullivan-part-glasses-a',): bad.append('geometry differs %s %s' % ((sn, vn_, in_), (so, vo, io_)))
    if len(mn) != len(mo) and len(mn) != ps3_tint(name)['__n__']: bad.append('materials %d!=%d' % (len(mn), len(mo)))   # or the PS3 count (+ shadowblocker)
    tw = tint_wrong(name, tintinfo(q))
    if tw: bad.append('tint wrong: ' + ' '.join(x.split('/')[-1] for x in tw))
    if q.u32(0x20) > BUDGET: bad.append('vram over %#x' % q.u32(0x20))
    info = dict(layer=lay, old=oldf, check=(len(eo), len(en)), check_new=en[:4], crash=(len(co), len(cn)), verify=vn[:4], vram=(old.u32(0x20), q.u32(0x20)),
                size=(os.path.getsize(oldf), os.path.getsize(dst)), tint_old=tintinfo(old), tint_new=tintinfo(q))
    if verbose:
        print(name, lay, 'check %d->%d crash %d->%d verify %d vram %#x->%#x size %d->%d' % (len(eo), len(en), len(co), len(cn), len(vn), old.u32(0x20), q.u32(0x20), os.path.getsize(oldf), os.path.getsize(dst)))
        for k, v in info['tint_new'].items(): print('   new', k, v, '  (old:', info['tint_old'].get(k), ')')
        for b in bad: print('   BAD', b)
    return bad, info


if __name__ == '__main__':
    name, t = sys.argv[1], sys.argv[2]; cap = int(sys.argv[3]) if len(sys.argv) > 3 else 256
    dst, meta = build(name, t, cap)
    bad, info = gate(name, dst)
    print('RESULT', name, 'OK' if not bad else 'REJECT')
