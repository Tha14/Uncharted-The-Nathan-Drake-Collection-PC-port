// dctools/texcodec.cpp: see texcodec.hpp.
#include "texcodec.hpp"

#include "ndpak.hpp"
#include "u3fix.hpp"

#include <cmath>

#if defined(__clang__)
#pragma clang fp contract(off)   // float32 results must match numpy's (no fused multiply-add)
#endif

namespace u3::tc {

const std::map<uint32_t, std::pair<std::string, int>> DXGI = {
    {0x47, {"BC1_UNORM", 8}},      {0x48, {"BC1_UNORM_SRGB", 8}},      {0x4a, {"BC2_UNORM", 16}},
    {0x4b, {"BC2_UNORM_SRGB", 16}}, {0x4d, {"BC3_UNORM", 16}},          {0x4e, {"BC3_UNORM_SRGB", 16}},
    {0x50, {"BC4_UNORM", 8}},      {0x51, {"BC4_SNORM", 8}},           {0x53, {"BC5_UNORM", 16}},
    {0x54, {"BC5_SNORM", 16}},     {0x1c, {"R8G8B8A8_UNORM", 4}},      {0x1d, {"R8G8B8A8_UNORM_SRGB", 4}},
    {0x57, {"B8G8R8A8_UNORM", 4}}, {0x5b, {"B8G8R8A8_UNORM_SRGB", 4}},
};

std::optional<Format> ps3_format(const PakTexture &t) {
    switch (t.gcmRemapCtl >> 24) {
    case 0x06: return Format{"DXT1", 8, true};
    case 0x07: return Format{"DXT3", 16, true};
    case 0x08: return Format{"DXT5", 16, true};
    case 0x05: return Format{"A8R8G8B8", 4, false};
    default: return std::nullopt;
    }
}

std::vector<std::pair<uint32_t, uint32_t>> mip_dims(uint32_t w, uint32_t h, uint32_t mips) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (uint32_t m = 0; m < mips; m++) out.push_back({m < 32 ? std::max(1u, w >> m) : 1u, m < 32 ? std::max(1u, h >> m) : 1u});
    return out;
}

std::pair<uint32_t, uint32_t> blocks(uint32_t w, uint32_t h, bool compressed) {
    if (compressed) return {(w + 3) / 4, (h + 3) / 4};
    return {w, h};
}

uint64_t ps3_linear_size(uint32_t w, uint32_t h, uint32_t mips, int bpb, bool compressed) {
    uint64_t s = 0;
    for (auto [mw, mh] : mip_dims(w, h, mips)) {
        auto [bw, bh] = blocks(mw, mh, compressed);
        s += uint64_t(bw) * bh * bpb;
    }
    return s;
}

uint64_t ps4_tiled_size(uint32_t w, uint32_t h, uint32_t mips, int bpb, bool compressed) {
    uint64_t s = 0;
    for (auto [mw, mh] : mip_dims(w, h, mips)) {
        auto [bw, bh] = blocks(mw, mh, compressed);
        s += uint64_t((bw + 7) / 8 * 8) * ((bh + 7) / 8 * 8) * bpb;
    }
    return s;
}

namespace {

struct Morton8 {
    uint8_t x[64], y[64];
    Morton8() {
        for (int i = 0; i < 64; i++) {
            x[i] = uint8_t((i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4));
            y[i] = uint8_t(((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4));
        }
    }
};
const Morton8 M8;

}  // namespace

Bytes tile_mip(const uint8_t *lin, size_t len, uint32_t bw, uint32_t bh, int bpb) {
    const size_t n = size_t(bw) * bh * bpb;
    if (len < n) throw Stop(fmt("ValueError: texel data %zu bytes, mip needs %zu", len, n));
    const uint32_t pw = (bw + 7) / 8 * 8, ph = (bh + 7) / 8 * 8;
    Bytes out(size_t(pw) * ph * bpb);
    size_t o = 0;
    for (uint32_t ty = 0; ty < ph / 8; ty++)
        for (uint32_t tx = 0; tx < pw / 8; tx++)
            for (int e = 0; e < 64; e++) {
                // padding repeats the last column, then the last row (padded columns included)
                const uint32_t x = std::min(tx * 8 + M8.x[e], bw - 1), y = std::min(ty * 8 + M8.y[e], bh - 1);
                std::copy_n(lin + (size_t(y) * bw + x) * bpb, bpb, out.begin() + o);
                o += bpb;
            }
    return out;
}

Bytes detile_mip(const uint8_t *data, size_t len, uint32_t bw, uint32_t bh, int bpb) {
    const uint32_t pw = (bw + 7) / 8 * 8, ph = (bh + 7) / 8 * 8;
    if (len < size_t(pw) * ph * bpb) throw Stop(fmt("ValueError: tiled data %zu bytes, mip needs %zu", len, size_t(pw) * ph * bpb));
    Bytes out(size_t(bw) * bh * bpb);
    size_t o = 0;
    for (uint32_t ty = 0; ty < ph / 8; ty++)
        for (uint32_t tx = 0; tx < pw / 8; tx++)
            for (int e = 0; e < 64; e++, o += bpb) {
                const uint32_t x = tx * 8 + M8.x[e], y = ty * 8 + M8.y[e];
                if (x < bw && y < bh) std::copy_n(data + o, bpb, out.begin() + (size_t(y) * bw + x) * bpb);
            }
    return out;
}

Bytes ps3_to_ps4_texels(const Bytes &data, uint32_t w, uint32_t h, uint32_t mips, int bpb, bool compressed) {
    Bytes out;
    size_t o = 0;
    for (auto [mw, mh] : mip_dims(w, h, mips)) {
        auto [bw, bh] = blocks(mw, mh, compressed);
        const size_t n = size_t(bw) * bh * bpb;
        const size_t have = o < data.size() ? std::min(n, data.size() - o) : 0;
        Bytes t = tile_mip(data.data() + std::min(o, data.size()), have, bw, bh, bpb);
        out.insert(out.end(), t.begin(), t.end());
        o += n;
    }
    return out;
}

Bytes ps4_mip0_linear(const Bytes &data, uint32_t w, uint32_t h, int bpb, bool compressed) {
    auto [bw, bh] = blocks(w, h, compressed);
    return detile_mip(data.data(), data.size(), bw, bh, bpb);
}

// ------------------------------------------------------------------------------------------------ decoding
namespace {

void c565(uint32_t c, int p[3]) {
    p[0] = int((c >> 11) & 31) * 255 / 31;
    p[1] = int((c >> 5) & 63) * 255 / 63;
    p[2] = int(c & 31) * 255 / 31;
}

// one BC1 colour block (8 bytes) -> 16 RGBA pixels (row-major)
void bc1_block(const uint8_t *b, bool alpha, uint8_t px[16][4]) {
    const uint32_t c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    int pal[4][3];
    c565(c0, pal[0]);
    c565(c1, pal[1]);
    const bool four = c0 > c1;
    for (int k = 0; k < 3; k++) {
        pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
        pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
    }
    const int alp[4] = {255, 255, 255, alpha && !four ? 0 : 255};
    for (int i = 0; i < 16; i++) {
        const int s = (idx >> (2 * i)) & 3;
        for (int k = 0; k < 3; k++) px[i][k] = uint8_t(pal[s][k]);
        px[i][3] = uint8_t(alp[s]);
    }
}

void bc4_block(const uint8_t *b, uint8_t out[16]) {
    const int a0 = b[0], a1 = b[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
    int pal[8] = {a0, a1};
    if (a0 > a1)
        for (int i = 1; i < 7; i++) pal[1 + i] = ((7 - i) * a0 + i * a1) / 7;
    else {
        for (int i = 1; i < 5; i++) pal[1 + i] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    for (int i = 0; i < 16; i++) out[i] = uint8_t(pal[(bits >> (3 * i)) & 7]);
}

}  // namespace

Img decode(const Bytes &lin, uint32_t w, uint32_t h, const std::string &kind) {
    Img img;
    img.w = w;
    img.h = h;
    img.px.assign(size_t(w) * h * 4, 0);
    if (kind == "A8R8G8B8") {
        if (lin.size() < size_t(w) * h * 4) throw Stop("ValueError: A8R8G8B8 data too short");
        for (size_t i = 0; i < size_t(w) * h; i++)
            for (int k = 0; k < 4; k++) img.px[i * 4 + k] = lin[i * 4 + (k + 1) % 4];
        return img;
    }
    int t;
    if (kind == "DXT1" || kind == "BC1") t = 1;
    else if (kind == "DXT3" || kind == "BC2") t = 2;
    else if (kind == "DXT5" || kind == "BC3") t = 3;
    else throw Stop("ValueError: " + kind);
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const size_t bs = t == 1 ? 8 : 16;
    if (lin.size() < size_t(bw) * bh * bs) throw Stop("ValueError: " + kind + " data too short");
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = &lin[(size_t(by) * bw + bx) * bs];
            uint8_t px[16][4];
            if (t == 1) bc1_block(b, true, px);
            else {
                bc1_block(b + 8, false, px);
                uint8_t al[16];
                if (t == 3) bc4_block(b, al);
                else
                    for (int i = 0; i < 8; i++) {
                        al[2 * i] = uint8_t((b[i] & 15) * 17);
                        al[2 * i + 1] = uint8_t((b[i] >> 4) * 17);
                    }
                for (int i = 0; i < 16; i++) px[i][3] = al[i];
            }
            for (int i = 0; i < 16; i++) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) std::copy_n(px[i], 4, img.at(x, y));
            }
        }
    return img;
}

void gcm_remap(const uint8_t argb[4], uint32_t remap, uint8_t rgba[4]) {
    uint8_t out[4];   // A, R, G, B
    for (int k = 0; k < 4; k++) {
        const uint32_t sel = (remap >> (2 * k)) & 3, mode = (remap >> (8 + 2 * k)) & 3;
        out[k] = mode == 0 ? 0 : mode == 1 ? 255 : argb[sel];
    }
    rgba[0] = out[1];
    rgba[1] = out[2];
    rgba[2] = out[3];
    rgba[3] = out[0];
}

Img ps3_deswizzle_argb8(const Bytes &data, uint32_t w, uint32_t h, uint32_t remap) {
    const size_t n = size_t(w) * h;
    if (data.size() < n * 4) throw Stop("ValueError: A8R8G8B8 data too short");
    Img img;
    img.w = w;
    img.h = h;
    img.px.resize(n * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint64_t idx = 0;
            int b = 0;
            uint32_t ww = w, hh = h, xx = x, yy = y;
            while (ww > 1 || hh > 1) {
                if (ww > 1) { idx |= uint64_t(xx & 1) << b; xx >>= 1; ww >>= 1; b++; }
                if (hh > 1) { idx |= uint64_t(yy & 1) << b; yy >>= 1; hh >>= 1; b++; }
            }
            if (idx >= n) throw Stop("IndexError: swizzled texel outside the texture");
            gcm_remap(&data[idx * 4], remap, img.at(x, y));
        }
    return img;
}

Bytes encode_bc3(const Img &img) {
    const uint32_t bw = (img.w + 3) / 4, bh = (img.h + 3) / 4;
    const float sc[3] = {31.f, 63.f, 31.f};
    float w0[8];
    const float w0i[8] = {7, 0, 6, 5, 4, 3, 2, 1};
    for (int i = 0; i < 8; i++) w0[i] = w0i[i] / 7.f;
    Bytes out(size_t(bw) * bh * 16);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            float f[16][4];   // pixels below the image edge are zero (as the original encoder's padding)
            for (int i = 0; i < 16; i++) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                for (int k = 0; k < 4; k++) f[i][k] = x < img.w && y < img.h ? float(img.at(x, y)[k]) : 0.f;
            }
            float mx[3], mn[3], Q0[3], Q1[3], P[4][3];
            for (int k = 0; k < 3; k++) {
                mx[k] = mn[k] = f[0][k];
                for (int i = 1; i < 16; i++) {
                    mx[k] = std::max(mx[k], f[i][k]);
                    mn[k] = std::min(mn[k], f[i][k]);
                }
                Q0[k] = std::nearbyint(mx[k] * sc[k] / 255.f);
                Q1[k] = std::nearbyint(mn[k] * sc[k] / 255.f);
                const float E0 = Q0[k] * 255.f / sc[k], E1 = Q1[k] * 255.f / sc[k];
                P[0][k] = E0;
                P[1][k] = E1;
                P[2][k] = (2.f * E0 + E1) / 3.f;
                P[3][k] = (E0 + 2.f * E1) / 3.f;
            }
            const uint32_t c0 = uint32_t(Q0[0]) << 11 | uint32_t(Q0[1]) << 5 | uint32_t(Q0[2]);
            const uint32_t c1 = uint32_t(Q1[0]) << 11 | uint32_t(Q1[1]) << 5 | uint32_t(Q1[2]);
            uint32_t cidx = 0;
            if (c0 != c1)
                for (int i = 0; i < 16; i++) {
                    int best = 0;
                    float bd = 0;
                    for (int j = 0; j < 4; j++) {
                        float d = 0;
                        for (int k = 0; k < 3; k++) {
                            const float e = f[i][k] - P[j][k];
                            d = d + e * e;
                        }
                        if (j == 0 || d < bd) { bd = d; best = j; }
                    }
                    cidx |= uint32_t(best) << (2 * i);
                }
            float a0 = f[0][3], a1 = f[0][3];
            for (int i = 1; i < 16; i++) {
                a0 = std::max(a0, f[i][3]);
                a1 = std::min(a1, f[i][3]);
            }
            uint64_t abits = 0;
            if (a0 != a1) {
                float AP[8];
                for (int j = 0; j < 8; j++) AP[j] = a0 * w0[j] + a1 * (1.f - w0[j]);
                for (int i = 0; i < 16; i++) {
                    int best = 0;
                    float bd = 0;
                    for (int j = 0; j < 8; j++) {
                        const float d = std::fabs(f[i][3] - AP[j]);
                        if (j == 0 || d < bd) { bd = d; best = j; }
                    }
                    abits |= uint64_t(best) << (3 * i);
                }
            }
            uint8_t *o = &out[(size_t(by) * bw + bx) * 16];
            o[0] = uint8_t(a0);
            o[1] = uint8_t(a1);
            for (int j = 0; j < 6; j++) o[2 + j] = uint8_t(abits >> (8 * j));
            o[8] = uint8_t(c0);
            o[9] = uint8_t(c0 >> 8);
            o[10] = uint8_t(c1);
            o[11] = uint8_t(c1 >> 8);
            for (int j = 0; j < 4; j++) o[12 + j] = uint8_t(cidx >> (8 * j));
        }
    return out;
}

}  // namespace u3::tc

namespace u3::tc::sky {

void gcm_remap(const uint8_t argb[4], uint32_t remap, uint8_t rgba[4]) {
    const int order[4] = {1, 2, 3, 0};   // out R, G, B, A
    for (int i = 0; i < 4; i++) {
        const int k = order[i];
        const uint32_t sel = (remap >> (2 * k)) & 3, op = (remap >> (8 + 2 * k)) & 3;
        rgba[i] = op == 2 ? argb[sel] : op == 1 ? 255 : 0;
    }
}

Img ps3_deswizzle_argb8(const Bytes &data, uint32_t w, uint32_t h, uint32_t remap) {
    const size_t n = size_t(w) * h;
    const size_t have = std::min(data.size(), n * 4) / 4;
    auto bitlen = [](uint32_t v) { int b = 0; while (v) { b++; v >>= 1; } return b; };
    const int lw = bitlen(w) - 1, lh = bitlen(h) - 1, m = std::min(lw, lh);
    Img img;
    img.w = w;
    img.h = h;
    img.px.resize(n * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint64_t idx = 0;
            for (int b = 0; b < m; b++) idx |= uint64_t((x >> b) & 1) << (2 * b) | uint64_t((y >> b) & 1) << (2 * b + 1);
            if (lw > lh) idx |= uint64_t(x >> m) << (2 * m);
            else if (lh > lw) idx |= uint64_t(y >> m) << (2 * m);
            if (idx >= have) throw Stop("IndexError: swizzled texel outside the texture");
            gcm_remap(&data[idx * 4], remap, img.at(x, y));
        }
    return img;
}

Bytes encode_bc3(const Img &img) {
    const uint32_t bw = (img.w + 3) / 4, bh = (img.h + 3) / 4;
    Bytes out(size_t(bw) * bh * 16);
    auto q565 = [](const float c[3]) {
        const float s[3] = {31.f, 63.f, 31.f};
        uint32_t q[3];
        for (int k = 0; k < 3; k++) q[k] = uint32_t(std::min(std::max(std::nearbyint(c[k] * s[k] / 255.f), 0.f), s[k]));
        return (q[0] << 11) | (q[1] << 5) | q[2];
    };
    auto dq565 = [](uint32_t v, float p[3]) {
        p[0] = float(double(((v >> 11) & 31) * 255) / 31.0);
        p[1] = float(double(((v >> 5) & 63) * 255) / 63.0);
        p[2] = float(double((v & 31) * 255) / 31.0);
    };
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            float f[16][4];
            for (int i = 0; i < 16; i++) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                for (int k = 0; k < 4; k++) f[i][k] = x < img.w && y < img.h ? float(img.at(x, y)[k]) : 0.f;
            }
            float lo[3], hi[3];
            for (int k = 0; k < 3; k++) {
                lo[k] = hi[k] = f[0][k];
                for (int i = 1; i < 16; i++) {
                    lo[k] = std::min(lo[k], f[i][k]);
                    hi[k] = std::max(hi[k], f[i][k]);
                }
            }
            uint32_t c0 = q565(hi), c1 = q565(lo);
            if (c0 < c1) std::swap(c0, c1);
            float pal[4][3];
            dq565(c0, pal[0]);
            dq565(c1, pal[1]);
            for (int k = 0; k < 3; k++) {
                pal[2][k] = (2.f * pal[0][k] + pal[1][k]) / 3.f;
                pal[3][k] = (pal[0][k] + 2.f * pal[1][k]) / 3.f;
            }
            uint32_t cbits = 0;
            if (c0 != c1)
                for (int i = 0; i < 16; i++) {
                    int best = 0;
                    float bd = 0;
                    for (int j = 0; j < 4; j++) {
                        float d = 0;
                        for (int k = 0; k < 3; k++) {
                            const float e = f[i][k] - pal[j][k];
                            d = d + e * e;
                        }
                        if (j == 0 || d < bd) { bd = d; best = j; }
                    }
                    cbits |= uint32_t(best) << (2 * i);
                }
            float a0 = f[0][3], a1 = f[0][3];
            for (int i = 1; i < 16; i++) {
                a0 = std::max(a0, f[i][3]);
                a1 = std::min(a1, f[i][3]);
            }
            const int32_t a0i = int32_t(std::nearbyint(a0)), a1i = int32_t(std::nearbyint(a1));
            float apal[8] = {float(a0i), float(a1i)};
            for (int k = 1; k < 7; k++) apal[1 + k] = float(double((7 - k) * a0i + k * a1i) / 7.0);
            uint64_t abits = 0;
            if (a0i != a1i)
                for (int i = 0; i < 16; i++) {
                    int best = 0;
                    float bd = 0;
                    for (int j = 0; j < 8; j++) {
                        const float d = std::fabs(f[i][3] - apal[j]);
                        if (j == 0 || d < bd) { bd = d; best = j; }
                    }
                    abits |= uint64_t(best) << (3 * i);
                }
            uint8_t *o = &out[(size_t(by) * bw + bx) * 16];
            o[0] = uint8_t(a0i);
            o[1] = uint8_t(a1i);
            for (int j = 0; j < 6; j++) o[2 + j] = uint8_t(abits >> (8 * j));
            o[8] = uint8_t(c0);
            o[9] = uint8_t(c0 >> 8);
            o[10] = uint8_t(c1);
            o[11] = uint8_t(c1 >> 8);
            for (int j = 0; j < 4; j++) o[12 + j] = uint8_t(cbits >> (8 * j));
        }
    return out;
}

}  // namespace u3::tc::sky
