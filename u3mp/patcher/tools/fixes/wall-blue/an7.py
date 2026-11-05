from ilib import *
sys.path.insert(0,'{{PAKTOOLS}}')
import texcodec as tc
os.chdir(M2)
def d4(pk,t):
  kind={0x47:'DXT1',0x48:'DXT1',0x4d:'DXT5',0x4e:'DXT5',0x4a:'DXT3',0x4b:'DXT3'}.get(t['format'])
  if not kind:return None
  bpb=8 if kind=='DXT1' else 16
  d=pk.vram_bytes(t['vramOffset'],t['vramSize']);return tc.decode(tc.ps4_mip0_linear(d,t['width'],t['height'],bpb),t['width'],t['height'],kind)
def stat(i): v=i.reshape(-1,i.shape[-1]).astype(float);return v.mean(0).round(1).tolist()
p=Pak('p3/air-multi.pak')
for t in p.textures():
  if 'lightmaps/' in t['path'] or 'indirect' in t['path']:
    kind,bpb,comp=tc.ps3_format(t);d=p.vram_bytes(t['vramOffset'],t['vramSize']);img=tc.decode(d[:tc.ps3_linear_size(t['width'],t['height'],1,bpb)],t['width'],t['height'],kind)
    print('ps3',t['path'][-44:],kind,hex(t['gcmRemapCtl']),t['width'],t['height'],t['mips'],stat(img))
for tag,fn in (('ov air',OV+'air-multi.pak'),('ov 3a',OV+'coop-airport-3a.pak'),('rt 3a',RT+'airport-3a.pak'),('rt air1',RT+'airport-1.pak')):
  q=Pak(fn)
  for t in q.textures():
    if 'lightmaps/' in t['path'] or 'indirect' in t['path']:
      i=d4(q,t);print(tag,t['path'][-52:],hex(t['format']),hex(t['unk20']),t['width'],t['height'],t['mips'],stat(i) if i is not None else '?')
