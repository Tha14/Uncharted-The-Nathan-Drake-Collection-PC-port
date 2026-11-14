// u2mp spawn: lets GetSpawnLocator find a spawner even when the level's spawner package is missing.
//
// GetSpawnLocator (image 0x662fa0) asks for the package named by the net game's level record
// ("<map>-pak") and looks the wanted spawner id up in that package's spawner group. Those packages
// are often not installed, and the lookup then asserts. We take over that one call: it returns the
// first package whose spawner group really holds the id (the level package, the map package, then
// every loaded package). When none does, we pick a sensible spawner ourselves (an online spawner, a
// map spawner, or one at the origin) and return a package of our own whose group holds exactly
// {id -> that spawner}. The game then builds the locator from it as usual.
//
// Reimplemented from the behaviour description u2mp/docs/spec/gameplay.md.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "u2mp_internal.h"

namespace u2mp {
void install_spawn(int &installed);

namespace {

constexpr uint64_t kCallSite = 0x662fd0;
constexpr uint64_t kLevelPackage = 0x644130;    // the call's original target
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kGameIndex = kNgm + 0x1ca0;
constexpr uint64_t kPackageTable = 0x1130390;
constexpr uint64_t kLookupPointer = 0xa15e70;

constexpr uint32_t kNetGames = sid("*net-games*");
constexpr uint32_t kLevels = 0x0dec97a4;    // id of the *levels* table (not the plain CRC of its name)
constexpr uint32_t kOnlineSpawner = sid("online-spawner");
constexpr uint32_t kStartSpawner = sid("start-spawner");
static_assert(kNetGames == 0x3b9a067du, "net-games id");
static_assert(kOnlineSpawner == 0x5bfae217u && kStartSpawner == 0x33364361u, "tag keys");

constexpr uint64_t kGroupOfPackage = 0x9a0;
constexpr int32_t kMaxGroupCount = 100000;
constexpr int32_t kMaxTagCount = 4096;
constexpr int32_t kMaxBuckets = 1 << 20;
constexpr int kMaxNodesPerBucket = 4096;

template <typename T> T peek(const unsigned char *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
const unsigned char *pointer_at(const unsigned char *p) {
    return reinterpret_cast<const unsigned char *>(peek<uint64_t>(p));
}
template <typename T> void poke(unsigned char *p, T v) { std::memcpy(p, &v, sizeof v); }

// Our stand-in package: only its spawner group pointer (+0x9a0) is ever set.
alignas(16) unsigned char own_package[0x9b0];
alignas(16) unsigned char own_group[0x28];
alignas(16) unsigned char own_entry[16];
alignas(16) unsigned char origin_spawner[0x90];
const char kOriginName[] = "u2mp-origin";

uint32_t turn = 0;                 // rotates fallback picks so players do not all stack on one spawner
uint32_t last_logged_id = 0xffffffffu;
std::string last_logged_level;

// The spawner with `id` in the package's sorted spawner list, or NULL.
const unsigned char *find_in_package(const unsigned char *package, uint32_t id) {
    const unsigned char *group = pointer_at(package + kGroupOfPackage);
    if (!group) return nullptr;
    const unsigned char *entries = pointer_at(group + 0x18);
    const int32_t count = peek<int32_t>(group + 0x20);
    if (!entries || count <= 0 || count > kMaxGroupCount) return nullptr;
    // Lower bound: the first entry whose id is not below the wanted id.
    size_t low = 0, high = size_t(count);
    while (low < high) {
        const size_t mid = low + (high - low) / 2;
        if (peek<uint32_t>(entries + 16 * mid) < id) low = mid + 1;
        else high = mid;
    }
    if (low == size_t(count) || peek<uint32_t>(entries + 16 * low) != id) return nullptr;
    return pointer_at(entries + 16 * low + 8);
}

// Calls `test(package)` for every loaded package until it returns non-NULL; returns that result.
template <typename Test> const unsigned char *walk_packages(Test test) {
    const unsigned char *table = image.at<unsigned char>(kPackageTable);
    const unsigned char *buckets = pointer_at(table + 0x10);
    const int32_t count = peek<int32_t>(table + 0x4c);
    if (!buckets || count <= 0 || count > kMaxBuckets) return nullptr;
    const unsigned char *end_marker = pointer_at(buckets + 8 * uint64_t(count));
    for (int32_t b = 0; b < count; ++b) {
        const unsigned char *node = pointer_at(buckets + 8 * uint64_t(b));
        for (int visited = 0; node && node != end_marker && visited < kMaxNodesPerBucket; ++visited) {
            const unsigned char *package = pointer_at(node + 0x18);
            if (package) {
                if (const unsigned char *found = test(package)) return found;
            }
            node = pointer_at(node);
        }
    }
    return nullptr;
}

struct LevelRecord {
    const char *name;
    uint32_t package_id;
};

using LookupFn = const unsigned char *(U2MP_GUEST *)(uint32_t);

// The *levels* record of the current net game's level.
bool level_record(LevelRecord &out) {
    const LookupFn lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *net_games = lookup(kNetGames);
    const unsigned char *levels = lookup(kLevels);
    if (!net_games || !levels) return false;
    const unsigned char *games = pointer_at(net_games + 8);
    const unsigned char *level_records = pointer_at(levels + 8);
    if (!games || !level_records) return false;
    const int16_t game = peek<int16_t>(image.base + kGameIndex);
    if (game < 0 || game >= peek<int32_t>(net_games)) return false;
    const uint32_t level_id = peek<uint32_t>(games + 0x90 * uint64_t(game) + 4);
    const int32_t level_count = peek<int32_t>(levels);
    for (int32_t i = 0; i < level_count; ++i) {
        const unsigned char *record = level_records + 0x38 * uint64_t(i);
        if (peek<uint32_t>(record) != level_id) continue;
        out.name = reinterpret_cast<const char *>(pointer_at(record + 0x18));
        out.package_id = peek<uint32_t>(record + 0x20);
        return out.name != nullptr;   // only the first matching record counts
    }
    return false;
}

// An int tag of the spawner, 0 when it is missing or not an int.
int32_t int_tag(const unsigned char *spawner, uint32_t key) {
    const unsigned char *list = pointer_at(spawner + 0x68);
    if (!list) return 0;
    const unsigned char *entries = pointer_at(list);
    const int32_t count = peek<int32_t>(list + 8);
    if (!entries || count <= 0 || count > kMaxTagCount) return 0;
    for (int32_t i = 0; i < count; ++i) {
        const unsigned char *entry = entries + 16 * uint64_t(i);
        if (peek<uint32_t>(entry) != key) continue;
        const unsigned char *value = pointer_at(entry + 8);
        if (!value || peek<uint16_t>(value) != 1) return 0;
        const unsigned char *data = pointer_at(value + 8);
        return data ? peek<int32_t>(data) : 0;
    }
    return 0;
}

const char *spawner_name(const unsigned char *spawner) {
    const char *name = reinterpret_cast<const char *>(pointer_at(spawner + 0x20));
    return name ? name : "?";
}

// The non-NULL entries of the package's spawner list ("all"), in order; `first` gets entry 0.
// Only called after group_usable(package).
std::vector<const unsigned char *> all_spawners(const unsigned char *package, const unsigned char **first) {
    const unsigned char *group = pointer_at(package + kGroupOfPackage);
    const unsigned char *all = pointer_at(group + 0x08);
    const int32_t count = peek<int32_t>(group + 0x10);
    *first = pointer_at(all);
    std::vector<const unsigned char *> list;
    for (int32_t i = 0; i < count; ++i) {
        if (const unsigned char *s = pointer_at(all + 8 * uint64_t(i))) list.push_back(s);
    }
    return list;
}

bool group_usable(const unsigned char *package) {
    if (!package) return false;
    const unsigned char *group = pointer_at(package + kGroupOfPackage);
    if (!group) return false;
    const int32_t count = peek<int32_t>(group + 0x10);
    return pointer_at(group + 0x08) && count > 0 && count <= kMaxGroupCount;
}

const unsigned char *rotate(const std::vector<const unsigned char *> &candidates, uint32_t t) {
    return candidates.empty() ? nullptr : candidates[t % candidates.size()];
}

// A spawner meant for online play; at match start (`start`) preferably a start spawner.
const unsigned char *online_pick(const unsigned char *package, uint32_t t, bool start) {
    if (!group_usable(package)) return nullptr;
    const unsigned char *first;
    const std::vector<const unsigned char *> all = all_spawners(package, &first);
    if (start) {
        std::vector<const unsigned char *> starts;
        for (const unsigned char *s : all)
            if (int_tag(s, kOnlineSpawner) == 1 && int_tag(s, kStartSpawner) == 1) starts.push_back(s);
        if (!starts.empty()) return rotate(starts, t);
    }
    std::vector<const unsigned char *> online;
    for (const unsigned char *s : all)
        if (int_tag(s, kOnlineSpawner) == 1) online.push_back(s);
    return rotate(online, t);
}

// Any reasonable spawner of a map package, by name prefix in order of preference.
const unsigned char *map_pick(const unsigned char *package, uint32_t t) {
    if (!group_usable(package)) return nullptr;
    const unsigned char *first;
    const std::vector<const unsigned char *> all = all_spawners(package, &first);
    for (const char *prefix : {"pickup-", "npc-", "online-", "continue-player"}) {
        std::vector<const unsigned char *> matches;
        const size_t length = std::strlen(prefix);
        for (const unsigned char *s : all)
            if (std::strncmp(spawner_name(s), prefix, length) == 0) matches.push_back(s);
        if (!matches.empty()) return rotate(matches, t);
    }
    return first;
}

const unsigned char *make_origin_spawner() {
    std::memset(origin_spawner, 0, sizeof origin_spawner);
    poke<float>(origin_spawner + 0x1c, 1.0f);   // identity quaternion (0, 0, 0, 1) at +0x10
    poke<uint64_t>(origin_spawner + 0x20, uint64_t(uintptr_t(kOriginName)));
    return origin_spawner;
}

int32_t spawner_count(const unsigned char *package) {
    if (!package) return 0;
    const unsigned char *group = pointer_at(package + kGroupOfPackage);
    return group ? peek<int32_t>(group + 0x10) : 0;
}

U2MP_GUEST const unsigned char *spawner_package(void *ngm, uint32_t id, int32_t mode) {
    (void)ngm;
    LevelRecord level{};
    const bool have_level = level_record(level);
    const unsigned char *pak = have_level ? package_by_id(level.package_id) : nullptr;

    std::string map_name;
    if (have_level) {
        map_name = level.name;
        if (map_name.size() > 4 && map_name.compare(map_name.size() - 4, 4, "-pak") == 0)
            map_name.resize(map_name.size() - 4);
    }
    const unsigned char *map_package = map_name.empty() ? nullptr : package_by_id(sid(map_name.c_str()));

    if (have_level && last_logged_level != level.name) {
        last_logged_level = level.name;
        log("spawn: level %s %s (%d spawners), map %s %s (%d spawners)", level.name, pak ? "loaded" : "missing",
            spawner_count(pak), map_name.c_str(), map_package ? "loaded" : "missing", spawner_count(map_package));
        if (!pak)
            log("spawn: %s holds the multiplayer spawners (online-spawner-*); it is not installed, players spawn at "
                "fallback spawners",
                level.name);
    }

    if (pak && find_in_package(pak, id)) return pak;
    if (map_package && find_in_package(map_package, id)) return map_package;
    if (const unsigned char *found =
            walk_packages([id](const unsigned char *p) { return find_in_package(p, id) ? p : nullptr; }))
        return found;

    // Nobody has it: choose a stand-in spawner.
    const uint32_t t = turn++;
    const bool start = mode == 0;
    const char *method = "online spawner of the level";
    const unsigned char *spawner = online_pick(pak, t, start);
    if (!spawner) {
        method = "online spawner of a loaded package";
        spawner = walk_packages([t, start](const unsigned char *p) { return online_pick(p, t, start); });
    }
    if (!spawner) {
        method = "map spawner";
        spawner = map_pick(map_package, t);
    }
    if (!spawner) {
        method = "own spawner at the origin";
        spawner = make_origin_spawner();
    }

    if (id != last_logged_id) {
        last_logged_id = id;
        log("spawn: spawner %08x (mode %d) is in no loaded package; using %s %s (%08x)", id, mode, method,
            spawner_name(spawner), peek<uint32_t>(spawner + 0x28));
    }

    std::memset(own_entry, 0, sizeof own_entry);
    poke<uint32_t>(own_entry, id);
    poke<uint64_t>(own_entry + 8, uint64_t(uintptr_t(spawner)));
    std::memset(own_group, 0, sizeof own_group);
    poke<uint64_t>(own_group + 0x18, uint64_t(uintptr_t(own_entry)));
    poke<int32_t>(own_group + 0x20, 1);
    poke<uint64_t>(own_package + kGroupOfPackage, uint64_t(uintptr_t(own_group)));
    return own_package;
}

}  // namespace

void install_spawn(int &installed) {
    // GetSpawnLocator must pass the NetGameManager to the call and read the group at result+0x9a0.
    static const unsigned char lea_rdi[] = {0x48, 0x8D, 0x3D};
    static const unsigned char reads_group[] = {0x4C, 0x8B, 0x88, 0xA0, 0x09, 0x00, 0x00};
    int32_t lea_rel;
    std::memcpy(&lea_rel, image.base + 0x662fcc, 4);
    if (!image.matches(0x662fc9, lea_rdi, sizeof lea_rdi) || int64_t(kCallSite) + lea_rel != int64_t(kNgm) ||
        !image.matches(0x662fd5, reads_group, sizeof reads_group)) {
        log("spawn: GetSpawnLocator (662fa0) bytes differ, left alone");
        return;
    }
    if (!retarget_call(kCallSite, kLevelPackage, reinterpret_cast<void *>(&spawner_package),
                       "GetSpawnLocator spawner package (662fd0)"))
        return;
    log("spawn: GetSpawnLocator (662fa0) looks the spawner up in every loaded package, with a fallback");
    installed += 1;
}

}  // namespace u2mp
