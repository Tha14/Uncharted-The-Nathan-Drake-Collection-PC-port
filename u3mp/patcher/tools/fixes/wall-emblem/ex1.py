import sys,glob,os
T=r'{{SCRATCH}}/actor33/toolsets/4c36373'
sys.path.insert(0,T)
from psarc_lib import Psarc
PS4="{{PS4}}/u3data/build/main/"
for f in glob.glob(PS4+'actor33/**/*',recursive=True):
    if 'loading-menu' in f or f.endswith('.psarc'): print(f[-60:],os.path.getsize(f))
    if 'loading-menu-cloth' in f and f.endswith('.pak'):
        open('tmpl/ps4-loading-menu-cloth.pak','wb').write(open(f,'rb').read())
for f in glob.glob(PS4+'**/*.psarc',recursive=True):
    try:a=Psarc(f)
    except Exception as e:continue
    for i,n in enumerate(a.names):
        if 'loading-menu-cloth' in n:
            b=a.read(i+1);open('tmpl/ps4-'+n.split('/')[-1],'wb').write(b);print(f[-40:],n,len(b))
U="{{PS3_DISC}}/build/main/"
a=Psarc(U+'pak23.psarc')
air=[n for n in a.names if 'air-multi' in n];print(air)
os.makedirs('p3',exist_ok=True)
for n in air:
    open('p3/'+n.split('/')[-1],'wb').write(a.read(a.names.index(n)+1))
for n in Psarc(U+'actor33.psarc').names:
    if 'loading-menu' in n: print(n)
