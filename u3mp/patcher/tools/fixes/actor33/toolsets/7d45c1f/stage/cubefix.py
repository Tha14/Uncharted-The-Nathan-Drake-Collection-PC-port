#!/usr/bin/env python3
"""cubefix.py: repair PS3 cube environment maps that older ps4stage builds converted as 2D textures.
For every resident (flag 3) VRAM_DESC of type 1 in a PS4 BG pak whose short path is a type-2 (cube) texture in
the PS3 source pak: append retail-layout cube texels (mip-major, 8 slots per mip, PS4-tiled, faces in PS3 order)
to the pak's trailing VRAM blob, patch the desc (offset, size, type 2, sRGB fmt, +0x20 0x060f0000) and the
GEOMETRY_1 texture entry (+4 = 0x100).  Layout from the rosetta table-top-d-env-refeletion.dds pair.
usage: cubefix.py ps4.pak ps3.pak [out.pak]   (in place when out is omitted)"""
import os, re, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.dirname(HERE)]
from ndpak import Pak
import texcodec as tc


def short_of(path):
    m = re.search(r'texture[12]/(.*?)/[0-9A-F]{16}(\.ndb)?$', path)
    return m.group(1) if m else path


def cube_texels(t3, data, bpb):
    w, h, mips = t3['width'], t3['height'], t3['mips']
    face = tc.ps3_linear_size(w, h, mips, bpb)
    stride = (face + 127) // 128 * 128
    if len(data) < stride * 5 + face:
        return None
    out, o, mipoffs = bytearray(), 0, []
    for mw, mh in tc.mip_dims(w, h, mips):
        bw, bh = tc.blocks(mw, mh, True)
        mipoffs.append((o, bw, bh)); o += bw * bh * bpb
    for mo, bw, bh in mipoffs:
        tiles = [tc.tile_mip(data[k * stride + mo:k * stride + mo + bw * bh * bpb], bw, bh, bpb) for k in range(6)]
        for k in range(8):
            out += tiles[min(k, 5)]
    return bytes(out)


def cube_texels_argb(t3, data):
    """uncompressed swizzled A8R8G8B8 PS3 cube (faces padded to 128 bytes, every mip swizzled on its own)
    -> BC3 cube texels (retail layout); mips below 4x4 are padded by the encoder"""
    import numpy as np
    w, h, mips = t3['width'], t3['height'], t3['mips']
    sizes = [max(1, w >> m) * max(1, h >> m) * 4 for m in range(mips)]
    face = sum(sizes); stride = (face + 127) // 128 * 128
    if len(data) < stride * 5 + face:
        return None
    out = bytearray()
    for m in range(mips):
        mw, mh = max(1, w >> m), max(1, h >> m)
        mo = sum(sizes[:m])
        bw, bh = (mw + 3) // 4, (mh + 3) // 4
        tiles = []
        for k in range(6):
            img = tc.ps3_deswizzle_argb8(data[k * stride + mo:k * stride + mo + sizes[m]], mw, mh,
                                         0xaae4 if os.environ.get('U3_STAGE_NO_REMAP') == '1' else (t3['gcmRemapCtl'] & 0xffff))   # agent Q
            tiles.append(tc.tile_mip(tc.encode_bc3(np.ascontiguousarray(img)), bw, bh, 16))
        for k in range(8):
            out += tiles[min(k, 5)]
    return bytes(out)


def fix_retail_types(path, out=None):
    """descs copied from a retail PS4 template keep the retail cube texels and +0x20 0x060f0000 but older ps4actor
    builds wrote type 1: set type 2 (retail: 0x060f0000 <=> type 2).  Returns the fixed short names."""
    q = Pak(path); d = bytearray(q.d); fixed = []
    for t in q.textures():
        if t.get('type') == 1 and (t.get('unk20', 0) >> 24) == 6:
            struct.pack_into('<I', d, q.pbase(t['page']) + t['off'] + 0x20 + 0x18, 2)
            fixed.append(short_of(t['path']).rsplit('/', 1)[-1])
    if fixed:
        open(out or path, 'wb').write(bytes(d))
    return fixed


def fix(p4path, p3path, out=None):
    if out and out != p4path:
        import shutil; shutil.copyfile(p4path, out); p4path, out = out, None
    rt = fix_retail_types(p4path)
    if p3path is None:
        return rt
    q4, q3 = Pak(p4path), Pak(p3path)
    cubes3 = {}
    for t in q3.textures():
        if t.get('type') == 2:
            cubes3[short_of(t['path'])] = t
    d = bytearray(q4.d)
    vram_extra = bytearray()
    vbase_len = q4.hdr['vramSize']
    fixed = []
    for t in q4.textures():
        if t.get('type') != 1 or t.get('unk34') not in (2, 3):
            continue
        sh = short_of(t['path'])
        t3 = cubes3.get(sh)
        if t3 is None:
            continue
        fmt3 = tc.ps3_format(t3)
        if not fmt3:
            continue
        kind, bpb, comp = fmt3
        if comp:
            tex = cube_texels(t3, q3.vram_bytes(t3['vramOffset'], t3['vramSize']), bpb)
        elif kind == 'A8R8G8B8':
            tex = cube_texels_argb(t3, q3.vram_bytes(t3['vramOffset'], t3['vramSize']))
            kind = 'DXT5'
        else:
            continue
        if tex is None:
            continue
        while (vbase_len + len(vram_extra)) % 0x100:
            vram_extra.append(0)
        voff = vbase_len + len(vram_extra)
        vram_extra += tex
        db = q4.pbase(t['page']) + t['off'] + 0x20          # VRAM_DESC data (after the 0x20 ResItem header)
        fmt4 = {'DXT1': 0x48, 'DXT5': 0x4e, 'DXT3': 0x4b}[kind]
        struct.pack_into('<QI', d, db + 8, voff, len(tex))
        struct.pack_into('<IIIIII', d, db + 0x18, 2, fmt4, 0x060f0000, t3['mips'], t3['width'], t3['height'])
        # GEOMETRY_1 texture entry {hash, size class, 0x101, -1, 0, cls, 0}
        hb = struct.pack('<I', t['hash'])
        i = d.find(hb)
        while i != -1:
            if struct.unpack_from('<I', d, i + 8)[0] == 0x101 and struct.unpack_from('<i', d, i + 12)[0] == -1 and i < q4.vram_base:
                struct.pack_into('<I', d, i + 4, 0x100)
                struct.pack_into('<I', d, i + 0x14, {0x48: 4, 0x4b: 5, 0x4e: 5}[fmt4])
            i = d.find(hb, i + 1)
        fixed.append(sh.rsplit('/', 1)[-1])
    if not fixed:
        return rt
    d += vram_extra
    struct.pack_into('<I', d, 0x20, vbase_len + len(vram_extra))
    open(out or p4path, 'wb').write(bytes(d))
    q = Pak(out or p4path)            # re-parse sanity
    for t in q.textures():
        if short_of(t['path']).rsplit('/', 1)[-1] in fixed:
            assert t['type'] == 2, t
    return rt + fixed


if __name__ == '__main__':
    r = fix(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
    print(len(r), 'cubes fixed', r)
