"""dis.py fn ADDR [n] | xref ADDR | grep REGEX  -- query the PS4 disassembly pickle"""
import pickle,sys,re,bisect
P='{{PROJECT}}'
A,T=pickle.load(open(P+'/scratch/live/instance-mat/_dis.pkl','rb'))
cmd=sys.argv[1]
if cmd=='fn':
  a=int(sys.argv[2],16);n=int(sys.argv[3]) if len(sys.argv)>3 else 400
  i=bisect.bisect_left(A,a)
  for x in A[i:i+n]:
    t=T[x];print('%x %s'%(x,t))
    if t.strip().startswith('ret') and x>a+8 and '--all' not in sys.argv:break
elif cmd=='xref':
  k=sys.argv[2].lower()
  for x in A:
    if k in T[x].lower():print('%x %s'%(x,T[x]))
elif cmd=='grep':
  r=re.compile(sys.argv[2])
  for x in A:
    if r.search(T[x]):print('%x %s'%(x,T[x]))
