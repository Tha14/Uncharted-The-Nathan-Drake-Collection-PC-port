"""u2_levels.py - parse Uncharted 2 level registries (pak22.txt / pak22_ps4.txt).

The registry is a flat list of "key value" lines; each "level-name X" starts a
level block followed by level-id / package / package-ts / sound-bank / cache /
actor lines. This prints, per level, the packages, sound banks and actors it
needs, and can diff two registries.

Usage:
  python u2_levels.py dump <registry.txt|psarc::member> [filter_regex]
  python u2_levels.py diff <a> <b> [filter_regex]
A registry inside a psarc is addressed as  path/to/x.psarc::build/main/pak22.txt
Only names are printed (no game data).
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def read_src(src):
    if '::' in src:
        from psarc_lib import Psarc
        arc, mem = src.split('::', 1)
        p = Psarc(arc)
        for i, n in enumerate(p.names):
            if n.lstrip('/') == mem:
                return p.read(i + 1).decode('latin-1')
        raise SystemExit('member not found: ' + mem)
    return open(src, encoding='latin-1').read()


def parse(text):
    levels = {}; cur = None
    for line in text.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) != 2: continue
        k, v = parts[0], parts[1].strip()
        if k == 'level-name':
            cur = levels.setdefault(v, {}); continue
        if cur is not None:
            cur.setdefault(k, []).append(v)
    return levels


def main():
    cmd = sys.argv[1]
    if cmd == 'dump':
        lv = parse(read_src(sys.argv[2])); flt = re.compile(sys.argv[3] if len(sys.argv) > 3 else '.')
        for name in lv:
            if not flt.search(name): continue
            d = lv[name]
            print(name)
            for k in ('package', 'package-ts', 'sound-bank', 'actor', 'cache'):
                if k in d: print(f'  {k}: ' + ' '.join(d[k]))
        print(len(lv), 'levels total', file=sys.stderr)
    elif cmd == 'check':
        # check <registry> <compare_all.tsv> <column ps3|ps4> [filter]
        lv = parse(read_src(sys.argv[2])); col = sys.argv[4]
        flt = re.compile(sys.argv[5] if len(sys.argv) > 5 else '.')
        have = set()
        for line in open(sys.argv[3], encoding='utf-8').read().splitlines()[1:]:
            k, s3, z3, z4, st, mp = line.split('\t')
            if (col == 'ps3' and z3) or (col == 'ps4' and z4):
                have.add(k)
        miss = {}
        for name, d in lv.items():
            if not flt.search(name): continue
            for p in d.get('package', []):
                k = f'pak22/{p}.pak'.lower()
                if p.endswith('-ingame'):  # resolved per language: -ingame10.pak = English
                    k = f'pak22/{p}10.pak'.lower()
                if k not in have:miss.setdefault(('pak22', k), set()).add(name)
            for b in d.get('sound-bank', []):
                k = f'sound1/banks/{b}.bnk'.lower()
                if k not in have: miss.setdefault(('bank', k), set()).add(name)
            for a in d.get('actor', []):
                k = f'actor27/{a}.pak'.lower()
                if k not in have: miss.setdefault(('actor', k), set()).add(name)
        for (t, k), users in sorted(miss.items()):
            print(f'{t}\t{k}\t{",".join(sorted(users))}')
        print(len(miss), 'missing', file=sys.stderr)
    elif cmd == 'diff':
        a = parse(read_src(sys.argv[2])); b = parse(read_src(sys.argv[3]))
        flt = re.compile(sys.argv[4] if len(sys.argv) > 4 else '.')
        print('only in A:', ' '.join(sorted(n for n in a if n not in b and flt.search(n))))
        print('only in B:', ' '.join(sorted(n for n in b if n not in a and flt.search(n))))
        for n in sorted(set(a) & set(b)):
            if not flt.search(n): continue
            for k in ('package', 'sound-bank', 'actor'):
                x = set(a[n].get(k, [])); y = set(b[n].get(k, []))
                if x != y:
                    print(f'{n} {k}: A-B={sorted(x - y)} B-A={sorted(y - x)}')


if __name__ == '__main__':
    main()
