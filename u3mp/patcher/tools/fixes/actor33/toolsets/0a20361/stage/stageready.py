#!/usr/bin/env python3
"""Verify converted stage paks (container side) and publish them into a batch folder +
READY.tsv.  usage: stageready.py <batch nn> <stage> --src DIR --reports DIR [--source disc|dlc]
Checks: stagecheck (structure/resolution), ingame12 parses and holds SPAWNER_GROUP/REGION_TREE,
size sanity vs the PS3 source."""
import argparse, json, os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
from ndpak import Pak
import stagecheck

OUT = '/mnt/user-data/outputs/stages'
INV = '/mnt/user-data/uploads/Uncharted The Nathan Drake Collection - U3 Multiplayer/work/inventory/ps3-pak23-psarc.txt'


def disc_names():
    s = set()
    try:
        for l in open(INV):
            f = l.split()
            if len(f) == 3 and f[2].endswith('.pak'):
                s.add(f[2][:-4])
    except OSError:
        pass
    return s


def publish(batch, stage, src, reports, ps3size=None, extra_notes='', ps3dir=None):
    d = os.path.join(OUT, 'batch%02d' % batch, 'u3data', 'build', 'main', 'pak23', 'ps4')
    os.makedirs(d, exist_ok=True)
    pak = os.path.join(src, stage + '.pak')
    ig = os.path.join(src, stage + '-ingame12.pak')
    e, info = stagecheck.check(pak, quiet=True)
    checks = []
    checks.append('stagecheck OK' if not e else 'stagecheck FAIL: ' + '; '.join(e[:3]))
    q = Pak(ig)
    ig_err = q.validate()
    types = sorted(i.type for i in q.items)
    if ig_err or 'SPAWNER_GROUP' not in types:
        checks.append('ingame12 FAIL %s %s' % (ig_err[:2], types))
    else:
        checks.append('ingame12 OK')
    sz = os.path.getsize(pak)
    if ps3size:
        checks.append('size %.1fMB (PS3 %.1fMB)' % (sz / 1e6, ps3size / 1e6))
    rep = {}
    rp = os.path.join(reports, stage + '.report.json')
    if os.path.exists(rp):
        rep = json.load(open(rp))
    approx = rep.get('approx', [])
    nfb = sum(1 for a in approx if a.startswith('material '))
    notes = ['%d materials (%d PS4 templates for PS3-only materials), %d textures reused from PS4, %d converted from PS3'
             % (rep.get('materials', 0), nfb, len(set(rep.get('reused', []))), len(set(rep.get('converted', []))))]
    notes += rep.get('lights', [])
    if rep.get('capped'):
        notes.append('%d PS3-only textures capped to %spx (no PS4 stream; VRAM %.1fMB)'
                     % (len(set(rep['capped'])), os.environ.get('U3_STAGE_MAX_TEX', '256'), rep.get('vram', 0) / 1e6))
    notes += [a for a in approx if not a.startswith('material ') and 'generated tangents' not in a]
    ng = sum(1 for a in approx if 'generated tangents' in a)
    if ng:
        notes.append('%d submeshes with generated tangents' % ng)
    dropped = (rep.get('ingame12') or {}).get('dropped')
    if dropped is None and ps3dir and os.path.exists(os.path.join(ps3dir, stage + '-ingame12.pak')):
        dropped = [i.type for i in Pak(os.path.join(ps3dir, stage + '-ingame12.pak')).items
                   if i.type in ('FEATURE_DB_2', 'LEVEL_OFFSET_1')]
    if dropped:
        notes.append('ingame12: dropped %s (FEATURE_DB_2 = AI cover db, unused in the lobby; LEVEL_OFFSET_1 is zero)'
                     % ','.join(dropped))
    notes.append('PS4 level list has no %s entry: needs level-id %s / packages %s, %s-ingame' % (stage, stage, stage, stage))
    notes.append('no COLLISION_DATA_HAVOK_BG (not converted; retail san-upper-multi-stage has none)')
    notes.append('sky: built into the stage pak (no separate sky pak)')
    if extra_notes:
        notes.append(extra_notes)
    ok = not e and 'FAIL' not in ' '.join(checks)
    if ok:
        shutil.copyfile(pak, os.path.join(d, stage + '.pak'))
        shutil.copyfile(ig, os.path.join(d, stage + '-ingame12.pak'))
    source = 'disc' if stage in disc_names() else 'dlc'
    line = '\t'.join(['batch%02d' % batch, stage, source, ', '.join(checks), ' | '.join(notes)])
    rt = os.path.join(OUT, 'READY.tsv')
    new = not os.path.exists(rt)
    lines = [] if new else [l for l in open(rt).read().splitlines() if l and l.split('\t')[1] != stage]
    if new or not lines or not lines[0].startswith('batch\t'):
        lines.insert(0, 'batch\tname\tsource\tchecks\tnotes')
    if ok:
        lines.append(line)
    open(rt, 'w').write('\n'.join(lines) + '\n')
    print(('PUBLISHED ' if ok else 'NOT PUBLISHED ') + line)
    return ok


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('batch', type=int)
    ap.add_argument('stage', nargs='+')
    ap.add_argument('--src', required=True)
    ap.add_argument('--reports', required=True)
    ap.add_argument('--ps3dir')
    a = ap.parse_args()
    for s in a.stage:
        sz = None
        if a.ps3dir and os.path.exists(os.path.join(a.ps3dir, s + '.pak')):
            sz = os.path.getsize(os.path.join(a.ps3dir, s + '.pak'))
        publish(a.batch, s, a.src, a.reports, sz, ps3dir=a.ps3dir)
