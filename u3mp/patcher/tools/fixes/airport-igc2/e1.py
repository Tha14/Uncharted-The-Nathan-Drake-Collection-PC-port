import sys
sys.path.insert(0,'../coop-voice')
import ppc,re
d=ppc.d
for s in [b'show-scope-reticle\0',b'DcShowScopeReticle\0',b'binoculars\0',b'binocular']:
    print(s,[hex(m.start()) for m in re.finditer(s,d)][:6])
print([ (hex(va),hex(off),hex(fs),fl) for va,off,fs,fl in ppc.S])
print([k for k in dir(ppc) if not k.startswith('_')])
