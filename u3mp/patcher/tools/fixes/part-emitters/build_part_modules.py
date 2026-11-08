#!/usr/bin/env python3
"""Recipe step convert-dc2-part-modules: rebuild the 3 overlay-r3 PS4 particle modules
  dc2/part-module/part-cave-multi.bin, part-gompa-multi.bin, part-shambhala-multi.bin
from the player's own PS3 Uncharted 3 (1.19 update + disc) and PS4 Collection files, with the recovered batch45/batch48
tools (the 2026-10-27 cloud session):
 1. batch45 (graft44.py): the 31 Sanctuary water effects of 1.19 are copied, with their object closures, from the retail
    PS4 village / oasis / graveyard / fort / old-quarter / warzone modules into the retail PS4 part-gompa-multi.bin
    (batch45-graft.tsv). Result must have md5 c77a9e0c....
 2. batch48 (build48.py, emitconv.py): the 22 PS3 1.19 emitters of 7 effect groups are converted to the PS4 emitter
    format and appended to the retail PS4 cave / shambhala modules and to the batch45 gompa module; the groups are
    re-pointed to their 1.19 emitter lists. The s11 struct samples the converter needs are learned first from the 33
    PS3/PS4 MP part module pairs (epairs.s11_samples).
Every output is md5-checked against the reference overlay; on any mismatch nothing is written and the script exits 1.
  python build_part_modules.py [--apply] [--dest DIR]
Dry run by default; idempotent (files already at the reference md5 are left alone). An existing different file is
backed up to work/backup-part-emitters/ (an existing backup is never overwritten)."""
import argparse, hashlib, os, shutil, sys, time
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import epairs, emitconv, graft44, build48  # noqa: E402

PAKTOOLS = os.environ.get('PAKTOOLS') or "{{PAKTOOLS}}"
sys.path.insert(0, os.path.normpath(os.path.join(PAKTOOLS, '..', 'dcconv')))
from ps3src import Index  # noqa: E402  (tools/dcconv: PS3 dc1 modules by layer, from the psarc archives)

PROJECT = "{{PROJECT}}"
PS4 = "{{PS4}}"
DISC, UPD = "{{PS3_DISC}}", "{{PS3_UPDATE}}"
DEST_REL = 'work/overlay-r3/u3data/build/main/dc2/part-module'
BACKUP_REL = 'work/backup-part-emitters'
BATCH45_MD5 = 'c77a9e0cb983d9ca2072b561565aa537'
EXPECT = {'part-cave-multi': '9c9d7dad7bcc21df44cdf6707cec394a',
          'part-gompa-multi': 'e9c4ea7542f5965f019a6105925e235f',
          'part-shambhala-multi': '3e843674e8996bc1ff018040cbd91038'}


def md5(b): return hashlib.md5(b).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', action='store_true')
    ap.add_argument('--dest', help='write the 3 files here instead of <project>/' + DEST_REL)
    a = ap.parse_args()
    for k, v in (('PROJECT', PROJECT), ('PS4', PS4), ('PS3_DISC', DISC), ('PS3_UPDATE', UPD)):
        if v.startswith('{{') or not v:
            print('FAILED: %s is not set (run this through recipe/rebuild_overlays.py)' % k); return 1
    t0 = time.time()
    main4 = os.path.join(PS4, 'u3data', 'build', 'main', 'dc2', 'part-module')
    idx = Index(DISC, UPD, None)
    want = {}
    for kind, rel, h in build48.read_tsv('inputs.tsv'): want[(kind, rel)] = h
    differ = []

    def ps4(m):
        p = os.path.join(main4, m + '.bin')
        if not os.path.exists(p): raise FileNotFoundError('PS4 retail module missing: ' + p)
        b = open(p, 'rb').read()
        if want.get(('ps4', 'dc2/part-module/%s.bin' % m)) not in (None, hashlib.sha256(b).hexdigest()): differ.append('PS4 ' + m)
        return b

    def ps3(m, layer):
        rel = 'dc1/ps4/%s.bin' % m
        b = idx.read(rel, layer)
        if b is None:
            where = 'dlc-patch18.psarc of the 1.19 update' if layer == 'patch18' else 'build/main/bin.psarc of the disc'
            raise FileNotFoundError('PS3 module %s not found in %s' % (m, where))
        if want.get(('ps3/' + layer, rel)) not in (None, hashlib.sha256(b).hexdigest()): differ.append('PS3 %s (%s)' % (m, layer))
        return b

    try:
        mods = list(build48.read_tsv('mp-modules.tsv'))
        p4 = {m: ps4(m) for m, _, _ in mods}
        p3 = {m: ps3(m, layer) for m, layer, _ in mods}
        # s11 struct samples, learned from the PS3/PS4 MP pairs (what the batch48 session's s11map.pkl held)
        pairs = [(m, epairs.M3(p3[m]), graft44.Mod(p4[m])) for m, _, _ in mods]
        samples = epairs.s11_samples(pairs)
        print('s11 samples: %s' % ', '.join('%08x: %d' % (t, len(v)) for t, v in sorted(samples.items())))
        emitconv.set_s11_samples(samples)
        log = []
        # batch45: retail gompa + the 31 water effects grafted from other retail MP modules
        items = [(int(s, 16), m) for s, m in build48.read_tsv('batch45-graft.tsv')]
        g45 = graft44.graft(p4['part-gompa-multi'], items, {m: p4[m] for _, m in items}, log)
        if md5(g45) != BATCH45_MD5:
            print('\n'.join(log))
            print('FAILED: batch45 gompa base md5 %s, expected %s' % (md5(g45), BATCH45_MD5)); raise SystemExit(1)
        log.append('batch45 part-gompa-multi base: md5 %s (%d bytes)' % (md5(g45), len(g45)))
        # batch48
        names, mat = build48.load_names(), build48.load_mattab()
        outs = {}
        for mod in build48.GROUPS:
            base = g45 if mod == 'part-gompa-multi' else p4[mod]
            outs[mod] = build48.build(mod, base, p3[mod], names, mat, log)
    except SystemExit:
        if differ: print('inputs that differ from the reference copies: ' + ', '.join(differ))
        return 1
    except Exception as ex:
        print('FAILED: %s: %s' % (type(ex).__name__, ex))
        if differ: print('inputs that differ from the reference copies: ' + ', '.join(differ))
        return 1
    for line in log: print(line)
    bad = [m for m in EXPECT if md5(outs[m]) != EXPECT[m]]
    for m in EXPECT:
        print('%s.bin  md5 %s  %s  (%d bytes)' % (m, md5(outs[m]), 'OK' if m not in bad else 'MISMATCH, expected ' + EXPECT[m], len(outs[m])))
    if bad:
        if differ: print('inputs that differ from the reference copies: ' + ', '.join(differ))
        print('FAILED: %d of 3 part modules do not match the reference md5; nothing written' % len(bad))
        return 1
    if differ: print('note: some inputs differ from the reference copies, the outputs still match: ' + ', '.join(differ))
    dest = a.dest or os.path.join(PROJECT, DEST_REL)
    n = {'written': 0, 'already': 0}
    for m, b in outs.items():
        p = os.path.join(dest, m + '.bin')
        if os.path.exists(p) and md5(open(p, 'rb').read()) == EXPECT[m]:
            n['already'] += 1; continue
        if a.apply:
            os.makedirs(dest, exist_ok=True)
            if os.path.exists(p) and not a.dest:
                bk = os.path.join(PROJECT, BACKUP_REL, m + '.bin')
                if not os.path.exists(bk):
                    os.makedirs(os.path.dirname(bk), exist_ok=True); shutil.copy2(p, bk)
            tmp = p + '.tmp'
            open(tmp, 'wb').write(b); os.replace(tmp, p)
        n['written'] += 1
    print('part modules: %d written, %d already at the reference md5%s  (%.1f s)'
          % (n['written'], n['already'], '' if a.apply else ' (dry run, nothing written)', time.time() - t0))
    print('convert-dc2-part-modules: 3/3 md5 OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
