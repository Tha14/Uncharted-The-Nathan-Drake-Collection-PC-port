import sys, json, collections
sys.path.insert(0, sys.argv[1])
import dcconv as C
S = [json.load(open(x)) for x in sys.argv[2].split(',')]; CV = C.Converter(S)
agg = collections.Counter(); ex = {}
for p3, p4 in C.read_pairs(sys.argv[3]):
    d3 = open(p3, 'rb').read(); d4 = open(p4, 'rb').read()
    out, rep = CV.convert(d3, collections.Counter())
    if out == d4: continue
    a = C.Mod(d3); b = C.Mod(d4); M = C.anchors(a, b)
    prev = None; found = False
    for pl in CV.last_plans:
        hl3 = 4 if pl['hdr'] is not None else 0; hl4 = 8 if pl['hdr'] is not None else 0
        exp = M.get(pl['s3'] + hl3)
        if exp is not None and exp - hl4 != pl['s4']:
            k = (prev['key'] if prev else None, prev['kind'] if prev else None, pl['s4'] - (exp - hl4))
            agg[k] += 1; ex.setdefault(k, (p4.split('/')[-1], hex(prev['s3']) if prev else None)); found = True; break
        prev = pl
    if not found:
        # content-only differences
        for pl in CV.last_plans:
            if out[pl['s4']:pl['end']] != d4[pl['s4']:pl['end']]:
                k = ('CONTENT', pl['key'], pl['kind']); agg[k] += 1; ex.setdefault(k, (p4.split('/')[-1], hex(pl['s3']))); found = True; break
    if not found:
        k = ('TAIL', len(out) - len(d4)); agg[k] += 1; ex.setdefault(k, (p4.split('/')[-1],))
for k, n in agg.most_common(40): print(n, k, ex[k])
