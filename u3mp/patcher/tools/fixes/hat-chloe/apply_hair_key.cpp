// Recipe step hair-key: the PS3 hair match keys in overlay-r3 net.bin.
//   u3fix hair-key --project P [--also-hair-kind] [--apply] [net.bin]
//
// The game's part lookup finds a hero's hair as the first body-list entry with kind (+0x18) 7 and key (+0x20) equal to
// the accessory entry's key (none = 0x5e74ad64). Retail PS4 has no accessory slot on these heroes, so key 0 and its hair
// entries carry 0; the rebuilt heroes have the slot back but not the key, so without a hat the hero is bald. Fixed:
// mp-chloe-u2-hair (entry sid 0x61465a13) in body lists 8f89357a (chloe-u2) and d22cbb8d (chloe-u2-leather), kind 7 key 0
// -> kind 7 key 0x5e74ad64. --also-hair-kind: the rescuer and dead-agent hair too (kind 5 key 0 -> 7 / 0x5e74ad64). Each
// list (DC type 0x2a8027cf: count, pointer to 0x38-byte entries, entry sid at +0x10) must have exactly one such entry in
// the old or the new state. Dry run without --apply; --apply backs net.bin up once to net.bin.bak-hairkey. Same size,
// no pointer moves. Replaces apply_hair_key.py (same messages).
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {

int hair_key(const std::vector<std::string> &argv) {
    struct Fix {
        uint32_t list, entry, old_kind, old_key, kind, key;
    };
    static const Fix FIX[] = {{0x8f89357a, 0x61465a13, 7, 0, 7, 0x5e74ad64}, {0xd22cbb8d, 0x61465a13, 7, 0, 7, 0x5e74ad64}};
    static const Fix OPT[] = {{0x4a7d3938, 0x34521b74, 5, 0, 7, 0x5e74ad64}, {0xc9d6ac4b, 0x9376a2f1, 5, 0, 7, 0x5e74ad64}};
    std::vector<std::string> opts, files;
    for (const std::string &x : argv) (x.rfind("--", 0) == 0 ? opts : files).push_back(x);
    // --project takes a value: put it back together
    std::vector<std::string> args;
    for (size_t i = 0; i < argv.size(); i++) {
        if (argv[i] == "--project" && i + 1 < argv.size()) {
            args.push_back(argv[i]);
            args.push_back(argv[++i]);
        } else if (argv[i].rfind("--", 0) == 0) args.push_back(argv[i]);
    }
    files.clear();
    for (size_t i = 0; i < argv.size(); i++) {
        if (argv[i] == "--project") { i++; continue; }
        if (argv[i].rfind("--", 0) != 0) files.push_back(argv[i]);
    }
    const Args a(args, {"--apply", "--also-hair-kind"}, {"--project"});
    const std::string path = !files.empty() ? files[0] : a.need("--project") + "/work/overlay-r3/u3data/build/main/dc1/ps4/net.bin";
    const Dc c(upath(path));
    Bytes data = read_file(upath(path));
    std::vector<Fix> fixes(std::begin(FIX), std::end(FIX));
    if (a.has("--also-hair-kind")) fixes.insert(fixes.end(), std::begin(OPT), std::end(OPT));
    struct Todo {
        uint64_t e;
        uint32_t kind, key;
    };
    std::vector<Todo> todo;
    for (const Fix &f : fixes) {
        auto it = c.bysid.find(f.list);
        if (it == c.bysid.end()) throw Stop(fmt("KeyError: list %08x not in %s", f.list, path.c_str()));
        if (it->second.first != 0x2a8027cf) throw Stop("AssertionError: " + pyhex(it->second.first));
        const uint64_t o = it->second.second, q = c.ptr(o + 8);
        const uint32_t n = c.u32(o);
        std::vector<uint64_t> hits;
        for (uint32_t j = 0; j < n; j++)
            if (c.u32(q + uint64_t(j) * 0x38 + 0x10) == f.entry) hits.push_back(q + uint64_t(j) * 0x38);
        if (hits.size() != 1) throw Stop(fmt("AssertionError: list %08x has %zu matching entries", f.list, hits.size()));
        const uint64_t e = hits[0];
        const uint32_t kind = rd32le(data, e + 0x18), key = rd32le(data, e + 0x20);
        const auto art = c.cstr(c.ptr(e + 8));
        const std::string name = art ? *art : "None";
        if (kind == f.kind && key == f.key) {
            std::printf("%08x %s already fixed\n", f.list, name.c_str());
            continue;
        }
        if (kind != f.old_kind || key != f.old_key)
            throw Stop(fmt("AssertionError: ('%s', '%s', %u, '%s')", pyhex(f.list).c_str(), name.c_str(), kind, pyhex(key).c_str()));
        std::printf("%08x %-26s entry %s: kind %u -> %u (+0x18), key %s -> %s (+0x20)\n", f.list, name.c_str(), pyhex(e).c_str(), kind, f.kind,
                    pyhex(key).c_str(), pyhex(f.key).c_str());
        todo.push_back({e, f.kind, f.key});
    }
    if (!a.has("--apply")) {
        std::printf("dry run: %zu entries would change in %s; nothing written\n", todo.size(), path.c_str());
        return 0;
    }
    if (!todo.empty()) {
        const std::string bak = path + ".bak-hairkey";
        if (!fs::exists(upath(bak))) {
            copy2(upath(path), upath(bak));
            std::printf("backup %s\n", bak.c_str());
        }
        for (const Todo &t : todo) {
            wr32le(&data[t.e + 0x18], t.kind);
            wr32le(&data[t.e + 0x20], t.key);
        }
        write_file_replace(upath(path), data);
        std::printf("written; same size, no pointers moved\n");
    }
    return 0;
}

}  // namespace u3
