"""u2_compare.py - compare PS3 and PS4 Uncharted 2 listings made by u2_list.py.

Usage:
  python u2_compare.py <out_dir> <ps4_list.tsv> <ps3_list.tsv> [<ps3_list.tsv> ...]

Builds a logical key per asset: "<category>/<file name>" where category is the
build/main sub-folder (pak22, actor27, dc1, text1, sound1/..., ...), with the
PS4 "ps4/" platform folder, the "_ps4" suffix and the PS3 psarc container removed.

PS3 sources (several listings may be given, e.g. the PSN NPUA80698 copy and the
RPCS3 BCUS98123 HDD copy with DLC) are tagged:
  base        build/main/*.psarc or loose base files
  patch       NPUA80698 all-patches.psarc
  dlc-patchN  BCUS98123 build/main/dlc-patchN.psarc (disc title update data)
  dlc-mapN    BCUS98123 build/main/dlc-mapN.psarc   (map-pack DLC)
The size kept for PS3 is the one from the highest-precedence source
(dlc-patch7 > ... > dlc-patch2 > patch > dlc-map2 > dlc-map1 > base); all
sources are listed.

Writes to out_dir:
  compare_all.tsv  key, ps3_src(best), ps3_size, ps4_size, status, mp, ps3_all_srcs
  compare_mp.tsv   only rows flagged as multiplayer-related (name regex)
Prints a summary per category. Names and sizes only.
"""
import csv, os, re, sys
from collections import defaultdict

MP_RE = re.compile(r'(^|[-_/])(mp|multi|multiplayer|coop|co-op|net|lobby|online|'
                   r'matchmak|deathmatch|plunder|elimination|turf|siege|gold-rush|'
                   r'king-of-the-hill|team|networking)([-_.0-9]|$)', re.I)


def prec(src):
    if src.startswith('dlc-patch'): return 100 + int(src[9:] or 0)
    if src == 'patch': return 90
    if src.startswith('dlc-map'): return 50 + int(src[7:] or 0)
    return 0


def ps3_key(container, path):
    """Return (key, source)."""
    if container == 'all-patches.psarc':
        p, src = path, 'patch'
    elif container == '-':
        p, src = path, 'base'
        m = re.match(r'^build/main/(dlc-(?:map|patch)\d+)/(.*)$', p)
        if m:  # loose extracted copy of a dlc psarc
            p, src = 'build/main/' + m.group(2), m.group(1)
    else:
        m = re.match(r'^build/main/(dlc-(?:map|patch)\d+)\.psarc$', container)
        if m:
            p, src = 'build/main/' + path, m.group(1)
        else:
            base = container[:-len('.psarc')]
            if base.startswith('build/main/sound1/speech-update/'):
                base = base.replace('speech-update', 'speech')
            p, src = base + '/' + path, 'base'
    p = re.sub(r'^build/main/', '', p)
    p = re.sub(r'^fonts/', 'data/fonts/', p)
    return p, src


def ps4_key(container, path):
    p = path if container == '-' else container[:-len('.psarc')] + '/' + path
    p = re.sub(r'^build/main/', '', p)
    p = re.sub(r'^(core|bin|uiextra)/', '', p)  # PS4 regroups text1/boot1/gallery1 into psarcs
    p = p.replace('/ps4/', '/')
    p = re.sub(r'_ps4(?=\.|/)', '', p)
    p = re.sub(r'^dc2/part-module/', 'dc1/', p)  # PS4 moved level dc parts to dc2/part-module
    return p


def norm(k):
    k = k.lower()
    # PS4 speech psarc members carry their language folder again: speech/english/english/x
    k = re.sub(r'^(sound1/speech/)([a-z-]+)/\2/', r'\1\2/', k)
    return k


def load_ps3(paths):
    d = {}
    for path in paths:
        with open(path, encoding='utf-8') as f:
            r = csv.reader(f, delimiter='\t'); next(r)
            for label, cont, p, sz in r:
                if cont == '-' and p.lower().endswith('.psarc'):
                    continue
                k, src = ps3_key(cont, p); k = norm(k)
                e = d.setdefault(k, {'best': None, 'size': 0, 'srcs': set()})
                e['srcs'].add(src)
                if e['best'] is None or prec(src) >= prec(e['best']):
                    e['best'], e['size'] = src, int(sz)
    return d


def load_ps4(path):
    d = {}
    with open(path, encoding='utf-8') as f:
        r = csv.reader(f, delimiter='\t'); next(r)
        for label, cont, p, sz in r:
            if cont == '-' and p.lower().endswith('.psarc'):
                continue
            d[norm(ps4_key(cont, p))] = int(sz)
    return d


def main():
    out, p4path, p3paths = sys.argv[1], sys.argv[2], sys.argv[3:]
    p3 = load_ps3(p3paths); p4 = load_ps4(p4path)
    keys = sorted(set(p3) | set(p4))
    summ = defaultdict(lambda: [0, 0, 0])
    with open(os.path.join(out, 'compare_all.tsv'), 'w', encoding='utf-8') as o, \
         open(os.path.join(out, 'compare_mp.tsv'), 'w', encoding='utf-8') as m:
        hdr = 'key\tps3_src\tps3_size\tps4_size\tstatus\tmp\tps3_all_srcs\n'; o.write(hdr); m.write(hdr)
        for k in keys:
            s3 = p3.get(k); s4 = p4.get(k)
            st = 'both' if s3 and s4 is not None else ('ps3_only' if s3 else 'ps4_only')
            mp = 1 if MP_RE.search(os.path.basename(k)) else 0
            line = (f'{k}\t{s3["best"] if s3 else ""}\t{s3["size"] if s3 else ""}\t'
                    f'{"" if s4 is None else s4}\t{st}\t{mp}\t'
                    f'{",".join(sorted(s3["srcs"], key=prec)) if s3 else ""}\n')
            o.write(line)
            if mp: m.write(line)
            cat = k.split('/')[0] if '/' in k else '(root)'
            summ[(cat, mp)][{'both': 0, 'ps3_only': 1, 'ps4_only': 2}[st]] += 1
    print('category\tmp\tboth\tps3_only\tps4_only')
    for (c, mp), v in sorted(summ.items()):
        print(f'{c}\t{mp}\t' + '\t'.join(map(str, v)))


if __name__ == '__main__':
    main()
