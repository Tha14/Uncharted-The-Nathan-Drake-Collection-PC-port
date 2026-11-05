import struct
VEC={0:'NOP',1:'MOV',2:'MUL',3:'ADD',4:'MAD',5:'DP3',6:'DPH',7:'DP4',8:'DST',9:'MIN',10:'MAX',11:'SLT',12:'SGE',13:'ARL',14:'FRC',15:'FLR',16:'SEQ',17:'SFL',18:'SGT',19:'SLE',20:'SNE',21:'STR',22:'SSG',25:'TXL'}
SCA={0:'NOP',1:'MOV',2:'RCP',3:'RCC',4:'RSQ',5:'EXP',6:'LOG',7:'LIT',8:'BRA',9:'BRI',10:'CAL',11:'CLI',12:'RET',13:'LG2',14:'EX2',15:'SIN',16:'COS',17:'BRB',18:'CLB',19:'PSH',20:'POP'}
OUT={0:'HPOS',1:'COL0',2:'COL1',3:'BFC0',4:'BFC1',5:'FOGC',6:'PSZ',7:'TEX0',8:'TEX1',9:'TEX2',10:'TEX3',11:'TEX4',12:'TEX5',13:'TEX6',14:'TEX7',15:'TEX8',16:'TEX9',31:'(none)'}
VNS={'MOV':1,'MUL':2,'ADD':2,'MAD':3,'DP3':2,'DPH':2,'DP4':2,'DST':2,'MIN':2,'MAX':2,'SLT':2,'SGE':2,'ARL':1,'FRC':1,'FLR':1,'SEQ':2,'SGT':2,'SLE':2,'SNE':2,'SSG':1,'TXL':1}
def dis(code,maxn=512):
  out=[];i=0
  while i+16<=len(code) and len(out)<maxn:
    d0,d1,d2,d3=struct.unpack('>4I',code[i:i+16]);i+=16
    src0=((d1&0xff)<<9)|(d2>>23); src1=(d2>>6)&0x1ffff; src2=((d2&0x3f)<<11)|(d3>>21)
    inp=(d1>>8)&0xf; cst=(d1>>12)&0x3ff; vop=(d1>>22)&0x1f; sop=(d1>>27)&0x1f
    absb=[(d0>>21)&1,(d0>>22)&1,(d0>>23)&1]
    def S(s,k):
      t=s&3;tmp=(s>>2)&0x3f
      swz=''.join('xyzw'[(s>>sh)&3] for sh in (14,12,10,8))
      r={1:'R%d'%tmp,2:'v%d'%inp,3:'c[%d]'%cst}.get(t,'?')
      if swz!='xyzw': r+='.'+swz
      if absb[k]: r='|%s|'%r
      if (s>>16)&1: r='-'+r
      return r
    end=d3&1; dst=(d3>>2)&0x1f; scatmp=(d3>>7)&0x3f
    vm=''.join('xyzw'[j] for j,b in enumerate((16,15,14,13)) if (d3>>b)&1)
    sm=''.join('xyzw'[j] for j,b in enumerate((20,19,18,17)) if (d3>>b)&1)
    dtmp=(d0>>15)&0x3f; vres=(d0>>30)&1; sat=(d0>>26)&1
    srcs=[src0,src1,src2]
    if vop and vm:
      n=VNS.get(VEC.get(vop,''),3)
      dn=('o[%s]'%OUT.get(dst,dst)) if vres else 'R%d'%dtmp
      out.append('%s%s %s.%s, %s'%(VEC.get(vop,'v%d'%vop),'_SAT' if sat else '',dn,vm,', '.join(S(srcs[k],k) for k in range(n))))
    if sop and sm:
      dn=('o[%s]'%OUT.get(dst,dst)) if not vres else 'R%d'%scatmp
      out.append('  %s %s.%s, %s'%(SCA.get(sop,'s%d'%sop),dn,sm,S(src2,2)))
    if end: break
  return out
