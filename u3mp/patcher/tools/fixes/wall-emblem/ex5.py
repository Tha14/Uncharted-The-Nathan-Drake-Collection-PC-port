from lib import *
import ps4actor,ps3params as P
q=Pak('tmpl/ps4-loading-menu-cloth.pak')
m=ps4actor.tmpl_materials(q)[0]
print('BINDS');[print(' ',a,b.split('/')[-1]) for a,b,c in m['binds']]
print('PARAMS');[print(' ',a,c,struct.unpack('<4f',d)) for a,c,d in m['params']]
print('x3',m['x3'],'tail',m['tail'],'shader',m['shader'],'hash',m['hash'])
g=q.items_of('GEOMETRY_1')[0];pg,gb=g.page,g.data_off
sr=q.resolve(pg,gb+0x10);ns=q.u64(q.pbase(pg)+gb+8)
print('protos',q.u64(q.pbase(pg)+gb+0x18),'insts',q.u64(q.pbase(pg)+gb+0x28),'tex',q.u64(q.pbase(pg)+gb+0x38))
for i in range(ns):
    o=sr[1]+0x88*i;print('sub',q.pstr(*q.resolve(sr[0],o+8)),'mat',q.pstr(*q.resolve(sr[0],o+0x48))[:40],'skin',q.pu32(sr[0],o+0x58),'flags',hex(q.pu32(sr[0],o+0x10)),'raw',q.d[q.pbase(sr[0])+o:q.pbase(sr[0])+o+0x88].hex()[:200])
p=Pak('p3/air-multi.pak')
C=G.collection(p)
for pi,c in enumerate(C):
  for m3,fi,cnt in c['groups']:
    if 'emblem' not in m3:continue
    for k in range(fi,fi+cnt):
      pg,o=c['blocks'][k];d=G.decode_block(p,pg,o)
      print(pi,m3.split('/')[-2],[ (u,np.round(d[u].min(0),3).tolist(),np.round(d[u].max(0),3).tolist()) for u in('uv0','uv1','uv2')],'col',np.round(d['color'].min(0),2).tolist() if 'color' in d else None)
      for ii,ia in enumerate(c['insts']):
          I=G.instance(p,ia);print('   inst',ii,'mat',np.round(I['mat'],3).tolist(),'x8',I['x8'])
# materials
mt=p.items_of('MATERIAL_TABLE_1')[0];pg,b=mt.page,mt.data_off
nmat=p.pu32(pg,b+0xc);ma=p.resolve(pg,b+0x20)
print('P',[n for n in dir(P) if not n.startswith('__')])
def params(node):
    tbl=p.resolve(node[0],node[1]+0x14);r=[]
    if not tbl or not P._is_param_table(p,tbl):return r
    tb=p.pbase(tbl[0])+tbl[1]
    for k in range(256):
        rr=p.resolve(tbl[0],tbl[1]+0x10*k+0xc)
        if not rr:break
        try:n=p.pstr(*rr)
        except:break
        if not n or not n.isprintable():break
        idx,z,size,cls,kind,off=struct.unpack_from('>HHHBBI',p.d,tb+0x10*k);r.append((n,kind))
    return r
seen=0
for i in range(nmat):
    m=p.resolve(ma[0],ma[1]+4*i);n=p.pstr(*p.resolve(*m))
    if 'emblem' in n:
        S_=[];Cc=[]
        for k,e,node in P._walk(p,m):
            if e>0:break
            for a,kind in params(node):
                (S_ if kind==1 else Cc).append(a)
        print(n.split('/')[-2],'S',list(dict.fromkeys(S_)),'C',list(dict.fromkeys(Cc))[:30])
