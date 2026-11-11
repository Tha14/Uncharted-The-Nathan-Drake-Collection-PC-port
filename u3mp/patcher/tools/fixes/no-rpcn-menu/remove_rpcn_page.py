#!/usr/bin/env python3
"""Recipe step remove-rpcn-page (recipe 1.15.0): no RPCN page in the game's menus.

  python remove_rpcn_page.py [--apply]

Local builds used to add a menu page 0x52504e01 to overlay-r3 menu.bin (a copy of the Options page that the
multiplayer front end opened with L2). The launcher now signs in and shows the RPCN section, the helper (v7.105) never
opens the page, and the step that added it is gone. This step takes the page out of older imports: the page table
(DC symbol 0x74ffceeb, 0x80-byte entries, page id at +8) is rewritten without it (DCEdit, append-only: a new array, the
old bytes stay), and the menu .bin size word of modules.bin (module found by name) follows the new size.
Dry run by default; --apply writes (menu.bin and modules.bin backed up once to work/backup-no-rpcn-page/).
Idempotent: 'already applied' when menu.bin has no such page."""
import argparse, os, shutil, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[0:0] = [HERE, os.path.join(HERE, '..', 'dctools')]
from common import DC                     # noqa: E402  (dctools job, rendered next to this one: with_jobs)
from dcedit import DCEdit                 # noqa: E402

PROJECT = "{{PROJECT}}"
D = PROJECT + "/work/overlay-r3/u3data/build/main/dc1/ps4/"
BK = PROJECT + "/work/backup-no-rpcn-page/"
PAGES, PAGE, PAGE_ID = 0x74ffceeb, 0x80, 0x52504e01
MODULE_T = 0x61183179                      # modules.bin module list (entries 0x28: name pointer +0, .bin size +0x10)


def module_size_slot(mod, name):
    m = DC(mod)
    hits = []
    for s, t, o in m.syms:
        if t != MODULE_T:
            continue
        n, arr = m.u32(o), m.ptr(o + 8)
        for i in range(n):
            e = arr + i * 0x28
            if m.isptr(e) and m.cstr(m.ptr(e)) == name:
                hits.append(e + 0x10)
    if len(hits) != 1:
        raise SystemExit('modules.bin: %d entries for module %r (expected 1)' % (len(hits), name))
    return hits[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--apply', action='store_true')
    a = ap.parse_args()
    menu, mods = D + 'menu.bin', D + 'modules.bin'
    c = DC(menu)
    o = [o for s, t, o in c.syms if s == PAGES]
    if len(o) != 1:
        raise SystemExit('menu.bin: page table symbol %08x found %d times' % (PAGES, len(o)))
    o = o[0]
    n, arr = c.u32(o), c.ptr(o + 8)
    ids = [c.u32(arr + k * PAGE + 8) for k in range(n)]
    if PAGE_ID not in ids:
        print('already applied: menu.bin has no page %08x (%d pages)' % (PAGE_ID, n))
        return 0
    keep = [k for k in range(n) if ids[k] != PAGE_ID]
    e = DCEdit(menu)
    na = e.alloc(PAGE * len(keep), 16)
    for j, k in enumerate(keep):
        e.copy_words(arr + k * PAGE, na + j * PAGE, PAGE)
    e.put32(o, len(keep))
    e.putptr(o + 8, na)
    out = e.build()
    slot = module_size_slot(mods, 'menu')
    md = bytearray(open(mods, 'rb').read())
    old = struct.unpack_from('<Q', md, slot)[0]
    if old != os.path.getsize(menu):
        raise SystemExit('modules.bin: menu size %d is not menu.bin\'s %d' % (old, os.path.getsize(menu)))
    struct.pack_into('<Q', md, slot, len(out))
    print('page %08x removed: %d -> %d pages; menu.bin %d -> %d bytes, modules.bin menu size updated'
          % (PAGE_ID, n, len(keep), old, len(out)))
    if not a.apply:
        print('dry run: nothing written')
        return 0
    os.makedirs(BK, exist_ok=True)
    for f in (menu, mods):
        k = BK + os.path.basename(f)
        if not os.path.exists(k):
            shutil.copyfile(f, k)
    for f, data in ((menu, out), (mods, bytes(md))):
        tmp = f + '.tmp'
        with open(tmp, 'wb') as fh:
            fh.write(data)
        os.replace(tmp, f)
    print('applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
