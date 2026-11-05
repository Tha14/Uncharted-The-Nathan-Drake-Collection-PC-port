import json,struct,sys,difflib
sys.path.insert(0,'{{PAKTOOLS}}')
from dc import DC,sid
G='{{PS4}}/u3data/build/main/dc1/ps4/render-settings/game/'
c=DC(G+'airport.bin');c3=DC('rsdc/ps3-airport.bin')
Sch=json.load(open('rs_schema.json'));K={}
for s,k,f,of in Sch:
    for o in of:K.setdefault(o,s+'/'+k)
for n in ['*airport-rs-coop-dawn-igc-intro*','*airport-rs-coop-dawn-stage*','*airport-rs-coop-dawn*']:
    t,o=c.sym(n);t3,o3=c3.sym(n)
    a=[struct.unpack_from('<I',c.d,o+i)[0] for i in range(0,3168,4)]
    b=[struct.unpack_from('>I',c3.d,o3+i)[0] for i in range(0,3072,4)]
    sm=difflib.SequenceMatcher(None,a,b,autojunk=False)
    print(n,'ratio',round(sm.ratio(),4))
    for tag,i1,i2,j1,j2 in sm.get_opcodes():
        if tag=='equal': continue
        if i2-i1>12 or j2-j1>12: print(' ',tag,hex(i1*4),hex(i2*4),hex(j1*4),hex(j2*4));continue
        print(' ',tag,[ (hex(i*4),K.get(i*4,''),round(struct.unpack('<f',struct.pack('<I',a[i]))[0],3)) for i in range(i1,i2)],'ps3',[round(struct.unpack('<f',struct.pack('<I',b[j]))[0],3) for j in range(j1,j2)])
