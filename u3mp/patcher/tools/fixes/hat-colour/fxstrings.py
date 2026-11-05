import sys,os,re
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
sys.path.insert(0,AM)
from lib import *
for f in sys.argv[1:]:
    q=Pak(f if os.path.exists(f) else find('actor33',f)[0]); print('==',f)
    for e in q.items_of('EFFECT'):
        d=q.d[q.pbase(e.page)+e.data_off:q.pbase(e.page)+e.data_off+e.size]
        ss=sorted(set(m.group().decode() for m in re.finditer(rb'[A-Za-z_][A-Za-z0-9_]{3,}',d)))
        h=[s for s in ss if re.match(r'[0-9A-F]{16}$',s)]
        tint=[s for s in ss if 'int' in s.lower() and 'Tint' in s or 'tint' in s.lower()]
        print('  EFFECT size %#x'%e.size,'hash',h[:2],'tint-ish',tint[:12],'nstr',len(ss))
        print('     cb-ish',[s for s in ss if s.startswith(('g_','cb','Instance','instance'))][:40])
