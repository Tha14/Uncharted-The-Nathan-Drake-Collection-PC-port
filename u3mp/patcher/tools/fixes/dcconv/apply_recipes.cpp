// Recipe step dc-recipes: the overlay DC files convert-dc1 does not reproduce (the merged modules net.bin, menu.bin,
// modules.bin, medals, looks, scenes, vox*, weapon-*, net-always-loaded(.dci), ss-coop-adv-fort, the 3 dc2 part
// modules, overlay-r2 / overlay-port net / medals / modules / render-settings, and the 80 conversions convert-dc1 does
// not reproduce, with their .dci) rebuilt byte for byte from the player's PS4 and PS3 files. Replaces
// apply_recipes.py and dcrecipe.py apply.
//   u3fix dc-recipes --project P --dcconv D --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D [--apply] [--only TEXT]
// D (tools/dcconv): dc-recipes.json.gz, one provenance recipe per file: copy ops (source, byte transform r raw / w
// 32-bit swap / h 16-bit halves swap / o script opcodes renumbered, offset, length), zero runs, a few literal bytes and
// the pointer slots (the relocation bitmap is recomputed from them); the sources are checked by sha256 and every
// rebuilt file must have the recorded sha256. net18-versions.json: other accepted versions of the server's net18.bin
// (their newer values land in the same copied slots, so only the size is checked). schema-oldconv.json: the opcode map.
// Dry run by default; files already at the recorded hash are left alone.
#include "inflate.h"
#include "json.h"
#include "proc.hpp"
#include "psarc.hpp"
#include "u3fix.hpp"

namespace u3 {
namespace {

Bytes xform(const Bytes &buf, char x, const std::map<int, int> &opmap) {
    if (x == 'r') return buf;
    Bytes out;
    if (x == 'o') {
        out = buf;
        for (size_t i = 0; i + 3 < out.size(); i += 4)
            if (auto it = opmap.find(out[i]); it != opmap.end()) out[i] = uint8_t(it->second);
        return out;
    }
    out.assign(buf.size(), 0);
    const size_t n = buf.size() - buf.size() % 4;
    for (size_t i = 0; i < n; i += 4) {
        if (x == 'w') out[i] = buf[i + 3], out[i + 1] = buf[i + 2], out[i + 2] = buf[i + 1], out[i + 3] = buf[i];
        else if (x == 'h') out[i] = buf[i + 1], out[i + 1] = buf[i], out[i + 2] = buf[i + 3], out[i + 3] = buf[i + 2];
    }
    std::copy(buf.begin() + std::ptrdiff_t(n), buf.end(), out.begin() + std::ptrdiff_t(n));
    return out;
}

struct Fail : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// rebuild a recipe's file; accepted: other accepted versions (sha256) per source name
Bytes apply(const json::Value &rec, std::map<std::string, std::optional<Bytes>> &sources, const std::map<std::string, std::set<std::string>> &accepted,
            const std::map<int, int> &opmap) {
    bool other = false;
    const json::Value *srcs = rec.get("sources");
    if (!srcs) throw Stop("dc recipe without sources");
    for (const auto &[n, h] : srcs->fields) {
        auto it = sources.find(n);
        if (it == sources.end()) throw Fail("missing source " + n);
        if (!it->second) throw Fail("TypeError: a bytes-like object is required, not 'NoneType'");
        const std::string s = sha256_hex(*it->second);
        if (s != h.s) {
            auto v = accepted.find(n);
            if (v == accepted.end() || !v->second.count(s)) throw Fail("source " + n + " differs from the one the recipe was made from");
            other = true;
        }
    }
    std::map<std::pair<std::string, char>, Bytes> cache;
    Bytes out;
    for (const json::Value &op : rec.get("ops")->items) {
        const std::string &k = op.items.at(0).s;
        if (k == "z") out.resize(out.size() + size_t(op.items.at(1).i), 0);
        else if (k == "l") {
            const std::string &h = op.items.at(1).s;
            for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)));
        } else {
            const std::string &name = op.items.at(1).s;
            const char x = op.items.at(2).s.empty() ? 'r' : op.items.at(2).s[0];
            const int64_t j = op.items.at(3).i, n = op.items.at(4).i;
            auto c = cache.find({name, x});
            if (c == cache.end()) c = cache.emplace(std::make_pair(name, x), xform(*sources.at(name), x, opmap)).first;
            const Bytes &b = c->second;
            const int64_t a = std::clamp<int64_t>(j, 0, int64_t(b.size())), e = std::clamp<int64_t>(j + n, a, int64_t(b.size()));
            out.insert(out.end(), b.begin() + a, b.begin() + e);
        }
    }
    const json::Value *slots = rec.get("slots");
    for (const json::Value &s : slots->items) {
        const int64_t o = s.items.at(0).i;
        const uint64_t v = uint64_t(s.items.at(1).i);
        if (o < 0 || o + 8 > int64_t(out.size())) throw Stop("struct.error: pack_into out of range");
        for (int k = 0; k < 8; k++) out[size_t(o + k)] = uint8_t(v >> (8 * k));
    }
    if (const json::Value *dc = rec.get("dc"); dc && dc->truthy()) {
        const int64_t reloc = rec.get("body_end")->i, nb = (reloc + 63) / 64;
        Bytes bm(size_t(nb), 0);
        for (const json::Value &s : slots->items) {
            const int64_t w = s.items.at(0).i / 8;
            bm.at(size_t(w / 8)) |= uint8_t(1 << (w % 8));
        }
        for (int k = 0; k < 4; k++) out.push_back(uint8_t(uint32_t(nb) >> (8 * k)));
        out.insert(out.end(), bm.begin(), bm.end());
    }
    if (int64_t(out.size()) != rec.get("size")->i || (!other && sha256_hex(out) != rec.get("sha256")->s)) throw Fail("rebuilt file differs from the recipe target");
    return out;
}

}  // namespace

int dc_recipes(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--dcconv", "--ps4", "--ps3-disc", "--ps3-update", "--ps3-data2", "--only"});
    const std::string project = a.need("--project"), main4 = a.need("--ps4") + "/u3data/build/main/";
    const fs::path dd = upath(a.need("--dcconv"));
    json::Value R;
    {
        const Bytes gz = read_file(dd / "dc-recipes.json.gz");
        if (!json::parse(inflate::gunzip(std::string(gz.begin(), gz.end())), R)) throw Stop("dc-recipes.json.gz is not JSON");
    }
    std::map<int, int> opmap;
    {
        json::Value s;
        if (!json::parse(text_file(dd / "schema-oldconv.json"), s)) throw Stop("schema-oldconv.json is not JSON");
        if (const json::Value *o = s.get("opmap"))
            for (const auto &[k, v] : o->fields) opmap[std::stoi(k)] = int(v.i);
    }
    // other accepted versions of the server's net18.bin (hashes only)
    std::map<std::string, std::set<std::string>> accepted;
    if (fs::exists(dd / "net18-versions.json")) {
        json::Value v;
        if (!json::parse(text_file(dd / "net18-versions.json"), v)) throw Stop("net18-versions.json is not JSON");
        if (const json::Value *acc = v.get("accepted")) {
            auto &s = accepted["ps3/data2:dc1/ps4/net.bin"];
            for (const auto &[h, x] : acc->fields) s.insert(h);
            for (const json::Value &h : acc->items) s.insert(h.s);
        }
    }
    Ps3Index idx(a.get("--ps3-disc", ""), a.get("--ps3-update", ""), a.get("--ps3-data2", ""));
    std::map<std::string, std::optional<Bytes>> cache;
    auto src = [&](const std::string &name) -> std::optional<Bytes> {
        auto it = cache.find(name);
        if (it != cache.end()) return it->second;
        const size_t c = name.find(':');
        const std::string kind = name.substr(0, c), rel = name.substr(c + 1);
        std::optional<Bytes> d;
        if (kind == "ps4") {
            if (!fs::exists(upath(main4 + rel))) throw Fail("[Errno 2] No such file or directory: '" + main4 + rel + "'");
            d = read_file(upath(main4 + rel));
        } else
            d = idx.read(rel, kind.substr(kind.find('/') + 1));
        cache[name] = d;
        return d;
    };
    std::vector<std::pair<std::string, int>> tot;
    auto count = [&](const std::string &k) {
        for (auto &x : tot)
            if (x.first == k) { x.second++; return; }
        tot.push_back({k, 1});
    };
    std::vector<const std::pair<std::string, json::Value> *> recs;
    for (const auto &f : R.fields) recs.push_back(&f);
    std::stable_sort(recs.begin(), recs.end(), [](const auto *x, const auto *y) { return x->first < y->first; });
    const std::string only = a.get("--only", "");
    for (const auto *kv : recs) {
        const std::string &key = kv->first;
        const json::Value &rec = kv->second;
        if (!only.empty() && key.find(only) == std::string::npos) continue;
        const size_t sl = key.find('/');
        const fs::path dst = upath(project + "/work/" + key.substr(0, sl) + "/u3data/build/main/" + key.substr(sl + 1));
        const std::string want = rec.get("sha256")->s;
        if (fs::exists(dst) && sha_file(dst) == want) {
            count("already");
            continue;
        }
        Bytes out;
        try {
            std::map<std::string, std::optional<Bytes>> s;
            for (const auto &[n, h] : rec.get("sources")->fields) s[n] = src(n);
            out = apply(rec, s, accepted, opmap);
        } catch (const Fail &ex) {
            count("failed");
            std::printf("FAILED %s %s\n", key.c_str(), ex.what());
            continue;
        }
        if (sha256_hex(out) != want) {   // built from another accepted net18.bin: the reference structure with the server's newer values
            if (fs::exists(dst) && read_file(dst) == out) {
                count("already");
                continue;
            }
            count("rebuilt (newer server net18)");
            std::printf("rebuilt from the newer server net18.bin: %s\n", key.c_str());
        } else
            count("rebuilt");
        if (a.has("--apply")) {
            fs::create_directories(dst.parent_path());
            write_file_replace(dst, out);
        }
    }
    std::string d = "{";
    int failed = 0;
    for (size_t i = 0; i < tot.size(); i++) {
        d += (i ? ", '" : "'") + tot[i].first + "': " + std::to_string(tot[i].second);
        if (tot[i].first == "failed") failed = tot[i].second;
    }
    std::printf("dc-recipes: %s} %s\n", d.c_str(), a.has("--apply") ? "" : "(dry run, nothing written)");
    return failed ? 1 : 0;
}

}  // namespace u3
