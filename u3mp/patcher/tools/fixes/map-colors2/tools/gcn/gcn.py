import json,struct
def fmt_of(w):
  t=w>>25
  if t==0x3f: return 'VOP1'
  if t==0x3e: return 'VOPC'
  if w>>31==0: return 'VOP2'
  if (w>>23)==0x17d: return 'SOP1'
  if (w>>23)==0x17e: return 'SOPC'
  if (w>>23)==0x17f: return 'SOPP'
  if (w>>28)==0xb: return 'SOPK'
  if (w>>30)==2: return 'SOP2'
  if (w>>27)==0x18: return 'SMRD'
  t6=w>>26
  return {0x34:'VOP3',0x32:'VINTRP',0x36:'DS',0x38:'MUBUF',0x3a:'MTBUF',0x3c:'MIMG',0x3e:'EXP'}.get(t6,'?')
def op_of(f,w):
  return {'VOP2':(w>>25)&0x3f,'VOP1':(w>>9)&0xff,'VOPC':(w>>17)&0xff,'SOP1':(w>>8)&0xff,'SOPC':(w>>16)&0x7f,'SOPP':(w>>16)&0x7f,'SOPK':(w>>23)&0x1f,'SOP2':(w>>23)&0x7f,
          'SMRD':(w>>22)&0x1f,'VOP3':(w>>17)&0x1ff,'VINTRP':(w>>16)&3,'DS':(w>>18)&0xff,'MUBUF':(w>>18)&0x7f,'MTBUF':(w>>16)&7,'MIMG':(w>>18)&0x7f,'EXP':0,'?':0}[f]
E=json.load(open(__file__.rsplit('/',1)[0]+'/enc.json'))
TAB={}
for name,(ops,h) in E.items():
  b=bytes.fromhex(h);w=struct.unpack('<I',b[:4])[0];f=fmt_of(w)
  k=(f,op_of(f,w))
  if k not in TAB or (name.endswith('_e64') and f=='VOP3'): TAB.setdefault(k,name.replace('_e32','').replace('_e64',''))
CONST={106:'vcc_lo',107:'vcc_hi',124:'m0',126:'exec_lo',127:'exec_hi',128:'0',240:'0.5',241:'-0.5',242:'1.0',243:'-1.0',244:'2.0',245:'-2.0',246:'4.0',247:'-4.0',251:'vccz',252:'execz',253:'scc',254:'lds'}
def src(v,lit=None):
  if v<104: return 's%d'%v
  if v>=256: return 'v%d'%(v-256)
  if 129<=v<=192: return str(v-128)
  if 193<=v<=208: return str(192-v)
  if v==255: return ('lit(0x%08x=%g)'%(lit,struct.unpack('<f',struct.pack('<I',lit))[0])) if lit is not None else 'lit'
  return CONST.get(v,'?%d'%v)
def dis(code,base=0):
  out=[];i=0;n=len(code)//4
  W=struct.unpack('<%dI'%n,code[:n*4])
  while i<n:
    w=W[i];f=fmt_of(w);op=op_of(f,w);nm=TAB.get((f,op),'%s_op%d'%(f,op));sz=1;s=''
    lit=W[i+1] if i+1<n else None
    if f=='VOP2':
      s0=w&0x1ff;s='v%d, %s, v%d'%((w>>17)&0xff,src(s0,lit),(w>>9)&0xff);sz+= s0==255
      if nm in('v_madmk_f32','v_madak_f32'): s+=', lit(%g)'%struct.unpack('<f',struct.pack('<I',lit))[0];sz=2
    elif f=='VOP1': s0=w&0x1ff;s='v%d, %s'%((w>>17)&0xff,src(s0,lit));sz+=s0==255
    elif f=='VOPC': s0=w&0x1ff;s='vcc, %s, v%d'%(src(s0,lit),(w>>9)&0xff);sz+=s0==255
    elif f=='VOP3':
      w2=W[i+1];sz=2
      neg=w2>>29;ab=(w>>8)&7;om=(w2>>27)&3;cl=(w>>11)&1
      ss=[src(w2&0x1ff),src((w2>>9)&0x1ff),src((w2>>18)&0x1ff)]
      ss=[('-' if neg>>j&1 else '')+('|%s|'%x if ab>>j&1 else x) for j,x in enumerate(ss)]
      s='v%d, %s%s%s'%(w&0xff,', '.join(ss),' clamp' if cl else '',' omod%d'%om if om else '')
    elif f in('SOP2','SOPK','SOP1','SOPC','SOPP'):
      if f=='SOP2': a,b2=w&0xff,(w>>8)&0xff;s='%s, %s, %s'%(src((w>>16)&0x7f),src(a,lit),src(b2,lit));sz+=(a==255 or b2==255)
      elif f=='SOP1': a=w&0xff;s='%s, %s'%(src((w>>16)&0x7f),src(a,lit));sz+=a==255
      elif f=='SOPC': a,b2=w&0xff,(w>>8)&0xff;s='%s, %s'%(src(a,lit),src(b2,lit));sz+=(a==255 or b2==255)
      elif f=='SOPK': s='%s, 0x%x'%(src((w>>16)&0x7f),w&0xffff)
      else: s='0x%x'%(w&0xffff)
    elif f=='SMRD':
      s='s%d, s[%d], %s'%((w>>15)&0x7f,((w>>9)&0x3f)*2,('0x%x'%((w&0xff)*4)) if w>>8&1 else src(w&0xff))
    elif f=='VINTRP':
      s='v%d, v%d, attr%d.%s'%((w>>18)&0xff,w&0xff,(w>>10)&0x3f,'xyzw'[(w>>8)&3])
    elif f in ('MIMG','MUBUF','MTBUF','DS','EXP'):
      w2=W[i+1];sz=2
      if f=='MIMG': s='v%d(dmask %x), vaddr v%d, T#s%d, S#s%d'%((w2>>8)&0xff,(w>>8)&0xf,w2&0xff,((w2>>16)&0x1f)*4,((w2>>21)&0x1f)*4)
      elif f=='EXP': s='tgt%d en%x v%d v%d v%d v%d%s%s'%((w>>4)&0x3f,w&0xf,w2&0xff,(w2>>8)&0xff,(w2>>16)&0xff,(w2>>24)&0xff,' compr' if w>>10&1 else '',' done' if w>>11&1 else '')
      else: s='%08x %08x'%(w,w2)
    out.append('%5x: %-24s %s'%(base+4*i,nm,s))
    i+=sz
    if f=='SOPP' and op==1: break   # s_endpgm
  return out
TAB[('VOP2',32)]='v_madmk_f32';TAB[('VOP2',33)]='v_madak_f32'
