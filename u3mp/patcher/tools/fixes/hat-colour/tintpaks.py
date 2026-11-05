"""retail actor paks (retail.pkl set) that own a material with COLOR_1_APPLY_INSTANCE_TINT and no overlay features -> tintpaks.json"""
import sys,os,pickle,json
sys.dont_write_bytecode=True
AM='{{PROJECT}}/scratch/actor-materials'
sys.path.insert(0,AM)
from lib import *
import ps4actor as A
R=pickle.load(open(AM+'/retail.pkl','rb')); out={}
for b in R:
    try: ms=A.tmpl_materials(Pak(RET+'/'+b+'.pak'))
    except Exception as e: continue
    t=[m['name'].split(',')[0] for m in ms if 'COLOR_1_APPLY_INSTANCE_TINT' in m['feats'] and not any('OVERLAY' in f or 'UV_3' in f for f in m['feats'])]
    if t: out[b]=t
json.dump(out,open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'tintpaks.json'),'w'),indent=0); print(len(out),'of',len(R))
