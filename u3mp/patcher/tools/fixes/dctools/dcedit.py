"""Append-only editor for PS4 DC modules ('00CD', LE, 64-bit pointers).

Original bytes [0, reloc) are kept; new objects are appended after them (before the
relocation bitmap). Pointers are file offsets; every pointer slot needs its bit in the
LSB-first relocation bitmap (one bit per 8-byte word)."""
import struct


class DCEdit:
    def __init__(self, path):
        d = open(path, 'rb').read()
        assert d[:4] == b'00CD', 'not a PS4 DC'
        self.orig = d
        self.reloc = struct.unpack_from('<Q', d, 8)[0]
        n = struct.unpack_from('<I', d, self.reloc)[0]
        bm = d[self.reloc + 4:self.reloc + 4 + n]
        assert self.reloc + 4 + n == len(d), 'unexpected data after relocation bitmap'
        self.ptrs = set()
        for bi, b in enumerate(bm):
            if b:
                for k in range(8):
                    if b >> k & 1:
                        self.ptrs.add((bi * 8 + k) * 8)
        self.d = bytearray(d[:self.reloc])
        self.patched = []          # (offset, length) of in-place changes to original bytes

    # ---- raw access
    def u32(self, o): return struct.unpack_from('<I', self.d, o)[0]
    def u64(self, o): return struct.unpack_from('<Q', self.d, o)[0]

    def put32(self, o, v):
        if o < self.reloc:
            self.patched.append((o, 4))
        struct.pack_into('<I', self.d, o, v & 0xffffffff)

    def putptr(self, o, target):
        assert o % 8 == 0
        if o < self.reloc:
            self.patched.append((o, 8))
        struct.pack_into('<Q', self.d, o, target)
        self.ptrs.add(o)

    # ---- appending
    def align(self, a):
        while len(self.d) % a:
            self.d.append(0)

    def alloc(self, size, align=16):
        self.align(align)
        o = len(self.d)
        self.d += bytes(size)
        return o

    def add_string(self, s):
        o = len(self.d)
        self.d += s.encode('latin1') + b'\0'
        return o

    def copy_words(self, src, dst, size):
        """copy size bytes of original data src -> dst, carrying pointer bits (8-aligned)"""
        assert src % 8 == 0 and dst % 8 == 0 and size % 8 == 0
        self.d[dst:dst + size] = self.orig[src:src + size]
        for k in range(0, size, 8):
            if src + k in self.ptrs:
                self.ptrs.add(dst + k)

    # ---- output
    def build(self):
        self.align(16)
        reloc = len(self.d)
        nbytes = (reloc + 63) // 64
        bm = bytearray(nbytes)
        for p in self.ptrs:
            assert p < reloc and p % 8 == 0
            w = p // 8
            bm[w // 8] |= 1 << (w % 8)
        out = bytearray(self.d)
        struct.pack_into('<Q', out, 8, reloc)
        out += struct.pack('<I', nbytes) + bm
        return bytes(out)
