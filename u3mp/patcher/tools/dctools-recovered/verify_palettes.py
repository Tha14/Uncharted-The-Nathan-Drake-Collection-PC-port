import sys, json, struct
sys.path.insert(0, '.')
from common import DC, PAL
from verify_dc import check
base, new, p3, logp = sys.argv[1:5]
log = json.load(open(logp))
allowed = []
for x in log:
    if x['action'] == 'replaced':
        o = int(x['sym_value'], 16); allowed += [(o, 4), (o + 8, 8)]
A, N = check(base, new, allowed)
from build_palettes import keyed
B = DC(p3); b3 = keyed(B)
n_ok = 0
for (s, k), o in keyed(N).items():
    t = PAL
    n = N.u32(o); q = N.ptr(o + 8)
    f = [N.u32(q + 4 * k) for k in range(3 * n)]
    if (s, k) in b3:
        m = B.u32(b3[(s, k)]); r = B.ptr(b3[(s, k)] + 4)
        assert f == [B.u32(r + 4 * k) for k in range(3 * m)], hex(s)
        n_ok += 1
    assert N.u32(o + 4) == 0 and N.u32(o - 4) == PAL
print('palettes equal to PS3 1.19:', n_ok)
# non-palette symbols' value objects unchanged
print('OK')
