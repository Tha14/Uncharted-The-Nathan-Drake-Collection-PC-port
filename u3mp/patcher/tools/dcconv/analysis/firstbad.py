import sys, json, collections, struct
sys.path.insert(0, sys.argv[1])
import dcconv as C
S = [json.load(open(x)) for x in sys.argv[2].split(',')]; CV = C.Converter(S)
d3 = open(sys.argv[3], 'rb').read(); d4 = open(sys.argv[4], 'rb').read()
a = C.Mod(d3); b = C.Mod(d4); M = C.anchors(a, b)
out, rep = CV.convert(d3, collections.Counter())
plans = CV.last_plans
nshow = int(sys.argv[5]) if len(sys.argv) > 5 else 3
cnt = 0; prevpl = None; shown = 0
for pl in plans:
    hl3 = 4 if pl['hdr'] is not None else 0; hl4 = 8 if pl['hdr'] is not None else 0
    exp = M.get(pl['s3'] + hl3)
    exp = exp - hl4 if exp is not None else None
    if exp is not None and exp != pl['s4']:
        cnt += 1
        if shown < nshow and (prevpl is None or True):
            shown += 1
            print('MISMATCH obj ps3 %x-%x key %s kind %s: ours s4=%x expected %x (delta %d)' % (pl['s3'], pl['e3'], pl['key'], pl['kind'], pl['s4'], exp, pl['s4'] - exp))
            if prevpl:
                print('  prev:', prevpl['key'], prevpl['kind'], 'ps3 %x-%x' % (prevpl['s3'], prevpl['e3']), 'ours %x..%x' % (prevpl['s4'], prevpl['end']), 'span', prevpl.get('span'), 'units', prevpl.get('units', '')[:16])
                print('  ps3', a.d[prevpl['s3']:prevpl['e3']][:96].hex())
                e_prev = M.get(prevpl['s3'] + (4 if prevpl['hdr'] is not None else 0))
                if e_prev is not None:
                    e_prev -= (8 if prevpl['hdr'] is not None else 0)
                    print('  ps4 expected @%x' % e_prev, b.d[e_prev:exp][:128].hex())
                print('  ps4 ours     @%x' % prevpl['s4'], out[prevpl['s4']:pl['s4']][:128].hex())
    prevpl = pl
# also byte diffs inside correctly placed objects
bad = 0
for pl in plans:
    hl3 = 4 if pl['hdr'] is not None else 0; hl4 = 8 if pl['hdr'] is not None else 0
    exp = M.get(pl['s3'] + hl3)
    if exp is None or exp - hl4 != pl['s4']: continue
    if out[pl['s4']:pl['end']] != d4[pl['s4']:pl['end']]:
        bad += 1
        if bad <= nshow:
            print('CONTENT obj %x key %s kind %s ours %s' % (pl['s3'], pl['key'], pl['kind'], out[pl['s4']:pl['end']][:64].hex()))
            print('                                exp  %s' % d4[pl['s4']:pl['end']][:64].hex())
print('misplaced objects', cnt, 'content-different', bad, 'of', len(plans), 'len', len(out), len(d4))
