// dctools/dc.cpp: see dc.hpp
#include "dc.hpp"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <memory>
#include <thread>
#include <unordered_map>

namespace u3 {

namespace {

struct CacheEntry {
    std::shared_ptr<Bytes> data;
    uint64_t used = 0, gen = 0;
    bool pinned = false;   // no copy on disk: never evicted, only dropped by generation (read_cache_discard_before) or clear
};
std::mutex g_cache_m;
std::unordered_map<std::string, CacheEntry> g_cache;
uint64_t g_cache_bytes = 0, g_cache_limit = 0, g_cache_tick = 0;
std::atomic<uint64_t> g_cache_gen{0};

// drops the least recently used files until the cache fits its limit. A pinned file is never dropped here: a reader
// may still need it and there is no copy on disk to fall back to, so pinned files can hold the cache above the limit.
void evict_locked() {
    while (g_cache_bytes > g_cache_limit) {
        auto old = g_cache.end();
        for (auto it = g_cache.begin(); it != g_cache.end(); ++it)
            if (!it->second.pinned && (old == g_cache.end() || it->second.used < old->second.used)) old = it;
        if (old == g_cache.end()) return;
        g_cache_bytes -= old->second.data->size();
        g_cache.erase(old);
    }
}

void cache_add_locked(const std::string &k, std::shared_ptr<Bytes> d, uint64_t gen, bool pinned = false) {
    g_cache_bytes += d->size();
    g_cache[k] = {std::move(d), ++g_cache_tick, gen, pinned};
    evict_locked();
}

void cache_drop_locked(const fs::path &p) {
    auto it = g_cache.find(utf8(p));
    if (it == g_cache.end()) return;
    g_cache_bytes -= it->second.data->size();
    g_cache.erase(it);
}

}  // namespace

void set_read_cache_mb(size_t mb) {
    std::lock_guard<std::mutex> l(g_cache_m);
    g_cache_limit = uint64_t(mb) << 20;
    if (!g_cache_limit) {
        g_cache.clear();
        g_cache_bytes = 0;
        return;
    }
    evict_locked();
}

bool prefetch_read(const fs::path &p) {
    const std::string k = utf8(p);
    {
        std::lock_guard<std::mutex> l(g_cache_m);
        if (!g_cache_limit) return false;
        auto it = g_cache.find(k);
        if (it != g_cache.end()) {
            it->second.used = ++g_cache_tick;
            return true;
        }
    }
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec) return false;
    {
        std::lock_guard<std::mutex> l(g_cache_m);
        if (sz > g_cache_limit) return false;
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    auto d = std::make_shared<Bytes>(size_t(sz));
    if (sz) f.read(reinterpret_cast<char *>(d->data()), std::streamsize(sz));
    if (!f) return false;
    std::lock_guard<std::mutex> l(g_cache_m);
    if (!g_cache_limit) return false;
    if (!g_cache.count(k)) cache_add_locked(k, std::move(d), g_cache_gen.load());
    return true;
}

bool have_read_cache(const fs::path &p) {
    std::lock_guard<std::mutex> l(g_cache_m);
    return g_cache_limit && g_cache.count(utf8(p)) != 0;
}

bool provide_read_cache(const fs::path &p, Bytes data, bool pin) {
    auto d = std::make_shared<Bytes>(std::move(data));
    std::lock_guard<std::mutex> l(g_cache_m);
    if (!g_cache_limit || d->size() > g_cache_limit) return false;
    const std::string k = utf8(p);
    if (auto it = g_cache.find(k); it != g_cache.end()) {
        g_cache_bytes -= it->second.data->size();
        g_cache.erase(it);
    }
    cache_add_locked(k, std::move(d), g_cache_gen.load(), pin);
    return true;
}

std::string fmt(const char *f, ...) {
    va_list a, b;
    va_start(a, f);
    va_copy(b, a);
    int n = std::vsnprintf(nullptr, 0, f, a);
    va_end(a);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) std::vsnprintf(s.data(), n + 1, f, b);
    va_end(b);
    return s;
}

std::string hex8(uint32_t v) { return fmt("%08x", v); }

std::string utf8(const fs::path &p) {
    auto u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

Bytes read_file(const fs::path &p) {
    const std::string k = utf8(p);
    std::shared_ptr<Bytes> hit;
    {
        std::lock_guard<std::mutex> l(g_cache_m);
        if (g_cache_limit) {
            auto it = g_cache.find(k);
            if (it != g_cache.end()) {
                it->second.used = ++g_cache_tick;
                hit = it->second.data;
            }
        }
    }
    if (hit) return *hit;
    std::error_code ec;
    const auto sz = fs::file_size(p, ec);
    if (ec) throw Stop("cannot read " + k);
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Stop("cannot read " + k);
    Bytes d(static_cast<size_t>(sz));
    if (sz) f.read(reinterpret_cast<char *>(d.data()), std::streamsize(sz));
    if (!f) throw Stop("cannot read " + k);
    return d;
}

void write_file_replace(const fs::path &p, const Bytes &data) {
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) throw Stop("cannot write " + utf8(tmp));
        f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!f) throw Stop("cannot write " + utf8(tmp));
    }
    // another process (a virus scanner, the search indexer) may hold the old file for a moment: retry for up to 2 s
    for (int k = 0;; k++) {
        std::error_code ec;
        fs::rename(tmp, p, ec);
        if (!ec) {
            {
                std::lock_guard<std::mutex> l(g_cache_m);
                cache_drop_locked(p);
            }
            return;
        }
        if (k == 100) fs::rename(tmp, p);   // throws with the error
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void copy_once(const fs::path &src, const fs::path &dst) {
    if (!fs::exists(dst)) fs::copy_file(src, dst);
}

uint32_t sid(const std::string &s) {
    uint32_t c = 0;
    for (unsigned char ch : s) {
        c ^= uint32_t(ch) << 24;
        for (int i = 0; i < 8; i++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

// ---------------------------------------------------------------- SHA-256 (the step logs record file hashes)
namespace {
constexpr std::array<uint32_t, 64> K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
}  // namespace

void clear_read_cache() {
    std::lock_guard<std::mutex> l(g_cache_m);
    g_cache.clear();
    g_cache_bytes = 0;
}

uint64_t read_cache_new_generation() { return g_cache_gen.fetch_add(1) + 1; }

void read_cache_discard_before(uint64_t gen) {
    std::lock_guard<std::mutex> l(g_cache_m);
    for (auto it = g_cache.begin(); it != g_cache.end();)
        if (it->second.gen && it->second.gen < gen) {
            g_cache_bytes -= it->second.data->size();
            it = g_cache.erase(it);
        } else {
            ++it;
        }
}

std::string sha256_hex(const Bytes &data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Bytes m(data);
    uint64_t bits = uint64_t(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back(uint8_t(bits >> (8 * i)));
    for (size_t blk = 0; blk < m.size(); blk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = uint32_t(m[blk + 4 * i]) << 24 | uint32_t(m[blk + 4 * i + 1]) << 16 | uint32_t(m[blk + 4 * i + 2]) << 8 |
                   m[blk + 4 * i + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], dd = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = dd + t1; dd = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += dd; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string out;
    for (uint32_t v : h) out += fmt("%08x", v);
    return out;
}

// ---------------------------------------------------------------- Dc
Dc::Dc(const fs::path &path) : d(read_file(path)), name(utf8(path)) { parse(); }
Dc::Dc(Bytes data, std::string n) : d(std::move(data)), name(std::move(n)) { parse(); }

uint32_t Dc::u32(uint64_t o) const {
    if (o + 4 > d.size()) throw Stop(fmt("%s: read past the end at %llx", name.c_str(), (unsigned long long)o));
    uint32_t v = rd32le(&d[o]);
    return be ? __builtin_bswap32(v) : v;
}

uint64_t Dc::ptr(uint64_t o) const {
    if (ps == 4) return u32(o);
    if (o + 8 > d.size()) throw Stop(fmt("%s: read past the end at %llx", name.c_str(), (unsigned long long)o));
    return rd64le(&d[o]);
}

std::optional<std::string> Dc::cstr(uint64_t o) const {
    if (!o || o >= d.size()) return std::nullopt;
    uint64_t z = o;
    while (z < d.size() && d[z]) z++;
    if (z == d.size()) z = d.size() - 1;        // no terminator: as the Python reader (find -> -1)
    return std::string(reinterpret_cast<const char *>(&d[o]), z - o);
}

void Dc::parse() {
    if (d.size() < 0x18) throw Stop(name + ": not a DC file");
    if (!std::memcmp(d.data(), "DC00", 4)) { be = true; ps = 4; }
    else if (!std::memcmp(d.data(), "00CD", 4)) { be = false; ps = 8; }
    else throw Stop(name + ": not a DC file");
    reloc = ps == 4 ? u32(8) : rd64le(&d[8]);
    uint32_t n = u32(0x14);
    uint64_t o = 0x18;
    for (uint32_t i = 0; i < n; i++) {
        Sym s;
        if (ps == 4) { s.sid = u32(o); s.type = u32(o + 4); s.off = u32(o + 8); o += 12; }
        else { s.sid = u32(o); s.type = u32(o + 4); s.off = ptr(o + 8); o += 16; }
        syms.push_back(s);
        bysid[s.sid] = {s.type, s.off};
    }
    if (reloc && reloc < d.size()) {
        uint32_t cnt = u32(reloc);
        uint64_t end = std::min<uint64_t>(reloc + 4 + cnt, d.size());
        for (uint64_t bi = 0; reloc + 4 + bi < end; bi++) {
            uint8_t byte = d[reloc + 4 + bi];
            for (int k = 0; byte && k < 8; k++)
                if (byte & (1 << k)) ptrs.insert((bi * 8 + (be ? 7 - k : k)) * ps);
        }
    }
}

// ---------------------------------------------------------------- DcEdit
DcEdit::DcEdit(const fs::path &path) : orig(read_file(path)) {
    if (orig.size() < 16 || std::memcmp(orig.data(), "00CD", 4)) throw Stop(utf8(path) + ": not a PS4 DC");
    reloc = rd64le(&orig[8]);
    if (reloc + 4 > orig.size()) throw Stop(utf8(path) + ": bad relocation offset");
    uint32_t n = rd32le(&orig[reloc]);
    if (reloc + 4 + n != orig.size()) throw Stop(utf8(path) + ": unexpected data after relocation bitmap");
    for (uint64_t bi = 0; bi < n; bi++) {
        uint8_t b = orig[reloc + 4 + bi];
        for (int k = 0; b && k < 8; k++)
            if (b >> k & 1) ptrs.insert((bi * 8 + k) * 8);
    }
    d.assign(orig.begin(), orig.begin() + static_cast<std::ptrdiff_t>(reloc));
}

void DcEdit::check(uint64_t o, size_t n) const {
    if (o + n > d.size()) throw Stop(fmt("internal: write past the end at %llx", (unsigned long long)o));
}

void DcEdit::put32(uint64_t o, uint32_t v) {
    check(o, 4);
    if (o < reloc) patched.push_back({o, 4});
    wr32le(&d[o], v);
}

void DcEdit::putptr(uint64_t o, uint64_t target) {
    if (o % 8) throw Stop(fmt("internal: unaligned pointer slot %llx", (unsigned long long)o));
    check(o, 8);
    if (o < reloc) patched.push_back({o, 8});
    wr64le(&d[o], target);
    ptrs.insert(o);
}

uint64_t DcEdit::alloc(size_t size, size_t align_to) {
    align(align_to);
    uint64_t o = d.size();
    d.resize(d.size() + size, 0);
    return o;
}

uint64_t DcEdit::add_string(const std::string &s) {
    uint64_t o = d.size();
    d.insert(d.end(), s.begin(), s.end());
    d.push_back(0);
    return o;
}

void DcEdit::copy_words(uint64_t src, uint64_t dst, size_t size) {
    if (src % 8 || dst % 8 || size % 8) throw Stop("internal: copy_words alignment");
    if (src + size > orig.size()) throw Stop("internal: copy_words source past the end");
    check(dst, size);
    std::memcpy(&d[dst], &orig[src], size);
    for (size_t k = 0; k < size; k += 8)
        if (ptrs.count(src + k)) ptrs.insert(dst + k);
}

Bytes DcEdit::build() {
    align(16);
    uint64_t r = d.size();
    uint64_t nbytes = (r + 63) / 64;
    Bytes bm(nbytes, 0);
    for (uint64_t p : ptrs) {
        if (p >= r || p % 8) throw Stop(fmt("internal: pointer slot %llx", (unsigned long long)p));
        uint64_t w = p / 8;
        bm[w / 8] |= uint8_t(1u << (w % 8));
    }
    Bytes out(d);
    wr64le(&out[8], r);
    uint8_t cnt[4];
    wr32le(cnt, uint32_t(nbytes));
    out.insert(out.end(), cnt, cnt + 4);
    out.insert(out.end(), bm.begin(), bm.end());
    return out;
}

uint64_t module_size_slot(const Dc &m, const std::string &name) {
    constexpr uint32_t MODULE_T = 0x61183179;
    std::vector<uint64_t> hits;
    for (const Sym &s : m.syms) {
        if (s.type != MODULE_T) continue;
        uint32_t n = m.u32(s.off);
        uint64_t arr = m.ptr(s.off + 8);
        for (uint32_t i = 0; i < n; i++) {
            uint64_t e = arr + uint64_t(i) * 0x28;
            if (m.isptr(e) && m.cstr(m.ptr(e)) == name) hits.push_back(e + 0x10);
        }
    }
    if (hits.size() != 1) throw Stop(fmt("modules.bin: %d entries for module '%s' (expected 1)", int(hits.size()), name.c_str()));
    return hits[0];
}

}  // namespace u3
