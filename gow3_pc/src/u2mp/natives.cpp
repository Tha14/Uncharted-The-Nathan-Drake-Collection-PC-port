// u2mp: the multiplayer / co-op DC script natives that Uncharted 2 PS4 1.02 stubbed, ported from the
// PS3 1.10 originals (u2mp/docs/FUNCTIONS-MATCH-GAMEPLAY.md section 3 blocker 2 and section 8).
//
// PS4 DC native calling convention (read from the intact natives at 0x667d40..0x669c80):
//   uint64_t native(int argc, const uint64_t *argv)            (System V: edi = argc, rsi = argv)
//   - argv is an array of 8-byte DC values; a native reads the low 32 bits of a slot for an int,
//     a string id or a net id (`mov ebx, [rsi]`, `[rsi+0x8]`, `[rsi+0x10]`, `[rsi+0x18]`).
//     Natives that need it check argc themselves (net-update-fight-ref 0x667f80: `test edi, edi`,
//     `cmp eax, 2/3/4`); most just read the slots.
//   - The result is the DC value itself, returned in rax: a pointer (net-current-net-task 0x669780),
//     a bool zero-extended (`movzx eax, al`), an int sign-extended (`cdqe`, `movsxd rbx, [..]`), a
//     float in the low 32 bits (net-get-booster-amount 0x668820). Natives with nothing to return
//     return 0. No helper boxes or unboxes values; there is no context argument.
//   - PS3 1.10 used the same table with an explicit result slot (r3 = result pointer, r4 = argc,
//     r5 = argv of 4-byte slots); every argv[i] of the PS3 bodies is PS4 argv[i] here.
//   - Errors go to the game's script log (PS4 0xa35970, PS3 0x7ceb10); we print the PS3 messages
//     with u2mp::log, a few times each.
//
// What the PS3 bodies use, and where it is on PS4 (each checked in install_natives() against code
// that survives in the PS4 image; see the anchors there):
//   IsMultiplayer (PS3 0x4384b0)      NET_SM 0x14ce97c > 1
//   IsHost        (PS3 0x494ea8)      match session 0x1065b08: context +0x10 == 0, or me +0xb00 ==
//                                     host +0xb04 (inlined in net-is-host 0x669c80)
//   g_netGameManager                  0x113b760 (PS3 0xf1dc80)
//     local player tracker            +0x00 (pointer)
//     player trackers [10]            +0x10 + i*0x2c0 (PS3 +0x10 + i*0x270)
//       process handle                tracker+0x10: {slot pointer, id u32 at +0x08}; the handle is
//                                     valid when *slot != 0 and (*slot)+0x44 == id (PS3 +0x38)
//       net id                        tracker+0x120 (PS3 +0x118)
//       coop index                    tracker+0x198 (PS3 +0x184)
//     NetNpcTracker map (by name)     buckets +0x1ba0, bucket count +0x1bdc; FNV-1a 32 of the 4
//                                     little-endian bytes of the name id; node {next +0, key +0x10,
//                                     tracker +0x18}; end marker = buckets[count]
//       NetNpcTracker                 {npc handle +0 / +8, owner +0x24/+0x28, attacker net ids[3]
//                                     +0x30, damage[3] +0x3c} (PS3 {+0/+4, +0x1c/+0x20, +0x28, +0x34})
//     current net game index (s16)    +0x1ca0 (PS3 +0x18ec)
//     current net task index          +0x1d6c (PS3 +0x19b0)
//   *net-games* (sid 0x3b9a067d)      lookup 0xa15e70 -> {count, records}; record stride 0x90 (PS3
//                                     0x80); record+0x0c game type; record+0x48 task list
//     task list                       {default task pointer +0, count +8, tasks +0x10}; task stride
//                                     0x28 (PS3 0x20): +0x00 name id, +0x08 pointer, +0x18 float,
//                                     +0x1c host script id, +0x20 script id (PS3 +0, +8, +0x14,
//                                     +0x18, +0x1c)
//   process name id                   process+0x68 (PS3 +0x4c)
//   character health component        character+0x1f0 (PS3 +0x15c); virtual slot 10 = max health
//   character virtual slots           74 (+0x250) alive test, 110 (+0x370) take damage(int)
//   npc "killed by a player" flag     byte npc+0x262a |= 0x02 (PS3: u64 at +0x1878 |= 1 << 46)
//   frame counter                     u32 0x1116180+0x204 (PS3 0xe60a00+0x1cc)
//   capture-zone spawner by id        0x64b960(ngm, id) (PS3 0x49dd48)
//   spawner data table                spawner+0x68 -> {entries, count}; entry {key u32, value +8};
//                                     value {type u16, data pointer +8}; type 13 = string id,
//                                     type 5 = string (converted by the function at [0x10666c8])
//   NetEventKillInfoClient sender     0x60eb90(kind, attacker, victim, &frame, b, b, b) (PS3
//                                     0x48a3c0, whose initialiser 0x472ce0 PS4 inlined into it)
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

// ---- guest addresses (PS4 1.02 image offsets) -----------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c, kNgm = 0x113b760, kMatch = 0x1065b08, kClock = 0x1116180,
                   kStringToSid = 0x10666c8;
constexpr uint64_t fLookupSymbol = 0xa15e70, fCaptureZoneSpawner = 0x64b960, fSendKillInfoClient = 0x60eb90;

// NetGameManager fields
constexpr uint64_t NGM_LOCAL = 0x00, NGM_TRACKERS = 0x10, NGM_TRACKER_SIZE = 0x2c0, NGM_NPC_BUCKETS = 0x1ba0,
                   NGM_NPC_BUCKET_COUNT = 0x1bdc, NGM_GAME = 0x1ca0, NGM_TASK = 0x1d6c;
constexpr int kTrackers = 10;
constexpr uint64_t T_HANDLE = 0x10, T_NETID = 0x120, T_COOP = 0x198;
constexpr uint64_t NT_HANDLE = 0x00, NT_ATTACKERS = 0x30, NT_DAMAGE = 0x3c;
constexpr uint64_t P_ID = 0x44, P_NAME = 0x68, C_HEALTH = 0x1f0, C_FLAGS2 = 0x262a;
constexpr uint64_t VT_ALIVE = 74, VT_TAKE_DAMAGE = 110, VT_HEALTH_MAX = 10;
constexpr uint32_t kNetGames = 0x3b9a067d;
constexpr uint64_t REC_SIZE = 0x90, REC_TYPE = 0x0c, REC_TASKS = 0x48;
constexpr uint64_t TL_DEFAULT = 0x00, TL_COUNT = 0x08, TL_TASKS = 0x10, TASK_SIZE = 0x28,
                   TASK_HOST_SCRIPT = 0x1c, TASK_SCRIPT = 0x20;

// the stubbed natives (net-coop-task is cut to "look up *net-games*, return 0")
constexpr uint64_t nCoopHostScriptId = 0x669c40, nCoopScriptId = 0x669c30, nPlayerCoopId = 0x669240,
                   nGetCaptureZone = 0x6687d0, nApplyNpcFightDamage = 0x668db0,
                   nGetPlayerByNetId = 0x6690b0, nCoopTask = 0x6697e0;

using LookupSymbolFn = uint8_t *(U2MP_GUEST *)(uint32_t sid);
using CaptureZoneSpawnerFn = uint8_t *(U2MP_GUEST *)(void *ngm, int32_t id);
using SendKillInfoClientFn = void(U2MP_GUEST *)(uint32_t kind, void *attacker, void *victim, const uint32_t *frame,
                                                bool a, bool b, bool c);
using StringToSidFn = uint32_t(U2MP_GUEST *)(uint64_t, const char *, int);
using IntVirtual = int32_t(U2MP_GUEST *)(void *);
using BoolVirtual = bool(U2MP_GUEST *)(void *);
using DamageVirtual = void(U2MP_GUEST *)(void *, int32_t);

template <typename T> T rd(const uint8_t *p, uint64_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
template <typename T> void wr(uint8_t *p, uint64_t off, T v) { std::memcpy(p + off, &v, sizeof v); }
uint8_t *ptr(const uint8_t *p, uint64_t off) { return rd<uint8_t *>(p, off); }
uint8_t *ngm() { return image.at<uint8_t>(kNgm); }
template <typename F> F guest_fn(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }
template <typename F> F virt(uint8_t *object, uint64_t slot) { return reinterpret_cast<F>(rd<void *>(ptr(object, 0), slot * 8)); }

// The PS3 script-error messages, at most a few times each (scripts call these every frame).
#define NATIVE_LOG(...)                       \
    do {                                      \
        static int logged_ = 0;               \
        if (logged_ < 8) {                    \
            ++logged_;                        \
            log(__VA_ARGS__);                 \
        }                                     \
    } while (0)

bool is_multiplayer() { return *image.at<int32_t>(kNetState) > 1; }

bool is_host() {
    const uint8_t *match = image.at<uint8_t>(kMatch);
    return rd<uint64_t>(match, 0x10) == 0 || rd<uint32_t>(match, 0xb00) == rd<uint32_t>(match, 0xb04);
}

// A process handle {slot pointer, id}: the process, or null when it is gone.
uint8_t *resolve(const uint8_t *handle) {
    uint8_t **slot = rd<uint8_t **>(handle, 0);
    const uint32_t id = rd<uint32_t>(handle, 8);
    if (!slot || !id) return nullptr;
    uint8_t *process = *slot;
    return process && rd<uint32_t>(process, P_ID) == id ? process : nullptr;
}

uint8_t *tracker(int i) { return ngm() + NGM_TRACKERS + uint64_t(i) * NGM_TRACKER_SIZE; }

// NetGameManager::GetPlayerTrackerByNetId (PS3 0x4918e8; PS4 inlines it, e.g. 0x668b30).
uint8_t *tracker_by_net_id(uint32_t net_id) {
    for (int i = 0; i < kTrackers; ++i)
        if (rd<uint32_t>(tracker(i), T_NETID) == net_id) return tracker(i);
    return nullptr;
}

// NetGameManager::GetNpcTracker(name) (PS3 0x4935d0; PS4 inlines it, e.g. 0x668a37..0x668ac7).
uint8_t *npc_tracker(uint32_t name) {
    uint8_t *m = ngm();
    const int32_t count = rd<int32_t>(m, NGM_NPC_BUCKET_COUNT);
    uint8_t **buckets = rd<uint8_t **>(m, NGM_NPC_BUCKETS);
    if (count <= 0 || !buckets) return nullptr;
    uint32_t hash = 0x811c9dc5u;
    for (int i = 0; i < 4; ++i) hash = (hash ^ ((name >> (8 * i)) & 0xff)) * 0x1000193u;
    uint8_t *node = buckets[hash % uint32_t(count)];
    while (node && rd<uint32_t>(node, 0x10) != name) node = ptr(node, 0);
    if (!node || node == buckets[count]) return nullptr;
    return node + 0x18;
}

// The current *net-games* record (PS3: inline in 0x49e368 / 0x49e2b0 / 0x49f058 / 0x4a0bcc).
uint8_t *net_game_record() {
    uint8_t *table = guest_fn<LookupSymbolFn>(fLookupSymbol)(kNetGames);
    if (!table) return nullptr;
    const int16_t index = rd<int16_t>(ngm(), NGM_GAME);
    if (index < 0 || index >= rd<int32_t>(table, 0)) return nullptr;
    uint8_t *records = ptr(table, 8);
    return records ? records + uint64_t(index) * REC_SIZE : nullptr;
}

// The current task of the net game's task list (PS3 0x49f058), or null.
uint8_t *current_task(uint8_t **list_out = nullptr) {
    uint8_t *record = net_game_record();
    uint8_t *list = record ? ptr(record, REC_TASKS) : nullptr;
    if (list_out) *list_out = list;
    if (!list) return nullptr;
    const int32_t index = rd<int32_t>(ngm(), NGM_TASK);
    uint8_t *tasks = ptr(list, TL_TASKS);
    if (index < 0 || index >= rd<int32_t>(list, TL_COUNT) || !tasks) return nullptr;
    return tasks + uint64_t(index) * TASK_SIZE;
}

// A string id field of the current task, falling back to the list's default task when it is 0
// (PS3 0x49e368 for +0x18, 0x49e2b0 for +0x1c). PS3 indexes the task array without a bounds check;
// out of range we use the default task.
uint32_t task_id_field(uint64_t field) {
    uint8_t *list = nullptr;
    uint8_t *task = current_task(&list);
    if (!list) return 0;
    if (task) {
        const uint32_t v = rd<uint32_t>(task, field);
        if (v) return v;
    }
    uint8_t *fallback = ptr(list, TL_DEFAULT);
    return fallback ? rd<uint32_t>(fallback, field) : 0;
}

// spawner->GetData<StringId>(key, default) (PS3 0x2be1c + 0x7b3d0 + 0x7b264; PS4 inline, e.g.
// 0x20d780..0x20d7fa).
uint32_t spawner_sid(const uint8_t *spawner, uint32_t key, uint32_t fallback) {
    const uint8_t *table = ptr(spawner, 0x68);
    if (!table) return fallback;
    const uint8_t *entries = ptr(table, 0);
    const int32_t count = rd<int32_t>(table, 8);
    if (!entries || count <= 0) return fallback;
    int32_t lo = 0, len = count;
    while (len > 0) {
        const int32_t half = len / 2, mid = lo + half;
        if (rd<uint32_t>(entries, uint64_t(mid) * 16) < key) {
            lo = mid + 1;
            len -= half + 1;
        } else {
            len = half;
        }
    }
    if (lo == count || rd<uint32_t>(entries, uint64_t(lo) * 16) != key) return fallback;
    const uint8_t *value = ptr(entries, uint64_t(lo) * 16 + 8);
    if (!value) return fallback;
    const uint8_t *data = ptr(value, 8);
    switch (rd<uint16_t>(value, 0)) {
    case 13:
        return data ? rd<uint32_t>(data, 0) : fallback;
    case 5: {
        auto convert = *image.at<StringToSidFn>(kStringToSid);
        return data && convert ? convert(0, rd<const char *>(data, 0), 1) : fallback;
    }
    default:
        return fallback;
    }
}

uint64_t arg(const uint64_t *argv, int i) { return uint32_t(argv[i]); }

// ---- the natives --------------------------------------------------------------------------------

// net-coop-host-script-id (PS3 DcNetCoopHostScriptId 0x4ccab8)
U2MP_GUEST uint64_t net_coop_host_script_id(int, const uint64_t *) { return task_id_field(TASK_HOST_SCRIPT); }

// net-coop-script-id (PS3 DcNetCoopScriptId 0x4cd8ac)
U2MP_GUEST uint64_t net_coop_script_id(int, const uint64_t *) { return task_id_field(TASK_SCRIPT); }

// net-coop-task (PS3 DcNetCoopTask 0x4cd850): the current task's name id.
U2MP_GUEST uint64_t net_coop_task(int, const uint64_t *) {
    const uint8_t *task = current_task();
    return task ? rd<uint32_t>(task, 0) : 0;
}

// net-player-coop-id (PS3 DcNetPlyaerCoopId 0x4cd6e8): the local player's coop id.
U2MP_GUEST uint64_t net_player_coop_id(int, const uint64_t *) {
    if (!is_multiplayer()) {
        NATIVE_LOG("net-player-coop-id - called outside of multiplayer");
        return 0;
    }
    const uint8_t *record = net_game_record();
    const int32_t type = record ? rd<int32_t>(record, REC_TYPE) : 0;
    if (!(type == 4 || type == 8 || type == 9 || type == 12)) {   // PS3 0x4a0bcc, PS4 0x642880
        NATIVE_LOG("net-player-coop-id - called outside of a coop game");
        return 0;
    }
    const uint8_t *local = ptr(ngm(), NGM_LOCAL);
    if (!local) {
        NATIVE_LOG("net-player-coop-id - no player tracker");
        return 0;
    }
    switch (rd<int32_t>(local, T_COOP)) {
    case 0: return 0x9ba01002u;
    case 1: return 0x9ffcfd6cu;
    case 2: return 0x75d3456fu;
    default:
        NATIVE_LOG("net-player-coop-id - invalid spawner index");
        return 0;
    }
}

// net-get-capture-zone (PS3 DcGetCaptureZone 0x4cd66c): tag 0x7af5d23b of capture-zone spawner 0.
U2MP_GUEST uint64_t net_get_capture_zone(int, const uint64_t *) {
    const uint8_t *spawner = guest_fn<CaptureZoneSpawnerFn>(fCaptureZoneSpawner)(ngm(), 0);
    return spawner ? spawner_sid(spawner, 0x7af5d23bu, 0) : 0;
}

// net-get-player-by-net-id (PS3 DCNetGetPlayerByNetId 0x4ce6f0): the player's process name id.
U2MP_GUEST uint64_t net_get_player_by_net_id(int argc, const uint64_t *argv) {
    if (!is_multiplayer()) {
        NATIVE_LOG("net-get-player-by-net-id - called outside of multiplayer");
        return 0;
    }
    if (argc < 1) return 0;
    const uint8_t *t = tracker_by_net_id(uint32_t(arg(argv, 0)));
    if (!t) {
        NATIVE_LOG("net-get-player-by-net-id - no player tracker");
        return 0;
    }
    const uint8_t *process = resolve(t + T_HANDLE);
    if (!process) {
        NATIVE_LOG("net-get-player-by-net-id - tracker has no character character");
        return 0;
    }
    return rd<uint32_t>(process, P_NAME);
}

// NetNpcTracker::AddDamage(net id, amount) (PS3 0x49177c): up to three attackers per NPC.
void npc_add_damage(uint8_t *npc_tracker, uint32_t net_id, int32_t amount) {
    for (int i = 0; i < 3; ++i) {
        const uint32_t id = rd<uint32_t>(npc_tracker, NT_ATTACKERS + 4 * i);
        if (id == net_id) {
            wr<int32_t>(npc_tracker, NT_DAMAGE + 4 * i, rd<int32_t>(npc_tracker, NT_DAMAGE + 4 * i) + amount);
            return;
        }
        if (id == 0) {
            wr<uint32_t>(npc_tracker, NT_ATTACKERS + 4 * i, net_id);
            wr<int32_t>(npc_tracker, NT_DAMAGE + 4 * i, amount);
            return;
        }
    }
}

// net-apply-npc-fight-damage player-net-id npc-name damage kind
// (PS3 DCNetApplyNpcFightDamage 0x4cdc90).
U2MP_GUEST uint64_t net_apply_npc_fight_damage(int argc, const uint64_t *argv) {
    if (!is_multiplayer()) {
        NATIVE_LOG("net-apply-npc-fight-damage - called outside of multiplayer");
        return 0;
    }
    if (argc < 4) return 0;
    const uint32_t net_id = uint32_t(arg(argv, 0));
    uint8_t *t = tracker_by_net_id(net_id);
    if (!t) {
        NATIVE_LOG("net-apply-npc-fight-damage - can't find player %u", net_id);
        return 0;
    }
    uint8_t *player = resolve(t + T_HANDLE);
    if (!player) {
        NATIVE_LOG("net-apply-npc-fight-damage - can't find player process %u", net_id);
        return 0;
    }
    const uint32_t name = uint32_t(arg(argv, 1));
    uint8_t *nt = npc_tracker(name);
    if (!nt) {
        NATIVE_LOG("net-apply-npc-fight-damage - can't npc tracker 0x%08x", name);
        return 0;
    }
    uint8_t *npc = resolve(nt + NT_HANDLE);
    if (!npc) {
        NATIVE_LOG("net-apply-npc-fight-damage - no npc 0x%08x", name);
        return 0;
    }
    const uint32_t damage = uint32_t(arg(argv, 2)), kind = uint32_t(arg(argv, 3));

    if (is_host()) npc_add_damage(nt, rd<uint32_t>(t, T_NETID), int32_t(damage));
    uint8_t *health = ptr(npc, C_HEALTH);
    const int64_t max_health = health ? virt<IntVirtual>(health, VT_HEALTH_MAX)(health) : 0;
    // a client only applies damage that kills outright; the host applies everything
    if (!is_host() && int64_t(damage) < max_health) return 0;
    const bool alive_before = virt<BoolVirtual>(npc, VT_ALIVE)(npc);
    virt<DamageVirtual>(npc, VT_TAKE_DAMAGE)(npc, int32_t(damage));
    if (virt<BoolVirtual>(npc, VT_ALIVE)(npc) == alive_before || !is_host()) return 0;
    const uint32_t frame = rd<uint32_t>(image.at<uint8_t>(kClock), 0x204);
    guest_fn<SendKillInfoClientFn>(fSendKillInfoClient)(kind, player, npc, &frame, false, false, false);
    npc[C_FLAGS2] |= 0x02;
    return 0;
}

// ---- install --------------------------------------------------------------------------------------

struct Anchor {
    uint64_t at;
    unsigned char bytes[8];
    uint8_t length;
};

// Short instruction encodings of surviving PS4 code that pin each layout fact used above.
const Anchor kAnchors[] = {
    {0x668b39, {0x39, 0x94, 0x30, 0x30, 0x01, 0x00, 0x00}, 7},        // cmp [ngm+i*0x2c0+0x130], net id
    {0x6428f6, {0x39, 0x8b, 0x88, 0x01, 0x00, 0x00}, 6},              // coop index, tracker+0x198
    {0x642880, {0x8b, 0x44, 0x08, 0x0c}, 4},                          // record+0x0c game type
    {0x60ecaf, {0x48, 0x8b, 0x09}, 3},                                // local tracker = [ngm]
    {0x60ecbb, {0x8b, 0x89, 0x20, 0x01, 0x00, 0x00}, 6},              // ... its net id +0x120
    {0x668a6c, {0x48, 0x63, 0x8e, 0xdc, 0x1b, 0x00, 0x00}, 7},        // npc map bucket count
    {0x668a7a, {0x48, 0x8b, 0x96, 0xa0, 0x1b, 0x00, 0x00}, 7},        // npc map buckets
    {0x668a98, {0x39, 0x78, 0x10}, 3},                                // node key +0x10
    {0x668abf, {0x48, 0x8d, 0x40, 0x18}, 4},                          // node value +0x18
    {0x668acc, {0x48, 0xc7, 0x42, 0x24, 0x00, 0x00, 0x00, 0x00}, 8},  // npc tracker owner +0x24
    {0x668d4c, {0xff, 0x90, 0x50, 0x02, 0x00, 0x00}, 6},              // npc virtual 74
    {0x25eb1a, {0xff, 0x90, 0x70, 0x03, 0x00, 0x00}, 6},              // character virtual 110 (damage)
    {0x61f11a, {0x48, 0x8b, 0x98, 0xf0, 0x01, 0x00, 0x00}, 7},        // health component +0x1f0
    {0x61f12b, {0xff, 0x50, 0x50}, 3},                                // ... its virtual 10 (max health)
    {0x249ef2, {0x41, 0x80, 0x8f, 0x2a, 0x26, 0x00, 0x00, 0x02}, 8},  // npc flag after the kill sender
    {0x6696d0, {0x44, 0x8b, 0x60, 0x68}, 4},                          // process name +0x68
    {0x669795, {0x48, 0x0f, 0xbf, 0x8a, 0xa0, 0x1c, 0x00, 0x00}, 8},  // ngm+0x1ca0 net game index
    {0x6697a1, {0x48, 0x8d, 0x0c, 0xc9, 0x48, 0xc1, 0xe1, 0x04}, 8},  // record stride 0x90
    {0x6697a9, {0x48, 0x8b, 0x4c, 0x08, 0x48}, 5},                    // record+0x48 task list
    {0x6697b5, {0x48, 0x63, 0x92, 0x6c, 0x1d, 0x00, 0x00}, 7},        // ngm+0x1d6c task index
    {0x6697c5, {0x3b, 0x51, 0x08}, 3},                                // task count +0x08
    {0x6697d2, {0x48, 0x03, 0x41, 0x10}, 4},                          // tasks +0x10 (stride 0x28)
    {0x66418f, {0x48, 0x8d, 0x48, 0x18}, 4},                          // task float +0x18 (PS3 +0x14)
    {0x669c92, {0x48, 0x83, 0x79, 0x10, 0x00}, 5},                    // IsHost: match context
    {0x669c99, {0x48, 0x8b, 0x81, 0x00, 0x0b, 0x00, 0x00}, 7},        // IsHost: me/host
    {0x60ec4c, {0x0f, 0x44, 0x81, 0x04, 0x02, 0x00, 0x00}, 7},        // frame counter +0x204
    {0x64ba83, {0x48, 0x8b, 0x48, 0x68}, 4},                          // spawner data table +0x68
    {0x20d7cb, {0x83, 0xff, 0x0d}, 3},                                // data type 13 = string id
    {0x20d7db, {0x83, 0xfa, 0x05}, 3},                                // data type 5 = string
    {0x64b960, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}, 8},  // capture-zone spawner
    {0x60eb90, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}, 8},  // kill-info-client sender
    {0xa15e70, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}, 8},  // symbol lookup
};

// RIP-relative operands that must name the globals above: {instruction, length, disp offset, target}.
struct RipRef {
    uint64_t at;
    uint8_t length, disp;
    uint64_t target;
};
const RipRef kRipRefs[] = {
    {0x669c20, 7, 3, kNgm},          // lea rax, g_netGameManager
    {0x669c50, 7, 2, kNetState},     // cmp NET_SM, 2
    {0x669c89, 7, 3, kMatch},        // lea rcx, match session
    {0x60ec45, 7, 3, kClock},        // lea rcx, clock
    {0x6698fb, 7, 3, kStringToSid},  // lea rax, string -> id function pointer
    {0x6697e9, 5, 1, fLookupSymbol}, // net-coop-task's call of the symbol lookup
    {0x249eed, 5, 1, fSendKillInfoClient},
};

bool layout_matches() {
    for (const Anchor &a : kAnchors)
        if (!image.matches(a.at, a.bytes, a.length)) {
            log("natives: code at 0x%llx differs, co-op natives left stubbed", (unsigned long long)a.at);
            return false;
        }
    for (const RipRef &r : kRipRefs) {
        if (r.at + r.length > image.size) return false;
        int32_t disp;
        std::memcpy(&disp, image.base + r.at + r.disp, 4);
        if (int64_t(r.at + r.length) + disp != int64_t(r.target)) {
            log("natives: 0x%llx does not reference 0x%llx, co-op natives left stubbed",
                (unsigned long long)r.at, (unsigned long long)r.target);
            return false;
        }
    }
    return true;
}

}  // namespace

void install_natives(int &installed) {
    if (!layout_matches()) return;
    // xor eax, eax; ret; then the alignment padding
    static const unsigned char ret0[] = {0x31, 0xc0, 0xc3, 0x66, 0x66};
    // push rbp; mov rbp, rsp; mov edi, *net-games*; call lookup; xor eax, eax; pop rbp; ret
    static const unsigned char coop_task[] = {0x55, 0x48, 0x89, 0xe5, 0xbf, 0x7d, 0x06, 0x9a, 0x3b,
                                              0xe8, 0x82, 0xc6, 0x3a, 0x00, 0x31, 0xc0, 0x5d, 0xc3};
    struct Native {
        uint64_t at;
        const unsigned char *body;
        size_t length;
        void *fn;
        const char *name;
    };
    const Native natives[] = {
        {nCoopHostScriptId, ret0, sizeof ret0, reinterpret_cast<void *>(&net_coop_host_script_id),
         "net-coop-host-script-id"},
        {nCoopScriptId, ret0, sizeof ret0, reinterpret_cast<void *>(&net_coop_script_id), "net-coop-script-id"},
        {nPlayerCoopId, ret0, sizeof ret0, reinterpret_cast<void *>(&net_player_coop_id), "net-player-coop-id"},
        {nGetCaptureZone, ret0, sizeof ret0, reinterpret_cast<void *>(&net_get_capture_zone),
         "net-get-capture-zone"},
        {nApplyNpcFightDamage, ret0, sizeof ret0, reinterpret_cast<void *>(&net_apply_npc_fight_damage),
         "net-apply-npc-fight-damage"},
        {nGetPlayerByNetId, ret0, sizeof ret0, reinterpret_cast<void *>(&net_get_player_by_net_id),
         "net-get-player-by-net-id"},
        {nCoopTask, coop_task, sizeof coop_task, reinterpret_cast<void *>(&net_coop_task), "net-coop-task"},
    };
    for (const Native &n : natives) {
        if (!image.matches(n.at, n.body, n.length)) {
            log("%s at 0x%llx is not the expected stub, left alone", n.name, (unsigned long long)n.at);
            continue;
        }
        if (replace_function(n.at, 5, n.fn, n.name)) ++installed;
    }
}

}  // namespace u2mp
