#!/usr/bin/env python3
"""patch_ps4writer.py - stop paktools/ps4writer.py from padding between the last page and the VRAM data.

PakWriter.build() lays pages out with `o = align(o + p.size, 16)` and then takes `vram_base = o`, so a pak whose
last page ends 8 mod 16 gets 8 pad bytes before its VRAM.  The PS4 package loader reads the VRAM bytes straight after
the last page (retail: 1053 paks with VRAM, every one gap 0, 489 of them with the last page ending 8 mod 16), so such
a pak fails to load (level state 0xc).  See fix_vram_gap.py for the paks already affected in overlay-r3.

usage: patch_ps4writer.py [--apply]     (dry run by default; the original is kept as ps4writer.py.orig)"""
import os, shutil, sys

WRITER = '{{PAKTOOLS}}/ps4writer.py'
OLD = """        o = align(hdr_size, 16)
        for p in self.pages:
            page_offs.append(o)
            o = align(o + p.size, 16)
        vram_base = o
"""
NEW = """        o = align(hdr_size, 16)
        end = o
        for p in self.pages:
            page_offs.append(o)
            end = o + p.size
            o = align(end, 16)
        vram_base = end   # the loader takes the VRAM bytes right after the last page: no padding (fix_vram_gap.py)
"""


def main():
    src = open(WRITER, encoding='utf-8').read()
    if NEW in src:
        print('already patched'); return
    if src.count(OLD) != 1:
        sys.exit('expected layout code not found exactly once in ' + WRITER)
    print('patch: vram_base = last page end (was align(last page end, 16)) in', WRITER)
    if '--apply' not in sys.argv:
        print('dry run: nothing written (use --apply)'); return
    if not os.path.exists(WRITER + '.orig'):
        shutil.copy2(WRITER, WRITER + '.orig')
    open(WRITER, 'w', encoding='utf-8').write(src.replace(OLD, NEW))
    print('written; original kept as ps4writer.py.orig')


if __name__ == '__main__':
    main()
