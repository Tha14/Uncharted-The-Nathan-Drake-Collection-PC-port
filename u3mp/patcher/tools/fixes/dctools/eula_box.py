"""Six u16 layout values 100 -> 200 in overlay menu.bin (the EULA page boxes), written 2026-11-01 22:21 together
with the EULA page textures (main transcript line 11901, comment 'box ok'). Not in the 2026-11-03 recipe; found
on 2026-11-05 when the rebuilt menu.bin differed from the overlay in exactly these six places.
The offsets are in the original part of menu.bin (mk_rpcn only appends), so they hold for the converted menu.bin
whatever the RPCN page. Dry run by default; --apply writes (backup once to work/backup-eula-box). Idempotent."""
import os, shutil, struct, sys
M = "{{PROJECT}}/work/overlay-r3/u3data/build/main/dc1/ps4/menu.bin"
BK = "{{PROJECT}}/work/backup-eula-box/"
OFFS = (0xdabc, 0xddfc, 0xe004, 0xe20c, 0xe414, 0xe61c)
m = bytearray(open(M, 'rb').read())
vals = [struct.unpack_from('<H', m, o)[0] for o in OFFS]
if all(v == 200 for v in vals):
    print('already applied: EULA boxes are 200'); sys.exit(0)
assert all(v == 100 for v in vals), ['%#x=%d' % (o, v) for o, v in zip(OFFS, vals)]
for o in OFFS: struct.pack_into('<H', m, o, 200)
if '--apply' not in sys.argv:
    print('dry run: 6 values would change'); sys.exit(0)
os.makedirs(BK, exist_ok=True)
if not os.path.exists(BK + 'menu.bin'): shutil.copy2(M, BK + 'menu.bin')
open(M, 'wb').write(m); print('box ok')
