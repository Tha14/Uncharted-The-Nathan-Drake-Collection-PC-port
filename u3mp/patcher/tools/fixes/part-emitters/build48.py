"""batch48: the PS3 1.19 emitters (converted with emitconv) + the groups' 1.19 emitter lists, appended to the PS4 dc2
part modules. Retail objects are left in place; only the group symbols are re-pointed to new group objects.
part-emitters copy of tools/build48.py: same algorithm; sources are bytes, the tables are tsv files (mattab.tsv,
emitter-names.tsv) instead of pickles, and part-ruins-multi is left out (the reference overlay never installed it)."""
import os, sys, struct, collections
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import dc2lib, emitconv
from dc import sid as sidof
from epairs import M3, ps3_inputs

GROUPS = {'part-cave-multi': [0xd030cb45, 0xcab6690c], 'part-gompa-multi': [0x69bfe094, 0xb534e0a7, 0x6d7d304e, 0x54a60c10],
          'part-shambhala-multi': [0xde6b6dd9]}
# not built: 'part-ruins-multi': [0xde6b6dd9] (batch48 made it, the reference overlay never installed it: "ruins pending")


def read_tsv(name):
    for line in open(os.path.join(HERE, name), encoding='utf-8'):
        if line.strip() and not line.startswith('#'): yield line.rstrip('\n').split('\t')


def load_mattab():
    mat = collections.OrderedDict()
    for k, v, n in read_tsv('mattab.tsv'): mat.setdefault(k, collections.OrderedDict())[v] = int(n)
    return mat


def load_names():
    return {int(s, 16): n for s, n in read_tsv('emitter-names.tsv')}


def build(mod, base_bytes, ps3_bytes, names, mat, log):
    """mod: module name; base_bytes: the PS4 module to append to (retail, or batch45's gompa); ps3_bytes: the PS3 1.19
    dc1 module. Returns the new dc2 bytes."""
    c = dc2lib.load(base_bytes)
    d = bytearray(c.d[:c.reloc]); ptrs = {p for p in c.ptrs if p >= c.data0}; syms = list(c.syms)
    a = M3(ps3_bytes)
    have = {s for s, _, _ in syms}
    def align(n, extra=0):
        while (len(d) + extra) % n: d.append(0)
    def add_str(s):
        o = len(d); d.extend(s.encode() + b'\0'); return o
    want = set()
    for g in GROUPS[mod]:
        o3 = a.c.bysid[g][1]; x = a.c.ptr(o3 + 0x18); n = a.c.u32(o3 + 0xc)
        want |= {a.c.u32(x + 4 * i) for i in range(n)}
    new_emitters = sorted(s for s in want if s not in have)
    for s in new_emitters:
        o3 = a.c.bysid[s][1]
        r3, s3, t11, t12, nested = ps3_inputs(a, o3)
        if s not in names: raise KeyError('emitter %08x of %s has no name in emitter-names.tsv' % (s, mod))
        name = names[s]; assert sidof(name) == s, (hex(s), name)
        key = r3[0x34:0x38].hex()
        if key not in mat: raise KeyError('PS3 material %s of emitter %s is not in mattab.tsv' % (key, name))
        m4 = int(max(mat[key].items(), key=lambda kv: kv[1])[0], 16)
        root, objs, n13, nested, _ = emitconv.convert(r3, s3, t11, t12, nested, m4, s, name)
        name_addr = add_str(name)
        align(16, 8); d.extend(b'\0' * 4 + struct.pack('<I', emitconv.E_TYPE))
        base = len(d)
        blob, bp = emitconv.layout(base, root, objs, nested, name_addr, (r3,))
        d.extend(blob)
        for q in bp: ptrs.add(base + q)
        syms.append((s, emitconv.E_TYPE, base))
        log.append('%s emitter %08x %s: %d bytes, size field %#x, mat %08x, nested %d' % (mod, s, name, len(blob), struct.unpack_from('<I', blob, 0)[0], m4, len(nested)))
    # groups: copy the retail PS4 group root (0x58) and give it the 1.19 count/list
    symd = {s: i for i, (s, t, o) in enumerate(syms)}
    for g in GROUPS[mod]:
        t, og = c.bysid[g]
        o3 = a.c.bysid[g][1]; x = a.c.ptr(o3 + 0x18); n = a.c.u32(o3 + 0xc)
        lst = [a.c.u32(x + 4 * i) for i in range(n)]
        for e in lst: assert e in {s for s, _, _ in syms}, hex(e)
        rootb = bytearray(c.d[og:og + 0x58])
        assert c.u32(og + 0x54) == 0x05dcdf85 and c.u32(og + 0x58) == 1
        align(16, 8); d.extend(b'\0' * 4 + struct.pack('<I', 0x77589471))
        gb = len(d); d.extend(rootb)
        struct.pack_into('<I', d, gb + 0x10, n)
        A = len(d); d.extend(struct.pack('<II', 1, 0) + bytes(16))
        L = len(d); d.extend(b''.join(struct.pack('<I', e) for e in lst)); align(8)
        Z = len(d); d.extend(bytes(8 * n))
        X = len(d); d.extend(bytes(8)); Y = len(d); d.extend(bytes(8))
        for slot, tgt in ((gb + 0x20, A), (gb + 0x28, L), (gb + 0x38, L), (gb + 0x40, Z), (A + 8, X), (A + 0x10, Y)):
            struct.pack_into('<Q', d, slot, tgt); ptrs.add(slot)
        ptrs.add(gb)  # name pointer copied from retail (absolute offset into the same module)
        syms[symd[g]] = (g, t, gb)
        log.append('%s group %08x: count %d -> %d, list %s' % (mod, g, c.u32(og + 0x10), n, ' '.join('%08x' % e for e in lst)))
    return dc2lib.build(d, ptrs, syms, len(c.syms))
