#!/usr/bin/env python3
"""PS-ADPCM (VAG) encoder used for the PS3 MP3 / PCM samples of the sound banks.

Quantiser: the classic AIFF2VAG pack() (noise-shaped, double precision, round by (int(x)+0x800) & ~0xfff).
Shift per filter: the classic find_predict() rule from the open-loop residual maximum, made monotonic
(a residual >= 0x7800 gets shift 0; the classic loop returns 1 there and clips loud blocks).
Filter: the one whose block, decoded by the real PS-ADPCM decoder (integer prediction, 16-bit clamp,
from the decoded history), has the smallest squared error against the input.

The lost reference encoder of 2026-10-27 was not recovered exactly: this one picks the same filter/shift in
about 70-85 % of the blocks, with the same quality (chloe.bnk sample 0: 20.97 dB SNR against the decoded
MP3, reference 21.26 dB; test.bnk PCM sample: 55.5 dB, reference 55.3 dB). See GAPS.md.

encode(samples) -> bytes   samples: list of int16, length a multiple of 28 (pad before calling)."""

F0 = (0.0, -60.0 / 64.0, -115.0 / 64.0, -98.0 / 64.0, -122.0 / 64.0)
F1 = (0.0, 0.0, 52.0 / 64.0, 55.0 / 64.0, 60.0 / 64.0)
K0 = (0, 60, 115, 98, 122)
K1 = (0, 0, -52, -55, -60)


def _shift(mx):
    m2 = int(mx)
    if m2 >= 0x7800:
        return 0
    mask = 0x4000
    sh = 0
    while sh < 12:
        if mask & (m2 + (mask >> 3)):
            break
        sh += 1
        mask >>= 1
    return sh


def encode(samples):
    n = len(samples)
    nb = n // 28
    out = bytearray(nb * 16)
    fs1 = fs2 = 0.0          # open-loop history (clamped input samples)
    ps1 = ps2 = 0.0          # noise-feedback state of the quantiser
    h1 = h2 = 0              # decoder history (what the console will have decoded)
    for b in range(nb):
        blk = samples[b * 28:b * 28 + 28]
        xs = [30719.0 if s > 30719 else -30720.0 if s < -30720 else float(s) for s in blk]
        best = None
        for i in range(5):
            f0 = F0[i]; f1 = F1[i]; k0 = K0[i]; k1 = K1[i]
            s1 = fs1; s2 = fs2
            d = []
            mx = 0.0
            for s0 in xs:
                v = s0 + s2 * f1 + s1 * f0
                d.append(v)
                a = v if v >= 0 else -v
                if a > mx:
                    mx = a
                s2 = s1; s1 = s0
            sh = _shift(mx)
            scale = float(1 << sh)
            e1 = ps1; e2 = ps2
            y1 = h1; y2 = h2
            err = 0
            nib = []
            lim = best[0] if best is not None else None
            for j in range(28):
                s0 = d[j] + e1 * f0 + e2 * f1
                di = (int(s0 * scale) + 0x800) & -4096
                if di > 32767:
                    di = 32767
                elif di < -32768:
                    di = -32768
                q = di >> 12
                nib.append(q & 15)
                e = float(di >> sh) - s0
                e2 = e1; e1 = e
                y = ((q << 12) >> sh) + ((y1 * k0 + y2 * k1 + 32) >> 6)
                if y > 32767:
                    y = 32767
                elif y < -32768:
                    y = -32768
                t = blk[j] - y
                err += t * t
                y2 = y1; y1 = y
                if lim is not None and err >= lim:
                    break
            else:
                best = (err, i, sh, nib, e1, e2, y1, y2)
        fs1 = xs[27]; fs2 = xs[26]
        _, pr, sh, nib, ps1, ps2, h1, h2 = best
        o = b * 16
        out[o] = (pr << 4) | sh
        out[o + 1] = 1 if b == nb - 1 else 0
        for k in range(14):
            out[o + 2 + k] = nib[2 * k] | (nib[2 * k + 1] << 4)
    return bytes(out)
