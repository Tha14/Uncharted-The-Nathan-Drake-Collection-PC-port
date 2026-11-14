// u2mp: net-event serializers for the event types whose Read/Write bodies the PS4 1.02 build
// reduced to a bare `ret`, plus the NpcSpawn Apply that was stripped the same way.
//
// Without these, the affected events travel the network with an empty payload and a received
// NPC spawn does nothing. Each port supplies working bodies for the PS4 payload layout and is
// hooked in by pointing the event's vtable slot at the new host function; the `ret` stub itself
// stays where it is.
//
// Reimplemented from the behaviour description u2mp/docs/spec/events.md.
#include "events.h"

#include <climits>
#include <cmath>
#include <cstring>

// The wire values must be bit-exact with the other peers: every float operation is rounded on
// its own, never fused.
#pragma clang fp contract(off)

namespace u2mp::events {

Game game;

namespace {

// ---------------------------------------------------------------------------------------------
// Small helpers

template <typename T> T load(const void *base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(base) + off, sizeof v);
    return v;
}
template <typename T> void store(void *base, size_t off, T v) {
    std::memcpy(static_cast<uint8_t *>(base) + off, &v, sizeof v);
}

template <typename Fn> Fn guest(uint64_t offset) { return reinterpret_cast<Fn>(image.base + offset); }

// Image facts (offsets into the PS4 1.02 image).
constexpr uint64_t kFrameClock = 0x1116384;         // s32, 300 ticks per second
constexpr uint64_t kNetGameManager = 0x113b760;
constexpr uint64_t kGameModeIndex = kNetGameManager + 0x1ca0;   // s16
constexpr uint64_t kLookupPointer = 0xa15e70;
constexpr uint32_t kGameModeTableId = 0x3b9a067d;
constexpr uint64_t kLookupProcess = 0x9bbe40;
constexpr uint64_t kSpawnerManager = 0x1130390;
constexpr uint64_t kLookupSpawner = 0x56c7b0;
constexpr uint64_t kSpawnParamsInit = 0x233600;
constexpr uint64_t kSpawnParamsSetSpawner = 0x235360;
constexpr uint64_t kSpawnerLocator = 0x4bdfe0;
constexpr uint64_t kSpawnNpc = 0x4be330;
constexpr uint64_t kAddNetNpcTracker = 0x644fa0;

constexpr size_t kPayload = 0x0c;   // first payload byte; the header before it is never touched

// Float constants of the wire encodings (exact single-precision values).
constexpr float kTimeIn = 0.006666666828095913f;        // 1/150
constexpr float kClockScale = 0.0033333334140479565f;   // 1/300
constexpr float kTimeHalfWrap = 6.826666831970215f;     // 1024/150
constexpr float kTimeWrap = 13.65333366394043f;         // 2048/150
constexpr float kTimeOut = 150.0f;
constexpr float kPosIn = 0.001953125f;                  // 2^-9
constexpr float kPosOut = 512.0f;
constexpr float kQuatIn = 0.00024420025874860585f;      // 1/4095
constexpr float kQuatOut = 4095.0f;
constexpr float kQuatPresent = 9.999999747378752e-05f;  // 1e-4

// x86 cvttss2si semantics: truncation, and the "integer indefinite" value when out of range.
int64_t to_s64(float p) {
    if (!(p >= -9223372036854775808.0f && p < 9223372036854775808.0f)) return INT64_MIN;
    return int64_t(p);
}
int32_t to_s32(float p) {
    if (!(p >= -2147483648.0f && p < 2147483648.0f)) return INT32_MIN;
    return int32_t(p);
}

// ---------------------------------------------------------------------------------------------
// Reading. Once one field does not fit, every later field is skipped; the caller then puts the
// payload back into its factory state.

struct Reader {
    BitStream &bs;
    bool ok = true;

    bool raw(uint32_t &v, unsigned n) {
        if (!ok) return false;
        ok = read_bits(bs, v, n);
        return ok;
    }
    // N bits (or a whole U32 / F32 bit pattern) stored as a u32 at `off`.
    void u32(void *ev, size_t off, unsigned n = 32) {
        uint32_t v;
        if (raw(v, n)) store<uint32_t>(ev, off, v);
    }
    void u16(void *ev, size_t off, unsigned n) {
        uint32_t v;
        if (raw(v, n)) store<uint16_t>(ev, off, uint16_t(v));
    }
    void flag(void *ev, size_t off) {
        uint32_t v;
        if (raw(v, 1)) store<uint8_t>(ev, off, v ? 1 : 0);
    }
    // 11-bit time, moved forward by whole wraps until it sits close behind the frame clock.
    void time(void *ev, size_t off) {
        uint32_t v;
        if (!raw(v, 11)) return;
        float t = float(v) * kTimeIn;
        if (game.frame_clock) {
            const float now = float(*game.frame_clock) * kClockScale;
            // One rounded addition per wrap: a closed form would round differently.
            while (now - t > kTimeHalfWrap) t = t + kTimeWrap;
        }
        store<float>(ev, off, t);
    }
    // Three 18-bit signed components; y is stored a second time in the fourth float.
    void position(void *ev, size_t off) {
        float c[3];
        for (float &f : c) {
            uint32_t v;
            if (!raw(v, 18)) return;
            const int32_t s = int32_t(v << 14) >> 14;
            f = float(s) * kPosIn;
        }
        store<float>(ev, off, c[0]);
        store<float>(ev, off + 4, c[1]);
        store<float>(ev, off + 8, c[2]);
        store<float>(ev, off + 12, c[1]);
    }
    // Position, then a quaternion whose near-zero components were left off the wire.
    void locator(void *ev, size_t off) {
        position(ev, off);
        float q[4];
        for (float &c : q) {
            uint32_t present, v = 4095;   // absent components decode to exactly 0
            if (!raw(present, 1)) return;
            if (present && !raw(v, 13)) return;
            c = float(v) * kQuatIn;
            c = c + (-1.0f);
        }
        const float s = ((q[0] * q[0] + q[1] * q[1]) + q[2] * q[2]) + q[3] * q[3];
        if (s > 0.0f) {
            const float r = 1.0f / std::sqrt(s);
            for (float &c : q) c = c * r;
        } else {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
        }
        for (int i = 0; i < 4; ++i) store<float>(ev, off + 0x10 + 4 * i, q[i]);
    }
};

// Factory state of a payload: all zero, except TerritoryOwn whose +0x10 starts as all ones.
void reset_payload(void *ev, int type, uint32_t size) {
    std::memset(static_cast<uint8_t *>(ev) + kPayload, 0, size - kPayload);
    if (type == 49) store<uint32_t>(ev, 0x10, 0xffffffffu);
}

// ---------------------------------------------------------------------------------------------
// Writing. A field past maxBits is dropped by write_bits (the cursor still advances); we say so
// once per process so a too-small stream shows up in the log without flooding it.

bool overflow_reported = false;

struct Writer {
    BitStream &bs;

    void raw(uint32_t v, unsigned n) {
        if (write_bits(bs, v, n) || overflow_reported) return;
        overflow_reported = true;
        log("net event: a Write ran past the BitStream's maxBits (fields dropped)");
    }
    void u32(const void *ev, size_t off, unsigned n = 32) { raw(load<uint32_t>(ev, off), n); }
    void u16(const void *ev, size_t off, unsigned n) { raw(load<uint16_t>(ev, off), n); }
    void flag(const void *ev, size_t off) { raw(load<uint8_t>(ev, off) & 1u, 1); }
    void time(const void *ev, size_t off) {
        const float p = load<float>(ev, off) * kTimeOut;
        raw(uint32_t(uint64_t(to_s64(p))), 11);
    }
    void position(const void *ev, size_t off) {
        for (size_t i = 0; i < 3; ++i) {
            const float p = load<float>(ev, off + 4 * i) * kPosOut;
            raw(uint32_t(to_s32(p)), 18);
        }
    }
    void locator(const void *ev, size_t off) {
        position(ev, off);
        for (size_t i = 0; i < 4; ++i) {
            const float c = load<float>(ev, off + 0x10 + 4 * i);
            const bool present = std::fabs(c) > kQuatPresent;
            raw(present ? 1 : 0, 1);
            if (present) {
                const float a = c + 1.0f;
                raw(uint32_t(to_s32(a * kQuatOut)), 13);
            }
        }
    }
};

// Shared tail of every Read: on a short (or rejected) stream the payload goes back to factory state.
void finish_read(const Reader &in, void *ev, int type, uint32_t size) {
    if (!in.ok) reset_payload(ev, type, size);
}

// ---------------------------------------------------------------------------------------------
// The game-side helpers install_events plugs into `game`.

// Mode of the current game type: index from the NetGameManager into the script game-mode table.
int current_mode_from_game() {
    const uint8_t *table = guest<const uint8_t *(U2MP_GUEST *)(uint32_t)>(kLookupPointer)(kGameModeTableId);
    if (!table) return 0;
    const int16_t index = *image.at<const int16_t>(kGameModeIndex);
    if (index < 0 || index >= load<int32_t>(table, 0)) return 0;
    const uint8_t *entries = load<const uint8_t *>(table, 8);
    if (!entries) return 0;
    return load<int32_t>(entries, size_t(index) * 0x90 + 0xc);
}

void *process_by_id(uint32_t id) {
    return guest<void *(U2MP_GUEST *)(void *, uint32_t)>(kLookupProcess)(nullptr, id);
}

int current_mode() { return game.game_mode ? game.game_mode() : 0; }

// ---------------------------------------------------------------------------------------------
// Per-type Read / Write

// 37 NetEventNpcSpawn: npc id, spawner id, second spawner id, spawn tag.
U2MP_GUEST void read_npc_spawn(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c); in.u32(ev, 0x10); in.u32(ev, 0x14); in.u32(ev, 0x18);
    finish_read(in, ev, 37, 0x20);
}
U2MP_GUEST void write_npc_spawn(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c); out.u32(ev, 0x10); out.u32(ev, 0x14); out.u32(ev, 0x18);
}

// 38 NetEventNpcKill and 76 NetEventKillEntity: one u32 each.
U2MP_GUEST void read_npc_kill(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c);
    finish_read(in, ev, 38, 0x10);
}
U2MP_GUEST void write_one_u32(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c);
}
U2MP_GUEST void read_kill_entity(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c);
    finish_read(in, ev, 76, 0x10);
}

// 44 NetEventNpcDealDamage.
U2MP_GUEST void read_npc_deal_damage(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c, 12); in.u32(ev, 0x10); in.u32(ev, 0x14); in.time(ev, 0x18); in.u32(ev, 0x1c, 6);
    finish_read(in, ev, 44, 0x20);
}
U2MP_GUEST void write_npc_deal_damage(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c, 12); out.u32(ev, 0x10); out.u32(ev, 0x14); out.time(ev, 0x18); out.u32(ev, 0x1c, 6);
}

// 47 NetEventEvent: a script message with up to 15 typed arguments on the wire, 7 kept.
constexpr size_t kArgSlots = 0x38, kArgStride = 0x18, kMaxKeptArgs = 7;

U2MP_GUEST void read_event(void *ev, BitStream *bs) {
    Reader in{*bs};
    uint32_t target = 0, message = 0, sender = 0, count = 0;
    in.raw(target, 32);
    in.raw(message, 32);
    in.raw(sender, 32);
    in.raw(count, 4);
    uint64_t values[16] = {};
    uint32_t types[16] = {};
    for (uint32_t i = 0; in.ok && i < count; ++i) {
        uint32_t type = 0, raw = 0;
        in.raw(type, 4);
        in.raw(raw, 32);
        if (type == 1) {
            values[i] = raw != 0;
            types[i] = 1;
        } else if (type == 2 || type == 3 || type == 6) {
            values[i] = raw;
            types[i] = type;
        }   // anything else decodes to value 0, type 0
    }
    if (!in.ok) {
        reset_payload(ev, 47, 0x100);
        return;
    }
    store<uint32_t>(ev, 0xfc, target);
    store<uint32_t>(ev, 0xf8, sender);
    store<uint64_t>(ev, 0x10, 0);
    store<uint32_t>(ev, 0x18, message);
    uint64_t handle = 0;
    uint32_t field = 0;
    if (game.lookup_process) {
        if (const uint8_t *proc = static_cast<const uint8_t *>(game.lookup_process(sender))) {
            handle = load<uint64_t>(proc, 0x38);
            field = load<uint32_t>(proc, 0x44);
        }
    }
    store<uint64_t>(ev, 0x20, handle);
    store<uint32_t>(ev, 0x28, field);
    const uint32_t kept = count < kMaxKeptArgs ? count : uint32_t(kMaxKeptArgs);
    store<int8_t>(ev, 0x30, int8_t(kept));
    for (uint32_t i = 0; i < 8; ++i) {
        const size_t slot = kArgSlots + i * kArgStride;
        store<uint64_t>(ev, slot, i < kept ? values[i] : 0);
        store<uint64_t>(ev, slot + 8, 0);
        store<uint32_t>(ev, slot + 0x10, i < kept ? types[i] : 0);
    }
}
U2MP_GUEST void write_event(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0xfc);
    out.u32(ev, 0x18);
    out.u32(ev, 0xf8);
    int n = load<int8_t>(ev, 0x30);
    if (n > 8) n = 8;
    out.raw(n < 0 ? 0 : uint32_t(n), 4);
    for (int i = 0; i < n; ++i) {
        const size_t slot = kArgSlots + size_t(i) * kArgStride;
        const uint32_t type = load<uint32_t>(ev, slot + 0x10);
        // Booleans travel as 0/1 judged by their first byte only.
        const uint32_t value = type == 1 ? (load<uint8_t>(ev, slot) != 0 ? 1u : 0u) : load<uint32_t>(ev, slot);
        out.raw(type, 4);
        out.raw(value, 32);
    }
}

// 49 NetEventTerritoryOwn: owner data plus up to 10 ids; the float exists only in game mode 12.
constexpr uint32_t kTerritorySlots = 10;
constexpr int kTerritoryFloatMode = 12;

U2MP_GUEST void read_territory_own(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c);
    in.u16(ev, 0x10, 16);
    in.u16(ev, 0x12, 16);
    uint32_t count = 0;
    if (in.raw(count, 4) && count > kTerritorySlots) {
        in.ok = false;   // more ids than slots: reject, the cursor stays after the count
        count = 0;
    }
    store<uint32_t>(ev, 0x14, in.ok ? count : 0);
    for (uint32_t i = 0; in.ok && i < count; ++i) in.u32(ev, 0x18 + 4 * i, 12);
    in.u32(ev, 0x40, 12);
    in.u32(ev, 0x44, 12);
    if (in.ok && current_mode() == kTerritoryFloatMode) in.u32(ev, 0x48);
    finish_read(in, ev, 49, 0x50);
}
U2MP_GUEST void write_territory_own(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c);
    out.u16(ev, 0x10, 16);
    out.u16(ev, 0x12, 16);
    uint32_t count = load<uint32_t>(ev, 0x14);
    if (count > kTerritorySlots) count = kTerritorySlots;
    out.raw(count, 4);
    for (uint32_t i = 0; i < count; ++i) out.u32(ev, 0x18 + 4 * i, 12);
    out.u32(ev, 0x40, 12);
    out.u32(ev, 0x44, 12);
    if (current_mode() == kTerritoryFloatMode) out.u32(ev, 0x48);
}

// 51 NetEventTerritorySpawn.
U2MP_GUEST void read_territory_spawn(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c); in.u32(ev, 0x10);
    finish_read(in, ev, 51, 0x18);
}
U2MP_GUEST void write_territory_spawn(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c); out.u32(ev, 0x10);
}

// 52 NetEventTerritorySetTimer: the trailing bit exists only when the flag is set.
U2MP_GUEST void read_territory_set_timer(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c);
    in.u32(ev, 0x10);
    uint32_t flag = 0;
    if (in.raw(flag, 1)) store<uint8_t>(ev, 0x14, flag ? 1 : 0);
    if (flag) in.u32(ev, 0x18, 1);
    finish_read(in, ev, 52, 0x20);
}
U2MP_GUEST void write_territory_set_timer(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c);
    out.u32(ev, 0x10);
    out.flag(ev, 0x14);
    // The flag goes out as its low bit, but the extra bit follows any non-zero byte (as on PS3).
    if (load<uint8_t>(ev, 0x14) != 0) out.u32(ev, 0x18, 1);
}

// 53 NetEventPlayVox: the flag picks a 12- or 32-bit sound id.
U2MP_GUEST void read_play_vox(void *ev, BitStream *bs) {
    Reader in{*bs};
    uint32_t flag = 0;
    if (in.raw(flag, 1)) store<uint8_t>(ev, 0x0c, flag ? 1 : 0);
    in.u32(ev, 0x10, flag ? 12 : 32);
    in.u32(ev, 0x14); in.u32(ev, 0x18); in.u32(ev, 0x1c); in.u32(ev, 0x20);
    finish_read(in, ev, 53, 0x28);
}
U2MP_GUEST void write_play_vox(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.flag(ev, 0x0c);
    // Same split as type 52: the width follows the whole byte, the flag bit only its low bit.
    out.u32(ev, 0x10, load<uint8_t>(ev, 0x0c) != 0 ? 12 : 32);
    out.u32(ev, 0x14); out.u32(ev, 0x18); out.u32(ev, 0x1c); out.u32(ev, 0x20);
}

// 54 NetEventSimpleSnapshot.
U2MP_GUEST void read_simple_snapshot(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.locator(ev, 0x10); in.u32(ev, 0x30); in.time(ev, 0x34); in.u32(ev, 0x38, 12);
    finish_read(in, ev, 54, 0x40);
}
U2MP_GUEST void write_simple_snapshot(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.locator(ev, 0x10); out.u32(ev, 0x30); out.time(ev, 0x34); out.u32(ev, 0x38, 12);
}

// 55 NetEventStartNpcFight.
U2MP_GUEST void read_start_npc_fight(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.locator(ev, 0x10);
    in.locator(ev, 0x30);
    in.u32(ev, 0x50, 12);
    in.u32(ev, 0x54);
    for (size_t off = 0x58; off <= 0x70; off += 4) in.u32(ev, off);   // five U32, then two F32
    in.u32(ev, 0x74, 6);
    in.flag(ev, 0x78);
    in.flag(ev, 0x79);
    finish_read(in, ev, 55, 0x80);
}
U2MP_GUEST void write_start_npc_fight(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.locator(ev, 0x10);
    out.locator(ev, 0x30);
    out.u32(ev, 0x50, 12);
    out.u32(ev, 0x54);
    for (size_t off = 0x58; off <= 0x70; off += 4) out.u32(ev, off);
    out.u32(ev, 0x74, 6);
    out.flag(ev, 0x78);
    out.flag(ev, 0x79);
}

// 57 NetEventSpawnHiddenTreasure.
U2MP_GUEST void read_spawn_hidden_treasure(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.position(ev, 0x10); in.u32(ev, 0x20);
    finish_read(in, ev, 57, 0x30);
}
U2MP_GUEST void write_spawn_hidden_treasure(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.position(ev, 0x10); out.u32(ev, 0x20);
}

// 61 NetEventEarnMedal.
U2MP_GUEST void read_earn_medal(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c); in.u32(ev, 0x10); in.u32(ev, 0x14);
    finish_read(in, ev, 61, 0x18);
}
U2MP_GUEST void write_earn_medal(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c); out.u32(ev, 0x10); out.u32(ev, 0x14);
}

// 67 NetEventTurretSnapshot.
U2MP_GUEST void read_turret_snapshot(void *ev, BitStream *bs) {
    Reader in{*bs};
    in.u32(ev, 0x0c); in.u32(ev, 0x10); in.u32(ev, 0x14); in.time(ev, 0x18); in.u32(ev, 0x1c, 12);
    finish_read(in, ev, 67, 0x20);
}
U2MP_GUEST void write_turret_snapshot(void *ev, BitStream *bs) {
    Writer out{*bs};
    out.u32(ev, 0x0c); out.u32(ev, 0x10); out.u32(ev, 0x14); out.time(ev, 0x18); out.u32(ev, 0x1c, 12);
}

// ---------------------------------------------------------------------------------------------
// 37 NetEventNpcSpawn Apply: spawn the npc the sender described and register it for the net.

U2MP_GUEST void apply_npc_spawn(void *ev, void * /*connection*/) {
    const uint32_t npc_id = load<uint32_t>(ev, 0x0c);
    const uint32_t spawner_id = load<uint32_t>(ev, 0x10);
    const uint32_t second_id = load<uint32_t>(ev, 0x14);
    const uint32_t tag = load<uint32_t>(ev, 0x18);
    void *const manager = image.base + kSpawnerManager;
    const auto lookup_spawner = guest<uint8_t *(U2MP_GUEST *)(void *, uint32_t)>(kLookupSpawner);

    if (game.lookup_process && game.lookup_process(npc_id)) {
        log("NetEventNpcSpawn: npc %u already exists", npc_id);
        return;
    }
    uint8_t *const spawner = lookup_spawner(manager, spawner_id);
    uint8_t *const second = second_id ? lookup_spawner(manager, second_id) : nullptr;
    if (!spawner) {
        log("NetEventNpcSpawn: can't find spawner %08x", spawner_id);
        return;
    }

    alignas(16) uint8_t info[0x60] = {};
    store<uint32_t>(info, 0x00, load<uint32_t>(spawner, 0x40));
    store<uint64_t>(info, 0x10, uint64_t(uintptr_t(spawner)));
    store<uint64_t>(info, 0x40, uint64_t(uintptr_t(spawner)));

    alignas(16) uint8_t params[0xb0] = {};
    guest<void(U2MP_GUEST *)(void *, void *)>(kSpawnParamsInit)(params, info);
    store<uint32_t>(params, 0x00, second_id);
    // The game looks the second spawner up again here, whether or not the id is 0.
    if (uint8_t *s = lookup_spawner(manager, second_id))
        guest<void(U2MP_GUEST *)(void *, void *)>(kSpawnParamsSetSpawner)(params, s);
    store<uint32_t>(params, 0x40, tag);

    alignas(16) uint8_t locator[0x40] = {};
    void *where = nullptr;
    if (second) {
        guest<void(U2MP_GUEST *)(void *, void *)>(kSpawnerLocator)(locator, second);
        where = locator;
    }
    uint8_t *const npc =
        guest<uint8_t *(U2MP_GUEST *)(void *, void *, void *, uint64_t)>(kSpawnNpc)(spawner, params, where, 0);
    if (!npc) {
        log("NetEventNpcSpawn: unable to spawn npc from spawner %08x", spawner_id);
        return;
    }
    store<uint32_t>(npc, 0x68, npc_id);
    // AddNetNpcTracker returns a bool: only its low byte is meaningful.
    const uint8_t added = guest<uint8_t(U2MP_GUEST *)(void *, void *, uint32_t)>(kAddNetNpcTracker)(
        image.base + kNetGameManager, npc, load<uint32_t>(npc, 0x3708));
    if (!added) log("NetEventNpcSpawn: failed to add npc %u to the NetGameManager", npc_id);
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The port table (its order is the install log order).

const Port kPorts[] = {
    {37, "NetEventNpcSpawn", 0x1040f30, 0x20, 0x62f240, 0x62f230, 0x62f220, read_npc_spawn, write_npc_spawn, apply_npc_spawn},
    {38, "NetEventNpcKill", 0x1040760, 0x10, 0x622080, 0x622070, 0, read_npc_kill, write_one_u32, nullptr},
    {44, "NetEventNpcDealDamage", 0x10405d0, 0x20, 0x61fe00, 0, 0, read_npc_deal_damage, write_npc_deal_damage, nullptr},
    {47, "NetEventEvent", 0x1040580, 0x100, 0x61fa60, 0x61fa50, 0, read_event, write_event, nullptr},
    {49, "NetEventTerritoryOwn", 0x1040440, 0x50, 0x61f030, 0x61f020, 0, read_territory_own, write_territory_own, nullptr},
    {51, "NetEventTerritorySpawn", 0x10403a0, 0x18, 0x61e890, 0x61e880, 0, read_territory_spawn, write_territory_spawn, nullptr},
    {52, "NetEventTerritorySetTimer", 0x1040350, 0x20, 0x61e700, 0x61e6f0, 0, read_territory_set_timer, write_territory_set_timer, nullptr},
    {53, "NetEventPlayVox", 0x1040300, 0x28, 0x61e600, 0x61e5f0, 0, read_play_vox, write_play_vox, nullptr},
    {54, "NetEventSimpleSnapshot", 0x10402b0, 0x40, 0x61e2a0, 0x61e290, 0, read_simple_snapshot, write_simple_snapshot, nullptr},
    {55, "NetEventStartNpcFight", 0x1040260, 0x80, 0x61e1b0, 0x61e1a0, 0, read_start_npc_fight, write_start_npc_fight, nullptr},
    {57, "NetEventSpawnHiddenTreasure", 0x10401c0, 0x30, 0x61d3a0, 0x61d390, 0, read_spawn_hidden_treasure, write_spawn_hidden_treasure, nullptr},
    {61, "NetEventEarnMedal", 0x1040120, 0x18, 0x61d240, 0x61d230, 0, read_earn_medal, write_earn_medal, nullptr},
    {67, "NetEventTurretSnapshot", 0x1040030, 0x20, 0x61bee0, 0x61bed0, 0, read_turret_snapshot, write_turret_snapshot, nullptr},
    {76, "NetEventKillEntity", 0x103fdb0, 0x10, 0x61afd0, 0x61afc0, 0, read_kill_entity, write_one_u32, nullptr},
};
const size_t kPortCount = sizeof kPorts / sizeof kPorts[0];

// ---------------------------------------------------------------------------------------------
// Install: point every stubbed vtable slot at its port, but only where the slot still holds the
// expected `ret` stub (anything else means the image is not what we think, so leave it alone).

void install_events(int &installed) {
    if (!image.base || kFrameClock + 4 > image.size) return;
    game.frame_clock = image.at<const int32_t>(kFrameClock);
    game.game_mode = current_mode_from_game;
    game.lookup_process = process_by_id;

    int ported = 0;
    for (const Port &p : kPorts) {
        if (p.vtable + 0x28 > image.size) continue;
        const struct {
            unsigned slot;
            uint64_t stub;
            void *fn;
        } slots[] = {
            {2, p.read_stub, reinterpret_cast<void *>(p.read)},
            {3, p.write_stub, reinterpret_cast<void *>(p.write)},
            {4, p.apply_stub, reinterpret_cast<void *>(p.apply)},
        };
        for (const auto &s : slots) {
            if (!s.stub || !s.fn) continue;
            uint64_t *entry = image.at<uint64_t>(p.vtable + 8 * s.slot);
            if (*entry != uint64_t(uintptr_t(image.base + s.stub)) || !is_ret_stub(s.stub)) {
                log("%s slot %u: expected the stub 0x%llx, left alone", p.name, s.slot,
                    static_cast<unsigned long long>(s.stub));
                continue;
            }
            *entry = uint64_t(uintptr_t(s.fn));
            ++ported;
        }
    }
    log("net events: %d stubbed Read/Write/Apply slots ported", ported);
    installed += ported;
}

}  // namespace u2mp::events
