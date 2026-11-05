"""Index every PS3 pak23 level pak in the PS3 archives, in the cloud pipeline's source priority (mapsets._ps3_index):
dlc-patch18, dlc-patch13, dlc-map1..4, goty-patch, goty-dlc, pak23.psarc, then DATA2 build/main[/pak23]/*-7.psarc.
usage: ps3index.py <update USRDIR> <dlcmaps USRDIR> <disc USRDIR> <DATA2 USRDIR> <out.tsv>   (PYTHONPATH = paktools)"""
import sys, os, glob
from psarc_lib import Psarc
U, M, D, D2, OUT = sys.argv[1:6]
arcs = [('dlc-patch18', U + '/dlc-patch18.psarc'), ('dlc-patch13', U + '/dlc-patch13.psarc')]
arcs += [('dlc-map%d' % k, M + '/dlc-map%d.psarc' % k) for k in (1, 2, 3, 4)]
arcs += [('goty-patch', D + '/goty-patch.psarc'), ('goty-dlc', D + '/goty-dlc.psarc'), ('pak23', D + '/build/main/pak23.psarc')]
arcs += [(os.path.basename(f)[:-6], f) for f in sorted(glob.glob(glob.escape(D2) + '/build/main/*.psarc') + glob.glob(glob.escape(D2) + '/build/main/pak23/*.psarc'))]
with open(OUT, 'w') as o:
    o.write('name\tprio\tarchive\tentry\tsize\tpath\n')
    for pr, (k, a) in enumerate(arcs):
        if not os.path.exists(a):
            print('missing', a); continue
        p = Psarc(a)
        n = 0
        for i, nm in enumerate(p.names):
            if nm.endswith('.pak') and ('/pak23/' in nm or '/' not in nm):
                o.write('%s\t%d\t%s\t%d\t%d\t%s\n' % (os.path.basename(nm)[:-4], pr, k, i + 1, p.entries[i + 1][2], a)); n += 1
        print(k, n)
