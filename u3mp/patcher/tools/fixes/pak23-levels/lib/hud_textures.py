"""overlay-r3 hud.pak (2026-10-29 10:48, cloud): the retail PS4 hud.pak with two HUD textures replaced by their
full-resolution PS3 versions (PS4 kept 64x16 BC4 / 64x64 copies): gun-galil-trans 256x64 and the distress font
128x128, both PS3 DXT1 converted 1:1 to PS4 tiled BC1 (texcodec.ps3_to_ps4_texels), appended to VRAM in this
order; only the two VRAM_DESC records (offset, size, format 0x47, mips, width, height, flag 3) and the header VRAM
size change.  Reconstructed 2026-11-05 from the installed file (20 changed bytes + the appended texels).
usage: hud_textures.py <paktools> <retail PS4 hud.pak> <PS3 hud.pak> <out.pak>"""
import struct
import sys

sys.path.insert(0, sys.argv[1])
from ndpak import Pak  # noqa: E402
import texcodec as tc  # noqa: E402

TEX = ['art/general/hud/guns/gun-galil-trans.tga', 'art/general/textures/font/distress.tga']
r, p = Pak(sys.argv[2]), Pak(sys.argv[3])
d = bytearray(r.d)
vram = bytearray(r.d[r.vram_base:])
for short in TEX:
    t4 = [t for t in r.textures() if '/' + short + '/' in t['path']][0]
    t3 = [t for t in p.textures() if '/' + short + '/' in t.get('path', '')][0]
    kind, bpb, comp = tc.ps3_format(t3)
    assert kind == 'DXT1', kind
    til = bytes(tc.ps3_to_ps4_texels(p.vram_bytes(t3['vramOffset'], t3['vramSize']), t3['width'], t3['height'], t3['mips'], bpb))
    b = r.pbase(t4['page']) + t4['off'] + 0x20
    struct.pack_into('<Q', d, b + 8, len(vram))
    struct.pack_into('<I', d, b + 0x10, len(til))
    struct.pack_into('<I', d, b + 0x1c, 0x47)
    struct.pack_into('<III', d, b + 0x24, t3['mips'], t3['width'], t3['height'])
    struct.pack_into('<I', d, b + 0x34, 3)
    vram += til
struct.pack_into('<I', d, 0x20, len(vram))
open(sys.argv[4], 'wb').write(bytes(d[:r.vram_base]) + bytes(vram))
