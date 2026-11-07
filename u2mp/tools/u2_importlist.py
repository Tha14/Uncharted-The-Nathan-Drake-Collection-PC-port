"""u2_importlist.py - derive the minimal PS3 -> PS4 Uncharted 2 multiplayer import list.

Usage:
  python u2_importlist.py <compare_all.tsv> <out.tsv> [--hash ps3_list.tsv=ROOT ...]

Reads compare_all.tsv (from u2_compare.py) and selects the PS3 assets the PS4
Nathan Drake Collection lacks for multiplayer / co-op, grouped:
  A-skins      actor27/mp-hero-*.pak missing on PS4 (net.bin still lists them)
  B-fort       The Fort (free map, title-update content) pieces missing on PS4
  C-dlc-map1   map pack 1 (facility-multi, ruins-multi DLC version)
  D-dlc-map2   map pack 2 (highrise-multi-2, museum-multi, museum-multi-eva, npc updates)
  E-base-mp    base-game MP/co-op extras missing on PS4 (train-wreck props, coop bank,
               MP/co-op music, DLC character voice lines pnk/rka)
  F-optional   late title-update content the PS4 code does not reference
               (siege-v1 / wt-v1 levels, lost-city-storm bank, *.networking.crypt)
Never selected: movies, EBOOT, promo1/*.edat (licence files - must not be copied),
fonts, debug paks.

With --hash, each selected row gets a SHA-1 computed from the best PS3 source
(read-only, in memory). Each listing is given as <list.tsv>=<root dir it was made from>.
Only names, sizes and hashes are written.
"""
import hashlib, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

MUSIC_MP = re.compile(r'(^|_)(mp_(win|lose)\d|[a-z0-9]+_coop\d)', re.I)


def group(k, best, srcs, st):
    name = k.split('/')[-1]
    if st == 'ps4_only':
        return None
    if k.startswith(('movie1/', 'promo1/', 'data/fonts/', 'misc1/')) or '/' not in k:
        return None
    if k.startswith('sound1/music/'):
        return 'E-base-mp' if st == 'ps3_only' and MUSIC_MP.search(name) else None
    if k.startswith('sound1/speech/'):
        return 'E-base-mp' if st == 'ps3_only' and re.match(r'(pnk|rka)-', name) else None
    if st == 'ps3_only':
        if k.startswith('actor27/mp-hero'):
            return 'A-skins'
        if re.search(r'siege-v1|wt-v1|lost-city-storm|networking\.crypt', k):
            return 'F-optional'
        if best == 'dlc-map1':
            return 'C-dlc-map1'
        if best == 'dlc-map2':
            return 'D-dlc-map2'
        if 'fort' in name or name in ('sea-plane.pak', 'plane-explosion.xvag'):
            return 'B-fort'
        if k.startswith(('pak22/', 'actor27/', 'sound1/banks/', 'sound1/streams/')) and \
                re.search(r'multi|coop|mp-', name):
            return 'E-base-mp'
        return None
    # present on both: take the DLC version where the DLC re-shipped the file
    if best == 'dlc-map1' and k.startswith('pak22/'):
        return 'C-dlc-map1'
    if best == 'dlc-map2' and k.startswith('pak22/') and 'ice-cave' not in name:
        return 'D-dlc-map2'
    return None


def locate_index(pairs):
    """map normalized key -> list of (prec, root, container, member)"""
    from u2_compare import ps3_key, norm, prec
    idx = {}
    for pair in pairs:
        lst, root = pair.split('=', 1)
        for line in open(lst, encoding='utf-8').read().splitlines()[1:]:
            label, cont, p, sz = line.split('\t')
            if cont == '-' and p.lower().endswith('.psarc'):
                continue
            k, src = ps3_key(cont, p)
            idx.setdefault(norm(k), []).append((prec(src), src, root, cont, p))
    return idx


def sha1_of(entry, cache):
    _, src, root, cont, member = entry
    if cont == '-':
        return hashlib.sha1(open(os.path.join(root, member), 'rb').read()).hexdigest()
    from psarc_lib import Psarc
    arc = os.path.join(root, cont)
    if arc not in cache:
        p = Psarc(arc); cache[arc] = (p, {n.lstrip('/'): i + 1 for i, n in enumerate(p.names)})
    p, names = cache[arc]
    return hashlib.sha1(p.read(names[member.lstrip('/')])).hexdigest()


def main():
    cmp_path, out = sys.argv[1], sys.argv[2]
    pairs = sys.argv[sys.argv.index('--hash') + 1:] if '--hash' in sys.argv else []
    rows = []
    for line in open(cmp_path, encoding='utf-8').read().splitlines()[1:]:
        k, best, z3, z4, st, mp, srcs = line.split('\t')
        g = group(k, best, srcs, st)
        if g:
            rows.append((g, k, best, int(z3), z4, st))
    idx = locate_index(pairs) if pairs else {}
    cache = {}
    tot = {}
    with open(out, 'w', encoding='utf-8') as o:
        o.write('group\tkey\tps3_source\tps3_size\tps4_size\tstatus\tsha1\n')
        for g, k, best, z3, z4, st in sorted(rows):
            h = ''
            if k in idx:
                ent = max(idx[k], key=lambda e: e[0])
                try: h = sha1_of(ent, cache)
                except Exception as e: h = 'ERR:' + type(e).__name__
            o.write(f'{g}\t{k}\t{best}\t{z3}\t{z4}\t{st}\t{h}\n')
            t = tot.setdefault(g, [0, 0]); t[0] += 1; t[1] += z3
    for g in sorted(tot):
        print(f'{g}\t{tot[g][0]} files\t{tot[g][1] / 1048576:.1f} MB')
    print(f'TOTAL\t{sum(v[0] for v in tot.values())} files\t{sum(v[1] for v in tot.values()) / 1048576:.1f} MB')


if __name__ == '__main__':
    main()
