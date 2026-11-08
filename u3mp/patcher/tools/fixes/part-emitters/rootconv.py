"""PS3 emitter root (0x16c) -> PS4 root (0x1e0) words. Rules: root4rules.tsv (PS4 offset, kind, source, pairs seen):
const = a fixed 4-byte value (hex), sw = PS3 word at source offset byte-swapped, id = copied as is, h16 = both u16 halves
swapped. Learned from the 1824 PS3/PS4 emitter pairs of the 33 MP modules (was root4rules.pkl; same content)."""
import os, struct
_T = os.path.dirname(os.path.abspath(__file__))
def _load_rules(p):
    rules = {}
    for line in open(p, encoding='utf-8'):
        if not line.strip() or line.startswith('#'): continue
        off, kind, src, n = line.rstrip('\n').split('\t')
        rules[int(off, 16)] = (kind, bytes.fromhex(src) if kind == 'const' else int(src, 16), int(n))
    return rules
RULES = _load_rules(os.path.join(_T, 'root4rules.tsv'))
PTR4={0x18,0xa8,0xb0,0xb8,0xc0,0xc8,0xd0,0xd8,0xe0,0xe8,0xf0,0xf8,0x1a8}
def u32b(b,o): return struct.unpack_from('>I',b,o)[0]
def bf28(x): return (x&0x7fff)|((x&~0x7fff)<<2)
def bf70(x): return ((x>>1)<<2)|(x&1)
def root_words(r3, counts, mat=None):
    """r3: PS3 root bytes (0x16c). counts: dict n4,n5,n6,n8,n9. returns dict PS4 offset -> 4 bytes (pointer slots/size/name left out)"""
    out={}
    for j,(kind,src,n) in RULES.items():
        if kind=='const': out[j]=src
        elif kind=='sw': out[j]=r3[src:src+4][::-1]
        elif kind=='id': out[j]=r3[src:src+4]
        elif kind=='h16': w=r3[src:src+4]; out[j]=w[1::-1]+w[3:1:-1]
    out[0x040]=struct.pack('<I',bf28(u32b(r3,0x28)))
    out[0x04c]=bytes([r3[0x30],2,0,0])
    if mat is not None: out[0x050]=struct.pack('<I',mat)
    w=u32b(r3,0x70); out[0x100]=struct.pack('<HH',bf70(w>>16),w&0xffff)
    out[0x150]=bytes([r3[0xb1],r3[0xb2],0,0])
    out[0x098]=struct.pack('<HH',counts['n4'],counts['n5'])
    out[0x09c]=struct.pack('<HBB',counts['n6'],r3[0x12c],r3[0x130])
    out[0x0a0]=struct.pack('<HH',0,counts['n8'])
    out[0x0a4]=struct.pack('<HH',counts['n8'],counts['n9'])
    return out
