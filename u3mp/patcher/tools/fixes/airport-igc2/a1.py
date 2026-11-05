import sys,os,glob,pickle,re
sys.path.insert(0,'../live/airport-igc')
from lib import *
from psarc_lib import Psarc
D=dict(pickle.load(open('../live/coop-tmp/dict.pkl','rb')))
L=[v for v in D.values() if isinstance(v,str)]
print([x for x in L if 'rs-coop' in x or ('dawn' in x and ('rs' in x or 'render' in x))][:60])
print([x for x in L if 'render-settings/game' in x and re.search('air|coop|yem',x)])
B=Psarc(PSA+'bin.psarc'); N=B.names
print(len(N),[n for n in N if re.search('render-settings.*(air|coop|yem)|rs-',n)][:60])
print([n for n in N if 'render' in n][:15])
for a in glob.glob(G+'/u3data/build/main/*.psarc'):
    try:
        Q=Psarc(a); m=[n for n in Q.names if re.search('render-settings.*(air|coop-|yemen-multi)',n)]
        print(os.path.basename(a),len(Q.names),m[:30])
    except Exception as e: print(a,e)
for l in LAY[:4]:
    print(l[-12:],[os.path.relpath(f,l)[-70:] for f in glob.glob(l+'/u3data/build/main/**/*',recursive=True) if re.search('render-settings.*(air|coop)|lut',f)][:20])
