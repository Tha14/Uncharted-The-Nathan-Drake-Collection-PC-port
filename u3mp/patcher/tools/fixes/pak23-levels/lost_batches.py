#!/usr/bin/env python3
"""Replay the lost cloud in-place batches of 2026-10-29/30 on overlay-r3 (recovered 2026-11-06: lost-scripts.zip and
n015-batches.zip from the claude.ai workspace), between convert-levels-r3 and cloud-batches-r3:
  batch97/98 water layer, batch99-lmuv, batch100-blockers A+B+C, revertB, batch101-detail, n012-museum-catchup,
  batch101m-museum, n014-ldn-tunnel-probes, n015-level-info, n015b-level-info
(lost-batches/replay_lost.py, md5-locked step by step; see scratch/pak23-recovery/chain/REPORT.md).

Only a pak that convert-levels-r3 built at its recorded pre-batch state (lost-batches/t0-md5.tsv) is touched, and it
is replaced only when every recorded step reproduced its md5: anything else is left exactly as it was. The PS3 detail
textures batch101 / 101m appended (data/*.bin, game-derived) are regenerated from the user's PS3 level paks
(data/binsrc.json) and md5-checked first, as cloud_batches.py does.

usage: lost_batches.py [--apply] [--only REGEX] [--pakdir DIR]   (dry run by default; --pakdir: test on other files)"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import levels as L                                   # paktools(), ps3_index(), ps3_source(), plan_rows()
import cloud_batches as CBM                          # md5f()

LB = os.path.join(HERE, 'lost-batches')
LOST = os.path.join(LB, 'lost')
PAKDIR = os.path.join(L.WORK, 'overlay-r3', 'u3data', 'build', 'main', 'pak23', 'ps4')


def t0_list():
    """{pak name: (md5 of the recorded pre-batch state, md5 after the lost batches)}"""
    out = {}
    for l in open(os.path.join(LB, 't0-md5.tsv'), encoding='utf-8'):
        if l.startswith('#'):
            continue
        x = l.rstrip('\r\n').split('\t')
        if len(x) >= 3 and x[1] and x[2]:
            out[x[0]] = (x[1], x[2])
    return out


def regen_bins(b, idx):
    """lost/<batch>/data/*.bin from the PS3 level paks (binsrc.json; flat-white is a plain white BC1 chain)."""
    f = os.path.join(LOST, b, 'data', 'binsrc.json')
    if not os.path.exists(f):
        return 0
    pt = L.paktools('f525859')
    sys.path[:0] = [pt]
    from ndpak import Pak
    import texcodec as tc
    S = json.load(open(f)); rows = {r['name']: r for r in L.plan_rows() if r['overlay'] == 'overlay-r3'}
    cache = {}; n = 0
    for fn, s in sorted(S.items()):
        dst = os.path.join(LOST, b, 'data', fn)
        if os.path.exists(dst) and hashlib.md5(open(dst, 'rb').read()).hexdigest() == s['md5']:
            continue
        if s.get('level') is None:                   # flat white: white BC1 blocks, every mip padded to 8x8 blocks
            w, h, data = s['w'], s['h'], b''
            for _ in range(s['mips']):
                data += bytes.fromhex('ffffffff00000000') * max(8, w // 4) * max(8, h // 4)
                w, h = max(1, w // 2), max(1, h // 2)
        else:
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
            raise RuntimeError('%s: %s regenerated from the PS3 texture differs from the batch record' % (b, fn))
        open(dst, 'wb').write(data); n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--only')
    ap.add_argument('--pakdir', default=PAKDIR)
    a = ap.parse_args()
    t0 = t0_list()
    todo = []
    for name, (start, _end) in sorted(t0.items()):
        p = os.path.join(a.pakdir, name + '.pak')
        if (not a.only or re.search(a.only, name)) and os.path.exists(p) and CBM.md5f(p) == start:
            todo.append(name)
    print('lost batches: %d paks have a recorded pre-batch state, %d are at it' % (len(t0), len(todo)))
    if not todo:
        print('nothing to replay')
        return 0
    idx = L.ps3_index()
    for b in ('batch101-detail', 'batch101m-museum'):
        print('  %s: regenerated %d PS3 textures' % (b, regen_bins(b, idx)))
    env = dict(os.environ, PYTHONPATH=os.pathsep.join([L.paktools('f525859'), os.path.join(LOST, 'batch100-blockers', 'tools')]))
    work = os.path.join(L.TMP, 'lost-batches'); os.makedirs(work, exist_ok=True)
    bak = os.path.join(L.WORK, 'backup-lost-batches')
    done = bad = 0
    for name in todo:
        src = os.path.join(a.pakdir, name + '.pak'); out = os.path.join(work, name + '.pak')
        r = subprocess.run([sys.executable, os.path.join(LB, 'replay_lost.py'), name, src, out],
                           capture_output=True, text=True, errors='replace', env=env)
        last = (r.stdout.strip().splitlines() or [''])[-1]
        # the end state decides (a step that does not apply to this pak's chain is reported by the replay but is
        # not an error: e.g. the batch98 water rebuilds start after batch98)
        if not os.path.exists(out) or CBM.md5f(out) != t0[name][1]:
            bad += 1; print('  %s: not replayed, the result is not the recorded state (%s)' % (name, last[:160]))
            continue
        if a.apply:
            os.makedirs(bak, exist_ok=True)
            if not os.path.exists(os.path.join(bak, name + '.pak')):
                shutil.copy2(src, os.path.join(bak, name + '.pak'))
            shutil.copyfile(out, src + '.tmp'); os.replace(src + '.tmp', src)
        done += 1; print('  %s: %s' % (name, last[:120]))
        os.remove(out)
    print('lost batches: %d replayed%s, %d left as built' % (done, '' if a.apply else ' (dry run)', bad))
    return 0


if __name__ == '__main__':
    sys.exit(main())
