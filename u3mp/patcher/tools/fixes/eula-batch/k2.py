import struct
d=open(r'C:/Users/jonas/Documents/Uncharted_Collection_MP/_claude_transfer/u3_ps4.elf','rb').read()
def off(v):return v-0x164c000+0x165c000
V=0x167a610
base=None
print([hex(struct.unpack_from('<Q',d,off(V+i*8))[0]) for i in range(40)])
