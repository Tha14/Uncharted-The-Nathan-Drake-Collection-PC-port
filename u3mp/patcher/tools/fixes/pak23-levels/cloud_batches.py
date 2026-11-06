#!/usr/bin/env python3
"""Replay the cloud in-place level-pak batches of 2026-10-30 on overlay-r3, in the order they were installed:
  b102   batch102-flatslots (fixer W)     PS3 -height/-blend/-inc maps appended, flat-mask bindings repointed
  h017c  h017c-bv-custom-fix (fixer H)    degenerate collision triangles (a,b,b,b) -> (b,a,b,b)  (Borneo)
  b103   batch103-blendpolarity (fixer W) PS3-inverted blend maps: block-inverted copy appended, BlendMap repointed
  h017co h017co (fixer H)                 the same collision rule on the other maps
  h028   h028-spec-masks (fixer H)        PS3 spec masks appended / spec constants scaled
The batch scripts are the originals (cloud-batches/<batch>/, our own code); each one is md5-locked to the states it was
written for, so a pak is only patched when the earlier steps reproduced that state exactly.  The appended PS3 textures
(data/*.bin, which the batches shipped) are regenerated here from the user's PS3 level paks: data/binsrc.json names the
PS3 texture, the top mips dropped (the 256 cap of h028) and the block size; the PS4 bytes are texcodec.ps3_to_ps4_texels
(BC1 1:1 re-tiling) and are md5-checked against the batch's own record before use.

usage: cloud_batches.py [--apply] [--only REGEX]
  default: dry run (each batch in --check mode on a scratch copy of the matching paks); --apply patches
  {{PROJECT}}/work/overlay-r3 (backups in {{PROJECT}}/work/backup-cloud-batches/<batch>/).
Idempotent: a pak already at a batch's md5_after is left alone."""
import argparse, hashlib, json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import levels as L                                   # paktools(), ps3_index(), ps3_source(), plan_rows()

ORDER = ['b102', 'h017c', 'b103', 'h017co', 'h028']
CB = os.path.join(HERE, 'cloud-batches')
PAKDIR = os.path.join(L.WORK, 'overlay-r3', 'u3data', 'build', 'main', 'pak23', 'ps4')


def md5f(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def swap_rows(b):
    """(pak file, md5_before, md5_after) of a batch's SWAP.tsv (or of its patches/*.json variants)."""
    d = os.path.join(CB, b); out = []
    if os.path.isdir(os.path.join(d, 'patches')):
        for f in sorted(os.listdir(os.path.join(d, 'patches'))):
            P = json.load(open(os.path.join(d, 'patches', f)))
            for v in P['variants']:
                out.append((P['pak'], v['md5_before'], v['md5_after']))
        return out
    for l in open(os.path.join(d, 'SWAP.tsv'), encoding='utf-8'):
        x = l.rstrip('\n').split('\t')
        if x[0] == 'pak' or len(x) < 4:
            continue
        out.append((x[0], x[2], x[3]))
    return out


def regen_bins(b, idx):
    """data/*.bin of batch b from the PS3 level paks (binsrc.json)."""
    f = os.path.join(CB, b, 'data', 'binsrc.json')
    if not os.path.exists(f):
        return 0
    pt = L.paktools('f525859')
    sys.path[:0] = [pt]
    from ndpak import Pak
    import texcodec as tc
    S = json.load(open(f)); rows = {r['name']: r for r in L.plan_rows() if r['overlay'] == 'overlay-r3'}
    cache = {}; n = 0
    for fn, s in sorted(S.items()):
        dst = os.path.join(CB, b, 'data', fn)
        if os.path.exists(dst) and hashlib.md5(open(dst, 'rb').read()).hexdigest() == s['md5']:
            continue
        lv = s['level']
        if lv not in cache:
            p = Pak(L.ps3_source(rows[lv], idx)); cache[lv] = (p, {t['path']: t for t in p.textures()})
        p, T = cache[lv]
        t = T.get(s['ps3'])
        if t is None:
            raise RuntimeError('%s: PS3 texture %s not in %s' % (b, s['ps3'], lv))
        lin = p.d[p.vram_base + t['vramOffset']:p.vram_base + t['vramOffset'] + t['vramSize']]
        off = tc.ps3_linear_size(t['width'], t['height'], s['skip'], s['bpb'])
        data = tc.ps3_to_ps4_texels(lin[off:], s['w'], s['h'], s['mips'], s['bpb'])
        if hashlib.md5(data).hexdigest() != s['md5']:
            raise RuntimeError('%s: %s from the PS3 texture differs from the batch record' % (b, fn))
        open(dst, 'wb').write(data); n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--only')
    a = ap.parse_args()
    idx = L.ps3_index()
    stage = os.path.join(L.TMP, 'cloud-batches-dry')
    if not a.apply:                                   # dry run: scratch copies of the paks the batches lock on
        shutil.rmtree(stage, ignore_errors=True)
        sd = os.path.join(stage, 'overlay-r3', 'u3data', 'build', 'main', 'pak23', 'ps4'); os.makedirs(sd)
        names = {r[0] for b in ORDER for r in swap_rows(b)}
        for nm in names:
            if os.path.exists(os.path.join(PAKDIR, nm)) and (not a.only or re.search(a.only, nm)):
                shutil.copyfile(os.path.join(PAKDIR, nm), os.path.join(sd, nm))
        work, pakdir = stage, sd
    else:
        work, pakdir = L.WORK, PAKDIR
    total = 0
    for b in ORDER:
        rows = [r for r in swap_rows(b) if not a.only or re.search(a.only, r[0])]
        before = {r[1] for r in rows}; after = {r[2] for r in rows}
        todo = [r[0] for r in rows if os.path.exists(os.path.join(pakdir, r[0])) and md5f(os.path.join(pakdir, r[0])) in before]
        done = len({r[0] for r in rows if os.path.exists(os.path.join(pakdir, r[0])) and md5f(os.path.join(pakdir, r[0])) in after})
        print('%s: %d paks in the batch, %d at its input state, %d already patched' % (b, len({r[0] for r in rows}), len(set(todo)), done))
        if not todo:
            continue
        print('  regenerated %d PS3 textures' % regen_bins(b, idx))
        d = os.path.join(CB, b)
        bak = os.path.join(L.WORK if a.apply else stage, 'backup-cloud-batches', b)
        script = [f for f in os.listdir(d) if f.endswith('.py') and (f.startswith('append') or f.startswith('apply'))][0]
        arg = pakdir if script == 'apply_patches.py' else work
        p = subprocess.run([sys.executable, os.path.join(d, script), arg, bak], capture_output=True, text=True, errors='replace')
        for l in p.stdout.splitlines():
            if l.startswith(('patched', 'MD5', 'ok ', 'NOTHING')):
                print('  ' + l)
        if p.returncode:
            print(p.stderr[-1500:]); return 1
        total += sum(1 for l in p.stdout.splitlines() if l.startswith('patched'))
    print('cloud batches: %d pak patches %s' % (total, 'applied' if a.apply else '(dry run on scratch copies)'))
    if not a.apply:
        shutil.rmtree(stage, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
