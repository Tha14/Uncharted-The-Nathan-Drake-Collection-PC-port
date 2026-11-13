// Recipe step enemy-stall-option-defaults: mis-converted state-script option defaults in overlay-r3
// dc1/ps4/ss-multiplayer/*.bin (the Syria co-op enemy stall).
//   u3fix enemy-stall --project P [--apply | --restore] [--only-tag-on-enter]
//
// In the PS3 -> PS4 conversion the last declared option of a state script lost its default-value slot: its value
// pointer (entry +0x10) points at the first word of the following track table (the track-name sid 'main', or 'player').
// ss-tag-on-enter.bin's (team int32 0) then reads 0x87f13bc2, the team test fails and the co-op region message is
// never sent. The fix points each of the nine entries at file offset 0xc, the upper half of the DC header's 64-bit
// relocation offset: always 0 and never written at run time, which is the PS3 default (0) of all nine. Size, symbols and
// relocation bitmap stay the same. Every entry is checked first (type, option sid, header word, old target and value);
// one failed check writes nothing. Dry run by default; --apply backs each file up once to work/backup-enemy-stall/;
// --restore copies the backups back. Replaces apply_ss_option_defaults.py (same messages).
#include "u3fix.hpp"

#include <algorithm>
#include <cstdio>

namespace u3 {
namespace {

constexpr uint32_t MAIN = 0x87f13bc2, PLAYER = 0xcde4b742;
constexpr uint64_t ZERO = 0xc;
struct Fix {
    const char *file;
    uint64_t entry;
    uint32_t type, sid;
    uint64_t target;
    uint32_t value;
};
const Fix FIXES[] = {
    {"ss-tag-on-enter.bin", 0x808, 0xc7cb275c, 0xd50bc34b, 0x970, MAIN},          // team int32 (every co-op region)
    {"ss-airport-bomb-drop.bin", 0xd610, 0xc7cb275c, 0xa0114308, 0xd710, MAIN},   // int32
    {"ss-coop-combat-tension.bin", 0x148, 0xc7cb275c, 0x2353ff86, 0x440, MAIN},   // int32
    {"ss-desert-events.bin", 0x1b0, 0x0f182ec3, 0xc3cc433c, 0x318, MAIN},         // float 'check'
    {"ss-facility-events.bin", 0x118, 0x0f182ec3, 0x3e50c397, 0x1b0, MAIN},       // float
    {"ss-facility-events.bin", 0x620, 0x0f182ec3, 0x3e50c397, 0x760, MAIN},       // float
    {"ss-heli-attack.bin", 0x5c78, 0x0f182ec3, 0xba9fbc94, 0x60d0, MAIN},         // float
    {"ss-net-breakable-car.bin", 0x7d0, 0x0f182ec3, 0xdcde1e21, 0xbc4, PLAYER},   // float
    {"ss-syria-events.bin", 0x5358, 0x0f182ec3, 0xdcde1e21, 0x53d8, MAIN},        // float
};

std::string sha16(const Bytes &d) { return sha256_hex(d).substr(0, 16); }

// ok, message, already fixed
struct Check {
    bool ok;
    std::string msg;
    bool done;
};
Check check(const Bytes &d, const Fix &f) {
    if (f.entry + 0x18 > d.size()) return {false, "entry beyond file", false};
    const uint32_t s = rd32le(d, f.entry), t = rd32le(d, f.entry + 4);
    const uint64_t p = rd64le(d, f.entry + 0x10);
    if (t != f.type) return {false, fmt("type %08x != %08x", t, f.type), false};
    if (s != f.sid) return {false, fmt("sid %08x != %08x", s, f.sid), false};
    if (rd32le(d, 0xc)) return {false, "header dword at 0xc not zero", false};
    if (p == ZERO) return {true, "already fixed (ptr -> 0xc)", true};
    if (p != f.target) return {false, fmt("ptr %llx != expected %llx", (unsigned long long)p, (unsigned long long)f.target), false};
    const uint32_t v = rd32le(d, p);
    if (v != f.value) return {false, fmt("value at %llx is %08x, expected %08x", (unsigned long long)p, v, f.value), false};
    return {true, fmt("ptr %llx -> %llx (%08x), will point to %llx (0)", (unsigned long long)(f.entry + 0x10), (unsigned long long)p, v, (unsigned long long)ZERO),
            false};
}

}  // namespace

int enemy_stall(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--restore", "--only-tag-on-enter"}, {"--project"});
    const std::string p = a.need("--project") + "/";
    const std::string dc = p + "work/overlay-r3/u3data/build/main/dc1/ps4/ss-multiplayer/", bk = p + "work/backup-enemy-stall/";
    const bool only = a.has("--only-tag-on-enter");
    std::vector<const Fix *> fixes;
    std::vector<std::string> files;   // sorted, each once
    for (const Fix &f : FIXES) {
        if (only && std::string(f.file) != "ss-tag-on-enter.bin") continue;
        fixes.push_back(&f);
        bool seen = false;
        for (const std::string &x : files) seen = seen || x == f.file;
        if (!seen) files.push_back(f.file);
    }
    std::sort(files.begin(), files.end());
    if (a.has("--restore")) {   // --restore wins over --apply, as in the script
        for (const std::string &f : files) {
            if (!fs::exists(upath(bk + f))) {
                std::printf("no backup %s\n", (bk + f).c_str());
                continue;
            }
            copy2(upath(bk + f), upath(dc + f));
            std::printf("restored %s %s\n", f.c_str(), sha16(read_file(upath(dc + f))).c_str());
        }
        return 0;
    }
    int bad = 0;
    std::vector<std::pair<std::string, Bytes>> todo;
    for (const std::string &f : files) {
        const Bytes d = read_file(upath(dc + f));
        std::printf("%s %zu bytes sha %s\n", f.c_str(), d.size(), sha16(d).c_str());
        Bytes nd = d;
        int n = 0;
        for (const Fix *x : fixes) {
            if (f != x->file) continue;
            const Check c = check(d, *x);
            std::printf("  @%llx %s: %s\n", (unsigned long long)x->entry, c.ok ? "OK" : "FAIL", c.msg.c_str());
            if (!c.ok) bad++;
            else if (!c.done) {
                wr64le(&nd[x->entry + 0x10], ZERO);
                n++;
            }
        }
        if (n) todo.push_back({f, std::move(nd)});
    }
    if (bad) {
        std::printf("%d check(s) failed, nothing written\n", bad);
        return 1;
    }
    if (!a.has("--apply")) {
        std::string names;
        for (const auto &t : todo) names += (names.empty() ? "" : ", ") + t.first;
        std::printf("dry run: %zu file(s) would change: %s\n", todo.size(), names.c_str());
        return 0;
    }
    fs::create_directories(upath(bk));
    for (const auto &[f, nd] : todo) {
        if (!fs::exists(upath(bk + f))) {
            copy2(upath(dc + f), upath(bk + f));
            std::printf("backup %s\n", (bk + f).c_str());
        }
        write_file_replace(upath(dc + f), nd);
        const Bytes d = read_file(upath(dc + f));   // check what is on disk
        for (const Fix *x : fixes)
            if (f == x->file && (rd64le(d, x->entry + 0x10) != ZERO || rd32le(d, ZERO) != 0)) throw Stop("AssertionError: " + f + " check after writing");
        std::printf("patched %s sha %s size %zu\n", f.c_str(), sha16(d).c_str(), d.size());
    }
    return 0;
}

}  // namespace u3
