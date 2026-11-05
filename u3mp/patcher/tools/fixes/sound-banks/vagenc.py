#!/usr/bin/env python3
"""PS-ADPCM (VAG) encoder of the sound-bank converter: an exact pure-Python port of the original 2026-10-27
cloud encoder (third_party/vagenc-oct27-src/vagenc-oct27/sound/c/vagenc.c, built there with gcc -O2, no FMA).

Per 28-sample block, for each filter 0..4:
  1. open-loop residual against the original samples (seeded with the encoder state for the first two
     predictions); shift = largest sh (12 down to 0) with peak <= (7 << (12 - sh)) - 1;
  2. closed-loop quantisation at sh and at sh-1 (floor 0): nibble = lrint((x - pred) * 2^shc / 4096) (round
     half to even, like C lrint and Python round()), clamped to [-8, 7]; y = ((nibble * 4096) >> shc) + pred
     clamped to the int16 range but kept as a double;
  3. the lowest summed squared error wins (strict <, order filter 0..4, then sh, sh-1); the winner's double
     state (y[-1], y[-2]) is carried into the next block.
Flags: one-shot (no loop) = flag 1 on the last block; loop=(start, end) sets 6 / 3 like the original.
Python floats are IEEE doubles and every operation is done in the C order, so the output is bit-identical.
The earlier approximate encoder (before the original source was recovered) is vagenc_approx.py.

encode(samples, loop=None) -> bytes   samples: sequence of int16 (any length; padded with zeros to 28)."""

F1 = (0.0, 60 / 64.0, 115 / 64.0, 98 / 64.0, 122 / 64.0)
F2 = (0.0, 0.0, -52 / 64.0, -55 / 64.0, -60 / 64.0)
LIM = [float((7 << (12 - sh)) - 1) for sh in range(13)]


def encode(pcm, loop=None):
    n = len(pcm)
    nblk = (n + 27) // 28 if n > 0 else 1
    s1 = s2 = 0.0
    out = bytearray(16 * nblk)
    for b in range(nblk):
        blk = [float(pcm[i]) if i < n else 0.0 for i in range(b * 28, b * 28 + 28)]
        best = 1e300
        bf = bs = 0
        bn = [0] * 28
        b1 = b2 = 0.0
        for fi in range(5):
            f1 = F1[fi]; f2 = F2[fi]
            p1 = s1; p2 = s2; peak = 0.0
            for x in blk:
                r = x - (p1 * f1 + p2 * f2)
                if abs(r) > peak:
                    peak = abs(r)
                p2 = p1; p1 = x
            sh = 12
            while sh > 0 and peak > LIM[sh]:
                sh -= 1
            for t in (0, 1):
                shc = sh if t == 0 else (sh - 1 if sh > 0 else 0)
                scale = float(1 << shc)
                q1 = s1; q2 = s2; err = 0.0
                nib = []
                for x in blk:
                    pred = q1 * f1 + q2 * f2
                    vi = round((x - pred) * scale / 4096.0)
                    if vi < -8:
                        vi = -8
                    if vi > 7:
                        vi = 7
                    y = float((vi * 4096) >> shc) + pred
                    if y > 32767:
                        y = 32767.0
                    if y < -32768:
                        y = -32768.0
                    d = x - y
                    err += d * d
                    nib.append(vi)
                    q2 = q1; q1 = y
                if err < best:
                    best = err; bf = fi; bs = shc; bn = nib; b1 = q1; b2 = q2
        s1 = b1; s2 = b2
        fl = 0
        if loop is not None and loop[0] >= 0:
            if b == loop[0] // 28:
                fl |= 6
            if b == (loop[1] - 1) // 28:
                fl |= 3
        elif b == nblk - 1:
            fl = 1
        o = 16 * b
        out[o] = (bf << 4) | bs
        out[o + 1] = fl
        for k in range(0, 28, 2):
            out[o + 2 + k // 2] = (bn[k] & 15) | ((bn[k + 1] & 15) << 4)
    return bytes(out)
