#!/usr/bin/env python3
"""Plan the PS4 resources a converted stage reuses, and the device extraction requests.

For every PS3 material of the stage:
  * exact: a PS4 level pak has a material with the same name (PS3 'dir/file/variant' ->
    PS4 'dir/file:variant') -> its record, EFFECT and textures are copied from that pak;
  * fallback: no PS4 material of that name -> a PS4 material with a similar texture-role
    signature (same file other variant first, then same level directory) is used as the
    template and re-bound to the PS3 material's textures.
Every PS3 texture that some PS4 pak carries is requested too (its PS4 VRAM_DESC + low mips).

usage: mkplan.py <ps3 stage pak> [--exclude PAK ...] -o plan.json
Inputs: ps4mats.jsonl.gz (material survey) and ps4tex.tsv (texture index) in --res DIR.
"""
import argparse, gzip, json, os, re, sys, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from ndpak import Pak
from bggeom import collection

RES = '/mnt/user-data/uploads/Uncharted-windows/_claude_transfer/stages'
ROLE_RE = [('normal', r'-normals?\b|-normal-|normal\.'), ('detail', r'-detail'), ('spec', r'-spec'),
           ('mask', r'-mask'), ('blend', r'-blend'), ('trans', r'-trans|-alpha|-opacity'),
           ('env', r'refle|env'), ('color', r'-color|-diffuse|-col\b')]
PARAM_ROLE = [('Normal', 'normal'), ('Detail', 'detail'), ('Mask', 'mask'), ('Blend', 'blend'),
              ('Transparency', 'trans'), ('Environment', 'env'), ('Color', 'color'), ('Spec', 'spec'),
              ('LightMap', 'lightmap')]


def ps4key(ps3name):
    d, v = ps3name.rsplit('/', 1)
    return d + ':' + v


def tex_role(short):
    s = short.lower()
    if 'lightmap' in s or 'missingtexture' in s or '.atgi/' in s or 'indirect.rgbmax' in s:
        return None
    for role, rx in ROLE_RE:
        if re.search(rx, s):
            return role
    return 'color'


def param_role(p):
    for k, r in PARAM_ROLE:
        if k in p:
            return r
    return None


def short_of(full):
    """'.../texture1|2/<short>/<HASH>.ndb' -> short path"""
    m = re.search(r'texture[12]/(.*?)/[0-9A-F]{16}(\.ndb)?$', full)
    return m.group(1) if m else full


def read_materials(p):
    """PS3 MATERIAL_TABLE_1 -> ([{name, textures:[idx]}], [{name, hash}]) (page-aware)"""
    mt = p.items_of('MATERIAL_TABLE_1')
    if not mt:
        return [], []
    mt = mt[0]
    pg, b = mt.page, mt.data_off
    ntex, nmat = p.pu32(pg, b + 4), p.pu32(pg, b + 0xc)
    th = p.resolve(pg, b + 0x18)
    nm = p.resolve(pg, b + 0x30)
    ma = p.resolve(pg, b + 0x20)
    texs = []
    for i in range(ntex):
        r = p.resolve(nm[0], nm[1] + 4 * i)
        texs.append(dict(name=p.pstr(*r) if r else '', hash=p.pu32(th[0], th[1] + 4 * i)))
    out = []
    for i in range(nmat):
        m = p.resolve(ma[0], ma[1] + 4 * i)
        name = p.pstr(*p.resolve(*m))
        idx = []
        try:
            r = p.resolve(m[0], m[1] + 0x10)
            node = p.resolve(*r) if r else None
            bb = p.resolve(node[0], node[1] + 0x10) if node else None
            if bb:
                n = p.pu32(*bb)
                n = p.pu32(bb[0], bb[1] + 4)
                for k in range(min(n, 32)):
                    rr = p.resolve(bb[0], bb[1] + 0x50 + 0x10 * k)
                    if rr and rr[0] == th[0] and th[1] <= rr[1] < th[1] + 4 * ntex:
                        idx.append((rr[1] - th[1]) // 4)
        except Exception:
            pass
        # the pass/sampler tree under +0 also references textures the first node misses (e.g.
        # the '-trans' alpha maps of foliage): bounded walk, appended in walk order
        seen = set()

        def walk(a, depth):
            if depth > 4 or a in seen:
                return
            seen.add(a)
            for k in range(0, 0x80, 4):
                try:
                    r = p.resolve(a[0], a[1] + k)
                except Exception:
                    return
                if not r:
                    continue
                if r[0] == th[0] and th[1] <= r[1] < th[1] + 4 * ntex:
                    j = (r[1] - th[1]) // 4
                    if j not in idx and j not in extra:
                        extra.append(j)
                else:
                    walk(r, depth + 1)
        extra = []
        try:
            # material {name*, 0, 0, 8, passes*[8], ...}: passes beyond the first three can point
            # at structures shared with other materials, so only passes 0..2 are followed
            pl_ = p.resolve(m[0], m[1] + 0x10)
            if pl_:
                seen.add(m)
                for k in range(3):
                    r = p.resolve(pl_[0], pl_[1] + 4 * k)
                    if r:
                        walk(r, 2)
        except Exception:
            pass
        out.append(dict(name=name, textures=idx + extra))
    return out, texs


FOLIAGE = re.compile(r'plant|leaf|leaves|vine|card(?!board)|grass|moss|brom|bush|fern|ivy|palm-frond|frond|shrub|weed|reed|'
                     r'flower|hedge|branch|twig|canopy|willow|banyan')


def ps3_alpha_textures(p):
    """hashes of PS3 DXT1 textures whose top mip has > 5% punch-through (transparent) blocks"""
    import numpy as np
    import texcodec as tc
    out = set()
    for t in p.textures():
        f = tc.ps3_format(t)
        if not f or not f[2]:
            continue
        bw, bh = (t['width'] + 3) // 4, (t['height'] + 3) // 4
        d = p.vram_bytes(t['vramOffset'], bw * bh * f[1])
        if f[0] == 'DXT1':
            a = np.frombuffer(d, '<u2').reshape(-1, 4)
            idx = np.frombuffer(d, '<u4').reshape(-1, 2)[:, 1]
            pt = a[:, 0] <= a[:, 1]
            has3 = np.zeros(len(idx), bool)
            for k in range(16):
                has3 |= ((idx >> (2 * k)) & 3) == 3
            if (pt & has3).mean() > 0.05:
                out.add(t['hash'])
        # DXT3/DXT5 alpha is mostly a blend/spec mask in these levels, not a cutout
    return out


def material_class(name, m, texs3, alpha_tex):
    """'cut' (alpha tested), 'blend' (alpha blended), 'leaf' (foliage) for a PS3 material, from
    its name and whether its colour texture carries alpha"""
    c = set()
    low = name.lower()
    col_alpha = False
    if m:
        for i in m['textures']:
            t = texs3[i]
            r = tex_role(short_of(t['name']))
            if r == 'trans' or (r == 'color' and t['hash'] in alpha_tex):
                col_alpha = True
    if FOLIAGE.search(low):
        c.add('leaf')
    if re.search(r'glass|window|-trans\b', low):
        c.add('blend')
    elif col_alpha:
        c.add('cut')
    return c


def load_survey(res):
    S = collections.defaultdict(list)       # key -> [(pak, size, rec)]
    for l in gzip.open(os.path.join(res, 'ps4mats.jsonl.gz'), 'rt'):
        d = json.loads(l)
        for m in d.get('mats', []):
            S[m['n'].split(',')[0]].append((d['pak'], d.get('size', 0), m))
    return S


def load_texindex(res):
    T = collections.defaultdict(list)       # short -> [(pak, fullpath, fields)]
    for l in open(os.path.join(res, 'ps4tex.tsv')):
        f = l.rstrip('\n').split('\t')
        if len(f) < 11:
            continue
        T[short_of(f[1])].append((f[0], f[1], f))
    return T


def plan(ps3path, exclude=(), res=RES, verbose=True):
    p = Pak(ps3path)
    stage = os.path.basename(ps3path)[:-4]
    exclude = set(exclude) | {stage + '.pak'}
    S = load_survey(res)
    T = load_texindex(res)
    paksize = {}
    for v in S.values():
        for pk, sz, _ in v:
            paksize[pk] = sz
    C = collection(p)
    names = []
    for c in C:
        for g in c['groups']:
            if g[0] not in names:
                names.append(g[0])
    mats3, texs3 = read_materials(p)
    m3 = {m['name']: m for m in mats3}

    def roles3(name):
        m = m3.get(name)
        if not m:
            return set()
        return {tex_role(texs3[i]['name']) for i in m['textures'] if i is not None} - {None}

    used = collections.Counter()
    out = {}
    # exact first (greedy: prefer paks that cover many materials)
    cand = {}
    for n in names:
        k = ps4key(n)
        cand[n] = [(pk, sz, m) for pk, sz, m in S.get(k, []) if pk not in exclude and m['s'] == 'prime-shader.fx'] or \
                  [(pk, sz, m) for pk, sz, m in S.get(k, []) if pk not in exclude]
    cover = collections.Counter()
    for n, cs in cand.items():
        for pk in {c[0] for c in cs}:
            cover[pk] += 1
    for n in names:
        cs = cand[n]
        if cs:
            pk, sz, m = max(cs, key=lambda c: (cover[c[0]], -c[1]))
            out[n] = dict(key=ps4key(n), pak=pk, exact=True, name=m['n'], binds=m['b'])
            used[pk] += 1
    # fallbacks
    alpha_tex = ps3_alpha_textures(p)
    for n in names:
        if n in out:
            continue
        r3 = roles3(n)
        c3 = material_class(n, m3.get(n), texs3, alpha_tex)
        fdir = n.rsplit('/', 1)[0]
        top = '/'.join(n.split('/')[:2])
        ncomp = n.split('/')[:-1]
        best = None
        for k, lst in S.items():
            for pk, sz, m in lst:
                if pk in exclude or m['s'] != 'prime-shader.fx':
                    continue
                if 'shadowblocker' in k and 'shadowblocker' not in n:
                    continue
                r4 = {param_role(b[0]) for b in m['b']} - {None, 'lightmap', 'env'}
                score = len(r3 & r4) * 2 - len(r4 - r3) - len(r3 - r4)
                f4 = set(m.get('f', []))
                c4 = {'cut'} if 'TRANSPARENCY_CUTOFF' in f4 else set()
                if 'TRANSPARENCY_ALPHA_BLEND' in f4:
                    c4.add('blend')
                if 'GLOBAL_LEAF' in f4:
                    c4.add('leaf')
                # render class (alpha test / alpha blend / foliage) matters more than the path
                score += 12 if ('cut' in c3) == ('cut' in c4) else -12
                score += 8 if ('blend' in c3) == ('blend' in c4) else -8
                score += 4 if ('leaf' in c3) == ('leaf' in c4) else -2
                if 'cut' in c3 and 'trans' not in r3 and 'TRANSPARENCY_MAP_USE_SHARED_TEXTURE_1_A' in f4:
                    score += 6            # alpha taken from the colour map, like the PS3 texture
                kc = k.split(':')[0].split('/')
                common = 0
                for a_, b_ in zip(ncomp, kc):
                    if a_ != b_:
                        break
                    common += 1
                if k.split(':')[0] == fdir:
                    score += 20
                elif k.startswith(top):
                    score += 3 + 2 * max(0, common - 2)
                if ('trans' in r3) != ('trans' in r4):
                    score -= 5
                score += 0.5 * (used[pk] > 0)
                key = (score, -sz)
                if best is None or key > best[0]:
                    best = (key, k, pk, m)
        if best is None:
            raise SystemExit('no fallback material for %s' % n)
        _, k, pk, m = best
        out[n] = dict(key=k, pak=pk, exact=False, name=m['n'], binds=m['b'], roles=sorted(r3), cls=sorted(c3))
        used[pk] += 1
    # textures: every PS3 texture and every texture bound by a chosen PS4 material
    want = {}
    shorts = [short_of(t['path']) for t in p.textures()]
    for e in out.values():
        shorts += [b[1] for b in e['binds']]
    for s in shorts:
        if s in want or s not in T:
            continue
        lst = T[s]
        pk, full, f = max(lst, key=lambda x: (used[x[0]] > 0, -paksize.get(x[0], 1 << 40)))
        want[s] = (pk, full)
    req = dict(mats=sorted({(e['pak'], e['key']) for e in out.values()}),
               tex=sorted(set(want.values())))
    if verbose:
        ne = sum(e['exact'] for e in out.values())
        print('%s: %d materials, %d exact, %d fallback; %d source paks; %d textures requested'
              % (stage, len(out), ne, len(out) - ne, len({e['pak'] for e in out.values()}), len(req['tex'])))
        for n, e in out.items():
            if not e['exact']:
                print('  fallback %s -> %s (%s) roles %s class %s' % (n, e['key'], e['pak'], e['roles'], e.get('cls')))
    return dict(stage=stage, mats=out, requests=req)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ps3', nargs='+')
    ap.add_argument('--exclude', nargs='*', default=[])
    ap.add_argument('--res', default=RES)
    ap.add_argument('-o', '--out', required=True, help='plan.json (one stage) or directory')
    a = ap.parse_args()
    for f in a.ps3:
        pl = plan(f, a.exclude, a.res)
        o = a.out if not os.path.isdir(a.out) else os.path.join(a.out, pl['stage'] + '.plan.json')
        json.dump(pl, open(o, 'w'), indent=1)


if __name__ == '__main__':
    main()
