"""PS3 1.19 -> PS4 particle emitter (DC type 0x4fec6221) converter.
Rules derived from the 1824 PS3/PS4 emitter pairs of the 33 MP part modules (see batch48 README):
 root 0x16c -> 0x1e0 (rootconv), 14 PS3 sub-objects -> 11 PS4 sub-objects:
  s0  u16 list (h16), 16 bytes          <- PS3 s0      (present when PS3 +0x120 hi16 != 0... see below)
  s1  float array, 24 words per unit    <- PS3 s1      count PS3 +0x122
  s2  {u8,u8,u16} entries               <- PS3 s2      count PS3 +0x124
  s3  4-float entries                   <- PS3 s3      count PS3 +0x126
  s4  bytecode, opcodes remapped        <- PS3 s8 (+0x12d instr) + s9 (+0x12e instr)
  s5  float array                       <- PS3 s10     count PS3 +0x12f
  s6  parameter table (ptconv)          <- PS3 s6      count PS3 +0x12b
  s7  {sid,int} pairs                   <- PS3 s7      count PS3 +0x12c
  s8  typed struct (donormap per type)  <- PS3 s11
  s9  typed 0x869bc537 struct + 16 const bytes <- PS3 s12
  s10 pointer list -> typed nested structs (sw, word1 bytes) <- PS3 s13, count PS3 +0x130
 PS3 s4 / s5 have no PS4 counterpart."""
import struct, os, sys
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import rootconv, ptconv, bcconv
from donormap import DonorMap
E_TYPE = 0x4fec6221
S9_TAIL = bytes.fromhex('e131ea00000000040000000000000000')
S11_TYPES = (0x1d9d708e, 0xaee75cf5)
_s11 = None
def set_s11_samples(samples):
    """part-emitters: the s11 -> s8 samples (PS3/PS4 word lists per struct type) are learned on the player's PC from
    the PS3/PS4 MP module pairs (epairs.s11_samples) instead of the bundled s11map.pkl, which held copies of game data."""
    global _s11
    _s11 = {ty: DonorMap(sm) for ty, sm in samples.items() if ty in S11_TYPES}
def s11maps():
    if _s11 is None: raise RuntimeError('call emitconv.set_s11_samples() first')
    return _s11
def sw(b): return b''.join(b[i:i + 4][::-1] for i in range(0, len(b), 4))
def h16(b): return b''.join(b[i:i + 2][::-1] for i in range(0, len(b), 2))
def u16(b, o): return struct.unpack_from('>H', b, o)[0]

class Obj:
    def __init__(self, kind, data, typ=None, ptrs=None, align=4):
        self.kind, self.data, self.typ, self.ptrs, self.align = kind, bytearray(data), typ, ptrs or [], align
        self.addr = None

def convert(r3, s3, s11type, s12type, nested, mat4, sid, name):
    """r3: PS3 root bytes (0x16c); s3: list of 14 PS3 sub-object byte strings; nested: [(type, bytes)];
    mat4: (PS4 material sid, PS4 +0x5c low bits) ; returns list of Obj (root first) and fix-ups."""
    n_s0 = u16(r3, 0x120); n_s1 = u16(r3, 0x122); n_s2 = u16(r3, 0x124); n_s3 = u16(r3, 0x126)
    n6 = r3[0x12b]; n7 = r3[0x12c]; n8 = r3[0x12d]; n9 = r3[0x12e]; n5 = r3[0x12f]; n13 = r3[0x130]
    if any(r3[0x58:0x5c]): raise ValueError('PS3 +0x58 set: PS4 extra-pointer variant not supported')
    objs = {}
    objs[0] = (h16(s3[0]) + bytes(16))[:16] if n_s0 else b''
    objs[1] = sw(s3[1][:96 * n_s1])
    objs[2] = b''.join(e[:2] + e[2:4][::-1] for e in (s3[2][i:i + 4] for i in range(0, 4 * n_s2, 4)))
    objs[3] = sw(s3[3][:16 * n_s3])
    objs[4] = bcconv.conv(s3[8], n8, s3[9], n9)
    objs[5] = sw(s3[10][:4 * n5])
    empty = (n8 == 0 and n9 == 0)
    tab = ptconv.conv(ptconv.parse3(s3[6], n6), empty)
    objs[6] = ptconv.emit4(tab)
    objs[7] = sw(s3[7][:8 * n7])
    # s8: typed struct
    dm = s11maps()[s11type]
    w3 = [s3[11][i:i + 4] for i in range(0, len(dm.samples[0][0]) * 4, 4)]
    w4, diff = dm.convert(w3)
    if diff: raise ValueError('s11 donor not exact (%d words differ)' % diff)
    objs[8] = b''.join(w4)
    if s12type != 0x869bc537: raise ValueError('s12 type %08x' % s12type)
    objs[9] = sw(s3[12][:48]) + S9_TAIL
    objs['t8'] = s11type
    # root
    present = {c[2] for v, c in tab}
    mask = sum(1 << b for b, p in ((0, 0x15), (1, 0x16), (2, 0x14), (3, 0x05), (4, 0x06)) if p in present)
    counts = dict(n4=n8 + n9, n5=n5, n6=len(tab), n8=n8, n9=n9)
    words = rootconv.root_words(r3, counts, mat4)
    words[0x05c] = struct.pack('<I', mask << 16)
    root = bytearray(0x1e0)
    for j, v in words.items(): root[j:j + 4] = v
    struct.pack_into('<I', root, 0x20, sid)
    return root, objs, n13, nested, name

def layout(base, root, objs, nested, name_addr, ps3size_src=None):
    """Place one emitter at file offset base (16-aligned, the 8-byte header at base-8). Returns (bytes, ptr slots
    relative to base, size) - pointers are absolute file offsets."""
    out = bytearray(root); ptr = []
    def put_ptr(slot, target):
        struct.pack_into('<Q', out, slot, target); ptr.append(slot)
    pos = 0x1e0
    addr = {}
    order = [0, 1, 2, 3, 4, 5, 6, 7]
    for j in order:
        data = objs[j]
        addr[j] = base + pos
        out += data; pos += len(data)
    # typed s8, s9: 16-aligned objects with an 8-byte {0, type} header
    def typed(j, data, ty):
        nonlocal pos, out
        while (pos + 8) % 16: out += b'\0'; pos += 1
        out += b'\0' * 4 + struct.pack('<I', ty); pos += 8
        addr[j] = base + pos; out += data; pos += len(data)
    typed(8, objs[8], objs['t8']); typed(9, objs[9], 0x869bc537)
    # s10: pointer list (at least 8 bytes), 8-aligned
    while pos % 16: out += b'\0'; pos += 1
    addr[10] = base + pos
    n = len(nested)
    out += bytes(max(8 * n, 8)); s10 = pos; pos += max(8 * n, 8)
    naddr = []
    for ty, data in nested:
        while (pos + 8) % 16: out += b'\0'; pos += 1
        out += b'\0' * 4 + struct.pack('<I', ty); pos += 8
        naddr.append(base + pos); out += data; pos += len(data)
    # empty objects point at the next object that has data
    nxt = None
    for j in [10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0]:
        if j in objs and isinstance(objs[j], (bytes, bytearray)) and len(objs[j]) == 0: addr[j] = nxt
        nxt = addr[j]
    for j, slot in enumerate(range(0xa8, 0x100, 8)): put_ptr(slot, addr[j])
    for i, a in enumerate(naddr): put_ptr(s10 + 8 * i, a)
    put_ptr(0x18, name_addr)
    while pos % 16: out += b'\0'; pos += 1
    # root +0 (size): retail values are not a pure layout extent (checked on 1732 pairs). Use a bound that is never
    # below retail on any pair: max(layout end + 8, PS3 size + 0xd0, rounded to 16) - over by <= 0x70 bytes.
    p3 = struct.unpack_from('>I', ps3size_src[0], 4)[0] if ps3size_src else 0
    size = max(pos + 8, (p3 + 0xd0 + 15) & ~15)
    struct.pack_into('<I', out, 0, size)
    return out, ptr
