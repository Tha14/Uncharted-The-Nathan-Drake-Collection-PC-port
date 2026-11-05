"""Split a concatenated PS3 actor pak (several art groups) into a stand-alone PS3 pak for one art group.
Keeps the pages reachable from that art's login/VRAM tables; rewrites header, page list, fixups and login entries.
usage: slicepak.py <in.pak> <art> <out.pak>   /  slice_bytes(path, art) -> bytes"""
import struct,sys,os
sys.path.insert(0,os.environ.get('PAKTOOLS',''))
from ndpak import Pak
def arts(q):return [i.name for i in q.items_of('ARTGROUP_DATA')]
def slice_bytes(path,art):
  q=Pak(path);assert q.platform=='ps3'
  ag=[i for i in q.items_of('ARTGROUP_DATA') if i.name==art]
  assert len(ag)==1,art
  ag=ag[0]
  # the art's own tail page: first page after ag.page that holds a VRAM_DESC_TABLE at +0xf0 and points back at ag.page
  out={}
  for a,b,_ in q.fixups:out.setdefault(a,set()).add(b)
  logins=[i for i in q.items_of('PAK_LOGIN_TABLE')]
  vts=q.items_of('VRAM_DESC_TABLE')
  tail=None
  for i in logins:
    if i.page>ag.page and ag.page in out.get(i.page,()) and i.page!=q.hdr['loginPage']:tail=i.page;break
  if tail is None:
    assert len(q.items_of('ARTGROUP_DATA'))==1
    tail=q.hdr['loginPage']
  lg=[i for i in logins if i.page==tail][0];vt=[i for i in vts if i.page==tail][0]
  keep={tail};st=[tail]
  while st:
    p=st.pop()
    for t in out.get(p,()):
      if t not in keep:keep.add(t);st.append(t)
  keep=sorted(keep);remap={p:k for k,p in enumerate(keep)}
  fx=[(remap[a],remap[b],o) for a,b,o in q.fixups if a in remap and b in remap]
  pages=[bytearray(q.d[q.pages[p][0]:q.pages[p][0]+q.pages[p][1]]) for p in keep]
  for i in logins:
    if i.page in remap:
      pg=pages[remap[i.page]];b=i.data_off;n=struct.unpack_from('>I',pg,b)[0]
      for k in range(n):
        a,c=struct.unpack_from('>II',pg,b+8+k*8)
        if a in remap:struct.pack_into('>I',pg,b+8+k*8,remap[a])
  hdr=0x2c+len(keep)*12;fxo=hdr;fxd=fxo+8*12;total=fxd+len(fx)*8
  total=(total+0xfff)&~0xfff;pos=total;ent=[]
  for p in pages:
    ent.append((pos,len(p),0));pos+=len(p)
  vram=q.d[q.vram_base:]
  h=bytearray(total)
  struct.pack_into('>I',h,0,struct.unpack_from('>I',q.d,0)[0])
  struct.pack_into('>10I',h,4,total,remap[tail],lg.off,len(keep),0x2c,8,fxo,len(vram),remap[tail],vt.off)
  for k,e in enumerate(ent):struct.pack_into('>3I',h,0x2c+k*12,*e)
  struct.pack_into('>3i',h,fxo,0,fxd,len(fx))
  for k,(a,b,o) in enumerate(fx):struct.pack_into('>HHI',h,fxd+k*8,a,b,o)
  return bytes(h)+b''.join(bytes(p) for p in pages)+vram
def slice_pages(path,page,dst):
  """keep the sub-package whose login table is on `page` (plus the pages it points to); same writer as slice_bytes"""
  q=Pak(path);assert q.platform=='ps3'
  out={}
  for a,b,_ in q.fixups:out.setdefault(a,set()).add(b)
  stop={i.page for i in q.items_of('ARTGROUP_DATA')}|{q.hdr['loginPage']}
  keep={page};st=[page]
  while st:
    for t in out.get(st.pop(),()):
      if t not in keep and t not in stop:keep.add(t);st.append(t)
  logins=q.items_of('PAK_LOGIN_TABLE');vts=q.items_of('VRAM_DESC_TABLE')
  lg=[i for i in logins if i.page==page][0]
  vt=[i for i in vts if i.page==page]
  keep=sorted(keep);remap={p:k for k,p in enumerate(keep)}
  fx=[(remap[a],remap[b],o) for a,b,o in q.fixups if a in remap and b in remap]
  drop=[(a,b,o) for a,b,o in q.fixups if a in remap and b not in remap]
  assert not drop,('dangling',page,drop[:3])
  pages=[bytearray(q.d[q.pages[p][0]:q.pages[p][0]+q.pages[p][1]]) for p in keep]
  for i in logins:
    if i.page in remap:
      pg=pages[remap[i.page]];b=i.data_off;n=struct.unpack_from('>I',pg,b)[0]
      for k in range(n):
        a,c=struct.unpack_from('>II',pg,b+8+k*8)
        if a in remap:struct.pack_into('>I',pg,b+8+k*8,remap[a])
  hdr=0x2c+len(keep)*12;fxo=hdr;fxd=fxo+8*12;total=(fxd+len(fx)*8+0xfff)&~0xfff;pos=total;ent=[]
  for p in pages:
    ent.append((pos,len(p),0));pos+=len(p)
  h=bytearray(total);struct.pack_into('>I',h,0,struct.unpack_from('>I',q.d,0)[0])
  struct.pack_into('>10I',h,4,total,remap[page],lg.off,len(keep),0x2c,8,fxo,0,remap[page] if vt else 0,vt[0].off if vt else 0)
  for k,e in enumerate(ent):struct.pack_into('>3I',h,0x2c+k*12,*e)
  struct.pack_into('>3i',h,fxo,0,fxd,len(fx))
  for k,(a,b,o) in enumerate(fx):struct.pack_into('>HHI',h,fxd+k*8,a,b,o)
  open(dst,'wb').write(bytes(h)+b''.join(bytes(p) for p in pages))
if __name__=='__main__':
  open(sys.argv[3],'wb').write(slice_bytes(sys.argv[1],sys.argv[2]))
