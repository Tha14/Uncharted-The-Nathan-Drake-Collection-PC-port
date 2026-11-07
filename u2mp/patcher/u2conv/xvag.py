"""PS3 XVAG (big-endian header, PS-ADPCM 'codec 6') -> the PS4 XVAG form the Uncharted 2 PS4 game reads.

Retail PS4 U2 ships most audio as ATRAC9 (codec 9), but four music tracks are PS-ADPCM XVAGs ported from PS3
(146/148/149/150_JungleExplore_*): they are the reference pairs (validate.py xvag) for this conversion.
No re-encoding: the ADPCM frames are kept; only the container changes.

  header  'XVAG', u32 header size, u8 version, 0, 0, u8 flags, then 20 zero bytes   (PS3: 01 00 00 60,
          PS4: 02 00 00 62; every PS4 header word is little-endian)
  chunks  {tag, u32 size, data}:
    fmat  PS3 0x1c: channels, codec 6, samples, samples, 1, rate, data size
          PS4 0x24: the same 7 words (word 4 = interleave in 16-byte frames per channel; data size
                unpadded) + 1, channels
    cpan  per-channel panning: words swapped, except word 2 (a float stored little-endian on PS3 already)
    0000  end of header (size 0)
  data    PS3 interleaves channels frame by frame (16-byte PS-ADPCM frames: L R L R ...); PS4 stores blocks of
          <interleave> frames per channel (L*n R*n L*n ...), the last block zero-padded.
"""
import struct

PS4_INTERLEAVE = 0x155        # frames per channel and block (retail 148/149_JungleExplore use 0x155)


def _chunks(d, hs):
    o = 0x20
    out = []
    while o + 8 <= hs:
        tag = d[o:o + 4]
        size = struct.unpack_from('>I', d, o + 4)[0]
        out.append((tag, d[o + 8:o + 8 + size]))
        o += 8 + size
    return out


def info(d):
    """(channels, codec, rate, samples) of a PS3 or PS4 XVAG"""
    e = '<' if d[4] != 0 or d[5] != 0 else '>'
    ch, codec, ns, _, _, rate = struct.unpack_from(e + '6I', d, 0x28)
    return ch, codec, rate, ns


def convert(d, interleave=PS4_INTERLEAVE):
    if d[:4] != b'XVAG':
        raise ValueError('not an XVAG file')
    hs = struct.unpack_from('>I', d, 4)[0]
    if hs > 0x1000 or d[4:6] != b'\0\0':
        raise ValueError('not a big-endian (PS3) XVAG')
    chunks = _chunks(d, hs)
    tags = [t for t, _ in chunks]
    if tags[0] != b'fmat' or tags[-1] != b'0000' or any(t not in (b'fmat', b'cpan', b'0000') for t in tags):
        raise ValueError('unexpected XVAG chunks %s' % tags)
    f = struct.unpack('>7I', chunks[0][1][:28])
    ch, codec, ns, ns2, _one, rate, dsize = f
    if codec != 6:
        raise ValueError('XVAG codec %d (only PS-ADPCM, codec 6, is converted)' % codec)
    data = d[hs:hs + dsize]
    if len(data) != dsize or dsize % (16 * ch):
        raise ValueError('XVAG data size %d does not hold whole frames for %d channels' % (dsize, ch))
    nfr = dsize // 16 // ch                       # frames per channel
    il = interleave if ch > 1 else max(1, min(interleave, nfr))
    if ch > 1:
        frames = [data[16 * k:16 * k + 16] for k in range(dsize // 16)]
        per = [frames[c::ch] for c in range(ch)]
        nblk = -(-nfr // il)
        out = bytearray()
        for b in range(nblk):
            for c in range(ch):
                blk = b''.join(per[c][b * il:(b + 1) * il])
                out += blk + bytes(16 * il - len(blk))
        data = bytes(out)
    else:
        data = data + bytes((-len(data)) % (16 * il))
    body = bytearray()
    for tag, c in chunks:
        if tag == b'fmat':
            c2 = struct.pack("<7I", ch, codec, ns, ns2, il, rate, dsize) + struct.pack("<II", 1, ch)
        elif tag == b'cpan':
            n = len(c) // 4
            w = list(struct.unpack('>%dI' % n, c[:4 * n]))
            c2 = bytearray(struct.pack('<%dI' % n, *w))
            if n > 2:
                c2[8:12] = c[8:12]                # float already in PS4 byte order
            c2 = bytes(c2) + c[4 * n:]
        else:
            c2 = c
        body += tag + struct.pack('<I', len(c2)) + c2
    hdr = b'XVAG' + struct.pack('<I', 0x20 + len(body)) + bytes([2, 0, 0, 0x62]) + bytes(20)
    return hdr + bytes(body) + data
