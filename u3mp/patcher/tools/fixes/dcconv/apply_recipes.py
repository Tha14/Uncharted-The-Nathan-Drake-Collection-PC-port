#!/usr/bin/env python3
"""Recipe step dc-recipes: rebuild the overlay DC files the converter does not reproduce (the merged modules
net.bin, menu.bin, modules.bin, medals, looks, scenes, vox*, weapon-*, net-always-loaded(.dci), ss-coop-adv-fort,
the 3 dc2 part modules, overlay-r2 / overlay-port net / medals / modules / render-settings, and the 80 conversions
convert-dc1 does not reproduce, with their .dci) byte for byte from the user's PS4 and PS3 files.

Each recipe (tools/dcconv/dc-recipes.json.gz, made by tools/dcconv/make_recipes.py) holds only the derivation:
copy ops (source file, byte transform, offset, length), the pointer slots and a handful of literal bytes (short
zero runs, a few string-id fragments). Sources are checked by sha256 and every rebuilt file must have the
reference sha256, so nothing is written unless it is exact.
  python apply_recipes.py [--apply] [--only TEXT]
Dry run by default; idempotent (files already at the reference hash are left alone)."""
import argparse, collections, gzip, hashlib, json, os, sys
PAKTOOLS = os.environ.get('PAKTOOLS') or "{{PAKTOOLS}}"
DCCONV = os.path.normpath(os.path.join(PAKTOOLS, '..', 'dcconv'))
sys.path.insert(0, DCCONV)
import dcrecipe            # noqa: E402
from ps3src import Index   # noqa: E402
PROJECT = "{{PROJECT}}"
MAIN4 = "{{PS4}}" + '/u3data/build/main/'
DISC, UPD, DATA2 = "{{PS3_DISC}}", "{{PS3_UPDATE}}", "{{PS3_DATA2}}"


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('--apply', action='store_true'); ap.add_argument('--only'); a = ap.parse_args()
    R = json.load(gzip.open(os.path.join(DCCONV, 'dc-recipes.json.gz'), 'rt', encoding='utf-8'))
    idx = Index(DISC, UPD, DATA2); cache = {}
    # other accepted versions of the server's net18.bin (tools/dcconv/net18-versions.json: hashes only)
    vf = os.path.join(DCCONV, 'net18-versions.json')
    acc = json.load(open(vf, encoding='utf-8'))['accepted'] if os.path.exists(vf) else {}
    variants = {'ps3/data2:dc1/ps4/net.bin': {h: None for h in acc}}

    def src(name):
        if name not in cache:
            kind, rel = name.split(':', 1)
            if kind == 'ps4': cache[name] = open(MAIN4 + rel, 'rb').read()
            else: cache[name] = idx.read(rel, kind.split('/', 1)[1])
        return cache[name]
    tot = collections.Counter()
    for key, rec in sorted(R.items()):
        if a.only and a.only not in key: continue
        ov, rel = key.split('/', 1)
        dst = PROJECT + '/work/' + ov + '/u3data/build/main/' + rel
        if os.path.exists(dst) and hashlib.sha256(open(dst, 'rb').read()).hexdigest() == rec['sha256']:
            tot['already'] += 1; continue
        try:
            out = dcrecipe.apply(rec, {n: src(n) for n in rec['sources']}, variants)
        except (SystemExit, OSError, TypeError) as ex:
            tot['failed'] += 1; print('FAILED', key, ex); continue
        if hashlib.sha256(out).hexdigest() != rec['sha256']:
            # built from another accepted net18.bin version: the reference structure with the server's newer values
            if os.path.exists(dst) and open(dst, 'rb').read() == out:
                tot['already'] += 1; continue
            tot['rebuilt (newer server net18)'] += 1; print('rebuilt from the newer server net18.bin:', key)
        else:
            tot['rebuilt'] += 1
        if a.apply:
            os.makedirs(os.path.dirname(dst), exist_ok=True); open(dst, 'wb').write(out)
    print('dc-recipes:', dict(tot), '' if a.apply else '(dry run, nothing written)')
    return 1 if tot['failed'] else 0


if __name__ == '__main__':
    sys.exit(main())
