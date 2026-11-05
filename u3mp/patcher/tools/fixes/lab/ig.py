import sys,os,traceback
T=sys.argv[1];sys.path[:0]=[T,T+'/stage']
import ps4ingame
from ndpak import Pak
ps4ingame.IGNORE_OTHERS=True
for f in sorted(os.listdir('../labsrc/pak')):
  if f.startswith('lab-') and f.endswith('-ingame12.pak') and not os.path.exists('out/'+f):
    try:ps4ingame.convert('../labsrc/pak/'+f,'out/'+f);print('ok',f,os.path.getsize('out/'+f))
    except BaseException as e:print('FAIL',f,repr(e)[:110])
