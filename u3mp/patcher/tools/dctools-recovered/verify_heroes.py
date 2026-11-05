import sys, json
sys.path.insert(0, '.')
from common import DC
from heroes_common import hero_rows
from parts_common import part_syms
inp, out, p3, mod_in, mod_out, logp = sys.argv[1:7]
A = DC(inp); N = DC(out); B = DC(p3); L = json.load(open(logp))
excl = {int(x['sid'], 16) for x in L['excluded']}
M4 = [(0, 0), (8, 0x10), (0xc, 0x18), (0x10, 0x1c), (0x14, 0x20), (0x18, 0x24), (0x1c, 0x28), (0x20, 0x2c), (0x24, 0x30), (0x28, 0x34)]
def same_entry(e4, e3):
    if N.cstr(N.ptr(e4 + 8)) != B.cstr(B.ptr(e3 + 4)): return False
    return all(N.u32(e4 + k4) == B.u32(e3 + k3) for k3, k4 in M4)
pa, pn, pb = part_syms(A), part_syms(N), part_syms(B)
# part lists
for s, (o, q, es) in pn.items():
    if s in pa:
        ea = pa[s][2]
        assert len(es) >= len(ea)
        for x, y in zip(es, ea):        # retail prefix byte-identical (string pointers moved by relayout)
            assert N.cstr(N.ptr(x + 8)) == A.cstr(A.ptr(y + 8)) and N.d[x:x + 8] == A.d[y:y + 8] and N.d[x + 0x10:x + 0x38] == A.d[y + 0x10:y + 0x38]
        tail = es[len(ea):]
        if tail:
            b3 = [e for e in pb[s][2] if B.u32(e) not in {A.u32(y) for y in ea} and B.u32(e) not in excl]
            assert len(b3) == len(tail) and all(same_entry(x, y) for x, y in zip(tail, b3)), hex(s)
    else:
        b3 = [e for e in pb[s][2] if B.u32(e) not in excl]
        assert len(b3) == len(es) and all(same_entry(x, y) for x, y in zip(es, b3)), hex(s)
# palettes
from build_palettes import keyed, pal_floats
kb = keyed(B); kn = keyed(N)
for k, o in kn.items():
    if k in kb: assert pal_floats(N, o)[1] == pal_floats(B, kb[k])[1], hex(k[0])
# heroes
ra, rn, rb = hero_rows(A), hero_rows(N), hero_rows(B)
db = {r['sid']: r for r in rb}
assert len(rn) == 79 and [r['sid'] for r in rn[:57]] == [r['sid'] for r in ra]
def lists_equal(sn, sb):
    if sb is None or isinstance(sb, tuple): return True
    return isinstance(sn, dict) and sn['parts'] == sb['parts']
changed = 0
sa_ = {r['sid'] for r in ra}; newb = [r for r in rb if r['sid'] not in sa_]
for i, r in enumerate(rn):
    y = db[r['sid']] if i < 57 else newb[i - 57]
    assert r['name'] == y['name'] and r['sid2'] == y['sid2']
    for k in range(7):
        sa = ra[i]['slots'][k] if i < 57 else None
        if i < 57 and sa is not None and not isinstance(sa, tuple) and not (k in (0, 1) and sa['parts'] != y['slots'][k]['parts']):
            assert r['slots'][k]['parts'] == sa['parts']
        else:
            assert lists_equal(r['slots'][k], y['slots'][k]), (r['name'], k)
    # every referenced list resolves
    for sl in r['slots']:
        if isinstance(sl, dict):
            for ps in sl['parts']: assert ps in N.bysid, (r['name'], hex(ps))
# modules
MI = DC(mod_in); MO = DC(mod_out)
s_, t_, o_ = MO.syms[0]; arr = MO.ptr(o_ + 8)
ent = int(L['modules']['net_entry'], 16)
assert MO.cstr(MO.ptr(ent)) == 'net' and MO.u32(ent + 0x10) == len(N.d)
lo = MO.ptr(ent + 0x20); exp = {MO.u32(MO.ptr(lo + 8) + 4 * i) for i in range(MO.u32(lo))}
assert exp == {x[0] for x in N.syms}, 'export list != net.bin symbols'
print('heroes/parts OK: heroes', len(rn), 'part lists', len(pn), 'symbols', len(N.syms))
