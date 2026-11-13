// Recipe step airport-igc3: the Airport co-op IGC T-pose fix, in two commands.
//
//   u3fix igc3-retargets --ps4 D --job J
//     Reads the retail PS4 dc1/ps4/skel-retargets.bin and writes J/out/skel-retargets.bin with the retarget entry
//     npc-normal-base -> proto enabled (its first dword 1 -> 0), so an NPC on the hero / proto skeleton can play the
//     npc-normal clips (Airport IGC villains d and e). Everything else stays byte-identical to retail. The table is DC
//     symbol '*skel-retarget-tables*' (pointer to 26 entries of 0x30 bytes at 0x40; source / target skeleton sids at
//     +0xc / +0x10). Replaces make_retargets.py (same messages).
//
//   u3fix igc3-install --project P --job J [--apply] [--restore]
//     Installs J/out/skel-retargets.bin (it must have the known sha256) into overlay-r3 dc1/ps4. Dry run without
//     --apply; --apply backs the current overlay copy up once to work/backup-igc3/ (or notes there that there was none);
//     --restore puts the backup back, or removes the installed file if there was none before. Refuses while the game
//     runs. Replaces apply_igc3.py (same messages).
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {
namespace {

const char *const NAME = "skel-retargets.bin";
const char *const WANT = "1237caf2ab9c7b1a20660e6d952e63a28148fc94e69f750de15c8d6efc90459d";

std::string h(const std::string &f) { return sha256_hex(read_file(upath(f))); }

}  // namespace

int igc3_retargets(const std::vector<std::string> &argv) {
    const Args a(argv, {}, {"--ps4", "--job"});
    const std::string src = a.need("--ps4") + "/u3data/build/main/dc1/ps4/skel-retargets.bin", here = win_path(a.need("--job"));
    const Dc c(upath(src));
    Bytes d = c.d;
    auto it = c.bysid.find(sid("*skel-retarget-tables*"));
    if (it == c.bysid.end()) throw Stop("TypeError: no *skel-retarget-tables* in " + src);
    const uint64_t o = it->second.second, ptr = c.ptr(o);
    const uint32_t n = c.u32(o + 8);
    if (ptr != 0x40 || n != 26) throw Stop(fmt("AssertionError: ('%s', %u)", pyhex(ptr).c_str(), n));
    const uint32_t s = sid("npc-normal-base"), dn = sid("proto");
    std::string changed;
    int count = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t e = ptr + 0x30ull * i;
        if (c.u32(e + 0xc) == s && c.u32(e + 0x10) == dn) {
            const uint32_t old = rd32le(d, e);
            wr32le(&d[e], 0);
            changed += (changed.empty() ? "" : ", ") + fmt("(%u, 'npc-normal-base', 'proto', %u)", i, old);
            count++;
        }
    }
    if (count != 1) throw Stop("AssertionError: [" + changed + "]");
    const std::string out = here + "/out/skel-retargets.bin";
    fs::create_directories(upath(here + "/out"));
    write_file_replace(upath(out), d);
    Dc check(upath(out));   // must still parse
    const Bytes &orig = c.d;
    std::string diffs;
    size_t nd = 0;
    for (size_t k = 0; k < orig.size(); k++)
        if (orig[k] != d[k]) {
            diffs += (nd++ ? ", '" : "'") + pyhex(k) + "'";
        }
    std::printf("changed entries [%s]\n", changed.c_str());
    std::printf("bytes differing %zu [%s]\n", nd, diffs.c_str());
    std::printf("sha256 %s\n", sha256_hex(d).c_str());
    return 0;
}

int igc3_install(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--restore"}, {"--project", "--job"});
    const std::string work = win_path(a.need("--project")) + "\\work", here = win_path(a.need("--job"));
    const std::string dstdir = work + "/overlay-r3/u3data/build/main/dc1/ps4", bak = work + "/backup-igc3", srcdir = here + "/out";
    const bool apply = a.has("--apply"), restore = a.has("--restore");
    if ((apply || restore) && game_running(true)) throw Stop("Uncharted.exe is running: refusing to change data");
    const std::string n = NAME, s = srcdir + "/" + n, d = dstdir + "/" + n, b = bak + "/" + n, none = bak + "/" + n + ".absent";
    if (restore) {
        if (fs::exists(upath(b))) {
            std::printf("%s restore %s\n", n.c_str(), h(b).substr(0, 16).c_str());
            copy2(upath(b), upath(d));
            if (h(b) != h(d)) throw Stop("AssertionError: restored copy differs");
        } else if (fs::exists(upath(none))) {
            if (fs::exists(upath(d))) {
                fs::remove(upath(d));
                std::printf("%s removed (was not in overlay before)\n", n.c_str());
            }
        } else {
            throw Stop("no backup for " + n);
        }
        return 0;
    }
    if (h(s) != WANT) throw Stop("source hash mismatch " + s);
    const std::string cur = fs::exists(upath(d)) ? h(d).substr(0, 16) + " " + std::to_string(fs::file_size(upath(d))) : "not in overlay (retail used)";
    std::printf("%s current: %s -> new %s %llu\n", n.c_str(), cur.c_str(), std::string(WANT).substr(0, 16).c_str(),
                (unsigned long long)fs::file_size(upath(s)));
    if (!apply) {
        std::printf("  dry run: no change\n");
        return 0;
    }
    fs::create_directories(upath(bak));
    fs::create_directories(upath(dstdir));
    if (!fs::exists(upath(b)) && !fs::exists(upath(none))) {
        if (fs::exists(upath(d))) {
            copy2(upath(d), upath(b));
            std::printf("  backup %s\n", b.c_str());
        } else {
            const std::string t = "no overlay copy existed before apply_igc3\r\n";   // the script wrote it in text mode
            write_file_replace(upath(none), Bytes(t.begin(), t.end()));
            std::printf("  noted: no previous overlay copy\n");
        }
    }
    copy2(upath(s), upath(d));
    if (h(d) != WANT) throw Stop("AssertionError: installed copy differs");
    std::printf("  installed %s\n", d.c_str());
    return 0;
}

}  // namespace u3
