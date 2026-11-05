import sys, os, collections
sys.path.insert(0, sys.argv[1])
import dcconv as C
P = sys.argv[2]; X = 'D:/1. PC Games/Uncharted-windows/u3-recipe-test-dc/ps3/'
out = open(sys.argv[3], 'w', encoding='utf-8'); rep = open(sys.argv[4], 'w', encoding='utf-8')


def score(a, b):
    """how many PS3 data words (byte-swapped) occur in the PS4 file: picks the PS3 layer that was converted"""
    w3 = collections.Counter(a.d[i:i + 4][::-1] for i in range(a.data_start, a.reloc - 3, 4))
    w4 = collections.Counter(b.d[i:i + 4] for i in range(b.data_start, b.reloc - 3, 4))
    return sum((w3 & w4).values())


for ov in ('overlay-r3', 'overlay-r2', 'overlay-port'):
    base = P + '/work/' + ov + '/u3data/build/main/'
    for sub in ('dc1', 'dc2'):
        for dp, dn, fn in os.walk(base + sub):
            for f in sorted(fn):
                if not f.endswith('.bin'): continue
                p4 = os.path.join(dp, f).replace(os.sep, '/'); rel = p4[len(base):]
                b = C.Mod(open(p4, 'rb').read())
                found = []
                for ly in ('data2', 'patch18', 'patch13', 'goty-patch', 'disc'):
                    p3 = X + ly + '/' + rel.replace('dc2/part-module/', 'dc1/ps4/')
                    if not os.path.exists(p3): continue
                    a = C.Mod(open(p3, 'rb').read())
                    ok = len(a.dslots) == len(b.dslots) and [s for s, _, _ in a.syms] == [s for s, _, _ in b.syms]
                    found.append((ly, ok, score(a, b) if ok else -1, p3))
                good = [x for x in found if x[1]]
                if good:
                    best = max(good, key=lambda x: x[2])   # first layer wins ties (newest)
                    out.write('%s\t%s\n' % (best[3], p4))
                rep.write('%s\t%s\t%d\t%d\t%s\n' % (ov, rel, len(b.dslots), len(b.syms), [(x[0], x[1], x[2]) for x in found]))
