"""London look investigation - shared helpers (PS3 sources, overlay, retail PS4)."""
import sys, os, struct
PT = '{{PAKTOOLS}}'
MC2 = '{{PROJECT}}/scratch/map-colors2'
for x in (PT + '/stage', PT, MC2, MC2 + '/tools'):   # map-colors2/tools/ndpak handles big PS3 paks (fixup dir beyond page 0)
    if x not in sys.path: sys.path.insert(0, x)
from ndpak import Pak, sid
from psarc_lib import Psarc
W = '{{PROJECT}}/work/'
OV = W + 'overlay-r3/u3data/build/main/pak23/ps4/'
OVDC = W + 'overlay-r3/u3data/build/main/dc1/ps4/'
RT = "{{PS4}}/u3data/build/main/pak23/ps4/"
RTDC = "{{PS4}}/u3data/build/main/dc1/ps4/"
PS3D = "{{PS3_DISC}}/"
RP = "{{PS3_UPDATE}}/"
RP2 = "{{PS3_DATA2}}/build/main/"
HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.environ.get('LONDON_PS3_CACHE') or os.path.join(os.environ.get('TEMP', HERE), 'u3-london-ps3cache') + '/'   # extracted PS3 paks (GBs): keep out of the synced folder
_ps3 = {}


def ps3_archives():
    """priority: patch18 > patch13 > goty-patch > goty-dlc > disc pak23/bin"""
    if _ps3: return _ps3
    for tag, f in (('p18', RP + 'dlc-patch18.psarc'), ('p13', RP + 'dlc-patch13.psarc'), ('gpatch', PS3D + 'goty-patch.psarc'),
                   ('gdlc', PS3D + 'goty-dlc.psarc'), ('pak23', PS3D + 'build/main/pak23.psarc'), ('bin', PS3D + 'build/main/bin.psarc')):
        _ps3[tag] = Psarc(f)
    # server-delivered map archives (lowest priority): DATA2 pak23/*-7.psarc and the lab archives
    for d in (RP2 + 'pak23/', RP2):
        for x in sorted(os.listdir(d)):
            if x.endswith('-7.psarc') and 'speech' not in x:
                try: _ps3['d2:' + x[:-8]] = Psarc(d + x)
                except Exception: pass
    return _ps3


def ps3_find(name):
    """-> list of (tag, index, fullname) in priority order"""
    out = []
    for tag, a in ps3_archives().items():
        for i, n in enumerate(a.names):
            if n.endswith('/' + name) or n == name:
                out.append((tag, i, n))
    return out


def ps3_read(name, which=0):
    os.makedirs(CACHE, exist_ok=True)
    c = CACHE + name
    if os.path.exists(c): return open(c, 'rb').read()
    h = ps3_find(name)
    if not h: raise FileNotFoundError(name)
    tag, i, n = h[which]
    d = ps3_archives()[tag].read(i + 1)
    open(c, 'wb').write(d)
    return d


def f32be(d, o): return struct.unpack_from('>f', d, o)[0]
def f32le(d, o): return struct.unpack_from('<f', d, o)[0]
