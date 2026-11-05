#!/usr/bin/env python3
"""Collect the game texts the overlay level registries are made of, into <out>/ (names = source ids):
  retail.txt   PS4 u3data/build/main/pak23_ps4.txt
  ps3-p18.txt / ps3-p13.txt   build/main/pak23.txt of the PS3 1.19 update psarcs
  ps3-disc.txt  disc build/main/pak23.txt
  txt__<archive>__<entry>.txt  every level .txt inside the update / DLC / GOTY / DATA2 psarcs (DLC packages ship
                               their own <level>.txt next to the pak)
usage: regsources.py <PS4 folder> <update USRDIR> <dlcmaps USRDIR> <disc USRDIR> <DATA2 USRDIR> <out>   (PYTHONPATH: psarc_lib)"""
import glob
import os
import sys

from psarc_lib import Psarc


def main():
    ps4, upd, dlc, disc, d2, out = sys.argv[1:7]
    os.makedirs(out, exist_ok=True)
    w = lambda n, b: open(os.path.join(out, n), 'wb').write(b)
    w('retail.txt', open(os.path.join(ps4, 'u3data', 'build', 'main', 'pak23_ps4.txt'), 'rb').read())
    if os.path.exists(os.path.join(disc, 'build', 'main', 'pak23.txt')):
        w('ps3-disc.txt', open(os.path.join(disc, 'build', 'main', 'pak23.txt'), 'rb').read())
    arcs = [(os.path.join(upd, 'dlc-patch18.psarc'), 'p18'), (os.path.join(upd, 'dlc-patch13.psarc'), 'p13')]
    arcs += [(f, os.path.basename(f)[:-6]) for f in sorted(glob.glob(glob.escape(dlc) + '/dlc-map[0-9].psarc'))]
    arcs += [(os.path.join(disc, n), n[:-6]) for n in ('goty-patch.psarc', 'goty-dlc.psarc')]
    arcs += [(f, os.path.basename(f)[:-6]) for f in sorted(glob.glob(glob.escape(d2) + '/build/main/*.psarc') +
                                                           glob.glob(glob.escape(d2) + '/build/main/pak23/*.psarc'))]
    n = 0
    for a, k in arcs:
        if not os.path.exists(a):
            continue
        p = Psarc(a)
        for i, nm in enumerate(p.names):
            if not nm.endswith('.txt'):
                continue
            if nm.lstrip('/') == 'build/main/pak23.txt' and k in ('p18', 'p13'):
                w('ps3-%s.txt' % k, p.read(i + 1)); n += 1
            elif '/pak23/' in nm or '/' not in nm or nm.split('/')[-1].startswith('lab'):
                w('txt__%s__%s' % (k, nm.lstrip('/').replace('/', '__')), p.read(i + 1)); n += 1
    print('sources', n + 2)


if __name__ == '__main__':
    main()
