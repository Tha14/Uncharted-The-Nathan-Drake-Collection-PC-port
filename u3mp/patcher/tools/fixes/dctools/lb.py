import sys,struct,re
sys.path.insert(0,'.')
from common import DC,sid
W="{{PROJECT}}/work/overlay-r3/u3data/build/main/dc1/ps4/"
A=DC("{{PS3_DATA2}}/net18.bin");B=DC(W+"net.bin")
print([m for m in dir(A) if not m.startswith('_')])
