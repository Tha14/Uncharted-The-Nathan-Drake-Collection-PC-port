// u2mp netanim: remote-character animation phases and the NetAnimList event path
// (PS3 1.10 FillAnimCmdBitStream 0x480698, snapshot reader 0x4da608, NetAnimCmdGenerator slot 5
// 0x453c10 with 0x4539f8 / 0x45212c AddAnim / the NetAnimList visitor 0xd705d8). Notes:
// u2mp/docs/PORT-NETANIM.md.
//
// What PS4 1.02 lost (all three pieces are needed together):
//
// 1. The phase of every animation in the snapshot stream. PS3 writes, for each clip command (type 8)
//    of the local character, the layer type (2 bits), the anim index and then the clip phase
//    frame / (frames - 1) quantised to 4 bits (frames <= 8, x15), 16 bits (frames > 64, x65535) or
//    8 bits (x255) (0x480fe0..0x481068, 0x481b68, 0x481d4c). The reader reads the same bits
//    (scale 1/15, 1/65535, 1/255) and calls visitor slot 3 (anim, counter, phase) (0x4dab94..0x4dac08).
//    PS4 writer 0x634970 (clip case 0x6361d3..0x6364bb) writes the type and the index only; reader
//    0x66b430 (anim case 0x66bcd0..0x66c04c) resolves the anim and asserts it, then only counts it: no
//    phase read and no slot 3 call. So the visitors' slot 3 - 0x5f5770 (CreateAnimCmds visitor
//    0x100ac80: the only producer of the clip command 8 for a remote character), 0x5f5440 (locator
//    visitor 0x103fb10), 0x5f5b40 (snapshot debug visitor 0x100acd0) - is never reached.
//    Here: a call-out after the index write (0x6364b5) writes the phase as PS3 did, and a call-out at
//    0x66c035 reads it and calls slot 3 with the PS3 arguments. Both PCs need this build (stream format).
//
// 2. NetAnimCmdGenerator slot 4 (PS4 0x5f48d0, `ret`; PS3 slot 5 0x453c10). The engine anim layer
//    (vptr 0xffca20; NetCharacter setup gives it the generator at 0x601cc1, layer+0x40) calls it every
//    frame from 0x391570: gen->slot6(); gen->slot4(gen, layer+0x24 > [0xe5f550] ? events : NULL). It
//    keeps a 16-entry NetAnimList per generator (PS3 gen+1856, count +2112) and, for each animation
//    playing between the two snapshots around the generator time, fires the animation's events
//    (sounds / effects keyed to frames) from the last phase to the new one: PS3 0x7ab88 = PS4 0x4bcfd0
//    (present on PS4, no caller), into the layer's event list. Ported whole below (net_anim_update).
//    PS4 has no room for the list in the generator (reset 0x5f3250 clears +0x70..+0x954, time at
//    +0x960), so it lives in a side table keyed by the generator, cleared when the generator has fewer
//    than 3 snapshots (what follows a reset; PS3 0x453b74 cleared it there).
//
// 3. The NetAnimList visitor (PS3 vtable 0xd705d8: 0 reset 0x4522d0, 3 visit 0x452238 -> AddAnim,
//    4 mark 0x452d98, 1/2/5/6 empty) is gone on PS4; it is rebuilt here with the PS4 visitor ABI
//    (base layout of 0x66b430: +8/+0xc flags, +0x10/+0x20 vectors, +0x30 abort byte; base reset
//    0x66b270).
//
// Structure mapping PS3 -> PS4: anim +36 flags -> +0x24 (bit 0 looping), +40 info -> +0x28 (frames
// u16 +34 -> +0x22, float +44 -> +0x2c), +56 event table -> +0x48 (+0 id, +4 count); generator
// snapshots +64 stride 128 -> +0x70 stride 0x90 (time +4), count +56 -> +0x68, time +2124 -> +0x960,
// +20 -> +0x24, +24 -> +0x28; clip command {+4 anim, +8 index, +12 frame} -> {+4 anim (8 bytes),
// +0xc index, +0x10 frame}.
//
// Switch: UNCHARTED_U2MP_NETANIM=0 leaves all of it out (then on every PC: the stream format changes).
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "u2mp_internal.h"

namespace u2mp {
void install_netanim(int &installed);

namespace {

// --- addresses (PS4 1.02 image offsets) ---
constexpr uint64_t kSlot4 = 0x5f48d0;          // NetAnimCmdGenerator vtable 0x103fab0 slot 4 (ret)
constexpr uint64_t fReadSnapshot = 0x66b430;   // PS3 0x4da608 (visitor, snapshot, gen+0x28, a, b, gen+0x24)
constexpr uint64_t fFireEvents = 0x4bcfd0;     // PS3 0x7ab88
constexpr uint64_t fVisitorReset = 0x66b270;   // base visitor reset (PS3 0x4daf88)
constexpr uint64_t kWriterSite = 0x6364b5;     // after the clip index write: inc r10d; inc r14d; jmp 0x638000
constexpr uint64_t kWriterResume = 0x638000;
constexpr uint64_t kReaderSite = 0x66c035;     // anim resolved: mov r14,[rbp-0x188]; counter++; jmp 0x66c250
constexpr uint64_t kReaderResume = 0x66c250;

// generator (PS3 NetAnimCmdGenerator)
constexpr uint64_t G_WORD24 = 0x24, G_ANIMS = 0x28, G_COUNT = 0x68, G_SNAPS = 0x70, G_SNAP_SIZE = 0x90,
                   G_TIME = 0x960, SNAP_TIME = 4;
// anim
constexpr uint64_t A_FLAGS = 0x24, A_INFO = 0x28, A_EVENTS = 0x48, I_FRAMES = 0x22, I_RATE = 0x2c;
// reader frame (0x66b430): bit reader at rbp-0x150, visitor rbp-0x190, anim counter rbp-0x180
constexpr int64_t R_READER = -0x150, R_VISITOR = -0x190, R_COUNTER = -0x180;

// PS3 constants (0x453c10 r30 = 0xdb8dac; reader 0xdbb6b0; writer 0xdb96e8)
constexpr float k30 = 30.0f, k01 = 0.1f, k015 = 0.15f, k085 = 0.85f;

using ReadSnapshotFn = void(U2MP_GUEST *)(void *, const uint8_t *, const uint8_t *, uint32_t, uint32_t,
                                          uint32_t);
using FireEventsFn = void(U2MP_GUEST *)(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, void *,
                                        float, float, float);
using VisitorResetFn = void(U2MP_GUEST *)(void *);
using VisitFn = void(U2MP_GUEST *)(void *, const uint8_t *, uint32_t, float);

template <typename T> T rd(const uint8_t *p, int64_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
const uint8_t *rdp(const uint8_t *p, int64_t off) { return rd<const uint8_t *>(p, off); }

// --- NetAnimList (PS3 0x45212c: 16-byte entries {key, anim, phase, dir, mark}, count +256, max 16) ---
struct Entry {
    uint32_t key;
    const uint8_t *anim;
    float phase;
    uint8_t dir;    // +12: 1 forward, 0 backward
    uint8_t mark;   // +13
};
struct List {
    Entry e[16];
    uint32_t n;
};

void add_anim(List *l, uint32_t key, const uint8_t *anim, float phase, uint8_t dir) {
    if (l->n > 15) return;   // PS3 asserts "m_numAnims < kMaxAnims", then skips the store
    Entry &x = l->e[l->n++];
    x.key = key;
    x.mark = 0;
    x.anim = anim;
    x.phase = phase;
    x.dir = dir;
}

// --- the NetAnimList visitor (PS3 vtable 0xd705d8) in the PS4 visitor layout ---
struct alignas(16) Visitor {
    const void *const *vt;    // +0
    uint32_t a, b;            // +8, +0xc (set by 0x66b430)
    uint8_t base[0x28];       // +0x10 vector, +0x20 vector, +0x30 abort byte (base reset 0x66b270)
    List *list;               // +0x38 (PS3 +52)
    uint32_t last;            // +0x40 (PS3 +56)
};
static_assert(offsetof(Visitor, list) == 0x38, "visitor layout");

U2MP_GUEST void v_reset(Visitor *v) {   // PS3 0x4522d0
    reinterpret_cast<VisitorResetFn>(image.base + fVisitorReset)(v);
    v->last = 0;
}
U2MP_GUEST void v_empty(Visitor *) {}   // PS3 0x451b30 / 0x451b34 / 0x451b3c / 0x451b40
U2MP_GUEST void v_visit(Visitor *v, const uint8_t *anim, uint32_t, float phase) {   // PS3 0x452238
    v->last = 0;
    if (!anim) return;
    const uint8_t *ev = rdp(anim, A_EVENTS);
    if (!ev || rd<uint32_t>(ev, 4) == 0 || !rdp(anim, A_INFO)) return;
    v->last = rd<uint32_t>(ev, 0);
    add_anim(v->list, v->last, anim, phase, 1);
}
U2MP_GUEST void v_mark(Visitor *v, uint32_t) {   // PS3 0x452d98
    if (!v->last) return;
    List *l = v->list;
    for (int j = int(l->n) - 1; j >= 0; --j)
        if (l->e[j].key == v->last) {
            l->e[j].mark = 1;
            return;
        }
}
const void *const kVisitorVt[7] = {reinterpret_cast<const void *>(&v_reset), reinterpret_cast<const void *>(&v_empty),
                                    reinterpret_cast<const void *>(&v_empty), reinterpret_cast<const void *>(&v_visit),
                                    reinterpret_cast<const void *>(&v_mark), reinterpret_cast<const void *>(&v_empty),
                                    reinterpret_cast<const void *>(&v_empty)};

// --- per-generator list (PS3 gen+1856) ---
std::mutex g_lists_lock;
std::unordered_map<const void *, std::unique_ptr<List>> g_lists;

List *old_list(const uint8_t *gen) {
    std::lock_guard<std::mutex> lock(g_lists_lock);
    auto &p = g_lists[gen];
    if (!p) p = std::make_unique<List>(List{});
    return p.get();
}

// PS3 0x451d50 on the PS4 layout (also inlined in PS4 0x5f3f70 / 0x5f48e0).
bool snapshot_pair(const uint8_t *gen, int *idx, float *frac) {
    const uint32_t n = rd<uint32_t>(gen, G_COUNT);
    if (n <= 2) return false;
    const float time = rd<float>(gen, G_TIME);
    *idx = 0;
    *frac = 0.0f;
    for (uint32_t i = 0; i + 1 < n; ++i) {
        const uint8_t *s0 = gen + G_SNAPS + i * G_SNAP_SIZE, *s1 = s0 + G_SNAP_SIZE;
        const float t1 = rd<float>(s1, SNAP_TIME);
        if (t1 == 0.0f) continue;
        const float t0 = rd<float>(s0, SNAP_TIME);
        if (t0 < time) {
            *idx = int(i);
            *frac = 0.0f;
            return true;
        }
        if (t1 < time) {
            *idx = int(i);
            *frac = (t0 - time) / (t0 - t1);
            return true;
        }
    }
    return true;
}

// PS3 0x4539f8: the newer snapshot's anims into `newer`, the older one's into `older`, and the time
// between the older snapshot and the generator time.
bool build_lists(const uint8_t *gen, List *older, List *newer, float *dt) {
    int idx;
    float frac;
    if (!snapshot_pair(gen, &idx, &frac)) return false;
    const auto read = reinterpret_cast<ReadSnapshotFn>(image.base + fReadSnapshot);
    Visitor v{};
    v.vt = kVisitorVt;
    const uint8_t *s0 = gen + G_SNAPS + uint64_t(idx) * G_SNAP_SIZE, *s1 = s0 + G_SNAP_SIZE;
    const uint32_t w24 = rd<uint32_t>(gen, G_WORD24);
    v.list = newer;
    read(&v, s0, gen + G_ANIMS, 0, 0, w24);
    v.list = older;
    read(&v, s1, gen + G_ANIMS, 1, 1, w24);
    const float t0 = rd<float>(s0, SNAP_TIME), t1 = rd<float>(s1, SNAP_TIME);
    *dt = (1.0f - frac) * t0 + t1 * frac - t1;
    return true;
}

// VMX vminfp / vmaxfp (a NaN operand gives NaN) and fsel.
float vmin(float a, float b) { return (std::isnan(a) || std::isnan(b)) ? NAN : (a < b ? a : b); }
float vmax(float a, float b) { return (std::isnan(a) || std::isnan(b)) ? NAN : (a > b ? a : b); }
float fsel(float a, float b, float c) { return a >= 0.0f ? b : c; }

// The new phase of an older-snapshot anim advanced by dt (PS3 0x454384.. / 0x45442c.. and the copies
// at 0x4546f0 / 0x4547c8).
float advance(float frames1, float phase, float dt30, bool forward) {
    const float v = forward ? frames1 * phase + dt30 : frames1 * phase - dt30;
    const float c = vmax(vmin(v, frames1), 0.0f);
    return vmax(vmin(c / frames1, 1.0f), 0.0f);
}

struct AnimInfo {
    const uint8_t *anim, *events;
    uint32_t loop;
    float frames1, dur;
};
AnimInfo anim_info(const uint8_t *anim) {
    AnimInfo r{anim, rdp(anim, A_EVENTS), rd<uint32_t>(anim, A_FLAGS) & 1u, 0.0f, 0.0f};
    const uint8_t *info = rdp(anim, A_INFO);
    r.frames1 = float(rd<uint16_t>(info, I_FRAMES)) - 1.0f;
    r.dur = r.frames1 * (rd<float>(info, I_RATE) * k30);
    return r;
}

void fire(const AnimInfo &a, bool forward, float from, float to, void *events) {
    reinterpret_cast<FireEventsFn>(image.base + fFireEvents)(a.events, forward ? 1 : 0, a.loop, 0, 1, events,
                                                             a.dur * from, a.dur * to, a.dur);
}

// PS3 0x453fa4: keep the list while one of its anims has not reached its end, else clear it.
void keep_or_clear(List *o) {
    for (uint32_t i = 0; i < o->n; ++i) {
        const Entry &e = o->e[i];
        if (e.dir ? e.phase < 1.0f : e.phase > 0.0f) return;
    }
    *o = List{};
}

bool same_keys(const List &x, const List &y, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i)
        if (x.e[i].key != y.e[i].key) return false;
    return true;
}

// PS3 0x4540e0: start a new list from the two snapshots.
void rebuild(List *o, const List &older, const List &newer) {
    *o = List{};
    for (uint32_t i = 0; i < older.n; ++i) {
        const Entry &a = older.e[i], &b = newer.e[i];
        const bool loop = (rd<uint32_t>(a.anim, A_FLAGS) & 1u) != 0;
        int dir = -1;
        if (loop) {
            if (a.phase - b.phase > k01) dir = 1;
            else if (a.phase < b.phase) dir = 1;
            else if (b.phase - a.phase > k01) dir = 0;
            else if (a.phase > b.phase) dir = 0;
        } else {
            if (a.phase < b.phase) dir = 1;
            else if (a.phase > b.phase) dir = 0;
        }
        if (dir == 1) add_anim(o, a.key, nullptr, fsel(a.phase - k015, a.phase, 0.0f), 1);
        else if (dir == 0) add_anim(o, a.key, nullptr, fsel(k085 - a.phase, a.phase, 1.0f), 0);
    }
}

// PS3 0x454298: same anims as the list, the newer snapshot differs.
void advance_main(List *o, const List &older, float dt, void *events) {
    const float dt30 = dt * k30;
    for (uint32_t i = 0; i < older.n; ++i) {
        const Entry &a = older.e[i];
        bool dup = false;
        for (uint32_t j = 0; j < i; ++j)
            if (older.e[j].key == a.key) dup = true;
        const AnimInfo ai = anim_info(a.anim);
        Entry &e = o->e[i];
        const float old = e.phase;
        if (e.dir) {
            const float np = advance(ai.frames1, a.phase, dt30, true);
            e.phase = fsel(old - np, old, np);
            if (np > old && !dup) fire(ai, true, old, np, events);
        } else {
            const float np = advance(ai.frames1, a.phase, dt30, false);
            e.phase = fsel(old - np, np, old);
            if (np < old && !dup) fire(ai, false, old, np, events);
        }
    }
}

// PS3 0x454544: same anims in both snapshots and the list.
void advance_same(List *o, const List &older, const List &newer, float dt, void *events) {
    const float dt30 = dt * k30;
    for (uint32_t i = 0; i < older.n; ++i) {
        const Entry &a = older.e[i], &b = newer.e[i];
        bool found = false;
        for (uint32_t j = 0; j < i && !found; ++j)
            if (older.e[j].key == a.key && std::fabs(older.e[j].phase - a.phase) < k01) found = true;
        const AnimInfo ai = anim_info(a.anim);
        Entry &e = o->e[i];
        const float old = e.phase;
        if (e.dir && b.phase > a.phase) {
            const float np = advance(ai.frames1, a.phase, dt30, true);
            e.phase = fsel(old - np, old, np);
            if (np > old && !found) fire(ai, true, old, np, events);
            continue;
        }
        if (!ai.loop) {
            if (e.dir) continue;
        } else if (e.dir) {
            if (a.phase - b.phase > k01) e.phase = 0.0f;
            continue;
        }
        // dir == 0
        if (a.phase > b.phase) {
            const float np = advance(ai.frames1, a.phase, dt30, false);
            e.phase = fsel(old - np, np, old);
            if (np < old && !found) fire(ai, false, old, np, events);
            continue;
        }
        if (ai.loop && b.phase - a.phase > k01) e.phase = 1.0f;
    }
}

int g_slot_calls = 0;

// NetAnimCmdGenerator slot 4 (PS3 slot 5 0x453c10).
U2MP_GUEST void net_anim_update(const uint8_t *gen, void *events) {
    if (g_slot_calls < 3 && ++g_slot_calls) log("netanim: generator slot called (generator %p)", (const void *)gen);
    List *o = old_list(gen);
    if (rd<uint32_t>(gen, G_COUNT) < 3) *o = List{};   // what a generator reset (PS3 0x453b74) leaves
    List older{}, newer{};
    float dt = 0.0f;
    const bool ok = build_lists(gen, &older, &newer, &dt);
    if (ok) {
        if (!events) return;
        const uint32_t n = older.n;
        if (o->n == n && same_keys(older, *o, n)) {
            if (n == 0) {
                keep_or_clear(o);
            } else if (newer.n == n && same_keys(newer, older, n)) {
                advance_same(o, older, newer, dt, events);
                keep_or_clear(o);
            } else {
                advance_main(o, older, dt, events);
                keep_or_clear(o);
            }
            return;
        }
        if (newer.n == n && same_keys(newer, older, n)) {
            rebuild(o, older, newer);
            keep_or_clear(o);
            return;
        }
    }
    keep_or_clear(o);
}

// --- snapshot stream: the clip phase (PS3 0x480fe0 writer, 0x4dab94 reader) ---
struct Bits {   // PS4 bit stream {+0 u32 *words, +8 position, +0xc limit}, MSB first
    uint32_t *w;
    uint32_t pos, limit;
};
static_assert(sizeof(Bits) == 16, "bit stream layout");

void phase_bits(uint32_t frames, uint32_t *n, float *k) {
    if (frames <= 8) *n = 4, *k = 15.0f;
    else if (frames > 64) *n = 16, *k = 65535.0f;
    else *n = 8, *k = 255.0f;
}

int32_t fctiwz(float x) {
    if (std::isnan(x)) return INT32_MIN;
    if (x >= 2147483648.0f) return INT32_MAX;
    if (x <= -2147483648.0f) return INT32_MIN;
    return int32_t(x);
}

int g_overflow_logs = 0;

// After the clip command's type and index (PS4 0x6364b5): cmd = r12, stream = r15.
U2MP_GUEST void write_phase(const uint8_t *cmd, Bits *s) {
    const uint8_t *anim = rdp(cmd, 4);
    const uint8_t *info = anim ? rdp(anim, A_INFO) : nullptr;
    uint32_t n;
    float k;
    int32_t q = 0;
    if (info) {
        const uint32_t frames = rd<uint16_t>(info, I_FRAMES);
        phase_bits(frames, &n, &k);
        q = fctiwz(rd<float>(cmd, 0x10) * k / (float(frames) - 1.0f));
    } else {
        n = 8;
    }
    if (uint64_t(s->pos) + n > s->limit) {
        if (g_overflow_logs < 8 && ++g_overflow_logs) log("netanim: snapshot stream full, clip phase not written");
        return;
    }
    // the PS4 writer's own sequence (x86 shift counts are taken mod 32)
    const uint32_t v = (uint32_t(q) & 0xffffu & ((1u << n) - 1u)) << (32 - n);
    const uint32_t idx = s->pos >> 5, off = s->pos & 31, rem = 32 - off;
    const uint32_t keep = (rem & 31) ? (~0u << rem) : ~0u;
    s->w[idx] = (s->w[idx] & keep) | (v >> off);
    if (rem < n) s->w[idx + 1] = v << rem;
    s->pos += n;
}

// Anim case of the reader (PS4 0x66c035): rax = anim, rbp = the reader's frame.
U2MP_GUEST void read_phase(const uint8_t *anim, uint8_t *frame) {
    Bits *s = reinterpret_cast<Bits *>(frame + R_READER);
    void *visitor = rd<void *>(frame, R_VISITOR);
    const uint32_t counter = rd<uint32_t>(frame, R_COUNTER);
    const uint8_t *info = anim ? rdp(anim, A_INFO) : nullptr;
    uint32_t n = 8;
    float k = 255.0f;
    if (info) phase_bits(rd<uint16_t>(info, I_FRAMES), &n, &k);
    if (uint64_t(s->pos) + n > s->limit) {
        if (g_overflow_logs < 8 && ++g_overflow_logs) log("netanim: snapshot stream ends before a clip phase");
        return;
    }
    const uint32_t idx = s->pos >> 5, off = s->pos & 31, rem = 32 - off;
    uint32_t v = (s->w[idx] << off) >> (32 - n);
    if (rem < n) v |= s->w[idx + 1] >> (rem + 32 - n);
    s->pos += n;
    if (!anim || !visitor) return;
    const float phase = float(v) * (1.0f / k);
    const auto vt = *reinterpret_cast<const VisitFn *const *>(visitor);
    vt[3](visitor, anim, counter, phase);
}

// --- call-out stubs ---
// lea rsp,[rsp-0x80] (red zone); push rax..r11, rbx; rbx = rsp; align; save xmm0-15; rdi/rsi from
// `args`; call host; restore; then `post`, then jmp resume.
unsigned char *callout(const unsigned char *pre, size_t pre_len, const unsigned char *args, size_t args_len,
                       void *host, const unsigned char *post, size_t post_len, uint64_t resume) {
    unsigned char *s = near_stub(512);
    if (!s) return nullptr;
    unsigned char *p = s;
    auto put = [&](std::initializer_list<unsigned char> b) {
        for (unsigned char c : b) *p++ = c;
    };
    auto putn = [&](const unsigned char *b, size_t n) {
        std::memcpy(p, b, n);
        p += n;
    };
    putn(pre, pre_len);
    put({0x48, 0x8d, 0x64, 0x24, 0x80});                                   // lea rsp,[rsp-0x80]
    put({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x53});   // push rax,rcx,rdx,rsi,rdi,r8-r11,rbx
    put({0x48, 0x89, 0xe3});                                               // mov rbx,rsp
    put({0x48, 0x83, 0xe4, 0xf0});                                         // and rsp,-16
    put({0x48, 0x81, 0xec, 0x00, 0x01, 0x00, 0x00});                       // sub rsp,0x100
    for (int i = 0; i < 16; ++i) {                                         // movups [rsp+16*i],xmm_i
        if (i >= 8) *p++ = 0x44;
        put({0x0f, 0x11, uint8_t(0x84 | ((i & 7) << 3)), 0x24});
        const int32_t d = 16 * i;
        std::memcpy(p, &d, 4);
        p += 4;
    }
    putn(args, args_len);
    put({0x48, 0xb8});                                                     // mov rax,host
    const uint64_t h = uint64_t(uintptr_t(host));
    std::memcpy(p, &h, 8);
    p += 8;
    put({0xff, 0xd0});                                                     // call rax
    for (int i = 0; i < 16; ++i) {                                         // movups xmm_i,[rsp+16*i]
        if (i >= 8) *p++ = 0x44;
        put({0x0f, 0x10, uint8_t(0x84 | ((i & 7) << 3)), 0x24});
        const int32_t d = 16 * i;
        std::memcpy(p, &d, 4);
        p += 4;
    }
    put({0x48, 0x89, 0xdc});                                               // mov rsp,rbx
    put({0x5b, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});   // pop in reverse
    put({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00});                 // lea rsp,[rsp+0x80]
    putn(post, post_len);
    put({0xff, 0x25, 0x00, 0x00, 0x00, 0x00});                             // jmp [rip+0]
    const uint64_t r = uint64_t(uintptr_t(image.base + resume));
    std::memcpy(p, &r, 8);
    p += 8;
    return s;
}

bool jump_to(uint64_t site, unsigned length, const unsigned char *stub) {
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + site + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    unsigned char *code = image.base + site;
    const int32_t rel = int32_t(delta);
    code[0] = 0xe9;
    std::memcpy(code + 1, &rel, 4);
    std::memset(code + 5, 0xcc, length - 5);
    return true;
}

}  // namespace

void install_netanim(int &installed) {
    const char *off = std::getenv("UNCHARTED_U2MP_NETANIM");
    if (off && std::strcmp(off, "0") == 0) {
        log("netanim: off (UNCHARTED_U2MP_NETANIM=0)");
        return;
    }
    // Writer site 0x6364b5: inc r10d; inc r14d; jmp 0x638000.
    static const unsigned char writer_site[] = {0x41, 0xff, 0xc2, 0x41, 0xff, 0xc6, 0xe9, 0x40, 0x1b, 0x00, 0x00};
    // ... and the clip case around it: cmp r11d,[rsp+0x54]; je 0x6364b5 / jae 0x6364b5 (index write end).
    static const unsigned char writer_skip[] = {0x44, 0x3b, 0x5c, 0x24, 0x54, 0x0f, 0x84, 0xb4};
    static const unsigned char writer_jae[] = {0x73, 0x47};
    // Reader site 0x66c035: mov [rbp-0x188],r14; mov rax,[rbp-0x180]; inc eax; mov [rbp-0x180],rax; jmp 0x66c250.
    static const unsigned char reader_site[] = {0x4c, 0x89, 0xb5, 0x78, 0xfe, 0xff, 0xff, 0x48, 0x8b, 0x85,
                                                0x80, 0xfe, 0xff, 0xff, 0xff, 0xc0, 0x48, 0x89, 0x85, 0x80,
                                                0xfe, 0xff, 0xff, 0xe9, 0xff, 0x01, 0x00, 0x00};
    // ... reached with the resolved anim in rax: test rax,rax; jne 0x66c035.
    static const unsigned char reader_test[] = {0x48, 0x85, 0xc0, 0x75, 0x35};
    // Helpers: prologues of 0x66b430 (reader) and 0x4bcfd0 (events); the whole base visitor reset.
    static const unsigned char read_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                  0x41, 0x54, 0x53, 0x48, 0x81, 0xec, 0x88, 0x01, 0x00, 0x00};
    static const unsigned char fire_prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
                                                  0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x18, 0x4d, 0x89, 0xce};
    static const unsigned char reset_body[] = {0xc5, 0xf8, 0x57, 0xc0, 0xc5, 0xf8, 0x29, 0x47, 0x10,
                                               0xc5, 0xf8, 0x28, 0x05, 0xbf, 0xa1, 0x7c, 0x00, 0xc5,
                                               0xf8, 0x29, 0x47, 0x20, 0xc6, 0x47, 0x30, 0x00, 0xc3};
    // Slot 4 0x5f48d0: ret and padding up to 0x5f48e0; the layer's call of it (0x391597: mov rax,[rax+0x20]).
    static const unsigned char slot_stub[] = {0xc3, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x2e,
                                              0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char layer_call[] = {0x48, 0x8b, 0x7b, 0x40, 0x48, 0x8b, 0x07, 0x48, 0x8b, 0x40, 0x20};

    const bool stream_ok = image.matches(kWriterSite, writer_site, sizeof writer_site) &&
                           image.matches(0x6363f6, writer_skip, sizeof writer_skip) &&
                           image.matches(0x63646c, writer_jae, sizeof writer_jae) &&
                           image.matches(kReaderSite, reader_site, sizeof reader_site) &&
                           image.matches(0x66bffb, reader_test, sizeof reader_test) &&
                           image.matches(fReadSnapshot, read_prologue, sizeof read_prologue);
    if (!stream_ok) {
        log("netanim: snapshot writer / reader bytes differ from 1.02, left alone (no clip phases)");
        return;
    }
    // Writer: no instruction before the call; after it the three replaced instructions' first two.
    static const unsigned char w_args[] = {0x4c, 0x89, 0xe7, 0x4c, 0x89, 0xfe};        // mov rdi,r12; mov rsi,r15
    static const unsigned char w_post[] = {0x41, 0xff, 0xc2, 0x41, 0xff, 0xc6};        // inc r10d; inc r14d
    // Reader: the replaced store first, then the call, then counter++.
    static const unsigned char r_pre[] = {0x4c, 0x89, 0xb5, 0x78, 0xfe, 0xff, 0xff};   // mov [rbp-0x188],r14
    static const unsigned char r_args[] = {0x48, 0x8b, 0x7b, 0x48, 0x48, 0x89, 0xee}; // mov rdi,[rbx+0x48] (saved rax); mov rsi,rbp
    static const unsigned char r_post[] = {0x48, 0x8b, 0x85, 0x80, 0xfe, 0xff, 0xff, 0xff, 0xc0,
                                           0x48, 0x89, 0x85, 0x80, 0xfe, 0xff, 0xff};   // counter++
    unsigned char *ws = callout(nullptr, 0, w_args, sizeof w_args, reinterpret_cast<void *>(&write_phase), w_post,
                                sizeof w_post, kWriterResume);
    unsigned char *rs = callout(r_pre, sizeof r_pre, r_args, sizeof r_args, reinterpret_cast<void *>(&read_phase),
                                r_post, sizeof r_post, kReaderResume);
    if (!ws || !rs) {
        log("netanim: no stub memory near the image");
        return;
    }
    if (!jump_to(kWriterSite, sizeof writer_site, ws) || !jump_to(kReaderSite, sizeof reader_site, rs)) {
        log("netanim: stub out of jump range");
        return;
    }
    installed += 2;
    log("netanim: snapshot clip phases restored (writer 0x6364b5, reader 0x66c035; both PCs need this build)");

    if (!is_ret_stub(kSlot4) || !image.matches(kSlot4, slot_stub, sizeof slot_stub) ||
        !image.matches(0x391590, layer_call, sizeof layer_call) ||
        !image.matches(fFireEvents, fire_prologue, sizeof fire_prologue) ||
        !image.matches(fVisitorReset, reset_body, sizeof reset_body)) {
        log("netanim: generator slot 4 / helpers differ from 1.02, slot left as it is");
        return;
    }
    if (replace_function(kSlot4, sizeof slot_stub, reinterpret_cast<void *>(&net_anim_update),
                         "NetAnimCmdGenerator slot 4 (PS3 0x453c10)"))
        ++installed;
}

}  // namespace u2mp
