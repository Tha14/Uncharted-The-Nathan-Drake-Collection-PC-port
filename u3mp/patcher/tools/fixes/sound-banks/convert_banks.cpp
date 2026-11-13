// Recipe step convert-banks: overlay-r3 sound1/banks/ps4, the 124 PS3 sound banks converted to the PS4 SCREAM format.
// Replaces convert_banks.py, bnkconv.py and vagenc.py (the encoder is native/vagenc.c's algorithm).
//   u3fix sound-banks --project P --job J --ps3-disc D --ps3-update D [--out DIR] [--backup DIR] [--only a.bnk,b.bnk]
//                     [--ffmpeg EXE] [--jobs N] [--force] [--apply]
// banks_list.json (job folder): output name, archive (disc:build/main/sound1/banks.psarc, disc:goty-dlc.psarc,
// update:dlc-patch18.psarc), entry, reference sha256. adsr_env.json: the original converter's SPU ADSR -> PS4 envelope
// table (142 pairs learned from retail bank pairs) with its nearest-pair fallback.
//
// PS3 bank (SBlk v4 / v5, big endian) -> PS4 (SBlk v13, little endian): the sound table (12 -> 0x24 bytes), the grain
// table (data grains point into the grain data, others keep three u8 params or a u24), the grain data blocks by type
// (1 sample grains with the pitch as a PS4 rate and the envelope, 0x2d stream grains, 4 / 5 / 6 / 7 / 0x2f / 0x30 /
// 0x32 records), the name table and the audio: PS-ADPCM samples copied with a 0x20-byte PS4 sample header, MP3
// (codec 3) samples decoded with ffmpeg (-f mp3 ... -f s16le -ac 1 -ar <rate>; trimmed to the header's sample count) and
// PCM16 samples re-encoded to PS-ADPCM. Without ffmpeg the banks with MP3 samples are skipped with a message.
// Dry run by default; --apply backs replaced files up once to work/backup-sound-banks. Idempotent: a bank at the
// reference sha256, or at what this step wrote last time (convert_banks-state.json), is left alone unless --force.
#include "json.h"
#include "proc.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace u3 {
namespace {

uint32_t rbe32(const Bytes &d, size_t o) {
    if (o + 4 > d.size()) throw Stop("struct.error: unpack_from requires a buffer of at least 4 bytes");
    return be32(d, o);
}
uint16_t rbe16(const Bytes &d, size_t o) {
    if (o + 2 > d.size()) throw Stop("struct.error: unpack_from requires a buffer of at least 2 bytes");
    return uint16_t(d[o] << 8 | d[o + 1]);
}
Bytes sub(const Bytes &d, size_t a, size_t b) {   // d[a:b]
    a = std::min(a, d.size());
    b = std::min(std::max(a, b), d.size());
    return Bytes(d.begin() + std::ptrdiff_t(a), d.begin() + std::ptrdiff_t(b));
}
void le32(Bytes &o, uint32_t v) { for (int k = 0; k < 4; k++) o.push_back(uint8_t(v >> (8 * k))); }
void le16(Bytes &o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); }
void set32(Bytes &o, size_t at, uint32_t v) { for (int k = 0; k < 4; k++) o.at(at + size_t(k)) = uint8_t(v >> (8 * k)); }
void app(Bytes &o, const Bytes &b) { o.insert(o.end(), b.begin(), b.end()); }
uint32_t quiet(uint32_t v) { return ((v & 0x7f800000u) == 0x7f800000u && (v & 0x7fffffu)) ? v | 0x400000u : v; }
uint32_t f32bits(double d) {   // struct.pack('<f', d)
    const float f = float(d);
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return quiet(v);
}
double f32val(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return double(f);
}
Bytes sw32(const Bytes &b) {   // big-endian u32 words -> little endian (a partial word at the end kept)
    Bytes o = b;
    for (size_t i = 0; i + 4 <= o.size(); i += 4) std::reverse(o.begin() + std::ptrdiff_t(i), o.begin() + std::ptrdiff_t(i + 4));
    return o;
}

const uint32_t ONE = 0x3f800000;
const std::set<int> SWAP24 = {0x1a, 0x1c, 0x1d, 0x20, 0x25};
const std::set<int> DATA_TYPES = {1, 4, 5, 6, 7, 0x2d, 0x2f, 0x30, 0x32};

// the ADSR1:ADSR2 -> envelope table, in the file's order
std::vector<std::pair<std::pair<uint32_t, uint32_t>, std::string>> g_env;
Bytes envelope(uint32_t a1, uint32_t a2) {   // PS4 envelope words 0..13 (word 0 is always 0)
    const std::string *v = nullptr;
    for (const auto &[k, h] : g_env)
        if (k.first == a1 && k.second == a2) { v = &h; break; }
    if (!v) {   // the original converter's fallback: the nearest learned pair (same modes, closest rates)
        std::optional<int64_t> best;
        for (const auto &[k, h] : g_env) {
            const uint32_t b1 = k.first, b2 = k.second;
            auto ad = [](int64_t x) { return x < 0 ? -x : x; };
            const int64_t d = ad(int64_t((a1 >> 8) & 0x7f) - int64_t((b1 >> 8) & 0x7f)) + ad(int64_t((a1 >> 4) & 15) - int64_t((b1 >> 4) & 15)) * 2 +
                              ad(int64_t(a1 & 15) - int64_t(b1 & 15)) * 2 + ad(int64_t((a2 >> 6) & 0x7f) - int64_t((b2 >> 6) & 0x7f)) +
                              ad(int64_t(a2 & 31) - int64_t(b2 & 31)) * 2 + (((a1 ^ b1) & 0x8000) || ((a2 ^ b2) & 0xe020) ? 100 : 0);
            if (!best || d < *best) best = d, v = &h;
        }
        if (!v) throw Stop("TypeError: 'NoneType' object is not subscriptable");
    }
    Bytes o(4, 0);
    for (size_t i = 0; i + 1 < v->size(); i += 2) o.push_back(uint8_t(std::stoul(v->substr(i, 2), nullptr, 16)));
    return o;
}

// type-1 / 0x2d record list after the base words, up to the (0, 0) terminator
Bytes conv_records(const Bytes &b, size_t pos) {
    Bytes out;
    while (pos + 8 <= b.size()) {
        const uint32_t t = be32(b, pos), sz = be32(b, pos + 4);
        if (t == 0 && sz == 0) break;
        if (sz < 8 || pos + sz > b.size()) break;
        const Bytes body = sub(b, pos + 8, pos + sz);
        if (t == 1) {   // filter: u16 pair, f freq, f q / bandwidth, f gain, u32 mode
            const uint16_t x0 = rbe16(body, 0), x1 = rbe16(body, 2);
            double fa = f32val(rbe32(body, 4)), fb = f32val(rbe32(body, 8));
            const double fc = f32val(rbe32(body, 12));
            if (!std::isfinite(fa + 0.5)) throw Stop("ValueError: cannot convert float NaN to integer");
            fa = std::trunc(fa + 0.5);   // the PS4 filter frequency is a whole number of Hz
            const uint32_t mode = rbe32(body, 16);
            if (mode == 7 || mode == 8) fb = f32val(f32bits(fb)) / 12.0;
            le32(out, 1);
            le32(out, 0x1c);
            le16(out, x0);
            le16(out, x1);
            le32(out, f32bits(fa));
            le32(out, f32bits(fb));
            le32(out, f32bits(fc));
            le32(out, mode);
        } else if (t == 2) {   // -> PS4 0xb, one extra zero word
            le32(out, 0xb);
            le32(out, 0x18);
            app(out, sw32(body));
            le32(out, 0);
        } else if (t == 7) {   // dropped
        } else if (t == 3) {   // -> PS4 type 9 (0x48 bytes); the reference wrote a fixed default body
            le32(out, 9);
            le32(out, 0x48);
            for (int k = 0; k < 16; k++) le32(out, k < 8 ? 0xbf800000u : 0x3f800000u);
        } else   // an unknown record type: stop
            break;
        pos += sz;
    }
    out.resize(out.size() + 8, 0);
    return out;
}

Bytes conv_type1(const Bytes &b, uint32_t ver, uint32_t audio_off, uint32_t audio_size) {
    if (b.size() < 28) throw Stop("IndexError: grain data too short");
    const int note = b[2], fine = b[3];
    const uint16_t a1 = rbe16(b, 10), a2 = rbe16(b, 12);
    const size_t base = ver == 4 ? 7 : 9;
    const uint32_t fl = rbe32(b, 24);
    Bytes o(0x88, 0);
    set32(o, 0, 1);
    o[4] = b[0], o[5] = b[1], o[6] = b[5], o[7] = b[4];
    o[8] = 0, o[9] = 0, o[10] = b[8], o[11] = b[9];
    o[12] = uint8_t(a1), o[13] = uint8_t(a1 >> 8), o[14] = uint8_t(a2), o[15] = uint8_t(a2 >> 8);
    o[16] = 0, o[17] = 0, o[18] = uint8_t(1 | (b[15] & 0x60)), o[19] = 0xcb;
    set32(o, 20, fl == ONE ? 0 : fl);
    const uint32_t eight[8] = {0x3f800000, 0x3f800000, 0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000};
    for (int k = 0; k < 8; k++) set32(o, 24 + 4 * size_t(k), eight[k]);
    set32(o, 68, audio_off);
    set32(o, 72, audio_size);
    set32(o, 76, f32bits(24000.0 * std::pow(2.0, (double(note - 184) + double(fine) / 128.0) / 12.0)));
    const Bytes env = envelope(a1, a2);
    o.resize(80);   // o[80:0x88] = the envelope
    app(o, env);
    const Bytes rec = (b[14] & 1) ? conv_records(b, base * 4) : Bytes(8, 0);
    app(o, rec);
    return o;
}

// stream grain (XVAG file reference); the params come from the fixed v5 offsets also for v4 banks, as the reference did
Bytes conv_type2d(const Bytes &b, uint32_t name_off, uint32_t x20) {
    const uint16_t a1 = rbe16(b, 10), a2 = rbe16(b, 12);
    Bytes o(0x6c, 0);
    set32(o, 0, 1);
    o[4] = b[0], o[5] = b[1], o[6] = b[5], o[7] = b[4];
    o[8] = 0, o[9] = 0, o[10] = b[8], o[11] = b[9];
    o[12] = uint8_t(a1), o[13] = uint8_t(a1 >> 8), o[14] = uint8_t(a2), o[15] = uint8_t(a2 >> 8);
    o[16] = 0, o[17] = 0, o[18] = uint8_t(1 | (b[15] & 0x60)), o[19] = 0xc3;
    const uint32_t fl = rbe32(b, 36);
    set32(o, 20, fl == ONE ? 0 : fl);
    const uint32_t eight[8] = {0x3f800000, 0x3f800000, 0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000};
    for (int k = 0; k < 8; k++) set32(o, 24 + 4 * size_t(k), eight[k]);
    set32(o, 68, name_off);
    set32(o, 72, rbe32(b, 20));
    set32(o, 76, rbe32(b, 24));
    set32(o, 80, x20);
    o.resize(0x6c);
    app(o, sw32(sub(b, 48, 84)));
    const Bytes env = envelope(a1, a2);
    o.insert(o.end(), env.begin() + 4, env.end());
    app(o, (b[14] & 1) ? conv_records(b, 84) : Bytes(8, 0));
    return o;
}

Bytes conv_block(int t, const Bytes &b) {
    if (t == 5 || t == 6) {   // u32 words 0, 1, 3, 4 (word 2 is four u8 fields); 5 keeps a trailing name string, 6 drops it
        Bytes o = sw32(sub(b, 0, 8));
        app(o, sub(b, 8, 12));
        app(o, sw32(sub(b, 12, 20)));
        if (t == 5) app(o, sub(b, 20, b.size()));
        return o;
    }
    if (t == 7) {
        Bytes o = sw32(sub(b, 0, 4));
        app(o, sub(b, 4, b.size()));
        return o;
    }
    if (t == 0x30) return sub(b, 0, 24);
    if (t == 0x32) return sw32(b);
    if (t == 0x2f) {
        if (b.size() < 4) throw Stop("IndexError: index out of range");
        return Bytes{b[0], b[1], b[3], 0};
    }
    if (t == 4) {
        if (b.size() < 12) throw Stop("IndexError: index out of range");
        Bytes o = sub(b, 0, 4);
        for (size_t i : {5, 4, 7, 6, 9, 8, 11, 10}) o.push_back(b[i]);
        Bytes w = sub(b, 12, 16);
        std::reverse(w.begin(), w.end());
        app(o, w);
        o.resize(o.size() + 4, 0);
        le32(o, 0x3f800000);
        o.resize(o.size() + 8, 0);
        return o;
    }
    throw Stop(u3::fmt("KeyError: 'grain data type %#x'", t));
}

// samples / loop start / loop end of a PS-ADPCM stream (the PS4 sample header fields)
std::array<uint32_t, 3> adpcm_info(const Bytes &data) {
    const size_t nfr = data.size() / 16;
    size_t end = nfr;
    uint32_t ls = 0xffffffff, le = 0xffffffff;
    for (size_t i = 0; i < nfr; i++) {
        const uint8_t f = data[i * 16 + 1];
        if ((f & 4) && ls == 0xffffffff) ls = uint32_t(i * 28);
        if (f & 1) {
            end = i + 1;
            break;
        }
    }
    if (ls != 0xffffffff) le = uint32_t(end * 28) - ls;   // the loop length
    return {uint32_t(end * 28), ls, le};
}
Bytes sample_header(uint32_t samples, uint32_t ls, uint32_t le) {
    Bytes o;
    for (uint32_t v : {0u, 1u, 0x10u, 0u, samples, 1u, ls, le}) le32(o, v);
    return o;
}

// PS-ADPCM (VAG) encoder of the original 2026-10-27 converter (native/vagenc.c): per 28-sample block every filter is
// tried with its open-loop shift and that shift - 1, closed-loop quantised; the lowest squared error wins
Bytes vag_encode(const std::vector<int16_t> &pcm) {
    static const double F[5][2] = {{0, 0}, {60 / 64.0, 0}, {115 / 64.0, -52 / 64.0}, {98 / 64.0, -55 / 64.0}, {122 / 64.0, -60 / 64.0}};
    const int n = int(pcm.size()), nblk = n > 0 ? (n + 27) / 28 : 1;
    Bytes out(size_t(nblk) * 16, 0);
    double s1 = 0, s2 = 0;
    for (int b = 0; b < nblk; b++) {
        double blk[28];
        for (int k = 0; k < 28; k++) {
            const int i = b * 28 + k;
            blk[k] = i < n ? pcm[size_t(i)] : 0;
        }
        double best = 1e300, b1 = 0, b2 = 0;
        int bf = 0, bs = 0, bn[28] = {};
        for (int fi = 0; fi < 5; fi++) {
            const double f1 = F[fi][0], f2 = F[fi][1];
            double p1 = s1, p2 = s2, peak = 0;
            for (int k = 0; k < 28; k++) {
                const double r = blk[k] - (p1 * f1 + p2 * f2);
                if (std::fabs(r) > peak) peak = std::fabs(r);
                p2 = p1;
                p1 = blk[k];
            }
            int sh = 12;
            while (sh > 0 && peak > double((7 << (12 - sh)) - 1)) sh--;
            for (int t = 0; t < 2; t++) {
                const int shc = t == 0 ? sh : (sh > 0 ? sh - 1 : 0);
                double q1 = s1, q2 = s2, err = 0;
                int nib[28];
                for (int k = 0; k < 28; k++) {
                    const double pred = q1 * f1 + q2 * f2;
                    const double v = (blk[k] - pred) * double(1 << shc) / 4096.0;
                    int vi = int(std::lrint(v));
                    vi = std::clamp(vi, -8, 7);
                    double y = double((vi * 4096) >> shc) + pred;
                    if (y > 32767) y = 32767;
                    if (y < -32768) y = -32768;
                    err += (blk[k] - y) * (blk[k] - y);
                    nib[k] = vi;
                    q2 = q1;
                    q1 = y;
                }
                if (err < best) {
                    best = err;
                    bf = fi;
                    bs = shc;
                    std::copy(nib, nib + 28, bn);
                    b1 = q1;
                    b2 = q2;
                }
            }
        }
        s1 = b1;
        s2 = b2;
        uint8_t *o = &out[16 * size_t(b)];
        o[0] = uint8_t((bf << 4) | bs);
        o[1] = b == nblk - 1 ? 1 : 0;
        for (int k = 0; k < 28; k += 2) o[2 + k / 2] = uint8_t((bn[k] & 15) | ((bn[k + 1] & 15) << 4));
    }
    return out;
}

struct Skip : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// an MP3 (codec 3) or PCM16 BE sample of the PS3 audio section -> PS4 0x20 header + PS-ADPCM
Bytes encode_sample(const Bytes &A, size_t ao, size_t asz, const std::string &ff) {
    const Bytes src = sub(A, ao, ao + asz);
    std::vector<int16_t> x;
    if (src.size() >= 56 && be32(src, 0) == 1 && be32(src, 4) == 3) {   // MP3 with its header
        const uint32_t rate = rbe32(A, ao + 0xc), nsmp = rbe32(A, ao + 0x24), dsz = rbe32(A, ao + 0x34);
        if (ff.empty()) throw Skip("needs ffmpeg (MP3 sample)");
        Bytes pcm;
        std::string err;
        const int rc = run_pipe({ff, "-v", "error", "-f", "mp3", "-i", "pipe:0", "-f", "s16le", "-ac", "1", "-ar", std::to_string(rate), "pipe:1"},
                                sub(A, ao + 0x80, ao + 0x80 + dsz), pcm, err);
        if (rc) throw Skip("ffmpeg: " + (err.size() > 300 ? err.substr(err.size() - 300) : err));
        for (size_t i = 0; i + 1 < pcm.size(); i += 2) x.push_back(int16_t(pcm[i] | pcm[i + 1] << 8));
        if (nsmp && x.size() > nsmp) x.resize(nsmp);
    } else   // PCM16 big endian
        for (size_t i = 0; i + 1 < src.size(); i += 2) x.push_back(int16_t(src[i] << 8 | src[i + 1]));
    const Bytes v = vag_encode(x);
    Bytes o = sample_header(uint32_t(v.size() / 16 * 28), 0xffffffff, 0xffffffff);
    app(o, v);
    return o;
}

Bytes convert(const Bytes &d, const std::string &ff) {
    // ---- PS3 parse
    uint32_t h[8];
    for (int k = 0; k < 8; k++) h[k] = rbe32(d, 4 * size_t(k));
    const size_t s = h[2];
    uint32_t H[16];
    for (int k = 0; k < 16; k++) H[k] = rbe32(d, s + 4 * size_t(k));
    if (H[0] != 0x6b6c4253 || (H[1] != 4 && H[1] != 5)) throw Stop("ValueError: not a PS3 SBlk v4/v5 bank");
    const uint32_t ver = H[1], flags = H[2], nsnd = H[5], ngr = H[6] >> 16, nnm = H[6] & 0xffff, so = H[7], go3 = H[8], gdo3 = H[13], no3 = H[14], ne3 = H[15];
    std::vector<Bytes> snd, gr;
    for (uint32_t i = 0; i < nsnd; i++) snd.push_back(sub(d, s + so + 12ull * i, s + so + 12ull * i + 12));
    for (uint32_t i = 0; i < ngr; i++) gr.push_back(sub(d, s + go3 + 8ull * i, s + go3 + 8ull * i + 8));
    const Bytes gd = sub(d, s + gdo3, s + no3);
    const size_t end3 = ne3 ? ne3 : h[3];
    const Bytes names = sub(d, s + no3, s + end3), tail = sub(d, s + end3, s + h[3]);
    const Bytes A = sub(d, h[4], size_t(h[4]) + h[5]);
    for (const Bytes &g : gr)
        if (g.size() < 8) throw Stop("IndexError: index out of range");
    for (const Bytes &p : snd)
        if (p.size() < 12) throw Stop("IndexError: index out of range");
    auto u24 = [](const Bytes &g) { return size_t(g[1]) << 16 | size_t(g[2]) << 8 | g[3]; };
    // grain-data blocks: offset -> end (the next referenced offset)
    std::set<size_t> offs{gd.size()};
    for (const Bytes &g : gr)
        if (DATA_TYPES.count(g[0])) offs.insert(u24(g));
    std::map<size_t, size_t> ext;
    for (auto it = offs.begin(); it != offs.end(); ++it) ext[*it] = std::next(it) != offs.end() ? *std::next(it) : *it;
    auto block = [&](const Bytes &g) {
        const size_t v = u24(g);
        return sub(gd, v, ext.at(v));
    };
    // ---- audio: unique samples (type-1 grains) and stream file names (0x2d), in first-reference order, names last
    struct K {
        char kind;
        uint32_t ao, asz;
        bool operator<(const K &o) const { return std::tie(kind, ao, asz) < std::tie(o.kind, o.ao, o.asz); }
    };
    std::vector<K> order;
    std::set<K> seen, pcm;
    for (const Bytes &g : gr) {
        if (g[0] != 1 && g[0] != 0x2d) continue;
        const Bytes b = block(g);
        const K k = g[0] == 1 ? K{'s', rbe32(b, 16), rbe32(b, 20)} : K{'n', rbe32(b, 16), 0};
        if (seen.insert(k).second) order.push_back(k);
        if (g[0] == 1 && b.size() > 15 && (b[15] & 0x80)) pcm.insert(k);
        if (g[0] == 1 && b.size() <= 15) throw Stop("IndexError: index out of range");
    }
    std::stable_partition(order.begin(), order.end(), [](const K &k) { return k.kind == 's'; });
    Bytes audio4;
    std::map<K, std::pair<uint32_t, uint32_t>> amap;
    for (const K &k : order) {
        Bytes blob;
        if (k.kind == 'n') {
            size_t e = A.size();
            for (size_t i = k.ao; i < A.size(); i++)
                if (!A[i]) { e = i + 1; break; }
            blob = sub(A, k.ao, e);
        } else {
            const Bytes src = sub(A, k.ao, size_t(k.ao) + k.asz);
            const bool mp3 = k.asz != 0 && src.size() >= 8 && be32(src, 0) == 1 && be32(src, 4) == 3;
            if (!pcm.count(k) && k.asz != 0 && src.size() < 8) throw Stop("struct.error: unpack_from requires a buffer of at least 8 bytes");
            if (!pcm.count(k) && (k.asz == 0 || !mp3)) {   // PS-ADPCM: copied
                const auto [smp, ls, le] = adpcm_info(src);
                blob = sample_header(smp, ls, le);
                app(blob, src);
            } else
                blob = encode_sample(A, k.ao, k.asz, ff);
        }
        amap[k] = {uint32_t(audio4.size()), uint32_t(blob.size())};
        app(audio4, blob);
    }
    const uint32_t audio_size = uint32_t(audio4.size());
    // ---- grain table and data
    Bytes gd4;
    std::vector<Bytes> gr4;
    for (const Bytes &g : gr) {
        const int t = g[0];
        const Bytes w{g[7], g[6], g[5], g[4]};
        auto entry = [&](uint32_t first) {
            Bytes e;
            le32(e, first);
            app(e, w);
            gr4.push_back(e);
        };
        if (t == 0x19) {
            entry(0x30u << 24 | uint32_t(gd4.size()));
            Bytes blk{g[1], g[2], g[3], uint8_t((g[1] - 1) & 0xff), 0};
            blk.insert(blk.end(), 16, 0xff);
            blk.insert(blk.end(), 3, 0);
            app(gd4, blk);
        } else if (t == 1) {
            const Bytes b = block(g);
            const auto [no, ns] = amap.at(K{'s', rbe32(b, 16), rbe32(b, 20)});
            entry(1u << 24 | uint32_t(gd4.size()));
            app(gd4, conv_type1(b, ver, no, ns));
        } else if (t == 0x2d) {
            const Bytes b = block(g);
            entry(uint32_t(t) << 24 | uint32_t(gd4.size()));
            app(gd4, conv_type2d(b, amap.at(K{'n', rbe32(b, 16), 0}).first, audio_size));
        } else if (DATA_TYPES.count(t)) {
            const Bytes b = block(g);
            entry(uint32_t(t) << 24 | uint32_t(gd4.size()));
            app(gd4, conv_block(t, b));
        } else {   // a u24 value vs three u8 params
            Bytes e = SWAP24.count(t) ? Bytes{g[3], g[2], g[1]} : Bytes{g[1], g[2], g[3]};
            e.push_back(uint8_t(t));
            app(e, w);
            gr4.push_back(e);
        }
    }
    // ---- SBlk
    const uint32_t go = 0x50 + ((0x24 * nsnd + 15) & ~15u);
    const uint32_t gdo = (go + 8 * ngr + 15) & ~15u;
    gd4.resize(gd4.size() + ((16 - gd4.size() % 16) % 16), 0);
    const uint32_t no = gdo + uint32_t(gd4.size());
    Bytes names4 = names;
    if (!names.empty()) {
        uint32_t nh[4];
        for (int k = 0; k < 4; k++) nh[k] = rbe32(names, 4 * size_t(k));
        for (int k = 0; k < 4; k++) set32(names4, 4 * size_t(k), nh[k]);
        for (uint32_t p = nh[2]; p < nh[3]; p += 16) {
            const uint32_t a = rbe32(names, p), b = rbe32(names, p + 4);
            uint16_t c[4];
            for (int k = 0; k < 4; k++) c[k] = rbe16(names, p + 8 + 2 * size_t(k));
            if (p + 16 > names4.size()) throw Stop("struct.error: pack_into out of range");
            set32(names4, p, a);
            set32(names4, p + 4, b);
            for (int k = 0; k < 4; k++) names4[p + 8 + 2 * size_t(k)] = uint8_t(c[k]), names4[p + 9 + 2 * size_t(k)] = uint8_t(c[k] >> 8);
        }
    }
    const uint32_t ne = no + uint32_t(names4.size()), end = ne + uint32_t(tail.size());
    Bytes sb = {'S', 'B', 'l', 'k'};
    for (uint32_t v : {13u, 5u, 0u, 0u, flags & 0xff00u, 0x50u, go, 0u, end, no, gdo, no, ne}) le32(sb, v);
    le16(sb, nsnd);
    le16(sb, ngr);
    le16(sb, nnm);
    le16(sb, 0);
    sb.resize(sb.size() + 16, 0);
    Bytes sndb;
    for (const Bytes &p : snd) {   // PS3 sound 12 bytes -> PS4 0x24 bytes
        le32(sndb, 1);
        for (uint8_t c : {p[0], p[1], p[2], p[3], p[4], p[5], p[7], uint8_t(p[6] | 1)}) sndb.push_back(c);
        for (int k = 11; k >= 8; k--) sndb.push_back(p[size_t(k)]);
        le32(sndb, ONE);
        sndb.resize(sndb.size() + 16, 0);
    }
    app(sb, sndb);
    if (go - 0x50 > sndb.size()) sb.resize(sb.size() + (go - 0x50 - sndb.size()), 0);
    for (const Bytes &e : gr4) app(sb, e);
    if (gdo - go > 8 * ngr) sb.resize(sb.size() + (gdo - go - 8 * ngr), 0);
    app(sb, gd4);
    app(sb, names4);
    app(sb, tail);
    for (uint8_t c : {0, 0, 0, 0, 1, 0, 0, 0, 0}) sb.push_back(c);
    const uint32_t sblk_size = uint32_t(sb.size()), doff = (0x20 + sblk_size + 15) & ~15u;
    Bytes out;
    for (uint32_t v : {3u, 2u, 0x20u, sblk_size, doff, audio_size, 0u, 0u}) le32(out, v);
    app(out, sb);
    out.resize(out.size() + (doff - 0x20 - sblk_size), 0);
    app(out, audio4);
    return out;
}

}  // namespace

std::string find_ffmpeg(const std::string &arg) {
    std::vector<std::string> c = {arg};
    if (const char *e = std::getenv("RECIPE_FFMPEG")) c.push_back(e);
    c.push_back(R"(C:\msys64\clang64\bin\ffmpeg.exe)");
    c.push_back(R"(C:\msys64\ucrt64\bin\ffmpeg.exe)");
    for (const std::string &x : c)
        if (!x.empty() && fs::is_regular_file(upath(x))) return x;
    if (const char *path = std::getenv("PATH")) {   // shutil.which('ffmpeg')
        const std::string p = path;
        size_t a = 0;
        for (size_t k = 0; k <= p.size(); k++)
            if (k == p.size() || p[k] == ';') {
                const std::string dir = p.substr(a, k - a);
                a = k + 1;
                if (!dir.empty() && fs::is_regular_file(upath(dir) / "ffmpeg.exe")) return utf8(upath(dir) / "ffmpeg.exe");
            }
    }
    return "";
}


int sound_banks(const std::vector<std::string> &argv) {
    const Args a(argv, {"--force", "--apply"}, {"--project", "--job", "--ps3-disc", "--ps3-update", "--out", "--backup", "--only", "--ffmpeg", "--jobs"});
    const std::string project = a.need("--project");
    const fs::path job = upath(a.need("--job"));
    const fs::path outd = a.val.count("--out") ? upath(a.get("--out", "")) : upath(project) / "work" / "overlay-r3" / "u3data" / "build" / "main" / "sound1" / "banks" / "ps4";
    const fs::path backup = a.val.count("--backup") ? upath(a.get("--backup", "")) : upath(project) / "work" / "backup-sound-banks";
    {
        json::Value e;
        if (!json::parse(text_file(job / "adsr_env.json"), e)) throw Stop("adsr_env.json is not JSON");
        for (const auto &[k, v] : e.fields) {
            const size_t c = k.find(':');
            g_env.push_back({{uint32_t(std::stoul(k.substr(0, c), nullptr, 16)), uint32_t(std::stoul(k.substr(c + 1), nullptr, 16))}, v.s});
        }
    }
    json::Value rows;
    if (!json::parse(text_file(job / "banks_list.json"), rows)) throw Stop("banks_list.json is not JSON");
    std::set<std::string> only;
    if (a.val.count("--only")) {
        const std::string on = a.get("--only", "");
        size_t s = 0;
        for (size_t k = 0; k <= on.size(); k++)
            if (k == on.size() || on[k] == ',') { only.insert(on.substr(s, k - s)); s = k + 1; }
    }
    const std::string ff = find_ffmpeg(a.get("--ffmpeg", ""));
    if (ff.empty()) std::printf("ffmpeg not found: banks with MP3 samples are skipped (set RECIPE_FFMPEG or put ffmpeg on PATH)\n");
    const std::map<std::string, std::string> roots = {{"disc", a.get("--ps3-disc", "")}, {"update", a.get("--ps3-update", "")}};
    const fs::path statef = backup / "convert_banks-state.json";
    std::map<std::string, std::string> state;   // json.dump(sort_keys=True)
    if (fs::is_regular_file(statef)) {
        json::Value s;
        if (json::parse(text_file(statef), s))
            for (const auto &[k, v] : s.fields) state[k] = v.s;
    }
    int identical = 0, equivalent = 0, skipped = 0, missing = 0;
    const auto t0 = std::chrono::steady_clock::now();
    struct Job {
        std::string name, path, entry, ref;
    };
    std::vector<Job> jobs;
    for (const json::Value &r : rows.items) {
        const std::string &name = r.items.at(0).s, &arc = r.items.at(1).s, &entry = r.items.at(2).s, &ref = r.items.at(3).s;
        if (!only.empty() && !only.count(name)) continue;
        const fs::path dst = outd / upath(name);
        if (fs::is_regular_file(dst) && !a.has("--force")) {
            const std::string cur = sha_file(dst);
            auto st = state.find(name);
            if (cur == ref || (st != state.end() && st->second == cur)) {
                skipped++;
                continue;
            }
        }
        const size_t c = arc.find(':');
        jobs.push_back({name, utf8(upath(roots.at(arc.substr(0, c))) / upath(arc.substr(c + 1))), entry, ref});
    }
    struct Res {
        Bytes out;
        bool ok = false;
        std::string msg, fatal;
    };
    std::vector<Res> res(jobs.size());
    std::mutex lock;
    std::map<std::string, std::pair<std::unique_ptr<Psarc>, std::map<std::string, size_t>>> arcs;
    std::atomic<size_t> next{0};
    const char *rj = std::getenv("RECIPE_JOBS");
    const int nj = a.val.count("--jobs") ? std::stoi(a.get("--jobs", "1")) : rj && *rj ? std::atoi(rj) : std::max(1, std::min(6, int(std::thread::hardware_concurrency()) - 1));
    std::vector<std::thread> pool;
    for (int w = 0; w < std::max(1, std::min<int>(nj, int(jobs.size()))); w++)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < jobs.size();) {
                const Job &j = jobs[i];
                Bytes data;
                {
                    std::lock_guard<std::mutex> g(lock);
                    auto it = arcs.find(j.path);
                    if (it == arcs.end()) {
                        if (!fs::is_regular_file(upath(j.path))) {
                            res[i].msg = "missing input " + win_path(j.path);
                            continue;
                        }
                        auto p = std::make_unique<Psarc>(upath(j.path));
                        std::map<std::string, size_t> idx;
                        for (size_t k = 0; k < p->names.size(); k++) idx[p->names[k]] = k + 1;   // a later duplicate wins
                        it = arcs.emplace(j.path, std::make_pair(std::move(p), idx)).first;
                    }
                    auto e = it->second.second.find(j.entry);
                    if (e == it->second.second.end()) {
                        res[i].msg = "missing entry " + j.entry + " in " + win_path(j.path);
                        continue;
                    }
                    data = it->second.first->read(e->second);
                }
                try {
                    res[i].out = convert(data, ff);
                    res[i].ok = true;
                } catch (const Skip &e) {
                    res[i].msg = std::string("skipped: ") + e.what();
                } catch (const std::exception &e) {
                    res[i].fatal = e.what();
                }
            }
        });
    for (auto &t : pool) t.join();
    for (size_t i = 0; i < jobs.size(); i++)
        if (!res[i].fatal.empty()) throw Stop(jobs[i].name + ": " + res[i].fatal);
    for (size_t i = 0; i < jobs.size(); i++) {
        const Job &j = jobs[i];
        const fs::path dst = outd / upath(j.name);
        if (!res[i].ok) {
            std::printf("%-40s %s\n", j.name.c_str(), res[i].msg.c_str());
            missing++;
            continue;
        }
        const std::string h = sha256_hex(res[i].out);
        const bool same = h == j.ref;
        (same ? identical : equivalent)++;
        std::printf("%-40s %s%s\n", j.name.c_str(), same ? "identical to the reference" : "equivalent (re-encoded ADPCM samples)", a.has("--apply") ? "" : " (dry run)");
        if (a.has("--apply")) {
            fs::create_directories(outd);
            if (fs::is_regular_file(dst)) {
                fs::create_directories(backup);
                const fs::path bk = backup / upath(j.name);
                if (!fs::exists(bk)) fs::copy_file(dst, bk);
            }
            write_file_replace(dst, res[i].out);
            state[j.name] = h;
        }
    }
    if (a.has("--apply") && !state.empty()) {
        fs::create_directories(backup);
        std::string t = "{";
        bool first = true;
        for (const auto &[k, v] : state) {
            t += std::string(first ? "\n" : ",\n") + "\"" + k + "\": \"" + v + "\"";
            first = false;
        }
        write_text(statef, t + "\n}");
    }
    if (identical + equivalent == 0 && missing == 0) std::printf("already applied: %d banks up to date\n", skipped);
    else
        std::printf("%s: %d identical, %d equivalent, %d already up to date, %d missing (%.0f s)\n", a.has("--apply") ? "written" : "dry run", identical, equivalent, skipped,
                    missing, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}

}  // namespace u3
