// u2mp spawn: where the multiplayer players spawn (u2mp/docs/PORT-SPAWN.md).
//
// GetSpawnLocator 0x662fa0 (PS3 0x4c8568, net-player-tracker.cpp) is bool (Locator *out, u32 spawner
// id, int mode). It asks 0x644130 (PS3 0x49cee8) for the package of the net game's level record (the
// "<map>-pak" package, e.g. gompa-multi-pak) and binary-searches that package's spawner group
// (+0x9a0; PS3 +0x4e4 via 0xb6e10) for the id. Missing -> "Missing spawner '%s' trying to spawn
// player" and int 0x41 (0x6630dc).
//
// The id is a spawner sid chosen by the initial spawn 0x644220 (PS3 0x4a1eb0) / respawn 0x642d20
// (PS3 0x4a6008): spawners of the "<map>-pak" package tagged online-spawner (5bfae217) = 1, team
// (d50bc34b) = the tracker's team (+0x134) or -1, start-spawner (33364361) at match start. Both inline
// the same "<map>-pak" lookup and return 0 when that package is not loaded.
//
// The "<map>-pak" packages ("<map>-pak-ingame", file <map>-pak-ingame10.pak: the online-spawner-*,
// net-flag*, net-territory-*, ss-battle-manager-* spawners of each map) are not part of the PS3 disc
// or of PS4: PS3 downloaded them from the server (net-patch9.bin maps "<map>-pak" to the package
// "1-<map>-pak-ingame"; the files sit in the PS3 game data BCES00757DATA2/USRDIR/build/main/pak22/
// 1-<map>-pak-ingame10.pak). Only stage-multi-pak-ingame10.pak ships on PS4 (not in pak22_ps4.txt).
// The map packages ("<map>", "<map>-ingame") hold no online spawners at all. The real fix is the
// import (PORT-SPAWN.md); this file makes the lookup never assert:
//
// 0x662fd0 (call 0x644130, the only caller) -> spawn_package(ngm, id): a package whose spawner group
// holds the id ("<map>-pak", then the map, then any loaded package). When none has it, a package of
// our own whose group holds one entry {id, fallback spawner}, the fallback being, in order, an
// online-spawner of the "<map>-pak" package, of any loaded package, a pickup / npc spawner of the map,
// any spawner of the map, or a spawner of our own at the origin. The guest then computes the locator
// from that spawner (0x4bdfe0) as usual.
#include <cstdint>
#include <cstring>
#include <string>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kPackageTable = 0x1130390, kNgm = 0x113b760, kNgmGame = 0x1ca0, kDcLookup = 0xa15e70;
constexpr uint64_t kGetSpawnLocatorCall = 0x662fd0, kLevelRecordPackage = 0x644130;
constexpr uint32_t kNetGames = 0x3b9a067d, kLevels = 0x0dec97a4;
// spawner tags (sid of the names); team d50bc34b is not used here: GetSpawnLocator has no tracker
constexpr uint32_t kTagOnlineSpawner = 0x5bfae217, kTagStartSpawner = 0x33364361;
constexpr size_t kGroup = 0x9a0;   // package -> spawner group (PS3 level +0x4e4)

// PS4 SPAWNER_GROUP (u3mp paktools ps4ingame.py): {u64 0, Spawner **all, u32 n, pad, Entry *sorted, u32 n}
struct Entry {
    uint32_t id;
    uint32_t pad;
    const uint8_t *spawner;
};
struct Group {
    uint64_t zero;
    const uint8_t *const *all;
    int32_t count;
    uint32_t pad;
    const Entry *sorted;
    int32_t sorted_count;
    uint32_t pad2;
};
static_assert(offsetof(Group, all) == 8 && offsetof(Group, count) == 0x10 && offsetof(Group, sorted) == 0x18 &&
                  offsetof(Group, sorted_count) == 0x20, "spawner group layout (0x662fe5, 0x6443a8)");
// PS4 spawner: pos +0, quat +0x10, name +0x20, sid +0x28, parent +0x60 (0x4bdfe0), tags +0x68
// ({Tag *entries, u32 n}, 0x6443ee), tag {u32 key, pad, Value *}, value {u16 type, u16 size, pad, data *}.
constexpr size_t kSpawnerName = 0x20, kSpawnerId = 0x28, kSpawnerTags = 0x68, kSpawnerSize = 0x90;
constexpr int32_t kMaxCount = 100000;

const Group *group_of(const uint8_t *pkg) {
    return pkg ? *reinterpret_cast<const Group *const *>(pkg + kGroup) : nullptr;
}

const uint8_t *find_in(const uint8_t *pkg, uint32_t id) {
    const Group *g = group_of(pkg);
    if (!g || !g->sorted || g->sorted_count <= 0 || g->sorted_count > kMaxCount) return nullptr;
    int32_t lo = 0, hi = g->sorted_count;   // the guest's own search (0x662fee): sorted by id
    while (lo < hi) {
        const int32_t mid = lo + (hi - lo) / 2;
        if (g->sorted[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < g->sorted_count && g->sorted[lo].id == id ? g->sorted[lo].spawner : nullptr;
}

// An int tag of a spawner (type 1), `fallback` when absent.
int32_t int_tag(const uint8_t *sp, uint32_t key, int32_t fallback) {
    struct Value { uint16_t type, size; uint32_t pad; const int32_t *data; };
    struct Tag { uint32_t key, pad; const Value *value; };
    struct Tags { const Tag *entries; int32_t count; };
    const Tags *t = *reinterpret_cast<const Tags *const *>(sp + kSpawnerTags);
    if (!t || !t->entries || t->count <= 0 || t->count > 4096) return fallback;
    for (int32_t i = 0; i < t->count; ++i) {
        const Tag &e = t->entries[i];
        if (e.key != key) continue;
        if (!e.value || e.value->type != 1 || !e.value->data) return fallback;
        return *e.value->data;
    }
    return fallback;
}

const char *spawner_name(const uint8_t *sp) {
    const char *n = *reinterpret_cast<const char *const *>(sp + kSpawnerName);
    return n ? n : "?";
}

// Every package in the table (buckets +0x10, bucket count +0x4c, node {next, ?, u32 id +0x10,
// package +0x18}, buckets[count] = end node; as session.cpp find_package / 0x6441b5).
template <typename F> void for_each_package(F &&f) {
    const uint64_t buckets = *image.at<uint64_t>(kPackageTable + 0x10);
    const int32_t count = *image.at<int32_t>(kPackageTable + 0x4c);
    if (!buckets || count <= 0 || count > 1 << 20) return;
    const auto *table = reinterpret_cast<const uint64_t *>(uintptr_t(buckets));
    const uint64_t end = table[count];
    for (int32_t b = 0; b < count; ++b) {
        int guard = 0;
        for (uint64_t node = table[b]; node && node != end && guard < 4096; ++guard) {
            const auto *n = reinterpret_cast<const uint8_t *>(uintptr_t(node));
            if (const uint8_t *pkg = *reinterpret_cast<const uint8_t *const *>(n + 0x18))
                if (f(*reinterpret_cast<const uint32_t *>(n + 0x10), pkg)) return;
            node = *reinterpret_cast<const uint64_t *>(n);
        }
    }
}

// The net game's level record name (*levels* +0x18, "<map>-pak") and its package id (+0x20), as
// 0x644130 / 0x6442b8 read them.
bool level_record(const char *&name, uint32_t &id) {
    struct Table { int32_t count; int32_t pad; uint8_t *records; };
    auto lookup = reinterpret_cast<Table *(U2MP_GUEST *)(uint32_t)>(image.base + kDcLookup);
    Table *games = lookup(kNetGames), *levels = lookup(kLevels);
    const int16_t game = *image.at<int16_t>(kNgm + kNgmGame);
    if (!games || !levels || !games->records || !levels->records || game < 0 || game >= games->count) return false;
    const uint32_t level_id = *reinterpret_cast<const uint32_t *>(games->records + 0x90 * game + 4);
    for (int32_t i = 0; i < levels->count; ++i) {
        const uint8_t *rec = levels->records + 0x38 * i;
        if (*reinterpret_cast<const uint32_t *>(rec) != level_id) continue;
        name = *reinterpret_cast<const char *const *>(rec + 0x18);
        id = *reinterpret_cast<const uint32_t *>(rec + 0x20);
        return name != nullptr;
    }
    return false;
}

// Online spawners of a package (tag online-spawner = 1); start spawners first when `start`.
const uint8_t *pick_online(const uint8_t *pkg, unsigned turn, bool start) {
    const Group *g = group_of(pkg);
    if (!g || !g->all || g->count <= 0 || g->count > kMaxCount) return nullptr;
    for (int pass = start ? 0 : 1; pass < 2; ++pass) {
        int32_t n = 0;
        for (int32_t i = 0; i < g->count; ++i)
            if (const uint8_t *sp = g->all[i])
                if (int_tag(sp, kTagOnlineSpawner, 0) == 1 && (pass || int_tag(sp, kTagStartSpawner, 0) == 1)) ++n;
        if (!n) continue;
        int32_t want = int32_t(turn % unsigned(n));
        for (int32_t i = 0; i < g->count; ++i)
            if (const uint8_t *sp = g->all[i])
                if (int_tag(sp, kTagOnlineSpawner, 0) == 1 && (pass || int_tag(sp, kTagStartSpawner, 0) == 1) &&
                    want-- == 0)
                    return sp;
    }
    return nullptr;
}

// A spawner of the map whose name starts with one of the prefixes (pickups and npc spawners stand on
// walkable ground), else the first spawner.
const uint8_t *pick_any(const uint8_t *pkg, unsigned turn) {
    const Group *g = group_of(pkg);
    if (!g || !g->all || g->count <= 0 || g->count > kMaxCount) return nullptr;
    for (const char *prefix : {"pickup-", "npc-", "online-", "continue-player"}) {
        const size_t len = std::strlen(prefix);
        int32_t n = 0;
        for (int32_t i = 0; i < g->count; ++i)
            if (g->all[i] && std::strncmp(spawner_name(g->all[i]), prefix, len) == 0) ++n;
        if (!n) continue;
        int32_t want = int32_t(turn % unsigned(n));
        for (int32_t i = 0; i < g->count; ++i)
            if (g->all[i] && std::strncmp(spawner_name(g->all[i]), prefix, len) == 0 && want-- == 0) return g->all[i];
    }
    return g->all[0];
}

// Our own package: only +0x9a0 is read (0x662fd5), pointing at a group with one entry.
alignas(16) uint8_t own_package[kGroup + 0x10];
alignas(16) Group own_group;
alignas(16) Entry own_entry;
// A spawner of our own at the origin (identity rotation, no parent, no tags), the last resort.
alignas(16) uint8_t own_spawner[kSpawnerSize];
const char own_spawner_name[] = "u2mp-origin";

const uint8_t *own_package_with(uint32_t id, const uint8_t *spawner) {
    own_entry = Entry{id, 0, spawner};
    own_group = Group{};
    own_group.sorted = &own_entry;
    own_group.sorted_count = 1;
    const Group *g = &own_group;
    std::memcpy(own_package + kGroup, &g, sizeof g);
    return own_package;
}

const uint8_t *origin_spawner() {
    std::memset(own_spawner, 0, sizeof own_spawner);
    const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    std::memcpy(own_spawner + 0x10, quat, sizeof quat);
    const char *name = own_spawner_name;
    std::memcpy(own_spawner + kSpawnerName, &name, sizeof name);
    return own_spawner;
}

// Replaces the call 0x662fd0 (rdi = NetGameManager, esi = spawner id, edx = mode still hold
// GetSpawnLocator's arguments there: 0x662fb1..0x662fc9 only copy them).
U2MP_GUEST const uint8_t *spawn_package(void *, uint32_t id, int32_t mode) {
    static unsigned turn = 0;
    static uint32_t said_id = ~0u;
    static std::string said_level;
    const char *name = nullptr;
    uint32_t pak_id = 0;
    const bool have_record = level_record(name, pak_id);
    const uint8_t *pak = have_record ? package_by_id(pak_id) : nullptr;
    std::string map;
    if (have_record) {
        const size_t n = std::strlen(name);
        map = n > 4 && std::strcmp(name + n - 4, "-pak") == 0 ? std::string(name, n - 4) : std::string(name);
    }
    const uint8_t *map_pkg = map.empty() ? nullptr : package_by_id(sid(map.c_str()));
    if (have_record && said_level != name) {
        said_level = name;
        const Group *pg = group_of(pak), *mg = group_of(map_pkg);
        log("spawn: level %s %s (%d spawners), map %s %s (%d spawners)", name, pak ? "loaded" : "missing",
            pg ? pg->count : 0, map.c_str(), map_pkg ? "loaded" : "missing", mg ? mg->count : 0);
        if (!pak)
            log("spawn: %s holds the multiplayer spawners (online-spawner-*); it is not installed, players "
                "spawn at fallback spawners", name);
    }
    // 1. The id where the game looks (0x644130's package), then the map, then any loaded package.
    if (pak && find_in(pak, id)) return pak;
    if (map_pkg && find_in(map_pkg, id)) return map_pkg;
    const uint8_t *found = nullptr;
    for_each_package([&](uint32_t, const uint8_t *pkg) {
        if (find_in(pkg, id)) found = pkg;
        return found != nullptr;
    });
    if (found) return found;
    // 2. Fallback spawner (never the assert at 0x6630dc).
    const unsigned t = turn++;
    const bool start = mode == 0;
    const char *how = "online spawner of the level";
    const uint8_t *sp = pick_online(pak, t, start);
    if (!sp) {
        how = "online spawner of a loaded package";
        for_each_package([&](uint32_t, const uint8_t *pkg) {
            sp = pick_online(pkg, t, start);
            return sp != nullptr;
        });
    }
    if (!sp) {
        how = "map spawner";
        sp = pick_any(map_pkg, t);
    }
    if (!sp) {
        how = "own spawner at the origin";
        sp = origin_spawner();
    }
    if (said_id != id) {
        said_id = id;
        log("spawn: spawner %08x (mode %d) is in no loaded package; using %s %s (%08x)", id, mode, how,
            spawner_name(sp), *reinterpret_cast<const uint32_t *>(sp + kSpawnerId));
    }
    return own_package_with(id, sp);
}

}  // namespace

void install_spawn(int &installed) {
    // 0x662fc9: lea rdi,[rip+..] (NetGameManager 0x113b760); 0x662fd0: call 0x644130;
    // 0x662fd5: mov r9,[rax+0x9a0]
    static const unsigned char before[] = {0x48, 0x8d, 0x3d};
    static const unsigned char after[] = {0x4c, 0x8b, 0x88, 0xa0, 0x09, 0x00, 0x00};
    int32_t rel = 0;
    std::memcpy(&rel, image.base + 0x662fc9 + 3, 4);
    if (!image.matches(0x662fc9, before, sizeof before) || 0x662fd0 + int64_t(rel) != int64_t(kNgm) ||
        !image.matches(0x662fd5, after, sizeof after)) {
        log("spawn: GetSpawnLocator (662fa0) bytes differ, left alone");
        return;
    }
    if (retarget_call(kGetSpawnLocatorCall, kLevelRecordPackage, reinterpret_cast<void *>(&spawn_package),
                      "GetSpawnLocator spawner package (662fd0)")) {
        log("spawn: GetSpawnLocator (662fa0) looks the spawner up in every loaded package, with a fallback");
        ++installed;
    }
}

}  // namespace u2mp
