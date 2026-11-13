// Recipe step eula-pages: the Options > User Agreement pages vtex1/ps4/eula/p1..p7.dds in overlay-r3. Replaces
// make_eula.py and reproduces the 2026-11-01 22:21 build byte for byte:
//   1. the 7 agreement pages BCES-EN-1..7.jpg (512x512) the PS3 game downloaded into {{PS3_DATA2}}/http-cache (file
//      names are hashes without extension); recognised by content sha256, the cache file name is only a fallback.
//   2. the reference build decoded each page with libjpeg (accurate integer IDCT, fancy upsampling), flipped it
//      top-bottom (the vtex is stored bottom-up) and re-saved it as JPEG quality 95 without chroma subsampling. That
//      lossy re-save is part of the reference, so this file has its own JPEG decoder and encoder that give the same
//      samples and the same quantised coefficients as libjpeg does.
//   3. ffmpeg decodes the re-saved page and scales it (lanczos) to rgb24 for every mip 1024 .. 1.
//   4. DXT5 (alpha block constant 0xff, colour endpoints = darkest and brightest pixel), 128-byte DDS header, 11 mips.
// Without ffmpeg (or with --no-ffmpeg) step 3 uses a built-in separable Lanczos-3 resize (the one the Pillow fallback
// of make_eula.py used): same layout and look, not byte-identical.
//   u3fix eula-pages --project P --cache DIR [--out DIR] [--backup DIR] [--ffmpeg EXE] [--no-ffmpeg] [--apply]
// Dry run by default; --apply writes (backup of replaced pages in work/backup-eula, once). Idempotent: when all 7
// pages already equal the reference nothing is done.
#include "proc.hpp"
#include "u3fix.hpp"

#include <array>
#include <cmath>
#include <cstring>

namespace u3 {

namespace {

const char *const PAGES[7][2] = {   // content sha256 of BCES-EN-<i>.jpg, cache file name prefix
    {"f76a3288082e72e463355350bce48d4bef17d0fed1f0d08055265e24e56af890", "6532dd2818"},
    {"8409a516381b46351799948d0221226d99545054d3af0aec059d8f885deb7791", "234fe07708"},
    {"7c1e4764cdb4e0d1d8fb9c5a0ea489913cd0df2c45f5c27054a0f741ab0b6193", "75713af431"},
    {"d27549bffcf407d8d95d9e47efdd717e32666fde07454c3dbaebb16edcf32274", "b6e53413d7"},
    {"e4acbaa48645be0876a01d09f046c0e0d9e50cfd090f544780b9ca2e433716dc", "9a4d4bc81c"},
    {"fb9794d128d11c8f106ade19b9d711d098cf3c056cb32987479782d72382c84a", "f6d6249a0f"},
    {"4507c5145379b02a3c792f3cc40497cf52524369b4155d7ed43fed74843af3e4", "22f8e3c4db"},
};
const char *const EXPECT[7] = {   // sha256 of the reference p1..p7.dds (recipe/manifest.tsv)
    "27bd9d43da2e70cb2df0f904cfa73613cfcf6156e38f79927bbe5e0f5d751415",
    "3a1418cf2252747ef50a465542ee9ae75342b40d13b54c6460c45e49039ab137",
    "1b179532e493e4181a8ceb9049d196ef656c4ec6dac1dd11322cffabda57e3ac",
    "22115bf206fec2e9e696119e533212daad8787d62b7baf7fb95f6b7154edd0bc",
    "c80441807b92cf923ec1cf4ecca6963e23b908d61cfe41793b18931bfd11e4b1",
    "f4f8809970734568c6d70d9b9fba4cac41134163bc50ed8ec2d03d5a1081c3e7",
    "792efc2bdbf92caf9c51870449f5826cff433bcc478490b3bf4630ad6a7901ef",
};

// ---- JPEG basics ----------------------------------------------------------------------------------------------
// zigzag position k -> row-major index in the 8x8 block, built by walking the anti-diagonals
std::array<int, 64> make_zigzag() {
    std::array<int, 64> z{};
    int k = 0;
    for (int s = 0; s < 15; s++)
        for (int i = 0; i <= s; i++) {
            const int row = (s & 1) ? i : s - i, col = s - row;
            if (row < 8 && col < 8) z[size_t(k++)] = row * 8 + col;
        }
    return z;
}
const std::array<int, 64> ZZ = make_zigzag();

uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

// the fixed-point constants of the accurate integer DCT (13 fraction bits, values of the Loeffler-Ligtenberg-Moschytz
// rotations) and its scaling: 2 extra bits between the passes
constexpr int CB = 13, P1 = 2;
constexpr int64_t C0298 = 2446, C0390 = 3196, C0541 = 4433, C0765 = 6270, C0899 = 7373, C1175 = 9633, C1501 = 12299,
                  C1847 = 15137, C1961 = 16069, C2053 = 16819, C2562 = 20995, C3072 = 25172;
int64_t descale(int64_t x, int n) { return (x + (int64_t(1) << (n - 1))) >> n; }

// the odd part shared by both directions: in t0..t3 = x7, x5, x3, x1 (inverse) or the differences (forward)
void odd_part(int64_t &t0, int64_t &t1, int64_t &t2, int64_t &t3) {
    int64_t z1 = t0 + t3, z2 = t1 + t2, z3 = t0 + t2, z4 = t1 + t3;
    const int64_t z5 = (z3 + z4) * C1175;
    t0 *= C0298, t1 *= C2053, t2 *= C3072, t3 *= C1501;
    z1 *= -C0899, z2 *= -C2562, z3 *= -C1961, z4 *= -C0390;
    z3 += z5, z4 += z5;
    t0 += z1 + z3, t1 += z2 + z4, t2 += z2 + z3, t3 += z1 + z4;
}

// inverse DCT of one dequantised block (row-major) to 8x8 samples with stride
void idct_block(const int32_t *in, uint8_t *out, size_t stride) {
    int64_t ws[64];
    for (int c = 0; c < 8; c++) {   // columns
        const int32_t *x = in + c;
        bool ac = false;
        for (int r = 1; r < 8; r++) ac = ac || x[r * 8] != 0;
        if (!ac) {
            for (int r = 0; r < 8; r++) ws[r * 8 + c] = int64_t(x[0]) * (1 << P1);
            continue;
        }
        const int64_t z1 = (int64_t(x[16]) + x[48]) * C0541;
        const int64_t t2 = z1 - int64_t(x[48]) * C1847, t3 = z1 + int64_t(x[16]) * C0765;
        const int64_t t0 = (int64_t(x[0]) + x[32]) * (1 << CB), t1 = (int64_t(x[0]) - x[32]) * (1 << CB);
        const int64_t e10 = t0 + t3, e13 = t0 - t3, e11 = t1 + t2, e12 = t1 - t2;
        int64_t o0 = x[56], o1 = x[40], o2 = x[24], o3 = x[8];
        odd_part(o0, o1, o2, o3);
        const int n = CB - P1;
        ws[0 * 8 + c] = descale(e10 + o3, n), ws[7 * 8 + c] = descale(e10 - o3, n);
        ws[1 * 8 + c] = descale(e11 + o2, n), ws[6 * 8 + c] = descale(e11 - o2, n);
        ws[2 * 8 + c] = descale(e12 + o1, n), ws[5 * 8 + c] = descale(e12 - o1, n);
        ws[3 * 8 + c] = descale(e13 + o0, n), ws[4 * 8 + c] = descale(e13 - o0, n);
    }
    // the results wrap at 10 bits (as a signed value) before the clamp to 0..255, as the reference decoder's table did
    auto lim = [](int64_t v) {
        int w = int(v & 1023);
        if (w >= 512) w -= 1024;
        return clamp8(w + 128);
    };
    for (int r = 0; r < 8; r++) {   // rows
        const int64_t *x = ws + r * 8;
        uint8_t *o = out + size_t(r) * stride;
        const int n = CB + P1 + 3;
        const int64_t z1 = (x[2] + x[6]) * C0541;
        const int64_t t2 = z1 - x[6] * C1847, t3 = z1 + x[2] * C0765;
        const int64_t t0 = (x[0] + x[4]) * (1 << CB), t1 = (x[0] - x[4]) * (1 << CB);
        const int64_t e10 = t0 + t3, e13 = t0 - t3, e11 = t1 + t2, e12 = t1 - t2;
        int64_t o0 = x[7], o1 = x[5], o2 = x[3], o3 = x[1];
        odd_part(o0, o1, o2, o3);
        o[0] = lim(descale(e10 + o3, n)), o[7] = lim(descale(e10 - o3, n));
        o[1] = lim(descale(e11 + o2, n)), o[6] = lim(descale(e11 - o2, n));
        o[2] = lim(descale(e12 + o1, n)), o[5] = lim(descale(e12 - o1, n));
        o[3] = lim(descale(e13 + o0, n)), o[4] = lim(descale(e13 - o0, n));
    }
}

// forward DCT of 8x8 samples (already minus 128), row-major; the result is 8 times the true DCT
void fdct_block(int64_t *d) {
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 8; i++) {
            int64_t *x = pass == 0 ? d + i * 8 : d + i;   // rows, then columns
            const int s = pass == 0 ? 1 : 8;
            const int64_t a0 = x[0] + x[7 * s], a7 = x[0] - x[7 * s], a1 = x[s] + x[6 * s], a6 = x[s] - x[6 * s];
            const int64_t a2 = x[2 * s] + x[5 * s], a5 = x[2 * s] - x[5 * s], a3 = x[3 * s] + x[4 * s], a4 = x[3 * s] - x[4 * s];
            const int64_t e10 = a0 + a3, e13 = a0 - a3, e11 = a1 + a2, e12 = a1 - a2;
            const int n = pass == 0 ? CB - P1 : CB + P1;
            x[0] = pass == 0 ? (e10 + e11) * (1 << P1) : descale(e10 + e11, P1);
            x[4 * s] = pass == 0 ? (e10 - e11) * (1 << P1) : descale(e10 - e11, P1);
            const int64_t z1 = (e12 + e13) * C0541;
            x[2 * s] = descale(z1 + e13 * C0765, n);
            x[6 * s] = descale(z1 - e12 * C1847, n);
            int64_t o0 = a4, o1 = a5, o2 = a6, o3 = a7;
            odd_part(o0, o1, o2, o3);
            x[7 * s] = descale(o0, n), x[5 * s] = descale(o1, n), x[3 * s] = descale(o2, n), x[s] = descale(o3, n);
        }
}

// ---- decoder (baseline and progressive Huffman JPEG, 1 or 3 components) ------------------------------------------
struct Huff {
    std::array<int, 18> maxcode{}, valptr{}, mincode{};
    std::vector<uint8_t> vals;
    bool ok = false;
    void build(const uint8_t *bits, const uint8_t *v, size_t nv) {
        vals.assign(v, v + nv);
        int code = 0, k = 0;
        for (int l = 1; l <= 16; l++) {
            valptr[size_t(l)] = k, mincode[size_t(l)] = code;
            code += bits[l - 1], k += bits[l - 1];
            maxcode[size_t(l)] = bits[l - 1] ? code - 1 : -1;
            code <<= 1;
        }
        maxcode[17] = 0x7fffffff;
        ok = true;
    }
};

struct Comp {
    int id = 0, h = 1, v = 1, tq = 0;
    int bw = 0, bh = 0;   // blocks per line / column, padded to whole MCUs
    int cw = 0, ch = 0;   // samples of the component (image size scaled by its sampling factors)
    std::vector<int32_t> coef;   // bw*bh blocks of 64 quantised coefficients, row-major inside the block
    std::array<int, 64> bits;    // per coefficient: -1 never sent, else the low bit position still to come (0 = complete)
    int pred = 0;
};

struct Image {
    int w = 0, h = 0, n = 0;
    std::vector<uint8_t> rgb;   // w*h*3
};

class Decoder {
public:
    explicit Decoder(const Bytes &d) : d_(d) {}
    Image decode();

private:
    const Bytes &d_;
    size_t pos_ = 0;
    uint32_t acc_ = 0;
    int cnt_ = 0;
    bool marker_ = false;
    std::array<std::array<int, 64>, 4> q_{};
    std::array<Huff, 4> dc_, ac_;
    std::vector<Comp> c_;
    int W_ = 0, H_ = 0, hmax_ = 1, vmax_ = 1, restart_ = 0, eobrun_ = 0;
    bool progressive_ = false, frame_ = false;

    uint8_t byte(size_t o) const {
        if (o >= d_.size()) throw Stop("JPEG: unexpected end of data");
        return d_[o];
    }
    uint16_t be16(size_t o) const { return uint16_t(byte(o) << 8 | byte(o + 1)); }
    int bit() {
        if (!cnt_) {
            uint8_t b = 0;
            if (!marker_ && pos_ < d_.size()) {
                b = d_[pos_];
                if (b == 0xff) {
                    const uint8_t n = pos_ + 1 < d_.size() ? d_[pos_ + 1] : 0xd9;
                    if (n == 0)
                        pos_ += 2;
                    else
                        marker_ = true, b = 0;   // a marker ends the data: zeros from here on
                } else
                    pos_++;
            } else
                marker_ = true;
            acc_ = b, cnt_ = 8;
        }
        return int(acc_ >> --cnt_) & 1;
    }
    int bits(int n) {
        int v = 0;
        for (int i = 0; i < n; i++) v = v << 1 | bit();
        return v;
    }
    static int extend(int v, int n) { return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }
    int huff(const Huff &t) {
        if (!t.ok) throw Stop("JPEG: Huffman table missing");
        int code = bit(), l = 1;
        while (code > t.maxcode[size_t(l)]) {
            code = code << 1 | bit();
            if (++l > 16) throw Stop("JPEG: bad Huffman code");
        }
        const size_t i = size_t(t.valptr[size_t(l)] + code - t.mincode[size_t(l)]);
        if (i >= t.vals.size()) throw Stop("JPEG: bad Huffman code");
        return t.vals[i];
    }
    void scan(size_t at);
    void block(Comp &c, int32_t *b, const std::vector<int> &td, const std::vector<int> &ta, size_t ci, int ss, int se, int ah, int al);
};

void Decoder::block(Comp &c, int32_t *b, const std::vector<int> &td, const std::vector<int> &ta, size_t ci, int ss, int se, int ah, int al) {
    if (!progressive_) {   // sequential: the whole block
        const int s = huff(dc_[size_t(td[ci])]);
        c.pred += extend(bits(s), s);
        b[0] = c.pred;
        for (int k = 1; k < 64; k++) {
            const int rs = huff(ac_[size_t(ta[ci])]), r = rs >> 4, sz = rs & 15;
            if (!sz) {
                if (r != 15) break;
                k += 15;
                continue;
            }
            k += r;
            if (k > 63) throw Stop("JPEG: coefficient index out of range");
            b[ZZ[size_t(k)]] = extend(bits(sz), sz);
        }
        return;
    }
    if (ss == 0) {   // DC scan
        if (ah == 0) {
            const int s = huff(dc_[size_t(td[ci])]);
            c.pred += extend(bits(s), s);
            b[0] = int32_t(uint32_t(c.pred) << al);
        } else if (bit())
            b[0] |= 1 << al;
        return;
    }
    const Huff &t = ac_[size_t(ta[ci])];
    if (ah == 0) {   // AC first pass
        if (eobrun_ > 0) {
            eobrun_--;
            return;
        }
        for (int k = ss; k <= se; k++) {
            const int rs = huff(t), r = rs >> 4, sz = rs & 15;
            if (sz) {
                k += r;
                if (k > 63) throw Stop("JPEG: coefficient index out of range");
                b[ZZ[size_t(k)]] = int32_t(uint32_t(extend(bits(sz), sz)) << al);
            } else if (r == 15)
                k += 15;
            else {
                eobrun_ = (1 << r) - 1;
                if (r) eobrun_ += bits(r);
                break;
            }
        }
        return;
    }
    // AC refinement: one more bit for coefficients already non-zero, new coefficients of magnitude 1 << al
    const int p1 = 1 << al, m1 = -p1;
    int k = ss;
    auto refine = [&](int32_t &x) {
        if (bit() && (x & p1) == 0) x += x >= 0 ? p1 : m1;
    };
    if (eobrun_ == 0) {
        for (; k <= se; k++) {
            const int rs = huff(t);
            int r = rs >> 4, s = rs & 15, val = 0;
            if (s) {
                if (s != 1) throw Stop("JPEG: bad refinement coefficient");
                val = bit() ? p1 : m1;
            } else if (r != 15) {
                eobrun_ = 1 << r;
                if (r) eobrun_ += bits(r);
                break;
            }
            do {   // skip r zero-history coefficients, refining the non-zero ones on the way
                int32_t &x = b[ZZ[size_t(k)]];
                if (x != 0)
                    refine(x);
                else if (--r < 0)
                    break;
                k++;
            } while (k <= se);
            if (val) {
                if (k > 63) throw Stop("JPEG: coefficient index out of range");
                b[ZZ[size_t(k)]] = val;
            }
        }
    }
    if (eobrun_ > 0) {
        for (; k <= se; k++) {
            int32_t &x = b[ZZ[size_t(k)]];
            if (x != 0) refine(x);
        }
        eobrun_--;
    }
}

void Decoder::scan(size_t at) {
    const size_t len = be16(at), n = byte(at + 2);
    if (n < 1 || n > 4 || len != 6 + 2 * n) throw Stop("JPEG: bad scan header");
    std::vector<Comp *> sc;
    std::vector<int> td, ta;
    for (size_t i = 0; i < n; i++) {
        const int id = byte(at + 3 + 2 * i), t = byte(at + 4 + 2 * i);
        Comp *c = nullptr;
        for (Comp &x : c_)
            if (x.id == id) c = &x;
        if (!c) throw Stop("JPEG: scan names an unknown component");
        sc.push_back(c), td.push_back(t >> 4 & 3), ta.push_back(t & 3);
    }
    const int ss = byte(at + 3 + 2 * n), se = byte(at + 4 + 2 * n), ah = byte(at + 5 + 2 * n) >> 4, al = byte(at + 5 + 2 * n) & 15;
    if (progressive_ ? (ss > se || se > 63 || (ss == 0 && se != 0) || (ss > 0 && n != 1)) : (ss != 0 || se != 63 || ah || al))
        throw Stop("JPEG: bad scan parameters");
    for (Comp *c : sc)   // what is known of each coefficient afterwards (to rule out the decoder's block smoothing)
        for (int k = ss; k <= se; k++) c->bits[size_t(ZZ[size_t(k)])] = al;
    pos_ = at + len, cnt_ = 0, marker_ = false, eobrun_ = 0;
    for (Comp *c : sc) c->pred = 0;
    std::vector<std::pair<Comp *, std::pair<int, int>>> units;   // one MCU's blocks: component, block x, y offsets
    int mx, my;
    if (n == 1) {   // one block per MCU, only the blocks that cover the component
        mx = (sc[0]->cw + 7) / 8, my = (sc[0]->ch + 7) / 8;
        units.push_back({sc[0], {0, 0}});
    } else {
        mx = (W_ + 8 * hmax_ - 1) / (8 * hmax_), my = (H_ + 8 * vmax_ - 1) / (8 * vmax_);
        for (Comp *c : sc)
            for (int y = 0; y < c->v; y++)
                for (int x = 0; x < c->h; x++) units.push_back({c, {x, y}});
    }
    int todo = restart_;
    for (int y = 0; y < my; y++)
        for (int x = 0; x < mx; x++) {
            if (restart_ && todo == 0) {   // RSTn: the bits restart byte-aligned, predictions and EOB runs reset
                while (pos_ + 1 < d_.size() && !(d_[pos_] == 0xff && d_[pos_ + 1] >= 0xd0 && d_[pos_ + 1] <= 0xd7)) pos_++;
                pos_ += 2, cnt_ = 0, marker_ = false, eobrun_ = 0, todo = restart_;
                for (Comp *c : sc) c->pred = 0;
            }
            for (const auto &u : units) {
                Comp &c = *u.first;
                const int bx = n == 1 ? x : x * c.h + u.second.first, by = n == 1 ? y : y * c.v + u.second.second;
                size_t ci = 0;
                while (sc[ci] != &c) ci++;
                block(c, &c.coef[(size_t(by) * size_t(c.bw) + size_t(bx)) * 64], td, ta, ci, ss, se, ah, al);
            }
            todo--;
        }
}

Image Decoder::decode() {
    if (d_.size() < 4 || d_[0] != 0xff || d_[1] != 0xd8) throw Stop("JPEG: no start of image");
    size_t p = 2;
    for (;;) {
        while (p < d_.size() && d_[p] != 0xff) p++;   // the scan data before the next marker was read by scan()
        while (p < d_.size() && d_[p] == 0xff) p++;
        const int m = byte(p++);
        if (m == 0xd9) break;
        if (m >= 0xd0 && m <= 0xd7) continue;
        const size_t len = be16(p);
        if (m == 0xdb) {   // DQT
            for (size_t o = p + 2; o < p + len;) {
                const int pq = byte(o) >> 4, tq = byte(o) & 3;
                o++;
                for (int k = 0; k < 64; k++, o += size_t(pq ? 2 : 1)) q_[size_t(tq)][size_t(ZZ[size_t(k)])] = pq ? be16(o) : byte(o);
            }
        } else if (m == 0xc4) {   // DHT
            for (size_t o = p + 2; o < p + len;) {
                const int tc = byte(o) >> 4, th = byte(o) & 3;
                uint8_t bits[16];
                size_t nv = 0;
                for (int i = 0; i < 16; i++) nv += bits[i] = byte(o + 1 + size_t(i));
                if (nv > 256) throw Stop("JPEG: bad Huffman table");
                byte(o + 16 + nv);
                (tc ? ac_ : dc_)[size_t(th)].build(bits, &d_[o + 17], nv);
                o += 17 + nv;
            }
        } else if (m == 0xdd) {   // DRI
            restart_ = be16(p + 2);
        } else if (m == 0xc0 || m == 0xc1 || m == 0xc2) {   // SOF0/1 sequential, SOF2 progressive (Huffman, 8 bit)
            if (frame_ || byte(p + 2) != 8) throw Stop("JPEG: unsupported frame");
            frame_ = true, progressive_ = m == 0xc2;
            H_ = be16(p + 3), W_ = be16(p + 5);
            const int n = byte(p + 7);
            if ((n != 1 && n != 3) || !W_ || !H_) throw Stop("JPEG: unsupported frame");
            for (int i = 0; i < n; i++) {
                Comp c;
                c.id = byte(p + 8 + 3 * size_t(i)), c.h = byte(p + 9 + 3 * size_t(i)) >> 4, c.v = byte(p + 9 + 3 * size_t(i)) & 15,
                c.tq = byte(p + 10 + 3 * size_t(i)) & 3;
                if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4) throw Stop("JPEG: bad sampling factors");
                hmax_ = std::max(hmax_, c.h), vmax_ = std::max(vmax_, c.v);
                c.bits.fill(-1);
                c_.push_back(c);
            }
            const int mx = (W_ + 8 * hmax_ - 1) / (8 * hmax_), my = (H_ + 8 * vmax_ - 1) / (8 * vmax_);
            for (Comp &c : c_) {
                c.cw = (W_ * c.h + hmax_ - 1) / hmax_, c.ch = (H_ * c.v + vmax_ - 1) / vmax_;
                c.bw = mx * c.h, c.bh = my * c.v;
                c.coef.assign(size_t(c.bw) * size_t(c.bh) * 64, 0);
            }
        } else if (m >= 0xc3 && m <= 0xcf && m != 0xc8 && m != 0xcc) {
            throw Stop("JPEG: unsupported coding (lossless, arithmetic or hierarchical)");
        } else if (m == 0xda) {
            if (!frame_) throw Stop("JPEG: scan before frame");
            scan(p);
            p = pos_;
            continue;
        }
        p += len;
    }
    if (!frame_) throw Stop("JPEG: no frame");
    if (progressive_)   // incomplete low coefficients would make the reference decoder smooth the blocks
        for (const Comp &c : c_)
            for (int k = 1; k <= 9; k++)
                if (c.bits[size_t(ZZ[size_t(k)])] != 0) throw Stop("JPEG: progressive image with unfinished coefficients");
    // samples of each component
    std::vector<std::vector<uint8_t>> pl;
    for (const Comp &c : c_) {
        const size_t stride = size_t(c.bw) * 8;
        std::vector<uint8_t> s(stride * size_t(c.bh) * 8);
        int32_t deq[64];
        for (int by = 0; by < c.bh; by++)
            for (int bx = 0; bx < c.bw; bx++) {
                const int32_t *b = &c.coef[(size_t(by) * size_t(c.bw) + size_t(bx)) * 64];
                for (int k = 0; k < 64; k++) deq[k] = b[k] * q_[size_t(c.tq)][size_t(k)];
                idct_block(deq, &s[size_t(by) * 8 * stride + size_t(bx) * 8], stride);
            }
        pl.push_back(std::move(s));
    }
    // full-size planes: the reference decoder's "fancy" (triangle) upsampling for 2:1 horizontal
    std::vector<std::vector<uint8_t>> full;
    for (size_t i = 0; i < c_.size(); i++) {
        const Comp &c = c_[i];
        const size_t stride = size_t(c.bw) * 8;
        std::vector<uint8_t> f(size_t(W_) * size_t(H_));
        if (c.h == hmax_ && c.v == vmax_) {
            for (int y = 0; y < H_; y++) std::memcpy(&f[size_t(y) * size_t(W_)], &pl[i][size_t(y) * stride], size_t(W_));
        } else if (c.h * 2 == hmax_ && c.v == vmax_) {
            const int n = c.cw;
            for (int y = 0; y < H_; y++) {
                const uint8_t *in = &pl[i][size_t(y) * stride];
                std::vector<uint8_t> row(size_t(n) * 2);
                if (n <= 2) {   // plain doubling for very narrow images
                    for (int x = 0; x < n; x++) row[size_t(2 * x)] = row[size_t(2 * x + 1)] = in[x];
                } else
                    for (int x = 0; x < n; x++) {
                        const int c3 = in[x] * 3;
                        row[size_t(2 * x)] = uint8_t(x == 0 ? in[0] : (c3 + in[x - 1] + 1) >> 2);
                        row[size_t(2 * x + 1)] = uint8_t(x == n - 1 ? in[x] : (c3 + in[x + 1] + 2) >> 2);
                    }
                std::memcpy(&f[size_t(y) * size_t(W_)], row.data(), size_t(W_));
            }
        } else
            throw Stop("JPEG: unsupported chroma subsampling");
        full.push_back(std::move(f));
    }
    Image im;
    im.w = W_, im.h = H_, im.n = int(c_.size());
    im.rgb.resize(size_t(W_) * size_t(H_) * 3);
    const size_t np = size_t(W_) * size_t(H_);
    if (c_.size() == 1) {
        for (size_t i = 0; i < np; i++) im.rgb[3 * i] = im.rgb[3 * i + 1] = im.rgb[3 * i + 2] = full[0][i];
        return im;
    }
    // YCbCr -> RGB with 16 fraction bits
    auto fix = [](double x) { return int64_t(x * 65536 + 0.5); };
    const int64_t half = 1 << 15;
    for (size_t i = 0; i < np; i++) {
        const int y = full[0][i], cb = full[1][i] - 128, cr = full[2][i] - 128;
        im.rgb[3 * i] = clamp8(y + int((fix(1.40200) * cr + half) >> 16));
        im.rgb[3 * i + 1] = clamp8(y + int((-fix(0.34414) * cb + half - fix(0.71414) * cr) >> 16));
        im.rgb[3 * i + 2] = clamp8(y + int((fix(1.77200) * cb + half) >> 16));
    }
    return im;
}

// ---- encoder: baseline, 4:4:4, quality-scaled standard tables, flat Huffman codes --------------------------------
// The pages only travel to ffmpeg, so the coded bits may differ from the reference save as long as the quantisation
// tables and every quantised coefficient are the same; flat codes (DC categories 4 bits, all AC symbols 9 bits) keep
// the writer short.
struct BitWriter {
    Bytes out;
    uint32_t acc = 0;
    int n = 0;
    void put(uint32_t v, int len) {
        for (int i = len - 1; i >= 0; i--) {
            acc = acc << 1 | (v >> i & 1);
            if (++n == 8) {
                out.push_back(uint8_t(acc));
                if (acc == 0xff) out.push_back(0);
                acc = 0, n = 0;
            }
        }
    }
    void flush() {
        while (n) put(1, 1);
    }
};

const uint8_t STD_Q[2][64] = {   // the example tables of the JPEG standard (luminance, chrominance), row-major
    {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
     18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99},
    {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
     99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99},
};

Bytes encode_jpeg(const Image &im, int quality) {
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    int qt[2][64];
    for (int t = 0; t < 2; t++)
        for (int k = 0; k < 64; k++) qt[t][k] = std::clamp((STD_Q[t][k] * scale + 50) / 100, 1, 255);
    const int W = im.w, H = im.h;
    // RGB -> YCbCr with 16 fraction bits (Cb/Cr round down from x.5 - 1/65536)
    auto fix = [](double x) { return int64_t(x * 65536 + 0.5); };
    const int64_t half = 1 << 15, off = int64_t(128) << 16;
    std::vector<uint8_t> pl[3];
    for (auto &p : pl) p.resize(size_t(W) * size_t(H));
    for (size_t i = 0; i < size_t(W) * size_t(H); i++) {
        const int64_t r = im.rgb[3 * i], g = im.rgb[3 * i + 1], b = im.rgb[3 * i + 2];
        pl[0][i] = uint8_t((fix(0.29900) * r + fix(0.58700) * g + fix(0.11400) * b + half) >> 16);
        pl[1][i] = uint8_t((-fix(0.16874) * r - fix(0.33126) * g + fix(0.5) * b + off + half - 1) >> 16);
        pl[2][i] = uint8_t((fix(0.5) * r - fix(0.41869) * g - fix(0.08131) * b + off + half - 1) >> 16);
    }
    Bytes o = {0xff, 0xd8};
    auto seg = [&](uint8_t m, const Bytes &body) {
        o.push_back(0xff), o.push_back(m), o.push_back(uint8_t((body.size() + 2) >> 8)), o.push_back(uint8_t(body.size() + 2));
        o.insert(o.end(), body.begin(), body.end());
    };
    seg(0xe0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    Bytes dqt;
    for (int t = 0; t < 2; t++) {
        dqt.push_back(uint8_t(t));
        for (int k = 0; k < 64; k++) dqt.push_back(uint8_t(qt[t][ZZ[size_t(k)]]));
    }
    seg(0xdb, dqt);
    seg(0xc0, {8, uint8_t(H >> 8), uint8_t(H), uint8_t(W >> 8), uint8_t(W), 3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1});
    Bytes dht;
    for (int t = 0; t < 2; t++) {   // DC: symbols 0..11 with 4-bit codes; AC: all 256 symbols with 9-bit codes
        dht.push_back(uint8_t(t));
        for (int l = 1; l <= 16; l++) dht.push_back(l == 4 ? 12 : 0);
        for (int s = 0; s < 12; s++) dht.push_back(uint8_t(s));
        dht.push_back(uint8_t(0x10 | t));
        for (int l = 1; l <= 16; l++) dht.push_back(0);
        dht[dht.size() - 16 + 8] = 255;   // 256 does not fit a byte: 255 codes of 9 bits ...
        dht[dht.size() - 16 + 9] = 1;   // ... and one of 10 bits
        for (int s = 0; s < 256; s++) dht.push_back(uint8_t(s));
    }
    seg(0xc4, dht);
    seg(0xda, {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0});
    BitWriter bw;
    auto ac_code = [](int sym, uint32_t &code, int &len) {   // 0..254: 9 bits, 255: 10 bits
        if (sym < 255)
            code = uint32_t(sym), len = 9;
        else
            code = 255u << 1, len = 10;
    };
    auto category = [](int v) {
        int n = 0;
        for (int a = v < 0 ? -v : v; a; a >>= 1) n++;
        return n;
    };
    int pred[3] = {0, 0, 0};
    for (int by = 0; by < (H + 7) / 8; by++)
        for (int bx = 0; bx < (W + 7) / 8; bx++)
            for (int c = 0; c < 3; c++) {
                int64_t blk[64];
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {   // edge blocks repeat the last column / row
                        const int sx = std::min(bx * 8 + x, W - 1), sy = std::min(by * 8 + y, H - 1);
                        blk[y * 8 + x] = int64_t(pl[c][size_t(sy) * size_t(W) + size_t(sx)]) - 128;
                    }
                fdct_block(blk);
                const int *q = qt[c ? 1 : 0];
                int zq[64];
                for (int k = 0; k < 64; k++) {   // divide by 8 * q, rounding half away from zero
                    const int64_t d = int64_t(q[ZZ[size_t(k)]]) * 8, v = blk[ZZ[size_t(k)]];
                    zq[k] = int(v < 0 ? -((-v + d / 2) / d) : (v + d / 2) / d);
                }
                const int diff = zq[0] - pred[c], dn = category(diff);
                pred[c] = zq[0];
                bw.put(uint32_t(dn), 4);
                if (dn) bw.put(uint32_t(diff < 0 ? diff + (1 << dn) - 1 : diff), dn);
                int run = 0;
                for (int k = 1; k < 64; k++) {
                    if (!zq[k]) {
                        run++;
                        continue;
                    }
                    uint32_t code;
                    int len;
                    while (run > 15) ac_code(0xf0, code, len), bw.put(code, len), run -= 16;
                    const int n = category(zq[k]);
                    ac_code(run << 4 | n, code, len), bw.put(code, len);
                    bw.put(uint32_t(zq[k] < 0 ? zq[k] + (1 << n) - 1 : zq[k]), n);
                    run = 0;
                }
                if (run) {
                    uint32_t code;
                    int len;
                    ac_code(0, code, len), bw.put(code, len);
                }
            }
    bw.flush();
    o.insert(o.end(), bw.out.begin(), bw.out.end());
    o.push_back(0xff), o.push_back(0xd9);
    return o;
}

// ---- built-in resize (without ffmpeg): separable Lanczos-3, 22-bit fixed-point weights, horizontal pass first ------
std::vector<uint8_t> resize_lanczos(const std::vector<uint8_t> &in, int iw, int ih, int ow, int oh) {
    const int PB = 22;
    constexpr double PI = 3.14159265358979323846;
    auto lanczos = [](double x) {
        auto sinc = [](double v) { return v == 0.0 ? 1.0 : std::sin(v * PI) / (v * PI); };
        return -3.0 <= x && x < 3.0 ? sinc(x) * sinc(x / 3) : 0.0;
    };
    struct W {
        int lo = 0;
        std::vector<int32_t> k;
    };
    auto weights = [&](int isz, int osz) {
        const double sc = double(isz) / osz, fs = std::max(sc, 1.0), sup = 3.0 * fs;
        std::vector<W> r(static_cast<size_t>(osz));
        for (int o = 0; o < osz; o++) {
            const double ctr = (o + 0.5) * sc;
            int lo = int(ctr - sup + 0.5), hi = int(ctr + sup + 0.5);
            lo = std::max(lo, 0), hi = std::min(hi, isz);
            std::vector<double> k(size_t(hi - lo));
            double ws = 0;
            for (int x = lo; x < hi; x++) ws += k[size_t(x - lo)] = lanczos((x - ctr + 0.5) / fs);
            r[size_t(o)].lo = lo;
            for (double v : k) {
                const double n = ws != 0 ? v / ws : v;
                r[size_t(o)].k.push_back(int32_t(n < 0 ? std::trunc(-0.5 + n * (1 << PB)) : std::trunc(0.5 + n * (1 << PB))));
            }
        }
        return r;
    };
    auto clip = [&](int64_t v) { return clamp8(int(v >> PB)); };
    std::vector<uint8_t> mid(size_t(ow) * size_t(ih) * 3), out(size_t(ow) * size_t(oh) * 3);
    const std::vector<W> hw = weights(iw, ow), vw = weights(ih, oh);
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < ow; x++)
            for (int c = 0; c < 3; c++) {
                int64_t s = int64_t(1) << (PB - 1);
                const W &w = hw[size_t(x)];
                for (size_t i = 0; i < w.k.size(); i++) s += int64_t(in[(size_t(y) * size_t(iw) + size_t(w.lo) + i) * 3 + size_t(c)]) * w.k[i];
                mid[(size_t(y) * size_t(ow) + size_t(x)) * 3 + size_t(c)] = clip(s);
            }
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++)
            for (int c = 0; c < 3; c++) {
                int64_t s = int64_t(1) << (PB - 1);
                const W &w = vw[size_t(y)];
                for (size_t i = 0; i < w.k.size(); i++) s += int64_t(mid[((size_t(w.lo) + i) * size_t(ow) + size_t(x)) * 3 + size_t(c)]) * w.k[i];
                out[(size_t(y) * size_t(ow) + size_t(x)) * 3 + size_t(c)] = clip(s);
            }
    return out;
}

// ---- DXT5 / DDS -----------------------------------------------------------------------------------------------------
uint16_t rgb565(const uint8_t *p) { return uint16_t((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3); }

void dxt5(const std::vector<uint8_t> &raw, int w, int h, Bytes &out) {
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const uint8_t *px[16];
            int lum[16];
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    const uint8_t *p = &raw[(size_t(std::min(y + j, h - 1)) * size_t(w) + size_t(std::min(x + i, w - 1))) * 3];
                    px[j * 4 + i] = p, lum[j * 4 + i] = p[0] * 3 + p[1] * 6 + p[2];
                }
            int lo = 0, hi = 0;   // first darkest / first brightest pixel
            for (int i = 1; i < 16; i++) {
                if (lum[i] < lum[lo]) lo = i;
                if (lum[i] > lum[hi]) hi = i;
            }
            uint16_t c0 = rgb565(px[hi]), c1 = rgb565(px[lo]);
            uint32_t bits = 0;
            if (c0 != c1) {
                if (c0 < c1) std::swap(c0, c1), std::swap(lo, hi);
                const int l0 = lum[hi], l1 = lum[lo], d = l0 - l1 ? l0 - l1 : 1;
                for (int i = 0; i < 16; i++) {
                    const double t = double((l0 - lum[i]) * 3) / d;
                    const uint32_t s = t < .5 ? 0 : t < 1.5 ? 2 : t < 2.5 ? 3 : 1;
                    bits |= s << (i * 2);
                }
            }
            const uint8_t blk[16] = {0xff, 0xff, 0, 0, 0, 0, 0, 0, uint8_t(c0), uint8_t(c0 >> 8), uint8_t(c1), uint8_t(c1 >> 8),
                                     uint8_t(bits), uint8_t(bits >> 8), uint8_t(bits >> 16), uint8_t(bits >> 24)};
            out.insert(out.end(), blk, blk + 16);
        }
}

Bytes build_page(const Bytes &jpg, const std::string &ff) {
    Image im = Decoder(jpg).decode();
    const size_t row = size_t(im.w) * 3;
    for (int y = 0; y < im.h / 2; y++) std::swap_ranges(&im.rgb[size_t(y) * row], &im.rgb[size_t(y) * row] + row, &im.rgb[size_t(im.h - 1 - y) * row]);
    const Bytes flipped = encode_jpeg(im, 95);
    Image back;
    if (ff.empty()) back = Decoder(flipped).decode();
    const int S = 1024;
    Bytes out(128, 0);
    std::memcpy(&out[0], "DDS ", 4);
    const uint32_t hdr[7] = {124, 0xa1007, uint32_t(S), uint32_t(S), uint32_t(S * S), 0, 11};
    for (int i = 0; i < 7; i++)
        for (int b = 0; b < 4; b++) out[size_t(4 + 4 * i + b)] = uint8_t(hdr[i] >> (8 * b));
    out[76] = 32, out[80] = 4;
    std::memcpy(&out[84], "DXT5", 4);
    out[108] = 0x08, out[109] = 0x10, out[110] = 0x40;
    for (int w = S;; w /= 2) {
        std::vector<uint8_t> raw;
        if (!ff.empty()) {
            const std::string sz = std::to_string(w);
            Bytes o;
            std::string err;
            const int rc = run_pipe({ff, "-v", "error", "-f", "jpeg_pipe", "-i", "-", "-vf", "scale=" + sz + ":" + sz + ":flags=lanczos", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"},
                                    flipped, o, err);
            if (rc) throw Stop("ffmpeg failed (exit " + std::to_string(rc) + "): " + err);
            if (o.size() != size_t(w) * size_t(w) * 3) throw Stop("ffmpeg: unexpected output size");
            raw.assign(o.begin(), o.end());
        } else
            raw = resize_lanczos(back.rgb, back.w, back.h, w, w);
        dxt5(raw, w, w, out);
        if (w == 1) break;
    }
    return out;
}

}  // namespace

int eula_pages(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--no-ffmpeg"}, {"--project", "--cache", "--out", "--backup", "--ffmpeg"});
    const std::string P = a.need("--project");
    const std::string cache = a.need("--cache");
    const std::string out = a.get("--out", P + "/work/overlay-r3/u3data/build/main/vtex1/ps4/eula"), backup = a.get("--backup", P + "/work/backup-eula");
    std::vector<std::string> dst;
    bool all = true;
    for (int i = 0; i < 7; i++) {
        dst.push_back(out + "/p" + std::to_string(i + 1) + ".dds");
        all = all && fs::is_regular_file(upath(dst.back())) && sha256_hex(read_file(upath(dst.back()))) == EXPECT[i];
    }
    if (all) {
        std::printf("already applied: 7 EULA pages match the reference\n");
        return 0;
    }
    if (!fs::is_directory(upath(cache))) throw Stop("no http-cache folder: " + cache);
    std::vector<std::string> names;
    for (const auto &e : fs::directory_iterator(upath(cache))) names.push_back(utf8(e.path().filename()));
    std::sort(names.begin(), names.end());
    std::map<std::string, std::string> by_sha;
    for (const std::string &n : names) {
        const fs::path p = upath(cache) / upath(n);
        if (fs::is_regular_file(p) && fs::file_size(p) < (4u << 20)) by_sha.emplace(sha256_hex(read_file(p)), n);
    }
    std::vector<fs::path> src;
    for (int i = 0; i < 7; i++) {
        std::string n;
        if (by_sha.count(PAGES[i][0]))
            n = by_sha[PAGES[i][0]];
        else
            for (const std::string &x : names)
                if (x.rfind(PAGES[i][1], 0) == 0) {
                    n = x;
                    break;
                }
        if (n.empty()) throw Stop("agreement page " + std::to_string(i + 1) + " not in " + cache + " (start the PS3 game online once so it caches the pages)");
        src.push_back(upath(cache) / upath(n));
    }
    std::string ff;
    if (!a.has("--no-ffmpeg")) {
        ff = find_ffmpeg(a.get("--ffmpeg", ""));
        if (ff.empty()) std::printf("ffmpeg not found: using the built-in Lanczos resize (equivalent pages, not byte-identical)\n");
    }
    int ident = 0;
    for (int i = 0; i < 7; i++) {
        const Bytes data = build_page(read_file(src[size_t(i)]), ff);
        const bool same = sha256_hex(data) == EXPECT[i];
        ident += same;
        std::printf("p%d.dds %s%s\n", i + 1, same ? "identical to the reference" : "differs from the reference", a.has("--apply") ? "" : " (dry run)");
        std::fflush(stdout);
        if (a.has("--apply")) {
            fs::create_directories(upath(out));
            if (fs::is_regular_file(upath(dst[size_t(i)]))) {
                fs::create_directories(upath(backup));
                const fs::path bk = upath(backup) / upath(dst[size_t(i)]).filename();
                if (!fs::exists(bk)) fs::copy_file(upath(dst[size_t(i)]), bk);
            }
            write_file_replace(upath(dst[size_t(i)]), data);
        }
    }
    std::printf("%s 7 pages, %d byte-identical to the reference\n", a.has("--apply") ? "wrote" : "would write", ident);
    return 0;
}

}  // namespace u3
