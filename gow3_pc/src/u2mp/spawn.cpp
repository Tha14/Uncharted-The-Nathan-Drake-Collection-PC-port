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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "u2mp_internal.h"

extern "C" int runtime_memory_is_mapped(uintptr_t address, uint64_t size);

namespace u2mp {
void install_spawn(int &installed);

namespace {

constexpr uint64_t kCallSite = 0x662fd0;
constexpr uint64_t kLevelPackage = 0x644130;    // the call's original target
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kGameIndex = kNgm + 0x1ca0;
constexpr uint64_t kTaskIndex = kNgm + 0x1d6c;
constexpr uint64_t kPackageTable = 0x1130390;
constexpr uint64_t kLookupPointer = 0xa15e70;
constexpr uint64_t kSpawnerById = 0x64b960;

constexpr uint32_t kNetGames = sid("*net-games*");
constexpr uint32_t kLevels = 0x0dec97a4;    // id of the *levels* table (not the plain CRC of its name)
constexpr uint32_t kOnlineSpawner = sid("online-spawner");
constexpr uint32_t kStartSpawner = sid("start-spawner");
constexpr uint32_t kTeam = sid("team");
static_assert(kNetGames == 0x3b9a067du, "net-games id");
static_assert(kOnlineSpawner == 0x5bfae217u && kStartSpawner == 0x33364361u, "tag keys");
static_assert(kTeam == 0xd50bc34bu, "team key");

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
std::string last_type12_key;
std::string last_type12_level;

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

const unsigned char *net_game_record() {
    const LookupFn lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *net_games = lookup(kNetGames);
    if (!net_games) return nullptr;
    const int16_t game = peek<int16_t>(image.base + kGameIndex);
    const unsigned char *records = pointer_at(net_games + 8);
    if (!records || game < 0 || game >= peek<int32_t>(net_games)) return nullptr;
    return records + 0x90 * uint64_t(game);
}

int32_t game_type() {
    const unsigned char *record = net_game_record();
    return record ? peek<int32_t>(record + 0x0c) : -1;
}

// Every net game's type, level and task list: one line each, once per run.
const char *level_name_by_id(uint32_t level_id) {
    const LookupFn lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *levels = lookup(kLevels);
    if (!levels) return "?";
    const unsigned char *records = pointer_at(levels + 8);
    const int32_t count = peek<int32_t>(levels);
    if (!records || count <= 0 || count > 4096) return "?";
    for (int32_t i = 0; i < count; ++i) {
        const unsigned char *record = records + 0x38 * uint64_t(i);
        if (peek<uint32_t>(record) != level_id) continue;
        const char *name = reinterpret_cast<const char *>(pointer_at(record + 0x18));
        return name ? name : "?";
    }
    return "?";
}

void log_net_games() {
    static bool done = false;
    if (done) return;
    done = true;
    const LookupFn lookup = reinterpret_cast<LookupFn>(image.base + kLookupPointer);
    const unsigned char *net_games = lookup(kNetGames);
    if (!net_games) {
        log("spawn: *net-games* is missing");
        return;
    }
    const int32_t count = peek<int32_t>(net_games);
    const unsigned char *records = pointer_at(net_games + 8);
    if (!records || count <= 0 || count > 512) return;
    for (int32_t i = 0; i < count; ++i) {
        const unsigned char *record = records + 0x90 * uint64_t(i);
        const unsigned char *list = pointer_at(record + 0x48);
        const unsigned char *tasks = list ? pointer_at(list + 0x10) : nullptr;
        const unsigned char *fallback = list ? pointer_at(list) : nullptr;
        log("spawn: net game %d type %d level %08x %s tasks %d default %08x first %08x", i,
            peek<int32_t>(record + 0x0c), peek<uint32_t>(record + 4), level_name_by_id(peek<uint32_t>(record + 4)),
            list ? peek<int32_t>(list + 8) : 0, fallback ? peek<uint32_t>(fallback) : 0u,
            tasks ? peek<uint32_t>(tasks) : 0u);
    }
}

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

void log_type12_spawn(const LevelRecord &level, const char *where, uint32_t id, const unsigned char *spawner) {
    if (!spawner || game_type() != 12) return;
    const int32_t task = peek<int32_t>(image.base + kTaskIndex);
    const std::string key = std::string(level.name ? level.name : "") + "|" + std::to_string(task) + "|" +
                            std::to_string(id);
    if (key == last_type12_key) return;
    last_type12_key = key;
    const unsigned char *record = net_game_record();
    const unsigned char *list = record ? pointer_at(record + 0x48) : nullptr;
    const unsigned char *ids = nullptr;
    if (list && task >= 0 && task < peek<int32_t>(list + 8)) {
        const unsigned char *tasks = pointer_at(list + 0x10);
        if (tasks) ids = pointer_at(tasks + 0x28 * uint64_t(task) + 8);
    }
    const unsigned char *local = pointer_at(image.base + kNgm);
    const int32_t coop = local ? peek<int32_t>(local + 0x198) : -1;
    log("spawn: type 12 %s spawner %08x name %s pos %.2f %.2f %.2f online %d start %d team %d; task %d coop %d ids "
        "%08x %08x %08x",
        where, id, spawner_name(spawner), peek<float>(spawner), peek<float>(spawner + 4), peek<float>(spawner + 8),
        int_tag(spawner, kOnlineSpawner), int_tag(spawner, kStartSpawner), int_tag(spawner, kTeam), task, coop,
        ids ? peek<uint32_t>(ids) : 0u, ids ? peek<uint32_t>(ids + 4) : 0u, ids ? peek<uint32_t>(ids + 8) : 0u);
    for (int32_t i = 0; ids && i < 3; ++i) {
        const uint32_t candidate_id = peek<uint32_t>(ids + 4 * uint64_t(i));
        if (candidate_id == id) continue;
        const unsigned char *candidate = walk_packages(
            [candidate_id](const unsigned char *package) { return find_in_package(package, candidate_id); });
        log("spawn: type 12 task %d candidate %d %08x name %s pos %.2f %.2f %.2f online %d start %d team %d", task,
            i, candidate_id, candidate ? spawner_name(candidate) : "?",
            candidate ? peek<float>(candidate) : 0.0f, candidate ? peek<float>(candidate + 4) : 0.0f,
            candidate ? peek<float>(candidate + 8) : 0.0f, candidate ? int_tag(candidate, kOnlineSpawner) : 0,
            candidate ? int_tag(candidate, kStartSpawner) : 0, candidate ? int_tag(candidate, kTeam) : 0);
    }
    using SpawnerByIdFn = const unsigned char *(U2MP_GUEST *)(void *, int32_t);
    const SpawnerByIdFn spawner_by_id = reinterpret_cast<SpawnerByIdFn>(image.base + kSpawnerById);
    const unsigned char *zone = spawner_by_id(reinterpret_cast<void *>(image.base + kNgm), 0);
    log("spawn: type 12 capture-zone spawner 0 name %s pos %.2f %.2f %.2f",
        zone ? spawner_name(zone) : "?", zone ? peek<float>(zone) : 0.0f, zone ? peek<float>(zone + 4) : 0.0f,
        zone ? peek<float>(zone + 8) : 0.0f);
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

void log_type12_level_spawners(const LevelRecord &level, const unsigned char *package) {
    if (!package || game_type() != 12 || !group_usable(package)) return;
    const int32_t task = peek<int32_t>(image.base + kTaskIndex);
    const std::string key = std::string(level.name ? level.name : "") + "|" + std::to_string(task);
    if (key == last_type12_level) return;
    last_type12_level = key;
    const unsigned char *group = pointer_at(package + kGroupOfPackage);
    const int32_t n_all = peek<int32_t>(group + 0x10);
    const unsigned char *sorted = pointer_at(group + 0x18);
    const int32_t n_sorted = peek<int32_t>(group + 0x20);
    log("spawn: group all %d sorted %d", n_all, n_sorted);
    for (int32_t i = 0; sorted && i < n_sorted && i < 200; ++i) {
        const unsigned char *s = pointer_at(sorted + 16 * uint64_t(i) + 8);
        if (!s) continue;
        const char *sn = spawner_name(s);
        if (!std::strstr(sn, "roof") && !std::strstr(sn, "vault")) continue;
        log("spawn: sorted-only %s pos %d %d %d", sn, int(peek<float>(s) * 10.0f), int(peek<float>(s + 4) * 10.0f),
            int(peek<float>(s + 8) * 10.0f));
    }
    const unsigned char *first = nullptr;
    for (const unsigned char *s : all_spawners(package, &first)) {
        const char *name = spawner_name(s);
        const int32_t online = int_tag(s, kOnlineSpawner);
        const int32_t start = int_tag(s, kStartSpawner);
        if (!online && !start && !std::strstr(name, "coop") && std::strncmp(name, "net-", 4) != 0) continue;
        log("spawn: type 12 %s spawner-list %08x name %s pos %.2f %.2f %.2f online %d start %d team %d",
            level.name ? level.name : "?", peek<uint32_t>(s + 0x28), name, peek<float>(s), peek<float>(s + 4),
            peek<float>(s + 8), online, start, int_tag(s, kTeam));
    }
}

// The co-op arena script names its wave spawners npc-spawn-1..10; say whether the loaded packages hold them.
void log_expected_spawners() {
    static const char *const kNames[] = {"npc-spawn-1", "npc-spawn-2", "npc-spawn-3", "npc-spawn-4", "npc-spawn-5",
                                         "npc-spawn-6", "npc-spawn-7", "npc-spawn-8", "npc-spawn-9", "npc-spawn-10",
                                         "defend-1", "online-spawner-coop-1"};
    for (const char *name : kNames) {
        const uint32_t id = sid(name);
        const unsigned char *found =
            walk_packages([id](const unsigned char *package) { return find_in_package(package, id); });
        log("spawn: wave spawner %s %08x %s", name, id, found ? "loaded" : "MISSING");
    }
}

const unsigned char *rotate(const std::vector<const unsigned char *> &candidates, uint32_t t) {
    return candidates.empty() ? nullptr : candidates[t % candidates.size()];
}// A spawner meant for online play; at match start (`start`) preferably a start spawner.
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
        if (game_type() == 12) log_expected_spawners();
        log_net_games();
    }

    if (pak) {
        log_type12_level_spawners(level, pak);
        if (const unsigned char *found = find_in_package(pak, id)) {
            log_type12_spawn(level, "level", id, found);
            return pak;
        }
    }
    if (map_package) {
        if (const unsigned char *found = find_in_package(map_package, id)) {
            log_type12_spawn(level, "map", id, found);
            return map_package;
        }
    }
    const unsigned char *other_package = nullptr;
    if (const unsigned char *found = walk_packages([id, &other_package](const unsigned char *p) {
            const unsigned char *spawner = find_in_package(p, id);
            if (spawner) other_package = p;
            return spawner;
        })) {
        log_type12_spawn(level, "loaded", id, found);
        return other_package;
    }

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
    log_type12_spawn(level, method, id, spawner);

    std::memset(own_entry, 0, sizeof own_entry);
    poke<uint32_t>(own_entry, id);
    poke<uint64_t>(own_entry + 8, uint64_t(uintptr_t(spawner)));
    std::memset(own_group, 0, sizeof own_group);
    poke<uint64_t>(own_group + 0x18, uint64_t(uintptr_t(own_entry)));
    poke<int32_t>(own_group + 0x20, 1);
    poke<uint64_t>(own_package + kGroupOfPackage, uint64_t(uintptr_t(own_group)));
    return own_package;
}

// NetNpc name formatting in the AI agent spawn: proves whether Siege ever creates a networked NPC.
constexpr uint64_t kNetNpcNameCall = 0x23bb22;
constexpr uint64_t kNetNpcName = 0x64d840;

U2MP_GUEST int32_t npc_name_probe(char *name, uint32_t sequence) {
    using FormatFn = int32_t(U2MP_GUEST *)(char *, ...);
    const int32_t result = reinterpret_cast<FormatFn>(image.base + kNetNpcName)(name, sequence);
    static uint32_t last = UINT32_MAX;
    if (sequence != last) {
        last = sequence;
        log("spawn: NetNpc name '%s' (seq %u) for a new AI agent", name, sequence);
    }
    return result;
}

// Every NPC the engine or a script creates comes through SpawnNpc; the caller tells which path asked.
// The converted multiplayer-level spawners carry no NPC class (see TODO.md); this is a character
// prototype class the village level loads, used until the real converter-side value is known.
U2MP_GUEST inline uint64_t image_va(uint64_t off) {
    return uint64_t(uintptr_t(image.base)) + off;
}
U2MP_GUEST inline bool in_live_match() {
    return peek<int32_t>(reinterpret_cast<const unsigned char *>(uintptr_t(image_va(0x113b760 + 0x1e80)))) == 2;
}
constexpr uint64_t kSpawnNpc = 0x4be330;
constexpr uint64_t kSpawnNpcSites[] = {0x13b9,   0x3b8c,   0x2e5780, 0x2e597a, 0x45b9f2, 0x4bf936,
                                       0x61e7be, 0x62378a, 0x647c62, 0x647dc6, 0x6484e8, 0x9fc1e4,
                                       0xa0962d, 0xa644ad, 0xa64d9a, 0xa69907};

float g_active_zone[3] = {0, 0, 0};
bool g_have_active_zone = false;

U2MP_GUEST uint8_t *spawn_npc_probe(uint8_t *spawner, void *params, void *locator, uint64_t zero) {
    using SpawnFn = uint8_t *(U2MP_GUEST *)(uint8_t *, void *, void *, uint64_t);
    uint8_t *const npc = reinterpret_cast<SpawnFn>(image.base + kSpawnNpc)(spawner, params, locator, zero);
    if (npc && spawner && game_type() == 12) {
        const char *sn = spawner_name(spawner);
        if (std::strncmp(sn, "net-territory", 13) == 0) {
            g_active_zone[0] = peek<float>(spawner);
            g_active_zone[1] = peek<float>(spawner + 4);
            g_active_zone[2] = peek<float>(spawner + 8);
            g_have_active_zone = true;
            log("spawn: active zone marker %s at %d %d %d", sn, int(g_active_zone[0] * 10.0f),
                int(g_active_zone[1] * 10.0f), int(g_active_zone[2] * 10.0f));
        }
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 300) {
        const uint64_t site = uint64_t(reinterpret_cast<uintptr_t>(__builtin_return_address(0))) -
                              uint64_t(reinterpret_cast<uintptr_t>(image.base));
        const uint32_t cls = spawner ? peek<uint32_t>(spawner + 0x40) : 0;
        log("spawn: SpawnNpc type %d from 0x%llx spawner %s class %08x -> %p", game_type(),
            (unsigned long long)site, spawner ? spawner_name(spawner) : "?", cls, static_cast<void *>(npc));
    }
    return npc;
}

// Every game type's per-frame update calls this shared wave/spawner routine; if the type 12
// (Siege) update never gets here, the missing enemies are upstream of the spawn logic.
constexpr uint64_t kSpawnerUpdate = 0x236f40;
constexpr uint64_t kSpawnerUpdateSites[] = {0x27e86c, 0x280e25, 0x28375e, 0x284b72, 0x28642a, 0x28c8e2,
                                            0x28e8ac, 0x2902ec, 0x292377, 0x294257, 0x295f7e, 0x296b44};

U2MP_GUEST uint64_t spawner_update_probe(uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    using UpdateFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t);
    const uint64_t result = reinterpret_cast<UpdateFn>(image.base + kSpawnerUpdate)(a, b, c, d);
    const uint64_t site = uint64_t(reinterpret_cast<uintptr_t>(__builtin_return_address(0))) -
                          uint64_t(reinterpret_cast<uintptr_t>(image.base));
    static std::atomic<uint64_t> seen[sizeof kSpawnerUpdateSites / sizeof kSpawnerUpdateSites[0]];
    for (size_t i = 0; i < sizeof kSpawnerUpdateSites / sizeof kSpawnerUpdateSites[0]; ++i) {
        if (kSpawnerUpdateSites[i] != site) continue;
        if (seen[i].fetch_add(1) == 0)
            log("spawn: spawner update first call from 0x%llx (type %d, npc seq %d)",
                (unsigned long long)site, peek<int16_t>(image.base + kGameIndex), peek<int32_t>(image.base + kNgm + 0x1c8c));
        break;
    }
    return result;
}

// The per-type update functions are reached through pointers built at load time, so find where the
// image points at them: that is the vtable slot the game dispatches the current type through.
constexpr uint64_t kTypeUpdates[] = {0x27e7e0, 0x280da0, 0x2836e0, 0x284af0, 0x2863a0, 0x28c860,
                                     0x28e840, 0x290250, 0x2922e0, 0x2941d0, 0x295f00, 0x296b20};

// The script native get-spawner-locator asks its table twice: log the id and what came back.
constexpr uint64_t kSpawnerLookup = 0xce33a0;
constexpr uint64_t kSpawnerLookupCall1 = 0xa08d42;
constexpr uint64_t kSpawnerLookupCall2 = 0xa08d59;

U2MP_GUEST const unsigned char *spawner_lookup_probe(uint32_t id, uint32_t flags) {
    using LookupByIdFn = const unsigned char *(U2MP_GUEST *)(uint32_t, uint32_t);
    const unsigned char *const result =
        reinterpret_cast<LookupByIdFn>(image.base + kSpawnerLookup)(id, flags);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 96) {
        const unsigned char *loaded =
            walk_packages([id](const unsigned char *package) { return find_in_package(package, id); });
        log("spawn: script asked for spawner %08x -> %p (%s, %s)", id, static_cast<const void *>(result),
            result ? spawner_name(result) : "no locator", loaded ? "loaded package" : "not in any package");
    }
    return result;
}

using CtorFn = uint64_t(U2MP_GUEST *)(void);
CtorFn type12_ctor_original = nullptr;
uint64_t type12_object = 0;

U2MP_GUEST uint64_t type12_ctor_wrapper() {
    const uint64_t object = type12_ctor_original();
    type12_object = object;
    const uint64_t host = reinterpret_cast<uint64_t>(image.base);
    log("spawn: type-12 class constructed at 0x%llx, vtable 0x%llx, from 0x%llx", (unsigned long long)object,
        (unsigned long long)(*reinterpret_cast<const uint64_t *>(object) - host),
        (unsigned long long)(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - uintptr_t(image.base)));
    int stored = 0;
    for (uint64_t a = 0x104c000; a + 8 <= 0x173b898 && stored < 4; a += 8)
        if (*reinterpret_cast<const uint64_t *>(image.base + a) == object) {
            log("spawn: wave manager owner in image at 0x%llx", (unsigned long long)a);
            ++stored;
        }
    static bool heap_done = false;
    if (!heap_done) {
        heap_done = true;
        const uint64_t ranges[3][2] = {{0x1000000000ull, 0x1064000000ull},
                                       {0x1100000000ull, 0x11bad00000ull},
                                       {0x1200000000ull, 0x1214000000ull}};
        for (int r = 0; r < 3 && stored < 10; ++r)
            for (uint64_t page = ranges[r][0]; page < ranges[r][1] && stored < 10; page += 0x1000) {
                if (!runtime_memory_is_mapped(uintptr_t(page), 0x1000)) continue;
                for (uint64_t a = page; a < page + 0x1000; a += 8) {
                    if (*reinterpret_cast<const uint64_t *>(a) != object) continue;
                    const uint64_t owner = (a & ~0xfffull);
                    const uint64_t vt = *reinterpret_cast<const uint64_t *>(owner);
                    log("spawn: wave manager owner at 0x%llx (page base vtable 0x%llx)", (unsigned long long)a,
                        (unsigned long long)(vt >= host ? vt - host : vt));
                    ++stored;
                    break;
                }
            }
    }
    log("spawn: wave manager owners found: %d", stored);
    {
        const uint64_t host_b = reinterpret_cast<uint64_t>(image.base);
        int anchor = 0;
        const uint64_t *slot = reinterpret_cast<const uint64_t *>(&anchor);
        int seen = 0;
        for (int i = 1; i < 512 && seen < 8; ++i) {
            const uint64_t v = slot[-i];
            const uint64_t off = v >= host_b ? v - host_b : v;
            if (off < 0x1000 || off >= 0xfe44b0) continue;
            log("spawn: wave manager stack 0x%llx", (unsigned long long)off);
            ++seen;
        }
    }
    return object;
}

}  // namespace

void install_update_slot_probes();

void scan_type_vtables() {
    constexpr uint64_t kScanStart = 0xfe8000, kScanEnd = 0x173b898;
    static int done = 0;
    // The registration table is built after boot, so watch its record from the first frame: the
    // class is constructed when the match is created, long before the match state reaches playing.
    const uint64_t host_now = reinterpret_cast<uint64_t>(image.base);
    if (!type12_ctor_original) {
        uint64_t *const record = reinterpret_cast<uint64_t *>(image.base + 0x14635b8);
        if (*record == host_now + 0x2922e0) {
            type12_ctor_original = reinterpret_cast<CtorFn>(host_now + 0x2922e0);
            *record = reinterpret_cast<uint64_t>(&type12_ctor_wrapper);
            log("spawn: type-12 class constructor hooked at the registration record");
        }
    }
    if (type12_object) {
        static int n = 0;
        if (++n % 600 == 0)
            log("spawn: type-12 object 0x%llx state %d, %d, child 0x%llx", (unsigned long long)type12_object,
                *reinterpret_cast<const int32_t *>(type12_object + 0x150),
                *reinterpret_cast<const int32_t *>(type12_object + 0x154),
                (unsigned long long)*reinterpret_cast<const uint64_t *>(type12_object + 0x3fa0));
    }
    // ngm+0x1ca0 is the *net-games* record index, not the game type (which lives in record+0xc).
    const int16_t game = peek<int16_t>(image.base + kGameIndex);
    static int tick = 0;
    install_update_slot_probes();
    if (peek<int32_t>(image.base + kNgm + 0x1e80) != 2) {
        if (++tick % 600 == 0)
            log("spawn: scan waiting (net game %d, net %d, ngm state %d)", game,
                peek<int32_t>(image.base + 0x14ce97c), peek<int32_t>(image.base + kNgm + 0x1e80));
        return;
    }
    // Mode objects are on the runtime heap, point at a static vtable inside the image, and carry their
    // game type in +0x150 (Gold Rush 7, Siege 2). Report those, so each match's live class is known.
    static int scanned_game = -1;
    if (scanned_game != game) {
        scanned_game = game;
        log("spawn: mode object scan for net game %d", game);
        int found = 0;
        const uint64_t ranges[3][2] = {{0x1000000000ull, 0x1064000000ull},
                                       {0x1100000000ull, 0x11bad00000ull},
                                       {0x1200000000ull, 0x1214000000ull}};
        for (int r = 0; r < 3 && found < 20; ++r)
            for (uint64_t page = ranges[r][0]; page < ranges[r][1] && found < 20; page += 0x1000) {
                if (!runtime_memory_is_mapped(uintptr_t(page), 0x1000)) continue;
                for (uint64_t a = page; a < page + 0x1000; a += 8) {
                    const uint64_t v = *reinterpret_cast<const uint64_t *>(a);
                    const uint64_t vbase = v >= host_now ? v - host_now : v;
                    if (vbase < 0xfe8000 || vbase >= 0x106b860) continue;
                    if (!runtime_memory_is_mapped(uintptr_t(a), 0x160)) continue;
                    const int32_t type = *reinterpret_cast<const int32_t *>(a + 0x150);
                    if (type < 0 || type > 20) continue;
                    log("spawn: mode object 0x%llx vtable 0x%llx type field %d", (unsigned long long)a,
                        (unsigned long long)vbase, type);
                    ++found;
                }
            }
        log("spawn: mode object scan done (%d objects)", found);
    }
    if (done) return;
    done = 1;
    log("spawn: scan base 0x%llx size 0x%llx, at 0xff6680: 0x%llx, at 0x1000: 0x%llx",
        (unsigned long long)(uintptr_t)image.base, (unsigned long long)image.size,
        (unsigned long long)*reinterpret_cast<const uint64_t *>(image.base + 0xff6680),
        (unsigned long long)*reinterpret_cast<const uint64_t *>(image.base + 0x1000));
    const uint64_t host = reinterpret_cast<uint64_t>(image.base);
    for (uint64_t a = kScanStart; a + 8 <= kScanEnd; a += 8) {
        const uint64_t v = *reinterpret_cast<const uint64_t *>(image.base + a);
        for (const uint64_t fn : kTypeUpdates)
            if (v == fn || v == host + fn)
                log("spawn: vtable hit 0x%llx -> 0x%llx (%s)", (unsigned long long)a, (unsigned long long)fn,
                    v == fn ? "guest" : "host");
    }
    // Live objects of the game-type family: a vtable pointer into the 0xff8000-0xffa800 window.
    // Report each one, and mark the ones whose vtable holds the type-12 update 0x292360.
    int live = 0;
    for (uint64_t a = 0x104c000; a + 8 <= 0x173b898 && live < 24; a += 8) {
        const uint64_t v = *reinterpret_cast<const uint64_t *>(image.base + a);
        if (v < host + 0xff8000 || v >= host + 0xffa800) continue;
        const uint64_t vt = v - host;
        int64_t update_slot = -1;
        for (uint64_t slot = 0; slot < 0x400; slot += 8)
            if (*reinterpret_cast<const uint64_t *>(image.base + vt + slot) == host + 0x292360) update_slot = int64_t(slot);
        log("spawn: live vtable object 0x%llx vtable 0x%llx state %d, %d, type-12 update %s", (unsigned long long)a,
            (unsigned long long)vt, peek<int32_t>(image.base + a + 0x150), peek<int32_t>(image.base + a + 0x154),
            update_slot >= 0 ? "at +0x28" : "absent");
        ++live;
    }

    // The update functions are also copied into runtime-built tables in the writable segment; dump the
    // neighbourhood of each such table and find the objects whose vtable pointer lands inside it.
    uint64_t tables[64];
    int table_count = 0;
    for (uint64_t a = 0x104c000; a + 8 <= 0x173b898 && table_count < 64; a += 8) {
        const uint64_t v = *reinterpret_cast<const uint64_t *>(image.base + a);
        for (const uint64_t fn : kTypeUpdates)
            if (v == host + fn) {
                log("spawn: update pointer at 0x%llx -> 0x%llx", (unsigned long long)a, (unsigned long long)fn);
                tables[table_count++] = a;
                break;
            }
    }
    for (int t = 0; t < table_count; ++t) {
        const uint64_t at = tables[t];
        for (int64_t k = -0x40; k <= 0x40; k += 8) {
            const uint64_t v = *reinterpret_cast<const uint64_t *>(image.base + at + k);
            if (v) log("spawn: table 0x%llx%+lld = 0x%llx", (unsigned long long)at, (long long)k,
                       (unsigned long long)(v >= host ? v - host : v));
        }
        for (uint64_t obj = 0x104c000; obj + 8 <= 0x173b898; obj += 8) {
            const uint64_t v = *reinterpret_cast<const uint64_t *>(image.base + obj);
            if (v >= host + at - 0x800 && v <= host + at)
                log("spawn: object 0x%llx vtable 0x%llx (slot +0x%llx)", (unsigned long long)obj,
                    (unsigned long long)(v - host), (unsigned long long)(at - (v - host)));
        }
    }

    // Live objects: a vtable pointer into the read-only data segment. Report the ones whose table
    // holds a per-type update, with the slot offset, so the current type's class is identifiable.
    constexpr uint64_t kVtableLo = 0xfe8000, kVtableHi = 0x104c000;
    for (uint64_t a = 0x104c000; a + 8 <= 0x173b898; a += 8) {
        const uint64_t vtable = *reinterpret_cast<const uint64_t *>(image.base + a);
        const bool rebased = vtable >= host + kVtableLo && vtable < host + kVtableHi;
        if (!rebased && (vtable < kVtableLo || vtable >= kVtableHi)) continue;
        const uint64_t vt = rebased ? vtable - host : vtable;
        for (uint64_t slot = 0; slot < 0x600; slot += 8) {
            const uint64_t fn = *reinterpret_cast<const uint64_t *>(image.base + vt + slot);
            bool known = false;
            for (const uint64_t f : kTypeUpdates)
                if (fn == f || fn == host + f) known = true;
            if (!known) continue;
            log("spawn: live object 0x%llx vtable 0x%llx slot +0x%llx -> 0x%llx (net game %d)",
                (unsigned long long)a, (unsigned long long)vtable, (unsigned long long)slot,
                (unsigned long long)(fn - (fn >= host ? host : 0)), game);
            break;
        }
    }
    log("spawn: vtable scan done (net game %d)", game);
}

// The per-type updates are only reachable through their vtable slots (no direct call in the image),
// so the slot itself is replaced: the first call of each type is reported with its game-type field.
constexpr uint64_t kUpdateSlots[] = {0xff6680, 0xff6b90, 0xff70a0, 0xff7570, 0xff7a40, 0xff8280,
                                     0xff8748, 0xff8c20, 0xff90f0, 0xff95c0, 0xff9a90, 0xff9f60};

constexpr uint64_t kSlotFns[] = {0x27e7e0, 0x280da0, 0x2836e0, 0x284af0, 0x2863a0, 0x28c860,
                                 0x28e840, 0x290250, 0x292360, 0x2941d0, 0x295f00, 0x296b20};

using SlotFn = uint64_t(U2MP_GUEST *)(void *);

template <int I> U2MP_GUEST uint64_t update_slot_probe(void *self) {
    static std::atomic<int> seen;
    if (seen.fetch_add(1) == 0)
        log("spawn: type update slot 0x%llx (fn 0x%llx) ran, this 0x%llx field %d", (unsigned long long)kUpdateSlots[I],
            (unsigned long long)kSlotFns[I], (unsigned long long)(uintptr_t)self,
            self ? *reinterpret_cast<const int32_t *>(static_cast<const char *>(self) + 0x150) : -1);
    return reinterpret_cast<SlotFn>(image.base + kSlotFns[I])(self);
}

void install_update_slot_probes() {
    const uint64_t host = reinterpret_cast<uint64_t>(image.base);
    static SlotFn probes[12] = {&update_slot_probe<0>, &update_slot_probe<1>, &update_slot_probe<2>,
                                &update_slot_probe<3>, &update_slot_probe<4>, &update_slot_probe<5>,
                                &update_slot_probe<6>, &update_slot_probe<7>, &update_slot_probe<8>,
                                &update_slot_probe<9>, &update_slot_probe<10>, &update_slot_probe<11>};
    static int patched = 0;
    static bool mismatch_reported = false;
    int now = 0;
    for (int i = 0; i < 12; ++i) {
        uint64_t *slot = reinterpret_cast<uint64_t *>(image.base + kUpdateSlots[i]);
        if (*slot == reinterpret_cast<uint64_t>(probes[i])) {
            ++now;
            continue;
        }
        if (*slot != host + kSlotFns[i]) {
            if (!mismatch_reported)
                log("spawn: update slot 0x%llx holds 0x%llx, expected 0x%llx", (unsigned long long)kUpdateSlots[i],
                    (unsigned long long)(*slot - host), (unsigned long long)kSlotFns[i]);
            continue;
        }
        *slot = reinterpret_cast<uint64_t>(probes[i]);
        ++now;
    }
    if (now != patched) {
        patched = now;
        log("spawn: %d of 12 per-type update slots hooked", patched);
    }
    mismatch_reported = true;
}

constexpr uint64_t kFactory = 0x9b5c20;

U2MP_GUEST void factory_probe(void *request, void *frame) {
    static uint32_t seen_ids[12];
    static int seen = 0;
    const uint32_t id = *reinterpret_cast<const uint32_t *>(request);
    for (int i = 0; i < seen; ++i)
        if (seen_ids[i] == id) return;
    if (seen < 12) seen_ids[seen++] = id;
    const uint64_t caller = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(frame) + 8);
    log("spawn: factory id %08x from 0x%llx", id,
        (unsigned long long)(caller - reinterpret_cast<uint64_t>(image.base)));
}

void install_factory_probe(int &installed) {
    static const unsigned char expected[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57};
    if (!image.matches(kFactory, expected, sizeof expected)) {
        log("spawn: factory 0x9b5c20 bytes differ, left alone");
        return;
    }
    unsigned char *stub = near_stub(64);
    const int64_t distance = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + kFactory + 5)) : 0;
    if (!stub || distance < INT32_MIN || distance > INT32_MAX) {
        log("spawn: factory probe: no stub memory in range");
        return;
    }
    size_t n = 0;
    auto bytes = [&](std::initializer_list<unsigned char> list) {
        for (unsigned char b : list) stub[n++] = b;
    };
    auto quad = [&](uint64_t v) {
        std::memcpy(stub + n, &v, 8);
        n += 8;
    };
    bytes({0x55});                              // push rbp
    bytes({0x48, 0x89, 0xE5});                  // mov rbp, rsp
    bytes({0x41, 0x57});                        // push r15
    bytes({0x57});                              // push rdi
    bytes({0x56});                              // push rsi
    bytes({0x48, 0x89, 0xEE});                  // mov rsi, rbp
    bytes({0x48, 0xB8});                        // mov rax, probe
    quad(uint64_t(uintptr_t(&factory_probe)));
    bytes({0xFF, 0xD0});                        // call rax
    bytes({0x5E});                              // pop rsi
    bytes({0x5F});                              // pop rdi
    bytes({0x48, 0xB8});                        // mov rax, resume
    quad(uint64_t(uintptr_t(image.base + kFactory + 6)));
    bytes({0xFF, 0xE0});                        // jmp rax
    unsigned char *code = image.base + kFactory;
    code[0] = 0xE9;
    const int32_t rel = int32_t(distance);
    std::memcpy(code + 1, &rel, 4);
    code[5] = 0x90;
    log("spawn: factory 0x9b5c20 hooked (class id and caller)");
    installed += 1;
}

U2MP_GUEST void spawn_result_probe(uint64_t, uint64_t, uint64_t) {}
bool install_result_probe(uint64_t at_entry, int &installed) {
    static const unsigned char expected[] = {0x49, 0x8B, 0x04, 0x24, 0x48, 0x3B, 0x45, 0xD8};
    if (!image.matches(at_entry, expected, sizeof expected)) {
        log("spawn: result probe 0x%llx bytes differ, left alone", (unsigned long long)at_entry);
        return false;
    }
    unsigned char *stub = near_stub(96);
    const int64_t distance = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + at_entry + 5)) : 0;
    if (!stub || distance < INT32_MIN || distance > INT32_MAX) {
        log("spawn: result probe: no stub memory in range");
        return false;
    }
    size_t n = 0;
    auto bytes = [&](std::initializer_list<unsigned char> list) {
        for (unsigned char b : list) stub[n++] = b;
    };
    auto quad = [&](uint64_t v) {
        std::memcpy(stub + n, &v, 8);
        n += 8;
    };
    bytes({0x57, 0x56, 0x52, 0x51});            // push rdi/rsi/rdx/rcx
    bytes({0x41, 0x50, 0x41, 0x51});            // push r8/r9
    bytes({0x44, 0x89, 0xF7});                  // mov edi, r14d
    bytes({0x4C, 0x89, 0xF2});                  // mov rdx, r14
    bytes({0x48, 0x8B, 0x75, 0x08});            // mov rsi, [rbp+8]
    bytes({0x48, 0xB8});                        // mov rax, probe
    quad(uint64_t(uintptr_t(&spawn_result_probe)));
    bytes({0xFF, 0xD0});                        // call rax
    bytes({0x41, 0x59, 0x41, 0x58});            // pop r9, r8
    bytes({0x59, 0x5A, 0x5E, 0x5F});            // pop rcx, rdx, rsi, rdi
    bytes({0x49, 0x8B, 0x04, 0x24});            // mov rax, [r12]
    bytes({0x48, 0x3B, 0x45, 0xD8});            // cmp rax, [rbp-0x28]
    bytes({0x48, 0xB8});                        // mov rax, resume
    quad(uint64_t(uintptr_t(image.base + at_entry + 8)));
    bytes({0xFF, 0xE0});                        // jmp rax
    unsigned char *code = image.base + at_entry;
    code[0] = 0xE9;
    const int32_t rel = int32_t(distance);
    std::memcpy(code + 1, &rel, 4);
    log("spawn: result probe installed at 0x%llx", (unsigned long long)at_entry);
    installed += 1;
    return true;
}

U2MP_GUEST void batch_candidate_probe(uint64_t result, uint64_t cand, uint64_t list) {
    const int32_t type = game_type();
    if (type != 9 && type != 12) return;
    if (!in_live_match()) return;
    if (!runtime_memory_is_mapped(cand, 0x80)) return;
    auto at = [](uint64_t addr) {
        return reinterpret_cast<const unsigned char *>(uintptr_t(addr));
    };
    const char *name = spawner_name(reinterpret_cast<const uint8_t *>(uintptr_t(cand)));
    // The converted level ships its NPC spawners disabled (bit 1 of the flags at +0x50, which
    // blocks them in 0x4bdb20). The online mode enables them when a territory starts; nothing does
    // here, so the batch skips every npc-soldier-* spawner and the mode has no attackers.
    static float spots[64][3];
    static int spots_count = 0;
    if (type == 12 && std::strncmp(name, "net-territory", 13) == 0 && spots_count < 64) {
        spots[spots_count][0] = peek<float>(at(cand));
        spots[spots_count][1] = peek<float>(at(cand + 4));
        spots[spots_count][2] = peek<float>(at(cand + 8));
        spots_count += 1;
        if (spots_count == 1 || spots_count % 16 == 0)
            log("spawn: territory spots %d first %d %d %d", spots_count, int(spots[0][0] * 10.0f),
                int(spots[0][1] * 10.0f), int(spots[0][2] * 10.0f));
    }
    if (type == 12 && spots_count) {
        static uint64_t seen[64] = {0};
        static int seen_n = 0;
        bool near = false;
        for (int i = 0; i < spots_count && !near; ++i) {
            const float dx = peek<float>(at(cand)) - spots[i][0];
            const float dy = peek<float>(at(cand + 4)) - spots[i][1];
            const float dz = peek<float>(at(cand + 8)) - spots[i][2];
            if (dx * dx + dy * dy + dz * dz < 2500.0f) near = true;
        }
        bool known = false;
        for (int i = 0; i < seen_n; ++i)
            if (seen[i] == cand) known = true;
        if (near && !known && seen_n < 64) {
            seen[seen_n++] = cand;
            log("spawn: near %s type %d group %d flags %04x at %d %d %d", name, type,
                peek<int32_t>(at(cand + 60)), unsigned(peek<uint16_t>(at(cand + 0x50))),
                int(peek<float>(at(cand)) * 10.0f), int(peek<float>(at(cand + 4)) * 10.0f),
                int(peek<float>(at(cand + 8)) * 10.0f));
        }
    }
    static float entries[8][3];
    static int entries_count = 0;
    if (type == 12 && (std::strstr(name, "roof-spawner") || std::strstr(name, "vault-spawner"))) {
        bool known = false;
        for (int i = 0; i < entries_count; ++i)
            if (entries[i][0] == peek<float>(at(cand)) && entries[i][2] == peek<float>(at(cand + 8)))
                known = true;
        if (!known && entries_count < 8) {
            entries[entries_count][0] = peek<float>(at(cand));
            entries[entries_count][1] = peek<float>(at(cand + 4));
            entries[entries_count][2] = peek<float>(at(cand + 8));
            entries_count += 1;
            const char *art = reinterpret_cast<const char *>(pointer_at(at(cand + 0x48)));
            char a0 = art && runtime_memory_is_mapped(uintptr_t(art), 1) ? art[0] : 0;
            char a1 = art && runtime_memory_is_mapped(uintptr_t(art), 2) ? art[1] : 0;
            log("spawn: entry point %s at %d %d %d (total %d) type %d art1 %c%c", name,
                int(entries[entries_count - 1][0] * 10.0f), int(entries[entries_count - 1][1] * 10.0f),
                int(entries[entries_count - 1][2] * 10.0f), entries_count, peek<int32_t>(at(cand + 0x40)), a0, a1);
        }
    }
    if (type == 12 && std::strncmp(name, "npc-soldier", 11) == 0) {
        const auto handles = peek<int32_t>(at(image_va(0x113b760 + 0x1dd0)));
        static int last_handles = 0;
        static int allowed = handles > 0 ? 4 : 0;
        static int ring = 0;
        if (handles != last_handles) {
            last_handles = handles;
            allowed = handles > 0 ? 4 : 0;
            ring = 0;
            log("spawn: zone wave: handles %d, releasing %d attackers", handles, allowed);
        }
        auto *flagsp = reinterpret_cast<volatile uint16_t *>(uintptr_t(cand + 0x50));
        if (allowed > 0 && entries_count && (*flagsp & 2u)) {
            allowed -= 1;
            auto *pos = reinterpret_cast<volatile float *>(uintptr_t(cand));
            int best = 0;
            if (g_have_active_zone)
                for (int i = 1; i < entries_count; ++i) {
                    const float bx = entries[best][0] - g_active_zone[0];
                    const float bz = entries[best][2] - g_active_zone[2];
                    const float cx = entries[i][0] - g_active_zone[0];
                    const float cz = entries[i][2] - g_active_zone[2];
                    if (cx * cx + cz * cz < bx * bx + bz * bz) best = i;
                }
            static const float off[4][2] = {{0.0f, 0.0f}, {1.6f, 0.0f}, {-1.6f, 0.0f}, {0.0f, 1.6f}};
            const int k = ring++ & 3;
            pos[0] = entries[best][0] + off[k][0];
            pos[1] = entries[best][1];
            pos[2] = entries[best][2] + off[k][1];
            const unsigned was = *flagsp;
            *flagsp = uint16_t(was & ~2u);
            log("spawn: released npc spawner %s at ring %d flags %04x->%04x", name, ring, was, unsigned(*flagsp));
        }
    }
    static int lines[2] = {0, 0};
    const int slot = type == 12 ? 1 : 0;
    if (lines[slot]++ >= 300) return;
    unsigned flags = unsigned(peek<uint16_t>(at(cand + 0x50))) | (unsigned(peek<uint8_t>(at(cand + 0x52))) << 16);
    log("spawn: gate type %d %llu flags 0x%06x name %s list 0x%llx", type, (unsigned long long)result, flags,
        name, (unsigned long long)list);
}

bool install_batch_result_probe(uint64_t at_entry, int &installed) {
    static const unsigned char expected[] = {0x84, 0xC0, 0x0F, 0x84, 0xC7, 0x00};
    if (!image.matches(at_entry, expected, sizeof expected)) {
        log("spawn: batch probe 0x%llx bytes differ, left alone", (unsigned long long)at_entry);
        return false;
    }
    unsigned char *stub = near_stub(96);
    const int64_t distance = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + at_entry + 5)) : 0;
    if (!stub || distance < INT32_MIN || distance > INT32_MAX) {
        log("spawn: batch probe: no stub memory in range");
        return false;
    }
    size_t n = 0;
    auto bytes = [&](std::initializer_list<unsigned char> list) {
        for (unsigned char b : list) stub[n++] = b;
    };
    auto quad = [&](uint64_t v) {
        std::memcpy(stub + n, &v, 8);
        n += 8;
    };
    bytes({0x50});                              // push rax
    bytes({0x57, 0x56, 0x52, 0x51});            // push rdi/rsi/rdx/rcx
    bytes({0x41, 0x50, 0x41, 0x51});            // push r8/r9
    bytes({0x0F, 0xB6, 0xF8});                  // movzx edi, al
    bytes({0x4C, 0x89, 0xF6});                  // mov rsi, r14
    bytes({0x4C, 0x89, 0xE2});                  // mov rdx, r12
    bytes({0x48, 0xB8});                        // mov rax, probe
    quad(uint64_t(uintptr_t(&batch_candidate_probe)));
    bytes({0xFF, 0xD0});                        // call rax
    bytes({0x41, 0x59, 0x41, 0x58});            // pop r9, r8
    bytes({0x59, 0x5A, 0x5E, 0x5F});            // pop rcx, rdx, rsi, rdi
    bytes({0x58});                              // pop rax
    bytes({0x84, 0xC0});                        // test al, al
    bytes({0x0F, 0x84});                        // je 0x4bf9f4
    int32_t je = int32_t(uintptr_t(image.base + 0x4bf9f4) - uintptr_t(stub + n + 4));
    std::memcpy(stub + n, &je, 4);
    n += 4;
    bytes({0x48, 0xB8});                        // mov rax, 0x4bf92d
    quad(uint64_t(uintptr_t(image.base + 0x4bf92d)));
    bytes({0xFF, 0xE0});                        // jmp rax
    unsigned char *code = image.base + at_entry;
    code[0] = 0xE9;
    const int32_t rel = int32_t(distance);
    std::memcpy(code + 1, &rel, 4);
    log("spawn: batch probe installed at 0x%llx", (unsigned long long)at_entry);
    installed += 1;
    return true;
}

void install_spawn(int &installed) {
    // Off while the boot crash is tracked: both patch the image at startup.
    // install_update_slot_probes();
    // install_factory_probe(installed);
    install_result_probe(0x4bdfad, installed);
    install_batch_result_probe(0x4bf925, installed);
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

    static const unsigned char netnpc_call[] = {0xE8, 0x19, 0x1D, 0x41, 0x00};
    if (image.matches(kNetNpcNameCall, netnpc_call, sizeof netnpc_call) &&
        retarget_call(kNetNpcNameCall, kNetNpcName, reinterpret_cast<void *>(&npc_name_probe),
                      "NetNpc name (AI spawn probe)"))
        installed += 1;

    for (const uint64_t site : kSpawnNpcSites) {
        if (retarget_call(site, kSpawnNpc, reinterpret_cast<void *>(&spawn_npc_probe), "SpawnNpc (probe)"))
            installed += 1;
    }

    for (const uint64_t site : kSpawnerUpdateSites) {
        if (retarget_call(site, kSpawnerUpdate, reinterpret_cast<void *>(&spawner_update_probe),
                          "spawner update (probe)"))
            installed += 1;
    }

    for (const uint64_t site : {kSpawnerLookupCall1, kSpawnerLookupCall2}) {
        if (retarget_call(site, kSpawnerLookup, reinterpret_cast<void *>(&spawner_lookup_probe),
                          "get-spawner-locator lookup (probe)"))
            installed += 1;
    }
}

}  // namespace u2mp
