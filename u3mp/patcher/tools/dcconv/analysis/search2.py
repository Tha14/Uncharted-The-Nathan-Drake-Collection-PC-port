import os,sys,json,re
ROOT="C:/Users/jonas/.claude/projects/C--Users-jonas-Documents-ChatGPT-test/"
keys=[k.encode() for k in sys.argv[1:]]
for dp,dn,fn in os.walk(ROOT):
  for f in fn:
    if not f.endswith('.jsonl'):continue
    p=os.path.join(dp,f)
    with open(p,'rb') as fh:
      for ln,line in enumerate(fh):
        if all(k in line for k in keys):
          print(os.path.relpath(p,ROOT),ln,len(line))
