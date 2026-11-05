"""Classic netgames in overlay net.bin (dctools/classic.py, final form of 2026-11-01 03:33 = v7.08 x051).

Every netgame: +0x68 = old +0x6c (new data copy of the kickback / weapon-mod flag, read by the 10 helper
sites patched in v7.08). The 23 type-0x13 (Classic) netgames: loadout +0x2c 0x8cf8e1cc -> 0xd932420c,
settings +0x64 0x63e35446 -> 0x1ad223b4, +0x6c 1 -> 0 (boosters on). Same size, no pointer change.

The 02:50 version (recovered/dctools/classic.py) lacked the +0x68/+0x6c lines; the 03:33 edit is in the
transcript (main line 8002) and reproduces work/backup-skins/net.bin from work/backup-classic/net.bin.
Recipe 1.1: works on the current net.bin (backup once to work/backup-classic); exits 0 with
'already applied' when no Classic netgame is in the old form. Rerun after any net.bin rebuild."""
import os, shutil, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import DC
from dcedit import DCEdit
ROOT = "{{PROJECT}}/"
R = ROOT + "work/overlay-r3/u3data/build/main/dc1/ps4/"; BK = ROOT + "work/backup-classic/"
NET = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else R + "net.bin"
a = DC(NET)
q = a.bysid[0x3b9a067d][1]; ar = a.ptr(q + 8); N = a.u32(q)
old = [ar + i * 0xd0 for i in range(N) if a.u32(ar + i * 0xd0 + 0xc) == 0x13 and a.u32(ar + i * 0xd0 + 0x2c) == 0x8cf8e1cc]
if not old:
    print('already applied: no Classic netgame in the old form'); sys.exit(0)
e = DCEdit(NET); n = 0
for i in range(N):
    g = ar + i * 0xd0
    e.put32(g + 0x68, a.u32(g + 0x6c))   # x051: kickback / mod flag
    if a.u32(g + 0xc) != 0x13: continue
    assert a.u32(g + 0x2c) == 0x8cf8e1cc and a.u32(g + 0x64) == 0x63e35446 and a.u32(g + 0x6c) == 1
    e.put32(g + 0x2c, 0xd932420c); e.put32(g + 0x64, 0x1ad223b4); e.put32(g + 0x6c, 0); n += 1   # x051: boosters on
assert n == 23, n
out = e.build(); assert len(out) == len(e.orig)
if '--dry-run' in sys.argv:
    print('dry run:', n, 'Classic netgames would change'); sys.exit(0)
os.makedirs(BK, exist_ok=True)
if not os.path.exists(BK + 'net.bin'): shutil.copy2(NET, BK + 'net.bin')
open(NET, 'wb').write(out)
c = DC(NET); q = c.bysid[0x3b9a067d][1]; ar = c.ptr(q + 8)
print(n, len(out), sum(1 for i in range(c.u32(q)) if c.u32(ar + i * 0xd0 + 0xc) == 0x13 and c.u32(ar + i * 0xd0 + 0x2c) == 0xd932420c))
