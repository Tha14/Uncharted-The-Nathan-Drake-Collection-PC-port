// Recipe step shade-npc-paks: the Shade Arena NPC spawner templates of Fort, Gompa, Shambhala, Village and Warzone.
// Their <map>-multi-shade-npc-ingame12.pak has 17 or 21 spawners, PS3 1.19 has 43 on all of them; the other shade
// maps' PS4 files are byte-identical copies of one 43-spawner pak without a map name inside (overlay-r2
// yemen-multi-shade-npc-ingame12.pak, md5 b397ee3b...), copied over each of the five effective files. Replaces
// apply_shade_npc_paks.py (its PS3 spawner comparison needs audit helpers the recipe does not ship and was always
// skipped there).
//   u3fix shade-npc --project P [--apply | --restore]
// Every target must be the effective file (the highest overlay that has it) in its expected state; a Game of the Year
// disc's gompa copy (goty-dlc, already 43 spawners) is accepted. --apply backs each target up once to
// work/backup-coop-audit/<overlay>/ and copies; --restore puts the backups back.
#include "u3fix.hpp"

namespace u3 {

int shade_npc(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply", "--restore"}, {"--project"});
    const std::string P = a.need("--project") + "/", BK = P + "work/backup-coop-audit/", MAIN = "/u3data/build/main/pak23/ps4/";
    const std::string SRC = P + "work/overlay-r2" + MAIN + "yemen-multi-shade-npc-ingame12.pak";
    const uint64_t SRC_SIZE = 110968;
    const std::string SRC_MD5 = "b397ee3b";
    struct T {
        std::string map, overlay;
        uint64_t size;
        std::string md5;
    };
    const std::vector<T> TG = {{"fort", "overlay-r2", 40632, "5016bd65"},
                               {"gompa", "overlay-port", 40632, "5016bd65"},
                               {"shambhala", "overlay-port", 40632, "5016bd65"},
                               {"village", "overlay-port", 52968, "48637de9"},
                               {"warzone", "overlay-port", 52968, "48637de9"}};
    auto md5f = [](const std::string &f) { return md5_hex(read_file(upath(f))); };
    auto tpath = [&](const T &t) { return P + "work/" + t.overlay + MAIN + t.map + "-multi-shade-npc-ingame12.pak"; };
    auto shadow = [&](const T &t) -> std::string {   // the overlay that holds the effective file
        for (const char *o : {"overlay-r3", "overlay-r2", "overlay-port", "overlay-mp"})
            if (fs::exists(upath(P + "work/" + o + MAIN + t.map + "-multi-shade-npc-ingame12.pak"))) return o;
        return "None";
    };
    auto base = [](const std::string &f) { return f.substr(f.find_last_of('/') + 1); };
    if (a.has("--restore")) {
        for (const T &t : TG) {
            const std::string b = BK + t.overlay + "/" + base(tpath(t));
            if (fs::exists(upath(b))) {
                copy2(upath(b), upath(tpath(t)));
                std::printf("restored %s %s\n", t.map.c_str(), md5f(tpath(t)).substr(0, 8).c_str());
            } else
                std::printf("no backup %s\n", b.c_str());
        }
        return 0;
    }
    int bad = 0;
    if (!fs::exists(upath(SRC)) || fs::file_size(upath(SRC)) != SRC_SIZE || md5f(SRC).rfind(SRC_MD5, 0) != 0) {
        std::printf("source changed: %s\n", SRC.c_str());
        bad++;
    }
    for (const T &t : TG) {
        const std::string f = tpath(t);
        if (shadow(t) != t.overlay) {
            std::printf("%s FAIL effective overlay is %s expected %s\n", t.map.c_str(), shadow(t).c_str(), t.overlay.c_str());
            bad++;
            continue;
        }
        const std::string cur = md5f(f);
        if (cur.rfind(SRC_MD5, 0) == 0) {
            std::printf("%s already fixed\n", t.map.c_str());
            continue;
        }
        const uint64_t sz = fs::file_size(upath(f));
        // a Game of the Year disc without the DLC packs: its gompa copy (goty-dlc.psarc) already has the 43 spawners
        const bool goty = t.map == "gompa" && sz == 110968 && cur.substr(0, 8) == "a2b322e2";
        if (!((sz == t.size && cur.substr(0, 8) == t.md5) || goty)) {
            std::printf("%s FAIL unexpected file %llu %s\n", t.map.c_str(), (unsigned long long)sz, cur.substr(0, 8).c_str());
            bad++;
            continue;
        }
        std::printf("%s: %s %llu bytes md5 %s -> %llu bytes (PS3 spawner check skipped: audit helpers not available)\n", t.map.c_str(), t.overlay.c_str(),
                    (unsigned long long)t.size, t.md5.c_str(), (unsigned long long)SRC_SIZE);
    }
    if (bad) {
        std::printf("%d check(s) failed, nothing written\n", bad);
        return 1;
    }
    if (!a.has("--apply")) {
        std::printf("dry run ok\n");
        return 0;
    }
    for (const T &t : TG) {
        const std::string f = tpath(t);
        if (md5f(f).rfind(SRC_MD5, 0) == 0) continue;
        const std::string b = BK + t.overlay + "/" + base(f);
        fs::create_directories(upath(b).parent_path());
        if (!fs::exists(upath(b))) {
            copy2(upath(f), upath(b));
            std::printf("backup %s\n", b.c_str());
        }
        write_file_replace(upath(f), read_file(upath(SRC)));
        if (md5f(f) != md5f(SRC)) throw Stop("AssertionError: copy of " + f);
        std::printf("copied %s %s\n", t.map.c_str(), f.c_str());
    }
    return 0;
}

}  // namespace u3
