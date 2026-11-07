"""u2_string_pairs.py - pair PS3 and PS4 Uncharted 2 functions that reference the same
unique string (assert texts, log texts, source file names). Read-only; prints addresses
and short strings only.

Usage:
  python u2_string_pairs.py <PS3 EBOOT.elf> <PS4 boot-linked.bin> [--range 42f000-4f2000]
         [--bias 183ce8]
--range limits the PS3 functions (default: the whole text). A PS3 string is used when it
is at least 8 characters, appears exactly once in each image, and both sides have code
references. PS4 function starts are heuristic (see ps4_xref.py); the reference site is
printed as well.
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ps3_elf as P
import ps3_map as M
import ps4_xref as X


def main(argv):
    argv = list(argv)
    bias = 0x183ce8
    lo, hi = 0, 1 << 32
    if '--bias' in argv:
        i = argv.index('--bias'); bias = int(argv[i + 1], 16); del argv[i:i + 2]
    if '--range' in argv:
        i = argv.index('--range'); lo, hi = [int(x, 16) for x in argv[i + 1].split('-')]; del argv[i:i + 2]
    if len(argv) < 3:
        print(__doc__)
        return 1
    an = P.load(argv[1], M.DEF_CACHE)
    im = X.Img(argv[2], bias)
    lea = im.lea_index()
    e = an.elf
    pairs = {}
    done = set()
    for s, v, k in an.refs:
        if k != 'a' or v in done or not (lo <= s < hi):
            continue
        st = e.cstr(v, 200)
        if not st or len(st) < 8:
            continue
        done.add(v)
        b = st.encode('latin1') + b'\0'
        if e.d.count(b) != 1:
            continue
        o = im.d.find(b'\0' + b) + 1 if im.d.find(b'\0' + b) >= 0 else -1
        if o <= 0 or im.d.find(b'\0' + b, o) >= 0:
            continue
        sites4 = lea.get(o, [])
        if not sites4:
            continue
        f3 = sorted(set(an.func_of(x[0]) for x in an.sites(v) if lo <= x[0] < hi))
        f4 = sorted(set(im.o2a(im.func_start(x)) for x in sites4))
        for a in f3:
            pairs.setdefault(a, {}).setdefault(tuple(f4), []).append(st)
    for a in sorted(pairs):
        for f4, sts in pairs[a].items():
            print('%08x -> %-24s %d str  "%s"' % (a, ' '.join('%x' % x for x in f4), len(sts),
                                                  sts[0].strip()[:60]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
