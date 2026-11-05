"""Shared helpers: PS3 1.19 unlock entries -> PS4 *unlock-list* (0xe2e8998e), previous agent's rules."""
import struct
UNLOCK, COND_TYPE, DLC_ARRAY_SIZE = 0xe2e8998e, 0x6aaa1453, 0x100

def ps3_unlocks(b):
    t, o = b.bysid[UNLOCK]; n = b.u32(o); a = b.ptr(o + 4); r = []
    for i in range(n):
        q = a + i * 0x1c; cp = b.ptr(q + 0x14); assert b.u32(cp - 4) == COND_TYPE
        cn = b.u32(cp); cq = b.ptr(cp + 4)
        r.append(dict(i=i, off=q, id=b.u32(q), cat=b.u32(q + 4), sid=b.u32(q + 8), price=b.u32(q + 12),
                      f10=b.u32(q + 0x10), f18=b.u32(q + 0x18),
                      conds=[tuple(b.u32(cq + 12 * k + j) for j in (0, 4, 8)) for k in range(cn)]))
    return r

def ps4_keys(a):
    t, o = a.bysid[UNLOCK]; n = a.u32(o); arr = a.ptr(o + 8)
    return o, n, arr, {(a.u32(arr + i * 0x28 + 4), a.u32(arr + i * 0x28 + 8), a.u32(arr + i * 0x28)) for i in range(n)}

def dlc_blocked(y):
    return [c for c in y['conds'] if c[0] == 6 and c[2] >= DLC_ARRAY_SIZE]

def insert(a, e, add, cat):
    """insert converted entries after the last PS4 entry of category `cat` (their PS3 position)"""
    o4, n4, arr4, _ = ps4_keys(a)
    last = max(i for i in range(n4) if a.u32(arr4 + i * 0x28 + 4) == cat)
    order = list(range(last + 1)) + [('new', k) for k in range(len(add))] + list(range(last + 1, n4))
    na = e.alloc(0x28 * len(order))
    out = []
    for slot, src in enumerate(order):
        dst = na + slot * 0x28
        if isinstance(src, int):
            e.copy_words(arr4 + src * 0x28, dst, 0x28); continue
        y = add[src[1]]
        h = e.alloc(0x18, 8); struct.pack_into('<II', e.d, h, 0, COND_TYPE); blk = h + 8
        items = e.alloc(12 * max(1, len(y['conds'])), 8)
        for k, c in enumerate(y['conds']): struct.pack_into('<III', e.d, items + 12 * k, *c)
        struct.pack_into('<I', e.d, blk, len(y['conds'])); e.putptr(blk + 8, items)
        f20 = y['f18'] if y['f18'] < 100 else 0
        struct.pack_into('<IIIIII', e.d, dst, y['id'], y['cat'], y['sid'], y['price'], y['f10'], 0)
        e.putptr(dst + 0x18, blk); struct.pack_into('<II', e.d, dst + 0x20, f20, 0)
        out.append(dict(index=slot, id='%#x' % y['id'], cat=y['cat'], sid='%08x' % y['sid'], price=y['price'],
                        f10=y['f10'], f20=f20, conds=y['conds'], ps3_index=y['i']))
    e.put32(o4, len(order)); e.putptr(o4 + 8, na)
    return out, last, (o4, 4), (o4 + 8, 8)

def insert_multi(a, e, adds):
    """adds: {cat: [ps3 entries]}; each group goes after the last PS4 entry of its category.
    `a` must describe the input file (table read from it; source bytes are original)."""
    o4, n4, arr4, _ = ps4_keys(a)
    pos = {}
    for cat, lst in adds.items():
        if not lst: continue
        last = max(i for i in range(n4) if a.u32(arr4 + i * 0x28 + 4) == cat)
        pos.setdefault(last, []).extend(lst)
    order = []
    for i in range(n4):
        order.append(i)
        for y in pos.get(i, []): order.append(('new', y))
    na = e.alloc(0x28 * len(order))
    out = []
    for slot, src in enumerate(order):
        dst = na + slot * 0x28
        if isinstance(src, int):
            e.copy_words(arr4 + src * 0x28, dst, 0x28); continue
        y = src[1]
        h = e.alloc(0x18, 8); struct.pack_into('<II', e.d, h, 0, COND_TYPE); blk = h + 8
        items = e.alloc(12 * max(1, len(y['conds'])), 8)
        for k, c in enumerate(y['conds']): struct.pack_into('<III', e.d, items + 12 * k, *c)
        struct.pack_into('<I', e.d, blk, len(y['conds'])); e.putptr(blk + 8, items)
        f20 = y['f18'] if y['f18'] < 100 else 0
        struct.pack_into('<IIIIII', e.d, dst, y['id'], y['cat'], y['sid'], y['price'], y['f10'], 0)
        e.putptr(dst + 0x18, blk); struct.pack_into('<II', e.d, dst + 0x20, f20, 0)
        out.append(dict(index=slot, id='%#x' % y['id'], cat=y['cat'], sid='%08x' % y['sid'], price=y['price'],
                        f10=y['f10'], f20=f20, conds=y['conds'], ps3_index=y['i']))
    e.put32(o4, len(order)); e.putptr(o4 + 8, na)
    return out, {k: v for k, v in ((i, len(l)) for i, l in pos.items())}
