from common import *
H=0xbd3bb3b9
def hero_rows(c):
    es=0x70 if c.ps==8 else 0x3c
    t,o=c.bysid[H]; n=c.u32(o); a=c.ptr(o+c.ps)
    rows=[]
    for i in range(n):
        e=a+i*es
        if c.ps==8: slot_offs=[0x18+8*k for k in range(7)]
        else: slot_offs=[0x10+4*k for k in range(7)]
        slots=[]
        for so in slot_offs:
            if not c.isptr(e+so): slots.append(None); continue
            sl=c.ptr(e+so)
            if c.u32(sl-4)!=0x88e3fe54: slots.append(('type',hex(c.u32(sl-4)))); continue
            cnt=c.u32(sl)
            q=c.ptr(sl+(8 if c.ps==8 else 4))
            slots.append(dict(obj=sl,parts=[c.u32(q+4*j) for j in range(cnt)]))
        rows.append(dict(i=i,e=e,name=c.cstr(c.ptr(e)),sid=c.u32(e+c.ps),sid2=c.u32(e+c.ps+(8 if c.ps==8 else 4)),slots=slots))
    return rows
def part_list(c,ps):
    if ps not in c.bysid: return None
    t,o=c.bysid[ps]; n=c.u32(o); q=c.ptr(o+c.ps); es=0x38 if c.ps==8 else 0x2c
    return [c.cstr(c.ptr(q+j*es+c.ps)) for j in range(n)]
