import struct
OPS={0:'NOP',1:'MOV',2:'MUL',3:'ADD',4:'MAD',5:'DP3',6:'DP4',7:'DST',8:'MIN',9:'MAX',0xA:'SLT',0xB:'SGE',0xC:'SLE',0xD:'SGT',0xE:'SNE',0xF:'SEQ',0x10:'FRC',0x11:'FLR',0x12:'KIL',0x13:'PK4',0x14:'UP4',0x15:'DDX',0x16:'DDY',0x17:'TEX',0x18:'TXP',0x19:'TXD',0x1A:'RCP',0x1B:'RSQ',0x1C:'EX2',0x1D:'LG2',0x1E:'LIT',0x1F:'LRP',0x20:'STR',0x21:'SFL',0x22:'COS',0x23:'SIN',0x24:'PK2',0x25:'UP2',0x26:'POW',0x27:'PKB',0x28:'UPB',0x29:'PK16',0x2A:'UP16',0x2B:'BEM',0x2C:'PKG',0x2D:'UPG',0x2E:'DP2A',0x2F:'TXL',0x31:'TXB',0x33:'TEXBEM',0x34:'TXPBEM',0x35:'BEMLUM',0x36:'REFL',0x37:'TIMESWTEX',0x38:'DP2',0x39:'NRM',0x3A:'DIV',0x3B:'DIVSQ',0x3C:'LIF',0x3D:'FENCT',0x3E:'FENCB',0x40:'BRK',0x41:'CAL',0x42:'IFE',0x43:'LOOP',0x44:'REP',0x45:'RET'}
NSRC={'MOV':1,'MUL':2,'ADD':2,'MAD':3,'DP3':2,'DP4':2,'MIN':2,'MAX':2,'SLT':2,'SGE':2,'SLE':2,'SGT':2,'SNE':2,'SEQ':2,'FRC':1,'FLR':1,'KIL':0,'TEX':1,'TXP':1,'TXL':1,'TXB':1,'TXD':3,'RCP':1,'RSQ':1,'EX2':1,'LG2':1,'LRP':3,'COS':1,'SIN':1,'POW':2,'DP2':2,'NRM':1,'DIV':2,'DIVSQ':2,'DDX':1,'DDY':1,'PK2':1,'UP2':1,'PKB':1,'UPB':1,'PK16':1,'UP16':1,'PK4':1,'UP4':1,'DP2A':3,'REFL':2,'LIT':1,'DST':2}
ATTR=['WPOS','COL0','COL1','FOGC','TEX0','TEX1','TEX2','TEX3','TEX4','TEX5','TEX6','TEX7','TEX8','TEX9','SSA','?15']
def sw(w): return ((w&0xffff)<<16)|(w>>16)
def fswap(b):
  w=struct.unpack('>I',b)[0];return struct.unpack('>f',struct.pack('>I',sw(w)))[0]
def dis(code,maxn=2000):
  out=[];i=0
  while i+16<=len(code) and len(out)<maxn:
    w=[sw(x) for x in struct.unpack('>4I',code[i:i+16])];i+=16
    d,s0,s1,s2=w
    op=((d>>24)&0x3f)|(((s1>>31)&1)<<6);name=OPS.get(op,'op%x'%op)
    srcs=[s0,s1,s2];const=None
    if any((s&3)==2 for s in srcs[:NSRC.get(name,3)]):
      const=[fswap(code[i+4*k:i+4*k+4]) for k in range(4)];i+=16
    def S(s,k):
      t=s&3;idx=(s>>2)&0x3f;h=(s>>8)&1
      swz=''.join('xyzw'[(s>>(9+2*j))&3] for j in range(4))
      if t==0: r=('H' if h else 'R')+str(idx)
      elif t==1: r='f[%s]'%ATTR[(d>>13)&0xf]
      elif t==2: r='{%g,%g,%g,%g}'%tuple(const)
      else: r='?'
      if swz!='xyzw': r+='.'+swz
      ab=(s>>29)&1 if k==0 else (s>>18)&1
      if ab: r='|%s|'%r
      if (s>>17)&1: r='-'+r
      return r
    mask=''.join('xyzw'[j] for j in range(4) if (d>>(9+j))&1)
    dst=('H' if (d>>7)&1 else 'R')+str((d>>1)&0x3f)+('.'+mask if mask!='xyzw' else '')
    if (d>>30)&1: dst='(nodst)'
    sc={1:'_x2',2:'_x4',3:'_x8',5:'_d2',6:'_d4',7:'_d8'}.get((s1>>28)&7,'')
    sat='_SAT' if (d>>31)&1 else ''
    n=NSRC.get(name,3)
    args=[S(srcs[k],k) for k in range(n)]
    if name in ('TEX','TXP','TXL','TXB','TXD'): args.append('tex%d'%((d>>17)&0xf))
    out.append('%s%s%s %s, %s%s'%(name,sc,sat,dst,', '.join(args),'  (end)' if d&1 else ''))
    if d&1: break
  return out
if __name__=='__main__':
  raws=['17028200 1c9dc801 0001c800 3fe1c800','01403e84 1c9dc801 0001c800 3fe1c800','02001000 1c9c0000 0001c908 0001c800','17008e04 1c9dc801 0001c800 3fe1c800','02000e01 1c9dc908 0001c808 0001c800']
  code=b''.join(struct.pack('>I',sw(int(x,16))) for r in raws for x in r.split())
  print('\n'.join(dis(code)))
