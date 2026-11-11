#!/usr/bin/env python3
"""Recipe step remove-lab (recipe 1.15.0): no Lab maps on PC.

  python remove_lab.py [--apply]

The owner decided that the PS3 Lab maps (the 14 map-list entries whose main level starts with 'lab-') are not part of
the PC import, whatever PS3 game data the player has (their RPCS3 DATA2 still holds the Lab psarcs). Since 1.15.0 the
steps that built them are retired (levels-lab, levels-maps-batch) and convert-actors skips the 48 Lab art groups. This
step removes what older imports (and steps that still touch Lab lines, such as lab-registration and registry-final)
left in the four overlays:
  * pak23/ps4/lab-*.pak and lab34-*.pak (the Lab level paks: 70 in a 1.14 import),
  * actor33/ps4/<lab_actors.txt>.pak (the 48 box / stadium art groups only Lab levels use),
  * vtex1/ps4/net-levels/lab-*.dds (the Lab map pictures),
  * every pak23_ps4.txt block whose level-name starts with 'lab' (from that line to the next level-name line).
Kept on purpose: render-settings/game/lab.bin (a DC module modules.bin lists), the Lab playlist picture and the
'labicon' emblem (not maps), Lab modes on the regular maps. The net.bin side (Lab maps not listed, Lab netgames out of
the playlists, PS3 netgame indices kept for crossplay) is step netgame-order-119, which runs after this one.
Dry run by default; --apply deletes and rewrites (pak23_ps4.txt backed up once to work/backup-no-lab/).
Idempotent: prints 'already applied' and exits 0 when nothing Lab is left."""
import argparse, os, re, shutil, sys

PROJECT = "{{PROJECT}}"
WORK = PROJECT + "/work/"
BK = WORK + "backup-no-lab/"
OVERLAYS = ('overlay-r3', 'overlay-r2', 'overlay-port', 'overlay-mp')
HERE = os.path.dirname(os.path.abspath(__file__))
LAB_LEVEL = re.compile(r'^lab(-|\d)', re.I)        # lab-multi-*, lab-stage, lab-trench-*, lab34-*


def lab_actors():
    with open(os.path.join(HERE, 'lab_actors.txt'), encoding='utf-8') as f:
        return {ln.strip() for ln in f if ln.strip() and not ln.startswith('#')}


def strip_registry(text):
    """text without the blocks of Lab levels; (new text, removed level names)"""
    nl = '\r\n' if '\r\n' in text else '\n'
    out, removed, drop = [], [], False
    for ln in text.split(nl):
        w = ln.split()
        if w and w[0] == 'level-name':
            drop = len(w) > 1 and bool(LAB_LEVEL.match(w[1]))
            if drop:
                removed.append(w[1])
        if not drop:
            out.append(ln)
    return nl.join(out), removed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    actors = lab_actors()
    files, registries = [], []
    for ov in OVERLAYS:
        main_dir = WORK + ov + '/u3data/build/main/'
        d = main_dir + 'pak23/ps4/'
        if os.path.isdir(d):
            files += [d + n for n in sorted(os.listdir(d)) if n.endswith('.pak') and LAB_LEVEL.match(n)]
        d = main_dir + 'actor33/ps4/'
        if os.path.isdir(d):
            files += [d + n for n in sorted(os.listdir(d)) if n.endswith('.pak') and n[:-4] in actors]
        d = main_dir + 'vtex1/ps4/net-levels/'
        if os.path.isdir(d):
            files += [d + n for n in sorted(os.listdir(d)) if n.endswith('.dds') and n.lower().startswith('lab-')]
        reg = main_dir + 'pak23_ps4.txt'
        if os.path.isfile(reg):
            with open(reg, 'rb') as f:
                text = f.read().decode('latin-1')
            new, removed = strip_registry(text)
            if removed:
                registries.append((ov, reg, new, removed))
    if not files and not registries:
        print('already applied: no Lab level, actor, picture or registry block in the overlays')
        return 0
    for f in files:
        print('remove', os.path.relpath(f, WORK).replace(os.sep, '/'))
    for ov, reg, new, removed in registries:
        print('%s pak23_ps4.txt: %d Lab level blocks removed (%s)' % (ov, len(removed), ', '.join(removed)))
    if not a.apply:
        print('dry run: %d files, %d registries; nothing changed' % (len(files), len(registries)))
        return 0
    os.makedirs(BK, exist_ok=True)
    for ov, reg, new, removed in registries:
        keep = BK + ov + '-pak23_ps4.txt'
        if not os.path.exists(keep):
            shutil.copyfile(reg, keep)
        tmp = reg + '.tmp'
        with open(tmp, 'wb') as f:
            f.write(new.encode('latin-1'))
        os.replace(tmp, reg)
    for f in files:
        os.remove(f)
    print('applied: %d Lab files removed, %d registries cleaned' % (len(files), len(registries)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
