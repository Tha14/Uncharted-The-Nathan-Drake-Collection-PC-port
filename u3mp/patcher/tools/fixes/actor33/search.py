"""Recover the conversion parameters of every converted actor pak by reconverting it and
comparing sha256: tool version x PS3 source x template x mode x texture cap x env.

usage: search.py <test root> <targets.tsv> <out found.tsv> [--max N] [--only name,...] [--overlays o1,o2]
targets.tsv: overlay, name, sha256 (from mktargets in this folder: inv.tsv + as1 + backups)
<test root>/t/<toolset>/u3mp/paktools  (toolsets: git archive of the repo commits + port-v1)
<test root>/ps3/<source>/<name>        (extract.py)
<test root>/ident.tsv                   (ident.py: template candidates per pak, keyed by sha)
Writes one line per target: overlay name sha status toolset source template mode cap env post tries
"""
import argparse, hashlib, json, os, re, subprocess, sys, time

PS4 = "D:/2. PS Games/PS4/Uncharted - The Nathan Drake Collection [CUSA02320]/u3data/build/main/actor33/ps4"
PY = sys.executable
HERE = os.path.dirname(os.path.abspath(__file__))
SRC_ORDER = ['dlc-patch18', 'dlc-patch13', 'goty-patch', 'goty-dlc', 'disc', 'data2-parts']


def mp_template(n, ps4):
    """mpbatch.py template rule (verbatim logic)."""
    b = n[:-4]
    m = re.match(r'mp-kit-base-(.*)-skin-[bc](-00)?$', b)
    if m:
        part = m.group(1)
        for c in ('mp-kit-base-%s-skin-a-00' % part, 'mp-kit-base-%s-skin-a' % part):
            if c in ps4: return c
        if part.startswith('torso'): return 'mp-kit-base-neck-skin-a'
    if b.startswith('mp-kit-base-torso'): return 'mp-kit-base-neck-skin-a'
    if b.startswith('mp-kit-'):
        if '-torso-' in b or 'killzone-full-a' == b[7:] or b.startswith('mp-kit-misc') or b.startswith('mp-kit-shirt'):
            return 'mp-kit-merc-torso-shirt-w' if '-merc-' in b else 'mp-kit-hunter-torso-shirt-h'
        if '-legs-' in b: return 'mp-kit-hunter-legs-pants-j'
        if '-feet-' in b: return 'mp-kit-hunter-feet-boots-a'
        if '-arms-' in b: return 'mp-kit-base-arms-hands-skin-a-00'
        return 'mp-kit-hunter-head-hat-d'
    if b.startswith('mp-chloe-'): return 'mp-chloe-elena-hair'
    if b.startswith('mp-hero-'):
        if b.endswith('-torso') or b == 'mp-hero-sol-med-b-1a': return 'mp-hero-london-torso'
        if b.endswith('-legs'): return 'mp-hero-london-legs'
        if b.endswith('-hair'): return 'mp-hero-sullivan-hair'
        return 'mp-hero-ramses-bandana'
    return None


class Worker:
    def __init__(self, tools):
        self.p = subprocess.Popen([PY, os.path.join(HERE, 'worker.py'), tools], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)

    def run(self, job):
        try:
            self.p.stdin.write(json.dumps(job) + '\n')
            self.p.stdin.flush()
            return json.loads(self.p.stdout.readline())
        except Exception as ex:          # the worker died (converter crash): start a new one
            try:
                self.p.kill()
            except Exception:
                pass
            self.p = subprocess.Popen(self.p.args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
            return dict(err='worker died: %r' % (ex,))


VARIANTS = [  # (mode label, convert kwargs, env, post)
    ('mp', dict(stream_flags=2), {}, []),
    ('kit', dict(stream_flags=2, mat_by_file=True, match_streams=True), {}, []),
    ('kit-post', dict(stream_flags=2, mat_by_file=True, match_streams=True), {}, ['cubefix', 'cb3fix']),
    ('kit-spread', dict(stream_flags=2, mat_by_file=True, match_streams=True), {'PS4ACTOR_SPREAD_MATS': '1'}, []),
    ('kit-notmpl', dict(stream_flags=2, mat_by_file=True, match_streams=True), {'PS4ACTOR_NO_TMPL_TEX': '1'}, []),
    ('kit-nomatch', dict(stream_flags=2, mat_by_file=True), {}, []),
    ('graft', dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {}, []),
    ('graft-post', dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {}, ['cubefix', 'cb3fix']),
    ('own', dict(stream_flags=3, mat_by_file=True, match_streams=True, own_skel=True), {}, []),
    ('mp-sf3', dict(stream_flags=3), {}, []),
    ('kit-spread-notmpl', dict(stream_flags=2, mat_by_file=True, match_streams=True), {'PS4ACTOR_SPREAD_MATS': '1', 'PS4ACTOR_NO_TMPL_TEX': '1'}, []),
    ('own-spread-notmpl', dict(stream_flags=3, mat_by_file=True, match_streams=True, own_skel=True), {'PS4ACTOR_SPREAD_MATS': '1', 'PS4ACTOR_NO_TMPL_TEX': '1'}, []),
    ('own-spread', dict(stream_flags=3, mat_by_file=True, match_streams=True, own_skel=True), {'PS4ACTOR_SPREAD_MATS': '1'}, []),
    ('own-notmpl', dict(stream_flags=3, mat_by_file=True, match_streams=True, own_skel=True), {'PS4ACTOR_NO_TMPL_TEX': '1'}, []),
    ('graft-spread', dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {'PS4ACTOR_SPREAD_MATS': '1'}, []),
    ('graft-notmpl', dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {'PS4ACTOR_NO_TMPL_TEX': '1'}, []),
    ('graft-spread-notmpl', dict(stream_flags=3, mat_by_file=True, match_streams=True, graft=True), {'PS4ACTOR_SPREAD_MATS': '1', 'PS4ACTOR_NO_TMPL_TEX': '1'}, []),
]
CAPS = [int(c) for c in os.environ.get('SEARCH_CAPS', '256,128,64').split(',')]
LIGHT = {  # lightbatch.py JOBS
    'propane-tank-mp': ('rename', 'propane-tank'), 'mp-hero-skel-bones-glow': ('rename', 'mp-hero-skel-bones'),
    'capture-idol-silver': ('graft', 'capture-idol-gold'), 'turret-gatling-tripod-mp': ('graft', 'turret-gatling-tripod'),
    'gold-treasure-aa': ('graft', 'gold-treasure-ac'), 'gold-treasure-ab': ('graft', 'gold-treasure-ac'),
    'gold-treasure-aj': ('graft', 'gold-treasure-ak'), 'ivory-treasure-aa': ('graft', 'ivory-treasure-ai'),
    'ivory-treasure-ab': ('graft', 'ivory-treasure-ai'), 'ivory-treasure-ad': ('graft', 'ivory-treasure-ai'),
    'silver-treasure-ab': ('graft', 'silver-treasure-av'), 'silver-treasure-ad': ('graft', 'silver-treasure-ax-1'),
    'silver-treasure-aj': ('graft', 'silver-treasure-ax-1'), 'mp-chloe-skel': ('kit', 'mp-hero-skel-bones'),
}


def _pak(tools):
    sys.path.insert(0, tools)
    from ndpak import Pak
    return Pak


def jh_key(q, it):
    """JOINT_HIERARCHY bytes with the skeleton id cleared and in-item pointers made item-relative"""
    import struct
    d = bytearray(q.d[q.pbase(it.page) + it.data_off: q.pbase(it.page) + it.data_off + it.size])
    d[0x30:0x34] = bytes(4)
    for o in range(0, len(d) - 7, 8):
        v = struct.unpack_from('<Q', d, o)[0]
        if it.data_off - 0x40 <= v <= it.data_off + it.size + 0x40:
            struct.pack_into('<Q', d, o, v - it.data_off + 0x1000000)
    return hashlib.sha1(bytes(d)).hexdigest()


def retail_jh_index(root):
    """sha1 of the JOINT_HIERARCHY item of every retail actor pak (graft copies it verbatim)."""
    import pickle
    f = os.path.join(root, 'retail-jh3.pkl')
    if os.path.exists(f):
        return pickle.load(open(f, 'rb'))
    Pak = _pak(os.path.join(root, 't', '7d45c1f', 'u3mp', 'paktools'))
    out = {}
    for fn in os.listdir(PS4):
        try:
            q = Pak(os.path.join(PS4, fn))
            for it in q.items_of('JOINT_HIERARCHY'):
                out.setdefault(jh_key(q, it), []).append(fn[:-4])
        except Exception:
            pass
    pickle.dump(out, open(f, 'wb'))
    return out


def retail_tex_index(root):
    import pickle
    f = os.path.join(root, 'retail-tex.pkl')
    if os.path.exists(f):
        return pickle.load(open(f, 'rb'))
    Pak = _pak(os.path.join(root, 't', '7d45c1f', 'u3mp', 'paktools'))
    out = {}
    for fn in sorted(os.listdir(PS4)):
        if not fn.startswith(('mp-', 'hero')):
            continue
        try:
            for t in Pak(os.path.join(PS4, fn)).textures():
                out.setdefault(t['path'], []).append(fn[:-4])
        except Exception:
            pass
    pickle.dump(out, open(f, 'wb'))
    return out


def target_info(path, root, name, srcs, retail_jh):
    """what the target pak tells about its conversion: own skeleton, the template whose skeleton it carries,
    the texture cap, whether cubefix / cb3fix ran."""
    ti = dict(has_js=False, caps=[], cb3=False, cube=False, jh_tmpl=[])
    if not path:
        return ti
    try:
        Pak = _pak(os.path.join(root, 't', '7d45c1f', 'u3mp', 'paktools'))
        q = Pak(path)
        js = q.items_of('JOINT_HIERARCHY')
        ti['has_js'] = bool(js)
        for it in js:
            ti['jh_tmpl'] += retail_jh.get(jh_key(q, it), [])
        tt = {t['path'].split('/')[-2] if t['path'].endswith('.ndb') else t['path'].split('/')[-1]: t for t in q.textures()}
        for t in tt.values():
            if 'env-reflection' in t['path'] and t['vramSize'] >= 6 * (t['width'] * t['height'] // 2):
                ti['cube'] = True
        sys.path.insert(0, os.path.join(root, 't', '7d45c1f', 'u3mp', 'paktools', 'stage'))
        import cb3fix
        ti['cb3'] = any(c >= 8 for _o, c, _n, _nm, dm in cb3fix.prototypes(q) if not dm)
        caps = set()
        for src in srcs:
            p = Pak(os.path.join(root, 'ps3', src, name))
            for t in p.textures():
                sh = t['path'].split('/')[-2] if t['path'].endswith('.ndb') else t['path'].split('/')[-1]
                if sh in tt and t.get('type', 1) == 1:
                    w4 = max(tt[sh]['width'], tt[sh]['height']); w3 = max(t['width'], t['height'])
                    if w4 < w3:
                        caps.add(w4)
                    else:
                        caps.add(-w3)       # not capped: any cap >= w3
        # donor textures: in the target, not in any PS3 source -> retail mp-/hero paks that have them
        p3names = set()
        for src in srcs:
            for t in Pak(os.path.join(root, 'ps3', src, name)).textures():
                p3names.add(t['path'].split('/')[-2] if t['path'].endswith('.ndb') else t['path'].split('/')[-1])
        ti['donor_tex'] = [t['path'] for k, t in tt.items() if k not in p3names and 'generated/u3mp' not in t['path']]
        lo = sorted(c for c in caps if c > 0)
        ti['caps'] = lo[:1] if lo else ([256] if all(-c <= 256 for c in caps) else [0, 512])
    except Exception as ex:
        ti['err'] = repr(ex)
    return ti


def candidates(ov, order, srcs, tm, ti):
    tier1 = order[:3]
    tiers = [tier1, [o for o in order if o not in tier1]]
    if ti['has_js']:
        modes = [v for v in VARIANTS if v[0].startswith(('graft', 'own'))]
    else:
        modes = [v for v in VARIANTS if not v[0].startswith(('graft', 'own'))]
    modes = [v for v in modes if not v[3]]       # post passes are chosen below
    posts = [[], ['cubefix'], ['cubefix', 'cb3fix'], ['cb3fix']]
    if ti['cb3']:
        posts = [['cubefix', 'cb3fix'], ['cb3fix'], [], ['cubefix']]
    elif ti['cube']:
        posts = [['cubefix'], ['cubefix', 'cb3fix'], [], ['cb3fix']]
    caps = ti['caps'] + [c for c in (256, 128, 64, 0, 512) if c not in ti['caps']]
    if ti.get('donors'):
        mods = []
        for d in ti['donors']:
            for vm, kw, env, p_ in modes:
                e = dict(env); e['PS4ACTOR_DONORS'] = os.path.join(PS4, d + '.pak')
                mods.append((vm + '+donor:' + d, kw, e, p_))
        modes = mods + modes
    for tier in tiers:
        for capset in (caps[:1], caps[1:]):
            for vm, kw, env, _p in modes:
                for post in posts:
                    for src in srcs:
                        for t in tm:
                            for cap in capset:
                                for td in ('full', 'solo'):
                                    for ts in tier:
                                        if vm.startswith('own') and ts == '7d45c1f':
                                            ts = '003b433'      # own-skel needs collision.decode_mesh (003b433+)
                                        yield ts, vm, kw, env, post, src, t, cap, td


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('root')
    ap.add_argument('targets')
    ap.add_argument('out')
    ap.add_argument('--max', type=int, default=400)
    ap.add_argument('--only')
    ap.add_argument('--overlays')
    ap.add_argument('--toolsets', default='')
    a = ap.parse_args()
    ps4 = {f[:-4] for f in os.listdir(PS4)}
    ident = {}
    for l in open(os.path.join(a.root, 'ident.tsv')):
        f = l.rstrip('\n').split('\t')
        if len(f) >= 6:
            ident[(f[0], f[1])] = f[5].split()
    # ident keyed by path; build sha->cands
    sha_c = {}
    sha_p = {}
    for (n, path), c in ident.items():
        try:
            h = hashlib.sha256(open(path, 'rb').read()).hexdigest()
        except OSError:
            continue
        sha_c[h] = [x for x in c if not x.endswith('~j')] + [x[:-2] for x in c if x.endswith('~j')]
        sha_p[h] = path
    retail_jh = retail_jh_index(a.root)
    done = set()
    seen = {}
    if os.path.exists(a.out):
        for l in open(a.out):
            f = l.split('\t')
            if len(f) > 3 and f[3] in ('MATCH', 'NOMATCH'):
                done.add((f[0], f[1], f[2]))
                seen[(f[1], f[2])] = [x.rstrip(chr(10)) for x in f[3:]]
    tsets = {}
    fo = open(a.out, 'a')
    only = set(a.only.split(',')) if a.only else None
    ovs = set(a.overlays.split(',')) if a.overlays else None
    for l in open(a.targets):
        ov, name, sha = l.rstrip('\n').split('\t')[:3]
        if only and name not in only and name[:-4] not in only:
            continue
        if ovs and ov not in ovs:
            continue
        if (ov, name, sha) in done:
            continue
        if (name, sha) in seen:
            fo.write(chr(9).join([ov, name, sha] + seen[(name, sha)]) + chr(10)); fo.flush()
            continue
        t0 = time.time()
        srcs = [s for s in SRC_ORDER if os.path.exists(os.path.join(a.root, 'ps3', s, name))]
        tm = list(sha_c.get(sha, []))[:4]
        r = mp_template(name, ps4)
        if r and r not in tm:
            tm.append(r)
        if ov == 'overlay-port':
            order = ['port-v1', 'de13aa1', '0a20361', '7d45c1f']
        else:
            order = ['7d45c1f', '0a20361', 'de13aa1', 'port-v1']
        if a.toolsets:
            order = a.toolsets.split(',')
        os.makedirs(os.path.join(a.root, 'work-' + os.path.basename(a.out)), exist_ok=True)
        found = None
        tries = 0
        b = name[:-4]
        pre = []        # (label, job-extra) tried before the ps4actor variants
        if b.startswith('anim-') or b.startswith('multi-celebrate'):
            for src in srcs:
                for ts in ['1323bad', 'anim32', 'animng1']:
                    for st in ([], ['stm']):
                        pre.append((ts, src, '-', 'anim' + ('-stm' if st else ''), dict(anim=[os.path.join(a.root, s) for s in st])))
        if b in LIGHT and LIGHT[b][0] == 'rename':
            for ts in ['6db6b8f', '7d45c1f', 'cb545d9']:
                pre.append((ts, '-', LIGHT[b][1], 'rename', dict(rename=[LIGHT[b][1], b], tmpl=os.path.join(PS4, LIGHT[b][1] + '.pak'))))
        if b in LIGHT and LIGHT[b][1] not in tm:
            tm.insert(0, LIGHT[b][1])
        for ts, src, t, lab, extra in pre:
            if ts not in tsets:
                tsets[ts] = Worker(os.path.join(a.root, 't', ts, 'u3mp', 'paktools'))
            tries += 1
            job = dict(ps3=os.path.join(a.root, 'ps3', src, name), out=os.path.join(a.root, 'work-' + os.path.basename(a.out), name))
            job.update(extra)
            res = tsets[ts].run(job)
            if res.get('sha') == sha:
                found = (ts, src, t, lab, '-', '{}', '')
                break
        if not found:
            ti = target_info(sha_p.get(sha), a.root, name, srcs, retail_jh)
            if ti.get('jh_tmpl'):
                full = sha_c.get(sha, [])
                both = [x for x in full if x in ti['jh_tmpl']]
                if b in LIGHT and LIGHT[b][1] in ti['jh_tmpl']:
                    both.insert(0, LIGHT[b][1])
                rest = [x for x in ti['jh_tmpl'] if x not in both] if len(ti['jh_tmpl']) <= 6 else []
                tm = both + rest + [x for x in tm if x not in both and x not in rest]
            if os.environ.get('SEARCH_DONORS') == '1':
                rt = retail_tex_index(a.root)
                cand = []
                for tp_ in ti.get('donor_tex', []):
                    for d_ in rt.get(tp_, []):
                        if d_ not in tm and d_ not in cand:
                            cand.append(d_)
                ti['donors'] = cand[:3]
            gen = candidates(ov, order, srcs, tm, ti)
            seen_combo = set()
            for ts, vm, kw, env, post, src, t, cap, td in gen:
                if tries >= a.max:
                    break
                if ts not in tsets:
                    tsets[ts] = Worker(os.path.join(a.root, 't', ts, 'u3mp', 'paktools'))
                key = (ts, vm, src, t, cap, '+'.join(post), td)
                if key in seen_combo:
                    continue
                seen_combo.add(key)
                tries += 1
                out = os.path.join(a.root, 'work-' + os.path.basename(a.out), name)
                k = dict(kw); k['max_tex'] = cap
                tp = os.path.join(PS4, t + '.pak')
                if td == 'solo':        # template directory holding only the template (retail_names lookup)
                    sd = os.path.join(a.root, 'tsolo', t)
                    if not os.path.exists(os.path.join(sd, t + '.pak')):
                        os.makedirs(sd, exist_ok=True)
                        import shutil
                        shutil.copyfile(tp, os.path.join(sd, t + '.pak'))
                    tp = os.path.join(sd, t + '.pak')
                res = tsets[ts].run(dict(ps3=os.path.join(a.root, 'ps3', src, name), tmpl=tp, out=out,
                                         kw=k, env=env, post=post))
                if res.get('sha') == sha:
                    found = (ts, src, t, vm, cap, json.dumps(env), '+'.join(post) + ('|solo' if td == 'solo' else ''))
                    break
        line = (['MATCH'] + [str(x) for x in found] + [str(tries), '%.1f' % (time.time() - t0)]) if found else             ['NOMATCH', '', ','.join(srcs), ','.join(tm), '', '', '', '', str(tries), '%.1f' % (time.time() - t0)]
        seen[(name, sha)] = line
        if found:
            fo.write('\t'.join([ov, name, sha, 'MATCH'] + [str(x) for x in found] + [str(tries), '%.1f' % (time.time() - t0)]) + '\n')
        else:
            fo.write('\t'.join([ov, name, sha, 'NOMATCH', '', ','.join(srcs), ','.join(tm), '', '', '', '', str(tries), '%.1f' % (time.time() - t0)]) + '\n')
        fo.flush()
        print(ov, name, 'MATCH ' + ' '.join(map(str, found)) if found else 'NOMATCH', tries, flush=True)


if __name__ == '__main__':
    main()
