"""windoff.py <paktools> <in.pak> <out.pak> [material substring ...]
Zero, in place, the g_pivotWind* strength constants (g_pivotWindStrength, g_pivotWindSubobjBendStrength,
g_pivotWindSubobjVarStrength, g_pivotWindPlayerBendIntensity) of the GEOMETRY_1 materials whose name contains one of the
substrings (default: the City of Brass reeds and grass-shrubs: 'oas-reeds', 'grass-shrub'); speed / radius / position
stay. No layout change. Re-creation of agent P's agm/windoff.py (2026-10-29: PS3 1.19 has no g_pivotWind* on these
materials; the script itself is lost), verified on the retail PS4 san-upper-multi-stage.pak against the overlay-r3
reference (scratch/pak23-recovery/no-t0/windoff_re.py)."""
import struct
import sys

PT, src, dst = sys.argv[1:4]
subs = sys.argv[4:] or ['oas-reeds', 'grass-shrub']
sys.path.insert(0, PT)
from ndpak import Pak  # noqa: E402

ZERO = {'g_pivotWindStrength', 'g_pivotWindSubobjBendStrength', 'g_pivotWindSubobjVarStrength', 'g_pivotWindPlayerBendIntensity'}
q = Pak(src)
d = bytearray(q.d)


def S(a, o):
    r = q.resolve(a, o)
    return q.pstr(*r) if r else ''


g = q.items_of('GEOMETRY_1')[0]
pg, b = g.page, g.data_off
n = q.u64(q.pbase(pg) + b + 0x48)
mp, mo = q.resolve(pg, b + 0x50)
done = 0
for i in range(n):
    o = mo + 0x48 * i
    name = S(mp, o)
    if not any(s in name for s in subs):
        continue
    npar = struct.unpack_from('<I', q.d, q.pbase(mp) + o + 0x34)[0]
    pr = q.resolve(mp, o + 0x20)
    for k in range(npar if pr else 0):
        eo = pr[1] + 0x18 * k
        if S(pr[0], eo) not in ZERO:
            continue
        for slot in (8, 0x10):
            if q.is_ptr(pr[0], eo + slot):
                vp = q.resolve(pr[0], eo + slot)
                struct.pack_into('<f', d, q.pbase(vp[0]) + vp[1], 0.0)
                done += 1
                break
open(dst, 'wb').write(d)
print('%s: %d wind strength constants zeroed' % (dst, done))
