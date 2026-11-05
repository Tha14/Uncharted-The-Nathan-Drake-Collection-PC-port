#!/usr/bin/env python3
"""Compare every out/*.pak with the pak the game loads now. Writes verify.json and prints a table. Read only outside this folder."""
import sys, os, json, io, contextlib
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); sys.path.insert(0, HERE)
from lib import *
import ps4actor as A, ps4check, ps4verify, crashcheck
A.tmpl_fg_info = _fg if False else A.tmpl_fg_info


def geo(q):
    g = q.items_of('GEOMETRY_1')[0]; pg, b = g.page, g.data_off
    ns = q.pu32(pg, b + 8); r = q.resolve(pg, b + 0x10); c = collections.Counter(); nv = ni = 0; mj = -1; names = {}
    for i in range(ns):
        o = r[1] + 0x88 * i
        x = q.resolve(r[0], o + 0x48); m = q.pstr(*x).split(',')[0] if x else None; c[m] += 1
        v = q.pu32(r[0], o + 0x20); nv += v; ni += q.pu32(r[0], o + 0x24)
        names[q.pstr(*q.resolve(r[0], o + 8))] = (m, v)
        sk = q.resolve(r[0], o + 0x60)
        if sk:
            tot = q.pu32(sk[0], sk[1] + 4); jr = q.resolve(sk[0], sk[1] + 0x18)
            if jr and tot:
                a = struct.unpack_from('<%dH' % tot, q.d, q.pbase(jr[0]) + jr[1]); mj = max(mj, max(a))
    return dict(subs=ns, mats=len(c), nv=nv, ni=ni, maxj=mj, used=c, names=names, cnt=q.pu32(pg, b + 0x48))


def errs(f):
    with contextlib.redirect_stdout(io.StringIO()):
        e, _, _ = ps4check.check(f, quiet=True)
    return [x for x in e if not x.startswith('unknown item type') and not (x.startswith('tiled size') and 'env-reflection' in x)]


def fg(q):
    fi = A.tmpl_fg_info(q)
    return fi[0], fi[1], fi[3], hashlib.sha1(fi[2]).hexdigest()[:8], sorted(i.name for i in q.items_of('FOREGROUND_SECTION_2')), len(q.items_of('TAG_VEC4')), len(q.items_of('TAG_INT')), len(q.items_of('TAG_FLOAT')), q.items_of('ARTGROUP_DATA')[0].name


res = {}
for fn in sorted(os.listdir('out')):
    if not fn.endswith('.pak'):
        continue
    n = fn[:-4]; new = 'out/' + fn; old, lay = find('actor33', n); meta = json.load(open(f'out/{n}.json'))
    qn, qo = Pak(new), Pak(old); gn, go = geo(qn), geo(qo); fn_, fo = fg(qn), fg(qo)
    en, eo = errs(new), errs(old)
    vn, _ = ps4verify.verify(new, name=n, quiet=True)
    cn, _ = crashcheck.check(new); co, _ = crashcheck.check(old)
    mt = A.tmpl_materials(qn)
    flat = [(m['name'].split(',')[0].split('/')[-2:], b[0][9:]) for m in mt for b in m['binds'] if 'flat-' in b[2] or 'generated/u3mp' in b[1] or 'generated/u3mp' in b[2]]
    r = dict(layer=lay, ps3src=meta['src'], template=meta['template'], cap=meta['cap'], before=go['mats'], after=gn['mats'], tabB=go['cnt'], tabA=gn['cnt'],
             size=(os.path.getsize(old), os.path.getsize(new)), vram=(qo.u32(0x20), qn.u32(0x20)),
             subs=(go['subs'], gn['subs']), nv=(go['nv'], gn['nv']), ni=(go['ni'], gn['ni']), maxj=(go['maxj'], gn['maxj']),
             fg_same=fn_ == fo, skel=hex(fn_[0]), hier=hex(fn_[1]), nj=fn_[2], jh=fn_[3], fg_old=fo[:4], check=(len(eo), len(en)), check_new=en[:6],
             verify=vn[:6], crash=(len(co), len(cn)), crash_new=cn[:6], crash_old=co[:3], cb3=meta['cb3'], cubes=meta['cubes'], flat=flat,
             roles=meta['roles'], old_used={k: v for k, v in go['used'].items()})
    res[n] = r
    print(f"{n:32s} mats {go['mats']:2d}->{gn['mats']:2d} tab {go['cnt']}->{gn['cnt']} subs {go['subs']}->{gn['subs']} nv {go['nv']}->{gn['nv']} ni {go['ni']}->{gn['ni']} vram {qo.u32(0x20):#x}->{qn.u32(0x20):#x} size {os.path.getsize(new)} joints_same={fn_[:4]==fo[:4]} fg/tags_same={fn_==fo} maxj {gn['maxj']}/{fn_[2]} check {len(eo)}->{len(en)} crash {len(co)}->{len(cn)} verify {len(vn)} flat {len(flat)} cap {meta['cap']}")
    for x in en + vn + cn:
        print('    !', x[:170])
    if fn_ != fo:
        print('    fg diff', [a for a in zip(fo, fn_) if a[0] != a[1]])
json.dump(res, open('verify.json', 'w'), indent=1, default=str)
