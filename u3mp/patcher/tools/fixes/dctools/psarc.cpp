// dctools/psarc.cpp: see psarc.hpp
#include "psarc.hpp"

#include "inflate.h"   // launcher/native/inflate.h: raw deflate
#include "u3fix.hpp"

#include <algorithm>
#include <cstring>

namespace u3 {
namespace {

uint64_t be(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}

// zlib.decompress: header, deflate stream, Adler-32; false when the data is not a valid zlib stream
bool unzlib(const Bytes &in, Bytes &out) {
    if (in.size() < 2 || (in[0] & 0x0f) != 8 || (in[0] >> 4) > 7 || (in[0] * 256 + in[1]) % 31 != 0 || (in[1] & 0x20)) return false;
    std::string o;
    size_t used;
    try {
        inflate::Inflater z(in.data() + 2, in.size() - 2, o);
        z.run();
        used = z.consumed();
    } catch (const inflate::InflateError &) {
        return false;
    }
    if (2 + used + 4 > in.size()) return false;
    uint32_t a = 1, b = 0;
    for (unsigned char c : o) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    if (uint32_t(be(&in[2 + used], 4)) != (b << 16 | a)) return false;
    out.assign(o.begin(), o.end());
    return true;
}

}  // namespace

Psarc::Psarc(const fs::path &p) : f(p, std::ios::binary), path(p) {
    if (!f) throw Stop("cannot read " + utf8(p));
    uint8_t h[32] = {};
    f.read(reinterpret_cast<char *>(h), 32);
    if (f.gcount() != 32 || std::memcmp(h, "PSAR", 4)) throw Stop("AssertionError: not a PSARC archive: " + utf8(p));
    const uint32_t toc_len = uint32_t(be(h + 12, 4)), es = uint32_t(be(h + 16, 4)), n = uint32_t(be(h + 20, 4));
    block_size = uint32_t(be(h + 24, 4));
    if (toc_len < 32 || es < 30) throw Stop("bad PSARC table of contents: " + utf8(p));
    Bytes t(toc_len - 32);
    f.read(reinterpret_cast<char *>(t.data()), std::streamsize(t.size()));
    if (size_t(f.gcount()) != t.size() || uint64_t(n) * es > t.size()) throw Stop("PSARC table of contents cut off: " + utf8(p));
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = &t[size_t(i) * es];
        toc.push_back({uint32_t(be(e + 16, 4)), be(e + 20, 5), be(e + 25, 5)});
    }
    int nb = 1;
    for (uint64_t x = block_size; x > 256; x /= 256) nb++;
    for (size_t o = size_t(n) * es; o + nb <= t.size(); o += nb) blocks.push_back(be(&t[o], nb));
    if (!toc.empty()) {
        const Bytes b = read(0);
        std::string s;
        for (uint8_t c : b)
            if (c != '\r') s += char(c);
        size_t a = 0;
        for (size_t k; (k = s.find('\n', a)) != std::string::npos; a = k + 1) names.push_back(s.substr(a, k - a));
        names.push_back(s.substr(a));
        if (!names.empty() && names.back().empty()) names.pop_back();
    }
}

Bytes Psarc::read(size_t i) {
    if (i >= toc.size()) throw Stop("IndexError: no entry " + std::to_string(i) + " in " + utf8(path));
    const Entry &e = toc[i];
    Bytes out;
    f.clear();
    f.seekg(std::streamoff(e.offset));
    size_t bi = e.first_block;
    while (out.size() < e.length) {
        if (bi >= blocks.size()) throw Stop("IndexError: block table of " + utf8(path) + " ends inside entry " + std::to_string(i));
        const uint64_t z = blocks[bi++];
        Bytes d(z ? z : block_size);
        f.read(reinterpret_cast<char *>(d.data()), std::streamsize(d.size()));
        d.resize(size_t(f.gcount()));
        f.clear();
        if (z) {
            const bool zhead = d.size() >= 2 && d[0] == 0x78 && (d[1] == 0xda || d[1] == 0x9c || d[1] == 0x01);
            const uint64_t want = std::min<uint64_t>(block_size, e.length - out.size());
            if (!(d.size() == want && !zhead)) {
                Bytes u;
                if (unzlib(d, u)) d.swap(u);
                else if ((d.size() >= 6 && !std::memcmp(d.data(), "\xfd" "7zXZ\0", 6)) || (!d.empty() && d[0] == 0x5d))
                    throw Stop("an LZMA-compressed block in " + utf8(path) + ": not supported");   // the script tried lzma here
            }
        }
        out.insert(out.end(), d.begin(), d.end());
    }
    out.resize(size_t(e.length));
    return out;
}

std::string ps3_to_ps4_rel(const std::string &entry) {
    std::string e = entry;
    std::replace(e.begin(), e.end(), '\\', '/');
    e.erase(0, e.find_first_not_of('/') == std::string::npos ? e.size() : e.find_first_not_of('/'));
    for (const char *pre : {"build/main/bin/", "bin/", "build/main/"})
        if (e.rfind(pre, 0) == 0) {
            e = e.substr(std::strlen(pre));
            break;
        }
    if (e.rfind("dc1/", 0) == 0) return "dc1/ps4/" + e.substr(4);
    if (e.rfind("dc2/", 0) == 0) return e;
    return "";
}

Ps3Index::Ps3Index(const std::string &disc, const std::string &update, const std::string &data2) {
    static const char *const ORDER[] = {"data2", "patch18", "patch13", "goty-patch", "goty-dlc", "disc"};
    if (!data2.empty() && fs::exists(upath(data2 + "/net18.bin"))) map["dc1/ps4/net.bin"].push_back({"data2", data2 + "/net18.bin", 0});
    std::vector<std::pair<std::string, std::string>> cands;
    if (!update.empty()) cands = {{"patch18", update + "/dlc-patch18.psarc"}, {"patch13", update + "/dlc-patch13.psarc"}};
    if (!disc.empty()) {
        cands.push_back({"goty-patch", disc + "/goty-patch.psarc"});
        cands.push_back({"goty-dlc", disc + "/goty-dlc.psarc"});
        cands.push_back({"disc", disc + "/build/main/bin.psarc"});
    }
    for (const auto &[layer, p] : cands) {
        if (!fs::exists(upath(p))) continue;
        auto a = std::make_unique<Psarc>(upath(p));
        for (size_t i = 0; i < a->names.size(); i++) {
            const std::string r = ps3_to_ps4_rel(a->names[i]);
            if (!r.empty() && (r.size() > 4 && (r.compare(r.size() - 4, 4, ".bin") == 0 || r.compare(r.size() - 4, 4, ".dci") == 0)))
                map[r].push_back({layer, p, i + 1});
        }
        arch[p] = std::move(a);
    }
    auto rank = [](const std::string &l) { return std::find(std::begin(ORDER), std::end(ORDER), l) - std::begin(ORDER); };
    for (auto &[r, v] : map) std::stable_sort(v.begin(), v.end(), [&](const Layer &x, const Layer &y) { return rank(x.layer) < rank(y.layer); });
}

std::optional<Bytes> Ps3Index::read(const std::string &rel, const std::string &layer) {
    auto it = map.find(rel);
    if (it == map.end()) return std::nullopt;
    for (const Layer &l : it->second) {
        if (!layer.empty() && l.layer != layer) continue;
        if (l.entry == 0) return read_file(upath(l.archive));
        return arch.at(l.archive)->read(l.entry);
    }
    return std::nullopt;
}

}  // namespace u3
