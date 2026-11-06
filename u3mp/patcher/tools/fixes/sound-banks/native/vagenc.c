/* Unchanged copy of third_party/vagenc-oct27-src/vagenc-oct27/sound/c/vagenc.c (the original 2026-10-27 encoder),
   exported for vagenc.py (ctypes). Build: build.sh (gcc -O2, no FMA contraction, like the original). */
#ifdef _WIN32
#define main_export __declspec(dllexport)
#else
#define main_export
#endif
/* PS-ADPCM (VAG) encoder: same algorithm as vag.py encode() (open-loop shift pick, closed-loop quantise,
   shift and shift-1 tried, lowest squared error wins). in: int16 mono; out: 16 bytes per 28 samples. */
#include <stdint.h>
#include <math.h>
static const double F[5][2] = {{0,0},{60/64.0,0},{115/64.0,-52/64.0},{98/64.0,-55/64.0},{122/64.0,-60/64.0}};
main_export int vag_encode(const int16_t *pcm, int n, uint8_t *out, int loop_s, int loop_e)
{
    int nblk = n > 0 ? (n + 27) / 28 : 1;
    double s1 = 0, s2 = 0;
    for (int b = 0; b < nblk; b++) {
        double blk[28];
        for (int k = 0; k < 28; k++) { int i = b * 28 + k; blk[k] = i < n ? pcm[i] : 0; }
        double best = 1e300; int bf = 0, bs = 0; int bn[28]; double b1 = 0, b2 = 0;
        for (int fi = 0; fi < 5; fi++) {
            double f1 = F[fi][0], f2 = F[fi][1], p1 = s1, p2 = s2, peak = 0;
            for (int k = 0; k < 28; k++) { double r = blk[k] - (p1 * f1 + p2 * f2); if (fabs(r) > peak) peak = fabs(r); p2 = p1; p1 = blk[k]; }
            int sh = 12;
            while (sh > 0 && peak > (double)((7 << (12 - sh)) - 1)) sh--;
            for (int t = 0; t < 2; t++) {
                int shc = t == 0 ? sh : (sh > 0 ? sh - 1 : 0);
                double q1 = s1, q2 = s2, err = 0; int nib[28];
                for (int k = 0; k < 28; k++) {
                    double pred = q1 * f1 + q2 * f2;
                    double v = (blk[k] - pred) * (double)(1 << shc) / 4096.0;
                    int vi = (int)lrint(v); if (vi < -8) vi = -8; if (vi > 7) vi = 7;
                    double y = (double)((vi * 4096) >> shc) + pred;
                    if (y > 32767) y = 32767; if (y < -32768) y = -32768;
                    err += (blk[k] - y) * (blk[k] - y); nib[k] = vi; q2 = q1; q1 = y;
                }
                if (err < best) { best = err; bf = fi; bs = shc; for (int k = 0; k < 28; k++) bn[k] = nib[k]; b1 = q1; b2 = q2; }
            }
        }
        s1 = b1; s2 = b2;
        uint8_t fl = 0;
        if (loop_s >= 0) { if (b == loop_s / 28) fl |= 6; if (b == (loop_e - 1) / 28) fl |= 3; }
        else if (b == nblk - 1) fl = 1;
        uint8_t *o = out + 16 * b; o[0] = (uint8_t)((bf << 4) | bs); o[1] = fl;
        for (int k = 0; k < 28; k += 2) o[2 + k / 2] = (uint8_t)((bn[k] & 15) | ((bn[k + 1] & 15) << 4));
    }
    return nblk * 16;
}
