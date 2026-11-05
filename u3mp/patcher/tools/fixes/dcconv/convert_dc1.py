#!/usr/bin/env python3
"""Recipe step convert-dc1: the PS3 1.19 DC modules that the overlays carry as whole-file conversions
(ss-multiplayer state scripts, ai-*, destruction, player-settings, render-settings) converted to PS4 DC with
tools/dcconv/dcconv.py, plus their .dci (the PS3 .dci with the size field set to the PS4 .bin size).

  python convert_dc1.py [--apply] [--only NAME[,NAME]] [--report FILE]

The file list (dc1-convert.tsv, next to this script) holds per file: overlay, PS4 path, PS3 layer, the sha256
the converted file must have (the 2026-10-27 cloud conversion, before the later option-default fixes) and the
state of the converter for it (identical / different). Layouts come from two schemas: schema-oldconv.json
(learned from the 2026-10-27 conversions, shipped: offsets and type ids only, no game data) and the retail
schema learned from the user's own PS3 disc + PS4 Collection DC pairs (learned once, cached in
work/_dcconv/schema-retail.json, about 10 minutes). Files whose sha256 differs from the reference are still
written (best effort) and listed as DIFFERENT. Dry run by default. Idempotent: files already at the reference
hash are left alone."""
import argparse, collections, hashlib, json, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
PAKTOOLS = os.environ.get('PAKTOOLS') or "{{PAKTOOLS}}"
DCCONV = os.path.normpath(os.path.join(PAKTOOLS, '..', 'dcconv'))
sys.path.insert(0, DCCONV)
import dcconv            # noqa: E402
from ps3src import Index  # noqa: E402
PROJECT = "{{PROJECT}}"
PS4 = "{{PS4}}"
DISC, UPD, DATA2 = "{{PS3_DISC}}", "{{PS3_UPDATE}}", "{{PS3_DATA2}}"


def sha(b): return hashlib.sha256(b).hexdigest()


def retail_schema(idx, log):
    cache = PROJECT + '/work/_dcconv/schema-retail.json'
    if os.path.exists(cache): return json.load(open(cache))
    ret = PS4 + '/u3data/build/main/'
    pairs = []
    tmp = PROJECT + '/work/_dcconv/ps3-disc/'
    for rel in idx.names():
        if not rel.endswith('.bin') or not os.path.exists(ret + rel): continue
        d = idx.read(rel, 'disc')
        if d is None: continue
        p3 = tmp + rel; os.makedirs(os.path.dirname(p3), exist_ok=True); open(p3, 'wb').write(d)
        a, b = dcconv.Mod(d), dcconv.Mod(open(ret + rel, 'rb').read())
        if len(a.slots) == len(b.slots): pairs.append((p3, ret + rel))      # same rule as the 2026-11-05 test
    log('learning the retail schema from %d PS3/PS4 pairs (one time)' % len(pairs))
    S = dcconv.learn(pairs, cache, log=log)
    import shutil; shutil.rmtree(tmp, ignore_errors=True)
    return S


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--apply', action='store_true'); ap.add_argument('--only')
    ap.add_argument('--report')
    ap.add_argument('--force-files', action='store_true', help='rewrite files that exist with other content')
    ap.add_argument('--all', action='store_true', help='also write the files the converter does not reproduce exactly '
                    '(best effort, unverified); default: only the files verified sha256-identical')
    a = ap.parse_args()
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(HERE, 'dc1-convert.tsv'), encoding='utf-8') if l.strip() and not l.startswith('#')]
    if a.only: rows = [r for r in rows if any(x in r[1] for x in a.only.split(','))]
    idx = Index(DISC, UPD, DATA2)
    schemas = [json.load(open(os.path.join(DCCONV, 'schema-oldconv.json'))), retail_schema(idx, print)]
    C = dcconv.Converter(schemas)
    tot = collections.Counter(); rep_lines = []
    for ov, rel4, layer, want, state in rows:
        dst = PROJECT + '/work/' + ov + '/u3data/build/main/' + rel4
        rel3 = rel4.replace('dc2/part-module/', 'dc1/ps4/')
        def put_dci(n):
            if '/ss-multiplayer/' not in '/' + rel4: return              # the overlays carry .dci only for these
            dci3 = idx.read(rel3[:-4] + '.dci', layer)
            if dci3 is None: dci3 = idx.read(rel3[:-4] + '.dci')       # the .dci may live in another layer than the .bin
            if dci3 is not None:
                dci = re.sub(rb'^\(([^ ]+) \((\d+)\)', lambda m_: b'(' + m_.group(1) + b' (' + str(n).encode() + b')', dci3, count=1)
                if not os.path.exists(dst[:-4] + '.dci') or open(dst[:-4] + '.dci', 'rb').read() != dci:
                    open(dst[:-4] + '.dci', 'wb').write(dci)
        if os.path.exists(dst):
            # present: either this step's own output or a later fix of it (enemy-stall / coop-audit patch these in
            # place) - never overwrite; --force-files rewrites
            if sha(open(dst, 'rb').read()) == want or not a.force_files:
                tot['already' if sha(open(dst, 'rb').read()) == want else 'present-kept'] += 1
                if a.apply: put_dci(os.path.getsize(dst))
                continue
        if state != 'identical' and not a.all:
            tot['skipped-unverified'] += 1; rep_lines.append('SKIPPED %s %s (converter not exact for it; --all writes it)' % (ov, rel4)); continue
        d3 = idx.read(rel3, layer)
        if d3 is None: tot['no-source'] += 1; rep_lines.append('NO-SOURCE %s %s' % (layer, rel3)); continue
        out, r = C.convert(d3, collections.Counter())
        ok = sha(out) == want
        tot['identical' if ok else 'different'] += 1
        rep_lines.append('%s %s %s' % ('IDENTICAL' if ok else 'DIFFERENT', ov, rel4))
        if a.apply and (ok or a.all):
            os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, 'wb').write(out)
            put_dci(len(out))
    print('convert-dc1:', dict(tot), '' if a.apply else '(dry run, nothing written)')
    for l in rep_lines:
        if not l.startswith('IDENTICAL'): print('  ' + l)
    if a.report: open(a.report, 'w').write('\n'.join(rep_lines) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
