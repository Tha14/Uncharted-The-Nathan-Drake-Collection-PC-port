from lib import *
from psarc_lib import Psarc
import re,os
B=Psarc(PSA+'bin.psarc')
for i,n in enumerate(B.names):
    if n.endswith('.bin') and 'render-settings/' not in n:
        d=B.read(i+1)
        if b'play-opening' in d or b'ss-igcs' in d: print(n,len(d))
