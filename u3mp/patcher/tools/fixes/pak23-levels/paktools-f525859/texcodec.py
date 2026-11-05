"""Texture helpers: BCn block decoding (for previews / validation) and the
PS3 (RSX linear) <-> PS4 (GNM 1D-thin micro-tiled) block layout conversion.

Block layouts established from the sample paks (see FINDINGS.md):
  * PS3 RSX DXT textures in these paks are stored linearly (row-major 4x4 blocks,
    pitch = blocksWide * blockBytes), mip chain packed back to back without padding.
  * PS4 textures are stored per mip, each mip padded to a multiple of 8x8 blocks and
    laid out as 8x8-block micro tiles (row-major tile order), each tile's 64 elements
    in "thin/non-displayable" Morton order (x0 y0 x1 y1 x2 y2).
"""
import struct
import numpy as np

# ---------------------------------------------------------------- formats
# DXGI_FORMAT values, which is what the PS4 VRAM_DESC format field holds.
DXGI = {
    0x47: ('BC1_UNORM', 8), 0x48: ('BC1_UNORM_SRGB', 8),
    0x4a: ('BC2_UNORM', 16), 0x4b: ('BC2_UNORM_SRGB', 16),
    0x4d: ('BC3_UNORM', 16), 0x4e: ('BC3_UNORM_SRGB', 16),
    0x50: ('BC4_UNORM', 8), 0x51: ('BC4_SNORM', 8),
    0x53: ('BC5_UNORM', 16), 0x54: ('BC5_SNORM', 16),
    0x1c: ('R8G8B8A8_UNORM', 4), 0x1d: ('R8G8B8A8_UNORM_SRGB', 4),
    0x57: ('B8G8R8A8_UNORM', 4), 0x5b: ('B8G8R8A8_UNORM_SRGB', 4),
}

# PS3 VRAM_DESC word at +0x14: top byte is the GCM texture format class used by
# these paks (observed values); low 16 bits is the GCM component remap.
PS3_FMT = {
    0x06: ('DXT1', 8, True),     # CELL_GCM_TEXTURE_COMPRESSED_DXT1   (size checks match 8 B/block)
    0x07: ('DXT3', 16, True),
    0x08: ('DXT5', 16, True),    # CELL_GCM_TEXTURE_COMPRESSED_DXT45
    0x05: ('A8R8G8B8', 4, False),
}


def ps3_format(desc):
    return PS3_FMT.get(desc['gcmRemapCtl'] >> 24)


def mip_dims(w, h, mips):
    out = []
    for m in range(mips):
        out.append((max(1, w >> m), max(1, h >> m)))
    return out


def blocks(w, h, compressed):
    if compressed:
        return (w + 3) // 4, (h + 3) // 4
    return w, h


def ps3_linear_size(w, h, mips, bpb, compressed=True):
    s = 0
    for mw, mh in mip_dims(w, h, mips):
        bw, bh = blocks(mw, mh, compressed)
        s += bw * bh * bpb
    return s


def ps4_tiled_size(w, h, mips, bpb, compressed=True):
    s = 0
    for mw, mh in mip_dims(w, h, mips):
        bw, bh = blocks(mw, mh, compressed)
        s += ((bw + 7) // 8 * 8) * ((bh + 7) // 8 * 8) * bpb
    return s


# --------------------------------------------------------- micro tiling
def _morton_xy(i):
    x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4)
    y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4)
    return x, y


MORTON8 = [_morton_xy(i) for i in range(64)]


def tile_mip(lin, bw, bh, bpb):
    """linear block array (bw x bh blocks) -> PS4 1D thin micro-tiled bytes (padded)."""
    pw, ph = (bw + 7) // 8 * 8, (bh + 7) // 8 * 8
    src = np.frombuffer(lin, dtype=np.uint8).reshape(bh, bw, bpb)
    pad = np.zeros((ph, pw, bpb), dtype=np.uint8)
    pad[:bh, :bw] = src
    # replicate edge blocks into the padding (keeps bilinear sampling of last mips sane)
    if pw > bw:
        pad[:bh, bw:] = pad[:bh, bw - 1:bw]
    if ph > bh:
        pad[bh:, :] = pad[bh - 1:bh, :]
    tx, ty = pw // 8, ph // 8
    t = pad.reshape(ty, 8, tx, 8, bpb).transpose(0, 2, 1, 3, 4)  # ty,tx,y,x,b
    order_y = np.array([y for x, y in MORTON8])
    order_x = np.array([x for x, y in MORTON8])
    out = t[:, :, order_y, order_x, :]  # ty,tx,64,b
    return out.tobytes()


def detile_mip(data, bw, bh, bpb):
    pw, ph = (bw + 7) // 8 * 8, (bh + 7) // 8 * 8
    tx, ty = pw // 8, ph // 8
    t = np.frombuffer(data[:pw * ph * bpb], dtype=np.uint8).reshape(ty, tx, 64, bpb)
    img = np.zeros((ty, tx, 8, 8, bpb), dtype=np.uint8)
    order_y = np.array([y for x, y in MORTON8])
    order_x = np.array([x for x, y in MORTON8])
    img[:, :, order_y, order_x, :] = t
    img = img.transpose(0, 2, 1, 3, 4).reshape(ph, pw, bpb)
    return img[:bh, :bw].tobytes()


def ps3_to_ps4_texels(data, w, h, mips, bpb, compressed=True):
    out = bytearray()
    o = 0
    for mw, mh in mip_dims(w, h, mips):
        bw, bh = blocks(mw, mh, compressed)
        n = bw * bh * bpb
        out += tile_mip(data[o:o + n], bw, bh, bpb)
        o += n
    return bytes(out)


def ps4_mip0_linear(data, w, h, bpb, compressed=True):
    bw, bh = blocks(w, h, compressed)
    return detile_mip(data, bw, bh, bpb)


# ------------------------------------------------------------ decoding
def _c565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def decode_bc1(lin, bw, bh, alpha=True):
    a = np.frombuffer(lin[:bw * bh * 8], dtype='<u2').reshape(-1, 4)
    c0, c1 = a[:, 0].astype(np.int32), a[:, 1].astype(np.int32)
    idx = (a[:, 2].astype(np.uint32) | (a[:, 3].astype(np.uint32) << 16))
    p0, p1 = _c565(c0), _c565(c1)
    four = (c0 > c1)[:, None]
    p2 = np.where(four, (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four, (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # n,4,3
    alp = np.full((len(c0), 4), 255)
    if alpha:
        alp[:, 3] = np.where(four[:, 0], 255, 0)
    sel = np.stack([(idx >> (2 * i)) & 3 for i in range(16)], 1)  # n,16
    rgb = np.take_along_axis(pal, sel[:, :, None].repeat(3, 2), 1)
    al = np.take_along_axis(alp, sel, 1)
    px = np.concatenate([rgb, al[:, :, None]], 2).reshape(bh, bw, 4, 4, 4)
    return px.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4).astype(np.uint8)


def _bc4_block(a8):
    # a8: n x 8 uint8 -> n x 16 values
    a0, a1 = a8[:, 0].astype(np.int32), a8[:, 1].astype(np.int32)
    bitsv = np.zeros(len(a0), dtype=np.uint64)
    for i in range(6):
        bitsv |= a8[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = np.stack([((bitsv >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int32) for i in range(16)], 1)
    pal = np.zeros((len(a0), 8), dtype=np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        pal[:, 1 + i] = np.where(big, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, 1 + i] = np.where(big, pal[:, 1 + i], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(big, pal[:, 6], 0)
    pal[:, 7] = np.where(big, pal[:, 7], 255)
    return np.take_along_axis(pal, sel, 1)


def decode_bc3(lin, bw, bh):
    a = np.frombuffer(lin[:bw * bh * 16], dtype=np.uint8).reshape(-1, 16)
    alpha = _bc4_block(a[:, :8])
    col = decode_bc1(a[:, 8:].tobytes(), bw, bh, alpha=False)
    al = alpha.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
    col[:, :, 3] = al
    return col


def decode_bc2(lin, bw, bh):
    a = np.frombuffer(lin[:bw * bh * 16], dtype=np.uint8).reshape(-1, 16)
    col = decode_bc1(a[:, 8:].tobytes(), bw, bh, alpha=False)
    al = np.zeros((len(a), 16), dtype=np.int32)
    for i in range(8):
        al[:, 2 * i] = (a[:, i] & 15) * 17
        al[:, 2 * i + 1] = (a[:, i] >> 4) * 17
    col[:, :, 3] = al.reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
    return col


def decode(lin, w, h, kind):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    if kind in ('DXT1', 'BC1'):
        img = decode_bc1(lin, bw, bh)
    elif kind in ('DXT3', 'BC2'):
        img = decode_bc2(lin, bw, bh)
    elif kind in ('DXT5', 'BC3'):
        img = decode_bc3(lin, bw, bh)
    elif kind == 'A8R8G8B8':
        a = np.frombuffer(lin[:w * h * 4], dtype=np.uint8).reshape(h, w, 4)
        return a[:, :, [1, 2, 3, 0]].copy()
    else:
        raise ValueError(kind)
    return img[:h, :w]


def save_png(img, path):
    from PIL import Image
    Image.fromarray(img, 'RGBA').save(path)


# ---------------------------------------------------------------- recovered f525859 additions (2026-11-05)
# The f525859 texcodec.py (agent Q: PS3 A8R8G8B8 sky maps and GCM remaps re-encoded as BC3) was lost. These
# functions were rebuilt from the installed overlay-r3 sky paks: the BC1 colour part reproduces 26985/26985
# non-flat blocks of sky-air-multi-color (and its endpoints 100 %); the BC3 alpha block is the analogous rule
# (only all-zero alpha was available to check).

def ps3_deswizzle_argb8(data, w, h, remap=0xaae4):
    """PS3 swizzled (Morton, x bit first) 32-bit texels -> (h, w, 4) uint8 RGBA after the GCM remap."""
    a = np.frombuffer(bytes(data[:w * h * 4]), np.uint8).reshape(-1, 4)
    ys, xs = np.mgrid[0:h, 0:w]
    idx = np.zeros((h, w), np.int64)
    b = 0
    ww, hh, xx, yy = w, h, xs.copy(), ys.copy()
    while ww > 1 or hh > 1:
        if ww > 1:
            idx |= (xx & 1) << b; xx >>= 1; ww >>= 1; b += 1
        if hh > 1:
            idx |= (yy & 1) << b; yy >>= 1; hh >>= 1; b += 1
    return gcm_remap(a[idx], remap)


def gcm_remap(argb, remap):
    """(.., 4) texels in stored A,R,G,B byte order -> RGBA by the GCM remap word (low byte: 2-bit source per output
    A,R,G,B from bit 0; high byte: per-output mode 2 = remap, 0 = zero, 1 = one)."""
    argb = np.asarray(argb)
    sel = [(remap >> (2 * k)) & 3 for k in range(4)]
    mode = [(remap >> (8 + 2 * k)) & 3 for k in range(4)]
    out = []
    for k in range(4):
        if mode[k] == 0:
            out.append(np.zeros(argb.shape[:-1], argb.dtype))
        elif mode[k] == 1:
            out.append(np.full(argb.shape[:-1], 255, argb.dtype))
        else:
            out.append(argb[..., sel[k]])
    a, r, g, b = out
    return np.stack([r, g, b, a], -1)


def encode_bc3(img):
    """(h, w, 4) uint8 RGBA -> BC3 blocks, row-major. Colour: bounding-box endpoints rounded to 565 (c0 = max,
    c1 = min), palette Q*255/scale with 1/3 steps, nearest by squared RGB distance (float32), flat block -> index 0.
    Alpha: a0 = max, a1 = min, 8-step palette, nearest; flat -> index 0."""
    h, w = img.shape[:2]
    bw, bh = (w + 3) // 4, (h + 3) // 4
    pad = np.zeros((bh * 4, bw * 4, 4), np.uint8)
    pad[:h, :w] = img                                # mips below 4x4: zero padding (as the 10-27 original)
    B = pad.reshape(bh, 4, bw, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    f = B.astype(np.float32)
    sc = np.array([31, 63, 31], np.float32)
    mx = f[:, :, :3].max(1); mn = f[:, :, :3].min(1)
    Q0 = np.round(mx * sc / np.float32(255)); Q1 = np.round(mn * sc / np.float32(255))
    E0, E1 = Q0 * np.float32(255) / sc, Q1 * np.float32(255) / sc
    P = np.stack([E0, E1, (2 * E0 + E1) / np.float32(3), (E0 + 2 * E1) / np.float32(3)], 1)
    d = ((f[:, :, None, :3] - P[:, None, :, :]) ** 2).sum(-1)
    ix = d.argmin(-1).astype(np.uint32)
    q0 = Q0.astype(np.uint32); q1 = Q1.astype(np.uint32)
    c0 = (q0[:, 0] << 11) | (q0[:, 1] << 5) | q0[:, 2]
    c1 = (q1[:, 0] << 11) | (q1[:, 1] << 5) | q1[:, 2]
    flat = c0 == c1
    ix[flat] = 0
    cidx = (ix << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    al = f[:, :, 3]
    a0 = al.max(1); a1 = al.min(1)
    k = np.arange(8, dtype=np.float32)
    w0 = np.array([7, 0, 6, 5, 4, 3, 2, 1], np.float32) / np.float32(7)
    AP = a0[:, None] * w0[None, :] + a1[:, None] * (np.float32(1) - w0[None, :])
    ai = np.abs(al[:, :, None] - AP[:, None, :]).argmin(-1).astype(np.uint64)
    ai[a0 == a1] = 0
    abits = (ai << (3 * np.arange(16, dtype=np.uint64))).sum(1).astype(np.uint64)
    out = np.zeros((B.shape[0], 16), np.uint8)
    out[:, 0] = a0.astype(np.uint8); out[:, 1] = a1.astype(np.uint8)
    for j in range(6):
        out[:, 2 + j] = ((abits >> np.uint64(8 * j)) & np.uint64(255)).astype(np.uint8)
    out[:, 8] = c0 & 255; out[:, 9] = c0 >> 8; out[:, 10] = c1 & 255; out[:, 11] = c1 >> 8
    for j in range(4):
        out[:, 12 + j] = (cidx >> (8 * j)) & 255
    return out.tobytes()
