"""u2_strgrep.py - count / show short printable strings matching a regex inside
files or psarc members (read-only; for inventory notes only).

Usage:
  python u2_strgrep.py <regex> <src> [<src> ...] [--show N] [--min L]
  src = file path, or archive.psarc::member_regex (all matching members)
Prints per source: number of matching strings and, with --show, up to N unique
matches (max 80 chars each). Use for names (maps, menus, files), not for dumping
game text.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def strings(data, minlen):
    for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, data):
        yield m.group().decode('ascii')


def sources(src):
    if '::' in src:
        from psarc_lib import Psarc
        arc, mr = src.split('::', 1)
        p = Psarc(arc); r = re.compile(mr)
        for i, n in enumerate(p.names):
            if r.search(n):
                yield f'{os.path.basename(arc)}::{n}', p.read(i + 1)
    else:
        yield src, open(src, 'rb').read()


def main():
    args = sys.argv[1:]; show = 0; minlen = 4
    if '--show' in args:
        i = args.index('--show'); show = int(args[i + 1]); del args[i:i + 2]
    if '--min' in args:
        i = args.index('--min'); minlen = int(args[i + 1]); del args[i:i + 2]
    rx = re.compile(args[0], re.I)
    for src in args[1:]:
        for name, data in sources(src):
            hits = [s for s in strings(data, minlen) if rx.search(s)]
            print(f'{len(hits)}\t{name}')
            if show:
                seen = []
                for h in hits:
                    h = h[:80]
                    if h not in seen: seen.append(h)
                for h in seen[:show]: print('   ', h)


if __name__ == '__main__':
    main()
