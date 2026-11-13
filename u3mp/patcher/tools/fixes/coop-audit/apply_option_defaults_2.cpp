// Recipe step coop-audit-option-defaults: the second batch of lost option-default slots (the defect of step
// enemy-stall-option-defaults) in overlay-r3 dc1/ps4: 15 entries in ss-coop-adv-fort.bin and ss-multiplayer/*.bin.
//   u3fix coop-audit --project P [--ps3-disc D] [--ps3-update D] [--ps3-data2 D] [--job D] [--apply | --restore]
//
// Each entry's value pointer (entry +0x10) points at the first word of the next structure instead of a zero word; it
// is pointed at file offset 0xc (the zero upper dword of the header's relocation offset), as in the first batch. Every
// entry is checked first: type and option sid, the old target and its value, and the option's default in each PS3
// version (patch18, patch13, goty-patch, disc: read from the player's PS3 archives, or from <job>/p3/<version>/ when
// such extracts are there), which must all be 0 and found at least once. One failed check writes nothing. Dry run by
// default; --apply backs each file up once to work/backup-coop-audit/overlay-r3-dc1/; --restore copies the backups
// back. Replaces apply_option_defaults_2.py (same messages).
#include "u3fix.hpp"
#include "psarc.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace u3 {
namespace {

constexpr uint32_t INT = 0xc7cb275c, FLT = 0x0f182ec3, MAIN = 0x87f13bc2, PLAYER = 0xcde4b742, GRAVE = 0xc9d0aab2;
constexpr uint64_t ZERO = 0xc;
struct Fix {
    const char *file;
    uint64_t entry;
    uint32_t type;
    bool has_sid;
    uint32_t sid;
    uint64_t target;
    uint32_t value;
    const char *why;
};
#define FORT "ss-coop-adv-fort.bin"
#define MP "ss-multiplayer/"
const Fix FIXES[] = {
    {FORT, 0x3f1a8, INT, true, 0x57af0546, 0x3f490, MAIN, "fort adventure"},
    {FORT, 0x40130, INT, true, 0xd50bc34b, 0x40308, MAIN, "fort: team, region-enter stall as in Syria"},
    {FORT, 0x56958, INT, true, 0x104e7d46, 0x56b90, MAIN, "fort: tower-idols idol-placed counter"},
    {FORT, 0x58c88, INT, true, 0xd50bc34b, 0x58df0, MAIN, "fort: team"},
    {FORT, 0x5aeb0, INT, true, 0xad536552, 0x5b0c8, MAIN, "fort adventure"},
    {FORT, 0x5cf30, INT, true, 0x104e7d46, 0x5d2e0, MAIN, "fort: courtyard-idols idol-placed counter"},
    {FORT, 0x67658, INT, true, 0x104e7d46, 0x676e0, MAIN, "fort: idol-placed"},
    {MP "ss-coop-arena-npcs.bin", 0x2ee0, INT, true, 0x011d3371, 0x3018, MAIN, "ss-shade-medal counter (shade arena medal)"},
    {MP "ss-facility-crane.bin", 0xd0, FLT, false, 0, 0x168, MAIN, "start-delay"},
    {MP "ss-grave-events.bin", 0x46c8, INT, true, 0x74188a62, 0x49d8, GRAVE, "ss-grave-container counter"},
    {MP "ss-ldn-streets-events.bin", 0x5b8, INT, true, 0xffe68374, 0xad8, MAIN, "ss-ldn-streets-drive"},
    {MP "ss-net-breakable-car-fort.bin", 0x610, FLT, true, 0xdcde1e21, 0xa04, PLAYER, "shader param"},
    {MP "ss-oasis-events.bin", 0x170, INT, true, 0xbe00ea93, 0x450, MAIN, "ss-cargo-plane-fly"},
    {MP "ss-old-quater-events.bin", 0xf10, FLT, true, 0xdcde1e21, 0xf90, MAIN, "shader param"},
    {MP "ss-sweeping-building.bin", 0x140, FLT, true, 0xe2c35779, 0x320, MAIN, "animate speed"},
};

std::string sha16(const Bytes &d) { return sha256_hex(d).substr(0, 16); }

struct Ps3 {
    std::string job, disc, update, data2;
    std::unique_ptr<Ps3Index> idx;
    // the PS3 module of one version: <job>/p3/<ver>/<name> when there, else the player's PS3 archives
    std::optional<Bytes> module(const std::string &rel, const std::string &ver) {
        static const std::map<std::string, std::string> LAYER = {{"p18", "patch18"}, {"p13", "patch13"}, {"gotyp", "goty-patch"}, {"bin", "disc"}};
        const fs::path f = upath(job + "/p3/" + ver + "/" + rel.substr(rel.find_last_of('/') == std::string::npos ? 0 : rel.find_last_of('/') + 1));
        if (!job.empty() && fs::exists(f)) return read_file(f);
        if (!idx) idx = std::make_unique<Ps3Index>(disc, update, data2);
        return idx->read("dc1/ps4/" + rel, LAYER.at(ver));
    }
    // the defaults of (option sid, type) in each PS3 version, in version order: (version, values)
    std::vector<std::pair<std::string, std::vector<uint32_t>>> defaults(const std::string &rel, uint32_t sid, uint32_t type) {
        std::vector<std::pair<std::string, std::vector<uint32_t>>> vals;
        for (const char *ver : {"p18", "p13", "gotyp", "bin"}) {
            const std::optional<Bytes> m = module(rel, ver);
            if (!m) continue;
            const Bytes &d = *m;
            const uint32_t r = be32(d, 8), cnt = be32(d, r);
            const uint64_t bm = r + 4ull, bm_end = std::min<uint64_t>(bm + cnt, d.size());
            const uint8_t pat[8] = {uint8_t(sid >> 24), uint8_t(sid >> 16), uint8_t(sid >> 8), uint8_t(sid), uint8_t(type >> 24), uint8_t(type >> 16),
                                    uint8_t(type >> 8), uint8_t(type)};
            for (size_t k = 0;;) {
                auto it = std::search(d.begin() + std::ptrdiff_t(std::min(k, d.size())), d.end(), pat, pat + 8);
                if (it == d.end()) break;
                k = size_t(it - d.begin());
                const uint64_t po = k + 0xc, bi = po / 4;
                if (k % 4 == 0 && bm + bi / 8 < bm_end && (d[bm + bi / 8] & (1 << (7 - bi % 8)))) {
                    const uint32_t t = be32(d, po);
                    auto slot = std::find_if(vals.begin(), vals.end(), [&](const auto &v) { return v.first == ver; });
                    if (slot == vals.end()) slot = vals.insert(vals.end(), {ver, {}});
                    slot->second.push_back(be32(d, t));
                }
                k += 4;
            }
        }
        return vals;
    }
};

}  // namespace

int coop_audit(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--restore"}, {"--project", "--ps3-disc", "--ps3-update", "--ps3-data2", "--job"});
    const std::string p = a.need("--project") + "/";
    const std::string dc = p + "work/overlay-r3/u3data/build/main/dc1/ps4/", bk = p + "work/backup-coop-audit/overlay-r3-dc1/";
    Ps3 ps3{a.get("--job", ""), a.get("--ps3-disc", ""), a.get("--ps3-update", ""), a.get("--ps3-data2", ""), nullptr};
    std::vector<std::string> files;
    for (const Fix &f : FIXES)
        if (std::find(files.begin(), files.end(), f.file) == files.end()) files.push_back(f.file);
    std::sort(files.begin(), files.end());
    if (a.has("--restore")) {
        for (const std::string &f : files) {
            if (fs::exists(upath(bk + f))) {
                copy2(upath(bk + f), upath(dc + f));
                std::printf("restored %s %s\n", f.c_str(), sha16(read_file(upath(dc + f))).c_str());
            } else {
                std::printf("no backup %s\n", (bk + f).c_str());
            }
        }
        return 0;
    }
    int bad = 0;
    std::vector<std::pair<std::string, Bytes>> todo;
    for (const std::string &f : files) {
        const Bytes d = read_file(upath(dc + f));
        Bytes ba = d;
        int n = 0;
        std::printf("%s %zu bytes sha %s\n", f.c_str(), d.size(), sha16(d).c_str());
        if (rd32le(d, 0xc)) {
            std::printf("  header dword at 0xc not zero\n");
            bad++;
            continue;
        }
        for (const Fix &x : FIXES) {
            if (f != x.file) continue;
            const uint32_t s = rd32le(d, x.entry), t = rd32le(d, x.entry + 4);
            const uint64_t ptr = rd64le(d, x.entry + 0x10);
            if (t != x.type || (x.has_sid && s != x.sid)) {
                std::printf("  @%llx FAIL type/sid %08x %08x\n", (unsigned long long)x.entry, s, t);
                bad++;
                continue;
            }
            if (ptr == ZERO) {
                std::printf("  @%llx already fixed\n", (unsigned long long)x.entry);
                continue;
            }
            if (ptr != x.target || rd32le(d, ptr) != x.value) {
                std::printf("  @%llx FAIL ptr %llx value %08x\n", (unsigned long long)x.entry, (unsigned long long)ptr, rd32le(d, ptr));
                bad++;
                continue;
            }
            const auto v = ps3.defaults(f, s, x.type);
            std::string counts, nz;
            for (const auto &[ver, l] : v) {
                counts += (counts.empty() ? "'" : ", '") + ver + "': " + std::to_string(l.size());
                for (uint32_t val : l)
                    if (val) nz += (nz.empty() ? "('" : ", ('") + ver + "', " + std::to_string(val) + ")";
            }
            std::printf("  @%llx %08x %s -> %llx(%08x), PS3 defaults {%s} %s\n", (unsigned long long)x.entry, s, x.why, (unsigned long long)ptr, x.value,
                        counts.c_str(), nz.empty() ? "all 0" : ("NONZERO [" + nz + "]").c_str());
            if (!nz.empty() || v.empty()) bad++;
            wr64le(&ba[x.entry + 0x10], ZERO);
            n++;
        }
        if (n) todo.push_back({f, std::move(ba)});
    }
    if (bad) {
        std::printf("%d check(s) failed, nothing written\n", bad);
        return 1;
    }
    if (!a.has("--apply")) {
        std::printf("dry run ok: %zu file(s) would change\n", todo.size());
        return 0;
    }
    for (const auto &[f, nd] : todo) {
        const std::string b = bk + f;
        fs::create_directories(upath(b).parent_path());
        if (!fs::exists(upath(b))) {
            copy2(upath(dc + f), upath(b));
            std::printf("backup %s\n", b.c_str());
        }
        write_file_replace(upath(dc + f), nd);
        std::printf("patched %s %s %zu\n", f.c_str(), sha16(nd).c_str(), nd.size());
    }
    return 0;
}

}  // namespace u3
