#!/usr/bin/env python3
"""Find every Havok 2011.3 hkpBvCompressedMeshShape packfile inside a PS4 pak and check / fix the section
shared-vertex word.

Runtime rule (section accessor 0x12fba70, vertex fetch 0x12fbc90): for section s,
  numPacked = low byte of u32 +0x4c; packed vertex k (k < numPacked) = packedVertices[+0x48 + k];
  shared vertex k (k >= numPacked) = sharedVertices[svi[(word +0x4c >> 8) - numPacked + k]].
+0x58 (u8) repeats numPacked.  collision.py before 2026-10-26 23:00 wrote the shared-vertex COUNT into the
low byte (the vertex fetch then read packedVertices through a null pointer: Cave crash at 0x12fbd17).
The fix sets the low byte to the +0x58 byte; retail packfiles already satisfy that (no-op).
usage: bvfix.py check|fix <pak> [out]   (check: exit 1 when a packfile breaks the rule)"""
import struct, sys

MAGIC = bytes.fromhex('57e0e05710c0c010')


def packfiles(d):
    """yield (offset, size) of each 2011.3 packfile in d that holds a hkpBvCompressedMeshShape"""
    o = d.find(MAGIC)
    while o >= 0:
        try:
            ver = d[o + 0x28:o + 0x36]
            if ver == b'hk_2011.3.0-r1':
                cn = struct.unpack_from('<I', d, o + 0x40 + 20)[0]
                cnn = struct.unpack_from('<I', d, o + 0x40 + 24)[0]
                names = d[o + cn:o + cn + cnn]
                st, lf, gf, vf, ex = struct.unpack_from('<5I', d, o + 0x40 + 0x30 * 2 + 20)
                if b'hkpBvCompressedMeshShape' in names:
                    yield o, st, lf, gf
        except struct.error:
            pass
        o = d.find(MAGIC, o + 8)


def sections(d, o, st, lf, gf):
    """data offsets (absolute) of every Section record of the root shape (data section starts with it)"""
    D0 = o + st
    loc = {}
    for x in range(o + st + lf, o + st + gf, 8):
        a, t = struct.unpack_from('<II', d, x)
        if a != 0xffffffff:
            loc[a] = t
    sp = loc.get(0x90)
    if sp is None:
        return []
    n = struct.unpack_from('<I', d, D0 + 0x98)[0]
    return [D0 + sp + 0x60 * i for i in range(n)]


def scan(d, fix=False):
    """-> (packfiles, sections, bad sections); with fix the bytearray d is patched in place"""
    npf = ns = nb = 0
    for o, st, lf, gf in list(packfiles(bytes(d))):
        npf += 1
        for so in sections(d, o, st, lf, gf):
            ns += 1
            w = struct.unpack_from('<I', d, so + 0x4c)[0]
            npk = d[so + 0x58]
            if w & 0xff != npk:
                nb += 1
                if fix:
                    struct.pack_into('<I', d, so + 0x4c, (w & 0xffffff00) | npk)
    return npf, ns, nb


if __name__ == '__main__':
    cmd, src = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, 'rb').read())
    npf, ns, nb = scan(d, fix=(cmd == 'fix'))
    print('%s: %d mesh packfiles, %d sections, %d with the bad shared-vertex byte%s' % (
        src, npf, ns, nb, ' (fixed)' if cmd == 'fix' and nb else ''))
    if cmd == 'fix' and len(sys.argv) > 3:
        open(sys.argv[3], 'wb').write(bytes(d))
    sys.exit(1 if (cmd == 'check' and nb) else 0)
