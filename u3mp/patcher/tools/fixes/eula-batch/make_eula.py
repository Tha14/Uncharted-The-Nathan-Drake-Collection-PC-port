#!/usr/bin/env python3
"""EULA pages for the Options > User Agreement page: vtex1/ps4/eula/p1..p7.dds in overlay-r3.

Reproduces the 2026-11-01 22:21 build byte for byte (verified 2026-11-05: 7/7 sha256-identical):
  1. the 7 agreement pages BCES-EN-1..7.jpg (512x512) that the PS3 game downloaded from the EULA server into
     its http-cache ({{PS3_DATA2}}/http-cache/<hash name>, no extension). They are recognised by content
     sha256; the cache file names are only a fallback.
  2. Pillow: flip top-bottom (the vtex is stored bottom-up) and re-save as JPEG quality 95, subsampling 0.
     This lossy re-save is part of the reference (Pillow 13.0.0 was used).
  3. ffmpeg: decode + scale=W:W:flags=lanczos to rgb24 for every mip 1024 .. 1 (ffmpeg 8.1.1, msys2 clang64).
  4. the DXT5 encoder of convert_eula.py (alpha block constant 0xff), 128-byte DDS header, 11 mips.
Without ffmpeg (or with --no-ffmpeg) step 3 uses Pillow's LANCZOS resize: same layout and look, but not
byte-identical (the script then reports the pages as differing from the reference).

usage: make_eula.py [--cache DIR] [--out DIR] [--ffmpeg EXE] [--no-ffmpeg] [--apply]
       dry run by default; --apply writes (backup of replaced pages in work/backup-eula, once).
       Idempotent: when all 7 pages already equal the reference sha256 nothing is done."""
import argparse, hashlib, io, os, shutil, struct, subprocess, sys

PAGES = [  # (content sha256 of BCES-EN-<i>.jpg, cache file name prefix)
    ('f76a3288082e72e463355350bce48d4bef17d0fed1f0d08055265e24e56af890', '6532dd2818'),
    ('8409a516381b46351799948d0221226d99545054d3af0aec059d8f885deb7791', '234fe07708'),
    ('7c1e4764cdb4e0d1d8fb9c5a0ea489913cd0df2c45f5c27054a0f741ab0b6193', '75713af431'),
    ('d27549bffcf407d8d95d9e47efdd717e32666fde07454c3dbaebb16edcf32274', 'b6e53413d7'),
    ('e4acbaa48645be0876a01d09f046c0e0d9e50cfd090f544780b9ca2e433716dc', '9a4d4bc81c'),
    ('fb9794d128d11c8f106ade19b9d711d098cf3c056cb32987479782d72382c84a', 'f6d6249a0f'),
    ('4507c5145379b02a3c792f3cc40497cf52524369b4155d7ed43fed74843af3e4', '22f8e3c4db'),
]
EXPECT = [  # sha256 of the reference p1..p7.dds (recipe/manifest.tsv)
    '27bd9d43da2e70cb2df0f904cfa73613cfcf6156e38f79927bbe5e0f5d751415',
    '3a1418cf2252747ef50a465542ee9ae75342b40d13b54c6460c45e49039ab137',
    '1b179532e493e4181a8ceb9049d196ef656c4ec6dac1dd11322cffabda57e3ac',
    '22115bf206fec2e9e696119e533212daad8787d62b7baf7fb95f6b7154edd0bc',
    'c80441807b92cf923ec1cf4ecca6963e23b908d61cfe41793b18931bfd11e4b1',
    'f4f8809970734568c6d70d9b9fba4cac41134163bc50ed8ec2d03d5a1081c3e7',
    '792efc2bdbf92caf9c51870449f5826cff433bcc478490b3bf4630ad6a7901ef',
]
FF_DEFAULT = [os.environ.get('RECIPE_FFMPEG', ''), r'C:\msys64\clang64\bin\ffmpeg.exe', r'C:\msys64\ucrt64\bin\ffmpeg.exe']


def sha(b):
    return hashlib.sha256(b).hexdigest()


def rgb565(r, g, b):
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def block(px):  # unchanged from convert_eula.py
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


def find_pages(cache):
    if not os.path.isdir(cache):
        sys.exit('no http-cache folder: ' + cache)
    by_sha = {}
    names = sorted(os.listdir(cache))
    for n in names:
        p = os.path.join(cache, n)
        if os.path.isfile(p) and os.path.getsize(p) < (4 << 20):
            by_sha.setdefault(sha(open(p, 'rb').read()), p)
    out = []
    for i, (h, pre) in enumerate(PAGES):
        p = by_sha.get(h) or next((os.path.join(cache, n) for n in names if n.startswith(pre)), None)
        if not p:
            sys.exit('agreement page %d not in %s (start the PS3 game online once so it caches the pages)' % (i + 1, cache))
        out.append(p)
    return out


def dds(mips_raw):
    S = 1024
    h = bytearray(128)
    h[:4] = b'DDS '
    struct.pack_into('<IIIIIII', h, 4, 124, 0xa1007, S, S, S * S, 0, 11)
    struct.pack_into('<II4s', h, 76, 32, 4, b'DXT5')
    struct.pack_into('<I', h, 108, 0x401008)
    return bytes(h) + b''.join(encode(r, w, w) for w, r in mips_raw)


def build(jpg_bytes, ff):
    from PIL import Image
    im = Image.open(io.BytesIO(jpg_bytes)).convert('RGB').transpose(Image.FLIP_TOP_BOTTOM)
    buf = io.BytesIO()
    im.save(buf, format='JPEG', quality=95, subsampling=0)
    flipped = buf.getvalue()
    mips = []
    w = 1024
    while True:
        if ff:
            raw = subprocess.run([ff, '-v', 'error', '-f', 'jpeg_pipe', '-i', '-', '-vf', 'scale=%d:%d:flags=lanczos' % (w, w),
                                  '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], input=flipped, capture_output=True, check=True).stdout
        else:
            raw = Image.open(io.BytesIO(flipped)).convert('RGB').resize((w, w), Image.LANCZOS).tobytes()
        mips.append((w, raw))
        if w == 1:
            break
        w //= 2
    return dds(mips)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cache', default=r"{{PS3_DATA2}}/http-cache")
    ap.add_argument('--out', default=r"{{PROJECT}}/work/overlay-r3/u3data/build/main/vtex1/ps4/eula")
    ap.add_argument('--backup', default=r"{{PROJECT}}/work/backup-eula")
    ap.add_argument('--ffmpeg', default='')
    ap.add_argument('--no-ffmpeg', action='store_true')
    ap.add_argument('--apply', action='store_true')
    A = ap.parse_args()
    dst = [os.path.join(A.out, 'p%d.dds' % (i + 1)) for i in range(7)]
    if all(os.path.isfile(p) and sha(open(p, 'rb').read()) == e for p, e in zip(dst, EXPECT)):
        print('already applied: 7 EULA pages match the reference')
        return
    src = find_pages(A.cache)
    ff = None
    if not A.no_ffmpeg:
        for c in [A.ffmpeg] + FF_DEFAULT + [shutil.which('ffmpeg') or '']:
            if c and os.path.isfile(c):
                ff = c
                break
        if not ff:
            print('ffmpeg not found: using the Pillow resize (equivalent pages, not byte-identical)')
    ident = 0
    for i, (s, d) in enumerate(zip(src, dst)):
        data = build(open(s, 'rb').read(), ff)
        same = sha(data) == EXPECT[i]
        ident += same
        print('p%d.dds %s%s' % (i + 1, 'identical to the reference' if same else 'differs from the reference',
                                 '' if A.apply else ' (dry run)'))
        if A.apply:
            os.makedirs(A.out, exist_ok=True)
            if os.path.isfile(d):
                os.makedirs(A.backup, exist_ok=True)
                bk = os.path.join(A.backup, os.path.basename(d))
                if not os.path.exists(bk):
                    shutil.copyfile(d, bk)
            tmp = d + '.tmp'
            open(tmp, 'wb').write(data)
            os.replace(tmp, d)
    print('%s 7 pages, %d byte-identical to the reference' % ('wrote' if A.apply else 'would write', ident))


if __name__ == '__main__':
    main()
