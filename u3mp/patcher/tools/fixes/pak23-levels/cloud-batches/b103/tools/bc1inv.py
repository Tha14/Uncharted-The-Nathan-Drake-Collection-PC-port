"""bc1inv: invert a BC1 (DXT1) texel blob value-wise (v -> 1 - v per channel), block by block.
Works on tiled or linear data alike (the 8-byte blocks stay in place).  4-colour blocks (c0 > c1) are inverted exactly:
c0' = ~c1, c1' = ~c0 (565 bitwise NOT = 1 - value per channel) and every 2-bit index XOR 1.  3-colour blocks (c0 <= c1)
are re-fitted: their decoded colours are inverted and re-encoded as a 4-colour block (index 3 = transparent black has no
inverse; it becomes white)."""
import struct
def _d565(c): return ((c>>11)&31)*255//31,((c>>5)&63)*255//63,(c&31)*255//31
def _e565(r,g,b): return ((r*31+127)//255)<<11|((g*63+127)//255)<<5|((b*31+127)//255)
def invert(blob):
    out=bytearray(blob)
    for o in range(0,len(blob)-7,8):
        c0,c1,ix=struct.unpack_from('<HHI',blob,o)
        if c0>c1:
            struct.pack_into('<HHI',out,o,c1^0xffff,c0^0xffff,ix^0x55555555)
        else:
            p0,p1=_d565(c0),_d565(c1)
            pal=[p0,p1,tuple((a+b)//2 for a,b in zip(p0,p1)),(0,0,0)]
            inv=[tuple(255-v for v in c) for c in pal]
            px=[inv[(ix>>(2*k))&3] for k in range(16)]
            lo=tuple(min(c[j] for c in px) for j in range(3));hi=tuple(max(c[j] for c in px) for j in range(3))
            e0,e1=_e565(*hi),_e565(*lo)
            if e0==e1:
                struct.pack_into('<HHI',out,o,e0,e1,0);continue
            if e0<e1: e0,e1=e1,e0
            q=[_d565(e0),_d565(e1)];q+= [tuple((2*a+b)//3 for a,b in zip(q[0],q[1])),tuple((a+2*b)//3 for a,b in zip(q[0],q[1]))]
            ni=0
            for k,c in enumerate(px):
                best=min(range(4),key=lambda i:sum((c[j]-q[i][j])**2 for j in range(3)));ni|=best<<(2*k)
            struct.pack_into('<HHI',out,o,e0,e1,ni)
    return bytes(out)
