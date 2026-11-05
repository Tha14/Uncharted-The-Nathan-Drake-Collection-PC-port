import sys,os,pickle,hashlib,json
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
sys.path.insert(0,AM); sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from lib import *
import ps4actor as A
from build_hat import fg
R=pickle.load(open(AM+'/retail.pkl','rb'))
b=json.load(open('batch.json'))
import collections; sk=collections.Counter()
for n,v in b.items():
    if v.get('ok') or v.get('failed'): continue
    f=fg(Pak(find('actor33',n)[0])); key=(hex(f[0]),hex(f[1]),f[3]); same=[k for k,r in R.items() if (r['skel'],r['hier'],r['jh'])==(f[0],f[1],f[3])]
    sk[key+(len(same),)]+=1
    if len(sys.argv)>1 and n in sys.argv[1:]:
        print(n,key,'same-skeleton retail:',same)
        for t in same:
            print('   ',t,[ (m['name'].split(',')[0].split('/')[-1],'TINT' if 'COLOR_1_APPLY_INSTANCE_TINT' in m['feats'] else '') for m in A.tmpl_materials(Pak(RET+'/'+t+'.pak'))])
for k,c in sk.most_common(): print(c,k)
