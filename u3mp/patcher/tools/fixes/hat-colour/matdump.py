import sys,os,struct,hashlib
sys.dont_write_bytecode=True
sys.path.insert(0,'{{PAKTOOLS}}')
from ndpak import Pak,sid
import ps4actor as A
W='{{PROJECT}}/work'
G='{{PS4}}'
def find(n):
    for l in [W+'/overlay-r3',W+'/overlay-r2',W+'/overlay-port',W+'/overlay-mp',G]:
        f=f'{l}/u3data/build/main/actor33/ps4/{n}.pak'
        if os.path.exists(f): return f
for n in sys.argv[1:]:
    f=find(n) if not os.path.exists(n) else n
    q=Pak(f); print('=====',n,f.split('/')[-6] if 'overlay' in f else 'RETAIL', 'vram %#x'%q.u32(0x20))
    print(' items:',[(i.type,i.name) for i in q.items if i.type not in('RAW_DATA','VRAM_DESC')])
    g=q.items_of('GEOMETRY_1')[0]; pg,b=g.page,g.data_off
    print(' geom hdr', ' '.join('%08x'%x for x in struct.unpack_from('<16I',q.d,q.pbase(pg)+b)))
    ns=q.pu32(pg,b+8); r=q.resolve(pg,b+0x10)
    for i in range(ns):
        o=r[1]+0x88*i
        x=q.resolve(r[0],o+0x48); m=q.pstr(*x) if x else None
        print('  sub',i,q.pstr(*q.resolve(r[0],o+8)),'nv',q.pu32(r[0],o+0x20),'ni',q.pu32(r[0],o+0x24),'mat',m, 'raw', ' '.join('%08x'%v for v in struct.unpack_from('<34I',q.d,q.pbase(r[0])+o)))
    for m in A.tmpl_materials(q):
        print(' MAT',m['name']); print('   shader',m['shader'],'hash',m['hash'],'x3',m['x3'],'tail %#x'%m['tail'])
        print('   feats',m['feats'])
        for p in m['params']: print('   param',p[0],p[1],p[2].hex())
        for bb in m['binds']: print('   bind',bb)
    for t in q.textures():
        print(' TEX %08x'%t['hash'],t['path'],'fmt %#x'%t['format'],t['width'],t['height'],'mips',t['mips'],'unk20 %#x unk30 %#x unk34 %#x'%(t['unk20'],t['unk30'],t['unk34']))
    for e in q.items_of('EFFECT_TABLE'):
        n_=q.pu32(e.page,e.data_off); er=q.resolve(e.page,e.data_off+8)
        print(' EFFECT_TABLE n',n_)
        for k in range(n_):
            eo=er[1]+0x18*k; tgt=q.resolve(er[0],eo)
            print('   fx',k,'idx',q.pu32(er[0],eo+8),'hash %016x'%q.u64(q.pbase(er[0])+eo+0x10), q.item_at(tgt[0],tgt[1]-0x20).name if tgt else None)
