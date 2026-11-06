import sys,os,struct,collections,json
T=r'{{SCRATCH}}/actor33/toolsets/4c36373'
sys.path.insert(0,T);sys.path.insert(0,T+'/stage')
# recipe 1.7.0: the 11-01 run used the scratchpad gh2/u3mp/paktools: ps4actor as the actor33 toolset 4c36373, but
# ps4ingame / navmesh of the f525859 tree (4c36373's navmesh rejects air-multi-ingame12's 0x1b0 header).  Load those two
# from the pak23-levels f525859 tree (set up by convert-levels-*) before anything imports them.
import importlib.util as _ilu
import ps4actor as _pa   # 4c36373's (copy_item_blob), loaded first so ps4ingame below does not pull f525859's
_T2 = r'{{SCRATCH}}/pak23-levels-work/pt-f525859'
for _m in ('navmesh', 'ps4ingame'):
    _s = _ilu.spec_from_file_location(_m, _T2 + '/' + _m + '.py'); _mod = _ilu.module_from_spec(_s); sys.modules[_m] = _mod; _s.loader.exec_module(_mod)
from ndpak import Pak,sid
import bggeom as G, numpy as np
