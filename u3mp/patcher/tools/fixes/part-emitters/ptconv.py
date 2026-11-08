import struct
ORDER=[0x00,0x10,0x04,0x12,0x13,0x14,0x15,0x16,0x05,0x06]
NCOMP={0x0:3,0x4:4,0x5:4,0x6:4,0x10:3,0x12:3,0x13:1,0x14:1,0x15:3,0x16:3}
F1=struct.pack('>f',1.0)
DEFAULT={(0x12,2):F1}
def parse3(w3,n=None):
    ents=[]
    for i in range(0,len(w3)-7,8):
        c=w3[i+4:i+8]
        if c[0] not in (1,2,3) or c[2] not in NCOMP or c[3]>=NCOMP[c[2]]: break
        ents.append((w3[i:i+4],c))
        if n is not None and len(ents)>=n: break
    return ents
def parse4(w4):
    ents=[]
    for i in range(0,len(w4)-7,8):
        c=w4[i+4:i+8]
        if c[0] not in (1,2,3) or c[2] not in NCOMP or c[3]>=NCOMP[c[2]]: break
        ents.append((w4[i:i+4][::-1],c))
    return ents
def conv(ents3,empty_prog=False):
    """PS3 entries (value BE bytes, code 4 bytes) -> PS4 entries (value BE bytes, code)"""
    got={}
    for v,c in ents3:
        k=(c[2],c[3])
        if k not in got: got[k]=(v,c)
    if empty_prog: params={0x00,0x12,0x13}; got={}
    else: params={p for p,_ in got}|{0x00,0x10,0x12}
    out=[]
    for p in ORDER:
        if p not in params: continue
        for comp in range(NCOMP[p]):
            if p==0x00: out.append((b'\0\0\0\0',bytes([1,0,0,comp]))); continue
            if (p,comp) in got: out.append(got[(p,comp)])
            else: out.append((DEFAULT.get((p,comp),b'\0\0\0\0'),bytes([1,0,p,comp])))
    return out
def emit4(ents): return b''.join(v[::-1]+c for v,c in ents)
