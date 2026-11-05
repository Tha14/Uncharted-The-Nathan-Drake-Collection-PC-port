#!/usr/bin/env python3
"""Local page images -> vtex DDS (DXT5, mips, 1024x1024). Usage: convert_eula.py SRC_DIR [N]
SRC_DIR holds BCES-EN-1.jpg .. BCES-EN-<N>.jpg (default N=7). Output: out/vtex1/ps4/eula/p<i>.dds next to this script."""
import os, struct, subprocess, sys
FF = r'C:\msys64\clang64\bin\ffmpeg.exe'
HERE = os.path.dirname(os.path.abspath(__file__))


def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def block(px):
    lum = [p[0] * 3 + p[1] * 6 + p[2] for p in px]
    lo = px[lum.index(min(lum))]
    hi = px[lum.index(max(lum))]
    c0, c1 = rgb565(*hi), rgb565(*lo)
    if c0 == c1:
        return struct.pack('<HHI', c0, c1, 0)
    if c0 < c1:
        c0, c1 = c1, c0
        lo, hi = hi, lo
    l0 = hi[0] * 3 + hi[1] * 6 + hi[2]
    l1 = lo[0] * 3 + lo[1] * 6 + lo[2]
    d = l0 - l1 or 1
    bits = 0
    for i, l in enumerate(lum):
        t = (l0 - l) * 3 / d
        s = 0 if t < .5 else 2 if t < 1.5 else 3 if t < 2.5 else 1
        bits |= s << (i * 2)
    return struct.pack('<HHI', c0, c1, bits)


def encode(raw, w, h):
    out = bytearray()
    alpha = b'\xff\xff' + bytes(6)
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            px = []
            for j in range(4):
                for i in range(4):
                    o = (min(y + j, h - 1) * w + min(x + i, w - 1)) * 3
                    px.append((raw[o], raw[o + 1], raw[o + 2]))
            out += alpha + block(px)
    return bytes(out)


def main():
    src = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 7
    dst = os.path.join(HERE, 'out', 'vtex1', 'ps4', 'eula')
    os.makedirs(dst, exist_ok=True)
    for p in range(1, n + 1):
        f = os.path.join(src, 'BCES-EN-%d.jpg' % p)
        if not os.path.isfile(f):
            sys.exit('missing ' + f)
        S = 1024
        h = bytearray(128)
        h[:4] = b'DDS '
        struct.pack_into('<IIIIIII', h, 4, 124, 0xa1007, S, S, S * S, 0, 11)
        struct.pack_into('<II4s', h, 76, 32, 4, b'DXT5')
        struct.pack_into('<I', h, 108, 0x401008)
        body = bytearray()
        w = S
        while True:
            raw = subprocess.run([FF, '-v', 'error', '-i', f, '-vf', 'scale=%d:%d:flags=lanczos' % (w, w), '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True, check=True).stdout
            body += encode(raw, w, w)
            if w == 1:
                break
            w //= 2
        open(os.path.join(dst, 'p%d.dds' % p), 'wb').write(bytes(h) + body)
        print('wrote p%d' % p)


main()
