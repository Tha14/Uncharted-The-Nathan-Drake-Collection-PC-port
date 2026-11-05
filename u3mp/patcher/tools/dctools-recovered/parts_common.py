from common import *
PT = 0x2a8027cf
def part_syms(c):
    es = 0x38 if c.ps == 8 else 0x2c; r = {}
    for s, t, o in c.syms:
        if t != PT: continue
        n = c.u32(o); q = c.ptr(o + c.ps)
        r[s] = (o, q, [q + j * es for j in range(n)])
    return r
def entry3(c, e):   # PS3 part entry -> dict
    return dict(sid=c.u32(e), art=c.cstr(c.ptr(e + 4)), f=[c.u32(e + k) for k in range(8, 0x2c, 4)])
def entry4(c, e):
    return dict(sid=c.u32(e), art=c.cstr(c.ptr(e + 8)), f=[c.u32(e + k) for k in (0x10, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34)])
def available():
    av = set(l.strip() for l in open('/tmp/claude-0/-home-claude/528884b1-f615-54ea-bcbe-b8ead4764d56/scratchpad/dc2/avail_actors.txt'))
    c4 = DC(P4R + 'net.bin')
    for s, (o, q, es) in part_syms(c4).items():
        for e in es: av.add(c4.cstr(c4.ptr(e + 8)))
    return av | {'', 'none'}
