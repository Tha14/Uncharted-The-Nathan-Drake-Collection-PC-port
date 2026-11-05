#!/usr/bin/env python3
"""Rebuild collapsed MP/co-op actor paks from PS3 data with retail templates.
  build.py            build every job, write out/*.pak and results.json
  build.py --one N    build one job in this process (used for the per-job timeout)
Nothing outside this folder is written. ps4actor_am.py is generated from the paktools copy by mkconv.py."""
import sys, os, re, json, subprocess, time, shutil
HERE = os.path.dirname(os.path.abspath(__file__)); os.chdir(HERE); sys.path.insert(0, HERE)
from lib import *
JOBS = [  # name, retail template
    ('mp-hero-french-torso-coop', 'mp-hero-french-torso'),
    ('mp-coop-cutter', 'cutter'),
    ('mp-hero-ngp-torso', 'mp-hero-french-torso'),
    ('mp-hero-sol-med-b-1a', 'mp-hero-french-torso'),
    ('mp-hero-ramses-equip-torso', 'mp-hero-french-torso'),
    ('mp-hero-subway-torso', 'mp-hero-french-torso'),
    ('mp-hero-fat-talbot-torso', 'mp-hero-fat-torso'),
    ('mp-hero-cutter-torso-w-scarf', 'mp-hero-cutter-torso'),
    ('mp-hero-agent-b-torso', 'mp-hero-agent-a-torso'),
    ('mp-hero-make-believe-torso', 'mp-hero-french-torso'),
    ('mp-hero-ramses-equip-legs', 'mp-hero-fat-ramses-legs'),
    ('mp-hero-guerro-torso', 'mp-hero-french-torso'),
    ('mp-hero-thug-e-torso', 'mp-hero-french-torso'),
    ('mp-hero-flynn-hair', 'mp-hero-flynn-torso'),
]
BUDGET = 0x80000
SPECIAL = {'eyelashes': r'eyelash', 'brows': r'brows', 'eyeoverlay': r'eyeoverlay',
           'eyes': r'(^|[/:-])eyes?([/:-]|$)', 'mouth': r'mouth|teeth|tongue', 'hair': r'hair'}
SKIN = {'head': r'head|neck|face', 'arms': r'arms|hands?|skin'}


def role(n):
    low = n.lower()
    if 'goggles' in low:
        return 'cloth'
    for r, x in SPECIAL.items():
        if re.search(x, low):
            return r
    for r, x in SKIN.items():
        if re.search(x, low.split('characters/')[-1]):
            return r
    return 'cloth'


def pool(tmats, r):
    main = [m for m in tmats if 'shadowblocker' not in m['name']]
    nm = lambda m: m['name'].split(',')[0]
    if r in SPECIAL:
        return [m for m in main if re.search(SPECIAL[r], nm(m).lower())]
    if r in SKIN:
        p = [m for m in main if role(nm(m)) == r]
        if p:
            return p
    return [m for m in main if role(nm(m)) == 'cloth']


def kinds_of_binds(A, m):
    s = set()
    for pname, _, _ in m['binds']:
        for suf, ps in A.TEX_KINDS:
            if pname in ps:
                s.add(suf)
    return s


def build_one(name, tname, cap):
    import ps4actor_am as A
    import ps3geom
    from ps3mat_am import read_materials
    sys.path.insert(0, T + '/stage')
    import cubefix, cb3fix
    A.MAX_TEX = cap
    s = S3(); src, d = s.get(name)
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
        best = max(c, key=lambda m: len(ks & kinds_of_binds(A, m)) - len(kinds_of_binds(A, m) - ks))
        choice[mn] = best['name']
        report.append((mn, r + warn, best['name'].split(',')[0], len(real.get(mn, []))))
    dst = f'out/{name}.pak'
    A.convert(p3, tpl, dst, mat_choice=choice, verbose=False)
    # repair: ps4writer aligns the last page end to 16, the loader expects VRAM right after it (ps4verify)
    q_ = Pak(dst); end = q_.pages[-1][0] + q_.pages[-1][1]
    if end != q_.vram_base:
        raw = open(dst, 'rb').read(); open(dst, 'wb').write(raw[:end] + raw[q_.vram_base:])
    cf = cubefix.fix(dst, p3)
    n, raised = cb3fix.fix(dst, dst)
    json.dump(dict(src=src, template=tname, cap=cap, roles=report, cubes=list(cf or []), cb3=raised, vram=Pak(dst).u32(0x20)),
              open(f'out/{name}.json', 'w'), indent=0)
    print('built', name, 'cap', cap, 'vram', hex(Pak(dst).u32(0x20)))


def main():
    only = sys.argv[1:]
    for name, t in JOBS:
        if only and name not in only:
            continue
        ok = False
        for cap in (256, 128, 64):
            t0 = time.time()
            try:
                r = subprocess.run([sys.executable, '-B', 'build.py', '--one', name, t, str(cap)], capture_output=True, text=True, timeout=600)
            except subprocess.TimeoutExpired:
                print('TIMEOUT', name, cap, flush=True); break
            print(name, cap, 'rc', r.returncode, int(time.time() - t0), 's', (r.stdout + r.stderr).strip()[-400:], flush=True)
            if r.returncode:
                break
            if Pak(f'out/{name}.pak').u32(0x20) <= BUDGET:
                ok = True; break
        print('RESULT', name, 'ok' if ok else 'FAILED-OR-OVER', flush=True)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--one':
        build_one(sys.argv[2], sys.argv[3], int(sys.argv[4]))
    else:
        main()
