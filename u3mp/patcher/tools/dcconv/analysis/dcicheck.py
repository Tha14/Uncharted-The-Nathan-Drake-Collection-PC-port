import os, re, sys, collections
P = sys.argv[1]; X = 'D:/1. PC Games/Uncharted-windows/u3-recipe-test-dc/ps3/'
C = collections.Counter(); ex = []
for ov in ('overlay-r3',):
    base = P + '/work/' + ov + '/u3data/build/main/'
    for dp, dn, fn in os.walk(base + 'dc1'):
        for f in fn:
            if not f.endswith('.dci'): continue
            p4 = os.path.join(dp, f).replace(os.sep, '/'); rel = p4[len(base):]
            d4 = open(p4, 'rb').read()
            binsz = os.path.getsize(p4[:-4] + '.bin') if os.path.exists(p4[:-4] + '.bin') else None
            m = re.match(rb'\(([^ ]+) \((\d+)\)', d4)
            sz_ok = m and binsz is not None and int(m.group(2)) == binsz
            hit = None
            for ly in ('patch18', 'patch13', 'goty-patch', 'disc'):
                p3 = X + ly + '/' + rel
                if not os.path.exists(p3): continue
                d3 = open(p3, 'rb').read()
                m3 = re.match(rb'\(([^ ]+) \((\d+)\)', d3)
                if m3 and m and d3[m3.end():] == d4[m.end():] and m3.group(1) == m.group(1):
                    hit = ly; break
            C[(bool(sz_ok), hit is not None)] += 1
            if hit is None or not sz_ok:
                if len(ex) < 12: ex.append((rel, binsz, m and m.group(2), hit))
print(C); print(*ex, sep='\n')
