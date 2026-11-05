#!/usr/bin/env python3
"""PS3 SCREAM sound bank (.bnk, SBlk v4/v5, big-endian) -> PS4 SCREAM bank (SBlk v13, little-endian).

Rewritten from scratch on 2026-11-05 from the 124 overlay-r3 banks that the lost cloud converter produced on
2026-10-27 (b34.tar / glob.tar), using the PS3 originals as the rosetta input. Layout of both formats as far
as the converter needs it is described next to the code. The SPU ADSR -> PS4 envelope table (adsr_env.json) and its nearest-pair fallback are the original converter's.

Audio: PS3 PS-ADPCM samples are copied unchanged (with a 0x20-byte PS4 sample header). PS3 MP3 samples
(codec 3) are decoded (ffmpeg) and re-encoded to PS-ADPCM by vagenc.py (exact port of the original vagenc.c).

Library use: convert(ps3_bytes, mp3_decoder=None) -> ps4_bytes."""
import json, os, struct

HERE = os.path.dirname(os.path.abspath(__file__))
_T = None


def tables():
    """adsr_env.json: the original converter's table (2026-10-27, 142 ADSR1:ADSR2 pairs learned from 57 retail
    PS3/PS4 bank pairs) -> PS4 envelope words 1..13 (hex)."""
    global _T
    if _T is None:
        raw = json.load(open(os.path.join(HERE, 'adsr_env.json')))
        _T = {tuple(int(x, 16) for x in k.split(':')): v for k, v in raw.items()}
    return _T


def be32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def sw32(b):
    """big-endian u32 words -> little-endian (len multiple of 4)"""
    n = len(b) // 4
    return struct.pack('<%dI' % n, *struct.unpack('>%dI' % n, b[:n * 4])) + b[n * 4:]


def f32(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


ONE = 0x3f800000
NAMES_LAST = True
SWAP24 = {0x1a, 0x1c, 0x1d, 0x20, 0x25}
AUDIO_SIZE = [0]
DATA_TYPES = {1, 4, 5, 6, 7, 0x2d, 0x2f, 0x30, 0x32}


# ---------------------------------------------------------------------------------------------- PS3 parse
def parse_ps3(d):
    h = struct.unpack_from('>8I', d, 0)
    s = h[2]
    H = struct.unpack_from('>16I', d, s)
    if H[0] != 0x6b6c4253 or H[1] not in (4, 5):
        raise ValueError('not a PS3 SBlk v4/v5 bank')
    r = dict(h=h, H=H, ver=H[1], flags=H[2], nsnd=H[5], ngr=H[6] >> 16, nnm=H[6] & 0xffff,
             so=H[7], go=H[8], dsz=H[10], gdo=H[13], no=H[14], ne=H[15], s=s)
    r['snd'] = [d[s + r['so'] + 12 * i:s + r['so'] + 12 * i + 12] for i in range(r['nsnd'])]
    r['gr'] = [d[s + r['go'] + 8 * i:s + r['go'] + 8 * i + 8] for i in range(r['ngr'])]
    r['gd'] = d[s + r['gdo']:s + r['no']]
    end = r['ne'] or h[3]
    r['names'] = d[s + r['no']:s + end]
    r['tail'] = d[s + end:s + h[3]]
    r['audio'] = d[h[4]:h[4] + h[5]]
    return r


# ---------------------------------------------------------------------------------------------- pieces
def conv_sound(p):
    """PS3 sound 12 bytes -> PS4 0x24 bytes"""
    return (b'\x01\0\0\0' + bytes([p[0], p[1], p[2], p[3], p[4], p[5], p[7], p[6] | 1]) + p[8:12][::-1]
            + struct.pack('<I', ONE) + bytes(16))


def conv_names(a):
    if not a:
        return a
    o = bytearray(a)
    h = struct.unpack_from('>4I', a, 0)
    struct.pack_into('<4I', o, 0, *h)
    for p in range(h[2], h[3], 16):
        struct.pack_into('<II4H', o, p, *struct.unpack_from('>II4H', a, p))
    return bytes(o)


def ps3_block_extent(r):
    """PS3 grain-data blocks: offset -> end (next referenced offset)"""
    offs = sorted(set(int.from_bytes(g[1:4], 'big') for g in r['gr'] if g[0] in DATA_TYPES) | {len(r['gd'])})
    return {o: offs[i + 1] if i + 1 < len(offs) else o for i, o in enumerate(offs)}


def conv_records(b, pos):
    """type-1/0x2d record list after the base words, up to the (0,0) terminator. Returns PS4 bytes."""
    out = b''
    while pos + 8 <= len(b):
        t, sz = struct.unpack_from('>II', b, pos)
        if t == 0 and sz == 0:
            break
        if sz < 8 or pos + sz > len(b):
            break
        body = b[pos + 8:pos + sz]
        if t == 1:      # filter: u16 pair, f freq, f q/bandwidth, f gain, u32 mode
            x = struct.unpack_from('>HH', body, 0)
            fa, fb, fc = struct.unpack_from('>3f', body, 4)
            fa = float(int(fa + 0.5))       # the PS4 filter frequency is a whole number of Hz
            mode = be32(body, 16)
            if mode in (7, 8):
                fb = struct.unpack('>f', struct.pack('>f', fb))[0] / 12.0
            out += struct.pack('<II', 1, 0x1c) + struct.pack('<HH', *x) + struct.pack('<3f', fa, fb, fc) + struct.pack('<I', mode)
        elif t == 2:    # -> PS4 0xb, one extra zero word
            out += struct.pack('<II', 0xb, 0x18) + sw32(body) + bytes(4)
        elif t == 7:    # dropped
            pass
        elif t == 3:    # -> PS4 type 9 (0x48 bytes); the reference wrote a fixed default body
            out += struct.pack('<II', 9, 0x48) + struct.pack('<16f', *([-1.0] * 8 + [1.0] * 8))
        else:           # unknown record type (also what a v4 0x2d block yields at the v5 offset): stop
            break
        pos += sz
    return out + bytes(8)


def env_formula(a1, a2):
    """the original converter's fallback for pairs no retail bank shows: nearest learned pair (same modes,
    closest rates); verified to reproduce all 186 pairs used by the 124 reference banks"""
    best = None
    for (b1, b2), v in tables().items():
        d = abs(((a1 >> 8) & 0x7f) - ((b1 >> 8) & 0x7f)) + abs(((a1 >> 4) & 15) - ((b1 >> 4) & 15)) * 2 +             abs((a1 & 15) - (b1 & 15)) * 2 + abs(((a2 >> 6) & 0x7f) - ((b2 >> 6) & 0x7f)) + abs((a2 & 31) - (b2 & 31)) * 2 +             (100 if (a1 ^ b1) & 0x8000 or (a2 ^ b2) & 0xe020 else 0)
        if best is None or d < best[0]:
            best = (d, v)
    return best[1]


def envelope(a1, a2):
    """PS4 envelope words 0..13 (word 0 is always 0)"""
    v = tables().get((a1, a2))
    if v is None:
        v = env_formula(a1, a2)
    return bytes(4) + bytes.fromhex(v)


def conv_type1(b, ver, audio_off, audio_size):
    note, fine = b[2], b[3]
    a1, a2 = struct.unpack_from('>HH', b, 10)
    base = 7 if ver == 4 else 9
    fl = be32(b, 24)
    o = bytearray(0x88)
    struct.pack_into('<I', o, 0, 1)
    o[4:8] = bytes([b[0], b[1], b[5], b[4]])
    o[8:12] = bytes([0, 0, b[8], b[9]])
    struct.pack_into('<HH', o, 12, a1, a2)
    o[16:20] = bytes([0, 0, 1 | (b[15] & 0x60), 0xcb])
    struct.pack_into('<I', o, 20, 0 if fl == ONE else fl)
    struct.pack_into('<8f', o, 24, 1, 1, -1, -1, -1, -1, -1, -1)
    struct.pack_into('<III', o, 68, audio_off, audio_size, f32(24000.0 * 2 ** ((note - 184 + fine / 128.0) / 12.0)))
    o[80:0x88] = envelope(a1, a2)
    rec = conv_records(b, base * 4) if b[14] & 1 else bytes(8)
    return bytes(o) + rec


def conv_type2d(b, ver, name_off, x20):
    """stream grain (XVAG file reference). The params are taken from the fixed v5 offsets also for v4 banks,
    as the reference converter did."""
    a1, a2 = struct.unpack_from('>HH', b, 10)
    o = bytearray(0x6c)
    struct.pack_into('<I', o, 0, 1)
    o[4:8] = bytes([b[0], b[1], b[5], b[4]])
    o[8:12] = bytes([0, 0, b[8], b[9]])
    struct.pack_into('<HH', o, 12, a1, a2)
    o[16:20] = bytes([0, 0, 1 | (b[15] & 0x60), 0xc3])
    fl = be32(b, 36) if ver == 4 else be32(b, 36)
    struct.pack_into('<I', o, 20, 0 if fl == ONE else fl)
    struct.pack_into('<8f', o, 24, 1, 1, -1, -1, -1, -1, -1, -1)
    struct.pack_into('<IIII', o, 68, name_off, be32(b, 20), be32(b, 24), x20)
    prm = sw32(b[48:84])
    env = envelope(a1, a2)[4:]
    rec = conv_records(b, 84) if b[14] & 1 else bytes(8)
    return bytes(o) + prm + env + rec


def conv_block(t, b, ver, ctx):
    if t == 5 or t == 6:
        # u32 words 0,1,3,4 (word 2 is four u8 fields); type 5 keeps a trailing name string as bytes, type 6 drops it
        o = sw32(b[0:8]) + b[8:12] + sw32(b[12:20])
        return o + (b[20:] if t == 5 else b'')
    if t == 7:
        return sw32(b[:4]) + b[4:]
    if t == 0x30:
        return b[:24]
    if t == 0x32:
        return sw32(b)
    if t == 0x2f:
        return bytes([b[0], b[1], b[3], 0])
    if t == 4:
        return (b[0:4] + bytes([b[5], b[4], b[7], b[6], b[9], b[8], b[11], b[10]]) + b[12:16][::-1]
                + bytes(4) + struct.pack('<f', 1.0) + bytes(8))
    raise KeyError('grain data type %#x' % t)


# ---------------------------------------------------------------------------------------------- audio
def adpcm_info(data):
    """samples / loop start / loop end of a PS-ADPCM stream (PS4 sample header fields)"""
    nfr = len(data) // 16
    end = nfr
    ls = le = 0xffffffff
    for i in range(nfr):
        f = data[i * 16 + 1]
        if f & 4 and ls == 0xffffffff:
            ls = i * 28
        if f & 1:
            end = i + 1
            break
    if ls != 0xffffffff:
        le = end * 28 - ls          # loop length
    return end * 28, ls, le


def sample_header(samples, ls, le):
    return struct.pack('<8I', 0, 1, 0x10, 0, samples, 1, ls, le)


# ---------------------------------------------------------------------------------------------- convert
def convert(d, mp3_to_adpcm=None, audio_override=None):
    r = parse_ps3(d)
    ver = r['ver']
    ext = ps3_block_extent(r)
    A = r['audio']
    # audio: unique samples (type-1 grains) and stream file names (type-0x2d grains), in first-reference
    # order of the grain table; each name is the NUL-terminated string at the PS3 offset
    order = []
    seen = set()
    pcm = set()                 # samples flagged PCM16 (grain byte 15 bit 7)
    for g in r['gr']:
        if g[0] not in (1, 0x2d):
            continue
        v = int.from_bytes(g[1:4], 'big')
        b = r['gd'][v:ext[v]]
        k = ('s', be32(b, 16), be32(b, 20)) if g[0] == 1 else ('n', be32(b, 16), 0)
        if k not in seen:
            seen.add(k)
            order.append(k)
        if g[0] == 1 and b[15] & 0x80:
            pcm.add(k)
    if NAMES_LAST:
        order = [k for k in order if k[0] == 's'] + [k for k in order if k[0] == 'n']
    audio4 = bytearray()
    amap = {}
    for k in order:
        kind, ao, asz = k
        if kind == 'n':
            e = A.find(bytes(1), ao) + 1 or len(A)
            blob = A[ao:e]
        else:
            src = A[ao:ao + asz]
            if audio_override is not None and (ao, asz) in audio_override:
                blob = audio_override[(ao, asz)]
            elif k not in pcm and (asz == 0 or struct.unpack_from('>II', src, 0) != (1, 3)):   # PS-ADPCM: copy
                smp, ls, le = adpcm_info(src)
                blob = sample_header(smp, ls, le) + src
            else:
                if mp3_to_adpcm is None:
                    raise RuntimeError('MP3 sample needs a decoder')
                blob = mp3_to_adpcm(A, ao, asz)
        amap[k] = (len(audio4), len(blob))
        audio4 += blob
    AUDIO_SIZE[0] = len(audio4)
    # grain data
    gd4 = bytearray()
    gr4 = []
    for g in r['gr']:
        t = g[0]
        v = int.from_bytes(g[1:4], 'big')
        w = g[4:8][::-1]
        if t == 0x19:
            blk = bytes([g[1], g[2], g[3], (g[1] - 1) & 0xff, 0]) + b'\xff' * 16 + bytes(3)
            gr4.append(struct.pack('<I', 0x30 << 24 | len(gd4)) + w)
            gd4 += blk
        elif t == 1:
            b = r['gd'][v:ext[v]]
            ao, asz = be32(b, 16), be32(b, 20)
            no, ns = amap[('s', ao, asz)]
            gr4.append(struct.pack('<I', 1 << 24 | len(gd4)) + w)
            gd4 += conv_type1(b, ver, no, ns)
        elif t == 0x2d:
            b = r['gd'][v:ext[v]]
            gr4.append(struct.pack('<I', t << 24 | len(gd4)) + w)
            gd4 += conv_type2d(b, ver, amap[('n', be32(b, 16), 0)][0], AUDIO_SIZE[0])
        elif t in DATA_TYPES:
            b = r['gd'][v:ext[v]]
            gr4.append(struct.pack('<I', t << 24 | len(gd4)) + w)
            gd4 += conv_block(t, b, ver, None)
        else:
            p3 = bytes([g[3], g[2], g[1]]) if t in SWAP24 else g[1:4]   # u24 value vs three u8 params
            gr4.append(p3 + bytes([t]) + w)
    # sblk
    nsnd, ngr = r['nsnd'], r['ngr']
    go = 0x50 + ((0x24 * nsnd + 15) & ~15)
    gdo = (go + 8 * ngr + 15) & ~15
    gd4 += bytes(-len(gd4) & 15)
    no = gdo + len(gd4)
    names = conv_names(r['names'])
    ne = no + len(names)
    end = ne + len(r['tail'])
    hdr = struct.pack('<4sIIII', b'SBlk', 13, 5, 0, 0) + struct.pack('<I', r['flags'] & 0xff00)
    hdr += struct.pack('<IIIIIIII', 0x50, go, 0, end, no, gdo, no, ne)
    hdr += struct.pack('<HHHH', nsnd, ngr, r['nnm'], 0) + bytes(16)
    snd = b''.join(conv_sound(p) for p in r['snd'])
    sb = hdr + snd + bytes(go - 0x50 - len(snd)) + b''.join(gr4) + bytes(gdo - go - 8 * ngr) + bytes(gd4) + names + r['tail']
    sb += bytes.fromhex('000000000100000000')
    sblk_size = len(sb)
    doff = (0x20 + sblk_size + 15) & ~15
    head = struct.pack('<8I', 3, 2, 0x20, sblk_size, doff, len(audio4), 0, 0)
    return head + sb + bytes(doff - 0x20 - sblk_size) + bytes(audio4)
