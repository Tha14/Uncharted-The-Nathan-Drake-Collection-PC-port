#!/usr/bin/env python3
"""Backs up, patches u3_lan_profiles.c and installs the eula pages. Does not build, deploy or launch.
Usage: apply_eula.py [--apply]   (default: dry run, checks every anchor and the pages)."""
import os, shutil, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = r'{{PROJECT}}'
C = ROOT + '/outputs/builds/lan-lobby-v7.20/u3_lan_profiles.c'
VT = ROOT + '/work/overlay-r3/u3data/build/main/vtex1/ps4/eula'
PG = os.path.join(HERE, 'out', 'vtex1', 'ps4', 'eula')
N = 7
do = '--apply' in sys.argv
P = open(os.path.join(HERE, 'eula_port.c.txt'), encoding='utf8').read()


def blk(a):
    i = P.index('/* ----- BLOCK ' + a)
    k = P.index('\n', i) + 1
    j = P.find('/* ----- BLOCK', k)
    return P[i if a == 'A' else k:j if j >= 0 else None]   # A keeps its comment header


s = open(C, 'rb').read().decode('latin1')
nl = '\r\n' if '\r\n' in s else '\n'
if 'eu_source' in s:
    sys.exit('already patched')


def once(t, old, new):
    if t.count(old) != 1:
        sys.exit('anchor count %d: %r' % (t.count(old), old[:70]))
    return t.replace(old, new)


fix = lambda t: t.replace('\n', nl)
a = '/* widget source for the six PAGE_OPTIONS buttons'
s = once(s, a, fix(blk('A')) + a)
old = (' int hide=k==5&&base&&*(volatile uint8_t*)(base+0x17d5c61u)!=0&&!(X.ec[1]&0x4000u);' + nl +
       ' if(op==8)event[1]=hide?0:1;' + nl +
       ' else if(op==1)set_text(event,hide?"":text(base,s,*(uint32_t*)(source+8)));' + nl)
s = once(s, old, fix(blk('B')))
a = ' if(*(uint32_t*)(page+8)!=0xc7597732u)return;'
s = once(s, a, fix(blk('C')) + a)
a = '  if(n==6){*(uint32_t*)(it+0x30)=21;*(uint32_t*)(it+0x34)=0xcf;*(uint32_t*)(it+0x40)=0x4c414830u+j;X.ec[215]++;}}}'
s = once(s, a, '  if(n==6&&j==5)eu_row=it;' + nl + a)
a = '  if(rpm_source(base,event,s))return 1;   /* x037 */'
s = once(s, a, fix(blk('E')) + a)
pages = [os.path.join(PG, 'p%d.dds' % i) for i in range(1, N + 1)]
miss = [p for p in pages if not os.path.isfile(p)]
print('C anchors ok; pages missing:', len(miss))
if miss:
    sys.exit('run convert_eula.py first')
if not do:
    sys.exit('dry run; pass --apply')
bak = os.path.join(HERE, 'backup-' + time.strftime('%Y%m%d-%H%M%S'))
os.makedirs(bak)
shutil.copy2(C, bak)
if os.path.isdir(VT):
    shutil.copytree(VT, os.path.join(bak, 'eula'))
open(C, 'wb').write(s.encode('latin1'))
os.makedirs(VT, exist_ok=True)
for p in pages:
    shutil.copy2(p, VT)
print('applied; backup', bak, '; rebuild the helper yourself')
