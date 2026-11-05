#!/usr/bin/env python3
"""Second batch: Flynn/Lazarevic co-op with fixed templates, and the shadowblocker-only paks with templates picked
automatically. Tries templates in rank order and keeps the first that passes every gate. Writes build2.json."""
import sys, os, json, pickle, subprocess, io, contextlib
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); sys.path.insert(0, HERE)
from lib import *
import ps4actor as A, ps3geom, ps4check, ps4verify, crashcheck
from build import role, SPECIAL, BUDGET
FIXED = {'mp-hero-flynn-coop': 'mp-hero-flynn-torso', 'mp-hero-lazarevic-coop': 'mp-hero-lazarevic-torso'}
AUTO = """mp-hero-french-hair-coop mp-hero-fat-hair mp-kit-killzone-helmet-a mp-kit-merc-arms-gloves-b mp-kit-merc-arms-gloves-d
mp-kit-merc-head-helmet-b mp-kit-merc-head-helmet-c mp-kit-merc-head-helmet-e mp-kit-merc-head-helmet-k mp-kit-merc-head-mask-a
mp-kit-merc-head-wrap-a mp-kit-merc-head-wrap-b mp-kit-pirate-arms-gloves-b-00 mp-kit-pirate-feet-boots-b-00
mp-kit-pirate-feet-boots-c-00 mp-kit-pirate-feet-sandles-a-00 mp-kit-pirate-feet-sandles-b-00 mp-kit-pirate-helmet-wrap-a-00
mp-kit-pirate-helmet-wrap-a-03 mp-kit-pirate-helmet-wrap-a-04 mp-kit-pirate-helmet-wrap-a-06 mp-kit-pirate-helmet-wrap-b-00""".split()
R = pickle.load(open('retail.pkl', 'rb')); s3 = S3()


def errs(f):
    with contextlib.redirect_stdout(io.StringIO()):
        e, _, _ = ps4check.check(f, quiet=True)
    return [x for x in e if not x.startswith('unknown item type') and not (x.startswith('tiled size') and 'env-reflection' in x)]


def mats_used(q):
    g = q.items_of('GEOMETRY_1')[0]; pg, b = g.page, g.data_off; r = q.resolve(pg, b + 0x10); c = set(); nv = 0
    for i in range(q.pu32(pg, b + 8)):
        o = r[1] + 0x88 * i; c.add(q.pstr(*q.resolve(r[0], o + 0x48)).split(',')[0]); nv += q.pu32(r[0], o + 0x20)
    return c, q.pu32(pg, b + 8), nv


def fg(q):
    f = A.tmpl_fg_info(q)
    return f[0], f[1], f[3], hashlib.sha1(f[2]).hexdigest()[:8], sorted(i.name for i in q.items_of('FOREGROUND_SECTION_2'))


def candidates(name, old):
    if name in FIXED:
        return [FIXED[name]]
    _, d = s3.get(name); p = Pak(d); ms = set()
    for v in ps3geom.prototypes(p).values():
        ms |= set(A.ps3_block_materials(p, v['proto']))
    keys = {m.rsplit('/', 1)[0] + ':' + m.rsplit('/', 1)[1] for m in ms}
    need = {role(m) for m in ms if 'shadowblocker' not in m} & set(SPECIAL)
    f = fg(old); c = []
    for b, r in R.items():
        if (r['skel'], r['hier'], r['jh']) != f[:2] + f[3:4]:
            continue
        names = [x[0] for x in r['mats']]
        have = {x for x in need if any(re.search(SPECIAL[x], n.lower()) for n in names)}
        sb = any('shadowblocker' in n for n in names)
        c.append((len(have), len(keys & set(names)), sb, -len(names), b))
    return [x[-1] for x in sorted(c, reverse=True)[:6]]


res = {}
for name in list(FIXED) + AUTO:
    oldf, lay = find('actor33', name); old = Pak(oldf); fo = fg(old); mo, so, no = mats_used(old); eo = errs(oldf); dst = f'out/{name}.pak'
    _, d = s3.get(name); p = Pak(d); ps3 = set()
    for v in ps3geom.prototypes(p).values():
        ps3 |= set(A.ps3_block_materials(p, v['proto']))
    why = []; ok = False
    for t in candidates(name, old):
        for cap in (256, 128, 64):
            r = subprocess.run([sys.executable, '-B', 'build.py', '--one', name, t, str(cap)], capture_output=True, text=True, timeout=600)
            if r.returncode:
                why.append((t, cap, 'build failed', r.stderr.strip()[-160:])); break
            if Pak(dst).u32(0x20) <= BUDGET:
                break
        if r.returncode:
            continue
        q = Pak(dst); mn, sn, nn = mats_used(q); en = errs(dst); vn, _ = ps4verify.verify(dst, name=name, quiet=True); cn, _ = crashcheck.check(dst)
        bad = []
        if len(en) > len(eo): bad.append('ps4check %d>%d' % (len(en), len(eo)))
        if cn: bad.append('crash ' + cn[0][:80])
        if vn: bad.append('verify ' + vn[0][:80])
        if fg(q) != fo: bad.append('joints/fg differ')
        if (sn, nn) != (so, no): bad.append('geometry differs')
        if len(mn) != len(ps3): bad.append('materials %d!=%d' % (len(mn), len(ps3)))
        if q.u32(0x20) > BUDGET: bad.append('vram over')
        if bad:
            why.append((t, cap, bad)); continue
        ok = True
        res[name] = dict(template=t, cap=cap, before=len(mo), after=len(mn), vram=(old.u32(0x20), q.u32(0x20)), check=(len(eo), len(en)), layer=lay, tried=why)
        print('OK ', name, t, 'cap', cap, len(mo), '->', len(mn), 'vram %#x->%#x' % (old.u32(0x20), q.u32(0x20)), 'check', len(eo), len(en), flush=True)
        break
    if not ok:
        res[name] = dict(failed=why); print('SKIP', name, why, flush=True)
        for x in (dst, f'out/{name}.json'):
            if os.path.exists(x): os.remove(x)
json.dump(res, open('build2.json', 'w'), indent=0, default=str)
