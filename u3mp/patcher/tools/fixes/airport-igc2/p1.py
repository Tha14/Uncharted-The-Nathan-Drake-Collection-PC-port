from lib import *
from psarc_lib import Psarc
from dc import sid
import struct,glob
B=Psarc(PSA+'bin.psarc')
names=['mp-hero-flynn-coop','mp-hero-lazarevic-coop','eddie','lazarevic','flynn','mp-hero-flynn-torso','yem-civ-male-torso-all','agent-tint-all','bedouin-all','cartaciv-male-b-all']
S={sid(n):n for n in names}
from dc import DC
import tempfile,os
hits={}
for i,n in enumerate(B.names):
    if n.endswith('.bin'):
        d=B.read(i+1)
        if d[:4]!=b'DC00':continue
        cnt=struct.unpack_from('>I',d,0x14)[0]
        for k in range(cnt):
            s,t,o=struct.unpack_from('>III',d,0x18+12*k)
            if s in S: hits.setdefault(S[s],[]).append((n,hex(t)))
print(hits)
