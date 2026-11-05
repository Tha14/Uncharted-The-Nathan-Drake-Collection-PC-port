import sys,struct
sys.path.insert(0,"{{PAKTOOLS}}");sys.dont_write_bytecode=True
from ndpak import Pak,sid
def dump(path,filt):
    p=Pak(path);raw=p.d
    print('==',path.split('/')[-1],len(raw))
    for it in p.items_of('SPAWNER_GROUP'):
        a=(it.page,it.data_off);bb=p.pbase(it.page)
        arr=p.resolve(it.page,it.data_off+8);n=p.pu32(it.page,it.data_off+0x10)
        print(' group',it.name,'n',n)
        for k in range(n):
            s=p.resolve(arr[0],arr[1]+8*k);sb=p.pbase(s[0])+s[1]
            nm=p.resolve(s[0],s[1]+0x20);nm=p.pstr(*nm) if nm else None
            art=p.resolve(s[0],s[1]+0x48);artn=p.pstr(*art) if art else None
            if filt and not any(f in (artn or '') or f in (nm or '') for f in filt):continue
            pos=struct.unpack_from('<4f',raw,sb);rot=struct.unpack_from('<4f',raw,sb+16)
            ty=p.u32(sb+0x40);typ=p.u32(sb+0x50)
            print('  sp',k,'name',nm,'sid %08x'%p.u32(sb+0x28),'type %08x'%ty,'art',artn,'at file+%#x artstr file+%#x'%(sb,p.pbase(art[0])+art[1]),'pos',[round(v,3) for v in pos],'rot',[round(v,3) for v in rot],'+0x50 %08x'%typ)
            tg=p.resolve(s[0],s[1]+0x60)
            if tg:
                tb=p.resolve(tg[0],tg[1]);nt=p.pu32(tg[0],tg[1]+8)
                ents=[]
                for j in range(nt):
                    key=p.pu32(tb[0],tb[1]+16*j);v=p.resolve(tb[0],tb[1]+16*j+8)
                    if v:
                        ty_=p.u16(p.pbase(v[0])+v[1]);d=p.resolve(v[0],v[1]+8)
                        dd=raw[p.pbase(d[0])+d[1]:p.pbase(d[0])+d[1]+8].hex() if d else None
                        if ty_==13 and d: dd=p.pstr(*d) if False else dd
                        ents.append(('%08x'%key,ty_,dd))
                print('     tags',ents)
for a in sys.argv[1:]:
    path,_,f=a.partition('::');dump(path,f.split(',') if f else None)
