#!/usr/bin/env python3
"""Restore the PS3 hair match keys in the overlay net.bin. Dry run by default; --apply writes, after a backup.
   python apply_hair_key.py [--also-hair-kind] [--apply] [net.bin]
Why: the part lookup 0x9967f0/0x9952a0 case k=7 finds the hair as: key = accessory entry +0x20; hair = first body-list
entry with +0x18 == 7 and +0x20 == key (none entry key = 0x5e74ad64). Retail PS4 has no accessory slot on these heroes,
so key = 0 and its hair entries carry 0; build_heroes.py restored the accessory slot but not the key: no hat -> bald.
Default: mp-chloe-u2-hair in lists 8f89357a (chloe-u2) and d22cbb8d (chloe-u2-leather).
--also-hair-kind: rescuer and dead-agent hair too (PS4 kind 5/key 0 -> PS3 kind 7/key 5e74ad64): not bald today,
but the hair shows under hats, unlike PS3."""
import os, shutil, struct, sys
ROOT = r"{{PROJECT}}/"
NET = ROOT + "work/overlay-r3/u3data/build/main/dc1/ps4/net.bin"
# (list sid, entry sid, old kind, old key, new kind, new key); overlay file offsets are found below, never hardcoded
FIX = [(0x8f89357a, 0x61465a13, 7, 0, 7, 0x5e74ad64), (0xd22cbb8d, 0x61465a13, 7, 0, 7, 0x5e74ad64)]
OPT = [(0x4a7d3938, 0x34521b74, 5, 0, 7, 0x5e74ad64), (0xc9d6ac4b, 0x9376a2f1, 5, 0, 7, 0x5e74ad64)]
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'dctools'))
from common import DC
args = [a for a in sys.argv[1:] if not a.startswith('--')]
apply_ = '--apply' in sys.argv
path = args[0] if args else NET
c = DC(path); data = bytearray(open(path, 'rb').read()); todo = []
for ps, es, ok, ov, nk, nv in FIX + (OPT if '--also-hair-kind' in sys.argv else []):
    t, o = c.bysid[ps]
    assert t == 0x2a8027cf, hex(t)
    n = c.u32(o); q = c.ptr(o + 8)
    hits = [q + j * 0x38 for j in range(n) if c.u32(q + j * 0x38 + 0x10) == es]
    assert len(hits) == 1, (hex(ps), hits)
    e = hits[0]; kind, key = struct.unpack_from('<I', data, e + 0x18)[0], struct.unpack_from('<I', data, e + 0x20)[0]
    art = c.cstr(c.ptr(e + 8))
    if (kind, key) == (nk, nv):
        print('%08x %s already fixed' % (ps, art)); continue
    assert (kind, key) == (ok, ov), (hex(ps), art, kind, hex(key))
    print('%08x %-26s entry %#x: kind %d -> %d (+0x18), key %#x -> %#x (+0x20)' % (ps, art, e, kind, nk, key, nv))
    todo.append((e, nk, nv))
if not apply_:
    print('dry run: %d entries would change in %s; nothing written' % (len(todo), path)); sys.exit(0)
if todo:
    bak = path + '.bak-hairkey'
    if not os.path.exists(bak): shutil.copy2(path, bak); print('backup', bak)
    for e, nk, nv in todo:
        struct.pack_into('<I', data, e + 0x18, nk); struct.pack_into('<I', data, e + 0x20, nv)
    open(path, 'wb').write(data); print('written; same size, no pointers moved')
