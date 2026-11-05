import sys,os,glob,re,ast,struct,hashlib,collections
T='{{PAKTOOLS}}'
sys.path.insert(0,T)
sys.dont_write_bytecode=True
from ndpak import Pak,sid
from psarc_lib import Psarc
PR='{{PROJECT}}'
W=PR+'/work'
G='{{PS4}}'
LAY=[W+'/overlay-r3',W+'/overlay-r2',W+'/overlay-port',W+'/overlay-mp',G]
def find(kind,name):
    for l in LAY:
        f=f'{l}/u3data/build/main/{kind}/ps4/{name}.pak'
        if os.path.exists(f): return f,os.path.basename(l)[:12]
    return None,None
RET=G+'/u3data/build/main/actor33/ps4'
DISC="{{PS3_DISC}}/build/main/"
D2='{{PS3_DATA2}}/build/main/'
X='{{PS3_UPDATE}}/'
# newest first
PS3=[('actor-parts-7',D2+'actor-parts-7.psarc'),('patch18',X+'dlc-patch18.psarc'),('patch13',X+'dlc-patch13.psarc'),('disc',DISC+'actor33.psarc')]
class S3:
    def __init__(s):
        s.a=[(n,Psarc(f)) for n,f in PS3]
        s.idx=[(n,a,{x.split('/')[-1]:i+1 for i,x in enumerate(a.names)}) for n,a in s.a]
    def all(s,name):
        return [(n,a.read(ix[name+'.pak'])) for n,a,ix in s.idx if name+'.pak' in ix]
    def get(s,name):
        r=s.all(name); return r[0] if r else (None,None)
import ps4actor as _pa
def _fg(q):
    # repair: submesh array may sit on another page than GEOMETRY_1
    it=q.items_of('FOREGROUND_SECTION_2')[0];pg,b=it.page,it.data_off
    skel,hier=struct.unpack_from('<II',q.d,q.pbase(pg)+b+8)
    jr=q.resolve(pg,b+0x10);jitem=q.item_at(jr[0],jr[1]-0x20)
    names=q.d[q.pbase(jr[0])+jr[1]:q.pbase(jr[0])+jr[1]+jitem.size]
    g=q.items_of('GEOMETRY_1')[0];sp,sm=q.resolve(g.page,g.data_off+0x10)
    return skel,hier,names,q.pu32(sp,sm+0x58)
_pa.tmpl_fg_info=_fg
