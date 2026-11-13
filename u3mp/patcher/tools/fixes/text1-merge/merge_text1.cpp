// Recipe step convert-text1: overlay-r3 text1, the PS4 core.psarc text banks 2..23 merged with the PS3 multiplayer
// strings.   u3fix text1-merge --ps4 D --ps3-disc D --project P [--out D] [--backup D] [--apply]
//
// Text bank (big endian on PS3 and PS4): u32 count; count x (u32 hash, u32 offset) sorted by hash; then the string blob
// (offsets from its start, strings NUL-terminated). For each language 2..23 and category common / networking /
// subtitles: start from the PS4 bank text1/ps4/<lang>.<cat> of u3data/build/main/core.psarc (its blob kept as is);
// for every key of KEYS[cat] in that order whose string in the PS3 goty-patch.psarc build/main/text1/<lang>.<cat> is
// missing from the PS4 bank or differs, append the PS3 string and point the record at it; then append the helper's
// own message (English in every language) to <lang>.common. Files already equal to the result are left alone. Dry run
// without --apply; --apply backs replaced files up once to work/backup-text1-merge/. Replaces merge_text1.py.
#include "u3fix.hpp"
#include "psarc.hpp"

#include <algorithm>
#include <cstdio>

namespace u3 {
namespace {

struct Bank {
    std::map<uint32_t, uint32_t> recs;   // hash -> offset (a later record of a hash wins)
    Bytes blob;
};

Bank parse(const Bytes &d) {
    Bank b;
    const uint32_t n = uint32_t(be32(d, 0));
    for (uint32_t i = 0; i < n; i++) b.recs[uint32_t(be32(d, 4 + 8ull * i))] = uint32_t(be32(d, 8 + 8ull * i));
    b.blob.assign(d.begin() + std::ptrdiff_t(std::min<uint64_t>(4 + 8ull * n, d.size())), d.end());
    return b;
}

Bytes string_at(const Bytes &blob, uint32_t o) {
    for (size_t k = o; k < blob.size(); k++)
        if (!blob[k]) return Bytes(blob.begin() + o, blob.begin() + std::ptrdiff_t(k));
    throw Stop("ValueError: a text bank string without its NUL");
}

void put_be(Bytes &o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s));
}

}  // namespace

int text1_merge(const std::vector<std::string> &argv) {
    static const std::map<std::string, std::vector<uint32_t>> KEYS = {
        {"common", {0x23a8bc26, 0x5a32b147, 0xa13fea91, 0xeaff5517, 0xec7dd02d}},
        {"networking", {0x06b674d2, 0x0bf5520b, 0x0d751ea9, 0x2337fc6d, 0x292e3933, 0x3142b4b9, 0x34a82676, 0x3c019260, 0x40b4fcf5,
                        0x4936c79b, 0x4b9fc741, 0x4df7da2c, 0x4fb13f09, 0x5089f550, 0x53b08a29, 0x5448e8e7, 0x5771979e, 0x5a32b147,
                        0x5b81d425, 0x5dcad389, 0x5ef3acf0, 0x627d0cfa, 0x66091197, 0x6632d07d, 0x66bc114d, 0x68b998bd, 0x6d39fb20,
                        0x71fa6cb2, 0x72ce8aea, 0x80312981, 0x80c4bacf, 0x86dcad2f, 0x8d720f58, 0xa56b5768, 0xb5864132, 0xb5a7aba3,
                        0xbb8baf42, 0xe012fa1d, 0xeba721fe, 0xecb2dcd1, 0xfebc1573}},
        {"subtitles", {0x9291ae5e}}};
    static const char CUSTOM[] = "Your party does not have enough players for the selected game.";
    static const uint32_t CUSTOM_KEY = 0x4a93e6b9;
    static const char *const CATS[] = {"common", "networking", "subtitles"};
    const Args a(argv, {"--apply"}, {"--ps4", "--ps3-disc", "--project", "--out", "--backup"});
    const std::string project = a.get("--project", "");
    const std::string out = a.get("--out", project + "/work/overlay-r3/u3data/build/main/text1/ps4");
    const std::string backup = a.get("--backup", project + "/work/backup-text1-merge");
    const bool apply = a.has("--apply");
    Psarc core(upath(a.need("--ps4") + "/u3data/build/main/core.psarc"));
    Psarc gp(upath(a.need("--ps3-disc") + "/goty-patch.psarc"));
    std::map<std::string, size_t> idx4, idx3;   // a later entry of a name wins, as the script's dict
    for (size_t i = 0; i < core.names.size(); i++) idx4[core.names[i]] = i + 1;
    for (size_t i = 0; i < gp.names.size(); i++) idx3[gp.names[i]] = i + 1;
    int nw = 0, ns = 0;
    for (int lang = 2; lang < 24; lang++)
        for (const char *cat : CATS) {
            const std::string f = std::to_string(lang) + "." + cat, k4 = "text1/ps4/" + f, k3 = "build/main/text1/" + f;
            if (!idx4.count(k4) || !idx3.count(k3)) throw Stop("missing input " + k4 + " (PS4 core.psarc) or " + k3 + " (PS3 goty-patch.psarc)");
            Bank b = parse(core.read(idx4[k4]));
            const Bank p3 = parse(gp.read(idx3[k3]));
            int changed = 0;
            for (uint32_t h : KEYS.at(cat)) {
                auto it = p3.recs.find(h);
                if (it == p3.recs.end()) continue;
                const Bytes s = string_at(p3.blob, it->second);
                auto own = b.recs.find(h);
                if (own != b.recs.end() && string_at(b.blob, own->second) == s) continue;
                b.recs[h] = uint32_t(b.blob.size());
                b.blob.insert(b.blob.end(), s.begin(), s.end());
                b.blob.push_back(0);
                changed++;
            }
            if (std::string(cat) == "common") {
                b.recs[CUSTOM_KEY] = uint32_t(b.blob.size());
                b.blob.insert(b.blob.end(), CUSTOM, CUSTOM + sizeof CUSTOM);   // with its NUL
                changed++;
            }
            Bytes o;
            put_be(o, uint32_t(b.recs.size()));
            for (const auto &[h, off] : b.recs) {
                put_be(o, h);
                put_be(o, off);
            }
            o.insert(o.end(), b.blob.begin(), b.blob.end());
            const fs::path dst = upath(out) / f;
            if (fs::is_regular_file(dst) && read_file(dst) == o) {
                ns++;
                continue;
            }
            std::printf("%-16s %d strings merged%s\n", f.c_str(), changed, apply ? "" : " (dry run)");
            if (apply) {
                fs::create_directories(upath(out));
                if (fs::is_regular_file(dst)) {
                    fs::create_directories(upath(backup));
                    const fs::path bk = upath(backup) / f;
                    if (!fs::exists(bk)) fs::copy_file(dst, bk);
                }
                write_file_replace(dst, o);
            }
            nw++;
        }
    if (nw == 0) std::printf("already applied: %d text banks up to date\n", ns);
    else std::printf("%s %d text banks (%d already up to date)\n", apply ? "wrote" : "would write", nw, ns);
    return 0;
}

}  // namespace u3
