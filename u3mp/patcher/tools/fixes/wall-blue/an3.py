import sys,os,re,gzip,json
import numpy as np
M2=os.path.abspath('../map-colors2');sys.path.insert(0,M2);os.chdir(M2)
from lib import *
PT='{{PAKTOOLS}}'
sys.path.insert(0,PT+'/stage')
import ps3params,bggeom,ps3fp,vp,p3prog,fp
BK=W+'backup-mapcolor2/air-multi.pak'
M_W=W
p=Pak('p3/air-multi.pak')
mt=p.items_of('MATERIAL_TABLE_1')[0];pg,b=mt.page,mt.data_off
nmat=p.pu32(pg,b+0xc);ma=p.resolve(pg,b+0x20);th=p.resolve(pg,b+0x18);nm=p.resolve(pg,b+0x30);ntex=p.pu32(pg,b+4);sr=p.resolve(pg,b+0x1c)
for i in range(nmat):
  m=p.resolve(ma[0],ma[1]+4*i);name=p.pstr(*p.resolve(*m))
  if name.endswith('corrugated-main-blue/default'):
    WW=ps3params._walk(p,m)
    for k,e,node in []:
      pid=struct.unpack_from('>H',p.d,p.pbase(node[0])+node[1])[0]
      bb=p.resolve(node[0],node[1]+0x10);print('pass',k,e,'pid',hex(pid),'node',p.d[p.pbase(node[0])+node[1]:p.pbase(node[0])+node[1]+0x20].hex())
      row=[]
      for o in range(0,0x100,0x10):
        rr=p.resolve(bb[0],bb[1]+o) if bb else None; x=None
        if rr and rr[0]==th[0] and th[1]<=rr[1]<th[1]+4*ntex: x=p.pstr(*p.resolve(nm[0],nm[1]+4*((rr[1]-th[1])//4))).split('/')[-2]
        if x: row.append((hex(o),x))
      print('  ',row)
      t=p.resolve(sr[0],sr[1]+0x10*pid+12)
      if e==0:
        print('  table',[ (hex(z)) for z in struct.unpack_from('>4I',p.d,p.pbase(sr[0])+sr[1]+0x10*pid)])
        vr=p.resolve(sr[0],sr[1]+0x10*pid+8)
        if vr:
          B=p.pbase(vr[0])+vr[1];print('  vp hdr',p.d[B:B+0x30].hex())
          try:
            h=struct.unpack_from('>8I',p.d,B);print(h);L=vp.dis(p.d[B+h[4]:B+h[4]+h[0]])
            print('\n'.join('   '+x for x in L[:60]))
          except Exception as ex:print('vp err',ex)
print(sorted((t['path'],hex(t['hash'])) for t in Pak(OV+'air-multi.pak').textures() if 'corrugated-wall-painted' in t['path']))
print(sorted((t['path'],hex(t['hash'])) for t in Pak(BK).textures() if 'corrugated-wall-painted' in t['path']))
for tag,fn in (('ov',OV+'air-multi.pak'),('rtrest',RT+'airport-rest.pak')):
  for m in mats4(Pak(fn)):
    if re.search(r'corrugated-main-blue:',m['n']):print(tag,m['n'].split(',')[0],[(x[0],x[2][:x[1]]) for x in m['p']],m['b'],m['s'],m['h'])
import texcodec as tc

q=Pak(OV+'air-multi.pak');rt=Pak(RT+'airport-rest.pak')
def dec4(pk,t):
  kind={0x47:'DXT1',0x48:'DXT1',0x4d:'DXT5',0x4e:'DXT5'}[t['format']];bpb=8 if kind=='DXT1' else 16
  d=pk.vram_bytes(t['vramOffset'],t['vramSize'])
  lin=tc.ps4_mip0_linear(d,t['width'],t['height'],bpb)
  return tc.decode(lin,t['width'],t['height'],kind)
os.makedirs(HOME:=os.path.abspath(M2+'/../wall-blue/tex'),exist_ok=True)
def prn(tag,img): print(tag,img.shape,'mean',img.reshape(-1,img.shape[-1]).mean(0).round(1).tolist(),'p5,50,95',np.percentile(img[...,1],[5,50,95]).round(0).tolist())
for s in ['wall-color','wall-blend','wall-painted-blend']:
  n='air-yem-m-corrugated-%s.tga'%s
  for t in p.textures():
    if t['path'].split('/')[-2]==n:
      kind,bpb,comp=tc.ps3_format(t);d=p.vram_bytes(t['vramOffset'],t['vramSize']);w,h=t['width'],t['height'];o=0;lv={}
      for mw,mh in tc.mip_dims(w,h,t['mips']):
        sz=tc.ps3_linear_size(mw,mh,1,bpb);lv[mw]=tc.decode(d[o:o+sz],mw,mh,kind);o+=sz
      prn('ps3 %s %d'%(s,w),lv[w]);prn('ps3 %s mip64'%s,lv[64])
  for pk,tg in ((q,'ov'),(rt,'rt')):
    for t in pk.textures():
      if t['path'].split('/')[-2]==n:
        img=dec4(pk,t);prn('%s %s %d fmt%x %s'%(tg,s,t['width'],t['format'],t['path'][-20:]),img)
        if t['width']==64:
          d=np.abs(img[...,:3].astype(float)-lv[64][...,:3].astype(float)).mean();print('   mean abs diff vs PS3 mip64',round(d,2))
        elif t['width']==lv and 0:pass
        if t['width'] in lv and t['width']!=64: print('   diff vs PS3 same-size',round(np.abs(img[...,:3].astype(float)-lv[t['width']][...,:3].astype(float)).mean(),2))
for m in mats4(q):
  if 'corrugated-main-blue:' in m['n']:print(m['b'])
