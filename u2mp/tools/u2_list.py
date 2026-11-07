"""u2_list.py - read-only file listing of an Uncharted 2 dump (PS3 or PS4).

Walks a directory tree and writes one TSV row per loose file and per member of
every .psarc found (PSARC TOC is read; only entry 0, the manifest, is
decompressed). No game bytes are written - only names and sizes.

Usage:
  python u2_list.py <root_dir> <out.tsv> [label]

Columns: label, container (relative path of psarc or '-'), path, size
Requires psarc_lib.py (copied from u3mp/paktools) next to this script.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from psarc_lib import Psarc


def main():
    root, out = sys.argv[1], sys.argv[2]
    label = sys.argv[3] if len(sys.argv) > 3 else os.path.basename(root)
    rows = 0
    with open(out, 'w', encoding='utf-8') as o:
        o.write('label\tcontainer\tpath\tsize\n')
        for dp, dn, fn in os.walk(root):
            dn.sort()
            for f in sorted(fn):
                full = os.path.join(dp, f)
                rel = os.path.relpath(full, root).replace('\\', '/')
                sz = os.path.getsize(full)
                o.write(f'{label}\t-\t{rel}\t{sz}\n'); rows += 1
                if f.lower().endswith('.psarc'):
                    try:
                        p = Psarc(full)
                        for i, n in enumerate(p.names):
                            o.write(f'{label}\t{rel}\t{n.lstrip("/")}\t{p.entries[i+1][2]}\n'); rows += 1
                    except Exception as e:
                        print('psarc fail', rel, e, file=sys.stderr)
    print(rows, 'rows ->', out)


if __name__ == '__main__':
    main()
