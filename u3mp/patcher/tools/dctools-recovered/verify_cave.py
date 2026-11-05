import sys, json
sys.path.insert(0, '.')
from common import DC, sid
from verify_dc import check
base, new, p3, logp = sys.argv[1:5]
A = DC(base); N = DC(new); B = DC(p3)
log = json.load(open(logp))
allowed = []
for S in (0xdec97a4, 0x3b9a067d):
    t, o = A.bysid[S]; allowed += [(o, 4), (o + 8, 8)]
for p in log['playlists']:
    o = int(p['list_obj'], 16); allowed += [(o, 4), (o + 8, 8)]
check(base, new, allowed)
def tab(c, S, es):
    t, o = c.bysid[S]; n = c.u32(o); a = c.ptr(o + c.ps); return n, a
# maps: retail prefix identical incl. pointers
for S, es in ((0xdec97a4, 0x98), (0x3b9a067d, 0xd0)):
    n0, a0 = tab(A, S, es); n1, a1 = tab(N, S, es)
    for k in range(0, n0 * es, 8):
        assert A.d[a0 + k:a0 + k + 8] == N.d[a1 + k:a1 + k + 8] and ((a0 + k) in A.ptrs) == ((a1 + k) in N.ptrs)
        if (a0 + k) in A.ptrs: assert A.ptr(a0 + k) == N.ptr(a1 + k)
# cave map strings / sids
n1, a1 = tab(N, 0xdec97a4, 0x98); m = a1 + 27 * 0x98
s = lambda o: N.cstr(N.ptr(o))
assert N.u32(m) == 0x3869aa4e and s(m + 0x10) == 'net-levels/ice-cave' and N.ptr(m + 0x18) == 0
assert s(m + 0x28) == 'cave-multi-comp-pak' and N.u32(m + 0x30) == sid('cave-multi-comp-pak')
assert s(m + 0x38) == 'cave-multi-killvis' and N.u32(m + 0x40) == sid('cave-multi-killvis')
assert s(m + 0x48) == '' and s(m + 0x70) == 'world-multiplayer' and N.u32(m + 0x78) == sid('world-multiplayer')
assert s(m + 0x80) == 'cave-multi-stage' and N.u32(m + 0x88) == sid('cave-multi-stage') and N.f32(m + 0x90) == 1.0
l = N.ptr(m + 0x58); assert N.u32(l - 4) == 0xd14c6ffc
it = N.ptr(l + 8); subs = [s(it + 16 * k) for k in range(N.u32(l))]
assert subs == log['map']['sublevels'] and all(N.u32(it + 16 * k + 8) == sid(x) for k, x in enumerate(subs))
# netgames converted equal PS3 by the mapping
n3, a3 = tab(B, 0x3b9a067d, 0xe4); i3 = {B.u32(a3 + i * 0xe4): a3 + i * 0xe4 for i in range(n3)}
n1, a1 = tab(N, 0x3b9a067d, 0xd0)
MAP = [(k, k) for k in range(0, 0x48, 4)] + [(0x50, 0x58), (0x54, 0x5c), (0x58, 0x60), (0x5c, 0x64), (0x60, 0x68), (0x64, 0x6c), (0x78, 0x78), (0x7c, 0x7c)] + [(k, k + 4) for k in range(0x84, 0xbc, 4)]
for j in range(89, n1):
    g = a1 + j * 0xd0; src = i3[N.u32(g)]
    for k3, k4 in MAP: assert B.u32(src + k3) == N.u32(g + k4), (hex(N.u32(g)), hex(k3))
    ptrs = sorted(p - g for p in N.ptrs if g <= p < g + 0xd0); assert ptrs == [0x70], ptrs
    t = N.ptr(g + 0x70); t3 = B.ptr(src + 0x74); assert N.u32(t - 4) == 0xf0670de8
    for k in range(3):
        r4 = N.ptr(t + 8 * k); r3 = B.ptr(t3 + 4 * k)
        assert N.u32(r4 - 4) == 0x8b617b51 and [N.u32(r4 + 4 * w) for w in range(9)] == [B.u32(r3 + 4 * w) for w in range(9)]
# playlists: prefix unchanged, added items appended
t, o = N.bysid[0xc1b0be0d]; a = N.ptr(o + 8)
for p in log['playlists']:
    lo = int(p['list_obj'], 16); n = N.u32(lo); q = N.ptr(lo + 8); q0 = A.ptr(lo + 8)
    assert n == p['old_count'] + len(p['added'])
    assert N.d[q:q + 8 * p['old_count']] == A.d[q0:q0 + 8 * p['old_count']]
    assert [('%08x' % N.u32(q + 8 * (p['old_count'] + k)), N.u32(q + 8 * (p['old_count'] + k) + 4)) for k in range(len(p['added']))] == [tuple(x) for x in p['added']]
print('cave net.bin OK')
