from common import *
def rows(c, S, es):
    t, o = c.bysid[S]; n = c.u32(o); a = c.ptr(o + c.ps)
    return [a + i * es for i in range(n)]
def derive(c3, c4, S, es3, es4, key=0):
    r3 = rows(c3, S, es3); r4 = rows(c4, S, es4)
    k3 = {c3.u32(e + key): e for e in r3}; com = [(k3[c4.u32(e + key)], e) for e in r4 if c4.u32(e + key) in k3]
    out = []
    for k in range(0, es4, 4):
        isptr = any(c4.isptr(e4 + k) for _, e4 in com)
        if isptr:
            # match to PS3 ptr offset with equal strings
            best = None
            for j in range(0, es3, 4):
                if all((c3.isptr(e3 + j) == c4.isptr(e4 + k)) and (not c3.isptr(e3 + j) or c3.cstr(c3.ptr(e3 + j)) == c4.cstr(c4.ptr(e4 + k)) or not (c3.cstr(c3.ptr(e3+j)) or '').isprintable()) for e3, e4 in com):
                    best = j; break
            out.append((k, 'PTR', best)); continue
        col4 = [c4.u32(e4 + k) for _, e4 in com]
        if not any(col4): out.append((k, 'zero', None)); continue
        best = max(((sum(1 for (e3, e4) in com if c3.u32(e3 + j) == c4.u32(e4 + k)), j) for j in range(0, es3, 4)))
        out.append((k, '%d/%d' % (best[0], len(com)), best[1]))
    return out, len(com)
