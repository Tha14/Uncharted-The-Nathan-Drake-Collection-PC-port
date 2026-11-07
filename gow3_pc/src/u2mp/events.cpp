// u2mp: net-event Read/Write (and NpcSpawn Apply) ported from PS3 1.10 to PS4 1.02 (see events.h).
//
// Sources: the PS3 slot bodies (EBOOT 1.10; the slot labels in FUNCTIONS-MATCH-GAMEPLAY.md section 7
// are swapped: slot 2 is Read, slot 3 is Write) and, for the PS4 payload offsets, the surviving PS4
// code that builds or consumes each event (factory 0x6180f0, senders, Applies). PS4 payloads start
// at +0xc (PS3 +8); 16-byte vectors stay 16-aligned, so events holding a locator keep the PS3
// offsets. Every offset below was checked against PS4 code unless marked "unverified".
//
// PS3 BitStream wrappers (all thin calls of ReadBits 0x42f8d8 / WriteBits 0x42fe78):
//   U32/S32/F32 = 32 bits, U16/S16 = 16, U8 = 8, Bool = 1 (read: != 0; write: low bit).
//   Time (R 0x42f9c0 / W 0x42ff4c, PS4 R 0x5e3680, W inlined): 11 bits of int64(t * 150); read
//     t = v / 150, then moved forward by 2048/150 s while it is more than 1024/150 s behind now
//     (frame counter / 300).
//   Position (R 0x42fa70 / W 0x42ff78, PS4 R 0x5e2ff0 / W 0x5e2930): x, y, z as 18-bit signed
//     int32(f * 512); read f = v / 512, PS4 stores (x, y, z, y).
//   Locator (R 0x4306e8 / W 0x4302d8, PS4 R 0x5e2b40 / W 0x5e2320): position, then quaternion
//     x, y, z, w each as 1 bit (|c| > 1e-4) [+ 13 bits int32((c + 1) * 4095)]; read
//     c = v / 4095 - 1 (absent: v = 4095), then normalized.
// Readers check every field before reading it; on a short stream they stop and leave the payload
// in its factory state (zero), never reading past maxBits. Writers never write past maxBits.
#include "events.h"

#include <cmath>
#include <cstring>

namespace u2mp::events {

Game game;

namespace {

// ---- PS4 1.02 image offsets used at run time ------------------------------------------------
constexpr uint64_t kFrameClock = 0x1116180 + 0x204;     // s32, 300 per second (ReadTime 0x5e3680)
constexpr uint64_t kNetGameManager = 0x113b760;
constexpr uint64_t kGameModeIndex = 0x1ca0;             // s16 in the NetGameManager (PS3 +6380)
constexpr uint64_t kLookupPointer = 0xa15e70;           // ScriptManager LookupPointer(id)
constexpr uint32_t kGameModeTable = 0x3b9a067d;         // its id; entries 0x90 bytes, mode at +0xc
constexpr uint64_t kLookupProcess = 0x9bbe40;           // LookupProcessByUniqueId(unused, id)
constexpr uint64_t kSpawnerManager = 0x1130390;
constexpr uint64_t kLookupSpawner = 0x56c7b0;           // (manager, name id) -> spawner (PS3 0xb565c)
constexpr uint64_t kSpawnParamsInit = 0x233600;         // (params, info)          (PS3 0x1b9978)
constexpr uint64_t kSpawnParamsSpawner = 0x235360;      // (params, spawner)       (PS3 0x1b9504)
constexpr uint64_t kSpawnerLocator = 0x4bdfe0;          // (locator out, spawner)  (PS3 0x7c9dc)
constexpr uint64_t kSpawnNpc = 0x4be330;                // (spawner, params, locator|0, 0) -> npc (PS3 0x7dda0)
constexpr uint64_t kAddNetNpcTracker = 0x644fa0;        // (ngm, npc, npc+0x3708)  (PS3 0x498b28)

template <typename T> T get(const void *ev, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(ev) + off, sizeof v);
    return v;
}
template <typename T> void put(void *ev, size_t off, T v) { std::memcpy(static_cast<uint8_t *>(ev) + off, &v, sizeof v); }

uint32_t float_bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return v;
}
float bits_float(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
// x86 cvttss2si: truncation, 0x80000000... when out of range or NaN.
int32_t cvtt32(float f) { return (f >= -2147483648.0f && f < 2147483648.0f) ? int32_t(f) : INT32_MIN; }
int64_t cvtt64(float f) {
    return (f >= -9223372036854775808.0f && f < 9223372036854775808.0f) ? int64_t(f) : INT64_MIN;
}

// Exact float constants of the PS3 and PS4 code (PS4 0xe62170.., 0xe62198.., 0xe624b4).
constexpr float kTimeScale = 0.006666666828095913f, kClockScale = 0.0033333334140479565f;
constexpr float kTimeHalfWrap = 6.826666831970215f, kTimeWrap = 13.65333366394043f, kTimeWrite = 150.0f;
constexpr float kPosRead = 0.001953125f, kPosWrite = 512.0f;
constexpr float kQuatRead = 0.00024420025874860585f, kQuatWrite = 4095.0f, kQuatMin = 9.999999747378752e-05f;

void log_overflow_once() {
    static bool said = false;
    if (!said) {
        said = true;
        log("net event: a Write ran past the BitStream's maxBits (fields dropped)");
    }
}

struct In {
    BitStream &bs;
    bool ok = true;
    uint32_t bits(unsigned n) {
        uint32_t v = 0;
        if (ok && !read_bits(bs, v, n)) ok = false;
        return ok ? v : 0;
    }
    uint32_t u32() { return bits(32); }
    float f32() { return bits_float(bits(32)); }
    uint8_t flag() { return bits(1) != 0; }
    float time() {   // PS4 0x5e3680
        float t = float(bits(11));
        t = t * kTimeScale;
        if (ok && game.frame_clock) {
            const float now = float(*game.frame_clock) * kClockScale;
            while (now - t > kTimeHalfWrap) t += kTimeWrap;
        }
        return t;
    }
    void position(void *ev, size_t off) {   // PS4 0x5e2ff0: (x, y, z, y)
        float p[3];
        for (float &c : p) {
            const int32_t v = int32_t(bits(18) << 14) >> 14;
            c = float(v) * kPosRead;
        }
        const float out[4] = {p[0], p[1], p[2], p[1]};
        std::memcpy(static_cast<uint8_t *>(ev) + off, out, sizeof out);
    }
    void locator(void *ev, size_t off) {   // PS4 0x5e2b40: position, then the quaternion at +0x10
        position(ev, off);
        float q[4];
        for (float &c : q) {
            uint32_t v = 4095;
            if (bits(1)) v = bits(13);
            c = float(v) * kQuatRead;
            c = c + -1.0f;
        }
        const float s = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
        if (s > 0.0f) {
            const float r = 1.0f / std::sqrt(s);
            for (float &c : q) c *= r;
        } else {
            q[0] = q[1] = q[2] = 0.0f;
            q[3] = 1.0f;
        }
        std::memcpy(static_cast<uint8_t *>(ev) + off + 0x10, q, sizeof q);
    }
};

struct Out {
    BitStream &bs;
    void bits(uint32_t v, unsigned n) {
        if (!write_bits(bs, v, n)) log_overflow_once();
    }
    void u32(uint32_t v) { bits(v, 32); }
    void f32(float f) { bits(float_bits(f), 32); }
    void flag(uint8_t b) { bits(b & 1, 1); }
    void time(float t) { bits(uint32_t(cvtt64(t * kTimeWrite)), 11); }
    void position(const void *ev, size_t off) {   // PS4 0x5e2930
        for (int i = 0; i < 3; ++i) bits(uint32_t(cvtt32(get<float>(ev, off + 4 * i) * kPosWrite)), 18);
    }
    void locator(const void *ev, size_t off) {   // PS4 0x5e2320
        position(ev, off);
        for (int i = 0; i < 4; ++i) {
            const float c = get<float>(ev, off + 0x10 + 4 * i);
            const bool present = std::fabs(c) > kQuatMin;
            bits(present, 1);
            if (present) bits(uint32_t(cvtt32((c + 1.0f) * kQuatWrite)), 13);
        }
    }
};

const Port *port_of(int type);

// Factory state of the payload (what CreateNetEventByType leaves): zero, TerritoryOwn +0x10 = -1.
void reset(void *ev, int type) {
    const Port *p = port_of(type);
    if (!p) return;
    std::memset(static_cast<uint8_t *>(ev) + 0xc, 0, p->size - 0xc);
    if (type == 49) put<uint32_t>(ev, 0x10, 0xffffffffu);
}

void finish(In &in, void *ev, int type) {
    if (!in.ok) reset(ev, type);
}

int current_game_mode() { return game.game_mode ? game.game_mode() : 0; }

// ---- 37 NetEventNpcSpawn -----------------------------------------------------------------------
// PS3 Read 0x46b200 / Write 0x46a730 / Apply 0x475578; PS4 vtable 0x1040f30 slots 2/3/4 (stubs
// 0x62f240 / 0x62f230 / 0x62f220), size 0x20. Sender PS4 0x6133d0 (PS3 0x48bd9c).
//   +0x0c (PS3 +8)  npc unique id      32 bits   (sender: npc+0x68)
//   +0x10 (PS3 +12) spawner name id    U32       (sender: [spawner+0x40]+0x28, optional)
//   +0x14 (PS3 +16) second spawner id  U32       (sender: [arg2])
//   +0x18 (PS3 +20) spawn tag value    U32       (sender: [arg2+0x40]; Apply: params+0x40)
U2MP_GUEST void npc_spawn_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.bits(32));
    put(ev, 0x10, in.u32());
    put(ev, 0x14, in.u32());
    put(ev, 0x18, in.u32());
    finish(in, ev, 37);
}
U2MP_GUEST void npc_spawn_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.bits(get<uint32_t>(ev, 0x0c), 32);
    out.u32(get<uint32_t>(ev, 0x10));
    out.u32(get<uint32_t>(ev, 0x14));
    out.u32(get<uint32_t>(ev, 0x18));
}

// Apply: spawn the NPC from its spawner like the host did, give it the host's unique id and
// register it with the NetGameManager. PS4 calls and struct layouts follow the PS4 "spawn-npc"
// script native 0x2e57b0, which is the PS3 0x21df40 sequence the PS3 Apply repeats.
U2MP_GUEST void npc_spawn_apply(void *ev, void *) {
    using Lookup = void *(U2MP_GUEST *)(void *manager, uint32_t id);
    using ParamsInit = void(U2MP_GUEST *)(void *params, void *info);
    using ParamsSpawner = void(U2MP_GUEST *)(void *params, void *spawner);
    using SpawnerLocator = void(U2MP_GUEST *)(void *locator, void *spawner);
    using Spawn = void *(U2MP_GUEST *)(void *spawner, void *params, void *locator, uint64_t zero);
    using AddTracker = uint8_t(U2MP_GUEST *)(void *ngm, void *npc, uint32_t value);
    unsigned char *base = image.base;
    const uint32_t npc_id = get<uint32_t>(ev, 0x0c);
    if (game.lookup_process && game.lookup_process(npc_id)) {
        log("NetEventNpcSpawn: npc %u already exists", npc_id);   // PS3 asserts here
        return;
    }
    auto lookup = reinterpret_cast<Lookup>(base + kLookupSpawner);
    void *manager = base + kSpawnerManager;
    void *spawner = lookup(manager, get<uint32_t>(ev, 0x10));
    const uint32_t second_id = get<uint32_t>(ev, 0x14);
    void *second = second_id ? lookup(manager, second_id) : nullptr;
    if (!spawner) {
        log("NetEventNpcSpawn: can't find spawner %08x", get<uint32_t>(ev, 0x10));
        return;
    }
    // SpawnInfo (PS4 0x60 bytes, PS3 52): +0 [spawner+0x40], +0x10 spawner, +0x40 spawner.
    alignas(16) uint8_t info[0x60] = {};
    put(info, 0x00, get<uint32_t>(spawner, 0x40));
    put(info, 0x10, spawner);
    put(info, 0x40, spawner);
    // Spawn params (PS4 0xa8 bytes): PS3 0x1ba558 = init, +0 = second id, spawner by that id.
    alignas(16) uint8_t params[0xb0] = {};
    reinterpret_cast<ParamsInit>(base + kSpawnParamsInit)(params, info);
    put(params, 0x00, second_id);
    if (void *s = lookup(manager, second_id)) reinterpret_cast<ParamsSpawner>(base + kSpawnParamsSpawner)(params, s);
    put(params, 0x40, get<uint32_t>(ev, 0x18));   // PS3 params+64 (spawner tag 0x2be2f3bd)
    alignas(16) uint8_t locator[0x40] = {};
    void *where = nullptr;
    if (second) {
        reinterpret_cast<SpawnerLocator>(base + kSpawnerLocator)(locator, second);
        where = locator;
    }
    void *npc = reinterpret_cast<Spawn>(base + kSpawnNpc)(spawner, params, where, 0);
    if (!npc) {
        log("NetEventNpcSpawn: unable to spawn npc from spawner %08x", get<uint32_t>(ev, 0x10));
        return;
    }
    put(npc, 0x68, npc_id);   // PS3 npc+76
    if (!reinterpret_cast<AddTracker>(base + kAddNetNpcTracker)(base + kNetGameManager, npc, get<uint32_t>(npc, 0x3708)))
        log("NetEventNpcSpawn: failed to add npc %u to the NetGameManager", npc_id);
}

// ---- 38 NetEventNpcKill: PS3 R 0x46b1cc / W 0x46a6fc; PS4 vt 0x1040760 (0x622080 / 0x622070),
// size 0x10. +0x0c (PS3 +8) npc id, 32 bits (Apply 0x621f80 reads +0xc).
U2MP_GUEST void npc_kill_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.bits(32));
    finish(in, ev, 38);
}
U2MP_GUEST void npc_kill_write(void *ev, BitStream *bs) { Out{*bs}.bits(get<uint32_t>(ev, 0x0c), 32); }

// ---- 44 NetEventNpcDealDamage Read: PS3 0x46de30; PS4 vt 0x10405d0 slot 2 (0x61fe00), size 0x20.
// The PS4 Write 0x61fb10 survives and fixes the layout:
//   +0x0c 12 bits, +0x10 32 bits, +0x14 S32, +0x18 time, +0x1c 6 bits (PS3 +8..+24).
U2MP_GUEST void npc_deal_damage_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.bits(12));
    put(ev, 0x10, in.bits(32));
    put(ev, 0x14, in.u32());
    put(ev, 0x18, in.time());
    put(ev, 0x1c, in.bits(6));
    finish(in, ev, 44);
}
// Not installed (PS4 has it); used by the host test to round-trip the Read.
U2MP_GUEST void npc_deal_damage_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.bits(get<uint32_t>(ev, 0x0c), 12);
    out.bits(get<uint32_t>(ev, 0x10), 32);
    out.u32(get<uint32_t>(ev, 0x14));
    out.time(get<float>(ev, 0x18));
    out.bits(get<uint32_t>(ev, 0x1c), 6);
}

// ---- 47 NetEventEvent (net-send-event) --------------------------------------------------------
// PS3 R 0x4789c8 / W 0x4792b0; PS4 vt 0x1040580 (0x61fa60 / 0x61fa50), size 0x100.
// Sender PS4 0x614c40 (PS3 0x471034), Apply 0x61fa10 (PS3 0x470fc8).
//   +0x10 EventMemento (PS3 +8, filled by PS4 0x4c47a0 / PS3 0x82400):
//         +0x00 q source message (PS3: a stack temporary; Apply 0x4c4900 never reads it: 0 here)
//         +0x08 u32 message id         +0x10 q / +0x18 u32 sender process handle
//         +0x20 s8 argument count (<= 7)
//         +0x28 + i*0x18 argument: +0 q value, +8 q, +0x10 u32 type (PS3: 16-byte args, type +12)
//   +0xf8 sender process unique id (PS3 +160), +0xfc target process unique id (PS3 +164)
// Wire: U32 target, U32 message, U32 sender, 4 bits count, count x {4 bits type, U32 value};
// types 1 bool, 2 int, 3 uint/string id, 6 float (others assert on PS3).
constexpr size_t kMemento = 0x10, kArgs = kMemento + 0x28, kArgSize = 0x18;
constexpr int kMaxArgs = 7;

U2MP_GUEST void event_read(void *ev, BitStream *bs) {
    In in{*bs};
    const uint32_t target = in.u32(), message = in.u32(), sender = in.u32();
    const uint32_t count = in.bits(4);
    struct Arg {
        uint64_t value;
        uint32_t type;
    } args[15] = {};
    for (uint32_t i = 0; i < count && in.ok; ++i) {
        const uint32_t type = in.bits(4), value = in.u32();
        switch (type) {
        case 1: args[i] = {uint64_t(value != 0), 1}; break;
        case 2: case 3: case 6: args[i] = {value, type}; break;
        default: args[i] = {0, 0}; break;   // PS3: assert, then an empty argument
        }
    }
    if (!in.ok) {
        reset(ev, 47);
        return;
    }
    put(ev, 0xfc, target);
    put(ev, 0xf8, sender);
    put<uint64_t>(ev, kMemento + 0x00, 0);
    put(ev, kMemento + 0x08, message);
    void *process = game.lookup_process ? game.lookup_process(sender) : nullptr;
    put<uint64_t>(ev, kMemento + 0x10, process ? get<uint64_t>(process, 0x38) : 0);
    put<uint32_t>(ev, kMemento + 0x18, process ? get<uint32_t>(process, 0x44) : 0);
    const int kept = count > uint32_t(kMaxArgs) ? kMaxArgs : int(count);   // "too many parameters"
    put<int8_t>(ev, kMemento + 0x20, int8_t(kept));
    for (int i = 0; i < 8; ++i) {
        const size_t a = kArgs + size_t(i) * kArgSize;
        put<uint64_t>(ev, a + 0x00, i < kept ? args[i].value : 0);
        put<uint64_t>(ev, a + 0x08, 0);
        put<uint32_t>(ev, a + 0x10, i < kept ? args[i].type : 0);
    }
}
U2MP_GUEST void event_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.u32(get<uint32_t>(ev, 0xfc));
    out.u32(get<uint32_t>(ev, kMemento + 0x08));
    out.u32(get<uint32_t>(ev, 0xf8));
    int count = get<int8_t>(ev, kMemento + 0x20);
    if (count > 8) count = 8;   // the memento holds 8 slots (the copy keeps at most 7)
    out.bits(uint32_t(count < 0 ? 0 : count), 4);
    for (int i = 0; i < count; ++i) {
        const size_t a = kArgs + size_t(i) * kArgSize;
        const uint32_t type = get<uint32_t>(ev, a + 0x10);
        // PS3 wrote the first word of the argument; a bool is its first byte (little-endian here).
        const uint32_t value = type == 1 ? uint32_t(get<uint8_t>(ev, a) != 0) : get<uint32_t>(ev, a);
        out.bits(type, 4);
        out.u32(value);
    }
}

// ---- 49 NetEventTerritoryOwn: PS3 R 0x46fc10 / W 0x46f4e8; PS4 vt 0x1040440 (0x61f030 / 0x61f020),
// size 0x50, factory sets +0x10 = -1. Apply 0x61eb90 reads every field:
//   +0x0c U32 object id, +0x10 S16 / +0x12 S16 team net ids, +0x14 4-bit count,
//   +0x18 + 4i 12-bit net ids (10 slots), +0x40 / +0x44 12 bits, +0x48 F32 only in game mode 12
//   (PS3 0x49e4a0 = PS4 LookupPointer(0x3b9a067d) entry [ngm+0x1ca0] +0xc; mode 7 compare in both
//   Applies shows the same numbering).
constexpr uint32_t kTerritorySlots = 10;
U2MP_GUEST void territory_own_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.u32());
    put(ev, 0x10, uint16_t(in.bits(16)));
    put(ev, 0x12, uint16_t(in.bits(16)));
    const uint32_t count = in.bits(4);
    if (count > kTerritorySlots) in.ok = false;   // PS3 would write past the array
    put(ev, 0x14, in.ok ? count : 0);
    for (uint32_t i = 0; i < count && in.ok; ++i) put(ev, 0x18 + 4 * i, in.bits(12));
    put(ev, 0x40, in.bits(12));
    put(ev, 0x44, in.bits(12));
    if (in.ok && current_game_mode() == 12) put(ev, 0x48, in.f32());
    finish(in, ev, 49);
}
U2MP_GUEST void territory_own_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.u32(get<uint32_t>(ev, 0x0c));
    out.bits(get<uint16_t>(ev, 0x10), 16);
    out.bits(get<uint16_t>(ev, 0x12), 16);
    uint32_t count = get<uint32_t>(ev, 0x14);
    if (count > kTerritorySlots) count = kTerritorySlots;
    out.bits(count, 4);
    for (uint32_t i = 0; i < count; ++i) out.bits(get<uint32_t>(ev, 0x18 + 4 * i), 12);
    out.bits(get<uint32_t>(ev, 0x40), 12);
    out.bits(get<uint32_t>(ev, 0x44), 12);
    if (current_game_mode() == 12) out.f32(get<float>(ev, 0x48));
}

// ---- 51 NetEventTerritorySpawn: PS3 R 0x469e9c / W 0x469b80; PS4 vt 0x10403a0 (0x61e890 / 0x61e880),
// size 0x18. +0x0c U32 process to kill, +0x10 U32 spawner to spawn (Apply 0x61e710).
U2MP_GUEST void territory_spawn_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.u32());
    put(ev, 0x10, in.u32());
    finish(in, ev, 51);
}
U2MP_GUEST void territory_spawn_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.u32(get<uint32_t>(ev, 0x0c));
    out.u32(get<uint32_t>(ev, 0x10));
}

// ---- 52 NetEventTerritorySetTimer: PS3 R 0x46af48 / W 0x46a4b8; PS4 vt 0x1040350 (0x61e700 / 0x61e6f0),
// size 0x20. +0x0c U32, +0x10 F32, +0x14 Bool, +0x18 1 bit only when +0x14 (Apply 0x61e610).
U2MP_GUEST void territory_set_timer_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.u32());
    put(ev, 0x10, in.f32());
    const uint8_t on = in.flag();
    put(ev, 0x14, on);
    if (on) put(ev, 0x18, in.bits(1));
    finish(in, ev, 52);
}
U2MP_GUEST void territory_set_timer_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.u32(get<uint32_t>(ev, 0x0c));
    out.f32(get<float>(ev, 0x10));
    out.flag(get<uint8_t>(ev, 0x14));
    if (get<uint8_t>(ev, 0x14)) out.bits(get<uint32_t>(ev, 0x18), 1);
}

// ---- 53 NetEventPlayVox: PS3 R 0x46ae78 / W 0x46a408; PS4 vt 0x1040300 (0x61e600 / 0x61e5f0),
// size 0x28. +0x0c Bool, +0x10 12 bits if +0x0c else 32 bits, +0x14 U32, +0x18 U32, +0x1c F32,
// +0x20 F32 (Apply 0x61e2b0).
U2MP_GUEST void play_vox_read(void *ev, BitStream *bs) {
    In in{*bs};
    const uint8_t small = in.flag();
    put(ev, 0x0c, small);
    put(ev, 0x10, in.bits(small ? 12 : 32));
    put(ev, 0x14, in.u32());
    put(ev, 0x18, in.u32());
    put(ev, 0x1c, in.f32());
    put(ev, 0x20, in.f32());
    finish(in, ev, 53);
}
U2MP_GUEST void play_vox_write(void *ev, BitStream *bs) {
    Out out{*bs};
    const uint8_t small = get<uint8_t>(ev, 0x0c);
    out.flag(small);
    out.bits(get<uint32_t>(ev, 0x10), small ? 12 : 32);
    out.u32(get<uint32_t>(ev, 0x14));
    out.u32(get<uint32_t>(ev, 0x18));
    out.f32(get<float>(ev, 0x1c));
    out.f32(get<float>(ev, 0x20));
}

// ---- 54 NetEventSimpleSnapshot: PS3 R 0x46dda8 / W 0x46d218; PS4 vt 0x10402b0 (0x61e2a0 / 0x61e290),
// size 0x40, mode 2. +0x10 locator, +0x30 U32 object id, +0x34 time, +0x38 12 bits (Apply 0x61e1c0).
U2MP_GUEST void simple_snapshot_read(void *ev, BitStream *bs) {
    In in{*bs};
    in.locator(ev, 0x10);
    put(ev, 0x30, in.u32());
    put(ev, 0x34, in.time());
    put(ev, 0x38, in.bits(12));
    finish(in, ev, 54);
}
U2MP_GUEST void simple_snapshot_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.locator(ev, 0x10);
    out.u32(get<uint32_t>(ev, 0x30));
    out.time(get<float>(ev, 0x34));
    out.bits(get<uint32_t>(ev, 0x38), 12);
}

// ---- 55 NetEventStartNpcFight: PS3 R 0x46c198 / W 0x46bd8c; PS4 vt 0x1040260 (0x61e1b0 / 0x61e1a0),
// size 0x80. +0x10 / +0x30 locators, +0x50 12 bits, +0x54 32 bits, +0x58..+0x68 5 x U32,
// +0x6c / +0x70 F32, +0x74 6 bits, +0x78 / +0x79 Bool (Apply 0x61d6d0 reads all of them).
U2MP_GUEST void start_npc_fight_read(void *ev, BitStream *bs) {
    In in{*bs};
    in.locator(ev, 0x10);
    in.locator(ev, 0x30);
    put(ev, 0x50, in.bits(12));
    put(ev, 0x54, in.bits(32));
    for (size_t off = 0x58; off <= 0x68; off += 4) put(ev, off, in.u32());
    put(ev, 0x6c, in.f32());
    put(ev, 0x70, in.f32());
    put(ev, 0x74, in.bits(6));
    put(ev, 0x78, in.flag());
    put(ev, 0x79, in.flag());
    finish(in, ev, 55);
}
U2MP_GUEST void start_npc_fight_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.locator(ev, 0x10);
    out.locator(ev, 0x30);
    out.bits(get<uint32_t>(ev, 0x50), 12);
    out.bits(get<uint32_t>(ev, 0x54), 32);
    for (size_t off = 0x58; off <= 0x68; off += 4) out.u32(get<uint32_t>(ev, off));
    out.f32(get<float>(ev, 0x6c));
    out.f32(get<float>(ev, 0x70));
    out.bits(get<uint32_t>(ev, 0x74), 6);
    out.flag(get<uint8_t>(ev, 0x78));
    out.flag(get<uint8_t>(ev, 0x79));
}

// ---- 57 NetEventSpawnHiddenTreasure: PS3 R 0x46cb60 / W 0x46c650; PS4 vt 0x10401c0
// (0x61d3a0 / 0x61d390), size 0x30. +0x10 position, +0x20 U32 (Apply 0x61d280).
U2MP_GUEST void spawn_hidden_treasure_read(void *ev, BitStream *bs) {
    In in{*bs};
    in.position(ev, 0x10);
    put(ev, 0x20, in.u32());
    finish(in, ev, 57);
}
U2MP_GUEST void spawn_hidden_treasure_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.position(ev, 0x10);
    out.u32(get<uint32_t>(ev, 0x20));
}

// ---- 61 NetEventEarnMedal: PS3 R 0x469ef8 / W 0x469bd4; PS4 vt 0x1040120 (0x61d240 / 0x61d230),
// size 0x18. +0x0c, +0x10, +0x14 U32 (Apply 0x61d1e0).
U2MP_GUEST void earn_medal_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.u32());
    put(ev, 0x10, in.u32());
    put(ev, 0x14, in.u32());
    finish(in, ev, 61);
}
U2MP_GUEST void earn_medal_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.u32(get<uint32_t>(ev, 0x0c));
    out.u32(get<uint32_t>(ev, 0x10));
    out.u32(get<uint32_t>(ev, 0x14));
}

// ---- 67 NetEventTurretSnapshot: PS3 R 0x46dc98 / W 0x46d124; PS4 vt 0x1040030 (0x61bee0 / 0x61bed0),
// size 0x20, mode 2. +0x0c F32, +0x10 F32, +0x14 U32, +0x18 time, +0x1c 12 bits (Apply 0x61be10).
U2MP_GUEST void turret_snapshot_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.f32());
    put(ev, 0x10, in.f32());
    put(ev, 0x14, in.u32());
    put(ev, 0x18, in.time());
    put(ev, 0x1c, in.bits(12));
    finish(in, ev, 67);
}
U2MP_GUEST void turret_snapshot_write(void *ev, BitStream *bs) {
    Out out{*bs};
    out.f32(get<float>(ev, 0x0c));
    out.f32(get<float>(ev, 0x10));
    out.u32(get<uint32_t>(ev, 0x14));
    out.time(get<float>(ev, 0x18));
    out.bits(get<uint32_t>(ev, 0x1c), 12);
}

// ---- 76 NetEventKillEntity: PS3 R 0x469e6c / W 0x469b50; PS4 vt 0x103fdb0 (0x61afd0 / 0x61afc0),
// size 0x10. +0x0c U32 (Apply 0x61afa0).
U2MP_GUEST void kill_entity_read(void *ev, BitStream *bs) {
    In in{*bs};
    put(ev, 0x0c, in.u32());
    finish(in, ev, 76);
}
U2MP_GUEST void kill_entity_write(void *ev, BitStream *bs) { Out{*bs}.u32(get<uint32_t>(ev, 0x0c)); }

// ---- PS4 game state for the serializers ---------------------------------------------------
int game_mode_from_game() {   // PS3 NetGameManager::GetGameMode 0x49e4a0, inlined on PS4
    using LookupPointer = void *(U2MP_GUEST *)(uint32_t id);
    void *table = reinterpret_cast<LookupPointer>(image.base + kLookupPointer)(kGameModeTable);
    if (!table) return 0;
    const int index = get<int16_t>(image.base, kNetGameManager + kGameModeIndex);
    if (index < 0 || index >= get<int32_t>(table, 0)) return 0;
    const uint8_t *entries = get<const uint8_t *>(table, 8);
    return entries ? get<int32_t>(entries, size_t(index) * 0x90 + 0xc) : 0;
}

void *lookup_process_in_game(uint32_t id) {
    using Lookup = void *(U2MP_GUEST *)(void *unused, uint32_t id);
    return reinterpret_cast<Lookup>(image.base + kLookupProcess)(nullptr, id);
}

}  // namespace

// Types 0 (StartConnection), 60 (EarnMedalClient) and 65 (CompleteTask) have empty Read/Write on
// PS3 as well (blr at 0x468c18/0x468c1c, 0x468dc0/0x468dc4, 0x468dcc/0x468dd0): nothing to port.
const Port kPorts[] = {
    {37, "NetEventNpcSpawn", 0x1040f30, 0x20, 0x62f240, 0x62f230, 0x62f220, npc_spawn_read, npc_spawn_write, npc_spawn_apply},
    {38, "NetEventNpcKill", 0x1040760, 0x10, 0x622080, 0x622070, 0, npc_kill_read, npc_kill_write, nullptr},
    {44, "NetEventNpcDealDamage", 0x10405d0, 0x20, 0x61fe00, 0, 0, npc_deal_damage_read, npc_deal_damage_write, nullptr},
    {47, "NetEventEvent", 0x1040580, 0x100, 0x61fa60, 0x61fa50, 0, event_read, event_write, nullptr},
    {49, "NetEventTerritoryOwn", 0x1040440, 0x50, 0x61f030, 0x61f020, 0, territory_own_read, territory_own_write, nullptr},
    {51, "NetEventTerritorySpawn", 0x10403a0, 0x18, 0x61e890, 0x61e880, 0, territory_spawn_read, territory_spawn_write, nullptr},
    {52, "NetEventTerritorySetTimer", 0x1040350, 0x20, 0x61e700, 0x61e6f0, 0, territory_set_timer_read, territory_set_timer_write, nullptr},
    {53, "NetEventPlayVox", 0x1040300, 0x28, 0x61e600, 0x61e5f0, 0, play_vox_read, play_vox_write, nullptr},
    {54, "NetEventSimpleSnapshot", 0x10402b0, 0x40, 0x61e2a0, 0x61e290, 0, simple_snapshot_read, simple_snapshot_write, nullptr},
    {55, "NetEventStartNpcFight", 0x1040260, 0x80, 0x61e1b0, 0x61e1a0, 0, start_npc_fight_read, start_npc_fight_write, nullptr},
    {57, "NetEventSpawnHiddenTreasure", 0x10401c0, 0x30, 0x61d3a0, 0x61d390, 0, spawn_hidden_treasure_read, spawn_hidden_treasure_write, nullptr},
    {61, "NetEventEarnMedal", 0x1040120, 0x18, 0x61d240, 0x61d230, 0, earn_medal_read, earn_medal_write, nullptr},
    {67, "NetEventTurretSnapshot", 0x1040030, 0x20, 0x61bee0, 0x61bed0, 0, turret_snapshot_read, turret_snapshot_write, nullptr},
    {76, "NetEventKillEntity", 0x103fdb0, 0x10, 0x61afd0, 0x61afc0, 0, kill_entity_read, kill_entity_write, nullptr},
};
const size_t kPortCount = sizeof kPorts / sizeof kPorts[0];

namespace {
const Port *port_of(int type) {
    for (const Port &p : kPorts)
        if (p.type == type) return &p;
    return nullptr;
}
}  // namespace

void install_events(int &installed) {
    const uint64_t base = uint64_t(uintptr_t(image.base));
    if (!image.base || kFrameClock + 4 > image.size) return;
    game.frame_clock = reinterpret_cast<const int32_t *>(image.base + kFrameClock);
    game.game_mode = game_mode_from_game;
    game.lookup_process = lookup_process_in_game;
    int done = 0;
    for (const Port &p : kPorts) {
        if (p.vtable + 0x28 > image.size) continue;
        const struct {
            uint64_t stub;
            unsigned slot;
            void *host;
        } slots[] = {{p.read_stub, 2, reinterpret_cast<void *>(p.read)},
                     {p.write_stub, 3, reinterpret_cast<void *>(p.write)},
                     {p.apply_stub, 4, reinterpret_cast<void *>(p.apply)}};
        for (const auto &s : slots) {
            if (!s.stub || !s.host) continue;
            uint64_t &slot = *reinterpret_cast<uint64_t *>(image.base + p.vtable + 8 * s.slot);
            if (slot != base + s.stub || !is_ret_stub(s.stub)) {
                log("%s slot %u: expected the stub 0x%llx, left alone", p.name, s.slot, (unsigned long long)s.stub);
                continue;
            }
            slot = uint64_t(uintptr_t(s.host));
            ++done;
        }
    }
    log("net events: %d stubbed Read/Write/Apply slots ported", done);
    installed += done;
}

}  // namespace u2mp::events
