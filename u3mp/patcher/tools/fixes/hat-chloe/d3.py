import sys,struct
sys.dont_write_bytecode=True
sys.path.insert(0,'hat-chloe')
from elf import rd
print('13a68a0',struct.unpack('<6i',rd(0x13a68a0,24)))
for base in(0x996a28,0x9954b8):
  print(hex(base),[hex(base+x) for x in struct.unpack('<11i',rd(base,44))])
print(rd(0x146fac8,16))
