import struct
d=open(r'C:/Users/jonas/Documents/Uncharted_Collection_MP/_claude_transfer/u3_ps4.elf','rb').read()
o=0x64bfd0+0x10000
T=[0x64bfd0+struct.unpack_from('<i',d,o+i*4)[0] for i in range(0x32)]
print({hex(i):hex(a) for i,a in enumerate(T)})
print(hex(T[0x27]),hex(T[0xc]),hex(T[2]))
