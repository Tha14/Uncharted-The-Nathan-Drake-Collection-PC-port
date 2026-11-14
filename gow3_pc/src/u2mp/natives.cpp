// u2mp natives: the co-op and net DC script natives that PS4 1.02 left as stubs.
//
// Reimplemented from the behaviour description u2mp/docs/spec/netanim.md.
//
// Seven natives used by co-op objective scripts and NPC fights return 0 on PS4 (net-coop-task only
// looks up *net-games* first). Each is replaced whole by a C++ function that gives the multiplayer
// answer from game state the PS4 image still keeps: the NetGameManager's player and NPC trackers,
// the current net game and task, the match session and the capture-zone spawner.
//
// A DC native is `u64 native(int argc, const u64 *argv)`; each argv slot is 8 bytes and only its
// low 32 bits are used here.

#include "u2mp_internal.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace u2mp {
namespace {

// ---- image offsets (Uncharted 2 PS4 1.02) -------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;     // i32, multiplayer when > 1
constexpr uint64_t kSession = 0x1065b08;      // match session object
constexpr uint64_t kNgm = 0x113b760;          // NetGameManager object
constexpr uint64_t kClock = 0x1116180;        // clock object, frame counter at +0x204
constexpr uint64_t kToStringId = 0x10666c8;   // pointer to the string -> string id function
constexpr uint64_t kSymbolLookup = 0xa15e70;
constexpr uint64_t kSpawnerById = 0x64b960;
constexpr uint64_t kSendKillInfo = 0x60eb90;

constexpr uint32_t kNetGamesId = 0x3b9a067d;      // *net-games*
constexpr uint32_t kCaptureZoneKey = 0x7af5d23b;  // spawner data key of the capture zone

// ---- guest memory access (unaligned, little endian) ---------------------------------------------
template <typename T> T peek(const void *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> void poke(void *p, T v) { std::memcpy(p, &v, sizeof v); }
uint8_t *peek_ptr(const void *p) { return peek<uint8_t *>(p); }
uint8_t *at(uint64_t offset) { return image.base + offset; }
uint8_t *ngm() { return at(kNgm); }

// Each message is printed at most 8 times; a per-site counter.
bool may_log(std::atomic<int> &lines) {
    if (lines.load() >= 8) return false;
    return lines.fetch_add(1) < 8;
}

// ---- shared game state --------------------------------------------------------------------------
bool in_multiplayer() { return peek<int32_t>(at(kNetState)) > 1; }

// Read fresh each time: host status may change at any point.
bool is_host() {
    const uint8_t *s = at(kSession);
    return peek<uint64_t>(s + 0x10) == 0 || peek<uint32_t>(s + 0xb00) == peek<uint32_t>(s + 0xb04);
}

// A process handle names a process by slot and unique id; stale handles resolve to nothing.
uint8_t *resolve_handle(const uint8_t *handle) {
    const uint8_t *slot = peek_ptr(handle);
    const uint32_t id = peek<uint32_t>(handle + 8);
    if (!slot || id == 0) return nullptr;
    uint8_t *process = peek_ptr(slot);
    if (!process || peek<uint32_t>(process + 0x44) != id) return nullptr;
    return process;
}

uint8_t *player_tracker_by_net_id(uint32_t net_id) {
    for (unsigned i = 0; i < 10; ++i) {
        uint8_t *tracker = ngm() + 0x10 + uint64_t(i) * 0x2c0;
        if (peek<uint32_t>(tracker + 0x120) == net_id) return tracker;
    }
    return nullptr;
}

uint32_t fnv1a_u32(uint32_t value) {
    uint32_t hash = 0x811c9dc5u;
    for (unsigned i = 0; i < 4; ++i) {
        hash ^= (value >> (8 * i)) & 0xff;
        hash *= 0x01000193u;
    }
    return hash;
}

// The NPC tracker hash map keyed by the NPC's name id.
uint8_t *npc_tracker_by_name(uint32_t name) {
    const uint8_t *buckets = peek_ptr(ngm() + 0x1ba0);
    const int32_t count = peek<int32_t>(ngm() + 0x1bdc);
    if (count <= 0 || !buckets) return nullptr;
    const uint8_t *end = peek_ptr(buckets + uint64_t(count) * 8);
    uint8_t *node = peek_ptr(buckets + uint64_t(fnv1a_u32(name) % uint32_t(count)) * 8);
    while (node && peek<uint32_t>(node + 0x10) != name) node = peek_ptr(node);
    if (!node || node == end) return nullptr;
    return node + 0x18;
}

// The *net-games* record being played, or NULL. Looks the symbol up once per call.
uint8_t *current_net_game() {
    const uint8_t *table = reinterpret_cast<uint8_t *(U2MP_GUEST *)(uint32_t)>(at(kSymbolLookup))(kNetGamesId);
    if (!table) return nullptr;
    const int16_t index = peek<int16_t>(ngm() + 0x1ca0);
    if (index < 0 || index >= peek<int32_t>(table)) return nullptr;
    uint8_t *records = peek_ptr(table + 8);
    if (!records) return nullptr;
    return records + int64_t(index) * 0x90;
}

uint8_t *task_list_of(const uint8_t *record) { return record ? peek_ptr(record + 0x48) : nullptr; }

uint8_t *current_task(const uint8_t *list) {
    if (!list) return nullptr;
    const int32_t index = peek<int32_t>(ngm() + 0x1d6c);
    if (index < 0 || index >= peek<int32_t>(list + 8)) return nullptr;
    uint8_t *tasks = peek_ptr(list + 0x10);
    if (!tasks) return nullptr;
    return tasks + int64_t(index) * 0x28;
}

// A u32 id of the current task, falling back to the list's default task when it is 0.
uint32_t task_id_field(uint64_t field) {
    const uint8_t *list = task_list_of(current_net_game());
    if (!list) return 0;
    if (const uint8_t *task = current_task(list)) {
        if (const uint32_t id = peek<uint32_t>(task + field)) return id;
    }
    const uint8_t *fallback_task = peek_ptr(list);
    return fallback_task ? peek<uint32_t>(fallback_task + field) : 0;
}

// A string id from a spawner's data table (sorted by key), or `fallback`.
uint32_t spawner_string_id(const uint8_t *spawner, uint32_t key, uint32_t fallback) {
    const uint8_t *table = peek_ptr(spawner + 0x68);
    if (!table) return fallback;
    const uint8_t *entries = peek_ptr(table);
    const int32_t count = peek<int32_t>(table + 8);
    if (!entries || count <= 0) return fallback;
    auto key_at = [&](int32_t i) { return peek<uint32_t>(entries + int64_t(i) * 16); };

    // First entry whose key is not below `key`.
    int32_t lo = 0, len = count;
    while (len != 0) {
        const int32_t half = len / 2;
        const int32_t mid = lo + half;
        if (key_at(mid) < key) {
            lo = mid + 1;
            len -= half + 1;
        } else {
            len = half;
        }
    }
    if (lo == count || key_at(lo) != key) return fallback;

    const uint8_t *value = peek_ptr(entries + int64_t(lo) * 16 + 8);
    if (!value) return fallback;
    const uint16_t type = peek<uint16_t>(value);
    const uint8_t *data = peek_ptr(value + 8);
    if (type == 13) return data ? peek<uint32_t>(data) : fallback;
    if (type == 5) {
        using ToIdFn = uint32_t(U2MP_GUEST *)(uint64_t, const char *, int);
        const ToIdFn to_id = peek<ToIdFn>(at(kToStringId));
        if (data && to_id) return to_id(0, peek<const char *>(data), 1);
    }
    return fallback;
}

// Virtual call helpers: the function pointer at vtable + slot * 8, vtable = first 8 bytes.
template <typename Fn> Fn virtual_slot(const uint8_t *object, unsigned slot) {
    return peek<Fn>(peek_ptr(object) + uint64_t(slot) * 8);
}

// ---- the natives --------------------------------------------------------------------------------
U2MP_GUEST uint64_t net_coop_host_script_id(int, const uint64_t *) { return task_id_field(0x1c); }

U2MP_GUEST uint64_t net_coop_script_id(int, const uint64_t *) { return task_id_field(0x20); }

U2MP_GUEST uint64_t net_coop_task(int, const uint64_t *) {
    const uint8_t *task = current_task(task_list_of(current_net_game()));
    return task ? peek<uint32_t>(task) : 0;
}

U2MP_GUEST uint64_t net_player_coop_id(int, const uint64_t *) {
    static std::atomic<int> not_mp{0}, not_coop{0}, no_tracker{0}, bad_index{0};
    if (!in_multiplayer()) {
        if (may_log(not_mp)) log("net-player-coop-id - called outside of multiplayer");
        return 0;
    }
    const uint8_t *record = current_net_game();
    const int32_t type = record ? peek<int32_t>(record + 0x0c) : 0;
    if (type != 4 && type != 8 && type != 9 && type != 12) {
        if (may_log(not_coop)) log("net-player-coop-id - called outside of a coop game");
        return 0;
    }
    const uint8_t *local = peek_ptr(ngm());
    if (!local) {
        if (may_log(no_tracker)) log("net-player-coop-id - no player tracker");
        return 0;
    }
    switch (peek<int32_t>(local + 0x198)) {
    case 0: return 0x9ba01002u;
    case 1: return 0x9ffcfd6cu;
    case 2: return 0x75d3456fu;
    default:
        if (may_log(bad_index)) log("net-player-coop-id - invalid spawner index");
        return 0;
    }
}

U2MP_GUEST uint64_t net_get_capture_zone(int, const uint64_t *) {
    const uint8_t *spawner =
        reinterpret_cast<uint8_t *(U2MP_GUEST *)(void *, int32_t)>(at(kSpawnerById))(ngm(), 0);
    if (!spawner) return 0;
    return spawner_string_id(spawner, kCaptureZoneKey, 0);
}

U2MP_GUEST uint64_t net_get_player_by_net_id(int argc, const uint64_t *argv) {
    static std::atomic<int> not_mp{0}, no_tracker{0}, no_character{0};
    if (!in_multiplayer()) {
        if (may_log(not_mp)) log("net-get-player-by-net-id - called outside of multiplayer");
        return 0;
    }
    if (argc < 1) return 0;
    const uint8_t *tracker = player_tracker_by_net_id(uint32_t(argv[0]));
    if (!tracker) {
        if (may_log(no_tracker)) log("net-get-player-by-net-id - no player tracker");
        return 0;
    }
    const uint8_t *process = resolve_handle(tracker + 0x10);
    if (!process) {
        if (may_log(no_character)) log("net-get-player-by-net-id - tracker has no character character");
        return 0;
    }
    return peek<uint32_t>(process + 0x68);
}

// Remembers who hurt the NPC, for up to three attackers.
void note_attacker(uint8_t *npc_tracker, uint32_t attacker, uint32_t damage) {
    for (unsigned s = 0; s < 3; ++s) {
        uint8_t *id = npc_tracker + 0x30 + 4 * s;
        uint8_t *total = npc_tracker + 0x3c + 4 * s;
        const uint32_t current = peek<uint32_t>(id);
        if (current == attacker) {
            poke<uint32_t>(total, peek<uint32_t>(total) + damage);
            return;
        }
        if (current == 0) {
            poke<uint32_t>(id, attacker);
            poke<uint32_t>(total, damage);
            return;
        }
    }
}

// argv: player net id, npc name id, damage, kill kind.
U2MP_GUEST uint64_t net_apply_npc_fight_damage(int argc, const uint64_t *argv) {
    static std::atomic<int> not_mp{0}, no_tracker{0}, no_player{0}, no_npc_tracker{0}, no_npc{0};
    if (!in_multiplayer()) {
        if (may_log(not_mp)) log("net-apply-npc-fight-damage - called outside of multiplayer");
        return 0;
    }
    if (argc < 4) return 0;
    const uint32_t net_id = uint32_t(argv[0]);
    const uint8_t *tracker = player_tracker_by_net_id(net_id);
    if (!tracker) {
        if (may_log(no_tracker)) log("net-apply-npc-fight-damage - can't find player %u", net_id);
        return 0;
    }
    uint8_t *player = resolve_handle(tracker + 0x10);
    if (!player) {
        if (may_log(no_player)) log("net-apply-npc-fight-damage - can't find player process %u", net_id);
        return 0;
    }
    const uint32_t name = uint32_t(argv[1]);
    uint8_t *npc_tracker = npc_tracker_by_name(name);
    if (!npc_tracker) {
        if (may_log(no_npc_tracker)) log("net-apply-npc-fight-damage - can't npc tracker 0x%08x", name);
        return 0;
    }
    uint8_t *npc = resolve_handle(npc_tracker);
    if (!npc) {
        if (may_log(no_npc)) log("net-apply-npc-fight-damage - no npc 0x%08x", name);
        return 0;
    }
    const uint32_t damage = uint32_t(argv[2]);
    const uint32_t kind = uint32_t(argv[3]);

    if (is_host()) note_attacker(npc_tracker, peek<uint32_t>(tracker + 0x120), damage);

    const uint8_t *health = peek_ptr(npc + 0x1f0);
    int64_t max_health = 0;
    if (health) max_health = virtual_slot<int32_t(U2MP_GUEST *)(const void *)>(health, 10)(health);

    // A client applies only a blow that kills outright; the host applies every blow.
    if (!is_host() && int64_t(damage) < max_health) return 0;

    const auto alive = virtual_slot<uint8_t(U2MP_GUEST *)(void *)>(npc, 74);
    const bool before = alive(npc) != 0;
    virtual_slot<void(U2MP_GUEST *)(void *, int32_t)>(npc, 110)(npc, int32_t(damage));
    const bool after = virtual_slot<uint8_t(U2MP_GUEST *)(void *)>(npc, 74)(npc) != 0;
    if (before == after) return 0;
    if (!is_host()) return 0;

    // The NPC died (or came back) from this blow: tell the clients who did it.
    uint32_t frame = peek<uint32_t>(at(kClock) + 0x204);
    reinterpret_cast<void(U2MP_GUEST *)(uint32_t, void *, void *, const uint32_t *, bool, bool, bool)>(
        at(kSendKillInfo))(kind, player, npc, &frame, false, false, false);
    npc[0x262a] |= 0x02;
    return 0;
}

// ---- install ------------------------------------------------------------------------------------
struct Anchor {
    uint64_t offset;
    std::initializer_list<uint8_t> bytes;
};

// Code that reads the fields and globals above in the way this file assumes.
const Anchor kAnchors[] = {
    {0x668b39, {0x39, 0x94, 0x30, 0x30, 0x01, 0x00, 0x00}},
    {0x6428f6, {0x39, 0x8b, 0x88, 0x01, 0x00, 0x00}},
    {0x642880, {0x8b, 0x44, 0x08, 0x0c}},
    {0x60ecaf, {0x48, 0x8b, 0x09}},
    {0x60ecbb, {0x8b, 0x89, 0x20, 0x01, 0x00, 0x00}},
    {0x668a6c, {0x48, 0x63, 0x8e, 0xdc, 0x1b, 0x00, 0x00}},
    {0x668a7a, {0x48, 0x8b, 0x96, 0xa0, 0x1b, 0x00, 0x00}},
    {0x668a98, {0x39, 0x78, 0x10}},
    {0x668abf, {0x48, 0x8d, 0x40, 0x18}},
    {0x668acc, {0x48, 0xc7, 0x42, 0x24, 0x00, 0x00, 0x00, 0x00}},
    {0x668d4c, {0xff, 0x90, 0x50, 0x02, 0x00, 0x00}},
    {0x25eb1a, {0xff, 0x90, 0x70, 0x03, 0x00, 0x00}},
    {0x61f11a, {0x48, 0x8b, 0x98, 0xf0, 0x01, 0x00, 0x00}},
    {0x61f12b, {0xff, 0x50, 0x50}},
    {0x249ef2, {0x41, 0x80, 0x8f, 0x2a, 0x26, 0x00, 0x00, 0x02}},
    {0x6696d0, {0x44, 0x8b, 0x60, 0x68}},
    {0x669795, {0x48, 0x0f, 0xbf, 0x8a, 0xa0, 0x1c, 0x00, 0x00}},
    {0x6697a1, {0x48, 0x8d, 0x0c, 0xc9, 0x48, 0xc1, 0xe1, 0x04}},
    {0x6697a9, {0x48, 0x8b, 0x4c, 0x08, 0x48}},
    {0x6697b5, {0x48, 0x63, 0x92, 0x6c, 0x1d, 0x00, 0x00}},
    {0x6697c5, {0x3b, 0x51, 0x08}},
    {0x6697d2, {0x48, 0x03, 0x41, 0x10}},
    {0x66418f, {0x48, 0x8d, 0x48, 0x18}},
    {0x669c92, {0x48, 0x83, 0x79, 0x10, 0x00}},
    {0x669c99, {0x48, 0x8b, 0x81, 0x00, 0x0b, 0x00, 0x00}},
    {0x60ec4c, {0x0f, 0x44, 0x81, 0x04, 0x02, 0x00, 0x00}},
    {0x64ba83, {0x48, 0x8b, 0x48, 0x68}},
    {0x20d7cb, {0x83, 0xff, 0x0d}},
    {0x20d7db, {0x83, 0xfa, 0x05}},
    {kSpawnerById, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}},
    {kSendKillInfo, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}},
    {kSymbolLookup, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}},
};

// Instructions whose RIP-relative operand must point at the globals and functions above.
struct RipRef {
    uint64_t at;
    unsigned length, disp_offset;
    uint64_t target;
};
const RipRef kRipRefs[] = {
    {0x669c20, 7, 3, kNgm},        {0x669c50, 7, 2, kNetState},     {0x669c89, 7, 3, kSession},
    {0x60ec45, 7, 3, kClock},      {0x6698fb, 7, 3, kToStringId},   {0x6697e9, 5, 1, kSymbolLookup},
    {0x249eed, 5, 1, kSendKillInfo},
};

struct Native {
    const char *name;
    uint64_t offset;
    std::initializer_list<uint8_t> stub;
    void *host;
};

bool layout_ok() {
    for (const Anchor &a : kAnchors) {
        if (!image.matches(a.offset, a.bytes.begin(), a.bytes.size())) {
            log("natives: code at 0x%llx differs, co-op natives left stubbed", (unsigned long long)a.offset);
            return false;
        }
    }
    for (const RipRef &r : kRipRefs) {
        if (r.at + r.length > image.size) return false;
        const int32_t disp = peek<int32_t>(image.base + r.at + r.disp_offset);
        if (int64_t(r.at + r.length) + disp != int64_t(r.target)) {
            log("natives: 0x%llx does not reference 0x%llx, co-op natives left stubbed", (unsigned long long)r.at,
                (unsigned long long)r.target);
            return false;
        }
    }
    return true;
}

}  // namespace

void install_natives(int &installed) {
    if (!layout_ok()) return;

    static const Native natives[] = {
        {"net-coop-host-script-id", 0x669c40, {0x31, 0xc0, 0xc3, 0x66, 0x66},
         reinterpret_cast<void *>(&net_coop_host_script_id)},
        {"net-coop-script-id", 0x669c30, {0x31, 0xc0, 0xc3, 0x66, 0x66}, reinterpret_cast<void *>(&net_coop_script_id)},
        {"net-player-coop-id", 0x669240, {0x31, 0xc0, 0xc3, 0x66, 0x66}, reinterpret_cast<void *>(&net_player_coop_id)},
        {"net-get-capture-zone", 0x6687d0, {0x31, 0xc0, 0xc3, 0x66, 0x66},
         reinterpret_cast<void *>(&net_get_capture_zone)},
        {"net-apply-npc-fight-damage", 0x668db0, {0x31, 0xc0, 0xc3, 0x66, 0x66},
         reinterpret_cast<void *>(&net_apply_npc_fight_damage)},
        {"net-get-player-by-net-id", 0x6690b0, {0x31, 0xc0, 0xc3, 0x66, 0x66},
         reinterpret_cast<void *>(&net_get_player_by_net_id)},
        {"net-coop-task", 0x6697e0,
         {0x55, 0x48, 0x89, 0xe5, 0xbf, 0x7d, 0x06, 0x9a, 0x3b, 0xe8, 0x82, 0xc6, 0x3a, 0x00, 0x31, 0xc0, 0x5d, 0xc3},
         reinterpret_cast<void *>(&net_coop_task)},
    };
    for (const Native &n : natives) {
        if (!image.matches(n.offset, n.stub.begin(), n.stub.size())) {
            log("%s at 0x%llx is not the expected stub, left alone", n.name, (unsigned long long)n.offset);
            continue;
        }
        if (replace_function(n.offset, 5, n.host, n.name)) installed += 1;
    }
}

}  // namespace u2mp
