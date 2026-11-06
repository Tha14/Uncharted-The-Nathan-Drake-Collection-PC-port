import sys,re
sys.path[:0]=['/home/claude/u3/paktools','/home/claude/u3/paktools/stage','.']
import ps3fp,fp
def ops(l):
  m=re.match(r'(\w+) (\S+?),? (.*)$',l)
  if not m: return None,None,[]
  a=[x.strip() for x in m.group(3).replace('  (end)','').split(', ')] if m.group(3) else []
  return m.group(1),m.group(2).rstrip(','),a
def base(x): return x.lstrip('-|').split('.')[0].rstrip('|')
def comps(d):
  b=base(d);m=d.split('.')[1] if '.' in d else 'xyzw';return b,m
def reads(x,reg):
  b=base(x);sw=x.lstrip('-|').split('.')[1].rstrip('|') if '.' in x else 'xyzw'
  return b==reg[0] and reg[1] in sw
def polarity(L):
  k2=None;rcp=None
  for l in L:
    op,d,a=ops(l)
    if not op: continue
    if op.startswith('MAD') and '0.0100021' in l: b,m=comps(d);k2=(b,m[0]);rcp=None;continue
    if k2 and op.startswith('RCP') and a and reads(a[0],k2): b,m=comps(d);rcp=(b,m[0]);continue
    if k2 and op.startswith('DIV') and len(a)>=2 and reads(a[1],k2): return 'non'
    if rcp:
      if op.startswith('MAD') and len(a)>=3 and a[0].startswith('-') and a[1]==a[2] and reads(a[1],rcp): return 'inv'
      if op.startswith('MUL') and any(reads(x,rcp) for x in a): return 'non'
      if op.startswith('MAD') and any(reads(x,rcp) for x in a[:2]): return 'non?'
    if d:
      b,m=comps(d)
      if k2 and b==k2[0] and k2[1] in m: k2=None
      if rcp and b==rcp[0] and rcp[1] in m: rcp=None
  return 'k2?' if k2 else None
if __name__=='__main__':
  from ndpak import Pak
  p=Pak(sys.argv[1])
  for mn in sys.argv[2:]:
    name,res=ps3fp.material_programs(p,mn);print(mn,polarity(fp.dis(res[0][4])) if res else 'NF')
def polarity2(L):
  tex=set()
  for l in L:
    op,d,a=ops(l)
    if not op: continue
    if op.startswith('TEX') and d and d.endswith('.y'): tex.add(base(d))
    if op.startswith('MAD') and len(a)>=3 and a[0].startswith('-') and a[1]==a[2] and base(a[0]) in tex and '.' in a[0] and a[0].lstrip('-').split('.')[1].startswith('y'):
      return 'inv'
  return 'non'
