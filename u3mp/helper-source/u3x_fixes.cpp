// u3x_fixes.cpp: helper code written in C++ (v7.111 on). Compiled by tools/u3x_splice (clang++, freestanding, the helper's
// flags) to assembly that is spliced into u3_lan_profiles.c between the "u3x" markers, so the existing helper build links it.
// Rules for this file: every function in .text.ext (U3X_FN), every constant in .rodata.ext (U3X_RO), every variable in
// .bss.u3ext.x3x (U3X_BSS); no string literals outside U3X_RO arrays (they would land in region A); no floats (the helper
// is built without SSE: float maths through the f_* helpers below, as the C part's kr_port.inc); entry points extern "C".
#include <stdint.h>

#define U3X_FN [[gnu::section(".text.ext"), gnu::noinline]]
#define U3X_RO [[gnu::section(".rodata.ext")]]
#define U3X_BSS [[gnu::section(".bss.u3ext.x3x"), gnu::aligned(16), gnu::used]]

namespace {

using Log = void (*)(uintptr_t base, const char *line);

U3X_RO const char hexdig[] = {'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f'};

constexpr uint32_t F_ONE = 0x3f800000u;

[[gnu::always_inline]] inline uint32_t f_add(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\taddss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    return r;
}
[[gnu::always_inline]] inline uint32_t f_sub(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tsubss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    return r;
}
[[gnu::always_inline]] inline uint32_t f_mul(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tmulss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    return r;
}
[[gnu::always_inline]] inline bool ptr_ok(const void *p) {   // = the C part's stage_ptr_ok
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47) && !(v & 7);
}
template <typename T> [[gnu::always_inline]] inline T rd(const void *p, uintptr_t off) {
    return *reinterpret_cast<const T *>(static_cast<const uint8_t *>(p) + off);
}

// a line for boot.log: text + decimal or hex numbers, at most 190 characters
struct Line {
    char b[200];
    int n = 0;
    [[gnu::always_inline]] Line &s(const char *t) { while (*t && n < 190) b[n++] = *t++; return *this; }
    [[gnu::always_inline]] Line &d(uint32_t v) {
        char t[12]; int k = 0;
        do { t[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (k && n < 190) b[n++] = t[--k];
        return *this;
    }
    [[gnu::always_inline]] Line &x(uint64_t v, int digits) {
        if (n < 180) { b[n++] = '0'; b[n++] = 'x'; }
        for (int i = (digits - 1) * 4; i >= 0 && n < 190; i -= 4) b[n++] = hexdig[(v >> i) & 15];
        return *this;
    }
    [[gnu::always_inline]] const char *z() { b[n] = 0; return b; }
};

}  // namespace

// ===== HUD element icons (knocked-down teammate), PS3 1.19 f_672404 =====
// PS3 draws an icon in the element's ring by mode +80 (0x6737e8..0x673f5c): mode 1 = a teammate knocked down, mode 2 = grabbed.
// Mode 1 (0x673864..0x67397c): the knocked-down icon (module +3952 = PS4 +0x10b0, "general/net/coop-icons/knocked-down"),
// or the triangle (+3940) when 0x25c1ac's distance check passes and the local player's byte +2796 is set; the triangle needs
// those two mapped on PS4, so the knocked-down icon is drawn in every case for now. The quad is mode 2's (g3_icon, PS3
// 0x673b9c): half size elem+8 * 0.5 * 60, colours (1,1,1,a) with a = elem+0x3c * screen alpha, depth elem+0x10, drawn by
// 0xe42310 with the slot's material from 0x11b89b0. PS4 compiled the whole icon block away (0xc3106c), so PC drew the ring only.
struct U3xIcons {
    uint32_t calls[8], drawn[8], refused[8], logged[8], lines;
};
U3X_BSS static U3xIcons u3x_icons;

U3X_RO static const char t_icon[] = "U3MP down: ", t_m1[] = "knocked-down icon", t_m[] = "icon of mode ",
    t_sid[] = ": slot sid ", t_idx[] = " index ", t_mat[] = ", material ", t_dr[] = ", drawn ", t_rf[] = ", refused ",
    t_none[] = ": no material (the icon texture is not loaded), not drawn";

extern "C" U3X_FN void u3x_mode_icon(uintptr_t b, const uint8_t *el, const uint8_t *mod, uint32_t x, uint32_t y,
                                     uint32_t mode, Log log) {
    const uint32_t m = mode & 7u;
    u3x_icons.calls[m]++;
    const uint8_t *slot = mod + (mode == 1 ? 0x10b0u : 0x10bcu);
    const uint32_t idx = rd<uint32_t>(slot, 8);
    const uint64_t mat = reinterpret_cast<uint64_t (*)(uint64_t, uint32_t)>(b + 0x11b89b0u)(rd<uint64_t>(slot, 0), idx);
    if (!mat) {
        u3x_icons.refused[m]++;
        if (log && !u3x_icons.logged[m] && u3x_icons.lines < 20) {
            u3x_icons.logged[m] = 1; u3x_icons.lines++;
            Line l; l.s(t_icon); if (mode == 1) l.s(t_m1); else l.s(t_m).d(mode);
            log(b, l.s(t_none).z());
        }
        return;
    }
    const uint32_t h = f_mul(f_mul(rd<uint32_t>(el, 8), 0x3f000000u), 0x42700000u);   // s * 0.5 * 60
    const uint8_t *hud = *reinterpret_cast<uint8_t *const *>(b + 0x1806480u);
    uint32_t scr = 0;
    if (ptr_ok(hud)) { scr = rd<uint32_t>(hud, 0x1d0); if (scr > 1) scr = 0; }
    const uint32_t a = f_mul(rd<uint32_t>(el, 0x3c), *reinterpret_cast<const uint32_t *>(b + 0x16eafe0u + 4u * scr));
    const uint32_t x0 = f_sub(x, h), x1 = f_add(x, h), y0 = f_sub(y, h), y1 = f_add(y, h);
    alignas(16) uint32_t v[0x44];
    for (int k = 0; k < 0x44; k++) v[k] = 0;
    const uint32_t px[4] = {x0, x1, x1, x0}, py[4] = {y0, y0, y1, y1}, pu[4] = {0, F_ONE, F_ONE, 0}, pv[4] = {0, 0, F_ONE, F_ONE};
    for (int k = 0; k < 4; k++) {
        uint32_t *w = v + k * 16;
        w[0] = px[k]; w[1] = py[k]; w[3] = pu[k]; w[4] = pv[k];
        w[8] = w[9] = w[10] = F_ONE; w[11] = a; w[12] = w[13] = w[14] = w[15] = F_ONE;
    }
    v[0x40] = rd<uint32_t>(el, 0x10);
    uint64_t ctx[6] = {}, td[4] = {mat, 0, 0, 0};
    reinterpret_cast<void (*)(uint64_t *, uint64_t *, void *)>(b + 0xe42310u)(ctx, td, v);
    u3x_icons.drawn[m]++;
    if (log && !u3x_icons.logged[m] && u3x_icons.lines < 20) {
        u3x_icons.logged[m] = 1; u3x_icons.lines++;
        Line l; l.s(t_icon); if (mode == 1) l.s(t_m1); else l.s(t_m).d(mode);
        l.s(t_sid).x(rd<uint32_t>(slot, 0), 8).s(t_idx).d(idx).s(t_mat).x(mat, 12).s(t_dr).d(u3x_icons.drawn[m]);
        log(b, l.z());
    }
}

// ===== checkpoint ring colour: the PS user data the ring draw runs with =====
// n033 / r01 (C part) recolour the ring by its PS constant ((PS user table +8) + 0x148), found from SET_SH_REG packets of
// USER_DATA_PS_0/1 that the draw emits. The 2026-11-12 test matched the tinted checkpoint ring draws (block colour 10, 0.5,
// 0.5) but found no user-data register in what those draws emitted ("regs 0", no fix): the registers were set by an earlier
// draw in the same command buffer and kept. This walks the command buffer from its start (Gnm CommandBuffer +0 begin,
// +0x10 write pointer, as n33_pre reads) up to the draw's end and returns the last values written to USER_DATA_VS_0/1
// (0x4c/0x4d) and USER_DATA_PS_0/1 (0x0c/0x0d): the ones in effect for the ring draw. Bits as n33_post's: 1 VS lo, 2 VS hi,
// 4 PS lo, 8 PS hi; 16 = the walk ran; 0 = the buffer start is not usable.
struct U3xRing {
    uint32_t walks, found, bad, last_dw;
};
U3X_BSS static U3xRing u3x_ring;

extern "C" U3X_FN uint32_t u3x_ring_state(const uint8_t *cmd, const uint32_t *end, uint32_t *vlo, uint32_t *vhi, uint32_t *plo,
                                          uint32_t *phi) {
    if (!ptr_ok(cmd)) { u3x_ring.bad++; return 0; }
    const uint32_t *p = *reinterpret_cast<const uint32_t *const *>(cmd);
    if (!p || (reinterpret_cast<uintptr_t>(p) & 3) || reinterpret_cast<uintptr_t>(p) >= (1ull << 47) || !end || end < p ||
        end - p > (1 << 20)) { u3x_ring.bad++; return 0; }
    u3x_ring.walks++;
    u3x_ring.last_dw = static_cast<uint32_t>(end - p);
    uint32_t got = 16;
    while (p < end) {
        const uint32_t hd = *p, t = hd >> 30;
        if (t == 3) {
            const uint32_t c = ((hd >> 16) & 0x3fffu) + 1u, op = (hd >> 8) & 0xffu;
            if (p + 1 + c > end) break;
            if (op == 0x76u && c >= 2) {   // SET_SH_REG
                const uint32_t reg = p[1] & 0xffffu;
                for (uint32_t i = 0; i + 1 < c; i++) {
                    const uint32_t rg = reg + i, v = p[2 + i];
                    if (rg == 0x4cu) { *vlo = v; got |= 1; }
                    else if (rg == 0x4du) { *vhi = v; got |= 2; }
                    else if (rg == 0x0cu) { *plo = v; got |= 4; }
                    else if (rg == 0x0du) { *phi = v; got |= 8; }
                }
            }
            p += 1 + c;
        } else if (t == 2) {
            p++;
        } else {
            p += 1 + (((hd >> 16) & 0x3fffu) + 1u);
        }
    }
    if ((got & 12) == 12) u3x_ring.found++;
    return got;
}

// ===== event diagnostic (v7.112): which game events go to and come from which peer =====
// 2026-11-12 Syria, 3 players: the host started the round (its round start event ran, co-op stage 1) but neither guest ever
// ran the round start (stage 0 all match: they could not move, no enemies). Nothing logged events per peer. This watches the
// game transport's queues of every connection (helper gt_pump, sub = entry +0x40): +0x110 reliable and +0x120 unreliable
// (to send), +0x138 received (after a read), nodes {+0 event, +0x20 next}. Each kind of event (its vtable, as an image
// offset) is logged the first time it shows up per peer and queue: "U3MP ev: peer P id I sends|got T vt 0x...". Every
// 30 s per peer a summary of the record counters (acked / lost) and queue lengths. Kinds are kept in a small table (the
// helper's data region is nearly full): 24 kinds, peers 0..3.
namespace {
constexpr int kKinds = 24, kPeers = 4, kQueues = 3;
}
struct U3xEv {
    uint32_t vt[kKinds];                      // event vtable - base
    uint8_t seen[kKinds][kQueues];            // bit p: logged for peer p
    uint32_t lines, next_sum[kPeers];
};
U3X_BSS static U3xEv u3x_ev;

U3X_RO static const char t_ev[] = "U3MP ev: peer ", t_id[] = " id ", t_q0[] = " sends reliable", t_q1[] = " sends unreliable",
    t_q2[] = " got", t_vt[] = " event vt ", t_full[] = " (kind table full)", t_sum[] = " summary: records acked ",
    t_lost[] = " lost ", t_rq[] = ", queued reliable ", t_uq[] = " unreliable ", t_gq[] = " received ";

namespace {
[[gnu::always_inline]] inline uint32_t qlen(const uint8_t *sub, uintptr_t off) {
    const uint8_t *n = *reinterpret_cast<const uint8_t *const *>(sub + off);
    uint32_t k = 0;
    while (n && ptr_ok(n) && k < 64) { k++; n = *reinterpret_cast<const uint8_t *const *>(n + 0x20); }
    return k;
}
}  // namespace

// queue: 0 reliable, 1 unreliable, 2 received; peer = connection entry index; id = its player id (entry +0x34)
extern "C" U3X_FN void u3x_ev_scan(uintptr_t b, uint32_t peer, int32_t id, const uint8_t *sub, uint32_t queue, uint32_t frame,
                                   Log log) {
    if (!ptr_ok(sub) || peer >= static_cast<uint32_t>(kPeers) || queue >= static_cast<uint32_t>(kQueues) || !log) return;
    const uintptr_t off = queue == 0 ? 0x110u : queue == 1 ? 0x120u : 0x138u;
    const uint8_t *n = *reinterpret_cast<const uint8_t *const *>(sub + off);
    for (int k = 0; n && ptr_ok(n) && k < 64; k++, n = *reinterpret_cast<const uint8_t *const *>(n + 0x20)) {
        const uint8_t *ev = *reinterpret_cast<const uint8_t *const *>(n);
        if (!ptr_ok(ev)) continue;
        const uintptr_t vtp = *reinterpret_cast<const uintptr_t *>(ev);
        if (vtp < b || vtp - b > 0x4000000u) continue;
        const uint32_t vt = static_cast<uint32_t>(vtp - b);
        int s = 0;
        while (s < kKinds && u3x_ev.vt[s] && u3x_ev.vt[s] != vt) s++;
        if (s == kKinds) continue;
        if (!u3x_ev.vt[s]) u3x_ev.vt[s] = vt;
        if (u3x_ev.seen[s][queue] & (1u << peer)) continue;
        u3x_ev.seen[s][queue] |= static_cast<uint8_t>(1u << peer);
        if (u3x_ev.lines >= 300) continue;
        u3x_ev.lines++;
        Line l;
        l.s(t_ev).d(peer).s(t_id).d(static_cast<uint32_t>(id)).s(queue == 0 ? t_q0 : queue == 1 ? t_q1 : t_q2).s(t_vt).x(vt, 8);
        if (s == kKinds - 1) l.s(t_full);
        log(b, l.z());
    }
    if (queue == 0 && frame >= u3x_ev.next_sum[peer] && u3x_ev.lines < 300) {   // every ~30 s (frames of the pump tick)
        u3x_ev.next_sum[peer] = frame + 1800;
        u3x_ev.lines++;
        Line l;
        l.s(t_ev).d(peer).s(t_id).d(static_cast<uint32_t>(id)).s(t_sum).d(rd<uint32_t>(sub, 0xb0)).s(t_lost).d(rd<uint32_t>(sub, 0xac));
        l.s(t_rq).d(qlen(sub, 0x110)).s(t_uq).d(qlen(sub, 0x120)).s(t_gq).d(qlen(sub, 0x138));
        log(b, l.z());
    }
}

// ===== independent rewrites of former PS3 ports (v7.113; HELPER-PORT-ANALYSIS.md, easiest first) =====
// Each function below was written from a description of what the game needs, not from the PS3 code or the former
// port's structure. The C part keeps thin wrappers (bookkeeping counters, its own helpers passed in as pointers).

// Match manager defaults the PS4 game never sets (its multiplayer start-up is missing): before any match, the
// synchronised-random seed slot is empty (-1), the three round bookkeeping words are zero, the camera flag is off and
// the manager's mode byte is 2. `mgr` is the match manager (image 0x17cb060).
extern "C" U3X_FN void u3x_match_defaults(uint8_t *mgr) {
    if (!ptr_ok(mgr)) return;
    struct Field { uint32_t off; int32_t value; uint8_t width; };
    const Field fields[] = {{0x350, -1, 4}, {0x354, 0, 1}, {0x358, 0, 4}, {0x35c, 0, 4}, {0x6a24, 0, 1}, {0x6a48, 2, 1}};
    for (const Field &f : fields) {
        if (f.width == 4) *reinterpret_cast<int32_t *>(mgr + f.off) = f.value;
        else mgr[f.off] = static_cast<uint8_t>(f.value);
    }
}

// An animation generator (NPC or net character) needs its 8 net animation tables: one per column of its row (+0x60),
// looked up by the caller's `lookup` and stored as pointers from +0x68. Returns a bit per column that was found.
using AnimLookup = uint8_t *(*)(uintptr_t base, uint32_t row, uint32_t column);
extern "C" U3X_FN uint32_t u3x_anim_tables(uintptr_t base, uint8_t *gen, AnimLookup lookup) {
    if (!ptr_ok(gen) || !lookup) return 0;
    const uint32_t row = rd<uint32_t>(gen, 0x60);
    uint8_t **slots = reinterpret_cast<uint8_t **>(gen + 0x68);
    uint32_t found = 0;
    for (uint32_t column = 0; column != 8; ++column) {
        slots[column] = lookup(base, row, column);
        found |= slots[column] ? (1u << column) : 0u;
    }
    return found;
}

// Oasis is the map whose water splashes use a second effect pair; its three map entries are recognised by id.
extern "C" U3X_FN int u3x_is_oasis(uint32_t map_id) {
    switch (map_id) {
    case 0xdeac0df3u: case 0x82fb8e9eu: case 0x421d5e2cu: return 1;
    default: return 0;
    }
}

// The playlist a player picked: the entry of the playlist table (u32 count, entries of 0x68 bytes from the pointer at
// +8, id first) whose id is `picked`, or the first entry when no entry has it. Null for an empty or missing table.
extern "C" U3X_FN const uint8_t *u3x_pick_playlist(const uint8_t *table, uint32_t picked) {
    if (!table) return nullptr;
    const int32_t count = rd<int32_t>(table, 0);
    const uint8_t *entries = *reinterpret_cast<const uint8_t *const *>(table + 8);
    if (count <= 0 || !entries) return nullptr;
    const uint8_t *end = entries + static_cast<uintptr_t>(count) * 0x68;
    for (const uint8_t *e = entries; e != end; e += 0x68)
        if (rd<uint32_t>(e, 0) == picked) return e;
    return entries;
}

// Weapons waiting for their skin: a list of 64 slots of 16 bytes (a handle to the weapon object, then the object's id).
// For each slot whose handle still points at the object with that id, the game's skin update (`apply`) runs on it,
// then the helper's own follow-up (`after`).
using ObjectFn = void (*)(void *object);
using HelperObjectFn = void (*)(uintptr_t base, uint8_t *object);
extern "C" U3X_FN void u3x_apply_pending_skins(uintptr_t base, uint8_t *list, ObjectFn apply, HelperObjectFn after) {
    if (!ptr_ok(list) || !apply) return;
    for (uint8_t *slot = list, *end = list + 64 * 16; slot != end; slot += 16) {
        uint8_t *const *handle = *reinterpret_cast<uint8_t *const *const *>(slot);
        const uint32_t id = rd<uint32_t>(slot, 8);
        if (!handle || !id) continue;
        uint8_t *object = *handle;
        if (!object || rd<uint32_t>(object, 0x7c) != id) continue;
        apply(object);
        if (after) after(base, object);
    }
}

// ===== v7.114: second batch (same rules as above) =====

// A fresh online multiplayer record, for a player with no saved profile yet or one saved in another format. The caller
// has zeroed it; this writes the format version, the default loadout and appearance ids the menus expect, the 0.5
// setting at +0x1628, and copies +0x504 from the game's current value (`current`: a handle, may be empty).
extern "C" U3X_FN void u3x_record_defaults(uint8_t *rec, uint32_t version, const uint32_t *const *current) {
    if (!rec) return;
    struct Word { uint32_t off, value; };
    const Word words[] = {{0x18, 0x6afcc877u}, {0x1c, 0xbe693215u}, {0x24, 0x9e96d6d7u}, {0xd8, 0xe0b92307u},
                          {0xdc, 0x85e71b0eu}, {0xe4, 0x6bbaf02au}, {0x53c, 0xc56be227u}, {0x1628, 0x3f000000u}};
    *reinterpret_cast<uint32_t *>(rec) = version;
    if (current && *current) *reinterpret_cast<uint32_t *>(rec + 0x504) = **current;
    for (const Word &w : words) *reinterpret_cast<uint32_t *>(rec + w.off) = w.value;
}

// The game's message dialog takes its content from a block the menu owns (menu +0x1438): the body and title text ids,
// the id of the action its button sends, the button layout (0 = none), two caller words, and the dialog's fixed style
// and sound ids. `close` 0 picks the game's default close label: one when the dialog has no buttons, another otherwise.
extern "C" U3X_FN void u3x_fill_dialog(uint8_t *d, uint32_t body, uint32_t title, uint32_t action, int32_t buttons,
                                       uint32_t extra, uint32_t close) {
    if (!d) return;
    if (close == 0) close = buttons ? 0xd75be949u : 0x576ed245u;
    auto put = [d](uint32_t off, uint32_t v) { *reinterpret_cast<uint32_t *>(d + off) = v; };
    put(0x08, body);
    put(0x10, buttons ? 1u : 0u);
    put(0x14, title);
    put(0x18, 0x11d8e78au);
    put(0x1c, static_cast<uint32_t>(buttons));
    put(0x20, 0x1519fa3du);
    put(0x24, close);
    put(0x28, action);
    put(0x2c, extra);
}

// Elimination rounds: each new round stamps its start time in the match manager (+0x66ec). The HUD's net clock
// (`clock`) has to follow it when the selected net game (+0x661c, an index into the net game table: count, then
// 0xd0-byte entries from the pointer at +8) is an Elimination game (type 10 at entry +0xc). Each stamp is handled once.
using TableLookup = void **(*)(uint32_t id);
U3X_BSS uint32_t round_start_seen;
extern "C" U3X_FN void u3x_round_clock(const uint8_t *mgr, TableLookup lookup, uint32_t *clock) {
    const uint32_t start = rd<uint32_t>(mgr, 0x66ec);
    if (start == round_start_seen) return;
    round_start_seen = start;
    void **handle = lookup ? lookup(0x3b9a067du) : nullptr;
    const uint8_t *games = handle ? static_cast<const uint8_t *>(*handle) : nullptr;
    const int32_t pick = rd<int16_t>(mgr, 0x661c);
    if (!games || pick < 0 || pick >= rd<int32_t>(games, 0)) return;
    const uint8_t *entry = *reinterpret_cast<const uint8_t *const *>(games + 8) + static_cast<uintptr_t>(pick) * 0xd0;
    if (rd<int32_t>(entry, 0xc) == 10) *clock = start;
}

// Spawner property tables: {entries, u32 count at +8}, entries of 16 bytes sorted by symbol id (id, then a pointer to a
// value {u16 type, ..., data pointer at +8}). The value stored under `sid`, or null.
extern "C" int32_t u3x_sorted_index(const uint8_t *entries, uint32_t count, uint32_t id);
U3X_FN static const uint8_t *find_property(const uint8_t *tab, uint32_t sid) {
    if (!tab) return nullptr;
    const uint8_t *entries = *reinterpret_cast<const uint8_t *const *>(tab);
    const uint32_t count = rd<uint32_t>(tab, 8);
    const int32_t at = static_cast<int32_t>(count) > 0 ? u3x_sorted_index(entries, count, sid) : -1;
    return at < 0 ? nullptr : *reinterpret_cast<const uint8_t *const *>(entries + static_cast<uint32_t>(at) * 16u + 8);
}
// An NPC counts as "marked" (extra kill reward) when its spawner (`spawn`, properties at +0x40, table at +0x68) sets
// property 0x22b25554 to a non-zero int (value type 1).
extern "C" U3X_FN int u3x_spawn_marked(const uint8_t *spawn) {
    const uint8_t *props = spawn ? *reinterpret_cast<const uint8_t *const *>(spawn + 0x40) : nullptr;
    const uint8_t *value = props ? find_property(*reinterpret_cast<const uint8_t *const *>(props + 0x68), 0x22b25554u) : nullptr;
    if (!value || rd<uint16_t>(value, 0) != 1) return 0;
    const uint32_t *data = *reinterpret_cast<const uint32_t *const *>(value + 8);
    return data && *data != 0;
}

// Report Player reasons: a table {u32 count, 16-byte entries from the pointer at +8: label id, then the reason text at +8}.
// How many the reason popup shows (at most 8; 0 when the table is missing or empty), and the reason text of row `row`.
extern "C" U3X_FN uint32_t u3x_report_rows(const uint8_t *table) {
    if (!table || !*reinterpret_cast<const uint8_t *const *>(table + 8)) return 0;
    const uint32_t n = rd<uint32_t>(table, 0);
    return n < 8 ? n : 8;
}
extern "C" U3X_FN const char *u3x_report_reason(const uint8_t *table, uint32_t row) {
    if (!table || row >= rd<uint32_t>(table, 0)) return nullptr;
    const uint8_t *entries = *reinterpret_cast<const uint8_t *const *>(table + 8);
    return entries ? *reinterpret_cast<const char *const *>(entries + static_cast<uintptr_t>(row) * 16 + 8) : nullptr;
}

// Saved online records are compressed with the game's LZ format: a control byte either starts a run of 1..32 literal
// bytes (values 0..31) or a back reference (top 3 bits = length - 2, 7 meaning "add the next byte"; low 5 bits and the
// next byte = distance - 1, up to 8 KiB). Decodes `in` into `out` and returns the decoded size, or 0 for a stream that
// is cut short, overflows `cap`, or points before the start.
extern "C" U3X_FN uint32_t u3x_lz_decode(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t cap) {
    uint32_t ip = 0, op = 0;
    while (ip < in_len) {
        const uint32_t ctl = in[ip++];
        if (ctl < 0x20) {
            const uint32_t run = ctl + 1;
            if (run > cap - op || run > in_len - ip) return 0;
            for (uint32_t k = 0; k < run; ++k) out[op + k] = in[ip + k];
            op += run; ip += run;
            continue;
        }
        uint32_t len = ctl >> 5;
        if (len == 7) {
            if (ip >= in_len) return 0;
            len += in[ip++];
        }
        if (ip >= in_len) return 0;
        const uint32_t dist = (((ctl & 0x1fu) << 8) | in[ip++]) + 1;
        len += 2;
        if (len > cap - op || dist > op) return 0;
        for (uint32_t k = 0; k < len; ++k) out[op + k] = out[op + k - dist];   // may overlap: byte by byte
        op += len;
    }
    return op;
}

// ===== v7.115: third batch (same rules as above) =====

// Menu pages with tabs: the tab table lists (u32 count, 0x20-byte entries from the pointer at +8) a page id, whether
// the entry is for LAN play, a list of tabs (12-byte entries, id first) at +0x10 and the index of the current tab at
// +0x18. The id of the current tab of `page` in the given play kind, or 0.
extern "C" U3X_FN uint32_t u3x_page_tab(const uint8_t *table, uint32_t page, uint32_t lan) {
    if (!table) return 0;
    const uint8_t *entry = *reinterpret_cast<const uint8_t *const *>(table + 8);
    if (!entry) return 0;
    for (uint32_t left = rd<uint32_t>(table, 0); left; --left, entry += 0x20) {
        if (rd<uint32_t>(entry, 0) != page || rd<uint32_t>(entry, 4) != lan) continue;
        const uint8_t *tabs = *reinterpret_cast<const uint8_t *const *>(entry + 0x10);
        const int32_t current = rd<int32_t>(entry, 0x18);
        if (!tabs || current < 0) return 0;
        return rd<uint32_t>(tabs, static_cast<uintptr_t>(current) * 12);
    }
    return 0;
}

// The "new" marker on the Profile row: when the player has unseen unlocks (`flags` non-zero) the list's query is
// answered with the game's own marker answer (kind 0x2f, no texture, both indices -1, scale 1.0). Returns 1 if answered.
extern "C" U3X_FN int u3x_new_marker(int32_t *event, uint32_t flags) {
    if (!flags) return 0;
    event[2] = 0x2f;
    for (int word = 4; word <= 8; ++word) event[word] = 0;
    event[9] = -1;
    event[10] = 0;
    event[11] = -1;
    event[12] = static_cast<int32_t>(F_ONE);
    return 1;
}

// Custom game set-up: the player picks side 0 or 1 for the character preview. The side is stored; then, when the
// set-up's own preview character (handle at +0xf60, id at +0xf68) is alive, the game's preview refresh runs and the
// "changed" byte (+0x104c) is set. Leaving the screen (`inplace` 0) skips the refresh when that preview is the one the
// scene currently shows (`shown`, `shown_id`).
using CustomRefresh = void (*)(int32_t *custom);
extern "C" U3X_FN void u3x_custom_side(int32_t *custom, int side, int inplace, const void *shown, uint32_t shown_id,
                                       CustomRefresh refresh) {
    if (side != 0 && side != 1) return;
    custom[0] = side;
    uint8_t *raw = reinterpret_cast<uint8_t *>(custom);
    uint8_t *const *handle = *reinterpret_cast<uint8_t *const *const *>(raw + 0xf60);
    const uint32_t id = rd<uint32_t>(raw, 0xf68);
    if (!id || !handle) return;
    if (!inplace && id == shown_id && handle == shown) return;
    if (!ptr_ok(handle) || !ptr_ok(*handle) || rd<uint32_t>(*handle, 0x7c) != id) return;
    refresh(custom);
    raw[0x104c] = 1;
}

// Closing the menu's open popup (menu +0x898): play the close sound, tell the popup it is no longer active, report the
// close (kind 3, the popup's id at +0x10), make the page on top of the page stack (+0x8b0, index at +0xdb0) active
// again, and keep the popup as "last closed" (+0x8a0). Activation is the 7th virtual function of a page.
using SoundFn = void (*)(int sound);
using MenuNoteFn = void (*)(void *menu, int kind, uint32_t id);
U3X_FN static void set_active(uint8_t *page, int on) {
    using Activate = void (*)(void *, int);
    reinterpret_cast<Activate>((*reinterpret_cast<uintptr_t *const *>(page))[6])(page, on);
}
extern "C" U3X_FN void u3x_close_popup(uint8_t *menu, SoundFn sound, MenuNoteFn note) {
    uint8_t *&open = *reinterpret_cast<uint8_t **>(menu + 0x898);
    uint8_t *popup = open;
    if (!popup) return;
    sound(0xd);
    set_active(popup, 0);
    note(menu, 3, rd<uint32_t>(popup, 0x10));
    uint8_t *const *stack = reinterpret_cast<uint8_t *const *>(menu + 0x8b0);
    set_active(stack[rd<int32_t>(menu, 0xdb0)], 1);
    *reinterpret_cast<uint8_t **>(menu + 0x8a0) = open;
    open = nullptr;
}

// Taking a co-op mission: row `row` of the shown mission set (count at +8, 0x4c-byte missions from the pointer at
// +0x18, id first) becomes the player's active mission when the game's own check allows it and the player has not
// completed it: active id at profile +0x1864, progress (+0xf4c) back to zero, start time (+0x1868) from `clock` when
// there is one. Returns 1 when the mission was taken (the caller then saves the profile).
using MissionCan = uint8_t (*)(uint8_t *owner, uint8_t *mission, int flag);
using MissionDone = int (*)(uint8_t *profile, uint32_t id);
using ClockFn = int64_t (*)(int64_t *out);
extern "C" U3X_FN int u3x_take_mission(uint8_t *owner, uint8_t *profile, const uint8_t *set, uint32_t row, MissionCan can,
                                       MissionDone done, ClockFn clock) {
    if (!profile || !set || !can) return 0;
    uint8_t *missions = *reinterpret_cast<uint8_t *const *>(set + 0x18);
    if (!missions || row >= rd<uint32_t>(set, 8)) return 0;
    uint8_t *mission = missions + static_cast<uintptr_t>(row) * 0x4c;
    const uint32_t id = rd<uint32_t>(mission, 0);
    if (!can(owner, mission, 0) || done(profile, id)) return 0;
    *reinterpret_cast<uint32_t *>(profile + 0x1864) = id;
    *reinterpret_cast<uint32_t *>(profile + 0xf4c) = 0;
    if (clock) *reinterpret_cast<uint32_t *>(profile + 0x1868) = static_cast<uint32_t>(clock(nullptr));
    return 1;
}

// ===== v7.116: fourth batch (same rules as above) =====

// A list item that shows a picture answers the list's query with an image: words 3..12 cleared, both indices -1,
// scale 1.0, and either kind 2 with the texture, or kind 0x10 (the loading spinner) while there is no texture yet.
extern "C" U3X_FN void u3x_image_answer(int32_t *ev, void *texture) {
    for (int word = 3; word <= 12; ++word) ev[word] = (word == 9 || word == 11) ? -1 : 0;
    ev[12] = static_cast<int32_t>(F_ONE);
    if (texture) {
        ev[2] = 2;
        *reinterpret_cast<void **>(ev + 4) = texture;
    } else {
        ev[2] = 0x10;
    }
}

// Rank badges are textures named after the rank: "ranks/<rank>" or, for a player whose prestige level is 1 up to
// (but not including) `prestige_limit`, "ranks/prestige-<rank>". `value` packs the rank (low 16 bits, counted from 0)
// and the prestige level (14 bits from bit 16). `out` needs room for 24 characters.
U3X_RO const char rank_dir[] = "ranks/";
U3X_RO const char rank_prestige[] = "prestige-";
extern "C" U3X_FN void u3x_rank_icon_name(char *out, uint32_t value, uint32_t prestige_limit) {
    const uint32_t prestige = (value >> 16) & 0x3fffu;
    for (const char *p = rank_dir; *p; ++p) *out++ = *p;
    if (prestige != 0 && prestige < prestige_limit)
        for (const char *p = rank_prestige; *p; ++p) *out++ = *p;
    uint32_t rank = (value & 0xffffu) + 1, scale = 1;
    while (rank / scale >= 10) scale *= 10;
    for (; scale; scale /= 10) *out++ = static_cast<char>('0' + rank / scale % 10);
    *out = 0;
}

// The matchmaking vote list asks two things about a row. Query 0x7754e7af, "which row won": nothing (-1) while the
// vote is open; once closed, row 0 if the selected game is the winner, else row 1. Query 0x17809498, "can the list
// take focus": only while the vote is open and this player has not voted for row 0 or 1 yet. Returns 1 with `answer`
// set for these two queries, 0 for any other.
extern "C" U3X_FN int u3x_vote_query(uint32_t query, int closed, int32_t my_vote, int32_t selected, int32_t winner,
                                     int32_t *answer) {
    switch (query) {
    case 0x7754e7afu:
        *answer = !closed ? -1 : (selected == winner ? 0 : 1);
        return 1;
    case 0x17809498u:
        *answer = (!closed && (my_vote < 0 || my_vote > 1)) ? 1 : 0;
        return 1;
    default:
        return 0;
    }
}

// Weapon statistics are numbered: the first 20 numbers are the fixed weapons (`fixed`), later ones index the game's
// extra weapon list (u32 count, ids from the pointer at +8). The weapon id for a number, or 0 with `*missing` set
// when the number is past the list (or the list is not there).
extern "C" U3X_FN uint32_t u3x_weapon_stat_id(uint32_t number, const uint32_t *fixed, const uint8_t *extra, int *missing) {
    *missing = 0;
    if (number < 20) return fixed[number];
    const uint32_t at = number - 20;
    const uint32_t *ids = extra ? *reinterpret_cast<const uint32_t *const *>(extra + 8) : nullptr;
    if (ids && at < rd<uint32_t>(extra, 0)) return ids[at];
    *missing = 1;
    return 0;
}

// Elimination late join. A player may not join a round that is mostly over: time is left on the round clock (`left`,
// in clock ticks) and less than a quarter of it would remain after the time already played (`elapsed`).
extern "C" U3X_FN int u3x_round_mostly_over(int32_t left, int32_t elapsed) {
    if (left <= 0) return 0;
    const int32_t remaining = static_cast<int32_t>(static_cast<uint32_t>(left) - static_cast<uint32_t>(elapsed));
    return static_cast<int64_t>(remaining) * 4 < left;
}
// Nor a match that is nearly decided: the leading team has won more than one round, or this is the last round.
extern "C" U3X_FN int u3x_match_nearly_decided(int32_t lead_wins, int32_t rounds, int32_t round) {
    return lead_wins > 1 || round >= rounds - 1;
}

// ===== v7.117: fifth batch (same rules as above) =====

// A profile name typed in the system keyboard (UTF-16, at most 15 units) becomes the stored name (UTF-8, zero padded
// to `bytes`, which must be at least 48). Control characters and broken surrogate pairs are refused; the characters
// the game's text and file code cannot take in a name (| \ .) become *; spaces at either end are dropped. Returns 1
// for a usable (non-empty) name; the buffer is all zero otherwise.
extern "C" U3X_FN int u3x_name_utf8(const uint16_t *in, char *out, uint32_t bytes) {
    for (uint32_t k = 0; k < bytes; ++k) out[k] = 0;
    uint32_t used = 0, keep = 0;
    bool bad = false;
    for (uint32_t i = 0; i < 15 && in[i] && !bad; ++i) {
        uint32_t cp = in[i];
        if (cp < 0x20 || cp == 0x7f) bad = true;
        else if (cp >= 0xdc00 && cp <= 0xdfff) bad = true;
        else if (cp >= 0xd800 && cp <= 0xdbff) {
            const uint32_t low = (i + 1 < 15) ? in[i + 1] : 0;
            if (low < 0xdc00 || low > 0xdfff) bad = true;
            else { cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00); ++i; }
        } else if (cp == 0x7c || cp == 0x5c || cp == 0x2e) cp = 0x2a;
        if (bad || (cp == 0x20 && used == 0)) continue;
        uint8_t *o = reinterpret_cast<uint8_t *>(out) + used;
        if (cp < 0x80) { o[0] = static_cast<uint8_t>(cp); used += 1; }
        else {
            const uint32_t extra = cp < 0x800 ? 1 : cp < 0x10000 ? 2 : 3;
            o[0] = static_cast<uint8_t>(((0xf0u << (3 - extra)) & 0xf0u) | (cp >> (6 * extra)));
            for (uint32_t k = 1; k <= extra; ++k) o[k] = static_cast<uint8_t>(0x80u | ((cp >> (6 * (extra - k))) & 0x3fu));
            used += extra + 1;
        }
        if (cp != 0x20) keep = used;
    }
    if (bad) keep = 0;
    for (uint32_t k = keep; k < used; ++k) out[k] = 0;
    return keep != 0;
}

// A weapon row's picture is the game's own weapon icon item: kind 0x21 with the weapon id, no texture, both indices
// -1, scale 1.0.
extern "C" U3X_FN void u3x_weapon_row_image(int32_t *ev, uint32_t weapon) {
    for (int word = 4; word <= 11; ++word) ev[word] = (word == 9 || word == 11) ? -1 : 0;
    ev[2] = 0x21;
    ev[6] = static_cast<int32_t>(weapon);
    ev[12] = static_cast<int32_t>(F_ONE);
}
// The weapon preview in the menu scene (a handle and the id the object must still have): when it is alive, its
// attachment mask (+0x6f0, 16 bits) is set and the game's rebuild runs on it. Returns 1 when it did.
using PreviewFn = void (*)(uint8_t *object);
extern "C" U3X_FN int u3x_preview_mods(uint8_t *const *handle, uint32_t id, uint32_t mask, PreviewFn rebuild) {
    if (!id || !ptr_ok(handle)) return 0;
    uint8_t *object = *handle;
    if (!object || rd<uint32_t>(object, 0x7c) != id) return 0;
    *reinterpret_cast<uint16_t *>(object + 0x6f0) = static_cast<uint16_t>(mask);
    rebuild(object);
    return 1;
}

// Leaving a match early: the record's "in a match" word (+0x558) is still set when the player backs out. That counts
// as a quit: the word is cleared and both quit counters (+0x5b0, +0x1f24) go up by one. Returns 1 when it counted.
extern "C" U3X_FN int u3x_count_quit(uint8_t *record) {
    uint32_t &in_match = *reinterpret_cast<uint32_t *>(record + 0x558);
    if (!in_match) return 0;
    in_match = 0;
    ++*reinterpret_cast<uint32_t *>(record + 0x5b0);
    ++*reinterpret_cast<uint32_t *>(record + 0x1f24);
    return 1;
}

// Party guests: this player is in a party (id at +0x18) that someone else leads (own member number at +0x12e4,
// leader's at +0x12e8).
extern "C" U3X_FN int u3x_party_guest(const uint8_t *party) {
    return rd<uint64_t>(party, 0x18) != 0 && rd<uint32_t>(party, 0x12e4) != rd<uint32_t>(party, 0x12e8);
}
// The lobby's two buttons. Only the leader sets up the game: for a guest the set-up button is hidden and unlabelled.
// The start button is always shown; a guest reads "waiting on party leader" there, the leader the custom-game or the
// normal start label. Label result: 0 = empty, 1 = the waiting text, else a text id.
extern "C" U3X_FN int u3x_lobby_button_shown(int setup_button, int guest) { return !(setup_button && guest); }
extern "C" U3X_FN uint32_t u3x_lobby_button_label(int setup_button, int guest, int custom_game, uint32_t own_label) {
    if (setup_button) return guest ? 0u : own_label;
    if (guest) return 1u;
    return custom_game ? 0x92db2b96u : 0x63091cd0u;
}

// Leaving to the menu has to wait while the "net" level is still being loaded or unloaded (state 8 or 0xc at +0x30).
// Returns -1 without a level, 0 while it is busy, 1 when it is settled; `state` gets the level's state.
extern "C" U3X_FN int u3x_net_level_ready(const uint8_t *level, uint32_t *state) {
    if (!ptr_ok(level)) return -1;
    *state = rd<uint32_t>(level, 0x30);
    return (*state == 8 || *state == 0xc) ? 0 : 1;
}

// A list with nothing in it answers the list's standard questions: no rows, nothing selected, no focus.
extern "C" U3X_FN void u3x_empty_list_answer(int32_t *ev) {
    switch (ev[0]) {
    case 5: case 7: case 21: ev[1] = 0; break;
    case 6: ev[1] = -1; break;
    case 11: ev[14] = 0; break;
    default: break;
    }
}

// "Last game" line of the profile box: "<map> - <game type> - <third>". The map's text id comes from the map table
// (count, 0x98-byte entries from the pointer at +8: map id, text id; at most 128 are looked at).
extern "C" U3X_FN int u3x_map_name_id(const int32_t *maps, uint32_t map, uint32_t *text_id) {
    if (!maps) return 0;
    const uint8_t *entry = *reinterpret_cast<const uint8_t *const *>(maps + 2);
    if (!entry) return 0;
    const int32_t count = maps[0] < 128 ? maps[0] : 128;
    for (int32_t k = 0; k < count; ++k, entry += 0x98)
        if (rd<uint32_t>(entry, 0) == map) { *text_id = rd<uint32_t>(entry, 4); return 1; }
    return 0;
}
// The third part: for the two co-op game types (4 and 11) the difficulty's text (`difficulty_texts`, 5 ids; empty
// for an unknown difficulty); for other games the net game's subtitle unless it is one of the two "no subtitle" ids.
// Returns 0 for no third part, 1 with `text_id`, 2 for an empty third part.
extern "C" U3X_FN int u3x_last_game_third(int32_t game_type, uint32_t subtitle, uint32_t difficulty,
                                          const uint32_t *difficulty_texts, uint32_t *text_id) {
    if (game_type == 4 || game_type == 11) {
        if (difficulty >= 5) return 2;
        *text_id = difficulty_texts[difficulty];
        return 1;
    }
    if (subtitle == 0 || subtitle == 0x22a276ecu || subtitle == 0x6f96651fu) return 0;
    *text_id = subtitle;
    return 1;
}
// Joins the parts that are there with " - " into `out` (never past `end`, always terminated).
U3X_RO const char dash_sep[] = " - ";
extern "C" U3X_FN void u3x_join_dash(char *out, char *end, const char *a, const char *b, const char *c) {
    const char *parts[3] = {a, b, c};
    bool first = true;
    for (const char *part : parts) {
        if (!part) continue;
        if (!first)
            for (const char *p = dash_sep; *p && out < end; ++p) *out++ = *p;
        for (; *part && out < end; ++part) *out++ = *part;
        first = false;
    }
    *out = 0;
}

// The multiplayer profile is saved on certain menu page events: events 2, 3 and 4 of three pages, and events 3 and 4
// of two others.
extern "C" U3X_FN int u3x_page_saves(uint32_t page, uint32_t event) {
    switch (page) {
    case 0xe0c0028bu: case 0xe13ed766u: case 0xdfdb36fau: return event >= 2 && event <= 4;
    case 0x35957e36u: case 0xba7ae3b4u: return event == 3 || event == 4;
    default: return 0;
    }
}

// ===== v7.118: the session server's hello (sm01), without the start value =====

// The hello's 16-byte start value comes from the player's own PS3 executable: the launcher writes it as 32 hex digits
// to a file in the player's data folder, and no release contains it. Parses that text (digits, then optional line
// end or spaces) into `key`. Returns 1 for 16 good bytes.
extern "C" U3X_FN int u3x_session_key_parse(const char *text, int length, uint8_t *key) {
    if (!text || length < 32) return 0;
    for (int k = 32; k < length; ++k)
        if (text[k] != '\n' && text[k] != '\r' && text[k] != ' ' && text[k] != 0) return 0;
    uint8_t bytes[16];
    for (int k = 0; k < 32; ++k) {
        const char c = text[k];
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return 0;
        bytes[k / 2] = static_cast<uint8_t>((k & 1) ? (bytes[k / 2] | digit) : (digit << 4));
    }
    for (int k = 0; k < 16; ++k) key[k] = bytes[k];
    return 1;
}

// The session server asks each client to answer its challenge with a checksum only the game computes. The game's
// checksum keeps four 32-bit words; every 4 input bytes (a little-endian word) stir them with adds, two fixed xors
// and rotations by 7, 11 and 17. The answer is: start from the 16-byte start value, take in the challenge, then the
// state's own 16 bytes, then the 36-byte player id; the sum of the four words after that.
namespace {
struct SessionSum {
    uint32_t w[4];
    static uint32_t rol(uint32_t v, unsigned by) { return (v << by) | (v >> (32 - by)); }
    static uint32_t word(const uint8_t *p) {
        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
               static_cast<uint32_t>(p[3]) << 24;
    }
    [[gnu::always_inline]] void take(const uint8_t *bytes, int count) {
        uint32_t a = w[0], b = w[1], c = w[2], d = w[3];
        for (const uint8_t *p = bytes, *end = bytes + count; p < end; p += 4) {
            d ^= 0x5b3aa654u;
            c = rol(c + a + d, 7);
            b = rol(b + d + c, 11);
            c ^= 0x75970a4du;
            a = rol(a + c + b, 17);
            d += (a & b) | (c & ~b);
            a = ~a;
            d ^= word(p);
        }
        w[0] = a; w[1] = b; w[2] = c; w[3] = d;
    }
};
}  // namespace
extern "C" U3X_FN uint32_t u3x_session_answer(const uint8_t *key, uint32_t challenge, const uint8_t *player_id) {
    SessionSum sum;
    for (int k = 0; k < 4; ++k) sum.w[k] = SessionSum::word(key + 4 * k);
    uint8_t bytes[16];
    for (int k = 0; k < 4; ++k) bytes[k] = static_cast<uint8_t>(challenge >> (8 * k));
    sum.take(bytes, 4);
    for (int k = 0; k < 16; ++k) bytes[k] = static_cast<uint8_t>(sum.w[k / 4] >> (8 * (k % 4)));
    sum.take(bytes, 16);
    sum.take(player_id, 36);
    return sum.w[0] + sum.w[1] + sum.w[2] + sum.w[3];
}

// ===== v7.119: sixth batch (same rules as above) =====

// Clicking an item in the loadout lists plays the "picked" sound (0) when the item can be used (unlock status 0 or
// 1) and the "refused" sound (8) otherwise.
extern "C" U3X_FN int u3x_click_sound(int unlock_status) { return (unlock_status == 0 || unlock_status == 1) ? 0 : 8; }
// The paid booster slot shows what is equipped from its own word, or from the player's word when that is empty.
// True when the clicked booster is the one already there (then nothing plays).
extern "C" U3X_FN int u3x_booster_already(uint32_t slot_word, uint32_t player_word, uint32_t booster) {
    return (slot_word ? slot_word : player_word) == booster;
}
// A weapon's mod list: `count` entries of 0x14 bytes, the mod id first. The id of row `row`; 0 when there is none.
extern "C" U3X_FN int u3x_mod_id(const uint8_t *mods, uint32_t count, int32_t row, uint32_t *id) {
    if (!mods || row < 0 || static_cast<uint32_t>(row) >= count) return 0;
    *id = rd<uint32_t>(mods, static_cast<uintptr_t>(row) * 0x14);
    return 1;
}

// A menu page keeps its items in a column (page +0x48: u32 count, 0x68-byte items from the pointer at +8). The items
// that are lists (type 21 at +0x30) fed by the game's source number `source` (+0x34) and not yet tagged (+0x40) are
// handed to the helper instead: source 0xcf, tag `tag`. Pages with more than 64 items are left alone. Returns how
// many were changed.
extern "C" U3X_FN uint32_t u3x_retag_items(uint8_t *page, uint32_t source, uint32_t tag) {
    uint8_t *column = *reinterpret_cast<uint8_t *const *>(page + 0x48);
    if (!column) return 0;
    const uint32_t count = rd<uint32_t>(column, 0);
    uint8_t *item = *reinterpret_cast<uint8_t *const *>(column + 8);
    if (!item || count > 64) return 0;
    uint32_t changed = 0;
    for (uint8_t *end = item + static_cast<uintptr_t>(count) * 0x68; item != end; item += 0x68) {
        if (rd<uint32_t>(item, 0x30) != 21 || rd<uint32_t>(item, 0x34) != source || rd<uint32_t>(item, 0x40) != 0) continue;
        *reinterpret_cast<uint32_t *>(item + 0x34) = 0xcf;
        *reinterpret_cast<uint32_t *>(item + 0x40) = tag;
        ++changed;
    }
    return changed;
}
// The weapon statistics bars ask two things about a row's weapon: is it locked (query 0xa1443de7), and which weapon
// it is (query 0x1720a1ed). Returns 1 with `answer` set for these two.
extern "C" U3X_FN int u3x_comp_weapon_answer(uint32_t query, uint32_t weapon, int locked, int32_t *answer) {
    if (query == 0xa1443de7u) *answer = locked ? 1 : 0;
    else if (query == 0x1720a1edu) *answer = static_cast<int32_t>(weapon);
    else return 0;
    return 1;
}

// Animation groups are found by name id in the loaded packages: 350 package slots of 0x2c0 bytes (in use when +0x20
// is non-zero, package at +0x18); each package has a hash table of `n` 16-byte entries (n at +0x2c, table at +0x30:
// name id, then the group) probed from id % n, wrapping round; an empty entry ends the search in that package. The
// first package that has a group under `id` wins.
extern "C" U3X_FN uint8_t *u3x_find_art_group(const uint8_t *slots, uint32_t id) {
    if (!slots) return nullptr;
    for (const uint8_t *slot = slots, *last = slots + 0x15e * 0x2c0; slot != last; slot += 0x2c0) {
        if (!rd<uint32_t>(slot, 0x20)) continue;
        const uint8_t *package = *reinterpret_cast<const uint8_t *const *>(slot + 0x18);
        if (!package) continue;
        const int32_t size = rd<int32_t>(package, 0x2c);
        const uint8_t *table = *reinterpret_cast<const uint8_t *const *>(package + 0x30);
        if (size <= 0 || !table) continue;
        const uint32_t n = static_cast<uint32_t>(size);
        uint32_t at = id % n;
        for (uint32_t probes = 0; probes != n; ++probes, at = (at + 1 == n) ? 0 : at + 1) {
            const uint8_t *entry = table + static_cast<uintptr_t>(at) * 16;
            const uint32_t have = rd<uint32_t>(entry, 0);
            if (!have) break;
            if (have != id) continue;
            if (uint8_t *group = *reinterpret_cast<uint8_t *const *>(entry + 8)) return group;
            break;
        }
    }
    return nullptr;
}
// A player in multiplayer needs 8 animation groups (the array at Player +0x1c20), looked up by the names in `names`
// (8 fixed strings of 24 bytes; an empty name leaves the entry empty). `name_id` is the game's name hash. Returns a
// bit per entry that was found.
using NameIdFn = uint32_t (*)(int zero, const char *name, int one);
extern "C" U3X_FN uint32_t u3x_player_net_anims(uint8_t **groups, const uint8_t *slots, NameIdFn name_id, const char *names) {
    uint32_t found = 0;
    for (uint32_t k = 0; k != 8; ++k) {
        const char *name = names + k * 24;
        groups[k] = (slots && name_id && name[0]) ? u3x_find_art_group(slots, name_id(0, name, 1)) : nullptr;
        if (groups[k]) found |= 1u << k;
    }
    return found;
}

// The 14 button actions of the Cinema's film player (play, pause, speeds, camera modes and so on) go to the film
// player object as events instead of being handled by the menu.
extern "C" U3X_FN int u3x_is_film_action(uint32_t action) {
    switch (action) {
    case 0x38231b34u: case 0x3cf5deedu: case 0x43348860u: case 0x516bfbb8u: case 0x682c6868u: case 0x806c9824u:
    case 0x887c7d6eu: case 0x953b8f2au: case 0x9eb9632du: case 0xa65c61bau: case 0xbd8d7974u: case 0xc1f65aebu:
    case 0xe8184ce8u: case 0xff502160u:
        return 1;
    default:
        return 0;
    }
}
// A game object kept as a handle plus the id it had: the object while it is still that one, else null.
extern "C" U3X_FN uint8_t *u3x_live_object(uint8_t *const *handle, uint32_t id) {
    if (!handle || !id) return nullptr;
    uint8_t *object = *handle;
    return (object && rd<uint32_t>(object, 0x7c) == id) ? object : nullptr;
}

// "Invited ... ago": the number and the text to use for an age in seconds. Under two minutes it is seconds (at
// least 2); then minutes up to 119; then hours up to 47; then days up to 13; then weeks.
extern "C" U3X_FN uint32_t u3x_time_ago(uint32_t seconds, uint32_t *text_id) {
    struct Unit { uint32_t per, limit, text; };   // `limit`: the largest count still shown in this unit
    const Unit units[] = {{60, 119, 0x6498bce2u}, {60, 47, 0x624258c1u}, {24, 13, 0x60f0bee3u}, {7, 0xffffffffu, 0xd250139cu}};
    if (seconds < 120) {
        *text_id = 0x72cd40d5u;
        return seconds < 2 ? 2 : seconds;
    }
    uint32_t value = seconds;
    for (const Unit &unit : units) {
        value /= unit.per;
        *text_id = unit.text;
        if (value <= unit.limit) break;
    }
    return value;
}

// The received-message queue: `count` entries of 0x40 bytes, the message type at +0x28. Finds the first entry of
// `type`; with `remove` it is dropped (later entries move up, the count goes down), otherwise its index is given.
// Returns 0 when one was found, -1 when not.
extern "C" U3X_FN int32_t u3x_queue_find(uint8_t *entries, uint32_t *count, uint32_t type, uint32_t *index, int remove) {
    const uint32_t n = *count;
    uint32_t at = 0;
    while (at != n && rd<uint32_t>(entries, static_cast<uintptr_t>(at) * 0x40 + 0x28) != type) ++at;
    if (at == n) return -1;
    if (!remove) {
        *index = at;
        return 0;
    }
    // byte by byte: the entries are read through other types too
    for (uint8_t *to = entries + static_cast<uintptr_t>(at) * 0x40, *end = entries + static_cast<uintptr_t>(n - 1) * 0x40; to != end; ++to)
        *to = to[0x40];
    *count = n - 1;
    return 0;
}

// ===== v7.120: seventh batch (same rules as above) =====

// Friend list order: friends playing this game first, then online elsewhere, then offline; within each group by
// name, ignoring case. Entries are 0x240 bytes (name from +1, state at +0x1b0: 1 = in this game, 2 = online).
// Friends that compare equal (same group, names equal apart from case) keep the order the game's own sort gives
// them, so the sort is the classic median-of-three quicksort with insertion sort for runs of up to 7 entries.
// `spare` is one entry of scratch space (the pivot).
namespace {
constexpr uintptr_t FRIEND = 0x240;
int friend_group(const uint8_t *e) {
    const int32_t state = rd<int32_t>(e, 0x1b0);
    return state == 1 ? 0 : state == 2 ? 1 : 2;
}
int lower_ascii(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
// < 0 when `a` comes first, > 0 when `b` does, 0 when they are equal for the list
int friend_order(const uint8_t *a, const uint8_t *b) {
    if (friend_group(a) != friend_group(b)) return friend_group(a) - friend_group(b);
    for (const uint8_t *x = a + 1, *y = b + 1;; ++x, ++y) {
        const int p = lower_ascii(*x), q = lower_ascii(*y);
        if (p != q) return p - q;
        if (!p) return 0;
    }
}
void copy_entry(uint8_t *to, const uint8_t *from) {   // byte by byte: the entries are read through other types too
    for (uintptr_t k = 0; k < FRIEND; ++k) to[k] = from[k];
}
void swap_entries(uint8_t *a, uint8_t *b) {
    for (uintptr_t k = 0; k < FRIEND; ++k) {
        const uint8_t t = a[k];
        a[k] = b[k];
        b[k] = t;
    }
}
}  // namespace

// Sorts entries [first, last] of `list`.
U3X_FN static void friend_quicksort(uint8_t *list, int64_t first, int64_t last, uint8_t *pivot) {
    auto at = [list](int64_t k) { return list + static_cast<uintptr_t>(k) * FRIEND; };
    while (last - first > 6) {
        // pivot: the median of the first, middle and last entries, which are left in order at first, first+1, last
        swap_entries(at((first + last) >> 1), at(first + 1));
        if (friend_order(at(first), at(last)) > 0) swap_entries(at(first), at(last));
        if (friend_order(at(first + 1), at(last)) > 0) swap_entries(at(first + 1), at(last));
        if (friend_order(at(first), at(first + 1)) > 0) swap_entries(at(first), at(first + 1));
        copy_entry(pivot, at(first + 1));
        // partition: entries before the pivot to the left, after it to the right
        int64_t up = first + 1, down = last;
        for (;;) {
            ++up;
            if (down >= up && friend_order(at(up), pivot) < 0) continue;
            do --down; while (down >= up && friend_order(at(down), pivot) > 0);
            if (down < up) break;
            swap_entries(at(up), at(down));
        }
        copy_entry(at(first + 1), at(down));
        copy_entry(at(down), pivot);
        // the shorter side first (recursion stays shallow), then the longer one in this loop
        if (last - up + 1 < down - first) {
            friend_quicksort(list, up, last, pivot);
            last = down - 1;
        } else {
            friend_quicksort(list, first, down - 1, pivot);
            first = up;
        }
    }
    // a short run: insertion sort (an entry moves left past every entry that comes after it)
    for (int64_t k = first + 1; k <= last; ++k) {
        copy_entry(pivot, at(k));
        int64_t to = k;
        while (to > 0 && friend_order(at(to - 1), pivot) > 0) {
            copy_entry(at(to), at(to - 1));
            --to;
        }
        copy_entry(at(to), pivot);
    }
}
extern "C" U3X_FN void u3x_friend_sort(uint8_t *entries, uint32_t count, uint8_t *spare) {
    if (count > 1) friend_quicksort(entries, 0, static_cast<int64_t>(count) - 1, spare);
}

// The index of `id` in a table of `count` 16-byte entries sorted by their first word, or -1.
extern "C" U3X_FN int32_t u3x_sorted_index(const uint8_t *entries, uint32_t count, uint32_t id) {
    if (!entries) return -1;
    const uint32_t total = count;
    uint32_t first = 0;
    while (count) {   // lower bound
        const uint32_t half = count / 2;
        if (rd<uint32_t>(entries, (first + half) * 16u) < id) { first += half + 1; count -= half + 1; }
        else count = half;
    }
    return first < total && rd<uint32_t>(entries, first * 16u) == id ? static_cast<int32_t>(first) : -1;
}

// Film data is a bit stream of 32-bit words, most significant bit first. Reads / writes `n` (1..32) bits at bit
// `pos`. A write clears the rest of the word(s) it ends in, so a stream is written front to back.
extern "C" U3X_FN uint32_t u3x_bits_get(const uint32_t *words, uint32_t pos, uint32_t n) {
    if (!n) return 0;
    const uint32_t at = pos >> 5, shift = pos & 31;
    const uint64_t both = static_cast<uint64_t>(words[at]) << 32 | (shift + n > 32 ? words[at + 1] : 0u);
    return static_cast<uint32_t>((both << shift) >> (64 - n));
}
extern "C" U3X_FN void u3x_bits_put(uint32_t *words, uint32_t pos, uint32_t value, uint32_t n) {
    const uint32_t at = pos >> 5, shift = pos & 31;
    const uint64_t keep = (static_cast<uint64_t>(words[at]) << 32) & ~(~0ull >> shift);
    const uint64_t field = n ? (static_cast<uint64_t>(value) << (64 - n)) >> shift : 0;
    const uint64_t both = keep | field;
    words[at] = static_cast<uint32_t>(both >> 32);
    if (shift + n > 32) words[at + 1] = static_cast<uint32_t>(both);
}

// Session members: 10 slots of 0x120 bytes from session +0x680 (in use at +0x25, member id at +0x2c, a second local
// player when the signed byte at +0x59 is non-zero). The slot of member `id`, and how many players a party has.
extern "C" U3X_FN uint8_t *u3x_find_member(uint8_t *session, uint32_t id) {
    for (uint8_t *m = session + 0x680, *end = m + 10 * 0x120; m != end; m += 0x120)
        if (m[0x25] && rd<uint32_t>(m, 0x2c) == id) return m;
    return nullptr;
}
extern "C" U3X_FN int u3x_party_players(const uint8_t *session) {
    if (!rd<uint64_t>(session, 0x18)) return 0;
    int players = 0;
    for (const uint8_t *m = session + 0x680, *end = m + 10 * 0x120; m != end; m += 0x120)
        if (m[0x25]) players += 1 + (static_cast<int8_t>(m[0x59]) != 0);
    return players;
}

// ===== v7.122: big batch, part A (same rules as above) =====

// A picture pager (the EULA pages): the list's questions about which page shows and how many there are, and the
// requests to go to a page or back to the first. Returns 1 for the questions it answers.
extern "C" U3X_FN int u3x_pager_answer(int32_t *ev, int32_t *current, int32_t pages) {
    switch (ev[0]) {
    case 5: ev[1] = *current; return 1;
    case 6: ev[1] = pages - 1; return 1;
    case 7: ev[1] = 0; return 1;
    case 10: *current = ev[1] < 0 ? 0 : ev[1] >= pages ? pages - 1 : ev[1]; return 1;
    case 23: if (ev[14]) *current = 0; return 1;
    default: return 0;
    }
}
// The menu's left / right step on that pager: 3 goes one page back, 4 one page on, never past the ends.
extern "C" U3X_FN void u3x_pager_step(int32_t *current, int32_t pages, int step) {
    if (step == 3 && *current > 0) --*current;
    else if (step == 4 && *current < pages - 1) ++*current;
}

// The profile box's experience bar: how far (in percent) the player is from this rank's threshold `low` to the
// next one `high`. 0 below the threshold and at the top rank, where both thresholds are the same.
extern "C" U3X_FN int32_t u3x_xp_percent(int32_t low, int32_t high, int32_t xp) {
    if (xp < low || high == low) return 0;
    return (xp - low) * 100 / (high - low);
}

// Booster level discs: the icon for level 0, 1 or 2 (none for other levels), and the word that marks a booster row
// as carrying its level for the disc drawer.
extern "C" U3X_FN uint32_t u3x_booster_level_icon(uint32_t level) {
    switch (level) {
    case 0: return 0x5ec0626au;
    case 1: return 0x5936816cu;
    case 2: return 0x512da466u;
    default: return 0;
    }
}
extern "C" U3X_FN uint32_t u3x_booster_level_mark(int level) { return 0x2e4c0000u | (static_cast<uint32_t>(level < 0 ? 0 : level) & 0xffu); }

// After a matchmade match the statistics page is shown on top for the ranked modes: every game mode except 0, 3
// and 8, and not for a private party.
extern "C" U3X_FN int u3x_shows_match_stats(uint32_t mode, int private_party) {
    return mode != 0 && mode != 3 && mode != 8 && !private_party;
}

// The end of a map vote: the second candidate wins only with more votes than the first. `vote` holds the two
// candidate net games at +8 / +0xc and their vote counts at +0x10 / +0x14.
extern "C" U3X_FN int32_t u3x_vote_winner(const uint8_t *vote) {
    return rd<int32_t>(vote, 0x14) > rd<int32_t>(vote, 0x10) ? rd<int32_t>(vote, 0xc) : rd<int32_t>(vote, 8);
}

// The matchmaking pool of a playlist entry: the first id of its pool list (+0x40: u32 count, ids from the pointer at
// +8), or 0 when it has none.
extern "C" U3X_FN uint32_t u3x_playlist_pool(const uint8_t *playlist) {
    const uint8_t *list = playlist ? *reinterpret_cast<const uint8_t *const *>(playlist + 0x40) : nullptr;
    const uint32_t *ids = list ? *reinterpret_cast<const uint32_t *const *>(list + 8) : nullptr;
    return (ids && rd<uint32_t>(list, 0)) ? ids[0] : 0;
}

// A party member's status byte in the shared member data: bit 0 custom game, bit 1 waiting to follow the host,
// bit 2 the host is in a match. And the "at the start" byte: the room being followed (else this one), with bit 0 set,
// or 0 when not at the start.
extern "C" U3X_FN uint8_t u3x_member_status(int custom_game, int guest_waiting, int host_playing) {
    return static_cast<uint8_t>((custom_game ? 1 : 0) | (guest_waiting ? 2 : 0) | (host_playing ? 4 : 0));
}
extern "C" U3X_FN uint8_t u3x_member_start(int at_start, uint64_t followed, uint64_t room) {
    return at_start ? static_cast<uint8_t>(static_cast<uint8_t>(followed ? followed : room) | 1) : 0;
}

// Challenge definitions: a table (i32 count, 0x28-byte entries from the pointer at +8, challenge number at +8).
// The entry of challenge `number`, or null.
extern "C" U3X_FN const uint8_t *u3x_challenge_entry(const uint8_t *table, int32_t number) {
    if (!table) return nullptr;
    const uint8_t *entry = *reinterpret_cast<const uint8_t *const *>(table + 8);
    if (!entry) return nullptr;
    for (int32_t left = rd<int32_t>(table, 0); left > 0; --left, entry += 0x28)
        if (rd<int32_t>(entry, 8) == number) return entry;
    return nullptr;
}

// ===== v7.123: big batch, part B (same rules as above) =====

// The icon answer most list items give: kind `kind`, no texture pointer, three data words (`a`, `b`, `c` in words
// 6..8), an index (word 9), the second index none (-1) and scale 1.0. Word 3 is left as it is.
extern "C" U3X_FN void u3x_icon_answer(int32_t *ev, int32_t kind, int32_t a, int32_t b, int32_t c, int32_t index) {
    ev[2] = kind;
    ev[4] = 0;
    ev[5] = 0;
    ev[6] = a;
    ev[7] = b;
    ev[8] = c;
    ev[9] = index;
    ev[10] = 0;
    ev[11] = -1;
    ev[12] = static_cast<int32_t>(F_ONE);
}

// The status icon of a locked item: 1, 2 and 3 have their own icons, the rest none (0).
extern "C" U3X_FN uint32_t u3x_unlock_icon(int status) {
    switch (status) {
    case 1: return 0xc2bb8920u;
    case 2: return 0x83817313u;
    case 3: return 0x7bebc95fu;
    default: return 0;
    }
}

// Matchmaking rows after the first (this player): the `row`-th other member of the match session (slots of 0x120
// bytes from +0x680, in use at +0x25, member id at +0x2c; this player's id at +0x12e4). Its name, or null.
extern "C" U3X_FN const char *u3x_other_member(const uint8_t *session, uint32_t row) {
    const uint32_t me = rd<uint32_t>(session, 0x12e4);
    uint32_t seen = 0;
    for (const uint8_t *m = session + 0x680, *end = m + 10 * 0x120; m != end; m += 0x120) {
        if (!m[0x25] || rd<uint32_t>(m, 0x2c) == me) continue;
        if (++seen == row) return reinterpret_cast<const char *>(m);
    }
    return nullptr;
}

// The particle module table hashes a module's id with 32-bit FNV-1a over its four bytes, low byte first.
extern "C" U3X_FN uint32_t u3x_fnv1a_id(uint32_t id) {
    uint32_t h = 0x811c9dc5u;
    for (int k = 0; k < 4; ++k) h = (h ^ ((id >> (8 * k)) & 0xffu)) * 0x1000193u;
    return h;
}
// Loaded particle modules: a count at +0x8c8 (at most 64 are trusted) and 0x30-byte records from +0x298, id first.
extern "C" U3X_FN int u3x_module_loaded(const uint8_t *modules, uint32_t id) {
    const uint32_t count = rd<uint32_t>(modules, 0x8c8);
    if (count > 64) return 0;
    for (uint32_t k = 0; k < count; ++k)
        if (rd<uint32_t>(modules, 0x298 + static_cast<uintptr_t>(k) * 0x30) == id) return 1;
    return 0;
}

// Elimination: a net player who is in the game (+0xe1 set, +0x5e4 clear), has joined (+0x5f9) and has not spawned in
// this round yet (+0x5fa clear). The round end waits for such players.
extern "C" U3X_FN int u3x_waiting_joiner(const uint8_t *net_player) {
    return net_player[0xe1] && !net_player[0x5e4] && net_player[0x5f9] && !net_player[0x5fa];
}

// ===== v7.124: big batch, part C (same rules as above) =====

// Like u3x_retag_items, for one widget: only items whose widget id (+8) is `widget`, on pages of at most `max_items`.
extern "C" U3X_FN uint32_t u3x_retag_widget(uint8_t *page, uint32_t widget, uint32_t source, uint32_t tag, uint32_t max_items) {
    uint8_t *column = *reinterpret_cast<uint8_t *const *>(page + 0x48);
    if (!column) return 0;
    const uint32_t count = rd<uint32_t>(column, 0);
    uint8_t *item = *reinterpret_cast<uint8_t *const *>(column + 8);
    if (!item || count > max_items) return 0;
    uint32_t changed = 0;
    for (uint8_t *end = item + static_cast<uintptr_t>(count) * 0x68; item != end; item += 0x68) {
        if (rd<uint32_t>(item, 8) != widget || rd<uint32_t>(item, 0x30) != 21 || rd<uint32_t>(item, 0x34) != source ||
            rd<uint32_t>(item, 0x40) != 0)
            continue;
        *reinterpret_cast<uint32_t *>(item + 0x34) = 0xcf;
        *reinterpret_cast<uint32_t *>(item + 0x40) = tag;
        ++changed;
    }
    return changed;
}

// Decimal text of a whole number (with a minus sign when negative), written from `out` up to `end`; returns the end.
namespace {
char *put_text(char *out, char *end, const char *text) {
    while (*text && out < end) *out++ = *text++;
    return out;
}
char *put_number(char *out, char *end, int64_t value) {
    char digits[24];
    int n = 0;
    uint64_t left = value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    do {
        digits[n++] = static_cast<char>('0' + left % 10);
        left /= 10;
    } while (left && n < 22);
    if (value < 0 && out < end) *out++ = '-';
    while (n && out < end) *out++ = digits[--n];
    return out;
}
}  // namespace

// The time-limit row of a game type: the game type's rule (entry +0x10 base, +0x18 step) gives `base + step * count`
// for the chosen setting, shown before the row's own label ("10 Minutes"). Writes the text into [out, end].
extern "C" U3X_FN void u3x_time_limit_text(char *out, char *end, const uint8_t *rule, int32_t count, const char *label) {
    const int64_t value = static_cast<int64_t>(rd<int32_t>(rule, 0x18)) * count + rd<int32_t>(rule, 0x10);
    out = put_number(out, end, value);
    if (*label) {
        out = put_text(out, end, " ");
        out = put_text(out, end, label);
    }
    *out = 0;
}
extern "C" U3X_FN int u3x_has_digit(const char *text) {
    for (; *text; ++text)
        if (*text >= '0' && *text <= '9') return 1;
    return 0;
}

// The legacy badge's back picture: "ranks/prestige-back-<legacy>", written into [out, end].
U3X_RO const char legacy_back[] = "ranks/prestige-back-";
extern "C" U3X_FN void u3x_legacy_back_name(char *out, char *end, int32_t legacy) {
    out = put_text(out, end, legacy_back);
    out = put_number(out, end, legacy);
    *out = 0;
}

// Deleting LAN profile `index`: the names after it move up one place and the last one is cleared.
extern "C" U3X_FN void u3x_remove_name(uint8_t *names, uint32_t count, uint32_t index, uint32_t bytes) {
    for (uint32_t k = index; k + 1 < count; ++k)
        for (uint32_t b = 0; b < bytes; ++b) names[k * bytes + b] = names[(k + 1) * bytes + b];
    for (uint32_t b = 0; b < bytes; ++b) names[(count - 1) * bytes + b] = 0;
}

// Two player keys are the same when they match up to their end (at most 32 bytes compared).
extern "C" U3X_FN int u3x_key_equal(const uint8_t *a, const uint8_t *b) {
    for (int k = 0; k < 32; ++k) {
        if (a[k] != b[k]) return 0;
        if (!a[k]) return 1;
    }
    return 1;
}
// The members of a session (backend at +8 and a session id at +0x18 required): every slot in use, with this player's
// own member moved to the front (the one that was first goes to the end of the list). Returns how many.
extern "C" U3X_FN uint32_t u3x_session_members(uint8_t *session, uint8_t **out) {
    if (!*reinterpret_cast<uint8_t *const *>(session + 8) || !rd<uint64_t>(session, 0x18)) return 0;
    const uint32_t me = rd<uint32_t>(session, 0x12e4);
    uint32_t n = 0;
    for (uint8_t *m = session + 0x680, *end = m + 10 * 0x120; m != end; m += 0x120) {
        if (!m[0x25]) continue;
        if (n && rd<uint32_t>(m, 0x2c) == me) {
            out[n++] = out[0];
            out[0] = m;
        } else {
            out[n++] = m;
        }
    }
    return n;
}

// ===== v7.125: big batch, part D (same rules as above) =====

// Standard algorithms used by the online profile seal: SHA-1 (FIPS 180-4), HMAC (RFC 2104) and Blowfish (Schneier).
// The keys are not here: the Blowfish state and the HMAC key come from the player's own EBOOT (see the key block).
namespace {
uint32_t load_be(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}
void store_be(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
uint32_t rotl(uint32_t v, unsigned by) { return (v << by) | (v >> (32 - by)); }

struct Sha1 {
    uint32_t h[5];
    uint8_t block[64];
    uint32_t used = 0;
    uint64_t total = 0;
    [[gnu::always_inline]] Sha1() {   // the start state, set word by word (a constant table would land outside the helper)
        h[0] = 0x67452301u; h[1] = 0xefcdab89u; h[2] = 0x98badcfeu; h[3] = 0x10325476u; h[4] = 0xc3d2e1f0u;
    }

    U3X_FN void compress() {
        uint32_t w[80];
        for (int t = 0; t < 16; ++t) w[t] = load_be(block + 4 * t);
        for (int t = 16; t < 80; ++t) w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int t = 0; t < 80; ++t) {
            uint32_t f, k;
            if (t < 20) { f = (b & c) | (~b & d); k = 0x5a827999u; }
            else if (t < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
            else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
            else { f = b ^ c ^ d; k = 0xca62c1d6u; }
            const uint32_t next = rotl(a, 5) + f + e + k + w[t];
            e = d; d = c; c = rotl(b, 30); b = a; a = next;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    U3X_FN void add(const uint8_t *data, uint32_t length) {
        total += length;
        for (uint32_t k = 0; k < length; ++k) {
            block[used++] = data[k];
            if (used == 64) { compress(); used = 0; }
        }
    }
    U3X_FN void finish(uint8_t *digest) {
        const uint64_t bits = total * 8;
        block[used++] = 0x80;
        if (used > 56) {
            while (used < 64) block[used++] = 0;
            compress();
            used = 0;
        }
        while (used < 56) block[used++] = 0;
        for (int k = 0; k < 8; ++k) block[56 + k] = static_cast<uint8_t>(bits >> (56 - 8 * k));
        compress();
        for (int k = 0; k < 5; ++k) store_be(digest + 4 * k, h[k]);
    }
};
}  // namespace

extern "C" U3X_FN void u3x_hmac_sha1(const uint8_t *key, uint32_t key_length, const uint8_t *data, uint32_t length,
                                     uint8_t *mac) {
    uint8_t padded[64];
    for (int k = 0; k < 64; ++k) padded[k] = 0;
    if (key_length > 64) {
        Sha1 short_key;
        short_key.add(key, key_length);
        short_key.finish(padded);
    } else {
        for (uint32_t k = 0; k < key_length; ++k) padded[k] = key[k];
    }
    uint8_t pad[64], inner[20];
    for (int k = 0; k < 64; ++k) pad[k] = padded[k] ^ 0x36;
    Sha1 first;
    first.add(pad, 64);
    first.add(data, length);
    first.finish(inner);
    for (int k = 0; k < 64; ++k) pad[k] = padded[k] ^ 0x5c;
    Sha1 second;
    second.add(pad, 64);
    second.add(inner, 20);
    second.finish(mac);
}

// One 8-byte Blowfish block (big-endian halves) with a scheduled state: 18 P words, then four 256-word S boxes.
extern "C" U3X_FN void u3x_blowfish(const uint32_t *state, uint8_t *block, int decrypt) {
    const uint32_t *p = state, *s = state + 18;
    auto round_f = [s](uint32_t x) { return ((s[x >> 24] + s[256 + ((x >> 16) & 0xff)]) ^ s[512 + ((x >> 8) & 0xff)]) + s[768 + (x & 0xff)]; };
    uint32_t left = load_be(block), right = load_be(block + 4);
    for (int round = 0; round < 16; ++round) {
        left ^= p[decrypt ? 17 - round : round];
        right ^= round_f(left);
        const uint32_t t = left; left = right; right = t;
    }
    const uint32_t t = left; left = right; right = t;
    right ^= p[decrypt ? 1 : 16];
    left ^= p[decrypt ? 0 : 17];
    store_be(block, left);
    store_be(block + 4, right);
}

// The online profile record's seal: bytes +8..+0x2820 in Blowfish blocks, with an HMAC-SHA1 of +8..+0x280c stored at
// +0x280c. Unseal decrypts and checks it (1 = good); seal writes the MAC and encrypts.
extern "C" U3X_FN int u3x_unseal_record(uint8_t *record, const uint32_t *state, const uint8_t *key, uint32_t key_length) {
    if (load_be(record + 4) != 0x2818u) return 0;
    for (uint32_t at = 8; at < 0x2820; at += 8) u3x_blowfish(state, record + at, 1);
    uint8_t mac[20];
    u3x_hmac_sha1(key, key_length, record + 8, 0x2804, mac);
    for (int k = 0; k < 20; ++k)
        if (mac[k] != record[0x280c + k]) return 0;
    return 1;
}
extern "C" U3X_FN void u3x_seal_record(uint8_t *record, const uint32_t *state, const uint8_t *key, uint32_t key_length) {
    u3x_hmac_sha1(key, key_length, record + 8, 0x2804, record + 0x280c);
    for (uint32_t at = 8; at < 0x2820; at += 8) u3x_blowfish(state, record + at, 0);
}

// ===== v7.126: big batch, part E (same rules as above) =====

// Invite-server frames: a 0x14-byte header (0x33, pad count, 16-bit big-endian data length, 16-byte MAC) and the data
// padded to whole 32-bit words, enciphered. The cipher keeps four 32-bit words that each data word (little-endian)
// stirs; the same stirring gives the MAC. The words start from the player's 16-byte session value and the frame's
// counter. Unlike the session hello's checksum, the second word stays inverted from one data word to the next.
namespace {
enum class Pass { Mac, Encrypt, Decrypt };
struct FrameCipher {
    uint32_t w[4];
    static uint32_t rol(uint32_t v, unsigned by) { return (v << by) | (v >> (32 - by)); }
    static uint32_t word(const uint8_t *p) {
        return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
               static_cast<uint32_t>(p[3]) << 24;
    }
    static void put(uint8_t *p, uint32_t v) {
        for (int k = 0; k < 4; ++k) p[k] = static_cast<uint8_t>(v >> (8 * k));
    }
    U3X_FN void run(uint8_t *data, uint32_t length, Pass pass) {
        uint32_t a = w[0], b = w[1], c = w[2], d = w[3];
        for (uint32_t at = 0; at + 4 <= length; at += 4) {
            d ^= 0x5b3aa654u;
            c = rol(c + a + d, 7);
            b = rol(b + d + c, 11);
            c ^= 0x75970a4du;
            a = rol(a + c + b, 17);
            const uint32_t both = a & b;
            b = ~b;
            d += both | (c & b);
            a = ~a;
            uint32_t value = word(data + at);
            if (pass == Pass::Decrypt) {
                const uint32_t plain = d ^ value;
                d = value;
                value = plain;
            } else {
                d ^= value;
                value = d;
            }
            if (pass != Pass::Mac) put(data + at, value);
        }
        w[0] = a; w[1] = b; w[2] = c; w[3] = d;
    }
    U3X_FN void start(const uint8_t *key, uint32_t counter) {
        for (int k = 0; k < 4; ++k) w[k] = word(key + 4 * k);
        uint8_t mix[16];
        put(mix, counter);
        run(mix, 4, Pass::Mac);
        for (int k = 0; k < 4; ++k) put(mix + 4 * k, w[k]);
        run(mix, 16, Pass::Encrypt);
    }
    void mac(uint8_t *out) const {
        const uint32_t all = w[0] ^ w[1] ^ w[2] ^ w[3];
        for (int k = 0; k < 4; ++k) put(out + 4 * k, all ^ w[k]);
    }
};
}  // namespace

// Builds a frame from `data` (`length` bytes) into `out`; returns the frame's length.
extern "C" U3X_FN int u3x_frame_seal(uint8_t *out, const uint8_t *data, uint32_t length, uint32_t counter, const uint8_t *key) {
    const uint32_t pad = (0u - length) & 3u;
    out[0] = 0x33;
    out[1] = static_cast<uint8_t>(pad);
    out[2] = static_cast<uint8_t>(length >> 8);
    out[3] = static_cast<uint8_t>(length);
    uint8_t *body = out + 0x14;
    for (uint32_t k = 0; k < length; ++k) body[k] = data[k];
    for (uint32_t k = 0; k < pad; ++k) body[length + k] = 0;
    FrameCipher cipher;
    cipher.start(key, counter);
    cipher.run(body, length + pad, Pass::Mac);
    cipher.mac(out + 4);
    cipher.start(key, counter);
    cipher.run(body, length + pad, Pass::Encrypt);
    return static_cast<int>(0x14 + length + pad);
}
// Opens a received frame of `total` bytes in place; returns its data length, or -1 when it is malformed or its MAC
// does not match. The server's pad count can be one short: the frame is whole words.
extern "C" U3X_FN int u3x_frame_open(uint8_t *frame, uint32_t total, uint32_t counter, const uint8_t *key) {
    if (total < 0x14 || frame[0] != 0x33) return -1;
    const uint32_t pad = frame[1], length = static_cast<uint32_t>(frame[2]) << 8 | frame[3];
    const uint32_t words = (length + pad + 3u) & ~3u;
    if (0x14 + words != total) return -1;
    uint8_t *body = frame + 0x14, mac[16];
    FrameCipher cipher;
    cipher.start(key, counter);
    cipher.run(body, words, Pass::Decrypt);
    cipher.start(key, counter);
    cipher.run(body, words, Pass::Mac);
    cipher.mac(mac);
    for (int k = 0; k < 16; ++k)
        if (mac[k] != frame[4 + k]) return -1;
    return static_cast<int>(length);
}

// Base64 with the standard alphabet and '=' padding (zero terminated; returns the text length), and the value of
// one character when reading: the standard ones, plus '*' for 62 and '-' for 63 (the server's URL-safe form); 0xff
// for anything else.
U3X_RO const char base64_digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
extern "C" U3X_FN uint32_t u3x_base64(char *out, const uint8_t *data, uint32_t length) {
    uint32_t used = 0, at = 0;
    for (; at + 3 <= length; at += 3) {
        const uint32_t v = static_cast<uint32_t>(data[at]) << 16 | static_cast<uint32_t>(data[at + 1]) << 8 | data[at + 2];
        for (int k = 3; k >= 0; --k) out[used++] = base64_digits[(v >> (6 * k)) & 63];
    }
    if (length - at == 1) {
        const uint32_t v = static_cast<uint32_t>(data[at]) << 16;
        out[used++] = base64_digits[(v >> 18) & 63];
        out[used++] = base64_digits[(v >> 12) & 63];
        out[used++] = '=';
        out[used++] = '=';
    } else if (length - at == 2) {
        const uint32_t v = static_cast<uint32_t>(data[at]) << 16 | static_cast<uint32_t>(data[at + 1]) << 8;
        out[used++] = base64_digits[(v >> 18) & 63];
        out[used++] = base64_digits[(v >> 12) & 63];
        out[used++] = base64_digits[(v >> 6) & 63];
        out[used++] = '=';
    }
    out[used] = 0;
    return used;
}
extern "C" U3X_FN uint8_t u3x_base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<uint8_t>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<uint8_t>(c - 'a' + 26);
    if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0' + 52);
    if (c == '+' || c == '*') return 62;
    if (c == '/' || c == '-') return 63;
    return 0xff;
}

// ===== v7.127: big batch, part F (same rules as above) =====

// The game-socket bit stream: words of 32 bits, filled most significant bit first, `pos` bits used of `max`. Bits
// past the end are dropped when writing and read as 0. The layout matches the C part's GtBits.
struct GtStream { uint32_t *words; uint32_t pos, max, x, pad; };
extern "C" U3X_FN void u3x_gt_write(GtStream *s, uint32_t value, int count) {
    for (int bit = count - 1; bit >= 0; --bit) {
        const uint32_t at = s->pos++;
        if (at >= s->max) continue;
        const uint32_t mask = 0x80000000u >> (at & 31);
        if ((value >> bit) & 1) s->words[at >> 5] |= mask;
        else s->words[at >> 5] &= ~mask;
    }
}
extern "C" U3X_FN uint32_t u3x_gt_read(GtStream *s, int count) {
    uint32_t value = 0;
    for (int k = 0; k < count; ++k) {
        const uint32_t at = s->pos++;
        const uint32_t bit = at < s->max ? (s->words[at >> 5] >> (31 - (at & 31))) & 1 : 0;
        value = value << 1 | bit;
    }
    return value;
}
// Bytes to send: the bits used (at most `max`), rounded up to whole words.
extern "C" U3X_FN uint32_t u3x_gt_bytes(const GtStream *s) {
    const uint32_t used = s->pos < s->max ? s->pos : s->max;
    return (used + 31) / 32 * 4;
}

// Experience thresholds. Legacy (prestige) levels 4 and 5 need 2.5 and 5 times the experience; the game computes it
// in single-precision floats and truncates, which the helper does the same way (the f_* helpers above).
namespace {
[[gnu::always_inline]] inline uint32_t f_from_int(int32_t v) {
    uint32_t r;
    __asm__("cvtsi2ss %1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(v) : "xmm0");
    return r;
}
[[gnu::always_inline]] inline int32_t f_to_int(uint32_t a) {
    int32_t r;
    __asm__("movd %1,%%xmm0\n\tcvttss2si %%xmm0,%0" : "=r"(r) : "r"(a) : "xmm0");
    return r;
}
}  // namespace
extern "C" U3X_FN int32_t u3x_xp_scale(int32_t xp, int32_t legacy) {
    if (legacy == 4) return f_to_int(f_mul(f_from_int(xp), 0x40200000u));   // x 2.5
    if (legacy == 5) return f_to_int(f_mul(f_from_int(xp), 0x40a00000u));   // x 5
    return xp;
}
// The threshold of `level` in the table (`count` values), clamped to the table.
extern "C" U3X_FN int32_t u3x_level_threshold(const int32_t *table, int32_t count, int32_t level, int32_t legacy) {
    int32_t at = level < 0 ? 0 : level;
    if (at > count - 1) at = count - 1;
    return u3x_xp_scale(table[at], legacy);
}
// The level for an amount of experience: the first level whose band [threshold, next threshold) holds it, else the
// top level.
extern "C" U3X_FN int32_t u3x_level_for_xp(const int32_t *table, int32_t count, int32_t xp, int32_t legacy) {
    for (int32_t level = 0; level + 1 < count; ++level)
        if (u3x_xp_scale(table[level], legacy) <= xp && xp < u3x_xp_scale(table[level + 1], legacy)) return level;
    return count - 1;
}

// ===== v7.128: big batch, part G (same rules as above) =====

// Text helpers of the friend list: compare ignoring ASCII case (-1, 0, 1), and copy at most `n` bytes, filling the
// rest of the `n` with zeros.
extern "C" U3X_FN int u3x_strcasecmp(const char *a, const char *b) {
    for (;; ++a, ++b) {
        const int x = lower_ascii(static_cast<uint8_t>(*a)), y = lower_ascii(static_cast<uint8_t>(*b));
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
extern "C" U3X_FN void u3x_strncpy_pad(char *to, const char *from, uint32_t n) {
    uint32_t k = 0;
    for (; k < n && from[k]; ++k) to[k] = from[k];
    for (; k < n; ++k) to[k] = 0;
}
// A friend is playing this game when their presence says so (state 1), or when they are online (state 2) and their
// presence's game title is this game's.
extern "C" U3X_FN int u3x_friend_in_game(const uint8_t *entry, const char *title) {
    const int32_t state = rd<int32_t>(entry, 0x1b0);
    if (state == 1) return 1;
    if (state != 2) return 0;
    return u3x_strcasecmp(reinterpret_cast<const char *>(entry + 0x28), title) == 0;
}

// The statistics card shows the viewer's own record when the selected player id is the viewer's: ids are compared up
// to their end (at most 32 bytes); an empty selection is nobody.
extern "C" U3X_FN int u3x_same_player(const uint8_t *selected, const uint8_t *id) {
    if (!selected[0]) return 0;
    for (int k = 0; k < 0x20; ++k) {
        if (selected[k] != id[k]) return 0;
        if (!selected[k]) return 1;
    }
    return 1;
}

// Co-op Find Game: the difficulty vote follows the map vote for the co-op game types (4, 8, 9, 11, 13, 16, 17).
extern "C" U3X_FN int u3x_has_difficulty_vote(uint32_t game_type) {
    switch (game_type) {
    case 4: case 8: case 9: case 11: case 13: case 16: case 17: return 1;
    default: return 0;
    }
}
// The vote block before a vote: open (byte 0 set, byte 1 clear), this player's vote none (-1), the four counts zero.
extern "C" U3X_FN void u3x_vote_open(uint8_t *vote) {
    *reinterpret_cast<int32_t *>(vote + 4) = -1;
    vote[1] = 0;
    vote[0] = 1;
    for (int k = 0; k < 4; ++k) *reinterpret_cast<int32_t *>(vote + 8 + 4 * k) = 0;
}
// The stand-in players' difficulty votes, from the clock: Easy 1, Normal 4, Hard 4, Crushing 1 in 10. Row 0..3.
extern "C" U3X_FN int u3x_stand_in_vote(uint32_t now) {
    const uint32_t roll = ((now * 2654435761u) >> 9) % 10u;
    if (roll == 0) return 0;
    if (roll < 5) return 1;
    if (roll < 9) return 2;
    return 3;
}

// ===== v7.129: big batch, part H (same rules as above) =====

// Cinema film list entries (0x1e90 bytes): the date text and file name (+0, +0x100, the flag byte at +0x200), the
// time, size and mode (+0x208..+0x217) and the film header (+0x218, 0x1c78 bytes). Bytes +0x201..+0x207 are not part of
// an entry and stay where they are.
extern "C" U3X_FN void u3x_film_entry_copy(uint8_t *to, const uint8_t *from) {
    for (uint32_t k = 0; k <= 0x200; ++k) to[k] = from[k];
    for (uint32_t k = 0x208; k < 0x1e90; ++k) to[k] = from[k];
}
// The film list, newest first; films with the same time keep their order. `spare` holds one entry.
extern "C" U3X_FN void u3x_sort_films(uint8_t *list, int count, uint8_t *spare) {
    auto at = [list](int k) { return list + static_cast<uintptr_t>(k) * 0x1e90; };
    auto time = [](const uint8_t *e) { return *reinterpret_cast<const int64_t *>(e + 0x208); };
    for (int k = 1; k < count; ++k) {
        if (time(at(k - 1)) >= time(at(k))) continue;
        u3x_film_entry_copy(spare, at(k));
        int to = k;
        for (; to > 0 && time(at(to - 1)) < time(spare); --to) u3x_film_entry_copy(at(to), at(to - 1));
        u3x_film_entry_copy(at(to), spare);
    }
}

// A film's date as e-mail headers write it ("Thu, 12 Nov 2026 18:44:32 +0100"), from the local and UTC broken-down
// times (sec, min, hour, mday, mon, year since 1900, wday, yday, isdst). The zone is the difference of the two; at a
// year change the day difference is one day either way. At most 63 characters.
U3X_RO const char day_names[] = "SunMonTueWedThuFriSat";
U3X_RO const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
U3X_RO const char comma_space[] = ", ";
U3X_RO const char zone_minus[] = " -";
U3X_RO const char zone_plus[] = " +";
namespace {
char *put_padded(char *out, char *end, int64_t value, int width) {
    char digits[24];
    int n = 0;
    uint64_t left = value < 0 ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    do {
        digits[n++] = static_cast<char>('0' + left % 10);
        left /= 10;
    } while (left && n < 22);
    if (value < 0 && out < end) *out++ = '-';
    while (n < width && n < 22) digits[n++] = '0';
    while (n && out < end) *out++ = digits[--n];
    *out = 0;
    return out;
}
char *put_three(char *out, char *end, const char *names, int index) {
    for (int k = 0; k < 3 && out < end; ++k) *out++ = names[index * 3 + k];
    *out = 0;
    return out;
}
char *put_char(char *out, char *end, char c) {
    if (out < end) *out++ = c;
    *out = 0;
    return out;
}
}  // namespace
extern "C" U3X_FN void u3x_rfc2822(char *out, const int32_t *local, const int32_t *utc) {
    enum { SEC, MIN, HOUR, MDAY, MON, YEAR, WDAY, YDAY };
    int64_t days = static_cast<int64_t>(local[YEAR] - utc[YEAR]) * 366 + (local[YDAY] - utc[YDAY]);
    if (days > 1) days = -1;
    if (days < -1) days = 1;
    int64_t zone = days * 1440 + (local[HOUR] - utc[HOUR]) * 60 + (local[MIN] - utc[MIN]);
    char *end = out + 63, *p = out;
    *p = 0;
    p = put_three(p, end, day_names, local[WDAY] % 7);
    p = put_text(p, end, comma_space);
    p = put_padded(p, end, local[MDAY], 2);
    p = put_char(p, end, ' ');
    p = put_three(p, end, month_names, local[MON] % 12);
    p = put_char(p, end, ' ');
    p = put_padded(p, end, local[YEAR] + 1900, 4);
    p = put_char(p, end, ' ');
    p = put_padded(p, end, local[HOUR], 2);
    p = put_char(p, end, ':');
    p = put_padded(p, end, local[MIN], 2);
    p = put_char(p, end, ':');
    p = put_padded(p, end, local[SEC], 2);
    p = put_text(p, end, zone < 0 ? zone_minus : zone_plus);
    if (zone < 0) zone = -zone;
    p = put_padded(p, end, zone / 60, 2);
    put_padded(p, end, zone % 60, 2);
}

// ===== v7.130: big batch, part I (same rules as above) =====

// net18.bin is a big-endian data file ("DC00"): a symbol count at +0x14 and 12-byte symbols {id, type, offset} from
// +0x18. The server list is the symbol 0x1d38f2f9. Returns its offset, or 0 when the file or the symbol is not usable.
namespace {
uint32_t be32(const uint8_t *p) { return load_be(p); }
}  // namespace
extern "C" U3X_FN uint32_t u3x_server_list(const uint8_t *file, uint32_t size) {
    if (size < 0x18 || file[0] != 'D' || file[1] != 'C' || file[2] != '0' || file[3] != '0') return 0;
    const uint32_t symbols = be32(file + 0x14);
    if (symbols > 4096 || 0x18 + symbols * 12 > size) return 0;
    uint32_t list = 0;
    for (uint32_t k = 0; k < symbols && !list; ++k)
        if (be32(file + 0x18 + k * 12) == 0x1d38f2f9u) list = be32(file + 0x18 + k * 12 + 8);
    if (!list || list + 0xa0 > size) return 0;
    return list;
}
// One server of that list: the field at `field` points to {count, first entry}; the entry is {host name offset,
// port}. Writes the host (at most 63 characters) and port only when everything is inside the file. 1 = written.
extern "C" U3X_FN int u3x_server_entry(const uint8_t *file, uint32_t size, uint32_t field, char *host, uint32_t *port) {
    const uint32_t ref = be32(file + field);
    if (ref + 8 > size || be32(file + ref) < 1) return 0;
    const uint32_t entry = be32(file + ref + 4);
    if (entry + 8 > size) return 0;
    const uint32_t name = be32(file + entry), number = be32(file + entry + 4);
    if (name >= size || number > 0xffff) return 0;
    int k = 0;
    for (; k < 63 && name + static_cast<uint32_t>(k) < size && file[name + k]; ++k) host[k] = static_cast<char>(file[name + k]);
    host[k] = 0;
    *port = number;
    return 1;
}

// LAN addresses learnt from the broadcast: up to 32 records of 0x28 bytes (a 0x24-byte name, then the IPv4 address)
// after a count. A known name gets the new address; a new one is added (the slot wraps round after 32).
namespace {
bool same_name(const uint8_t *a, const char *b) {
    for (int k = 0; k < 0x24; ++k) {
        if (a[k] != static_cast<uint8_t>(b[k])) return false;
        if (!a[k]) return true;
    }
    return true;
}
}  // namespace
extern "C" U3X_FN void u3x_lan_address(uint32_t *count, uint8_t *records, const char *name, uint32_t ip) {
    const uint32_t n = *count;
    for (uint32_t k = 0; k < n && k < 32; ++k) {
        uint8_t *r = records + k * 0x28;
        if (same_name(r, name)) {
            *reinterpret_cast<uint32_t *>(r + 0x24) = ip;
            return;
        }
    }
    uint8_t *r = records + (n & 0x1f) * 0x28;
    for (int k = 0; k < 0x24; ++k) r[k] = static_cast<uint8_t>(name[k]);
    *reinterpret_cast<uint32_t *>(r + 0x24) = ip;
    *count = n + 1;
}

// Session-server messages (ids 301..326) travel big-endian. This table gives, per message (id - 300), its header
// length, where its repeated records start and how long they are, then (offset, width) pairs of the header fields to
// byte-swap, 0xff, the same for one record, 0xff. It is the wire format, as the server sends it.
U3X_RO const uint8_t message_fields[] = {
    3, 232, 0, 0, 0, 4, 8, 4, 12, 4, 16, 4, 20, 4, 24, 2, 26, 2, 28, 2, 32, 2, 34, 2, 36, 2, 0xff, 0xff,
    4, 88, 0, 0, 0, 4, 8, 4, 16, 8, 0xff, 0xff,
    5, 160, 160, 104, 0, 4, 8, 4, 12, 2, 14, 2, 16, 8, 24, 2, 26, 2, 0xff, 36, 2, 0xff,
    6, 120, 0, 0, 0, 4, 8, 8, 52, 2, 0xff, 0xff,
    7, 16, 0, 0, 0, 4, 8, 8, 0xff, 0xff,
    8, 24, 0, 0, 0, 4, 8, 8, 16, 2, 0xff, 0xff,
    9, 36, 0, 0, 0, 4, 8, 4, 12, 4, 16, 4, 20, 2, 22, 2, 24, 2, 28, 2, 30, 2, 32, 4, 0xff, 0xff,
    10, 16, 16, 56, 0, 4, 8, 4, 12, 4, 0xff, 0, 8, 12, 2, 16, 2, 0xff,
    11, 16, 0, 0, 0, 4, 4, 2, 6, 2, 8, 8, 0xff, 0xff,
    12, 16, 0, 0, 0, 4, 8, 8, 0xff, 0xff, 13, 16, 0, 0, 0, 4, 8, 8, 0xff, 0xff,
    14, 80, 0, 0, 0, 4, 8, 8, 0xff, 0xff,
    15, 80, 0, 0, 0, 4, 4, 2, 8, 8, 0xff, 0xff,
    16, 16, 0, 0, 0, 4, 4, 2, 8, 8, 0xff, 0xff, 17, 16, 0, 0, 0, 4, 4, 2, 8, 8, 0xff, 0xff,
    18, 16, 0, 0, 0, 4, 8, 8, 0xff, 0xff, 19, 16, 0, 0, 0, 4, 8, 8, 0xff, 0xff,
    20, 16, 0, 0, 0, 4, 4, 2, 8, 8, 0xff, 0xff, 21, 16, 0, 0, 0, 4, 4, 2, 8, 8, 0xff, 0xff,
    22, 16, 16, 2, 0, 4, 4, 2, 8, 8, 0xff, 0, 2, 0xff,
    23, 144, 0, 0, 0, 4, 8, 8, 0xff, 0xff, 24, 144, 0, 0, 0, 4, 8, 8, 0xff, 0xff,
    25, 4, 0, 0, 0, 4, 0xff, 0xff,
    0};
namespace {
const uint8_t *message_layout(uint32_t id) {
    for (const uint8_t *p = message_fields; *p;) {
        if (*p == id - 300) return p;
        p += 4;
        while (*p != 0xff) p += 2;
        ++p;
        while (*p != 0xff) p += 2;
        ++p;
    }
    return nullptr;
}
const uint8_t *swap_fields(uint8_t *message, const uint8_t *pairs) {
    for (; *pairs != 0xff; pairs += 2) {
        uint8_t *field = message + pairs[0];
        const int width = pairs[1];
        for (int k = 0; k < width / 2; ++k) {
            const uint8_t t = field[k];
            field[k] = field[width - 1 - k];
            field[width - 1 - k] = t;
        }
    }
    return pairs + 1;
}
}  // namespace
// Swaps every field of message `id` (`length` bytes, its records included). Returns 0 for an unknown id.
extern "C" U3X_FN int u3x_message_swap(uint8_t *message, uint32_t id, uint32_t length) {
    const uint8_t *layout = message_layout(id);
    if (!layout) return 0;
    const uint32_t records = layout[2], record_length = layout[3];
    const uint8_t *record_pairs = swap_fields(message, layout + 4);
    if (record_length)
        for (uint32_t at = records; at + record_length <= length; at += record_length) swap_fields(message + at, record_pairs);
    return 1;
}
// Bytes the big-endian message at `message` (`have` bytes there) needs: 0 for an unknown id; for the two messages
// with records, the header's record count decides (until it has arrived, the header length).
extern "C" U3X_FN uint32_t u3x_message_length(const uint8_t *message, uint32_t have) {
    const uint32_t id = be32(message);
    const uint8_t *layout = (id >= 301 && id <= 326) ? message_layout(id) : nullptr;
    if (!layout) return 0;
    if (id == 305) return have < 28 ? 160u : 160u + (static_cast<uint32_t>(message[26]) << 8 | message[27]) * 104u;
    if (id == 310) return have < 16 ? 16u : 16u + be32(message + 12) * 56u;
    return layout[1];
}

// ===== v7.131: big batch, part J (same rules as above) =====

// A net player's id: its low 12 bits (+0x1e4), moved up by 0x800 when the player is a guest (+0x1ec) and the bits are
// not zero, plus 0x1000 times its user number (+0x1e8).
extern "C" U3X_FN uint32_t u3x_net_player_id(const uint8_t *np) {
    uint32_t low = rd<uint32_t>(np, 0x1e4) & 0xfff;
    if (np[0x1ec] && low) low += 0x800;
    return low + rd<uint32_t>(np, 0x1e8) * 0x1000u;
}
// A net player slot that is in the game: joined (+0xe1) and not leaving (+0x5e4).
extern "C" U3X_FN int u3x_net_player_in_game(const uint8_t *np) { return np[0xe1] && !np[0x5e4]; }

// Object tags: the tag table keeps, per bucket, a circular list (the bucket head is its end) of nodes {next, ...,
// +0x10 tag id, +0x14 bit number}. An object's tags are a bit set at +0x20. Whether `tags` has tag `id` (the walk stops
// after 4096 nodes).
extern "C" U3X_FN int u3x_has_tag(const uint8_t *bucket_head, const uint8_t *tags, uint32_t id) {
    const uint8_t *node = *reinterpret_cast<const uint8_t *const *>(bucket_head);
    for (int steps = 0; node && node != bucket_head && steps < 4096; ++steps, node = *reinterpret_cast<const uint8_t *const *>(node)) {
        if (rd<uint32_t>(node, 0x10) != id) continue;
        const uint32_t bit = rd<uint32_t>(node, 0x14);
        return static_cast<int>((rd<uint64_t>(tags, 0x20 + static_cast<uintptr_t>(bit >> 6) * 8) >> (bit & 63)) & 1);
    }
    return 0;
}

// The score manager's member table: a count, then (at +0x50e4) records of 0xa4 bytes with the player id first; at most
// 128 looked at. The index of player `id`, or -1.
extern "C" U3X_FN int u3x_score_member(const uint8_t *scores, uint32_t id) {
    const uint32_t n = rd<uint32_t>(scores, 0);
    for (uint32_t k = 0; k < n && k < 128; ++k)
        if (rd<uint32_t>(scores, 0x50e4 + static_cast<uintptr_t>(k) * 0xa4) == id) return static_cast<int>(k);
    return -1;
}

// Weapon statistics numbers on the PS4 game: the first 19 are its fixed weapon table, then the extra weapon list
// (count, ids), at most 64 of it. Weapon -> number (0 when unknown) and number -> weapon (0 when out of range).
extern "C" U3X_FN uint32_t u3x_weapon_stat_number(const uint32_t *fixed, const uint32_t *extra, uint32_t weapon) {
    for (uint32_t k = 0; k < 0x13; ++k)
        if (fixed[k] == weapon) return k;
    if (!extra) return 0;
    const uint32_t *ids = *reinterpret_cast<const uint32_t *const *>(extra + 2);
    for (uint32_t k = 0; k < extra[0] && k < 0x40; ++k)
        if (ids[k] == weapon) return k + 0x13;
    return 0;
}
extern "C" U3X_FN uint32_t u3x_weapon_for_stat(const uint32_t *fixed, const uint32_t *extra, uint32_t number) {
    if (number < 0x13) return fixed[number];
    if (!extra || number >= extra[0] + 0x13) return 0;
    return (*reinterpret_cast<const uint32_t *const *>(extra + 2))[number - 0x13];
}
// The two weapon groups the kill rewards treat apart: 6 for the six heavy weapons, 4 for the four launchers, else 0.
extern "C" U3X_FN int u3x_weapon_group(uint32_t weapon) {
    switch (weapon) {
    case 0xb934b2b2u: case 0xc58ae515u: case 0x472bca9au: case 0x16980e31u: case 0x22bf34f2u: case 0xb17d9450u: return 6;
    case 0x0b8bd6d6u: case 0x3d2f108bu: case 0x7fe77949u: case 0x9c766ee3u: return 4;
    default: return 0;
    }
}

// ===== v7.132: big batch, part K (same rules as above) =====

// The multiplayer lobby's main menu: one list of every row (text id, icon id, action id) in menu order, each marked
// with the menus it is in: the LAN lobby, the LAN co-op lobby and the online lobby. Fills `out` (8 rows of three
// words) for the menu asked for and returns the row count.
struct LobbyEntry { uint32_t text, icon, action, menus; };
enum : uint32_t { LAN = 1, COOP = 2, ONLINE = 4 };
U3X_RO const LobbyEntry lobby_entries[] = {
    {0xbd1d2c53u, 0xb530fe7bu, 0x649e809au, ONLINE},              // Find Game
    {0x66d63005u, 0x7e2c2757u, 0x7d593924u, LAN | COOP | ONLINE}, // Custom Game
    {0xebd77105u, 0x23e9be55u, 0x52f3d9f6u, LAN},                 // Find a Party
    {0xa2aa79eau, 0xb675afdbu, 0xc4acc827u, LAN | COOP | ONLINE}, // Profile
    {0xcb379354u, 0x12d18838u, 0xb85cb12au, LAN},                 // Cinema
    {0xe62d7aebu, 0x2e9c6d07u, 0x8891e664u, ONLINE},              // Community
    {0xfb08c743u, 0x2c68ff83u, 0xe7d7dbbbu, LAN | COOP | ONLINE}, // Options
    {0xd7d4ccadu, 0x88c52b24u, 0xcd127c29u, LAN | COOP | ONLINE}, // Splitscreen
};
extern "C" U3X_FN uint32_t u3x_lobby_rows(int online, int coop, uint32_t *out) {
    const uint32_t menu = online ? ONLINE : coop ? COOP : LAN;
    uint32_t n = 0;
    for (const LobbyEntry &e : lobby_entries) {
        if (!(e.menus & menu)) continue;
        out[3 * n] = e.text;
        out[3 * n + 1] = e.icon;
        out[3 * n + 2] = e.action;
        ++n;
    }
    return n;
}

// The UI texture table (u16 count at +2, 40-byte entries from the pointer at +8, the icon id at entry +8): the index
// of icon `id`, or -1.
extern "C" U3X_FN int32_t u3x_icon_index(const uint8_t *table, uint32_t id) {
    if (!table) return -1;
    const uint32_t count = rd<uint16_t>(table, 2);
    const uint8_t *entries = *reinterpret_cast<const uint8_t *const *>(table + 8);
    for (uint32_t k = 0; entries && k < count; ++k)
        if (rd<uint32_t>(entries, k * 40 + 8) == id) return static_cast<int32_t>(k);
    return -1;
}

// Co-op Adventure hero looks: the PS4 net game data stores the first descriptor word byte-reversed. A word whose
// first three bytes are 0 and whose last is a character type below 0x40 is turned back. Returns 1 when it was.
extern "C" U3X_FN int u3x_fix_hero_look(uint8_t *look) {
    if (look[0] || look[1] || look[2] || !look[3] || look[3] >= 0x40) return 0;
    look[0] = look[3];
    look[3] = 0;
    return 1;
}

// The rank word other players see: the level in the low 16 bits, the legacy (14 bits) from bit 16, and the badge
// bits the caller gives (star, paw).
extern "C" U3X_FN uint32_t u3x_rank_word(int32_t level, uint32_t legacy, uint32_t badge_bits) {
    return badge_bits | (static_cast<uint32_t>(level) & 0xffffu) | ((legacy & 0x3fffu) << 16);
}

// ===== v7.133: big batch, part L (same rules as above) =====

// The `index`-th name of a list of zero-terminated names (ended by an empty one), or null.
extern "C" U3X_FN const char *u3x_nth_name(const char *list, uint32_t index) {
    for (uint32_t k = 0; *list; ++k) {
        if (k == index) return list;
        while (*list) ++list;
        ++list;
    }
    return nullptr;
}

// The taunt controllers' previous taunts: 8 slots {controller, player id, previous taunt}. The slot for a controller
// and player: theirs when it exists, else the first free one, else slot `fallback` & 7.
struct TauntSlot { uint8_t *controller; uint32_t player, previous; };
extern "C" U3X_FN int u3x_taunt_slot(const TauntSlot *slots, const uint8_t *controller, uint32_t player, uint32_t fallback) {
    for (int k = 0; k < 8; ++k)
        if (slots[k].controller == controller && slots[k].player == player) return k;
    for (int k = 0; k < 8; ++k)
        if (!slots[k].controller) return k;
    return static_cast<int>(fallback & 7);
}
// The previous taunt of a controller and player, 0 when none is kept.
extern "C" U3X_FN uint32_t u3x_taunt_previous(const TauntSlot *slots, const uint8_t *controller, uint32_t player) {
    for (int k = 0; k < 8; ++k)
        if (slots[k].controller == controller && slots[k].player == player) return slots[k].previous;
    return 0;
}

// A menu image item (kind, then words 1..10): kind 2 with the texture in words 2..3, both indices -1, scale 1.0.
extern "C" U3X_FN void u3x_image_item(uint32_t *item, void *texture) {
    for (int k = 1; k < 10; ++k) item[k] = 0;
    item[7] = 0xffffffffu;
    item[9] = 0xffffffffu;
    item[10] = F_ONE;
    item[0] = 2;
    *reinterpret_cast<void **>(item + 2) = texture;
}

// ===== v7.134: big batch, part M (same rules as above) =====

// The invite server's list reply: lines; a line starting with '+' holds a row, base64 after its first space (up to
// '=' or the line end, carriage returns skipped), decoded into 0x30 bytes. A '+' line without a space ends the list.
// Returns the rows read, at most `max`.
extern "C" U3X_FN uint32_t u3x_invite_rows(const char *text, uint8_t (*rows)[0x30], uint32_t max) {
    uint32_t count = 0;
    const char *p = text;
    while (*p && count < max) {
        const char *line = p;
        while (*p && *p != '\n') ++p;
        const char *end = p;
        if (*p) ++p;
        if (line == end || *line != '+') continue;
        const char *q = line + 1;
        while (q < end && *q != ' ') ++q;
        if (q >= end) break;
        ++q;
        uint8_t *out = rows[count];
        for (int k = 0; k < 0x30; ++k) out[k] = 0;
        uint32_t phase = 0, at = 0;
        for (; q < end && *q != '='; ++q) {
            if (*q == '\r') continue;
            const int v = static_cast<int8_t>(u3x_base64_value(*q));   // a bad character counts as -1, as before
            if (at >= 0x30) break;
            switch (phase) {
            case 0: out[at] = static_cast<uint8_t>(v << 2); phase = 1; break;
            case 1: out[at] |= static_cast<uint8_t>(v >> 4); if (++at < 0x30) out[at] = static_cast<uint8_t>(v << 4); phase = 2; break;
            case 2: out[at] |= static_cast<uint8_t>(v >> 2); if (++at < 0x30) out[at] = static_cast<uint8_t>(v << 6); phase = 3; break;
            default: out[at] |= static_cast<uint8_t>(v); ++at; phase = 0; break;
            }
        }
        ++count;
    }
    return count;
}

// A quick-action menu (the friend row's Join / Invite / Stats / Report list): {argument, page, count, then up to 8
// (text, action) pairs}. Adding past 8 does nothing (0). Before it is shown, the two View Profile actions, which have
// nothing to open on this platform, are taken out. Returns how many were taken out.
struct QuickActions { uint32_t argument, page; int32_t count; uint32_t pairs[16]; };
extern "C" U3X_FN int u3x_quick_action_add(QuickActions *q, uint32_t text, uint32_t action) {
    if (q->count > 7) return 0;
    q->pairs[q->count * 2] = text;
    q->pairs[q->count * 2 + 1] = action;
    ++q->count;
    return 1;
}
extern "C" U3X_FN int u3x_quick_action_drop_profiles(QuickActions *q) {
    int32_t kept = 0, dropped = 0;
    for (int32_t k = 0; k < q->count; ++k) {
        const uint32_t action = q->pairs[k * 2 + 1];
        if (action == 0x3fe6f70eu || action == 0xe1810202u) { ++dropped; continue; }
        q->pairs[kept * 2] = q->pairs[k * 2];
        q->pairs[kept * 2 + 1] = action;
        ++kept;
    }
    q->count = kept;
    return dropped;
}

// A prompt's text: the button token (at most 80 characters), then the label (to 86 characters in all).
extern "C" U3X_FN void u3x_prompt_text(char *out, const char *token, const char *label) {
    int n = 0;
    while (*token && n < 80) out[n++] = *token++;
    while (label && *label && n < 86) out[n++] = *label++;
    out[n] = 0;
}

// ===== v7.135: big batch, part N (same rules as above) =====

// The five co-op difficulty names' text ids (Easy to Crushing and the one above), in difficulty order. One table for
// every page that shows the difficulty (lobby card, pause header, "last game" line, Find Game vote).
U3X_RO const uint32_t difficulty_names[5] = {0xc07f185fu, 0x461748d6u, 0xed7fc910u, 0xb0a09a11u, 0x36c8ca98u};
extern "C" U3X_FN const uint32_t *u3x_difficulty_texts() { return difficulty_names; }

// Two texts on two lines ("map name\nsubtitle") into [out, end], terminated.
extern "C" U3X_FN void u3x_two_lines(char *out, char *end, const char *first, const char *second) {
    out = put_text(out, end, first);
    if (out < end) *out++ = '\n';
    out = put_text(out, end, second);
    *out = 0;
}

// ===== v7.136: big batch, part O (same rules as above) =====

// Hat stats: the hat modifier table has rows {item id, count, pairs pointer} of 16 bytes; each pair (8 bytes) is one
// modifier. Every modifier of the rows for `item` goes into the net player's list (count at +0x84c, 16-byte entries
// from +0x850: the pair, the look slot, kind 4), at most 16. `added` counts the entries added. Returns 0, or -1 when
// the list became full.
extern "C" U3X_FN int u3x_add_hat_mods(uint8_t *np, const uint8_t *rows, int32_t row_count, uint32_t item, uint32_t slot,
                                       uint32_t *added) {
    for (int32_t r = 0; r < row_count; ++r) {
        const uint8_t *row = rows + static_cast<uintptr_t>(r) * 16;
        if (rd<uint32_t>(row, 0) != item) continue;
        const int32_t pairs = rd<int32_t>(row, 4);
        const uint8_t *pair = *reinterpret_cast<const uint8_t *const *>(row + 8);
        for (int32_t k = 0; k < pairs && pair; ++k) {
            uint32_t &count = *reinterpret_cast<uint32_t *>(np + 0x84c);
            if (count >= 16) return -1;
            uint8_t *e = np + 0x850 + static_cast<uintptr_t>(count) * 16;
            ++count;
            *reinterpret_cast<uint32_t *>(e) = rd<uint32_t>(pair, k * 8);
            *reinterpret_cast<uint32_t *>(e + 4) = rd<uint32_t>(pair, k * 8 + 4);
            *reinterpret_cast<uint32_t *>(e + 8) = slot;
            e[0xc] = 4;
            e[0xd] = 0;
            ++*added;
        }
    }
    return 0;
}

// The PS3 1.19 weapon numbering of the player state message: the first 20 are the 1.19 fixed weapons, then the extra
// weapon list (i32 count, ids from the pointer at +8). 0 when unknown.
extern "C" U3X_FN uint32_t u3x_weapon_number_119(const uint32_t *fixed20, const uint8_t *extra, uint32_t weapon) {
    for (uint32_t k = 0; k < 20; ++k)
        if (fixed20[k] == weapon) return k;
    if (!extra) return 0;
    const int32_t n = rd<int32_t>(extra, 0);
    const uint32_t *ids = *reinterpret_cast<const uint32_t *const *>(extra + 8);
    for (int32_t k = 0; k < n && ids; ++k)
        if (ids[k] == weapon) return static_cast<uint32_t>(k) + 0x14;
    return 0;
}
// The weapon mode the message sends: 1 stays 1, 5 becomes 3, 0 stays 0, anything else is 4.
extern "C" U3X_FN uint32_t u3x_weapon_mode_119(uint32_t mode) { return mode == 1 ? 1 : mode == 5 ? 3 : mode == 0 ? 0 : 4; }

// A HUD icon quad: four vertices of 16 words (x, y, -, u, v, ..., colour 1, 1, 1, alpha, then 1, 1, 1, 1) around
// (x, y) with half size `half`, then the depth word at 0x40. All values are float bits.
extern "C" U3X_FN void u3x_icon_quad(uint32_t *v, uint32_t x, uint32_t y, uint32_t half, uint32_t alpha, uint32_t depth) {
    for (int k = 0; k < 0x44; ++k) v[k] = 0;
    const uint32_t left = f_sub(x, half), right = f_add(x, half), top = f_sub(y, half), bottom = f_add(y, half);
    for (int corner = 0; corner < 4; ++corner) {
        uint32_t *w = v + corner * 16;
        const bool is_right = corner == 1 || corner == 2, is_bottom = corner >= 2;
        w[0] = is_right ? right : left;
        w[1] = is_bottom ? bottom : top;
        w[3] = is_right ? F_ONE : 0;
        w[4] = is_bottom ? F_ONE : 0;
        w[8] = w[9] = w[10] = F_ONE;
        w[11] = alpha;
        w[12] = w[13] = w[14] = w[15] = F_ONE;
    }
    v[0x40] = depth;
}

// ===== v7.138: big batch, part P (same rules as above) =====

namespace {
[[gnu::always_inline]] inline bool f_less(uint32_t a, uint32_t b) {   // a < b as floats (false when either is NaN)
    int r;
    __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\txorl %0,%0\n\tucomiss %%xmm0,%%xmm1\n\tseta %b0" : "=&r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1", "cc");
    return r != 0;
}
}  // namespace

// The objective ring's colour (float bits, RGBA): red for mode 1, light blue for 2, blue otherwise; the element's alpha.
// While its progress is under a quarter it blinks white three times a second (from the 300 Hz clock `ticks`).
extern "C" U3X_FN void u3x_ring_colour(uint32_t *colour, uint32_t mode, uint32_t alpha, uint32_t progress, int32_t ticks) {
    if (mode == 1) { colour[0] = 0x3f70f0f2u; colour[1] = 0x3dc0c0c2u; colour[2] = 0x3d808081u; }
    else if (mode == 2) { colour[0] = 0x3e008081u; colour[1] = 0x3f008081u; colour[2] = 0x3f70f0f2u; }
    else { colour[0] = 0x3e008081u; colour[1] = 0x3e008081u; colour[2] = 0x3f70f0f2u; }
    colour[3] = alpha;
    if (!f_less(progress, 0x3e800000u)) return;                               // 0.25
    const uint32_t cycles = f_mul(f_mul(f_from_int(ticks), 0x3b5a740eu), 0x40400000u);   // seconds x 3
    const uint32_t part = f_sub(cycles, f_from_int(f_to_int(cycles)));        // its fraction
    if (f_less(part, 0x3f000000u)) colour[0] = colour[1] = colour[2] = F_ONE;  // the first half of each blink
}

// Whether a page is anywhere on the menu's page stack (pages at menu +0x8b0, depth at +0xdb0; at most 160 looked at).
extern "C" U3X_FN int u3x_page_in_stack(const uint8_t *menu, uint32_t page) {
    const int32_t depth = rd<int32_t>(menu, 0xdb0);
    for (int32_t k = 0; k <= depth && k < 160; ++k) {
        const uint8_t *p = *reinterpret_cast<const uint8_t *const *>(menu + 0x8b0 + static_cast<intptr_t>(k) * 8);
        if (p && rd<uint32_t>(p, 0x10) == page) return 1;
    }
    return 0;
}

// The matchmaking states the start watch cares about: the match start (0x2c..0x30), and, while it follows a
// background join, the join states 0xb, 0xf, 0x11 and 0x28.
extern "C" U3X_FN int u3x_mm_starting(uint32_t state) { return state >= 0x2c && state <= 0x30; }
extern "C" U3X_FN int u3x_mm_joining(uint32_t state, int background) {
    return background && (state == 0xb || state == 0xf || state == 0x11 || state == 0x28);
}

// ===== v7.139: big batch, part Q (same rules as above) =====

// Connection entries (32 of 0x220 bytes from `entries`, in use at +4, address +0x2c, port +0x30): the one a packet
// from (ip, port) belongs to, or null.
extern "C" U3X_FN uint8_t *u3x_entry_by_address(uint8_t *entries, uint32_t ip, uint32_t port) {
    for (uint8_t *e = entries, *end = entries + 32 * 0x220; e != end; e += 0x220)
        if (e[4] && rd<uint32_t>(e, 0x2c) == ip && rd<uint16_t>(e, 0x30) == static_cast<uint16_t>(port)) return e;
    return nullptr;
}

// A replay clock set to time `t`: running (byte 0 clear), the game's base time, both times `t`, rate 1.0, the
// 1/60 s step, everything else zero.
extern "C" U3X_FN void u3x_clock_reset(uint8_t *clock, uint32_t game_time, int32_t t) {
    auto w32 = [clock](uint32_t off, uint32_t v) { *reinterpret_cast<uint32_t *>(clock + off) = v; };
    clock[0] = 0;
    w32(4, F_ONE);
    w32(8, game_time);
    w32(0xc, static_cast<uint32_t>(t));
    w32(0x10, 0);
    w32(0x14, 0x3c888889u);
    w32(0x18, 0);
    *reinterpret_cast<uint16_t *>(clock + 0x1c) = 0;
    w32(0x20, 0);
    w32(0x24, 0);
    w32(0x28, static_cast<uint32_t>(t));
    w32(0x2c, 0);
    *reinterpret_cast<uint64_t *>(clock + 0x30) = 0;
    w32(0x38, 0);
}

// The game's allocator context stack (entries of five words from the context, count at +0x730, at most 0x2e): push
// {allocator, name, 0, name, 1} (1 = pushed, 0 = full) and pop the top one (its words cleared).
extern "C" U3X_FN int u3x_context_push(uint8_t *context, uintptr_t allocator, uintptr_t name) {
    uint32_t &n = *reinterpret_cast<uint32_t *>(context + 0x730);
    if (n >= 0x2e) return 0;
    uintptr_t *e = reinterpret_cast<uintptr_t *>(context + static_cast<uintptr_t>(n) * 40);
    e[0] = allocator; e[1] = name; e[2] = 0; e[3] = name; e[4] = 1;
    ++n;
    return 1;
}
extern "C" U3X_FN void u3x_context_pop(uint8_t *context) {
    uint32_t &n = *reinterpret_cast<uint32_t *>(context + 0x730);
    --n;
    uintptr_t *e = reinterpret_cast<uintptr_t *>(context + static_cast<uintptr_t>(n) * 40);
    e[0] = e[1] = e[2] = e[3] = e[4] = 0;
}

// The spectator's fixed cameras: locator `index` of `count` (the caller has already counted a negative index from the
// end once); -1 when it is still negative.
extern "C" U3X_FN int32_t u3x_camera_slot(int32_t index, int32_t count) {
    const int32_t slot = index % count;
    return slot < 0 ? -1 : slot;
}

// ===== v7.140: the call sequences, part 1 =====
// These functions only drive the game: they call its functions in the order it needs and set its fields. They use the
// C part's own helpers through the u3h_* exports below the ProfileState / ExtState layouts they read.

// The C part's helpers and state, exported for the C++ part (u3_lan_profiles.c, "u3h exports"); hidden, so the calls
// are direct (the helper link has no PLT).
#define U3H_IMPORT [[gnu::visibility("hidden")]]
extern "C" {
U3H_IMPORT int u3h_show(uintptr_t base, void *state, uint32_t page, int silent);
U3H_IMPORT void u3h_show_dialog(uintptr_t base, void *state, uint32_t body, uint32_t title, uint32_t action, int32_t buttons,
                     uint32_t extra, uint32_t close);
U3H_IMPORT void u3h_lan_chosen(void);
U3H_IMPORT int u3h_offline_find(uintptr_t base, void *state);
U3H_IMPORT void u3h_onm_state(uintptr_t base, uint32_t net_state);
U3H_IMPORT void u3h_fj_note(const char *before, const uint8_t *id, const char *after, int32_t value);
U3H_IMPORT uint8_t *u3h_ext(void);   // the C part's ExtState
U3H_IMPORT int u3h_party_send(uintptr_t base, uint32_t message, const void *data, uint32_t length);   // 0 when there is no sender
U3H_IMPORT uint8_t *u3h_nmh(uintptr_t base);
U3H_IMPORT int u3h_lan_backend(void);
U3H_IMPORT void *u3h_online_backend(uintptr_t base);
U3H_IMPORT void u3h_jip_started(void);
}

namespace {
// ProfileState (checked layout in the C part): show_page +648, lobby_error +728, the switch word +0x20c0, natives +768
uintptr_t state_show_page(void *s) { return *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 648); }
uint32_t &state_lobby_error(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 728); }
uint32_t state_switches(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x20c0); }
uintptr_t state_native(void *s, int k) { return *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 768 + k * 8); }
// ExtState counters: coop +0x100, eb +0x4050
uint32_t *ext_coop() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x100); }
uint32_t *ext_eb() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4050); }
template <typename R, typename... A> R game_call(uintptr_t address, A... args) { return reinterpret_cast<R (*)(A...)>(address)(args...); }
template <typename T> T &at(uintptr_t address) { return *reinterpret_cast<T *>(address); }
}  // namespace

// The LAN / split-screen choice (context 0 LAN, else split-screen co-op): no change while the menu shell is in a
// transition (state 7 or 8); with the second-user switch on, split-screen shows the "no second user" dialog and stops;
// else LAN mode, the co-op byte set from the context, and the LAN profile page.
extern "C" U3X_FN void u3x_lan_choice(uintptr_t base, const int32_t *ev, void *s) {
    const uint32_t shell = at<uint32_t>(base + 0x17bd080 + 8);
    if (shell == 7 || shell == 8) return;
    const uint64_t context = *reinterpret_cast<const uint64_t *>(reinterpret_cast<const uint8_t *>(ev) + 8);
    if (context && (state_switches(s) & 0x10)) {
        u3h_show_dialog(base, s, 0x7c31f4d0u, 0xc6467f2du, 0, 0, 0, 0);
        return;
    }
    u3h_lan_chosen();
    uint32_t *coop = ext_coop();
    ++coop[0];
    coop[1] = static_cast<uint32_t>(context);
    at<uint8_t>(base + 0x17d6065) = context != 0;
    coop[2] = context != 0;
    if (state_show_page(s)) u3h_show(base, s, 0x4608c1adu, 0);
}

// The lobby's Find Game and Community rows: Find Game (offline it shows the failure dialog instead) sets the
// matchmaking net mode and user and opens the playlists; Community opens its page.
extern "C" U3X_FN void u3x_lobby_action(uintptr_t base, void *s, uint32_t action) {
    ext_eb()[168] = action;
    if (!state_show_page(s)) return;
    if (action == 0x649e809au && u3h_offline_find(base, s)) return;
    if (action == 0x649e809au) {
        at<uint32_t>(base + 0x17d603c) = 1;
        at<uint32_t>(base + 0x17bf714) = 0;
        u3h_show(base, s, 0x0b7714b5u, 0);
    } else if (action == 0x8891e664u) {
        u3h_show(base, s, 0xdb53e8ffu, 0);
    }
}

// Start Match (0xa0448333) in an online party: the leader tells the party (0x118 with the match room, if any), goes to
// net state 20 at the current time and opens matchmaking; a member marks itself ready and goes back a page. Either way
// the match session leaves its room. Returns 0 for other actions.
extern "C" U3X_FN int u3x_start_match_catch(uintptr_t base, void *s, uint32_t id) {
    if (id != 0xa0448333u) return 0;
    uint8_t *party = reinterpret_cast<uint8_t *>(base + 0x17dc298u), *match = reinterpret_cast<uint8_t *>(base + 0x17d6420u);
    ++ext_eb()[169];
    const bool leader = !rd<uint64_t>(party, 0x18) || rd<uint32_t>(party, 0x12e4) == rd<uint32_t>(party, 0x12e8);
    if (leader) {
        uint64_t room = match[0xc8] ? rd<uint64_t>(match, 0x18) : 0;
        u3h_party_send(base, 0x118, &room, 8);
        at<uint32_t>(base + 0x1b68810u) = 20;
        at<int32_t>(base + 0x1b68814u) = at<int32_t>(base + 0x17b4ba4u);
        if (state_show_page(s)) u3h_show(base, s, 0xe35f0f1au, 0);
        game_call<void, uint32_t, uint32_t>(base + 0x751aa0u, 0xe35f0f1au, 0xea2119f2u);
    } else {
        party[0x1334] = 1;
        game_call<void, void *, int>(base + 0x652200u, reinterpret_cast<void *>(base + 0x17bd080u), 0);
    }
    uint8_t *backend = *reinterpret_cast<uint8_t **>(match + 8);
    if (backend && *reinterpret_cast<uintptr_t **>(backend))
        reinterpret_cast<int64_t (*)(void *, void *)>((*reinterpret_cast<uintptr_t **>(backend))[7])(backend, match);
    return 1;
}

// Find Game online: clear the search status, leave the current room, reset the match manager, take the playlist's
// settings into the match session and ask the session server to search (309). -1 when there is no session manager or
// the search could not be sent; then net state 6.
extern "C" U3X_FN int u3x_online_search(uintptr_t base) {
    uint8_t *net = reinterpret_cast<uint8_t *>(base + 0x17d5c58u), *match = reinterpret_cast<uint8_t *>(base + 0x17d6420u);
    void *manager = at<void *>(base + 0x17dd768u);
    net[0x18] = 0;
    *reinterpret_cast<int32_t *>(net + 0x40) = 0;
    uint8_t *backend = *reinterpret_cast<uint8_t **>(match + 8);
    if (backend && *reinterpret_cast<uintptr_t **>(backend))
        reinterpret_cast<int64_t (*)(void *, void *)>((*reinterpret_cast<uintptr_t **>(backend))[7])(backend, match);
    game_call<void, void *>(base + 0x720500u, reinterpret_cast<void *>(base + 0x17cb060u));
    *reinterpret_cast<uint32_t *>(match + 0xf8) = rd<uint32_t>(net, 0x10);
    *reinterpret_cast<uint32_t *>(match + 0xfc) = rd<uint64_t>(net, 0x10) != 0;
    if (!manager || !*reinterpret_cast<uintptr_t **>(manager)) return -1;
    using Search = int64_t (*)(void *, void *, uint32_t, uint32_t, int32_t, int32_t, int32_t, uint32_t);
    const int64_t r = reinterpret_cast<Search>((*reinterpret_cast<uintptr_t **>(manager))[5])(manager, match, 0, 0, -1, 0, 0, 0);
    if (r < 0) return -1;
    u3h_onm_state(base, 6);
    return 0;
}

// Joining a match in progress: take the host's selected net game, run the game's late-join set-up, mark the match
// session as not starting, net step 4, enter the match manager's late-join mode, give the start 30 seconds, open the
// late-join page and go to net state 0x31.
U3X_RO const char jip_started_text[] = "join in progress: late join started (0x31), status ";
extern "C" U3X_FN void u3x_jip_enter(uintptr_t base) {
    uint8_t *match = reinterpret_cast<uint8_t *>(base + 0x17d6420u), *net = reinterpret_cast<uint8_t *>(base + 0x17d5c58u);
    at<int16_t>(base + 0x17cb060u + 0x661c) = at<int16_t>(base + 0x17d167cu);
    game_call<void>(base + 0x74ba60u);
    match[0x1338] = 0;
    *reinterpret_cast<uint32_t *>(net + 0x408) = 4;
    game_call<void, void *, int, int, int, int, int>(base + 0x72b570u, reinterpret_cast<void *>(base + 0x17cb060u), 1, 1, 0, 0, 0);
    const int32_t now = at<int32_t>(base + 0x17b4a90u + 0x114);
    *reinterpret_cast<int32_t *>(net + 0x2c) = now + 9000;
    game_call<int, void *, uint32_t, int, int>(base + 0x651940u, reinterpret_cast<void *>(base + 0x17bd080u), 0x69e10dadu, 0, 0);
    at<uint32_t>(base + 0x1b68810u) = 0x31;
    at<uint32_t>(base + 0x1b68814u) = static_cast<uint32_t>(now);
    u3h_jip_started();
    u3h_fj_note(jip_started_text, nullptr, nullptr, static_cast<int32_t>(at<uint32_t>(base + 0x1b69530u)));
}

// Custom Game: needs the three NetMenuHost natives (error 1) and a player profile (2); sets the custom-game net mode,
// refreshes the game types (3 when there are none), selects net game 6 (0 in co-op) and the first type, gives an online
// match session its backend back, and opens the lobby (4 when that fails).
extern "C" U3X_FN void u3x_custom_game(uintptr_t base, void *s) {
    const uintptr_t refresh = state_native(s, 1), select = state_native(s, 2), select_type = state_native(s, 3);
    uint32_t &error = state_lobby_error(s);
    if (!refresh || !select || !select_type) { error = 1; return; }
    if (!at<uint64_t>(base + 0x17dd610)) { error = 2; return; }
    at<uint32_t>(base + 0x17d603c) = 2;
    uint8_t *nmh = u3h_nmh(base);
    reinterpret_cast<void (*)(void *)>(refresh)(nmh);
    if (!*nmh) { error = 3; return; }
    const int coop = at<uint8_t>(base + 0x17d6065) != 0;
    reinterpret_cast<void (*)(void *, int, void *, uint32_t)>(select)(reinterpret_cast<void *>(base + 0x17cb060), coop ? 0 : 6, nullptr, 0xc);
    reinterpret_cast<void (*)(void *, int)>(select_type)(nmh, 0);
    uint8_t *match = reinterpret_cast<uint8_t *>(base + 0x17d6420u);
    void *backend = u3h_online_backend(base);
    if (!u3h_lan_backend() && backend && !rd<uint64_t>(match, 8))
        reinterpret_cast<int64_t (*)(void *, void *)>((*reinterpret_cast<uintptr_t **>(backend))[3])(backend, match);
    error = 0;
    if (!u3h_show(base, s, 0xb99a3235, 0)) error = 4;
}

// ===== v7.141: the call sequences, part 2 (party join and invites) =====

extern "C" {
U3H_IMPORT void u3h_fi_send(uintptr_t base);
U3H_IMPORT int32_t u3h_fi_selected(void);
U3H_IMPORT void u3h_fi_select(int32_t row);
U3H_IMPORT void u3h_fi_delete(uintptr_t base, int32_t row);
U3H_IMPORT int32_t u3h_mq_send(uintptr_t base, const uint8_t *id, uint32_t type, const void *data, uint32_t length);
U3H_IMPORT void u3h_join_reset(void);             // the previous join attempt is forgotten
U3H_IMPORT void u3h_join_waiting(uintptr_t base);  // the join watchdog starts waiting
U3H_IMPORT void u3h_wait_popup(uintptr_t base, uint32_t text, uintptr_t callback);
U3H_IMPORT int32_t u3h_presence(uintptr_t base, const uint8_t *id, uint8_t *presence);
U3H_IMPORT int u3h_key_ok(uintptr_t base, uint32_t key);
}

namespace {
uint32_t *ext_ea() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x3c50); }
uint8_t *party_session(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17dc298u); }
uint8_t *invites(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17d5c58u + 0x430); }
uint8_t *friends(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17caeb0u); }
const uint8_t *selected_player(uintptr_t base) { return reinterpret_cast<const uint8_t *>(base + 0x17c3871u); }
}  // namespace

// The party-create popup's end (event 6): once the new party has its id, send the pending invites and close the popup.
extern "C" U3X_FN void u3x_party_created(uintptr_t base, const int32_t *ev) {
    uint8_t *ps = party_session(base);
    if (ev[0] != 6 || !ps[0xc8] || !rd<uint64_t>(ps, 0x18)) return;
    u3h_fi_send(base);
    game_call<int, void *, int>(base + 0x652200u, reinterpret_cast<void *>(base + 0x17bd080u), 0);
}

// An invite row clicked: during a match (net step 1) the "not now" dialog; otherwise the row becomes the selected
// player (its 0x14-byte id) and its Accept / Decline menu opens.
extern "C" U3X_FN void u3x_invite_click(uintptr_t base, void *s, const int32_t *ev) {
    uint8_t *list = invites(base);
    if (!rd<uint32_t>(list, 0)) return;
    if (at<uint32_t>(base + 0x17d5c58u + 0x3e4) == 1) {
        u3h_show_dialog(base, s, 0xed3b546fu, 0xc6467f2du, 0xa8cda6e0u, 0, 0, 0xe1e040aau);
        return;
    }
    const int32_t row = ev[1];
    u3h_fi_select(row);
    uint8_t *to = reinterpret_cast<uint8_t *>(base + 0x17c3871u);
    const uint8_t *from = list + 8 + static_cast<intptr_t>(row) * 0x28 + 0x10;
    for (int k = 0; k < 0x14; ++k) to[k] = from[k];
    at<uint64_t>(base + 0x17c3898u) = 0;
    QuickActions q;
    q.argument = 0;
    q.page = 0xecc3219du;
    q.count = 0;
    u3x_quick_action_add(&q, 0xb1600ce3u, 0xd79bd5edu);
    u3x_quick_action_add(&q, 0xc6b89b8cu, 0x39764828u);
    game_call<void, void *, void *, int>(base + 0x6528d0u, reinterpret_cast<void *>(base + 0x17bd080u), &q, 1);
    ++ext_ea()[111];
}

// Asking to join a party: message 0x10b {party id, 0} (with the PC marker in the padding) to `id`, the log line, the
// join watchdog and the "Joining Party..." popup.
U3X_RO const char join_asked[] = "join: asked ";
U3X_RO const char join_known[] = " (0x10b), party id known";
U3X_RO const char join_unknown[] = " (0x10b), no party id";
extern "C" U3X_FN void u3x_send_join(uintptr_t base, const uint8_t *id, uint64_t party, uintptr_t callback) {
    uint8_t m[0x10];
    for (auto &b : m) b = 0;
    *reinterpret_cast<uint64_t *>(m) = party;
    m[9] = 0x4a;
    m[10] = 1;
    u3h_join_reset();
    u3h_mq_send(base, id, 0x10b, m, 0x10);
    ++ext_ea()[107];
    u3h_fj_note(join_asked, id, party ? join_known : join_unknown, -1);
    u3h_join_waiting(base);
    u3h_wait_popup(base, 0xe4e03363u, callback);
}

// Join Party on the selected player: their presence gives the party id; a locked party gets "Unable to join party";
// the party this player is already in is not asked again.
U3X_RO const char join_locked_a[] = "join: ";
U3X_RO const char join_locked_b[] = " party locked (presence): not asked";
extern "C" U3X_FN void u3x_join_party(uintptr_t base, void *s, uintptr_t callback) {
    uint8_t presence[0x190];
    for (auto &b : presence) b = 0;
    uint64_t party = 0;
    if (u3h_presence(base, selected_player(base), presence) == 0) {
        const uint8_t *d = presence + 0x100;
        if (d[7]) {
            u3h_show_dialog(base, s, 0x4d972420u, 0xc6467f2du, 0x52a9f855u, 0, 0, 0);
            u3h_fj_note(join_locked_a, selected_player(base), join_locked_b, -1);
            return;
        }
        party = rd<uint64_t>(d, 0x28);
        uint8_t *ps = party_session(base);
        if (party && ps[0xc8] && rd<uint64_t>(ps, 0x18) == party) return;
    }
    u3x_send_join(base, selected_player(base), party, callback);
}

// Accepting the selected invite: outside the menus' net step 0 the lobby page first; a row with our key finds the
// inviter among the friends (by the name in the row) and asks to join their party; the row is marked and deleted.
// While the friend list is being rebuilt the invite stays (accept again).
extern "C" U3X_FN void u3x_invite_accept(uintptr_t base, void *s, uintptr_t callback) {
    uint8_t *list = invites(base);
    const int32_t row_index = u3h_fi_selected();
    if (!rd<uint32_t>(list, 0) || row_index < 0) return;
    if (at<uint32_t>(base + 0x17d5c58u + 0x3e4)) u3h_show(base, s, 0xb99a3235u, 0);
    const uint8_t *row = list + 8 + static_cast<intptr_t>(row_index) * 0x28;
    if (u3h_key_ok(base, rd<uint32_t>(row, 0))) {
        char name[0x15];
        for (int k = 0; k < 0x14; ++k) name[k] = static_cast<char>(row[0x10 + k]);
        name[0x14] = 0;
        uint8_t *table = friends(base);
        if (name[0] && rd<uint64_t>(table, 0xc8)) return;
        uint8_t *inviter = nullptr;
        if (name[0])
            for (uint32_t k = 0; k < rd<uint32_t>(table, 0); ++k) {
                uint8_t *e = *reinterpret_cast<uint8_t **>(table + 8) + static_cast<uintptr_t>(k) * 0x240;
                if (e[1] && game_call<int, const void *, const void *>(base + 0x139c710u, e + 1, name) == 0) { inviter = e; break; }
            }
        if (inviter) u3x_send_join(base, inviter + 1, rd<uint64_t>(row, 8), callback);
    }
    list[0x198] = 1;
    u3h_fi_delete(base, row_index);
    ++ext_ea()[112];
}

// ===== v7.142: the call sequences, part 3 (menu actions and the clan tag) =====

extern "C" {
U3H_IMPORT int u3h_rpm_catch(uintptr_t base, void *s, uint32_t id);
U3H_IMPORT int u3h_fr_catch(uintptr_t base, void *s, uint32_t id, uintptr_t report_callback);
U3H_IMPORT int u3h_start_match_catch(uintptr_t base, void *s, uint32_t id);
U3H_IMPORT int u3h_film_action(uintptr_t base, const int32_t *ev);
U3H_IMPORT int u3h_community_catch(uintptr_t base, void *s, uint32_t id);
U3H_IMPORT void u3h_treasure_first_row(void *s);
U3H_IMPORT void u3h_motd_ok(uintptr_t base, void *s);
U3H_IMPORT void u3h_mission_take(uintptr_t base, void *s);
U3H_IMPORT void u3h_party_kick_promote(uintptr_t base, int promote);
U3H_IMPORT void u3h_report_popup(uintptr_t base, void *s);
U3H_IMPORT void u3h_report_pick(uintptr_t base, void *s, uint32_t row);
U3H_IMPORT void u3h_view_stats(uintptr_t base, void *s);
U3H_IMPORT void u3h_lan_delete_profile(uintptr_t base, void *s, uint32_t action);
U3H_IMPORT const char *u3h_text(uintptr_t base, void *s, uint32_t id);
U3H_IMPORT int u3h_publish(uintptr_t base, void *s, uint8_t *profile);
}

// Menu actions the game routes to no handler of its own (its catch-all is empty on PS4): the helper's handlers first
// (RPCN options, friends, Start Match, film player, Community), then each action's page or effect. The two counters
// at state +0x2428 count the delete-key action (low) and all others (from bit 16).
extern "C" U3X_FN void u3x_menu_action(uintptr_t base, const int32_t *ev, void *s) {
    const uint32_t id = static_cast<uint32_t>(ev[0]);
    if (u3h_rpm_catch(base, s, id)) return;
    *reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(s) + 0x2400 + 5 * 8) += id == 0x3406420cu ? 1 : 0x10000;
    const uint32_t report = ext_ea()[96];
    if (report && u3h_fr_catch(base, s, id, base + report)) return;
    if (u3h_start_match_catch(base, s, id)) return;
    if (u3h_film_action(base, ev)) return;
    if (u3h_community_catch(base, s, id)) return;
    switch (id) {
    case 0x9db19163u: u3h_show(base, s, 0xc98ee52a, 0); break;                              // Medals
    case 0xe9bce205u: u3h_treasure_first_row(s); u3h_show(base, s, 0xda577fac, 0); break;    // Treasures
    case 0x420f5e96u: u3h_show(base, s, 0x6281f21f, 0); break;                              // Records
    case 0x58dda3cdu: u3h_show(base, s, 0x0c3a3716, 0); break;                              // Unlocks
    case 0xab58f9dau: u3h_motd_ok(base, s); break;                                          // Message of the Day OK
    case 0xe4794ab9u: u3h_show(base, s, 0xdfdb36fa, 0); break;                              // Missions
    case 0xea1f40c5u: u3h_mission_take(base, s); break;                                     // take the mission
    case 0xccbf832eu: at<uint8_t>(base + 0x17c3570) = 1; break;                             // Lock Party
    case 0x0d0c91cdu: at<uint8_t>(base + 0x17c3570) = 0; break;                             // Unlock Party
    case 0x7cc46b94u: u3h_party_kick_promote(base, 0); break;                               // Kick
    case 0x2d32c8b0u: u3h_party_kick_promote(base, 1); break;                               // Promote to leader
    case 0x0ecbd5bdu: u3h_report_popup(base, s); break;                                     // Report Player
    case 0x089143eau: u3h_view_stats(base, s); break;                                       // View Stats
    case 0x6d6ad418u: case 0xda18d7d3u: u3h_lan_delete_profile(base, s, id); break;         // Delete Profile
    default:
        if (id >= 0x7ebae89cu && id <= 0x7ebae8a3u) u3h_report_pick(base, s, id - 0x7ebae89cu);   // a report reason
        break;
    }
}

// The clan tag (4 characters in the player's profile at +0xac0) is typed in the system keyboard. Opening: only when
// the keyboard is free and set up; the title is the game's clan tag text, the field starts with the current tag; a
// keyboard that does not open sets error 6. The ProfileState keyboard functions: init +0, status +8, result +16,
// term +24; phase +0x4c, error +0x50, text +0x58 (16 UTF-16 units); the title buffer at +0x2d80.
namespace {
struct ImeRequest {   // the system keyboard's parameter block (96 bytes)
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    uint32_t x, y, halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
};
uint8_t *player_profile(uintptr_t base) {
    uint8_t *profiles = at<uint8_t *>(base + 0x17dd610);
    if (!profiles) return nullptr;
    const uint32_t user = at<uint8_t>(base + 0x17bde75) ? 1u : at<uint32_t>(base + 0x17bf714);
    return user < 4 ? profiles + static_cast<uintptr_t>(user) * 0x7b00 : nullptr;
}
uintptr_t state_fn(void *s, int k) { return *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + k * 8); }
uint32_t &state_phase(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x4c); }
uint32_t &state_error(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x50); }
uint16_t *state_text(void *s) { return reinterpret_cast<uint16_t *>(static_cast<uint8_t *>(s) + 0x58); }
}  // namespace
extern "C" U3X_FN void u3x_clan_tag_open(uintptr_t base, void *s) {
    if (state_phase(s) != 0 || !state_fn(s, 0)) return;
    uint8_t *profile = player_profile(base);
    if (!profile) return;
    const char *label = u3h_text(base, s, 0x639e0fa8u);
    uint16_t *title = reinterpret_cast<uint16_t *>(static_cast<uint8_t *>(s) + 0x2d80);
    unsigned k = 0;
    for (; label[k] && k < 31; ++k) title[k] = static_cast<uint8_t>(label[k]);
    title[k] = 0;
    uint16_t *text = state_text(s);
    for (int j = 0; j < 16; ++j) text[j] = 0;
    for (unsigned j = 0; j < 4 && profile[0xac0 + j]; ++j) text[j] = profile[0xac0 + j];
    ImeRequest request;
    for (unsigned j = 0; j < sizeof request; ++j) reinterpret_cast<uint8_t *>(&request)[j] = 0;
    request.user = 1;
    request.max_length = 4;
    request.buffer = text;
    request.title = title;
    if (reinterpret_cast<int (*)(const ImeRequest *, const void *)>(state_fn(s, 0))(&request, nullptr) < 0) {
        state_error(s) = 6;
        return;
    }
    state_phase(s) = 3;
}
// Polling the keyboard: when it is done and the player confirmed, the typed tag becomes the profile's (printable
// characters only; '\' shows as a space; the rest after the end cleared) and the profile is saved.
extern "C" U3X_FN void u3x_clan_tag_poll(uintptr_t base, void *s) {
    if (state_phase(s) != 3) return;
    if (reinterpret_cast<int (*)()>(state_fn(s, 1))() != 2) return;
    uint32_t result = 1;
    const int r = reinterpret_cast<int (*)(uint32_t *)>(state_fn(s, 2))(&result);
    reinterpret_cast<int (*)()>(state_fn(s, 3))();
    state_phase(s) = 0;
    if (r < 0 || result) return;
    uint8_t *profile = player_profile(base);
    if (!profile) return;
    const uint16_t *text = state_text(s);
    for (unsigned j = 0; j < 4; ++j) {
        uint16_t c = text[j];
        if (c == '\\') c = ' ';
        profile[0xac0 + j] = (c >= 0x20 && c < 0x7f) ? static_cast<uint8_t>(c) : 0;
        if (!c) {
            for (; j < 4; ++j) profile[0xac0 + j] = 0;
            break;
        }
    }
    u3h_publish(base, s, profile);
}

// ===== v7.143: the call sequences, part 4 (profile save, two script natives, the matchmaking players list) =====

extern "C" {
U3H_IMPORT int u3h_mp_write(uintptr_t base, void *s, uint32_t slot, const uint8_t *record);
U3H_IMPORT void u3h_rank_fields(uint8_t *profile, uint32_t slot, int after);
U3H_IMPORT void u3h_server_queue(const uint8_t *record);
U3H_IMPORT int u3h_online_profile(void);
U3H_IMPORT uint32_t *u3h_m03(void);
U3H_IMPORT volatile uint32_t *u3h_m03_diag(void);
U3H_IMPORT void u3h_hud_alias_names(uintptr_t base, void *s);
U3H_IMPORT void u3h_stage_tick(uintptr_t base, void *s);
U3H_IMPORT void u3h_loadout_tick(uintptr_t base, void *s);
U3H_IMPORT void u3h_rank_icon(uintptr_t base, void *s, int32_t *ev);
U3H_IMPORT uint32_t u3h_team_colour(uintptr_t base, int32_t team);
}

// Saving the player's multiplayer profile (the LAN branch of the game's publish, done at once). Refused while the save
// is suppressed, for a slave profile, outside multiplayer, or when the profile on screen is not the one whose file
// was loaded. The live record is copied to the save copy (the rank fields held as the file has them while it is
// written), sealed into the slot file and, on the online path, queued for the server. The probe at state +0x2f00
// counts saves; the publish state is 0 saved, 2 failed.
namespace {
constexpr uint32_t record_bytes = 0x2828, live = 0x248, save_copy = 0x5298;
constexpr uint32_t publish_state = 0x7ae4, load_state = 0x7ae8, slave = 0x7afc, suppress = 0x7afd, save_count = live + 0x674;
enum Probe { saves = 4, save_ok, save_fail, refused, last_slot, last_count = 12, bound_slot = 15 };
uint32_t *probe(void *s) { return reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x2f00); }
uint32_t profile_count(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 128); }
uint32_t profile_selected(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 132); }
uint32_t &word(uint8_t *p, uint32_t off) { return *reinterpret_cast<uint32_t *>(p + off); }
}  // namespace
extern "C" U3X_FN int u3x_publish(uintptr_t base, void *s, uint8_t *profile) {
    if (!profile || profile[suppress] || profile[slave] || !at<uint8_t>(base + 0x17d5c61)) return 0;
    uint32_t *p = probe(s);
    const uint32_t slot = p[bound_slot];
    if (word(profile, load_state) != 3 || !slot || slot != profile_selected(s) || slot > profile_count(s)) {
        word(profile, publish_state) = 2;
        ++p[refused];
        return 0;
    }
    ++word(profile, save_count);
    for (uint32_t k = 0; k < record_bytes; ++k) profile[save_copy + k] = profile[live + k];
    u3h_rank_fields(profile, slot, 0);
    const int ok = u3h_mp_write(base, s, slot, profile + save_copy);
    if (ok && u3h_online_profile()) u3h_server_queue(profile + save_copy);
    u3h_rank_fields(profile, slot, 1);
    word(profile, publish_state) = ok ? 0 : 2;
    ++p[saves];
    ++p[ok ? save_ok : save_fail];
    p[last_slot] = slot;
    p[last_count] = word(profile, save_count);
    return ok;
}

// Two script natives PS4 no longer registers (the old-quarter and grave scripts call them). The image base comes from
// ExtState +0x21b0; ExtState +0x2218 counts calls: [1]/[2] underwater asks and the last answer, [3]..[5] zipline
// calls, the last player index and the queued changes.
namespace {
uintptr_t native_base() { return *reinterpret_cast<uintptr_t *>(u3h_ext() + 0x21b0); }
uint32_t *native_counts() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2218); }
int32_t first_arg(int32_t argc, const uint64_t *argv) { return argc > 0 && argv ? static_cast<int32_t>(argv[0]) : 0; }
// The game's queued-setting call: event, target byte, value, count 1, compare and set functions, a zero stack argument
// and a blend factor in xmm0 (the helper has no float code, so the factor's bits go in through the asm).
U3X_FN uint8_t *queue_setting(uintptr_t fn, uint32_t event, uint8_t *target, uint8_t *value, uintptr_t compare,
                              uintptr_t set, uint32_t factor_bits) {
    register uint64_t a4 asm("r8") = compare;
    register uint64_t a5 asm("r9") = set;
    register uint64_t bits asm("r10") = factor_bits;
    register uint64_t callee asm("r11") = fn;
    uint64_t a0 = event, a1 = reinterpret_cast<uint64_t>(target), a2 = reinterpret_cast<uint64_t>(value), a3 = 1;
    uint8_t *result;
    asm volatile(
        "movq %%rsp, %%r12\n\t"
        "andq $-16, %%rsp\n\t"
        "subq $16, %%rsp\n\t"
        "movq $0, (%%rsp)\n\t"
        "movd %%r10d, %%xmm0\n\t"
        "call *%%r11\n\t"
        "movq %%r12, %%rsp"
        : "=a"(result), "+D"(a0), "+S"(a1), "+d"(a2), "+c"(a3), "+r"(a4), "+r"(a5), "+r"(bits), "+r"(callee)
        :
        : "memory", "r12", "cc");
    return result;
}
}  // namespace
// is-camera-underwater? (screen): the camera table's underwater flag, kept by PS4 at base+0x1221430.
extern "C" U3X_FN int64_t u3x_native_underwater(int32_t argc, uint64_t *argv) {
    uint32_t *n = native_counts();
    const uintptr_t base = native_base();
    ++n[1];
    if (!base) return 0;
    const uint8_t r = game_call<uint8_t>(base + 0x1221430, first_arg(argc, argv));
    n[2] = r;
    return r ? 1 : 0;
}
// player-disable-zipline (player): the player's zipline setting (byte +0xf of its 0x20-byte slot at 0x17fad00) gets
// its pending change cancelled and the value 1 queued at priority 101, due 9 ticks from now.
extern "C" U3X_FN int64_t u3x_native_disable_zipline(int32_t argc, uint64_t *argv) {
    uint32_t *n = native_counts();
    const uintptr_t base = native_base();
    ++n[3];
    if (!base) return 0;
    const int32_t player = first_arg(argc, argv);
    n[4] = static_cast<uint32_t>(player);
    uint8_t *setting = reinterpret_cast<uint8_t *>(base + 0x17fad00) + static_cast<int64_t>(player) * 0x20 + 0xf;
    const uint32_t factor = at<uint32_t>(base + 0x14481f0);
    uint8_t off = 0, on = 1;
    uint8_t *e = queue_setting(base + 0x12284e0, 0x309861e2u, setting, &off, base + 0x12284c0, base + 0x1228ca0, factor);
    if (e) {
        *reinterpret_cast<uint32_t *>(e + 0x28) = 0;
        e[0x49] |= 8;
    }
    e = queue_setting(base + 0x12284e0, 0x4d95edb1u, setting, &on, base + 0x12284c0, base + 0x12284d0, factor);
    if (e) {
        e[0x49] = static_cast<uint8_t>((e[0x49] & 0xdb) | 4);
        *reinterpret_cast<uint32_t *>(e + 0x28) = 101;
        const uint8_t *clock = at<uint8_t *>(base + 0x1b24b80 + 0x18);
        *reinterpret_cast<int32_t *>(e + 0x10) = (clock ? *reinterpret_cast<const int32_t *>(clock + 0x94) : 0) + 9;
        ++n[5];
    }
    return 0;
}

// The players list while the spoofed matchmaking runs (list ops: 1 status, 2 row text, 4 row icon, 5/10 selection,
// 6 row count, 20 row tint). Row 0 is the player; rows below the found count are found players, the rest are still
// being searched ("Finding Player..."); a just-found row shows "Found Player" for 180 ticks (or, with the fill mode,
// until every slot is found, where found rows also get green connection bars). M03 words: armed, run, t0, phase,
// found, max, sel, tphase.
namespace {
enum M03Word { m_run = 1, m_phase = 3, m_found = 4, m_max = 5, m_sel = 6, m_tphase = 7 };
U3X_BSS char player_row_name[24];
U3X_RO const char player_word[] = "Player ";
U3X_RO const char empty_text[] = "";
void set_row_text(int32_t *ev, const char *t) { *reinterpret_cast<const char **>(reinterpret_cast<uint8_t *>(ev) + 0x40) = t; }
uint32_t *ext_ec() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x4450); }
void icon(int32_t *ev, int32_t type, int32_t value, int32_t colour) {
    ev[2] = type;
    *reinterpret_cast<uint64_t *>(ev + 4) = 0;
    ev[6] = value;
    ev[7] = 0;
    ev[8] = 0;
    ev[9] = -1;
    ev[10] = 0;
    ev[11] = colour;
    ev[12] = 0x3f800000;   // 1.0f
}
}  // namespace
extern "C" U3X_FN int u3x_m03_players(uintptr_t base, int32_t *ev, void *s, const char *self_name) {
    uint32_t *m = u3h_m03();
    if (!m[m_run] || at<int32_t>(base + 0x17d5c58 + 0x40) <= 0) return 0;
    const uint32_t op = static_cast<uint32_t>(ev[0]), row = static_cast<uint32_t>(ev[1]);
    ++u3h_m03_diag()[7];
    switch (op) {
    case 1: game_call<void>(base + 0x75c190, ev); return 1;
    case 6:
        ev[1] = static_cast<int32_t>(m[m_max] - 1);
        u3x_clan_tag_poll(base, s);
        u3h_hud_alias_names(base, s);
        u3h_stage_tick(base, s);
        u3h_loadout_tick(base, s);
        return 1;
    case 5: ev[1] = static_cast<int32_t>(m[m_sel]); return 1;
    case 10: m[m_sel] = static_cast<uint32_t>(ev[1]); return 1;
    }
    if (row >= m[m_max]) return 1;
    const bool fill = !(ext_eb()[2] & 0x2000u) && !(ext_eb()[3] & 0x10000u);
    enum { searching, just_found, found } kind = row < m[m_found] ? found : searching;
    if (kind == found && row) {
        if (fill ? m[m_phase] <= 2
                 : row + 1 == m[m_found] && m[m_phase] == 2 && at<uint32_t>(base + 0x17b4a90 + 0x114) - m[m_tphase] < 180)
            kind = just_found;
    }
    if (op == 4 && ev[0x14] == 0 && kind != searching && row && fill) {
        icon(ev, 31, static_cast<int32_t>(5u - (row * 7u) % 3u), static_cast<int32_t>(0x8000ff00u));
        return 1;
    }
    if (op == 2) {
        if (ev[0x15] != 0) set_row_text(ev, empty_text);
        else if (kind == searching) set_row_text(ev, u3h_text(base, s, 0xc0759450u));
        else if (kind == just_found) set_row_text(ev, u3h_text(base, s, 0xa91680a8u));
        else if (row == 0) set_row_text(ev, self_name);
        else {
            unsigned n = 0;
            for (; player_word[n]; ++n) player_row_name[n] = player_word[n];
            char digits[12];
            unsigned d = 0;
            for (uint32_t v = row + 1; v; v /= 10) digits[d++] = static_cast<char>('0' + v % 10);
            while (d) player_row_name[n++] = digits[--d];
            player_row_name[n] = 0;
            set_row_text(ev, player_row_name);
        }
        return 1;
    }
    if (op == 4 && static_cast<uint32_t>(ev[0x14]) == 0xf5a294f3u) {
        if (kind == found) {
            uint32_t rank;
            if (row == 0) {
                const uintptr_t rank_fn = state_native(s, 21);
                uint8_t *profile = player_profile(base);
                if (!rank_fn || !profile) return 1;
                rank = reinterpret_cast<uint32_t (*)(void *)>(rank_fn)(profile);
            } else {
                rank = 1 + (row * 17u + 5u) % 60u;
            }
            icon(ev, 0x29, static_cast<int32_t>(rank), -1);
            u3h_rank_icon(base, s, ev);
        } else if (kind == searching) {
            icon(ev, 16, 0, -1);
        }
        return 1;
    }
    if (op == 20 && !(ext_ec()[2] & 0x10u)) return 1;   // no team or party tint (PS3 tints only those)
    if (op == 20 && ev[0x16] == 2 && kind == found)
        ev[0xf] = static_cast<int32_t>((u3h_team_colour(base, static_cast<int32_t>(row & 1)) & 0xffffffu) | 0x80000000u);
    return 1;
}

// ===== v7.144: part 5 (death anim check, session members, message queue, net profile rows, film select / protect / delete) =====

extern "C" {
U3H_IMPORT int32_t u3h_cm_get(uintptr_t base, const char *name);
U3H_IMPORT int32_t u3h_cm_release(uintptr_t base, int32_t id);
U3H_IMPORT int32_t u3h_msg_send(uintptr_t base, const uint8_t *id, uint32_t type, const void *data, uint32_t length);
U3H_IMPORT int u3h_gate_closed(uintptr_t base, int32_t *ev);
U3H_IMPORT uint8_t *u3h_cin_state(uintptr_t base);
U3H_IMPORT char *u3h_cin_path(char *out, int size, const char *dir, const char *name);
U3H_IMPORT void u3h_cin_scan(uintptr_t base, void *s, uint8_t *st);
U3H_IMPORT void u3h_cin_flags(uint8_t *st);
U3H_IMPORT int u3h_cin_game(uintptr_t base, void *s, int16_t *game, uint8_t *st);
}

namespace {
uint32_t *ext_dth() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x3b20); }
uint64_t ext_dthr0() { return *reinterpret_cast<uint64_t *>(u3h_ext() + 0x3c20); }
uintptr_t ext_clock_fn() { return *reinterpret_cast<uintptr_t *>(u3h_ext() + 0x4050 + 4 * 4); }   // eb[4..5]: time()
uintptr_t ext_kernel(int k) { return *reinterpret_cast<uintptr_t *>(u3h_ext() + 0x3210 + k * 8); }
uint32_t ext_functions() { return *reinterpret_cast<uint32_t *>(u3h_ext() + 0x3a70); }
// a net player's id: low 12 bits (+0x800 when the second-screen flag is set), the high part from +0x1e8
uint32_t net_player_id(const uint8_t *np) {
    const uint32_t low = rd<uint32_t>(np, 0x1e4) & 0xfff;
    return ((np[0x1ec] && low) ? low + 0x800 : low) + (rd<uint32_t>(np, 0x1e8) << 12);
}
// a handle {object **, id}: the object when it still carries that id at +0x7c
uint8_t *handle_object(uint8_t **h, uint32_t id) { return (h && id && *h && rd<uint32_t>(*h, 0x7c) == id) ? *h : nullptr; }
}  // namespace

// The death event constructor (kind 56) looks the death anim up in the dying character's 8 anim tables and traps
// when it is missing. This replays the lookup and records it in ExtState +0x3b20 (dth); a missing anim skips the
// event (returns 1) unless switch bit 0x200 asks for the game's own behaviour. When the caller is the damage path
// 0x99cce0 the damage record (r13, stored at dthr) and its attacker are recorded too.
extern "C" U3X_FN int u3x_death_anim_check(uintptr_t base, void *s, const uint64_t *blk) {
    uint32_t *d = ext_dth();
    const uint32_t sid = static_cast<uint32_t>(blk[5]), player = static_cast<uint32_t>(blk[6]);
    const uint64_t caller = blk[10] - base;
    ++d[0];
    d[4] = static_cast<uint32_t>(caller);
    d[5] = player;
    d[7] = sid;
    if (caller == 0x99cce0) {
        ++d[12];
        if (const uint8_t *damage = reinterpret_cast<const uint8_t *>(ext_dthr0())) {
            d[9] = rd<uint32_t>(damage, 0x1c);
            for (int i = 0; i < 16; ++i) d[16 + i] = rd<uint32_t>(damage, i * 4);
            for (int i = 0; i < 12; ++i) d[52 + i] = rd<uint32_t>(damage, 0x40 + i * 4);
            const uint8_t *attacker = handle_object(rd<uint8_t **>(damage, 8), rd<uint32_t>(damage, 0x10));
            d[10] = attacker ? rd<uint32_t>(attacker, 0x30) : 0;
            d[11] = attacker ? rd<uint32_t>(attacker, 0x28) : 0;
            for (int i = 0; i < 8; ++i) d[32 + i] = attacker ? rd<uint32_t>(attacker, 0x20 + i * 4) : 0;
        } else {
            d[9] = 0xffffffffu;
        }
    }
    if (!player) return 0;
    uint8_t *character = nullptr;
    for (int i = 0; i < 10; ++i) {
        const uint8_t *np = reinterpret_cast<const uint8_t *>(base + 0x17cb060 + 0x390) + i * 0x9c0;
        if (net_player_id(np) != player) continue;
        character = handle_object(rd<uint8_t **>(np, 0x10), rd<uint32_t>(np, 0x18));
        for (int k = 0; k < 4; ++k) d[48 + k] = rd<uint32_t>(np, k * 4);
        break;
    }
    if (!character) return 0;
    const uint32_t type = rd<uint32_t>(character, 0x30);
    d[3] = type;
    uint8_t **tables;
    if (type == 0xf2c86c22u) tables = reinterpret_cast<uint8_t **>(character + 0x468);       // NetCharacter
    else if (type == 0xa5a63e9bu) tables = rd<uint8_t **>(character, 0x1c20);               // Player
    else return 0;
    if (!tables) return 0;
    uint32_t present = 0;
    for (int i = 0; i < 8; ++i) {
        // table -> +0x30 -> +0x10 = {u32 count, sorted 16-byte entries (sid at +0)}
        const uint8_t *a = tables[i] ? rd<const uint8_t *>(tables[i], 0x30) : nullptr;
        const uint8_t *list = a ? rd<const uint8_t *>(a, 0x10) : nullptr;
        if (!list) { d[40 + i] = 0; continue; }
        const uint32_t count = rd<uint32_t>(list, 0);
        d[40 + i] = count;
        present |= 1u << i;
        const int32_t at = u3x_sorted_index(rd<const uint8_t *>(list, 8), count, sid);
        if (at >= 0) {
            d[8] = present;
            ++d[13];
            d[14] = static_cast<uint32_t>(i);
            d[15] = static_cast<uint32_t>(at);
            return 0;
        }
    }
    d[8] = present;
    ++d[1];
    d[2] = sid;
    if (state_switches(s) & 0x200) return 0;
    ++d[6];
    return 1;
}

// Session members (10 slots of 0x120 bytes from session +0x680; +0x25 used, +0x28 index, +0x2c id, +0x30 NAT type,
// +0x34 connection, +0x3c/+0x40 member data up to 0x40 bytes). Adding a name that is already there gives its index;
// otherwise the first free slot is filled, the session's next index taken, "you" / host ids noted, and the session
// told (member joined) unless the party is over capacity, in which case the backend kicks the new member.
namespace {
struct MemberInfo { const char *name; uint32_t id, nat, length; const uint8_t *data; uint32_t has_data; };
uint8_t *member_slot(uint8_t *session, int i) { return session + 0x680 + i * 0x120; }
bool same_name(const char *a, const char *b) {   // the game's compare of 0x24-byte names
    for (int i = 0; i < 0x24; ++i) {
        if (a[i] != b[i]) return false;
        if (!a[i]) return true;
    }
    return true;
}
void session_event(void *object, int slot, void *arg) {
    reinterpret_cast<int64_t (*)(void *, void *)>((*reinterpret_cast<uintptr_t **>(object))[slot])(object, arg);
}
}  // namespace
extern "C" U3X_FN int32_t u3x_add_member(uintptr_t base, uint8_t *session, const void *info, int you, int host) {
    const MemberInfo *in = static_cast<const MemberInfo *>(info);
    for (int i = 0; i < 10; ++i) {
        uint8_t *m = member_slot(session, i);
        if (m[0x25] && same_name(reinterpret_cast<const char *>(m), in->name)) return rd<int32_t>(m, 0x28);
    }
    for (int i = 0; i < 10; ++i) {
        uint8_t *m = member_slot(session, i);
        if (m[0x25]) continue;
        ++ext_eb()[80];
        *reinterpret_cast<int32_t *>(m + 0x34) = u3h_cm_get(base, in->name);
        *reinterpret_cast<uint32_t *>(m + 0x2c) = in->id & 0xffff;
        *reinterpret_cast<uint32_t *>(m + 0x30) = in->nat & 0xff;
        for (int k = 0; k < 0x24; ++k) m[k] = static_cast<uint8_t>(in->name[k]);
        const int32_t index = rd<int32_t>(session, 0x12e0);
        *reinterpret_cast<int32_t *>(m + 0x28) = index;
        *reinterpret_cast<int32_t *>(session + 0x12e0) = index + 1;
        *reinterpret_cast<uint32_t *>(m + 0x3c) = 0;
        m[0x25] = 1;
        if (in->has_data == 1) {
            const uint32_t n = in->length > 0x40 ? 0x40 : in->length;
            *reinterpret_cast<uint32_t *>(m + 0x3c) = n;
            for (uint32_t k = 0; k < n; ++k) m[0x40 + k] = in->data[k];
        }
        if (you) *reinterpret_cast<uint32_t *>(session + 0x12e4) = rd<uint32_t>(m, 0x2c);
        if (host) *reinterpret_cast<uint32_t *>(session + 0x12e8) = rd<uint32_t>(m, 0x2c);
        int count;
        if (!rd<uint64_t>(session, 0x18))   // no party: one player, two in split-screen
            count = 1 + (at<uint8_t>(base + 0x17d7b06) || at<int32_t>(base + 0x17f2fc0) == 3);
        else
            count = u3x_party_players(session);
        const uint32_t capacity = rd<uint32_t>(session, 0x20c);
        if (capacity && capacity < static_cast<uint32_t>(count)) {
            uint8_t *backend = rd<uint8_t *>(session, 8);
            if (backend)
                reinterpret_cast<int64_t (*)(void *, void *, uint32_t, uint32_t)>((*reinterpret_cast<uintptr_t **>(backend))[9])(
                    backend, session, rd<uint32_t>(m, 0x2c), 0);
        } else if (capacity) {
            session_event(session, 3, m);
        }
        return rd<int32_t>(m, 0x28);
    }
    return -1;
}
extern "C" U3X_FN int32_t u3x_remove_member(uintptr_t base, uint8_t *session, const char *name) {
    for (int i = 0; i < 10; ++i) {
        uint8_t *m = member_slot(session, i);
        if (!m[0x25] || !same_name(reinterpret_cast<const char *>(m), name)) continue;
        ++ext_eb()[81];
        session_event(session, 4, m);
        u3h_cm_release(base, rd<int32_t>(m, 0x34));
        m[0x25] = 0;
        *reinterpret_cast<int32_t *>(m + 0x34) = -1;
        return 0;
    }
    return -1;
}

// The game's message queue (64 entries of 0x40 bytes at 0x1b6d3d0, count at 0x1b6d3c0): a received message is
// appended with its data copied into the game's heap 0x14, so the game's readers free it as usual. Sending goes
// through the active backend. Messages are at most 0x1fb bytes.
extern "C" U3X_FN int32_t u3x_queue_post(uintptr_t base, const uint8_t *id, uint32_t type, const void *data, uint32_t length) {
    uint32_t &count = at<uint32_t>(base + 0x1b6d3c0);
    if (length + 4 >= 0x200 || count == 0x40) return -1;
    uint8_t *e = reinterpret_cast<uint8_t *>(base + 0x1b6d3d0) + count * 0x40;
    for (int k = 0; k < 0x24; ++k) e[k] = id[k];
    *reinterpret_cast<uint32_t *>(e + 0x28) = type;
    *reinterpret_cast<uint32_t *>(e + 0x38) = length;
    *reinterpret_cast<uint64_t *>(e + 0x30) = 0;
    if (length) {
        uint32_t heap[4];   // the heap id 0x14, then zeros (set one by one: an initialiser list would be a .rodata constant)
        heap[0] = 0x14;
        heap[1] = heap[2] = heap[3] = 0;
        uint8_t *copy_ = game_call<uint8_t *>(base + 0xd9eae0, static_cast<uint64_t>(length), static_cast<void *>(heap), uint64_t{8});
        if (!copy_) return -1;
        for (uint32_t k = 0; k < length; ++k) copy_[k] = static_cast<const uint8_t *>(data)[k];
        *reinterpret_cast<uint8_t **>(e + 0x30) = copy_;
    }
    ++count;
    ++ext_ea()[97];
    return 0;
}
extern "C" U3X_FN int32_t u3x_queue_send(uintptr_t base, const uint8_t *id, uint32_t type, const void *data, uint32_t length) {
    if (length + 4 >= 0x200) return -1;
    const int32_t r = u3h_msg_send(base, id, type, data, length);
    ++ext_ea()[98];
    return r < 0 ? r : 0;
}
// The network time in microseconds since 0001-01-01 (Unix seconds + 62135596800), 0 without a clock.
extern "C" U3X_FN uint64_t u3x_network_usec() {
    const uintptr_t clock = ext_clock_fn();
    const int64_t t = clock ? reinterpret_cast<int64_t (*)(int64_t *)>(clock)(nullptr) : 0;
    return t > 0 ? (static_cast<uint64_t>(t) + 62135596800ull) * 1000000ull : 0;
}

// The Network Profile page: its list widget (enum 0xc6, Hat Stats as row 10, the DLC word as row 11) is retagged to
// the helper's enum 0xcf so rows 10 and 11 can be answered; every other row and op goes to the game's own 0x76d2d0.
// Switch ea[0] bit 6 leaves the page alone. ea[24] retags, ea[25] row asks, ea[26] toggles.
namespace {
U3X_RO const uint32_t profile_row_title[2] = {0x2e153e84u, 0x9811415du};   // Hat Stats, DLC
}
extern "C" U3X_FN void u3x_retag_net_profile(uint8_t *page) {
    if ((ext_ea()[0] & 64) || rd<uint32_t>(page, 8) != 0xe0c0028bu) return;
    const uint8_t *items_ = rd<const uint8_t *>(page, 0x48);
    if (!items_) return;
    const uint32_t n = rd<uint32_t>(items_, 0);
    uint8_t *items = rd<uint8_t *>(items_, 8);
    if (!items || n > 64) return;
    for (uint32_t j = 0; j < n; ++j) {
        uint8_t *it = items + j * 0x68;
        if (rd<uint32_t>(it, 8) == 0xc073ebb7u && rd<uint32_t>(it, 0x30) == 21 && rd<uint32_t>(it, 0x34) == 0xc6 &&
            rd<uint32_t>(it, 0x40) == 0) {
            *reinterpret_cast<uint32_t *>(it + 0x34) = 0xcf;
            *reinterpret_cast<uint32_t *>(it + 0x40) = 0x4c414ec6u;
            ++ext_ea()[24];
        }
    }
}
extern "C" U3X_FN int u3x_net_profile_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414ec6u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    const int32_t op = ev[0];
    const int32_t row = (op == 0x15 || op == 0x16) ? ev[0x17] : (op == 2 || op == 4) ? ev[1] : -1;
    if (row == 10 || row == 11) {
        uint8_t *profile = player_profile(base);
        uint32_t *word = profile ? reinterpret_cast<uint32_t *>(profile + (row == 10 ? 0x215c : 0xac8)) : nullptr;
        ++ext_ea()[25];
        if (op == 2) {
            if (ev[0x15] == 0) set_row_text(ev, u3h_text(base, s, profile_row_title[row - 10]));
            else if (ev[0x15] == 1 && word) set_row_text(ev, u3h_text(base, s, *word == 0 ? 0xd1831446u : 0x32066ec9u));   // On / Off
        } else if (op == 0x15) {
            ev[1] = static_cast<int32_t>(profile_row_title[row - 10]);
        } else if (op == 0x16 && word) {
            *word = *word == 0;
            ++ext_ea()[26];
        }
        return 1;
    }
    game_call<void>(base + 0x76d2d0, ev);
    if (op == 6 && ev[1] == 9) ev[1] = 11;   // 12 rows, as PS3
    return 1;
}

// Film list actions (U3MP_CINEMA). The film state: selection +0x1e48, count +0x1e4c, the list +0x1e50 (entries of
// 0x1e90: name +0x100, mode +0x214), directory +0xb9, the stream header +0x140. A protected film has a marker file
// <dir>/p/<name> (no chmod here); mode 0x100 protected, 0x180 not.
namespace {
constexpr uint32_t film_entry = 0x1e90;
U3X_RO const char marker_dir[] = "p";
int32_t &film_selected(uint8_t *st) { return *reinterpret_cast<int32_t *>(st + 0x1e48); }
int32_t film_count(uint8_t *st) { return rd<int32_t>(st, 0x1e4c); }
uint8_t *film_list(uint8_t *st) { return rd<uint8_t *>(st, 0x1e50); }
const char *film_dir(uint8_t *st) { return reinterpret_cast<const char *>(st + 0xb9); }
bool cinema_on() { return (ext_functions() & 2) && ext_kernel(0) && ext_kernel(1) && ext_kernel(2) && ext_kernel(3); }
int kernel_unlink(const char *path) { return reinterpret_cast<int (*)(const char *)>(ext_kernel(2))(path); }
void marker_path(char *out, const char *dir, const char *name) {
    char sub[0x180];
    u3h_cin_path(sub, sizeof sub, dir, marker_dir);
    u3h_cin_path(out, 0x180, sub, name);
}
int64_t file_open(void *s, const char *path, int flags, int mode) {
    return reinterpret_cast<int64_t (*)(const char *, int, int)>(state_fn(s, 4))(path, flags, mode);
}
void file_read(void *s, int fd, void *buffer, uint64_t n) {
    reinterpret_cast<int64_t (*)(int, void *, uint64_t)>(state_fn(s, 5))(fd, buffer, n);
}
void file_close(void *s, int fd) { reinterpret_cast<int64_t (*)(int)>(state_fn(s, 7))(fd); }
}  // namespace
// Select film idx: its header goes into the stream, its first 0x100 stream bytes give the format and the net game
// index (32-bit words, first bit = most significant), which is selected when the net game table has it.
extern "C" U3X_FN int u3x_film_select(uintptr_t base, void *s, int idx) {
    uint8_t *st = u3h_cin_state(base);
    if (!(idx < film_count(st)) || !film_list(st)) return -1;
    film_selected(st) = idx;
    char path[0x180];
    u3h_cin_path(path, sizeof path, film_dir(st),
                 reinterpret_cast<const char *>(film_list(st)) + static_cast<uint32_t>(idx) * static_cast<uintptr_t>(film_entry) + 0x100);
    const int fd = static_cast<int>(file_open(s, path, 0, 0));
    if (fd < 0) return -1;
    uint32_t start[0x40];
    for (uint32_t &w : start) w = 0;
    file_read(s, fd, st + 0x140, 0x1c78);
    file_read(s, fd, start, 0x100);
    file_close(s, fd);
    int16_t game = static_cast<int16_t>(start[1]);
    ext_eb()[17] = start[0];
    ext_eb()[18] = static_cast<uint32_t>(static_cast<int32_t>(game));
    if (u3h_cin_game(base, s, &game, st) && state_native(s, 2))
        reinterpret_cast<void (*)(void *, int, void *, uint32_t)>(state_native(s, 2))(reinterpret_cast<void *>(base + 0x17cb060), game,
                                                                                     nullptr, 0xc);
    u3h_cin_flags(st);
    return 0;
}
// Protect (1) / Unprotect (0) the selected film: create or remove its marker file; the entry's mode follows when that
// worked.
extern "C" U3X_FN int u3x_film_protect(uintptr_t base, void *s, int on) {
    if (!cinema_on()) return 1;
    uint8_t *st = u3h_cin_state(base);
    uint8_t *list = film_list(st);
    if (!list) return 1;
    uint8_t *e = list + static_cast<uint32_t>(film_selected(st)) * static_cast<uintptr_t>(film_entry);
    const char *name = reinterpret_cast<const char *>(e + 0x100);
    char m[0x180];
    u3h_cin_path(m, sizeof m, film_dir(st), marker_dir);
    reinterpret_cast<int (*)(const char *, int)>(ext_kernel(0))(m, 0x1c0);   // mkdir <dir>/p
    marker_path(m, film_dir(st), name);
    bool ok;
    if (on) {
        const int fd = static_cast<int>(file_open(s, m, 0x601, 0x180));
        ok = fd >= 0;
        if (ok) file_close(s, fd);
    } else {
        kernel_unlink(m);
        char check[0x180];
        uint8_t stat_buffer[0x78];
        for (uint8_t &b : stat_buffer) b = 0;
        marker_path(check, film_dir(st), name);
        ok = reinterpret_cast<int (*)(const char *, void *)>(ext_kernel(3))(check, stat_buffer) != 0;   // the marker is gone
    }
    if (ok) *reinterpret_cast<uint32_t *>(e + 0x214) = on ? 0x100u : 0x180u;
    u3h_cin_flags(st);
    return 1;
}
// Delete the selected film, step the selection back, rescan.
extern "C" U3X_FN void u3x_film_delete(uintptr_t base, void *s) {
    uint8_t *st = u3h_cin_state(base);
    if (!film_list(st)) return;
    char path[0x180];
    u3h_cin_path(path, sizeof path, film_dir(st),
                 reinterpret_cast<const char *>(film_list(st)) + static_cast<uint32_t>(film_selected(st)) * static_cast<uintptr_t>(film_entry) + 0x100);
    kernel_unlink(path);
    if (film_selected(st) != 0) --film_selected(st);
    u3h_cin_scan(base, s, st);
    u3h_cin_flags(st);
    ++ext_eb()[19];
}
// The record-mode film header (PS4/1.10 layout): +0 format version, +4 online flag, +8 net game index, +0x20 time
// now, +0x28 the 24-byte custom rules block, +0x1dc 2 when matchmaking.
extern "C" U3X_FN void u3x_film_record_header(uintptr_t base, uint8_t *st) {
    uint8_t *h = st + 0x140;
    const uint8_t *net = reinterpret_cast<const uint8_t *>(base + 0x17d5c58);
    *reinterpret_cast<uint32_t *>(h) = rd<uint32_t>(st, 0xa0);
    *reinterpret_cast<uint32_t *>(h + 4) = (rd<int32_t>(net, 0x14) != 0 || rd<int32_t>(net, 0x10) != 0) ? 1u : 0u;
    *reinterpret_cast<uint32_t *>(h + 8) = at<uint16_t>(base + 0x17cb060 + 0x661c);
    if (const uintptr_t clock = ext_clock_fn()) *reinterpret_cast<int64_t *>(h + 0x20) = reinterpret_cast<int64_t (*)(int64_t *)>(clock)(nullptr);
    for (int k = 0; k < 0x18; ++k) h[0x28 + k] = at<uint8_t>(base + 0x17cb060 + 0x6a38 + k);
    *reinterpret_cast<uint32_t *>(h + 0x1dc) = rd<int32_t>(net, 0x40) != 0 ? 2u : 0u;
}

// ===== v7.145: part 6 (spotted HUD icon, screen menus, Records rows and card tabs, slider track) =====

extern "C" {
U3H_IMPORT uint8_t u3h_spot_flag(uintptr_t base, uint8_t *np);
U3H_IMPORT int u3h_onb_mode(void);
U3H_IMPORT uint32_t u3h_active_page(uintptr_t base);
U3H_IMPORT volatile uint32_t *u3h_w10_diag(void);
U3H_IMPORT uint64_t u3h_game_call(void *call);
}

// The "spotted" HUD image (HUD-net slot 3): the hud-settings element (DC 0x9353ed16 +0x78 -> +0x158) is drawn while
// the local net player's tracker has the spotted flag. Its texture id is looked up in the icon table
// (*(0x1b24bc8)+0x20, 0x28-byte entries, id at +8); position +0x1c, size +0x14, direction +0x28, rotate +0x24,
// colour +0x30. The quad draw 0x11b8dd0 takes its vectors in xmm0..xmm4 (floats passed through as bits).
// ExtState +0x2fe0 (hs): [0] calls, [1] draws, [2] texture id, [3] icon index.
namespace {
U3X_RO const uint32_t unit_scale[2] = {0x3f800000u, 0x3f800000u};
U3X_FN void draw_hud_quad(uintptr_t base, const uint64_t *pos, const uint64_t *size, const uint64_t *dir, uint64_t texture,
                          int32_t index, uint32_t *colour, int rotate) {
    register uint64_t a4 asm("r8") = static_cast<uint64_t>(rotate);
    register uint64_t a5 asm("r9") = 1;
    uintptr_t fn = base + 0x11b8dd0;
    uint64_t a0 = 0, a3 = static_cast<uint32_t>(index);
    asm volatile(
        "movq %[p], %%xmm0\n\t"
        "movq %[sz], %%xmm1\n\t"
        "movq %[d], %%xmm2\n\t"
        "xorps %%xmm3, %%xmm3\n\t"
        "movq %[o], %%xmm4\n\t"
        "push %%rbp\n\t"
        "mov %%rsp, %%rbp\n\t"
        "and $-16, %%rsp\n\t"
        "call *%%rax\n\t"
        "mov %%rbp, %%rsp\n\t"
        "pop %%rbp"
        : "+a"(fn), "+D"(a0), "+S"(texture), "+d"(a3), "+c"(colour), "+r"(a4), "+r"(a5)
        : [p] "m"(*pos), [sz] "m"(*size), [d] "m"(*dir), [o] "m"(*reinterpret_cast<const uint64_t *>(unit_scale))
        : "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11",
          "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
}
}  // namespace
extern "C" U3X_FN void u3x_hud_spotted(uintptr_t base) {
    uint32_t *d = reinterpret_cast<uint32_t *>(u3h_ext() + 0x2fe0);
    ++d[0];
    const uint8_t *settings = game_call<const uint8_t *>(base + 0x11f2e30, 0x9353ed16u);
    if (!settings) return;
    const uint8_t *object = rd<const uint8_t *>(settings, 0x78);
    const uint8_t *hud = at<const uint8_t *>(base + 0x1806480);
    if (!object || !hud) return;
    const int32_t screen = rd<int32_t>(hud, 0x1d0);
    if (screen < 0 || screen > 1) return;
    uint8_t *tracker = at<uint8_t *>(base + 0x17cb3d0 + screen * 8);
    if (!tracker || !u3h_spot_flag(base, tracker)) return;
    uint8_t *e = rd<uint8_t *>(object, 0x158);
    if (!e) return;
    const uint32_t texture = rd<uint32_t>(e, 4);
    int32_t index = -1;
    if (const uint8_t *icons = at<const uint8_t *>(base + 0x1b24bc8)) {
        const uint8_t *t = rd<const uint8_t *>(icons, 0x20);
        if (t && rd<uint16_t>(t, 2)) {
            const uint8_t *entries = rd<const uint8_t *>(t, 8);
            for (int i = 0; entries && i < rd<uint16_t>(t, 2); ++i)
                if (rd<uint32_t>(entries, i * 0x28 + 8) == texture) { index = i; break; }
        }
    }
    alignas(16) uint32_t colour[4];
    for (int i = 0; i < 4; ++i) colour[i] = rd<uint32_t>(e, 0x30 + i * 4);
    d[2] = texture;
    d[3] = static_cast<uint32_t>(index);
    ++d[1];
    draw_hud_quad(base, reinterpret_cast<const uint64_t *>(e + 0x1c), reinterpret_cast<const uint64_t *>(e + 0x14),
                  reinterpret_cast<const uint64_t *>(e + 0x28), texture, index, colour, rd<int32_t>(e, 0x24) != 0);
}

// Menu objects per screen: screen 0 = *(0x16e3188), screen 1 = 0x17bfa10. Menu state +8 (0 idle, 5 closing, 6 closed,
// 0xc shown), +0xc 1 = root menu, page stack +0x8b0 (depth +0xdb0, at most 160), popup +0x898. The four game
// functions used here are checked for their usual prologue before any call.
namespace {
U3X_RO const uint8_t usual_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
U3X_FN bool has_prologue(uintptr_t base, uint32_t offset) {
    for (int i = 0; i < 12; ++i)
        if (at<uint8_t>(base + offset + i) != usual_prologue[i]) return false;
    return true;
}
U3X_FN bool menu_calls_ok(uintptr_t base) {
    return has_prologue(base, 0xf23970) && has_prologue(base, 0x6534e0) && has_prologue(base, 0x64c0a0) && has_prologue(base, 0x651940);
}
U3X_FN int show_menu_page(uintptr_t base, uint8_t *m, uint32_t page, int fresh, int silent) {
    return game_call<int>(base + 0x651940, m, page, fresh, silent);
}
}  // namespace
extern "C" U3X_FN uint8_t *u3x_screen_menu(uintptr_t base, int32_t screen) {
    return screen ? reinterpret_cast<uint8_t *>(base + 0x17bfa10) : at<uint8_t *>(base + 0x16e3188);
}
extern "C" U3X_FN int u3x_menu_open(const uint8_t *m) { return m && rd<uint32_t>(m, 8) != 0 && rd<uint32_t>(m, 0xc) == 1; }
// the page on top of the stack (any menu state)
extern "C" U3X_FN uint32_t u3x_menu_page(const uint8_t *m) {
    if (!m) return 0;
    const int32_t depth = rd<int32_t>(m, 0xdb0);
    if (depth < 0 || depth >= 160) return 0;
    const uint8_t *page = rd<const uint8_t *>(m, 0x8b0 + depth * 8);
    return page ? rd<uint32_t>(page, 0x10) : 0;
}
extern "C" U3X_FN int u3x_menu_calls_ok(uintptr_t base) { return menu_calls_ok(base); }
// A state change is announced around the write: an empty event (0xf23970) goes to the menu's listeners as "before"
// (3) and "after" (0); the transition timer +0xdb4 restarts and its speed +0xdb8 is 1.0.
extern "C" U3X_FN void u3x_menu_set_state(uintptr_t base, uint8_t *m, uint32_t state) {
    alignas(16) uint64_t ev[20];
    for (uint64_t &w : ev) w = 0;
    game_call<void>(base + 0xf23970, static_cast<void *>(ev), 0, 0);
    game_call<void>(base + 0x6534e0, m, static_cast<void *>(ev), 3);
    *reinterpret_cast<uint32_t *>(m + 8) = state;
    *reinterpret_cast<uint32_t *>(m + 0xdb4) = 0;
    *reinterpret_cast<uint32_t *>(m + 0xdb8) = 0x3f800000u;
    game_call<void>(base + 0x6534e0, m, static_cast<void *>(ev), 0);
}
// Show a page on a menu, activating it first when idle; then state 0xc.
extern "C" U3X_FN void u3x_menu_show(uintptr_t base, uint8_t *m, uint32_t page) {
    if (!m || !menu_calls_ok(base)) return;
    const int idle = rd<uint32_t>(m, 8) == 0;
    if (idle) game_call<void>(base + 0x64c0a0, m, 1, 1);
    show_menu_page(base, m, page, idle, 0);
    u3x_menu_set_state(base, m, 0xc);
}
// Back: the page under the top (the top itself while a popup is up), or close a root menu.
extern "C" U3X_FN int u3x_menu_back(uintptr_t base, uint8_t *m, int silent) {
    if (!m || !menu_calls_ok(base)) return 0;
    const uint32_t state = rd<uint32_t>(m, 8);
    if (!state) return 0;
    const int32_t depth = rd<int32_t>(m, 0xdb0);
    if (depth > 0 && depth < 160) {
        const int32_t i = rd<uint64_t>(m, 0x898) ? depth : depth - 1;
        const uint8_t *page = rd<const uint8_t *>(m, 0x8b0 + i * 8);
        ++ext_coop()[10];
        return page ? show_menu_page(base, m, rd<uint32_t>(page, 0x10), 0, silent) & 255 : 0;
    }
    if (rd<uint32_t>(m, 0xc) == 1) {
        if (state != 5 && state != 6) u3x_menu_set_state(base, m, 5);
        return 1;
    }
    return 0;
}
extern "C" U3X_FN void u3x_menu_close(uintptr_t base, uint8_t *m) {
    if (!m || !menu_calls_ok(base)) return;
    const uint32_t state = rd<uint32_t>(m, 8);
    if (state && state != 5 && state != 6) {
        u3x_menu_set_state(base, m, 5);
        ext_coop()[10] += 0x10000;
    }
}

// n032: in LAN Party (or, with ea[122] bit 2, only in offline mode) the Records page gets PS3 1.19's online rows
// (two extra rows ahead of the native ones, Legacy, Message of the Day), and the player card's tab bar and body run
// their natives with the LAN byte cleared, as net.bin has only the online entry for that page. ea[122] bit 0 keeps
// the 3 LAN Records rows, bit 1 leaves the card alone. ea[125] counts Records answers (low 16) and row clicks (<< 16),
// ea[127] card calls run as online (low 24) and retags (<< 24).
namespace {
U3X_FN uint32_t &n32_switches() { return ext_ea()[122]; }
U3X_FN bool n32_active(uintptr_t base) {
    if (!at<volatile uint8_t>(base + 0x17d5c60) || !at<volatile uint8_t>(base + 0x17d5c61)) return false;
    return !((n32_switches() & 4u) && u3h_onb_mode() != 2);
}
}  // namespace
extern "C" U3X_FN int u3x_n32_mode(uintptr_t base) { return n32_active(base); }
extern "C" U3X_FN int u3x_n32_records(uintptr_t base) { return !(n32_switches() & 1u) && n32_active(base); }
extern "C" U3X_FN int u3x_n32_call(uintptr_t base, int32_t *ev, void *s, void (*native)(int32_t *)) {
    const uint32_t *rows = reinterpret_cast<const uint32_t *>(base + 0x13a6620);   // {text, page, button, new mask} x 5
    const int32_t op = ev[0];
    uint32_t &counts = ext_ea()[125];
    if (op == 6) {
        native(ev);
        if (ev[1] >= 0) ev[1] += 3;
        counts = (counts & 0xffff0000u) | ((counts + 1u) & 0xffffu);
        return 1;
    }
    const int k = (op == 2 || op == 4) ? 1 : (op == 0x15 || op == 0x16) ? 0x17 : 0;
    if (!k) return 0;
    const int32_t row = ev[k];
    if (row == 5 || row == 2 || row == 3) {   // the native's rows: Legacy (its 2), then its 0 and 1
        ev[k] = row == 5 ? 2 : row - 2;
        native(ev);
        ev[k] = row;
        return 1;
    }
    if (row == 4) {   // Message of the Day
        if (op == 2) {
            if (ev[0x15] == 0) set_row_text(ev, u3h_text(base, s, 0xa6694384u));
        } else if (op == 0x15) {
            ev[1] = static_cast<int32_t>(0xa6694384u);
        } else if (op == 0x16) {
            u3h_show_dialog(base, s, 0x60edaa4fu, 0xc6467f2du, 0, 0, 0, 0xe1e040aau);
        }
        return 1;
    }
    if (row < 0 || row >= 4) return 1;
    const uint32_t *r = rows + row * 4;
    if (op == 2) {
        if (ev[0x15] == 0) set_row_text(ev, u3h_text(base, s, r[0]));
    } else if (op == 0x15) {
        ev[1] = static_cast<int32_t>(r[0]);
    } else if (op == 0x16) {
        void *menu = reinterpret_cast<void *>(base + 0x17bd080);
        if (r[2]) game_call<void>(base + 0x64ce90, menu, r[2], 0);
        if (r[1]) game_call<void>(base + 0x651940, menu, r[1], 0, 0);
        counts += 0x10000u;
    }
    return 1;   // op 4: mask 0, no icon, as the native
}
extern "C" U3X_FN void u3x_n32_retag(uint8_t *page) {
    if ((n32_switches() & 2u) || rd<uint32_t>(page, 8) != 0x83f24f04u) return;
    const uint8_t *c = rd<const uint8_t *>(page, 0x48);
    if (!c) return;
    const uint32_t n = rd<uint32_t>(c, 0);
    uint8_t *items = rd<uint8_t *>(c, 8);
    if (!items || n > 64) return;
    for (uint32_t j = 0; j < n; ++j) {
        uint8_t *w = items + j * 0x68;
        if (rd<uint32_t>(w, 0x30) != 21 || rd<uint32_t>(w, 0x40) != 0) continue;
        const uint32_t list_enum = rd<uint32_t>(w, 0x34);
        uint32_t tag = 0;
        if (list_enum == 0xc3 && rd<uint32_t>(w, 0x38) == 0x83f24f04u) tag = 0x4e4e32c3u;
        else if (list_enum == 0xde) tag = 0x4e4e32deu;
        if (!tag) continue;
        *reinterpret_cast<uint32_t *>(w + 0x34) = 0xcf;
        *reinterpret_cast<uint32_t *>(w + 0x40) = tag;
        ext_ea()[127] += 0x1000000u;
    }
}
extern "C" U3X_FN int u3x_n32_source(uintptr_t base, int32_t *ev) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source) return 0;
    const uint32_t tag = rd<uint32_t>(source, 0x1c);
    if (tag != 0x4e4e32c3u && tag != 0x4e4e32deu) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    uint8_t &lan = at<uint8_t>(base + 0x17d5c60);
    const uint8_t keep = lan;
    if (!(n32_switches() & 2u) && n32_active(base)) {
        lan = 0;
        uint32_t &c = ext_ea()[127];
        c = (c & 0xff000000u) | ((c + 1u) & 0xffffffu);
    }
    game_call<void>(base + (tag == 0x4e4e32c3u ? 0x76bbf0 : 0x76fa00), ev);
    lan = keep;
    return 1;
}

// w010: the slider track. On the PC the bar's black background (drawn by 0x63c730) does not show, only the fill; the
// background is drawn first as PS4's own box item (type 0x1b, colour 0xff000000): a copy of the laid-out item through
// the same item draw 0x63d6f0 with the caller's registers, then the native draws the fill on top. Only on the emblem
// page (ec[3] bit 1: on every multiplayer type-4 bar); ec[3] bit 0 turns it off. w10_d: [0] type-4 items, [1] tracks
// drawn, [2] skipped, [3] last track width | height << 16.
namespace {
struct GameCall { uint64_t fn, i[6], x[5], s[2], al; };
U3X_FN bool plausible(const void *p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47) && !(v & 3);
}
}  // namespace
extern "C" U3X_FN void u3x_slider_track(uintptr_t base, const uint64_t *r) {
    volatile uint32_t *d = u3h_w10_diag();
    const uint8_t *item = reinterpret_cast<const uint8_t *>(r[7]);
    if (!plausible(item) || rd<uint32_t>(item, 0) != 4u) return;
    d[0] = d[0] + 1;
    const uint32_t mode = ext_ec()[3];
    bool skip = (mode & 1u) != 0;
    if (!skip) skip = (mode & 2u) ? !at<uint8_t>(base + 0x17d5c61) : u3h_active_page(base) != 0x85d761b5u;
    if (!skip)
        skip = !plausible(reinterpret_cast<const void *>(r[6])) || !plausible(reinterpret_cast<const void *>(r[3])) ||
               at<uint32_t>(base + 0x63d6f0) == 0;
    if (skip) {
        d[2] = d[2] + 1;
        return;
    }
    alignas(16) uint8_t box[0x70];
    for (int k = 0; k < 0x70; ++k) box[k] = item[k];
    *reinterpret_cast<uint32_t *>(box) = 0x1bu;
    *reinterpret_cast<uint32_t *>(box + 0x24) = 0xff000000u;
    GameCall c;
    for (uint32_t k = 0; k < sizeof c; ++k) reinterpret_cast<uint8_t *>(&c)[k] = 0;
    c.fn = base + 0x63d6f0;
    c.i[0] = r[4];
    c.i[1] = r[5];
    c.i[2] = r[6];
    c.i[3] = reinterpret_cast<uint64_t>(box);
    c.i[4] = r[3];
    c.i[5] = r[2];
    c.s[0] = r[11];
    c.s[1] = r[12];
    u3h_game_call(&c);
    d[1] = d[1] + 1;
    d[3] = (static_cast<uint32_t>(f_to_int(rd<uint32_t>(item, 0x3c))) & 0xffffu) |
           static_cast<uint32_t>(f_to_int(rd<uint32_t>(item, 0x40))) << 16;
}

// ===== v7.146: part 7 (anim-state decode entry points, NPC anim bits, blended locator, wall emblems, film bit streams and bookmarks) =====

extern "C" {
U3H_IMPORT void u3h_decode(void *visitor, uint8_t *snapshot, uint8_t *generator, int32_t a4, int32_t a5);
U3H_IMPORT int32_t u3h_pick_snapshot(uintptr_t base, uint8_t *generator, uint32_t *blend);
U3H_IMPORT uint64_t u3h_call_f32(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
U3H_IMPORT uint32_t u3h_anim_pack(uintptr_t base, uint8_t *msg, uint8_t *stream, uint8_t *actor, uint8_t **tables);
U3H_IMPORT uint32_t *u3h_repl_error(void);
U3H_IMPORT void u3h_repl_loop(uintptr_t base, uint8_t *actor, uint8_t **tables, uint8_t *msg, uint32_t bits);
U3H_IMPORT uint8_t *u3h_emb_team_player(uintptr_t base, int32_t team);
U3H_IMPORT int u3h_emb_is_record(uint64_t p);
U3H_IMPORT uint32_t *u3h_emb_wall_diag(void);
U3H_IMPORT void u3h_rp_assert(uintptr_t base);
}

// The anim-state decoder's visitor (the decoder itself stays in the C part): frame, 4 locators {scale, quat, trans},
// image base, entries, command buffer, decoder args, ids, flags, kind (0 state, 1 commands, 2 locator) and the error
// that stopped the decode (12 = nothing to decode).
namespace {
struct alignas(16) Visitor {
    uint8_t frame[0x60];
    uint8_t loc[4][0x30];
    uintptr_t base;
    uint8_t *entries, *commands;
    int32_t v1, v2;
    uint32_t v60, key, channel;
    uint8_t abort, active;
    int kind, error;
};
static_assert(sizeof(Visitor) == 0x160);
constexpr uint32_t snapshot_bytes = 0x3f0;
// a locator as {quat, trans, 0}: the decoder's is {scale, quat, trans}
U3X_FN void take_locator(const Visitor &v, uint8_t *to) {
    for (int k = 0; k < 0x10; ++k) {
        to[k] = v.loc[0][0x20 + k];
        to[0x10 + k] = v.loc[0][0x10 + k];
        to[0x20 + k] = 0;
    }
}
}  // namespace
// kind 101 (state apply): decode the snapshot at generator +0xb0 + offset into its entries; a failed decode leaves
// them cleared as PS4 does. ec[0] bit 9 turns the c009 decoder off. ec[80] runs, ec[81] errors (not "empty"),
// ec[83] the snapshot's word +0x220.
extern "C" U3X_FN void u3x_apply_decode(uintptr_t base, const uint64_t *regs) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 512) return;
    uint8_t *g = reinterpret_cast<uint8_t *>(regs[4]);
    const uint64_t offset = regs[3];
    if (!g || offset >= 6 * snapshot_bytes) return;
    uint8_t *snapshot = g + 0xb0 + offset;
    ++ec[80];
    Visitor v;
    v.base = base;
    v.kind = 0;
    v.error = 0;
    v.entries = snapshot + 0x10;
    v.commands = nullptr;
    v.channel = 0;
    u3h_decode(&v, snapshot, g, 0, 0);
    if (v.error) {
        if (v.error != 12) ++ec[81];
        for (int i = 0; i < 0x214; ++i) snapshot[0x10 + i] = 0;
    }
    ec[83] = rd<uint32_t>(snapshot, 0x220);
}
// kind 103: an NPC's anim state into its state message (300 bytes of bits at +0x18, count at +0x144). ec[0] bit 11
// sends none. ec[90] packs, ec[91] last bit count, ec[92] failed packs.
extern "C" U3X_FN void u3x_npc_bits(uintptr_t base, const uint64_t *regs) {
    uint8_t *msg = reinterpret_cast<uint8_t *>(regs[4]), *npc = reinterpret_cast<uint8_t *>(regs[0]),
            *h = reinterpret_cast<uint8_t *>(regs[13]);
    if (!msg) return;
    uint32_t bits = 0;
    uint32_t *ec = ext_ec();
    if (!(ec[0] & 2048) && npc && h) {
        ++ec[90];
        alignas(16) uint8_t stream[0x10];
        for (int i = 0; i < 300; ++i) msg[0x18 + i] = 0;   // PS4's writers keep stale bits at word starts
        *reinterpret_cast<uint8_t **>(stream) = msg + 0x18;
        *reinterpret_cast<uint32_t *>(stream + 8) = 0;
        *reinterpret_cast<uint32_t *>(stream + 0xc) = 300 * 8;
        uint32_t &error = *u3h_repl_error();
        error = 0;
        bits = u3h_anim_pack(base, msg, stream, npc, reinterpret_cast<uint8_t **>(h + 0x68));
        if (error) {
            ++ec[92];
            bits = 0;
        }
        ec[91] = bits;
        u3h_repl_loop(base, npc, reinterpret_cast<uint8_t **>(h + 0x68), msg, bits);
    }
    *reinterpret_cast<uint32_t *>(msg + 0x144) = bits;
}
// kind 104 (GetLocator): a channel's locator blended between the generator's two current snapshots. No snapshot:
// the zero locator with w = 1. A failed decode runs PS4's own code (returns 0). ec[86] runs, ec[87] failures.
extern "C" U3X_FN int u3x_blended_locator(uintptr_t base, uint64_t *regs) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 512) return 0;
    uint8_t *out = reinterpret_cast<uint8_t *>(regs[4]), *g = reinterpret_cast<uint8_t *>(regs[5]);
    const uint32_t channel = static_cast<uint32_t>(regs[6]);
    if (!out || !g) return 0;
    ++ec[86];
    uint32_t blend;
    const int32_t idx = u3h_pick_snapshot(base, g, &blend);
    if (idx == -1) {
        uint32_t *o = reinterpret_cast<uint32_t *>(out);
        for (int k = 0; k < 8; ++k) o[k] = 0;
        o[7] = 0x3f800000u;
        regs[8] = reinterpret_cast<uint64_t>(out);
        return 1;
    }
    uint8_t *s0 = g + 0xb0 + idx * static_cast<intptr_t>(snapshot_bytes), *s1 = s0 + snapshot_bytes;
    alignas(16) uint8_t a[0x30], b[0x30], result[0x30];
    Visitor v;
    v.base = base;
    v.kind = 2;
    v.error = 0;
    v.commands = nullptr;
    v.entries = nullptr;
    v.channel = channel;
    u3h_decode(&v, s0, g, 0, 0);
    take_locator(v, a);
    if (!v.error) u3h_decode(&v, s1, g, 1, 1);
    if (v.error) {
        ++ec[87];
        if (v.error != 12) ++ec[81];
        return 0;
    }
    take_locator(v, b);
    u3h_call_f32(base + 0xfd3c60, blend, reinterpret_cast<uint64_t>(result), reinterpret_cast<uint64_t>(a), reinterpret_cast<uint64_t>(b), 0);
    for (int k = 0; k < 0x20; ++k) out[k] = result[k];
    regs[8] = reinterpret_cast<uint64_t>(out);
    return 1;
}

// Wall emblems: every map decal object whose art name has "-multi-emblem-" shows the emblem of the local player (in
// team modes, of the team's player): "...-multi-emblem-board" the board layer record (+0x7e8), the others +0x7f0;
// without a record the empty default (emblem manager +0x78). The record's texture goes into each of the object's
// render tables. ew: [0] ticks, [1] objects filled, [2] no table, [3] no record (low) | objects seen (<< 16).
// ec[147] player records used, ec[148] defaults, ec[151]/[152] the player's layer 0 and address (low).
namespace {
U3X_RO const char emblem_mark[] = "-multi-emblem-";
U3X_RO const uint16_t decal_roots[2] = {0xd0, 0xd8};
U3X_FN bool aligned_pointer(const void *p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47) && !(v & 7);
}
template <typename R, typename... A> R virtual_call(uint8_t *object, uint32_t slot, A... args) {
    return reinterpret_cast<R (*)(uint8_t *, A...)>(*reinterpret_cast<uintptr_t *>(*reinterpret_cast<uintptr_t *>(object) + slot))(object, args...);
}
}  // namespace
extern "C" U3X_FN void u3x_wall_emblems(uintptr_t b) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 0x4000u) return;
    const uint8_t *manager = reinterpret_cast<const uint8_t *>(b + 0x17c3440);
    uint32_t *ew = u3h_emb_wall_diag();
    ++ew[0];
    uint8_t *np = nullptr;
    if (at<int32_t>(b + 0x17d1868) == 2) np = u3h_emb_team_player(b, game_call<int32_t>(b + 0x737eb0, b + 0x17cb060));
    if (!np) np = at<uint8_t *>(b + 0x17cb060 + 0x370);
    if (np && !aligned_pointer(np)) np = nullptr;
    const uint64_t fallback = rd<uint64_t>(manager, 0x78);
    for (int bi = 0; bi < 2; ++bi) {
        uint8_t *root = at<uint8_t *>(b + 0x1b37610 + decal_roots[bi]);
        if (!aligned_pointer(root)) continue;
        uint8_t *p = root;
        for (uint32_t n = 0; n < 16384; ++n) {
            p = game_call<uint8_t *>(b + 0x11a4580, p, static_cast<void *>(root));   // next object in the tree
            if (!p) break;
            if ((rd<uint16_t>(p, 0x40) & 5) != 1) continue;
            const uint32_t type = rd<uint32_t>(p, 0x30);
            if (type != 0x2db62a42u && type != 0xb95beddbu) continue;
            ew[3] += 0x10000u;
            uint8_t *art = virtual_call<uint8_t *>(p, 0xe8);
            if (!art || !aligned_pointer(art)) continue;
            const char *name = rd<const char *>(art, 0x48);
            if (!name) continue;
            const char *after = nullptr;   // the text after the mark, in the first 40 characters
            const char *c = name;
            for (int w = 0; *c && w < 40; ++c, ++w) {
                const char *x = c, *m = emblem_mark;
                while (*m && *x == *m) { ++x; ++m; }
                if (!*m) { after = x; break; }
            }
            if (!after) continue;
            const bool board = after[0] == 'b' && after[1] == 'o' && after[2] == 'a';
            uint64_t record = np ? rd<uint64_t>(np, board ? 0x7e8 : 0x7f0) : 0;
            if (!record || !u3h_emb_is_record(record)) {
                record = fallback;
                ++ec[148];
            } else {
                ++ec[147];
            }
            if (np) {
                ec[151] = rd<uint32_t>(np, 0x980);
                ec[152] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(np));
            }
            const uint64_t texture = (record && u3h_emb_is_record(record)) ? *reinterpret_cast<const uint64_t *>(record) : 0;
            if (!texture) { ++ew[3]; continue; }
            const int32_t tables = virtual_call<int32_t>(p, 0x2c0);
            bool any = false;
            for (int32_t i = 0; i < tables && i < 8; ++i) {
                uint8_t *t = rd<uint8_t *>(p, 0x240 + 8 * i);
                if (t) {
                    *reinterpret_cast<uint64_t *>(t + 8) = texture;
                    any = true;
                }
            }
            ++ew[any ? 1 : 2];
        }
    }
}

// Film bit streams {u32 *words, pos +8, cap +0xc}: read / write n bits at the position (first bit = most
// significant), then move on; past the capacity the game's assert fires, as in the game's own reader.
extern "C" U3X_FN uint32_t u3x_stream_read(uintptr_t base, uint8_t *bs, uint32_t n) {
    const uint32_t pos = rd<uint32_t>(bs, 8);
    const uint32_t r = u3x_bits_get(rd<const uint32_t *>(bs, 0), pos, n);
    *reinterpret_cast<uint32_t *>(bs + 8) = pos + n;
    if (rd<uint32_t>(bs, 0xc) < pos + n) u3h_rp_assert(base);
    return r;
}
extern "C" U3X_FN void u3x_stream_write(uintptr_t base, uint8_t *bs, uint32_t value, uint32_t n) {
    const uint32_t pos = rd<uint32_t>(bs, 8);
    u3x_bits_put(rd<uint32_t *>(bs, 0), pos, value, n);
    *reinterpret_cast<uint32_t *>(bs + 8) = pos + n;
    if (rd<uint32_t>(bs, 0xc) < pos + n) u3h_rp_assert(base);
}
// Bookmarks (0x30 bytes: stream {words, pos +8, cap +0xc, length +0x10}, time +0x18, stream position +0x1c, buffer
// +0x20, writer stamp +0x28). Moving one keeps the target's buffer and copies the bytes into it.
extern "C" U3X_FN void u3x_bookmark_move(uint8_t *d, const uint8_t *s) {
    *reinterpret_cast<uint32_t *>(d + 0x18) = rd<uint32_t>(s, 0x18);
    *reinterpret_cast<uint32_t *>(d + 0x1c) = rd<uint32_t>(s, 0x1c);
    *reinterpret_cast<uint64_t *>(d + 0x28) = rd<uint64_t>(s, 0x28);
    *reinterpret_cast<uint32_t *>(d + 8) = rd<uint32_t>(s, 8);
    *reinterpret_cast<uint32_t *>(d + 0xc) = rd<uint32_t>(s, 0xc);
    uint8_t *to = rd<uint8_t *>(d, 0x20);
    const uint8_t *from = rd<const uint8_t *>(s, 0x20);
    const uint32_t n = rd<uint32_t>(d, 0x10);
    for (uint32_t k = 0; k < n; ++k) to[k] = from[k];
}
// A new bookmark at time t: a 0x2800-byte buffer from heap 0x14 (kept for reuse), an empty stream over it, the
// writer's stamp and the stream position; then, with the bookmark as the record target (0x1b68840), the game
// writes the first player's full state into it, followed by a 32-bit zero.
extern "C" U3X_FN void u3x_bookmark_create(uintptr_t base, uint8_t *bm, int32_t t) {
    if (!rd<uint8_t *>(bm, 0x20)) {
        uint32_t heap = 0x14;
        *reinterpret_cast<uint8_t **>(bm + 0x20) = game_call<uint8_t *>(base + 0xd9e990, uint64_t{0x2800}, &heap, uint64_t{8});
    }
    *reinterpret_cast<uint32_t *>(bm + 0x10) = 0;
    *reinterpret_cast<uint8_t **>(bm) = rd<uint8_t *>(bm, 0x20);
    *reinterpret_cast<uint32_t *>(bm + 0xc) = 0x2800 * 8;
    *reinterpret_cast<uint32_t *>(bm + 8) = 0;
    *reinterpret_cast<uint64_t *>(bm + 0x28) = at<uint64_t>(base + 0x17c3c58);
    at<uint8_t *>(base + 0x1b68840) = bm;
    *reinterpret_cast<uint32_t *>(bm + 0x1c) = at<uint32_t>(base + 0x17c3838 + 8);
    *reinterpret_cast<int32_t *>(bm + 0x18) = t;
    const uint8_t *first = at<uint8_t>(base + 0x17d5c61) ? reinterpret_cast<const uint8_t *>(base + 0x17cb3f0) : nullptr;
    const uint32_t id = net_player_id(first);
    game_call<void>(base + 0x6b1b90, id);
    game_call<void>(base + 0x6b1dc0);
    game_call<void>(base + 0x7427d0, base + 0x17cb060);
    game_call<void>(base + 0x72e560, base + 0x17cb060, id);
    u3x_stream_write(base, bm, 0, 32);
    at<uint8_t *>(base + 0x1b68840) = nullptr;
}

// ===== v7.147: part 8 (anim command emission from ice snapshots, bound frames, join hold queue, received messages) =====

extern "C" {
U3H_IMPORT void u3h_v_pose(void *v, uint32_t f, uint8_t *anim, uint32_t i);
U3H_IMPORT void u3h_v_blend(void *v, uint32_t f, uint32_t a, uint32_t b, uint32_t c, uint32_t m);
U3H_IMPORT void u3h_v_ice(void *v, uint16_t type, uint32_t i, uint64_t p1, uint64_t p2, uint64_t p3);
U3H_IMPORT void u3h_v_bind(void *v, uint32_t i);
U3H_IMPORT void u3h_v_error(void *v, int code);
U3H_IMPORT uint8_t *u3h_net_art_group(uintptr_t base, uint32_t row, uint32_t col);
U3H_IMPORT uint8_t *u3h_anim_by_sid(uint8_t *group, uint32_t sid);
U3H_IMPORT int32_t u3h_skeleton(uint8_t *generator);
U3H_IMPORT int u3h_join_locked(uintptr_t base);
U3H_IMPORT int u3h_join_can_accept(uint8_t *ps, uint32_t state);
U3H_IMPORT uint64_t u3h_join_party_reply(uintptr_t base);
U3H_IMPORT void u3h_join_info_send(uintptr_t base, const uint8_t *id);
U3H_IMPORT void u3h_join_answer(uintptr_t base, const uint8_t *id, uint64_t party, uint8_t flag);
U3H_IMPORT uint8_t *u3h_join_request(void);
U3H_IMPORT uint8_t *u3h_join_request_on(void);
U3H_IMPORT uint8_t *u3h_join_request_pc(void);
U3H_IMPORT int32_t u3h_msg_event(uintptr_t base, uint8_t *id, uint32_t *type, uint8_t *data, uint32_t max);
U3H_IMPORT int u3h_join_host_answer(uintptr_t base, const uint8_t *id, const uint8_t *d, int32_t n);
U3H_IMPORT int u3h_join_msg_in(uintptr_t base, const uint8_t *id, uint32_t type, const uint8_t *d, int32_t n);
}

// CreateAnimCmds (kind 118): the generator's two current snapshots become anim commands. A snapshot whose ice copy
// (two at generator +8, 0x20 bytes: pending byte +0x18, time +0x1c) is finished is used as it is; otherwise it is
// decoded and iced again into the copy that holds neither snapshot time. Without a snapshot pair the generator's
// pose anim plays. A decode the game would have trapped on restores the command buffer and the ice copies and runs
// PS4's own code (returns 0). ec[84] runs, ec[85] fallbacks, ec[103] pose anims through the skeleton remap.
namespace {
// float == as the PPC fcmpu: false for NaN, +0 equals -0
U3X_FN bool float_equal(uint32_t a, uint32_t b) {
    int r;
    asm("movd %1, %%xmm0\n\tmovd %2, %%xmm1\n\tucomiss %%xmm1, %%xmm0\n\tsete %%al\n\tsetnp %%cl\n\tandb %%cl, %%al\n\tmovzbl %%al, %0"
        : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1", "rax", "rcx", "cc");
    return r;
}
uint8_t *ice_with_time(uint8_t *g, uint32_t t) {
    if (float_equal(rd<uint32_t>(g, 0x24), t)) return g + 8;
    if (float_equal(rd<uint32_t>(g, 0x44), t)) return g + 0x28;
    return nullptr;
}
// the ice copy that holds neither time, marked pending with time t; none when both hold one
U3X_FN uint8_t *free_ice(uint8_t *g, uint32_t t, uint32_t other) {
    const uint32_t t0 = rd<uint32_t>(g, 0x24), t1 = rd<uint32_t>(g, 0x44);
    int k = 0;
    if (float_equal(t0, t) || float_equal(t0, other)) {
        if (float_equal(t1, t) || float_equal(t1, other)) return nullptr;
        k = 1;
    }
    uint8_t *e = g + 8 + k * 0x20;
    e[0x18] = 1;
    *reinterpret_cast<uint32_t *>(e + 0x1c) = t;
    return e;
}
// one snapshot: 1 when its finished ice copy was used
U3X_FN int snapshot_commands(Visitor *v, uint8_t *g, uint8_t *s, const uint8_t *other, uint32_t i, int32_t a5, uint32_t group) {
    uint8_t *e = ice_with_time(g, rd<uint32_t>(s, 4));
    if (e && !e[0x18]) {
        u3h_v_ice(v, 0xc, i, rd<uint64_t>(e, 8), rd<uint64_t>(e, 0x10), rd<uint64_t>(e, 0));
        return 1;
    }
    u3h_decode(v, s, g, static_cast<int32_t>(i), a5);
    if (v->error) return 0;
    if (!e) {
        e = free_ice(g, rd<uint32_t>(s, 4), rd<uint32_t>(other, 4));
        if (!e) {
            u3h_v_error(v, 9);
            return 0;
        }
    } else if (group == 1) {
        e[0x18] = 0;
    }
    u3h_v_ice(v, 0xe, i, rd<uint64_t>(e, 8), rd<uint64_t>(e, 0x10), rd<uint64_t>(e, 0));
    return 0;
}
}  // namespace
extern "C" U3X_FN int u3x_anim_commands(uintptr_t base, const uint64_t *regs) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 512) return 0;
    uint8_t *g = reinterpret_cast<uint8_t *>(regs[4]), *cb = reinterpret_cast<uint8_t *>(regs[5]);
    const uint32_t group = static_cast<uint32_t>(regs[7]), first = static_cast<uint32_t>(regs[3]);
    if (!g || !cb || group > 1) return 0;   // group > 1: the game asserts
    ++ec[84];
    uint32_t blend;
    const int32_t idx = u3h_pick_snapshot(base, g, &blend);
    const uint32_t pos0 = rd<uint32_t>(cb, 0xc), max0 = rd<uint32_t>(cb, 0x10);
    uint8_t ice[0x40];
    for (int k = 0; k < 0x40; ++k) ice[k] = g[8 + k];
    Visitor v;
    v.base = base;
    v.kind = 1;
    v.error = 0;
    v.commands = cb;
    v.entries = nullptr;
    v.channel = 0;
    if (idx == -1) {
        uint8_t *group_ = u3h_net_art_group(base, rd<uint32_t>(g, 0x60), rd<uint32_t>(g, 0x2178));
        uint8_t *anim = group_ ? u3h_anim_by_sid(group_, rd<uint32_t>(g, 0x217c)) : nullptr;
        const int32_t skeleton = u3h_skeleton(g);
        bool done = false;
        if (anim) {
            if (rd<int32_t>(anim, 0x40) == skeleton) {
                u3h_v_pose(&v, 0, anim, first);
                done = true;
            } else {
                uint64_t o1 = 0, o2 = 0;
                if (static_cast<uint8_t>(u3h_call_f32(base + 0xdc4240, 0, reinterpret_cast<uint64_t>(anim), static_cast<uint32_t>(skeleton),
                                                       reinterpret_cast<uint64_t>(&o1), reinterpret_cast<uint64_t>(&o2)))) {
                    ++ec[103];
                    u3h_v_ice(&v, 0xc, first, o1, 0, o2);
                    done = true;
                }
            }
        }
        if (!done) u3h_v_bind(&v, first);
    } else {
        uint8_t *s0 = g + 0xb0 + idx * static_cast<intptr_t>(snapshot_bytes), *s1 = s0 + snapshot_bytes;
        snapshot_commands(&v, g, s0, s1, first, 0, group);
        if (!v.error) snapshot_commands(&v, g, s1, s0, first + 1, 1, group);
        if (!v.error) u3h_v_blend(&v, blend, first, first + 1, first, 1);
    }
    if (v.error) {
        *reinterpret_cast<uint32_t *>(cb + 0xc) = pos0;
        *reinterpret_cast<uint32_t *>(cb + 0x10) = max0;
        for (int k = 0; k < 0x40; ++k) g[8 + k] = ice[k];
        ++ec[85];
        if (v.error != 12) ++ec[81];
        return 0;
    }
    return 1;
}

// Bound frames (0x60 bytes: two 0x7fcccccc words, rigid-body handle +8, id +0x10, parent position +0x20 and
// rotation +0x30, local position +0x40 and rotation +0x50).
extern "C" U3X_FN void u3x_frame_make(uint8_t *bf, const uint8_t *local) {
    for (int k = 0; k < 0x60; ++k) bf[k] = 0;
    *reinterpret_cast<uint32_t *>(bf) = 0x7fccccccu;
    *reinterpret_cast<uint32_t *>(bf + 4) = 0x7fccccccu;
    *reinterpret_cast<uint32_t *>(bf + 0x3c) = 0x3f800000u;   // parent rotation w = 1
    for (int k = 0; k < 0x20; ++k) bf[0x40 + k] = local[k];
}
extern "C" U3X_FN void u3x_frame_identity(uint8_t *bf) {
    for (int k = 0; k < 0x60; ++k) bf[k] = 0;
    *reinterpret_cast<uint32_t *>(bf) = 0x7fccccccu;
    *reinterpret_cast<uint32_t *>(bf + 4) = 0x7fccccccu;
    *reinterpret_cast<uint32_t *>(bf + 0x3c) = 0x3f800000u;
    *reinterpret_cast<uint32_t *>(bf + 0x5c) = 0x3f800000u;
}
// the parent from the bound rigid body while the handle is still valid (same id, or its low 16 bits for small ids,
// and the body's slot index matching the handle's place in the handle table at *(0x16e2f30))
extern "C" U3X_FN void u3x_frame_parent(uintptr_t base, uint8_t *bf) {
    uint8_t **handle = rd<uint8_t **>(bf, 8);
    const uint32_t id = rd<uint32_t>(bf, 0x10);
    if (!handle || !id) return;
    const uint8_t *body = *handle;
    if (!body) return;
    const uint32_t body_id = rd<uint32_t>(body, 0x50);
    const bool same = body_id == id || (id <= 0xfffe && (body_id & 0xffff) == id);
    const uint32_t slot = static_cast<uint32_t>((reinterpret_cast<uintptr_t>(handle) - at<uintptr_t>(base + 0x16e2f30)) >> 3);
    if (same && rd<uint16_t>(body, 0x54) == slot)
        for (int k = 0; k < 0x20; ++k) bf[0x20 + k] = body[k];
}

// Join requests that cannot be answered yet are held (at most 4, one per sender) until the party can accept them,
// is locked, has gone back to the game (state 0: the request goes to the game's own queue), or 5 s pass. A fresh
// PC request (marker 0x4a, version 2) waits while the party is in state 6.
namespace {
struct HoldQueue {
    uint8_t id[4][0x24];
    uint8_t flag[4];
    uint8_t count;
    int32_t since[4];
    uint8_t data[4][0x10];
};
U3X_BSS HoldQueue held;
U3X_RO const char hold_from[] = "held join request from ", hold_ok[] = " accepted", hold_locked[] = " refused: locked",
                  hold_late[] = " refused after 5 s, state ", hold_game[] = " to the game (state 0)";
U3X_RO const char request_from[] = "join request from ", request_done[] = " answered: new party up";
int32_t engine_clock(uintptr_t base) { return at<int32_t>(base + 0x17b4a90 + 0x114); }
uint8_t *party_state(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17dc298); }
bool same_id(const uint8_t *a, const uint8_t *b) {
    for (int k = 0; k < 0x24; ++k)
        if (a[k] != b[k]) return false;
    return true;
}
}  // namespace
extern "C" U3X_FN void u3x_join_hold(uintptr_t base, const uint8_t *id, const uint8_t *d) {
    unsigned i = 0;
    while (i < held.count && !same_id(held.id[i], id)) ++i;
    if (i == held.count) {
        if (held.count >= 4) return;
        for (int k = 0; k < 0x24; ++k) held.id[i][k] = id[k];
        held.since[i] = engine_clock(base);
        ++held.count;
    }
    held.flag[i] = d[8];
    for (int k = 0; k < 0x10; ++k) held.data[i][k] = d[k];
}
extern "C" U3X_FN void u3x_join_hold_tick(uintptr_t base) {
    uint8_t *ps = party_state(base);
    const uint32_t state = rd<uint32_t>(ps, 0x1338);
    const int32_t now = engine_clock(base);
    for (unsigned i = 0; i < held.count;) {
        enum { wait, accept, locked, to_game, too_late } how = wait;
        const bool pc = held.data[i][9] == 0x4a, fresh = pc && held.data[i][10] == 2;
        if (u3h_join_locked(base)) how = locked;
        else if (fresh && state == 6u && now - held.since[i] <= 5 * 300) how = wait;
        else if (u3h_join_can_accept(ps, state)) how = accept;
        else if (state == 0u) how = to_game;
        else if (now - held.since[i] > 5 * 300) how = too_late;
        if (how == accept && pc && u3h_join_party_reply(base)) u3h_join_info_send(base, held.id[i]);
        if (how == to_game) *u3h_join_request_pc() = static_cast<uint8_t>(pc);
        if (how == wait) {
            ++i;
            continue;
        }
        const uint8_t *name = held.id[i];
        if (how == accept) {
            u3h_join_answer(base, held.id[i], u3h_join_party_reply(base), held.flag[i]);
            u3h_fj_note(hold_from, name, hold_ok, -1);
        } else if (how == to_game) {
            u3x_queue_post(base, held.id[i], 0x10bu, held.data[i], 0x10);
            u3h_fj_note(hold_from, name, hold_game, -1);
        } else {
            u3h_join_answer(base, held.id[i], 0, held.flag[i]);
            u3h_fj_note(hold_from, name, how == locked ? hold_locked : hold_late, how == locked ? -1 : static_cast<int32_t>(state));
        }
        for (unsigned k = i; k + 1 < held.count; ++k) {
            for (int j = 0; j < 0x24; ++j) held.id[k][j] = held.id[k + 1][j];
            held.flag[k] = held.flag[k + 1];
            held.since[k] = held.since[k + 1];
            for (int j = 0; j < 0x10; ++j) held.data[k][j] = held.data[k + 1][j];
        }
        --held.count;
    }
}
// Once a party created for a join request is up (state 6 with a party id), the requester gets 0x10c {party id}
// (the game clears the request without sending it); a PC requester first gets the host's 0x1f0 info.
extern "C" U3X_FN void u3x_join_reply_tick(uintptr_t base) {
    uint8_t *ps = party_state(base);
    if (held.count) u3x_join_hold_tick(base);
    uint8_t *request = u3h_join_request(), &pending = *u3h_join_request_on();
    if (ps[0x1348]) {
        for (int k = 0; k < 0x24; ++k) request[k] = ps[0x1349 + k];
        pending = 1;
        return;
    }
    if (!pending) return;
    pending = 0;
    if (rd<uint32_t>(ps, 0x1338) != 6u || !rd<uint64_t>(ps, 0x18) || !request[0]) return;
    uint8_t &pc = *u3h_join_request_pc();
    if (pc) {
        pc = 0;
        u3h_join_info_send(base, request);
    }
    uint8_t reply[0x10];
    for (uint8_t &b : reply) b = 0;
    *reinterpret_cast<uint64_t *>(reply) = rd<uint64_t>(ps, 0x18);
    u3x_queue_send(base, request, 0x10cu, reply, 0x10);
    u3h_fj_note(request_from, request, request_done, -1);
}
// Received messages (up to 64 per call): join requests and the PC join messages are handled by the helper, the rest
// goes to the game's message queue.
extern "C" U3X_FN void u3x_message_events(uintptr_t base) {
    for (int k = 0; k < 64; ++k) {
        uint8_t id[0x24], d[0x200];
        uint32_t type = 0;
        for (uint8_t &b : id) b = 0;
        int32_t n = u3h_msg_event(base, id, &type, d, 0x1fc);
        if (n < 0) return;
        if (n > 0x1fc) n = 0x1fc;
        if (type == 0x10bu && n >= 9 && u3h_join_host_answer(base, id, d, n)) continue;
        if ((type == 0x1f0u || type == 0x1f1u) && u3h_join_msg_in(base, id, type, d, n)) continue;
        u3x_queue_post(base, id, type, d, static_cast<uint32_t>(n));
    }
}

// ===== v7.148: part 9 (online profile fetch, session hello, server sign-in) =====

extern "C" {
U3H_IMPORT void *u3h_rc(void *m, int field);
U3H_IMPORT uint64_t u3h_net_connect(uint32_t ip, uint32_t port);
U3H_IMPORT int u3h_net_sendall(uint64_t f, const uint8_t *p, int n);
U3H_IMPORT int u3h_net_recv(uint64_t f, void *b, int n, int flags);
U3H_IMPORT void u3h_net_close(uint64_t f);
U3H_IMPORT int u3h_net_error(void);
U3H_IMPORT void u3h_net_nonblock(uint64_t f, uint32_t on);
U3H_IMPORT void u3h_utc(uint16_t *st);
U3H_IMPORT int u3h_unseal(uint8_t *record);
U3H_IMPORT const char *u3h_profile_host(void);
U3H_IMPORT int u3h_sm_recv(void *m, uint64_t f, uint8_t *b, int want, int all);
U3H_IMPORT uint32_t u3h_dns(void);
U3H_IMPORT void u3h_sleep_ms(uint32_t ms);
U3H_IMPORT int u3h_resolve(const char *host, uint32_t *ip);
U3H_IMPORT uint32_t u3h_parse_ipv4(const char *text);
U3H_IMPORT int u3h_hello(void *m, uint64_t f, const char *name);
U3H_IMPORT int u3h_xchg(void *m, uint64_t f, const uint8_t *d, uint32_t n, uint8_t *o, uint32_t cap);
U3H_IMPORT void u3h_so_note(const char *a, const char *name, int32_t v, const char *b);
U3H_IMPORT void u3h_pf_fetch(void *m, uint32_t ip);
U3H_IMPORT void u3h_pf_note(void *m, const char *why);
}

// The online backend's memory (RcMem) stays in the C part; its fields are reached through u3h_rc.
namespace {
enum RcField {
    rc_me, rc_pf_rx, rc_pf_rx_len, rc_pf_rec, rc_pf_state, rc_pf_http, rc_pf_body, rc_pf_dec,
    rc_sm_ip, rc_sm_port, rc_sm_state, rc_sm_fd, rc_sm_sh, rc_sm_txlen, rc_sm_tries, rc_sm_next_ping, rc_sm_hs, rc_sm_dirty,
    rc_ticket_len, rc_ticket, rc_wbuf, rc_tk_rx, rc_ban_len, rc_ban_rx, rc_n18_state, rc_srv_host, rc_srv_port,
    rc_key_ok = 30, rc_key, rc_lookup_state, rc_lookup_tries, rc_ticket_ok, rc_report_ip, rc_report_port
};
template <typename T> T &rc(void *m, RcField f) { return *static_cast<T *>(u3h_rc(m, f)); }
template <typename T> T *rcp(void *m, RcField f) { return static_cast<T *>(u3h_rc(m, f)); }
constexpr uint64_t bad_socket = ~uint64_t{0};
void put_be32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
uint32_t &ext_room9() { return *reinterpret_cast<uint32_t *>(u3h_ext() + 0xbc0 + 9 * 4); }
// appends text, returns the new length
U3X_FN int append_text(uint8_t *q, int n, const char *t) {
    while (*t) q[n++] = static_cast<uint8_t>(*t++);
    return n;
}
U3X_FN int put_2digits(uint8_t *q, int n, uint32_t v) {
    q[n++] = static_cast<uint8_t>('0' + v / 10);
    q[n++] = static_cast<uint8_t>('0' + v % 10);
    return n;
}
U3X_RO const char http_get[] = "GET /profiles/", http_path[] = "/profile.86 HTTP/1.1\r\nHost: ", http_date[] = "\r\nDate: ",
                  http_gmt[] = " GMT", http_end[] = "\r\nConnection: close\r\nUser-Agent: DNTG-HTTPC/1.1\r\n\r\n";
}  // namespace

// The online profile: HTTP GET profiles/<account>/profile.86 with the headers PS3's HTTP client sends (the Date as
// RFC 1123 in UTC; the server checks the closing headers), the reply read to the end; status, Content-Length and a
// chunked Transfer-Encoding are understood. The body is ND's LZ stream of the sealed record: decoded, its version
// checked, unsealed, and its legacy level, selected loadout and XP range-checked. pf_state: 1 ready, 2 no profile
// (404), 3 failed; ec[105] the step reached (6 = ready), ec[106..114] sizes, status, decoded size and key fields.
extern "C" U3X_FN void u3x_profile_fetch(void *m, uint32_t ip) {
    uint32_t *ec = ext_ec();
    volatile uint32_t &state = rc<volatile uint32_t>(m, rc_pf_state);
    state = 0;
    rc<uint32_t>(m, rc_pf_rx_len) = 0;
    rc<uint32_t>(m, rc_pf_http) = 0;
    rc<uint32_t>(m, rc_pf_body) = 0;
    rc<uint32_t>(m, rc_pf_dec) = 0;
    for (int i = 105; i <= 114; ++i) ec[i] = 0;
    if (!ip) { ec[105] = 1; state = 3; return; }
    ec[105] = 2;
    const uint64_t f = u3h_net_connect(ip, 80);
    if (f == bad_socket) { ec[105] = 3; state = 3; return; }
    const char *me = rcp<const char>(m, rc_me);
    uint8_t q[320];
    int n = append_text(q, 0, http_get);
    for (int i = 0; me[i] && i < 16; ++i) q[n++] = static_cast<uint8_t>(me[i]);
    n = append_text(q, n, http_path);
    n = append_text(q, n, u3h_profile_host());
    {
        uint16_t st[8];   // year, month, weekday, day, hour, minute, second, ms
        for (uint16_t &w : st) w = 0;
        u3h_utc(st);
        n = append_text(q, n, http_date);
        for (int i = 0; i < 3; ++i) q[n++] = static_cast<uint8_t>(day_names[(st[2] % 7) * 3 + i]);
        q[n++] = ',';
        q[n++] = ' ';
        n = put_2digits(q, n, st[3]);
        q[n++] = ' ';
        for (int i = 0; i < 3; ++i) q[n++] = static_cast<uint8_t>(month_names[((st[1] + 11) % 12) * 3 + i]);
        q[n++] = ' ';
        const uint32_t y = st[0];
        q[n++] = static_cast<uint8_t>('0' + y / 1000 % 10);
        q[n++] = static_cast<uint8_t>('0' + y / 100 % 10);
        q[n++] = static_cast<uint8_t>('0' + y / 10 % 10);
        q[n++] = static_cast<uint8_t>('0' + y % 10);
        q[n++] = ' ';
        n = put_2digits(q, n, st[4]);
        q[n++] = ':';
        n = put_2digits(q, n, st[5]);
        q[n++] = ':';
        n = put_2digits(q, n, st[6]);
        n = append_text(q, n, http_gmt);
    }
    n = append_text(q, n, http_end);
    if (!u3h_net_sendall(f, q, n)) { u3h_net_close(f); ec[105] = 4; state = 3; return; }
    ec[105] = 7;   // sent; stays 7 when no reply comes
    uint8_t *r = rcp<uint8_t>(m, rc_pf_rx);
    constexpr uint32_t cap = 0x3000;
    uint32_t got = 0;
    for (;;) {
        const int k = u3h_net_recv(f, r + got, static_cast<int>(cap - 1 - got), 0);
        if (k <= 0) {
            if (k < 0) ec[67] = static_cast<uint32_t>(u3h_net_error());
            break;
        }
        got += static_cast<uint32_t>(k);
        if (got >= cap - 1) break;
    }
    u3h_net_close(f);
    rc<uint32_t>(m, rc_pf_rx_len) = got;
    r[got] = 0;
    ec[106] = got;
    uint32_t status = 0, i = 0;   // "HTTP/1.1 200 ..."
    while (i < got && r[i] != ' ') ++i;
    ++i;
    while (i < got && r[i] >= '0' && r[i] <= '9') status = status * 10 + (r[i++] - '0');
    rc<uint32_t>(m, rc_pf_http) = status;
    ec[107] = status;
    uint32_t header_end = 0, length = 0, chunked = 0;
    for (i = 0; i + 3 < got; ++i)
        if (r[i] == '\r' && r[i + 1] == '\n' && r[i + 2] == '\r' && r[i + 3] == '\n') { header_end = i + 4; break; }
    for (i = 0; i + 16 < header_end; ++i) {
        // Content-Length: (the digits add up over every match, as the original)
        if ((r[i] == 'C' || r[i] == 'c') && (r[i + 8] == 'L' || r[i + 8] == 'l') && r[i + 7] == '-' && r[i + 14] == ':') {
            uint32_t j = i + 15;
            while (r[j] == ' ') ++j;
            while (r[j] >= '0' && r[j] <= '9') length = length * 10 + (r[j++] - '0');
        }
        // Transfer-Encoding: ... chunked
        if ((r[i] == 'T' || r[i] == 't') && r[i + 8] == '-' && r[i + 17] == ':' && (r[i + 9] == 'E' || r[i + 9] == 'e'))
            for (uint32_t j = i + 18; j < header_end; ++j)
                if (r[j] == 'c' && r[j + 1] == 'h' && r[j + 2] == 'u') chunked = 1;
    }
    ec[114] = chunked;
    uint8_t *body = r + header_end;
    uint32_t size = header_end ? got - header_end : 0;
    if (chunked) {   // hex size line, CRLF, data, CRLF; joined in place
        uint32_t out = 0, p = 0;
        while (p < size) {
            uint32_t chunk = 0;
            while (p < size && body[p] != '\r') {
                const uint8_t c = body[p++];
                chunk = chunk * 16 + (c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
            }
            p += 2;
            if (!chunk || p + chunk > size) break;
            for (uint32_t k = 0; k < chunk; ++k) body[out++] = body[p + k];
            p += chunk + 2;
        }
        size = out;
    } else if (length && length < size) {
        size = length;
    }
    rc<uint32_t>(m, rc_pf_body) = size;
    ec[108] = size;
    if (status == 404) { state = 2; return; }
    if (status != 200 || !size) { state = 3; return; }
    uint8_t *record = rcp<uint8_t>(m, rc_pf_rec);
    const uint32_t decoded = u3x_lz_decode(body, size, record, 0x2828);
    rc<uint32_t>(m, rc_pf_dec) = decoded;
    ec[109] = decoded;
    if (decoded != 0x2828 || be32(record) != 0x56) { ec[105] = 5; state = 3; return; }
    if (!u3h_unseal(record)) { ec[105] = 8; state = 3; return; }   // a bad MAC: the record is refused
    if (be32(record + 0x1618) > 5u || be32(record + 0xf8) > 4u || be32(record + 0x500) >= 0x10000000u) { ec[105] = 9; state = 3; return; }
    ec[110] = be32(record + 0x4fc);
    ec[111] = be32(record + 0x500);
    ec[112] = be32(record + 0x504);
    ec[113] = be32(record + 0x1618);
    ec[105] = 6;
    __atomic_store_n(&rc<uint32_t>(m, rc_pf_state), 1u, __ATOMIC_RELEASE);
}

// The session server hello: 301 {0, the player's id}, the 302 reply carries a challenge, answered with 326 {the
// session answer}; then the socket goes non-blocking to the game thread (sm_state 1). A live socket from an earlier
// press is closed first. sm_state 2 = failed (ec[87] says where: 7 no session key, 2 connect, 3 send, 4 short reply,
// 5 not 302, 6 answer send; 1 = ok), ec[86] the step, ec[88..92] reply size, type, challenge, answer, hellos.
extern "C" U3X_FN void u3x_session(void *m) {
    uint32_t *ec = ext_ec();
    const uint32_t ip = rc<uint32_t>(m, rc_sm_ip), port = rc<uint32_t>(m, rc_sm_port);
    volatile int32_t &state = rc<volatile int32_t>(m, rc_sm_state);
    uint64_t &fd = rc<uint64_t>(m, rc_sm_fd);
    if (state == 1) {
        state = 0;
        u3h_net_close(fd);
        fd = 0;
    }
    state = 3;
    for (int i = 86; i < 92; ++i) ec[i] = 0;
    uint8_t *reply = rcp<uint8_t>(m, rc_sm_sh);
    for (int k = 0; k < 16; ++k) reply[k] = 0;
    if (!rc<uint8_t>(m, rc_key_ok)) { ec[87] = 7; state = 2; return; }
    ec[86] = 1;
    const uint64_t f = u3h_net_connect(ip, port);
    if (f == bad_socket) { ec[95] = ec[67]; ec[87] = 2; state = 2; return; }
    uint8_t h[48];
    for (uint8_t &b : h) b = 0;
    put_be32(h, 301);
    const char *me = rcp<const char>(m, rc_me);
    for (int i = 0; i < 16 && me[i]; ++i) h[8 + i] = static_cast<uint8_t>(me[i]);
    if (!u3h_net_sendall(f, h, 48)) { ec[95] = ec[67]; ec[87] = 3; u3h_net_close(f); state = 2; return; }
    ec[86] = 2;
    const int got = u3h_sm_recv(m, f, reply, 16, 1);
    ec[88] = static_cast<uint32_t>(got);
    ec[89] = got >= 4 ? be32(reply) : 0;
    if (got < 16) { ec[87] = 4; u3h_net_close(f); state = 2; return; }
    if (ec[89] != 302u) { ec[87] = 5; u3h_net_close(f); state = 2; return; }
    ec[86] = 3;
    const uint32_t challenge = be32(reply + 8);
    ec[90] = challenge;
    const uint32_t answer = u3x_session_answer(rcp<const uint8_t>(m, rc_key), challenge, h + 8);
    ec[91] = answer;
    uint8_t h2[8];
    put_be32(h2, 326);
    put_be32(h2 + 4, answer);
    if (!u3h_net_sendall(f, h2, 8)) { ec[95] = ec[67]; ec[87] = 6; u3h_net_close(f); state = 2; return; }
    ec[86] = 4;
    ec[87] = 1;
    u3h_net_nonblock(f, 1);   // from here the game thread owns the socket
    fd = f;
    rc<uint32_t>(m, rc_sm_txlen) = 0;
    rc<uint32_t>(m, rc_sm_tries) = 0;
    rc<int32_t>(m, rc_sm_next_ping) = 0;
    uint32_t &hellos = rc<uint32_t>(m, rc_sm_hs);
    ++hellos;
    ec[92] = hellos;
    rc<volatile uint8_t>(m, rc_sm_dirty) = 1;
    __atomic_store_n(&rc<int32_t>(m, rc_sm_state), 1, __ATOMIC_RELEASE);
    ec[86] = 100;
}

// Server sign-in (the online NetInit worker's order): the server address (own DNS lookup, up to 25 s for it and the
// server table), the ticket-server hello and the framed ticket, the report-server's is-banned query (its failure is
// no login failure), the online profile, the session hello. 1 = signed in. ec[65] the step reached (100 done),
// ec[66] the failure (8 no session key, 2 no address, 3 connect, 4 hello not sent, 5 hello refused, 6 ticket too
// long), ec[68] server table (1) or defaults (2), ec[70]/[71]/[85] addresses and ports, ec[76..81] sizes and replies.
namespace {
U3X_RO const char ticket_server[] = "ticket-server", report_server[] = "report-server", is_banned[] = "is-banned ",
                  sign_in_ok[] = "server sign-in accepted (ticket-server reply byte ",
                  sign_in_bad[] = "server sign-in refused: the ticket-server's reply was not valid", close_paren[] = ")",
                  at_sign_in[] = "at sign-in";
uint32_t le32(const uint8_t *p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }
}  // namespace
extern "C" U3X_FN int u3x_sign_in(void *m) {
    uint32_t *ec = ext_ec();
    ++ec[83];
    ec[65] = 1;
    ec[66] = 0;
    ec[79] = 0;
    ec[84] = rc<uint32_t>(m, rc_ticket_len);
    if (!rc<uint8_t>(m, rc_key_ok)) { ec[66] = 8; return 0; }
    uint32_t &lookup = rc<uint32_t>(m, rc_lookup_state);
    uint32_t &room9 = ext_room9();
    if (lookup == 11 || lookup == 12) {   // the last check found no address: look it up again
        rc<uint32_t>(m, rc_lookup_tries) = 0;
        lookup = 1;
    }
    if (lookup != 10) {
        const uint32_t a = u3h_dns();
        if (a) { room9 = a; lookup = 10; }
    }
    volatile uint32_t *table_state = rcp<volatile uint32_t>(m, rc_n18_state);
    for (int w = 0; w < 250 && ((lookup != 10 && lookup != 0) || !__atomic_load_n(table_state, __ATOMIC_ACQUIRE)); ++w) {
        if (lookup >= 11) break;
        u3h_sleep_ms(100);
    }
    if (!room9 || lookup != 10) { ec[66] = 2; return 0; }
    uint32_t ip[3], port[3];
    if (__atomic_load_n(table_state, __ATOMIC_ACQUIRE) == 1) {
        ec[68] = 1;
        char (*hosts)[64] = rcp<char[64]>(m, rc_srv_host);
        const uint32_t *ports = rcp<const uint32_t>(m, rc_srv_port);
        for (int k = 0; k < 3; ++k) {
            ip[k] = u3h_parse_ipv4(hosts[k]);
            if (!ip[k]) u3h_resolve(hosts[k], &ip[k]);
            if (!ip[k]) ip[k] = room9;
            port[k] = ports[k];
        }
    } else {
        ec[68] = 2;
        for (int k = 0; k < 3; ++k) {
            ip[k] = room9;
            port[k] = k == 2 ? 6303u : 6300u;
        }
    }
    ec[70] = ip[0];
    ec[71] = port[0];
    ec[85] = port[2];
    // ticket-server
    ec[65] = 3;
    uint64_t f = u3h_net_connect(ip[0], port[0]);
    if (f == bad_socket) { ec[66] = 3; return 0; }
    bool ok = false;
    uint8_t &ticket_ok = rc<uint8_t>(m, rc_ticket_ok);
    uint8_t *tk_rx = rcp<uint8_t>(m, rc_tk_rx);
    ec[65] = 4;
    const int hr = u3h_hello(m, f, ticket_server);
    if (hr < 0) {
        ec[66] = 4;
    } else if (!hr) {
        ec[66] = 5;
    } else {
        ec[65] = 6;
        const uint32_t tl = rc<uint32_t>(m, rc_ticket_len);
        uint8_t *o = rcp<uint8_t>(m, rc_wbuf);
        const uint8_t *ticket = rcp<const uint8_t>(m, rc_ticket);
        o[0] = static_cast<uint8_t>(tl >> 8);
        o[1] = static_cast<uint8_t>(tl);
        for (uint32_t i = 0; i < tl; ++i) o[2 + i] = ticket[i];
        uint8_t tq[0x200];
        if (tl + 2 > sizeof tq) {
            ec[66] = 6;
        } else {
            for (uint32_t i = 0; i < tl + 2; ++i) tq[i] = o[i];
            ec[76] = tl + 2;
            ec[65] = 7;
            for (int k = 0; k < 16; ++k) tk_rx[k] = 0;
            const int got = u3h_xchg(m, f, tq, tl + 2, tk_rx, 16);
            if (got < 0) {   // not taken: logged, the sign-in goes on
                ec[77] = 0;
                ticket_ok = 2;
            } else {
                ec[77] = static_cast<uint32_t>(got);
                ec[78] = le32(tk_rx);
                ticket_ok = 1;
            }
            ok = true;
        }
    }
    u3h_net_close(f);
    if (ticket_ok == 1) u3h_so_note(sign_in_ok, nullptr, static_cast<int32_t>(tk_rx[0]), close_paren);
    else if (ticket_ok == 2) u3h_so_note(sign_in_bad, nullptr, -1, nullptr);
    if (!ok) return 0;
    // report-server: is-banned
    rc<uint32_t>(m, rc_report_ip) = ip[1];
    rc<uint32_t>(m, rc_report_port) = port[1];
    ec[65] = 8;
    rc<uint32_t>(m, rc_ban_len) = 0;
    f = u3h_net_connect(ip[1], port[1]);
    if (f == bad_socket) {
        ec[79] = 1;
    } else {
        ec[65] = 9;
        if (u3h_hello(m, f, report_server) <= 0) {
            ec[79] = 2;
        } else {
            const char *me = rcp<const char>(m, rc_me);
            uint8_t q[64];
            int n = append_text(q, 0, is_banned);
            for (int i = 0; me[i] && n < 60; ++i) q[n++] = static_cast<uint8_t>(me[i]);
            q[n++] = '\n';
            ec[65] = 10;
            ec[79] = 3;
            ec[65] = 11;
            char *ban = rcp<char>(m, rc_ban_rx);
            for (int k = 0; k < 0x100; ++k) ban[k] = 0;
            const int got = u3h_xchg(m, f, q, static_cast<uint32_t>(n), reinterpret_cast<uint8_t *>(ban), 0xff);
            if (got > 0) {
                rc<uint32_t>(m, rc_ban_len) = static_cast<uint32_t>(got < 0xff ? got : 0xff);
                ec[79] = 4;
                ec[80] = static_cast<uint32_t>(got);
                ec[81] = static_cast<uint8_t>(ban[0]);
            } else {
                ec[80] = 0;
            }
        }
        u3h_net_close(f);
    }
    u3h_pf_fetch(m, room9);   // the online profile, before the session hello
    u3h_pf_note(m, at_sign_in);
    rc<uint32_t>(m, rc_sm_ip) = ip[2];
    rc<uint32_t>(m, rc_sm_port) = port[2];
    u3x_session(m);
    ec[65] = 100;
    ec[66] = 1;
    return 1;
}

// ===== v7.149: part 10 (cinema pages and the film worker, profile list ops, lobby row actions) =====

extern "C" {
U3H_IMPORT void u3h_cin_netinit(uintptr_t base, void *s);
U3H_IMPORT void u3h_j07_rec(uintptr_t base);
U3H_IMPORT void u3h_cin_scan_free(uintptr_t base, void *s, uint8_t *st);
U3H_IMPORT void u3h_j07_zeroed(void);
U3H_IMPORT int u3h_lan_party_size(uintptr_t base);
U3H_IMPORT const uint32_t *u3h_lobby_rows(uintptr_t base, uint32_t *n);
U3H_IMPORT void u3h_lnb_find_party(uintptr_t base);
U3H_IMPORT void u3h_splitscreen(uintptr_t base, void *s);
}

// The Cinema pages (U3MP_CINEMA). ExtState +0x3230 (cin): [0] set up, [1] prepare calls, [2] recordings skipped;
// eb[20]/[21] record opens / headers written, eb[22] worker done, eb[23] the page after the progress page, eb[25]
// list toggles. The local/online list flag is the byte at 0x16e7168 (1 = local films).
namespace {
uint32_t *ext_cin() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x3230); }
uint8_t &films_local(uintptr_t base) { return at<uint8_t>(base + 0x16e7168); }
void cinema_ready(uintptr_t base, void *s) {
    if (!ext_cin()[0]) u3h_cin_netinit(base, s);
}
// the film list refresh: done = 0, scan the local films (the online file list is never fetched in LAN play), done = 1
U3X_FN void film_worker(uintptr_t base, void *s, int param) {
    ext_eb()[22] = 0;
    if (param == 0) u3h_cin_scan(base, s, u3h_cin_state(base));
    ext_eb()[22] = 1;
}
// the progress page, then the worker (which runs at once here); the progress page moves on to target when done
U3X_FN void film_progress(uintptr_t base, void *s, uint32_t target, int param) {
    ext_eb()[22] = 0;
    ext_eb()[23] = target;
    u3h_show(base, s, 0x1d84d11eu, 0);
    film_worker(base, s, param);
}
}  // namespace
// kind 70, the film writer's Open: in record mode free a slot (at 20 films the oldest unprotected goes) and, unless
// 20 remain, write the record header. Returns 0: the game's Open runs.
extern "C" U3X_FN int u3x_film_open(uintptr_t base, void *s, const uint64_t *r) {
    uint8_t *st = reinterpret_cast<uint8_t *>(r[4]);
    ++ext_eb()[20];
    if (!cinema_on() || st != u3h_cin_state(base) || static_cast<uint8_t>(r[5]) != 0) return 0;
    cinema_ready(base, s);
    u3h_j07_rec(base);
    u3h_cin_scan_free(base, s, st);
    if (film_count(st) != 20) {
        u3x_film_record_header(base, st);
        ++ext_eb()[21];
    }
    return 0;
}
// kind 54, recording start: skipped (returns 1) when the port is off, the stream has no buffer or the film directory
// does not open; otherwise the stream buffer starts zeroed (eb[1] bit 15 keeps it).
extern "C" U3X_FN int u3x_film_prepare(uintptr_t base, void *s) {
    uint32_t *cin = ext_cin();
    ++cin[1];
    if (!cinema_on()) { ++cin[2]; return 1; }
    uint8_t *st = u3h_cin_state(base);
    cinema_ready(base, s);
    uint8_t *buffer = at<uint8_t *>(base + 0x17c3838);
    if (!buffer) { ++cin[2]; return 1; }
    const int fd = static_cast<int>(file_open(s, film_dir(st), 0x20000, 0));
    if (fd < 0) { ++cin[2]; return 1; }
    file_close(s, fd);
    if (!(ext_eb()[1] & 0x8000u)) {
        for (uint32_t k = 0; k < 0x20000u; ++k) buffer[k] = 0;
        u3h_j07_zeroed();
    }
    return 0;
}
// kind 71, the delete confirmation: the selected film goes first, then the game's own tail runs (returns 0)
extern "C" U3X_FN int u3x_film_delete_hook(uintptr_t base, void *s) {
    if (cinema_on()) u3x_film_delete(base, s);
    return 0;
}
// kind 72, the progress page handler: event 6 once the worker is done goes on to its page (0 = back) and then shows
// "no films" (local list empty) or "no online files" (and back to the local list); events 2..4 refresh the buttons.
extern "C" U3X_FN int u3x_film_progress_page(uintptr_t base, void *s, const uint64_t *r) {
    const int32_t *ev = reinterpret_cast<const int32_t *>(r[4]);
    if (!ev || !cinema_on()) return 1;
    uint8_t *st = u3h_cin_state(base);
    if (ev[0] == 6) {
        uint32_t *eb = ext_eb();
        if (!eb[22]) return 1;
        if (eb[23] == 0) game_call<void>(base + 0x652200, reinterpret_cast<void *>(base + 0x17bd080), 0);
        else u3h_show(base, s, eb[23], 0);
        if (!films_local(base)) {
            if (rd<int32_t>(st, 0x1e58) == 0) {
                u3h_show_dialog(base, s, 0x5d425a5bu, 0xc6467f2du, 0, 0, 0, 0xe1e040aau);
                films_local(base) = 1;
            }
        } else if (film_count(st) == 0) {
            u3h_show_dialog(base, s, 0x2760b1fdu, 0xc6467f2du, 0, 0, 0, 0xe1e040aau);
        }
    } else if (static_cast<uint32_t>(ev[0]) - 2u < 3u) {
        u3h_cin_flags(st);
    }
    return 1;
}
// kind 73, the local / online toggle button
extern "C" U3X_FN int u3x_film_toggle_list(uintptr_t base, void *s) {
    if (!cinema_on()) return 1;
    const uint8_t old = films_local(base);
    films_local(base) = old ^ 1;
    ++ext_eb()[25];
    film_progress(base, s, 0, old);
    return 1;
}
// The lobby's Cinema row: not with a party of more than one; net menu mode 4, the party session locked when we may,
// the films scanned, no net game selected, film 0 selected, then the progress page on to the film list.
extern "C" U3X_FN void u3x_cinema(uintptr_t base, void *s) {
    if (u3h_lan_party_size(base) > 1) {
        u3h_show_dialog(base, s, 0x06718e75u, 0xc6467f2du, 0x3a586923u, 0, 0, 0xe1e040aau);
        return;
    }
    const uintptr_t select_net_game = state_native(s, 2);
    if (!select_net_game) {
        state_lobby_error(s) = 5;
        return;
    }
    at<uint32_t>(base + 0x17d603c) = 4;
    uint8_t *ps = reinterpret_cast<uint8_t *>(base + 0x17dc298);
    uint8_t *backend = rd<uint8_t *>(ps, 8);
    if (backend && (!rd<uint64_t>(ps, 0x18) || rd<uint32_t>(ps, 0x12e4) == rd<uint32_t>(ps, 0x12e8))) {
        const uintptr_t lock = (*reinterpret_cast<uintptr_t **>(backend))[0x40 / 8];
        if (lock) reinterpret_cast<int64_t (*)(void *, void *, int)>(lock)(backend, ps, 1);
    }
    if (cinema_on()) {
        cinema_ready(base, s);
        u3h_cin_scan(base, s, u3h_cin_state(base));
    }
    reinterpret_cast<void (*)(void *, int, void *, uint32_t)>(select_net_game)(reinterpret_cast<void *>(base + 0x17cb060), -1, nullptr, 0xc);
    if (cinema_on()) u3x_film_select(base, s, 0);
    film_progress(base, s, 0x735c4dbeu, 1);
}

// The LAN profile list (PAGE_LAN_CHOOSE_PROFILE): row text = the profile's name, selection, count. The game's own
// "highlighted profile" word 0x1b6a400 follows the selection (it decides whether Delete Profile shows) unless ea[0]
// bit 1 is set. Returns 0 for ops the game answers.
extern "C" U3X_FN int u3x_profile_list_op(uintptr_t base, int32_t *ev, void *s) {
    const uint32_t count = profile_count(s);
    uint32_t &selected = *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 132);
    const char *names = reinterpret_cast<const char *>(static_cast<uint8_t *>(s) + 136);
    const bool follow = !(ext_ea()[0] & 2);
    uint32_t &game_selected = at<uint32_t>(base + 0x1b6a400);
    const uint32_t index = static_cast<uint32_t>(ev[1]);
    switch (ev[0]) {
    case 2:
        if (index && index <= count && ev[0x15] == 0) {
            set_row_text(ev, names + (index - 1) * 64);
            return 1;
        }
        return 0;
    case 5:
        ev[1] = static_cast<int32_t>(selected);
        if (follow && selected <= count) game_selected = selected;
        return 1;
    case 6: ev[1] = static_cast<int32_t>(count); return 1;
    case 10:
        if (index <= count) selected = index;
        if (follow) game_selected = index;
        return 1;
    case 23:
        if (selected > count) selected = 0;
        if (follow && game_selected > count) game_selected = 0;
        return 1;
    default: return 0;
    }
}
// A row of the lobby menu (LIST_MAIN_MENU) was pressed: the row number + 1 is kept in state +84, then its action.
extern "C" U3X_FN void u3x_lobby_row(uintptr_t base, const int32_t *ev, void *s) {
    uint32_t n;
    const uint32_t *rows = u3h_lobby_rows(base, &n);   // {text, icon, action}
    const uint32_t row = static_cast<uint32_t>(ev[1]);
    if (row >= n) return;
    *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 84) = row + 1;
    const uint32_t action = rows[row * 3 + 2];
    const uintptr_t button = state_native(s, 14);
    switch (action) {
    case 0x7d593924u: u3x_custom_game(base, s); break;
    case 0x52f3d9f6u:   // LAN Search, then the LAN Parties page
        u3h_lnb_find_party(base);
        if (state_show_page(s)) u3h_show(base, s, 0xbb3595bc, 0);
        break;
    case 0xc4acc827u:
    case 0xe7d7dbbbu:
        if (button) reinterpret_cast<void (*)(void *, uint32_t, int)>(button)(reinterpret_cast<void *>(base + 0x17bd080), action, 0);
        break;
    case 0xb85cb12au: u3x_cinema(base, s); break;
    case 0xcd127c29u: u3h_splitscreen(base, s); break;
    default: u3x_lobby_action(base, s, action); break;
    }
}

// ===== v7.150: part 11 (the LAN backend's socket class and its byte stream) =====

extern "C" {
U3H_IMPORT void u3h_net_pick(void);
}

// The LAN backend's sockets {fd, ip, port} over the libSceNet imports at ExtState +0x200 (socket, close, bind,
// sendto, recvfrom, setsockopt, netctl info, connect, listen, accept, send, recv). The last error goes to eb[91].
namespace {
enum NetFn { net_socket, net_close, net_bind, net_sendto, net_recvfrom, net_setsockopt, net_ctl_info, net_connect, net_listen,
             net_accept, net_send, net_recv };
struct Sock { int32_t fd; uint32_t ip, port; };
struct SockAddr { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; uint8_t zero[6]; };
static_assert(sizeof(SockAddr) == 16);
uintptr_t net_fn(NetFn f) { return reinterpret_cast<uintptr_t *>(u3h_ext() + 0x200)[f]; }
uint16_t swap16(uint16_t v) { return static_cast<uint16_t>(v << 8 | v >> 8); }
U3X_FN SockAddr inet_addr(uint32_t ip, uint32_t port) {
    SockAddr a;
    for (uint32_t k = 0; k < sizeof a; ++k) reinterpret_cast<uint8_t *>(&a)[k] = 0;
    a.len = 16;
    a.family = 2;
    a.port = swap16(static_cast<uint16_t>(port));
    a.addr = ip;
    return a;
}
U3X_RO const char lan_socket_name[] = "u3mp-lan", room_socket_name[] = "u3mp-room";
constexpr uint32_t loopback = 0x0100007fu;   // 127.0.0.1 as a sockaddr holds it
bool soft_error(int32_t r) {
    const uint32_t e = static_cast<uint32_t>(r) & 0xff;
    return r >= 0 || e == 35 || e == 36 || e == 37 || e == 57;   // EAGAIN EINPROGRESS EALREADY ENOTCONN
}
}  // namespace
extern "C" U3X_FN int32_t u3x_sock_error(int32_t r) {
    ext_eb()[91] = static_cast<uint32_t>(r);
    return r;
}
extern "C" U3X_FN int u3x_sock_open(const void *s) { return static_cast<const Sock *>(s)->fd > 0; }
extern "C" U3X_FN void u3x_sock_init(void *s_) {
    Sock *s = static_cast<Sock *>(s_);
    s->fd = 0;
    s->ip = 0;
    s->port = 0;
}
extern "C" U3X_FN int32_t u3x_sock_close(void *s_) {
    Sock *s = static_cast<Sock *>(s_);
    int32_t r = 0;
    if (s->fd > 0 && net_fn(net_close)) r = reinterpret_cast<int32_t (*)(int)>(net_fn(net_close))(s->fd);
    s->fd = 0;
    return r;
}
// the console's own address (netctl info 14 is its IP text)
extern "C" U3X_FN uint32_t u3x_sock_local_ip() {
    u3h_net_pick();
    char info[256];
    for (char &c : info) c = 0;
    if (net_fn(net_ctl_info) && reinterpret_cast<int32_t (*)(int, void *)>(net_fn(net_ctl_info))(14, info) == 0) return u3h_parse_ipv4(info);
    return 0;
}
extern "C" U3X_FN int32_t u3x_sock_setopt(int fd, int level, int opt, int32_t v) {
    return reinterpret_cast<int32_t (*)(int, int, int, const void *, uint32_t)>(net_fn(net_setsockopt))(fd, level, opt, &v, 4);
}
extern "C" U3X_FN int32_t u3x_sock_bind(int fd, uint32_t port) {
    const SockAddr a = inet_addr(0, port);
    return reinterpret_cast<int32_t (*)(int, const void *, uint32_t)>(net_fn(net_bind))(fd, &a, 16);
}
extern "C" U3X_FN void u3x_sock_local(void *s_, uint32_t port) {
    Sock *s = static_cast<Sock *>(s_);
    s->ip = u3x_sock_local_ip();
    s->port = port;
}
// UDP on a port, non-blocking, optionally broadcast and a receive buffer size; open already on that port = done.
// A bind failure keeps the descriptor, as PS3 does.
extern "C" U3X_FN int32_t u3x_sock_udp(void *s_, uint32_t port, int broadcast, int32_t receive_buffer) {
    Sock *s = static_cast<Sock *>(s_);
    if (s->fd > 0 && s->port == port) return 0;
    u3h_net_pick();
    if (!net_fn(net_socket) || !net_fn(net_setsockopt) || !net_fn(net_bind)) return -1;
    const int32_t fd = reinterpret_cast<int32_t (*)(const char *, int, int, int)>(net_fn(net_socket))(lan_socket_name, 2, 2, 0);
    s->fd = fd;
    if (fd < 0) {
        u3x_sock_error(fd);
        s->fd = 0;
        return -1;
    }
    if (u3x_sock_setopt(fd, 0xffff, 0x1200, 1) < 0 || (broadcast && u3x_sock_setopt(fd, 0xffff, 0x20, 1) < 0)) {   // SO_NBIO, SO_BROADCAST
        u3x_sock_error(-1);
        return -1;
    }
    if (receive_buffer) u3x_sock_setopt(fd, 0xffff, 0x1001, receive_buffer);   // SO_RCVBUF: a failure is only read back
    const int32_t r = u3x_sock_bind(fd, port);
    if (r < 0) {
        u3x_sock_error(r);
        return -1;
    }
    u3x_sock_local(s, port);
    return 0;
}
// TCP listen (reuse address, backlog 12), then non-blocking accept
extern "C" U3X_FN int32_t u3x_sock_listen(void *s_, uint32_t port) {
    Sock *s = static_cast<Sock *>(s_);
    if (s->fd > 0 && s->port == port) return 0;
    u3h_net_pick();
    if (!net_fn(net_socket) || !net_fn(net_listen)) return -1;
    const int32_t fd = reinterpret_cast<int32_t (*)(const char *, int, int, int)>(net_fn(net_socket))(room_socket_name, 2, 1, 0);
    s->fd = fd;
    if (fd < 0) {
        u3x_sock_error(fd);
        s->fd = 0;
        return -1;
    }
    int32_t r = u3x_sock_setopt(fd, 0xffff, 4, 1);   // SO_REUSEADDR
    if (r >= 0) r = u3x_sock_bind(fd, port);
    if (r >= 0) r = reinterpret_cast<int32_t (*)(int, int)>(net_fn(net_listen))(fd, 12);
    if (r < 0) {
        u3x_sock_error(r);
        u3x_sock_close(s);
        return -1;
    }
    u3x_sock_setopt(fd, 0xffff, 0x1200, 1);
    u3x_sock_local(s, port);
    return 0;
}
// TCP connect with 10 s timeouts, optional no-delay and non-blocking; "in progress" / "already" retried (up to
// 100000 times), "is connected" counts as connected
extern "C" U3X_FN int32_t u3x_sock_connect(void *s_, uint32_t ip, uint32_t port, int nodelay, int nonblocking) {
    Sock *s = static_cast<Sock *>(s_);
    u3h_net_pick();
    if (!net_fn(net_socket) || !net_fn(net_connect)) return -1;
    const int32_t fd = reinterpret_cast<int32_t (*)(const char *, int, int, int)>(net_fn(net_socket))(room_socket_name, 2, 1, 0);
    s->fd = fd;
    if (fd < 0) {
        u3x_sock_error(fd);
        s->fd = 0;
        return -1;
    }
    if (nodelay) u3x_sock_setopt(fd, 6, 1, 1);
    u3x_sock_setopt(fd, 0xffff, 0x1006, 10000000);
    u3x_sock_setopt(fd, 0xffff, 0x1005, 10000000);
    if (nonblocking && u3x_sock_setopt(fd, 0xffff, 0x1200, 1) < 0) {
        u3x_sock_close(s);
        return -1;
    }
    const SockAddr a = inet_addr(ip, port);
    int32_t r;
    int n = 0;
    do r = reinterpret_cast<int32_t (*)(int, const void *, uint32_t)>(net_fn(net_connect))(fd, &a, 16);
    while (r < 0 && ((static_cast<uint32_t>(r) & 0xff) == 36 || (static_cast<uint32_t>(r) & 0xff) == 37) && ++n < 100000);
    if (r >= 0 || (static_cast<uint32_t>(r) & 0xff) == 56) {
        s->ip = ip;
        s->port = port;
        return 0;
    }
    u3x_sock_error(r);
    u3x_sock_close(s);
    return -1;
}
extern "C" U3X_FN int32_t u3x_sock_send(const void *s_, const void *b, uint32_t n, int block) {
    const Sock *s = static_cast<const Sock *>(s_);
    if (!net_fn(net_send) || s->fd <= 0) return -1;
    return reinterpret_cast<int32_t (*)(int, const void *, uint32_t, int)>(net_fn(net_send))(s->fd, b, n, block ? 0 : 0x80);
}
// is there something to read (a 1-byte peek without waiting): 1 yes, 0 no, -1 broken
extern "C" U3X_FN int32_t u3x_sock_poll(const void *s_) {
    const Sock *s = static_cast<const Sock *>(s_);
    if (s->fd <= 0 || !net_fn(net_recv)) return -1;
    uint8_t b;
    const int32_t r = reinterpret_cast<int32_t (*)(int, void *, uint32_t, int)>(net_fn(net_recv))(s->fd, &b, 1, 0x82);
    if (r >= 0) return 1;
    return soft_error(r) ? 0 : -1;
}
// receive; a non-blocking "try again" is passed back, any other error closes the socket
extern "C" U3X_FN int32_t u3x_sock_recv(void *s_, void *b, uint32_t n, int block) {
    Sock *s = static_cast<Sock *>(s_);
    const int32_t r = reinterpret_cast<int32_t (*)(int, void *, uint32_t, int)>(net_fn(net_recv))(s->fd, b, n, block ? 0 : 0x80);
    if (r >= 0) return r;
    if (!block && (static_cast<uint32_t>(r) & 0xff) == 35) return r;
    u3x_sock_error(r);
    u3x_sock_close(s);
    return -1;
}
extern "C" U3X_FN int32_t u3x_sock_accept(const void *s_, uint32_t *ip, uint32_t *port) {
    const Sock *s = static_cast<const Sock *>(s_);
    if (!net_fn(net_accept)) return 0;
    SockAddr a = inet_addr(0, 0);
    a.len = 0;
    a.family = 0;
    uint32_t length = 16;
    const int32_t c = reinterpret_cast<int32_t (*)(int, void *, uint32_t *)>(net_fn(net_accept))(s->fd, &a, &length);
    if (c < 0) return 0;
    *ip = a.addr;
    *port = swap16(a.port);
    return c;
}
// an accepted connection becomes a socket; a loopback or empty address is our own
extern "C" U3X_FN void u3x_sock_adopt(void *s_, int32_t fd, uint32_t ip, uint32_t port) {
    Sock *s = static_cast<Sock *>(s_);
    s->fd = fd;
    s->port = port;
    s->ip = ip;
    if (ip == loopback || ip == 0) s->ip = u3x_sock_local_ip();
}
extern "C" U3X_FN int32_t u3x_sock_recvfrom(const void *s_, void *b, uint32_t n, uint32_t *ip, uint32_t *port) {
    const Sock *s = static_cast<const Sock *>(s_);
    SockAddr a = inet_addr(0, *port);
    uint32_t length = 16;
    const int32_t r = reinterpret_cast<int32_t (*)(int, void *, uint32_t, int, void *, uint32_t *)>(net_fn(net_recvfrom))(s->fd, b, n, 0, &a, &length);
    if (r > 0) {
        *ip = a.addr;
        *port = swap16(a.port);
    }
    return r;
}
extern "C" U3X_FN int32_t u3x_sock_sendto(const void *s_, const void *b, uint32_t n, uint32_t ip, uint32_t port) {
    const Sock *s = static_cast<const Sock *>(s_);
    const SockAddr a = inet_addr(ip, port);
    return reinterpret_cast<int32_t (*)(int, const void *, uint32_t, int, const void *, uint32_t)>(net_fn(net_sendto))(s->fd, b, n, 0, &a, 16);
}
// The LAN messages' byte stream {bytes, position, limit}: big-endian fields of 1..8 bytes; past the limit a write
// is dropped and a read gives 0, the position moves on either way.
namespace {
struct ByteStream { uint8_t *p; uint32_t pos, max; };
}
extern "C" U3X_FN void u3x_stream_put(void *w_, uint64_t v, int bytes) {
    ByteStream *w = static_cast<ByteStream *>(w_);
    for (int i = bytes - 1; i >= 0; --i) {
        if (w->pos < w->max) w->p[w->pos] = static_cast<uint8_t>(v >> (8 * i));
        ++w->pos;
    }
}
extern "C" U3X_FN uint64_t u3x_stream_get(void *r_, int bytes) {
    ByteStream *r = static_cast<ByteStream *>(r_);
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v = v << 8 | (r->pos < r->max ? r->p[r->pos] : 0);
        ++r->pos;
    }
    return v;
}

// ===== v7.151: part 12 (the Invites list) =====

extern "C" {
U3H_IMPORT uint32_t u3h_fr_tab(uintptr_t base, void *s);
U3H_IMPORT void u3h_fi_mark_seen(uintptr_t base, void *s);
U3H_IMPORT void u3h_fi_refresh(uintptr_t base, void *s, int notify);
U3H_IMPORT uint8_t *u3h_fr_ent(uintptr_t base, uint32_t i);
U3H_IMPORT int u3h_fr_strcmp(uintptr_t base, const void *a, const void *b);
U3H_IMPORT void u3h_fr_icon22(int32_t *ev, int32_t v);
U3H_IMPORT int32_t u3h_fr_state_icon(uintptr_t base, uint8_t *e);
U3H_IMPORT char *u3h_fr_buffer(void);
U3H_IMPORT uint64_t u3h_fi_time(int32_t i);
U3H_IMPORT int32_t *u3h_fi_list_selection(void);
U3H_IMPORT const char *u3h_fi_ago(uintptr_t base, void *s, uint32_t seconds);
}

// The Invites list (enum 0xb2, a RET on PS4), only on the friends page's Invites tab: one row per invite (up to 10
// of 0x28 bytes at invite object +8, inviter name +0x10, count at +0), shown when the inviter is a friend: the name,
// the friend's state icon and how long ago it was sent; with no invites the first row says so. Op 0x17 starts a
// refresh when no popup is up. The list's own selection is kept apart from the clicked invite. What the list asks
// goes to the log once per kind (tab, op, row asks; up to 8 row asks). ea[113] counts calls.
namespace {
struct InviteLog { int32_t last_tab; uint32_t ops_seen; uint32_t rows[8]; uint32_t row_count; };
U3X_BSS InviteLog invite_log;
U3X_RO const char il_tab[] = "Invites list: ", il_rows[] = " rows", il_not[] = "Invites list: not the Invites tab (tab text ",
                  il_close[] = ")", il_op[] = "Invites list: op ", il_first[] = " asked (first time)",
                  il_asks[] = "Invites list asks op ", il_row[] = ", row ", il_friend[] = " (a friend)",
                  il_stranger[] = " (not found among friends)", il_from[] = "Invites list row from ";
constexpr uint32_t invites_tab = 0x885ef7e9u;
}  // namespace
extern "C" U3X_FN int u3x_invite_list(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414eb2u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    uint8_t *o = reinterpret_cast<uint8_t *>(base + 0x17d5c58 + 0x430);
    if (ev[0] == 6) {
        const uint32_t tab = u3h_fr_tab(base, s);
        const int32_t v = tab == invites_tab ? static_cast<int32_t>(rd<uint32_t>(o, 0)) : -1 - static_cast<int32_t>(tab & 0xffffu);
        if (v + 0x10002 != invite_log.last_tab) {
            invite_log.last_tab = v + 0x10002;
            if (v >= 0) u3h_so_note(il_tab, nullptr, v, il_rows);
            else u3h_so_note(il_not, nullptr, static_cast<int32_t>(tab & 0xffffu), il_close);
        }
    }
    if (u3h_fr_tab(base, s) != invites_tab) {
        if (ev[0] == 6) ev[1] = -1;
        return 1;
    }
    u3h_fi_mark_seen(base, s);
    const int32_t op = ev[0], n = static_cast<int32_t>(rd<uint32_t>(o, 0));
    ++ext_ea()[113];
    if (op >= 0 && op < 32 && !(invite_log.ops_seen >> op & 1u)) {
        invite_log.ops_seen |= 1u << op;
        u3h_so_note(il_op, nullptr, op, il_first);
    }
    switch (op) {
    case 6: ev[1] = n - 1 < 0 ? 0 : n - 1; return 1;
    case 1: set_row_text(ev, empty_text); return 1;
    case 5: ev[1] = *u3h_fi_list_selection(); return 1;
    case 10: *u3h_fi_list_selection() = ev[1]; return 1;
    case 8: ev[1] = 1; return 1;
    case 0x17: {
        const uint8_t *popup = at<const uint8_t *>(base + 0x17bd080 + 0x898);
        if ((popup && rd<uint32_t>(popup, 0x10)) || !ev[14] || o[0x198]) return 1;
        o[0x198] = 1;
        u3h_fi_refresh(base, s, 0);
        return 1;
    }
    }
    const int32_t i = ev[1];
    if (i < 0 || i >= 10) return 1;
    char name[0x15];
    for (int k = 0; k < 0x14; ++k) name[k] = static_cast<char>(o[8 + i * 0x28 + 0x10 + k]);
    name[0x14] = 0;
    uint8_t *friend_ = nullptr;
    const uint8_t *friends = reinterpret_cast<const uint8_t *>(base + 0x17caeb0);
    if (name[0] && !rd<uint64_t>(friends, 0xc8))
        for (uint32_t k = 0; k < rd<uint32_t>(friends, 0); ++k) {
            uint8_t *x = u3h_fr_ent(base, k);
            if (x[1] && u3h_fr_strcmp(base, x + 1, name) == 0) { friend_ = x; break; }
        }
    {
        const uint32_t key = static_cast<uint32_t>(op) << 16 | static_cast<uint32_t>(i & 0xff) << 8 | (friend_ ? 1u : 0u) |
                             (static_cast<uint32_t>(ev[0x15]) & 0xffu) << 24 | 0x80u;
        bool seen = false;
        for (uint32_t k = 0; k < invite_log.row_count && k < 8; ++k)
            if (invite_log.rows[k] == key) seen = true;
        if (!seen && invite_log.row_count < 8) {
            invite_log.rows[invite_log.row_count++] = key;
            u3h_so_note(il_asks, nullptr, op, il_row);
            u3h_so_note(nullptr, nullptr, i, nullptr);
            u3h_so_note(il_from, name, -1, friend_ ? il_friend : il_stranger);
        }
    }
    if (!friend_) return 1;   // not a friend: nothing, as PS3
    if (op == 4) {
        if (static_cast<uint32_t>(ev[0x14]) != 0xf5a294f3u || !n) return 1;
        u3h_fr_icon22(ev, u3h_fr_state_icon(base, friend_));
        return 1;
    }
    if (op != 2) return 1;
    const uint32_t sub = static_cast<uint32_t>(ev[0x15]);
    if (sub == 0) {
        if (!n) {
            set_row_text(ev, u3h_text(base, s, 0x52237405u));
        } else {
            char *b = u3h_fr_buffer();
            for (int k = 0; k < 0x15; ++k) b[k] = name[k];
            set_row_text(ev, b);
        }
    }
    if (sub != 1 || !n) return 1;
    const int32_t age = static_cast<int32_t>(u3x_network_usec() / 1000000) - static_cast<int32_t>(u3h_fi_time(i) / 1000000);
    set_row_text(ev, u3h_fi_ago(base, s, static_cast<uint32_t>(age < 0 ? 0 : age)));
    return 1;
}

// ===== v7.152: next batch, part 1 (submesh draw diagnostic, spotted flags) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_f4_state(void);
U3H_IMPORT uint32_t *u3h_f4_current(void);
U3H_IMPORT volatile uint8_t *u3h_f4_entry(uint8_t *instance);
U3H_IMPORT uint8_t *u3h_spot_flags(void);
U3H_IMPORT uint8_t *u3h_netgame(uintptr_t base, void *s);
}

namespace {
U3X_FN bool code_starts(uintptr_t base, uint32_t offset, const uint8_t *bytes) {
    for (int i = 0; i < 12; ++i)
        if (at<uint8_t>(base + offset + i) != bytes[i]) return false;
    return true;
}
U3X_FN void atomic_add(volatile uint32_t *p) { __atomic_fetch_add(const_cast<uint32_t *>(p), 1u, __ATOMIC_RELAXED); }
U3X_FN void atomic_or(volatile uint8_t *p, uint8_t v) { __atomic_fetch_or(const_cast<uint8_t *>(p), v, __ATOMIC_RELAXED); }
}  // namespace

// f4001 (diagnostic, kind 235 at the submesh draw 0xe99530): classifies why a submesh of a background instance is
// skipped, as the draw does: A no geometry, B pass not in the submesh mask, C not visible, D no shader for the
// variant, Z draw type 0x1a, unknown variant path; else drawn. The variant comes from the game's own 27-entry
// table at 0x13a7bf0 (32-bit entries) with the same per-type overrides as the draw. eb[3] bit 4 (multiplayer only)
// fills a missing variant from variants 0, 1, 2, 3, 9. State in f4_d (hdr +0, why +0x280, ring +0x2c0) and f4_cur.
namespace {
U3X_RO const uint8_t fallback_variants[5] = {0, 1, 2, 3, 9};
}
extern "C" U3X_FN int u3x_f4_submesh(uintptr_t base, uint64_t *r) {
    uint8_t *d = u3h_f4_state();
    volatile uint32_t *hdr = reinterpret_cast<volatile uint32_t *>(d), *why = reinterpret_cast<volatile uint32_t *>(d + 0x280),
                      *ring = reinterpret_cast<volatile uint32_t *>(d + 0x2c0);
    uint32_t (*cur)[8] = reinterpret_cast<uint32_t (*)[8]>(u3h_f4_current());
    uint8_t *sub = reinterpret_cast<uint8_t *>(r[4]), *in = reinterpret_cast<uint8_t *>(r[3]) - 0x170;
    const uint32_t pass = static_cast<uint32_t>(r[6]), type = static_cast<uint32_t>(r[7]), p = pass & 15;
    const int32_t selector = static_cast<int32_t>(r[14]);
    atomic_add(&hdr[4]);
    if (!aligned_pointer(sub) || !aligned_pointer(in)) { atomic_add(&hdr[5]); return 0; }
    uint8_t *subs = rd<uint8_t *>(in, 0x168);
    const int32_t count = rd<int32_t>(in, 0x164);
    if (!aligned_pointer(subs) || count <= 0 || count > 4096 || sub < subs || sub >= subs + static_cast<uintptr_t>(count) * 0x3a0 ||
        static_cast<uintptr_t>(sub - subs) % 0x3a0) {
        atomic_add(&hdr[5]);
        return 0;
    }
    const uint32_t k = static_cast<uint32_t>(static_cast<uintptr_t>(sub - subs) / 0x3a0);
    const uint8_t pass_bit = static_cast<uint8_t>(pass < 7 ? 1u << pass : 0x80u);
    volatile uint8_t *e = u3h_f4_entry(in);
    atomic_or(&e[2], pass_bit);
    atomic_add(&cur[p][5]);
    if (rd<int32_t>(sub, 0x328) >= 0) atomic_add(&why[7]);
    uint8_t w = 0;
    if (!rd<uint64_t>(sub, 0x10) || !rd<uint64_t>(sub, 0x18)) w = 1;
    else if (type == 0x1a) w = 0x20;
    else if (!((rd<uint64_t>(sub, 0x240 + ((pass >> 3) & 0x1ffffff8u)) >> (pass & 63)) & 1)) w = 2;
    else if (!sub[0x256]) w = 4;
    else {
        int v = type < 27 ? static_cast<int>(at<uint32_t>(base + 0x13a7bf0 + type * 4)) : 0;
        bool fallback = false;
        const uint8_t no_override = at<volatile uint8_t>(base + 0x18c6994);
        if (type == 0) {
            if (selector == 1 && rd<uint64_t>(sub, 0x118)) v = 8;
        } else if (type == 3) {
            if (sub[0x255] && !no_override) {
                v = 2;
                if (selector == 1 && rd<uint64_t>(sub, 0x58)) v = 3;
            } else if (selector == 1 && rd<uint64_t>(sub, 0x48)) {
                v = 1;
            }
            if (rd<uint64_t>(sub, 0x138) && v != 1) v = 0x1f;
            fallback = true;
        } else if (type == 1) {
            if (selector == 1 && rd<uint64_t>(sub, 0x118)) v = 0x1b;
        } else if ((type & ~1u) == 4) {
            if (selector == 1 && rd<uint64_t>(sub, 0x120)) v = 0x1c;
        } else if (type == 9) {
            v = 0x11;
            if (selector == 1 && rd<uint64_t>(sub, 0xd0)) v = 0x12;
        } else if (type == 0xe) {
            if (rd<int32_t>(sub, 0x250) == 4) w = 0x10;
            v = 0xe;
        } else if (type == 0xf) {
            if (sub[0x255] && !no_override) v = 0x14;
        }
        if (!w && v < 0x20) {
            if (!rd<uint64_t>(sub, 0x40 + v * 8) && fallback && rd<uint64_t>(sub, 0x88)) v = 9;
            if (!rd<uint64_t>(sub, 0x40 + v * 8)) {
                w = 8;
                const uint32_t i = __atomic_fetch_add(const_cast<uint32_t *>(&hdr[7]), 1u, __ATOMIC_RELAXED) & 31;
                ring[i] = (rd<uint32_t>(in, 0xd4) & 0x3ff) | (k & 63) << 10 | (p << 16) | (static_cast<uint32_t>(v) & 63) << 20 | (type & 63) << 26;
                atomic_add(&cur[p][7]);
                const bool multiplayer = at<volatile uint8_t>(base + 0x17d5c61) || at<volatile uint8_t>(base + 0x17d5c60);
                if (multiplayer && (ext_eb()[3] & 0x10))
                    for (int j = 0; j < 5; ++j)
                        if (rd<uint64_t>(sub, 0x40 + fallback_variants[j] * 8)) {
                            *reinterpret_cast<uint64_t *>(sub + 0x40 + v * 8) = rd<uint64_t>(sub, 0x40 + fallback_variants[j] * 8);
                            w = 0;
                            break;
                        }
            }
        } else if (!w) {
            w = 0x40;
        }
    }
    if (w) {
        atomic_or(&e[4], w);
        for (int j = 0; j < 7; ++j)
            if (w >> j & 1) atomic_add(&why[j]);
    } else {
        atomic_or(&e[3], pass_bit);
        atomic_add(&cur[p][6]);
    }
    return 0;
}

// c007: PS3 1.19 spots enemies near a player with the Spotter booster on both screens (flag 0 at np+0x9b1, flag 1
// for the second screen in spot_flags[slot]); a Counter Intel booster (0x41479fa0) on the target or, per screen, on
// that screen's player blocks it. ec[0] bit 7 keeps one flag only. ec[50] calls, ec[51]/[52] flags set, ec[53]
// second-screen reads, ec[54] the replayed reader value.
namespace {
U3X_RO const uint8_t radius_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
U3X_RO const uint8_t booster_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49};
// a game function taking 4 integer arguments that returns a float (its bits)
U3X_FN uint32_t call_float(uintptr_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t rdi asm("rdi") = a0;
    register uint64_t rsi asm("rsi") = a1;
    register uint64_t rdx asm("rdx") = a2;
    register uint64_t rcx asm("rcx") = a3;
    register uint64_t rax asm("rax") = fn;
    asm volatile(
        "push %%rbp\n\tmov %%rsp,%%rbp\n\tand $-16,%%rsp\n\tcall *%%rax\n\tmov %%rbp,%%rsp\n\tpop %%rbp\n\tmovd %%xmm0,%%eax"
        : "+a"(rax), "+D"(rdi), "+S"(rsi), "+d"(rdx), "+c"(rcx)
        :
        : "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
          "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
    return static_cast<uint32_t>(rax);
}
U3X_FN bool has_booster(uintptr_t base, uint8_t *np, uint32_t sid) {
    return np && game_call<uint8_t>(base + 0x793280, np, sid, 0u) != 0;
}
U3X_FN int player_slot(uintptr_t base, const uint8_t *np) {
    const intptr_t d = np - reinterpret_cast<const uint8_t *>(base + 0x17cb060 + 0x390);
    if (d < 0 || d % 0x9c0 || d / 0x9c0 >= 10) return -1;
    return static_cast<int>(d / 0x9c0);
}
}  // namespace
extern "C" U3X_FN void u3x_spotter(uintptr_t base, void *s, uint8_t *spotter) {
    uint32_t *ec = ext_ec();
    ++ec[50];
    if (!code_starts(base, 0x689d20, radius_prologue) || !code_starts(base, 0x793280, booster_prologue)) return;
    const uint32_t radius = call_float(base + 0x689d20, base + 0x17c3b78, 0x1ca807a, reinterpret_cast<uint64_t>(spotter), 0);
    if (!f_less(0, radius)) return;
    const uint32_t radius2 = f_mul(radius, radius);
    uint8_t *manager = reinterpret_cast<uint8_t *>(base + 0x17cb060);
    uint8_t *second = u3h_spot_flags();
    const bool both = !(ec[0] & 128);
    for (int i = 0; i < 10; ++i) {
        if (!at<uint8_t>(base + 0x17d5c58 + 9)) continue;
        uint8_t *t = manager + 0x390 + i * 0x9c0;
        if (t == spotter) continue;
        const uint8_t *ng = u3h_netgame(base, s);
        if (!(ng && rd<int32_t>(ng, 0xc) == 0xe) && spotter && rd<int32_t>(t, 0x21c) == rd<int32_t>(spotter, 0x21c)) continue;   // a team mate
        if (rd<int32_t>(t, 0x39c) > rd<int32_t>(t, 0x3a0)) continue;   // dead
        for (int j = 0; j < (both ? 2 : 1); ++j) {
            uint8_t *flag = j ? &second[i] : &t[0x9b1];
            if (*flag) continue;
            const uint32_t dx = f_sub(rd<uint32_t>(spotter, 0), rd<uint32_t>(t, 0)), dy = f_sub(rd<uint32_t>(spotter, 4), rd<uint32_t>(t, 4)),
                           dz = f_sub(rd<uint32_t>(spotter, 8), rd<uint32_t>(t, 8));
            const uint32_t d2 = f_add(f_mul(dz, dz), f_add(f_mul(dx, dx), f_mul(dy, dy)));
            if (!f_less(d2, radius2)) continue;
            if (has_booster(base, t, 0x41479fa0u)) continue;
            if (both && has_booster(base, rd<uint8_t *>(manager, 0x370 + 8 * j), 0x41479fa0u)) continue;
            *flag = 1;
            ++ec[51 + j];
        }
    }
}
// the second-screen flag of one player (player reset) or of every player that is not in the game
extern "C" U3X_FN void u3x_spot_clear(uintptr_t base, uint8_t *np) {
    uint8_t *second = u3h_spot_flags();
    if (np) {
        const int i = player_slot(base, np);
        if (i >= 0) second[i] = 0;
        return;
    }
    for (int i = 0; i < 10; ++i)
        if (!at<uint8_t>(base + 0x17cb060 + 0x390 + i * 0x9c0 + 0x5e4)) second[i] = 0;
}
// np's flag for the screen being drawn
extern "C" U3X_FN uint8_t u3x_spot_flag(uintptr_t base, uint8_t *np) {
    uint32_t *ec = ext_ec();
    const uint8_t *hud = at<const uint8_t *>(base + 0x1806480);
    if (!(ec[0] & 128) && hud && rd<int32_t>(hud, 0x1d0) == 1) {
        const int i = player_slot(base, np);
        ++ec[53];
        if (i >= 0) return u3h_spot_flags()[i];
    }
    return np[0x9b1];
}
extern "C" U3X_FN void u3x_spot_read(uintptr_t base, uint8_t *np) { ext_ec()[54] = np ? u3x_spot_flag(base, np) : 0; }

// ===== v7.153: next batch, part 2 (LAN room client, co-op weapon mods list, NPC melee damage native) =====

extern "C" {
U3H_IMPORT int64_t u3h_lnb_call(void *object, int slot);
U3H_IMPORT void u3h_cm_lanaddr(uintptr_t base, const char *name, uint32_t ip);
U3H_IMPORT volatile uint8_t *u3h_elim_received(void);
U3H_IMPORT uint8_t *u3h_adv_rules(void);
U3H_IMPORT uint8_t *u3h_adv_received(void);
U3H_IMPORT int u3h_sym_ready(uintptr_t base, void *s, uint32_t id);
U3H_IMPORT uint8_t *u3h_coop_weapon(void *s, uint32_t index);
U3H_IMPORT void u3h_coop_mod_sound(uintptr_t base, void *s, uint32_t user, uint32_t weapon, uint8_t *mods, uint32_t n, int32_t row);
U3H_IMPORT uint32_t u3h_mod_name(uint32_t bit);
U3H_IMPORT uint32_t u3h_mod_desc(uint32_t bit);
U3H_IMPORT int u3h_unlock_status(uintptr_t base, void *s, uint32_t user, uint32_t weapon, uint32_t mod);
U3H_IMPORT void u3h_money_text(uintptr_t base, void *s, int32_t *ev);
U3H_IMPORT uint8_t *u3h_weapon_def(void *s, uint32_t weapon);
U3H_IMPORT void u3h_set_icon(uintptr_t base, int32_t *ev, uint32_t id);
U3H_IMPORT uint32_t u3h_mod_icon(void *s, uint32_t bit);
U3H_IMPORT void u3h_lock_overlay(int32_t *ev);
U3H_IMPORT void u3h_preview_mods(uintptr_t base, uint32_t mask);
}

// The LAN room client: the room server's messages, read from the client socket into the bind's 0x800-byte receive
// buffer and handled whole (each returns its size; 0 = incomplete): 0xc9 room state (capacity, party id, up to 10
// member records of 0x70 bytes; the first one makes the session joined and created), 0xca member joined, 0xcb
// member left, 0xce member data, 0xd3 the host's elimination round notice, 0xd2 the host's Advanced Settings, 0xcc
// kicked. A closed or broken connection leaves the room (session events 5, and 6 when kicked). eb[78] messages,
// eb[79] the last id.
namespace {
struct ClientView { uint8_t *session; void *socket; uint32_t *received; uint8_t *buffer; };
U3X_FN void leave_room(uintptr_t base, uint8_t *session) {
    u3h_lnb_call(session, 5);
    *reinterpret_cast<uint64_t *>(session + 0x18) = 0;
    game_call<int64_t>(base + 0x7a25f0, session);   // clear the members
}
MemberInfo member_record(const uint8_t *r) {
    MemberInfo in;
    in.name = reinterpret_cast<const char *>(r);
    in.id = rd<uint16_t>(r, 0x28);
    in.nat = r[0x2a];
    in.length = r[0x2b];
    in.data = r + 0x2e;
    in.has_data = 1;
    return in;
}
U3X_FN void member_joined(uintptr_t base, uint8_t *session, const uint8_t *r) {
    const MemberInfo in = member_record(r);
    if (!r[0x2c]) u3h_cm_lanaddr(base, reinterpret_cast<const char *>(r), rd<uint32_t>(r, 0x24));   // not us: its LAN address
    u3x_add_member(base, session, &in, r[0x2c], r[0x2d]);
}
}  // namespace
extern "C" U3X_FN void u3x_lan_client_lost(uintptr_t base, const void *view, int kicked) {
    const ClientView *b = static_cast<const ClientView *>(view);
    u3x_sock_close(b->socket);
    u3h_lnb_call(b->session, 5);
    if (kicked) u3h_lnb_call(b->session, 6);
    *reinterpret_cast<uint64_t *>(b->session + 0x18) = 0;
    game_call<int64_t>(base + 0x7a25f0, b->session);
}
extern "C" U3X_FN uint32_t u3x_lan_client_message(uintptr_t base, const void *view) {
    const ClientView *b = static_cast<const ClientView *>(view);
    uint8_t *ss = b->session;
    const uint8_t *m = b->buffer;
    const uint32_t n = *b->received, id = rd<uint32_t>(m, 0);
    uint32_t *eb = ext_eb();
    if (id == 0xc9) {
        if (n < 0x478) return 0;
        ++eb[78];
        eb[79] = id;
        *reinterpret_cast<uint32_t *>(ss + 0x20c) = rd<uint16_t>(m, 0x10);
        *reinterpret_cast<uint64_t *>(ss + 0x18) = rd<uint64_t>(m, 8);
        uint32_t count = rd<uint16_t>(m, 0x12);
        if (count > 10) count = 10;
        for (uint32_t i = 0; i < count; ++i) member_joined(base, ss, m + 0x18 + i * 0x70);
        if (!ss[0xb8]) {   // joined
            *reinterpret_cast<uint64_t *>(ss + 0xc0) = 1;
            u3h_lnb_call(ss, 2);
            ss[0xb8] = 1;
        }
        if (!ss[0xa1]) {   // created
            u3h_lnb_call(ss, 1);
            ss[0xa1] = 1;
        }
        return 0x478;
    }
    ++eb[78];
    eb[79] = id;
    uint8_t *party = reinterpret_cast<uint8_t *>(base + 0x17dc298);
    switch (id) {
    case 0xca:
        if (n < 0x80) return 0;
        member_joined(base, ss, m + 0x10);
        return 0x80;
    case 0xcb:
        if (n < 0x38) return 0;
        u3x_remove_member(base, ss, reinterpret_cast<const char *>(m + 0x10));
        return 0x38;
    case 0xce: {
        if (n < 0x58) return 0;
        uint8_t *member = u3x_find_member(ss, rd<uint16_t>(m, 4));
        if (member) {
            *reinterpret_cast<uint32_t *>(member + 0x3c) = rd<uint32_t>(m, 0x10);
            for (int k = 0; k < 0x40; ++k) member[0x40 + k] = m[0x14 + k];
        }
        return 0x58;
    }
    case 0xd3: {
        if (n < 0x10) return 0;
        if (ss == party && rd<uint32_t>(ss, 0x12e4) != rd<uint32_t>(ss, 0x12e8)) *u3h_elim_received() = 1;
        return 0x10;
    }
    case 0xd2: {   // the host's Advanced Settings, taken by a party member unless the advanced rows are off
        if (n < 0x20) return 0;
        const uint32_t host = rd<uint32_t>(ss, 0x12e8);
        if (ss == party && rd<uint32_t>(m, 4) == host && rd<uint32_t>(ss, 0x12e4) != host && !(ext_functions() & 1)) {
            uint8_t *a = u3h_adv_rules();   // {sel1, respawn, health, weapon, f1, f2}
            const int16_t sel = rd<int16_t>(m, 0x1e);
            *reinterpret_cast<uint32_t *>(a + 4) = rd<uint32_t>(m, 0x10);
            *reinterpret_cast<uint32_t *>(a + 8) = rd<uint32_t>(m, 0x14);
            *reinterpret_cast<uint32_t *>(a + 12) = rd<uint32_t>(m, 0x18);
            a[16] = m[0x1c];
            a[17] = m[0x1d];
            at<int16_t>(base + 0x17d167c) = sel;
            *reinterpret_cast<uint32_t *>(a) = static_cast<uint32_t>(sel + 1);
            *u3h_adv_received() = 1;
        }
        return 0x20;
    }
    case 0xcc:
        if (n < 0x10) return 0;
        u3x_lan_client_lost(base, view, 1);
        return 0x10;
    }
    return 0;
}
extern "C" U3X_FN void u3x_lan_client_update(uintptr_t base, const void *view) {
    const ClientView *b = static_cast<const ClientView *>(view);
    if (!u3x_sock_open(b->socket)) return;
    const int32_t p = u3x_sock_poll(b->socket);
    if (p < 0) {
        u3x_sock_close(b->socket);
        leave_room(base, b->session);
        return;
    }
    if (p == 0) return;
    uint32_t &have = *b->received;
    const int32_t r = u3x_sock_recv(b->socket, b->buffer + have, 0x800 - have, 1);
    if (r > 0) have += static_cast<uint32_t>(r);
    if (r == 0) {
        u3x_sock_close(b->socket);
        leave_room(base, b->session);
    }
    while (have) {
        uint32_t used = u3x_lan_client_message(base, view);
        if (!used) break;
        if (used > have) used = have;
        for (uint32_t j = used; j < have; ++j) b->buffer[j - used] = b->buffer[j];
        have -= used;
    }
}

// The co-op weapon mods list (enum 0xa7) for the weapon chosen at 0x17d9998: mod rows of 0x14 bytes (the mod bit at
// +0), their name, description, unlock text and price; icons (equipped, locked, buyable, new, can-buy overlay); the
// selection at 0x1b6ae68; picking a row toggles the mod in the profile's mask (+0x1534 + weapon) and previews it.
extern "C" U3X_FN int u3x_coop_mods(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414ea7) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 760) = base;   // profile_base
    const uint32_t user = (at<uint8_t>(base + 0x17bde75) ? 1u : at<uint32_t>(base + 0x17bf714)) & 3;
    if (!u3h_sym_ready(base, s, 0x556e3fb5) || !u3h_sym_ready(base, s, 0x61d589d9)) {
        if (ev[0] == 6) ev[1] = -1;
        return 1;
    }
    const uint32_t weapon_index = at<uint32_t>(base + 0x17d9998);
    const uint8_t *w = u3h_coop_weapon(s, weapon_index);
    if (!w) {
        if (ev[0] == 6) ev[1] = -1;
        return 1;
    }
    const uint32_t weapon = rd<uint32_t>(w, 0), n = rd<uint32_t>(w, 0x10);
    uint8_t *mods = rd<uint8_t *>(w, 8);
    uint8_t *profile = player_profile(base);
    const int32_t row = ev[1];
    const uint32_t bit = (mods && row >= 0 && static_cast<uint32_t>(row) < n) ? rd<uint32_t>(mods, row * 0x14) : 0;
    int32_t &selected = at<int32_t>(base + 0x1b6ae68);
    if (ev[0] == 0x16) u3h_coop_mod_sound(base, s, user, weapon, mods, n, ev[0x17]);
    switch (ev[0]) {
    case 2: {
        const uint32_t sub = static_cast<uint32_t>(ev[0x15]);
        if (sub == 0) {
            if (bit) set_row_text(ev, u3h_text(base, s, u3h_mod_name(bit)));
        } else if (sub == 3) {
            if (bit) set_row_text(ev, u3h_text(base, s, u3h_mod_desc(bit)));
        } else if (sub == 1) {
            if (bit && u3h_unlock_status(base, s, user, weapon, bit) != 0) {
                const uintptr_t item_text = state_native(s, 26);
                if (item_text) set_row_text(ev, reinterpret_cast<const char *(*)(uint32_t, uint32_t)>(item_text)(weapon, bit));
            }
        } else if (sub == 8) {
            const int st = bit ? u3h_unlock_status(base, s, user, weapon, bit) : 0;
            if (st == 1 || st == 2) u3h_money_text(base, s, ev);
        } else if (sub == 0x5d822913u) {
            const uint8_t *def = u3h_weapon_def(s, weapon);
            if (def) set_row_text(ev, u3h_text(base, s, rd<uint32_t>(def, 4)));
        }
        break;
    }
    case 4: {
        const uint32_t slot = static_cast<uint32_t>(ev[0x14]);
        if (!bit) break;
        if (slot == 0xf5a294f3u) {
            u3h_set_icon(base, ev, u3h_mod_icon(s, bit));
            break;
        }
        if (slot != 0) break;
        const uintptr_t can_buy = state_native(s, 36);
        if (can_buy && reinterpret_cast<uint8_t (*)(uintptr_t, uint32_t, uint32_t, uint32_t)>(can_buy)(base + 0x17f3068, user, weapon, bit)) {
            u3h_lock_overlay(ev);
            break;
        }
        switch (u3h_unlock_status(base, s, user, weapon, bit)) {
        case 0:
            if (profile && (profile[0x1534 + weapon_index] >> (row & 31) & 1)) u3h_set_icon(base, ev, 0x08208d0au);
            break;
        case 1: u3h_set_icon(base, ev, 0xc2bb8920u); break;
        case 2: u3h_set_icon(base, ev, 0x83817313u); break;
        case 3: u3h_set_icon(base, ev, 0x7bebc95fu); break;
        }
        break;
    }
    case 5: ev[1] = selected; break;
    case 6: ev[1] = static_cast<int32_t>(n) - 1; break;
    case 9: {
        if (profile) u3h_preview_mods(base, profile[0x1534 + weapon_index]);   // the saved mods again
        const int32_t sel = selected;
        const uintptr_t seen = state_native(s, 32);
        if (seen && mods && sel >= 0 && static_cast<uint32_t>(sel) < n)
            reinterpret_cast<void (*)(uintptr_t, uint32_t, uint32_t, uint32_t)>(seen)(base + 0x17f3068, user, weapon, rd<uint32_t>(mods, sel * 0x14));
        break;
    }
    case 10: selected = row; break;
    case 0x16: {
        const int32_t r = ev[0x17];
        if (!profile || !mods || r < 0 || static_cast<uint32_t>(r) >= n) break;
        if (u3h_unlock_status(base, s, user, weapon, rd<uint32_t>(mods, r * 0x14)) == 0)
            profile[0x1534 + weapon_index] ^= static_cast<uint8_t>(1u << (r & 7));
        u3h_preview_mods(base, profile[0x1534 + weapon_index]);
        break;
    }
    }
    return 1;
}

// DcNetApplyNpcFightDamage (empty on PS4): (net player id, NPC name, damage, attack name). The NPC is found in the
// net game manager's name hash (FNV-1a of the name's 4 bytes); the host notes the damage per attacker (3 slots), the
// damage is applied (a guest only when it does not exceed the NPC's health), and when the hit kills it the host sends
// the kill with a fresh hit record. ea[46] hits applied, ea[47] kills sent.
extern "C" U3X_FN int64_t u3x_native_npc_fight(int32_t argc, uint64_t *argv, void *ctx) {
    const uintptr_t base = native_base();
    if (!base) return 0;
    uint8_t *mg = reinterpret_cast<uint8_t *>(base + 0x17cb060), *party = reinterpret_cast<uint8_t *>(base + 0x17d6420);
    if (!at<uint8_t>(base + 0x17d5c58 + 9)) return 0;
    struct { uint32_t n, p0; uint64_t *v; uint32_t cur, req; void *c; uint64_t e; } args;   // the script argument reader
    game_call<void>(base + 0x11cc560, static_cast<void *>(&args), argc, argv, ctx, 4);
    const uint32_t name_type = at<uint32_t>(base + 0x16f0ae0);
    const uint64_t advance = at<uint64_t>(base + 0x16f0ae8);
    uint32_t player = 0, damage = 0;
    if (args.cur < args.n) { player = static_cast<uint32_t>(args.v[args.cur]); if (advance) ++args.cur; }
    const uint32_t name = game_call<uint32_t>(base + 0x11cc9d0, static_cast<void *>(&args), name_type, advance);
    if (args.cur < args.n) { damage = static_cast<uint32_t>(args.v[args.cur]); if (advance) ++args.cur; }
    const uint32_t attack = game_call<uint32_t>(base + 0x11cc9d0, static_cast<void *>(&args), name_type, advance);
    uint8_t *np = nullptr;
    uint32_t id = 0;
    if (player)
        for (int i = 0; i < 10; ++i) {
            uint8_t *q = mg + 0x390 + i * 0x9c0;
            if (net_player_id(q) == player) { np = q; id = player; break; }
        }
    if (!np) return 0;
    uint8_t *attacker = handle_object(rd<uint8_t **>(np, 0x10), rd<uint32_t>(np, 0x18));
    if (!attacker) return 0;
    uint32_t h = 0x811c9dc5u, k = name;
    for (int i = 0; i < 4; ++i) { h = (h ^ (k & 0xff)) * 0x1000193u; k >>= 8; }
    const int32_t buckets = rd<int32_t>(mg, 0x655c);
    uint64_t **table = rd<uint64_t **>(mg, 0x6520);
    if (buckets <= 0 || !table) return 0;
    uint64_t *end = table[buckets], *node = table[h % static_cast<uint32_t>(buckets)];
    while (node && rd<uint32_t>(node, 0x10) != name) node = reinterpret_cast<uint64_t *>(*node);
    if (!node || node == end) return 0;
    uint8_t *tracker = reinterpret_cast<uint8_t *>(node) + 0x18;
    uint8_t *npc = handle_object(rd<uint8_t **>(tracker, 0), rd<uint32_t>(tracker, 8));
    if (!npc) return 0;
    const bool host = !rd<uint64_t>(party, 0x18) || rd<uint32_t>(party, 0x12e4) == rd<uint32_t>(party, 0x12e8);
    if (host) {
        uint32_t *slot = reinterpret_cast<uint32_t *>(tracker + 0x50), *dealt = reinterpret_cast<uint32_t *>(tracker + 0x80);
        for (int i = 0; i < 3; ++i) {
            if (slot[i] == id) { dealt[i] += damage; break; }
            if (!slot[i]) { slot[i] = id; dealt[i] = damage; break; }
        }
    }
    uint8_t *health_part = rd<uint8_t *>(npc, 0x310);
    if (!health_part) return 0;
    const int32_t health = virtual_call<int32_t>(health_part, 0x78);
    if (!host && static_cast<int64_t>(static_cast<uint64_t>(damage)) < static_cast<int64_t>(health)) return 0;
    const uint8_t dead_before = virtual_call<uint8_t>(npc, 0x3a8) != 0;
    virtual_call<void>(npc, 0x5e0, damage);
    const uint8_t dead_after = virtual_call<uint8_t>(npc, 0x3a8) != 0;
    ++ext_ea()[46];
    if (dead_before == dead_after || !host) return 0;
    alignas(16) uint8_t info[0x90];
    for (uint8_t &b : info) b = 0;
    *reinterpret_cast<uint32_t *>(info + 0x10) = 0x7fcccccc;
    *reinterpret_cast<uint32_t *>(info + 0x14) = 0x7fcccccc;
    *reinterpret_cast<uint32_t *>(info + 0x4c) = 0x3f800000;
    *reinterpret_cast<uint32_t *>(info + 0x6c) = 0x3f800000;
    game_call<void>(base + 0x55cb20, static_cast<void *>(info), attacker);
    int32_t t = at<int32_t>(base + 0x17b4a90 + 0x214);
    game_call<void>(base + 0x6ae7a0, attack, attacker, npc, &t, 0, 0, 0, 0, 0, static_cast<void *>(info));
    npc[0x70a] |= 1;
    ++ext_ea()[47];
    return 0;
}

// ===== v7.154: next batch, part 3 (segmented progress rings: world markers and the HUD counter) =====

extern "C" {
U3H_IMPORT uint32_t u3h_sincos(uint32_t x, uint32_t cosine);
U3H_IMPORT void u3h_ring_colour(uint32_t *col);
U3H_IMPORT uint32_t *u3h_t009_diag(void);
}

// PS3 1.19 draws a progress ring as n quads around a point: n = scale * ((1 - p) * 10 + p * 30) (p and scale
// clamped to 1, p below 0 counts as 0), each quad from radius rin to rout over p * 2 pi / n, starting at the top; uv
// from the sprite's centre, colour with the HUD alpha, through the 2D quad draw 0xe42310 with the sprite's material.
// Float maths as PS3: single precision, with a*b+c done in double (the PPC fused multiply-add rounds once).
namespace {
constexpr uint32_t one = 0x3f800000u;
[[gnu::always_inline]] inline uint32_t f_div(uint32_t a, uint32_t b) {
    uint32_t r;
    __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tdivss %%xmm1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1");
    return r;
}
[[gnu::always_inline]] inline uint32_t f_madd(uint32_t a, uint32_t b, uint32_t c) {   // a * b + c, one rounding to float
    uint32_t r;
    __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\tmovd %3,%%xmm2\n\tcvtss2sd %%xmm0,%%xmm0\n\tcvtss2sd %%xmm1,%%xmm1\n\t"
            "cvtss2sd %%xmm2,%%xmm2\n\tmulsd %%xmm1,%%xmm0\n\taddsd %%xmm2,%%xmm0\n\tcvtsd2ss %%xmm0,%%xmm0\n\tmovd %%xmm0,%0"
            : "=r"(r) : "r"(a), "r"(b), "r"(c) : "xmm0", "xmm1", "xmm2");
    return r;
}
// the number of quads
U3X_FN int32_t ring_segments(uint32_t scale, uint32_t p) {
    uint32_t pc = (p >> 31) ? 0 : p;   // PPC fsel: -0 stays -0
    if (p == 0x80000000u) pc = p;
    pc = f_less(f_sub(pc, one), 0) ? pc : one;
    const uint32_t count = f_madd(f_sub(one, pc), 0x41200000u, f_mul(pc, 0x41f00000u));
    const uint32_t m = f_less(f_sub(one, scale), 0) ? scale : one;
    return f_to_int(f_mul(m, count));
}
// draws n quads (1..512); false when the sprite has no material
U3X_FN bool draw_ring(uintptr_t b, uint32_t rin, uint32_t rout, uint32_t p, int32_t n, uint32_t depth, uint32_t x, uint32_t y,
                      const uint8_t *desc, const uint32_t *col) {
    constexpr uint32_t half = 0x3f000000u, uv_radius = 0x3ee66666u, two_pi = 0x40c90fdbu, half_pi = 0x3fc90fdbu;
    const uint64_t material = game_call<uint64_t>(b + 0x11b89b0, rd<uint64_t>(desc, 0), rd<uint32_t>(desc, 8));
    if (!material) return false;
    const uint32_t ratio = f_div(f_mul(rin, uv_radius), rout), sweep = f_mul(p, two_pi), inv = f_div(one, f_from_int(n));
    const uint32_t alpha = f_mul(col[3], at<uint32_t>(b + 0x16eafe0));
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t a0 = f_madd(inv, f_mul(f_from_int(i), sweep), half_pi), a1 = f_madd(inv, f_mul(f_from_int(i + 1), sweep), half_pi);
        const uint32_t c0 = u3h_sincos(a0, 1), s0 = u3h_sincos(a0, 0), c1 = u3h_sincos(a1, 1), s1 = u3h_sincos(a1, 0);
        alignas(16) uint32_t v[0x44];
        for (uint32_t &w : v) w = 0;
        const uint32_t cs[4] = {c0, c0, c1, c1}, sn[4] = {s0, s0, s1, s1};
        for (int k = 0; k < 4; ++k) {   // corners: inner, outer at angle 0, then outer, inner at angle 1
            const bool outer = k == 1 || k == 2;
            uint32_t *w = v + k * 16;
            w[0] = f_madd(cs[k], outer ? rout : rin, x);
            w[1] = f_madd(sn[k], outer ? rout : rin, y);
            w[3] = f_madd(cs[k], outer ? uv_radius : ratio, half);
            w[4] = f_madd(sn[k], outer ? uv_radius : ratio, half);
            w[8] = col[0];
            w[9] = col[1];
            w[10] = col[2];
            w[11] = alpha;
            w[12] = w[13] = w[14] = w[15] = one;
        }
        v[0x40] = depth;
        uint64_t ctx[6], td[4];
        for (uint64_t &q : ctx) q = 0;
        td[0] = material;
        td[1] = td[2] = td[3] = 0;
        game_call<void>(b + 0xe42310, ctx, td, static_cast<void *>(v));
    }
    return true;
}
}  // namespace
// t009: the world marker ring (rin = scale * 24, rout = scale * 33). t009d: [2] drawn, [3] no material, [5] last
// progress, [6] last count, [7] refused, [8] last scale.
extern "C" U3X_FN void u3x_marker_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t depth, uint32_t x, uint32_t y, const uint8_t *desc,
                                       const uint32_t *col) {
    uint32_t *d = u3h_t009_diag();
    const int32_t n = ring_segments(scale, p);
    d[6] = static_cast<uint32_t>(n);
    d[5] = p;
    d[8] = scale;
    if (n <= 0) return;
    if (n > 512) { ++d[7]; return; }
    if (!draw_ring(b, f_mul(scale, 0x41c00000u), f_mul(scale, 0x42040000u), p, n, depth, x, y, desc, col)) { ++d[3]; return; }
    ++d[2];
}
// the two marker sites (kinds 200 / 201): r = {r12, r15, rbp}; the element's progress (+0x44, below 0 or NaN: no
// ring), its colour (ARGB bytes to floats; ec[2] bit 5 keeps the old byte order), scale from the HUD frame
extern "C" U3X_FN int u3x_marker_ring_site(uintptr_t b, const uint64_t *r, int site) {
    uint32_t *d = u3h_t009_diag();
    ++d[site];
    const uint8_t *rbp = reinterpret_cast<const uint8_t *>(r[2]), *pl = reinterpret_cast<const uint8_t *>(site ? r[1] : r[0]);
    if (!aligned_pointer(rbp) || !aligned_pointer(pl)) { ++d[7]; return 0; }
    const uint64_t k = site ? r[0] : rd<uint64_t>(rbp - 0x1c0, 0);
    if (k >= 0x60u * 8u || k % 0x60u) { ++d[7]; return 0; }
    const uint8_t *e = pl + 0x10 + k;
    const uint32_t p = rd<uint32_t>(e, 0x44);
    if ((p >> 31) || ((p >> 23) & 0xff) == 0xff) return 0;
    const uint32_t c = rd<uint32_t>(e, 0x34);
    d[4] = c;
    uint32_t col[4] = {f_mul(f_from_int(static_cast<int32_t>(c >> 24)), 0x3b808081u), f_mul(f_from_int(static_cast<int32_t>((c >> 16) & 0xff)), 0x3b808081u),
                       f_mul(f_from_int(static_cast<int32_t>((c >> 8) & 0xff)), 0x3b808081u), f_mul(f_from_int(static_cast<int32_t>(c & 0xff)), 0x3b808081u)};
    if (!(ext_ec()[2] & 0x20u)) {   // PS3's ring colour is ARGB
        const uint32_t a = col[0];
        col[0] = col[1];
        col[1] = col[2];
        col[2] = col[3];
        col[3] = a;
    }
    u3h_ring_colour(col);
    u3x_marker_ring(b, f_mul(rd<uint32_t>(rbp - 0x254, 0), 0x3fe00000u), p, site ? one : 0, rd<uint32_t>(rbp - 0x1a0, 0), rd<uint32_t>(rbp - 0x19c, 0), pl + 0x5d0, col);
    return 0;
}
// b024: the HUD counter ring (rin = scale * r1, rout = scale * r2). eb[211] drawn, eb[212] refused.
extern "C" U3X_FN void u3x_counter_ring(uintptr_t b, uint32_t scale, uint32_t p, uint32_t r1, uint32_t r2, uint32_t depth, uint32_t x, uint32_t y,
                                        const uint8_t *desc, const uint32_t *col) {
    uint32_t *eb = ext_eb();
    const int32_t n = ring_segments(scale, p);
    if (n <= 0) return;
    if (n > 512) { ++eb[212]; return; }
    if (!draw_ring(b, f_mul(scale, r1), f_mul(scale, r2), p, n, depth, x, y, desc, col)) { ++eb[212]; return; }
    ++eb[211];
}
namespace {
// a game function of one pointer argument returning a float
U3X_FN uint32_t call_float1(uintptr_t fn, uint64_t a0) {
    uint32_t r;
    __asm__ volatile("mov %%rsp,%%r12\n\tand $-16,%%rsp\n\tcall *%2\n\tmov %%r12,%%rsp\n\tmovd %%xmm0,%0"
                     : "=r"(r), "+D"(a0)
                     : "r"(fn)
                     : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "r12", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6",
                       "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
    return r;
}
}  // namespace
// the counter ring's site: r = {r15, r14, r13, r12, rbx, ..., [14] rbp}; progress from 0x745240 (net game manager),
// colour from the HUD element state; the three game functions must start with push rbp / mov rbp, rsp
extern "C" U3X_FN int u3x_counter_ring_site(uintptr_t b, const uint64_t *r) {
    uint32_t *eb = ext_eb();
    ++eb[210];
    const uint8_t *dc = reinterpret_cast<const uint8_t *>(r[4]), *rbp = reinterpret_cast<const uint8_t *>(r[14]);
    if (!aligned_pointer(dc) || !aligned_pointer(rbp)) { ++eb[212]; return 0; }
    const uint8_t *hud = rd<const uint8_t *>(rbp - 0x11a8, 0), *st = rd<const uint8_t *>(dc, 0x128);
    if (!aligned_pointer(hud) || !aligned_pointer(st)) { ++eb[212]; return 0; }
    if (at<uint32_t>(b + 0x745240) != 0xe5894855u || at<uint32_t>(b + 0x11b89b0) != 0xe5894855u || at<uint32_t>(b + 0xe42310) != 0xe5894855u) {
        ++eb[212];
        return 0;
    }
    const uint32_t p = call_float1(b + 0x745240, b + 0x17cb060);
    eb[213] = p;
    uint32_t col[4] = {rd<uint32_t>(st, 0x18), rd<uint32_t>(st, 0x1c), rd<uint32_t>(st, 0x20), rd<uint32_t>(st, 0x24)};
    u3h_ring_colour(col);
    u3x_counter_ring(b, 0x3ef33333u, p, 0x41f00000u, 0x42480000u, 0, rd<uint32_t>(dc, 0x120), rd<uint32_t>(dc, 0x124), hud + 0x1a90 + 0x5d0, col);
    return 0;
}

// ===== v7.155: next batch, part 4 (lobby scene, connection monitor) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_netgame_entry(uintptr_t base, void *s);
U3H_IMPORT int u3h_coop(uintptr_t base, void *s);
U3H_IMPORT uint32_t *u3h_scene_was(void);
U3H_IMPORT uint8_t *u3h_cm_entry(uintptr_t base, int i);
U3H_IMPORT uint32_t u3h_members(uint8_t *session, uint8_t **out);
U3H_IMPORT int u3h_link_timeout(uintptr_t base, uint8_t *entry);
U3H_IMPORT void u3h_sync_request(uintptr_t base);
}

// b034: the lobby's 3D MenuScene each frame, in PS3 1.19's order: leave it (0x5b7040(scene, 1)) while the menu is
// not active or a cinematic camera (0xb7b3c967; handle 0x1b3ee38, eb[1] bit 8: 0x1bb9500) holds the view; else
// update it and enter it unless a popup is up while joining / with a co-op hunter player, no player 0 exists, or
// the co-op hunter's look is already on the scene character. In team modes (0x17d1868 == 2) the team pass
// 0x5b6e40 runs. eb[1] bit 3 turns it off. eb[152..155] calls, leaves, enters, team passes; eb[28]/[29] leave reasons.
namespace {
U3X_FN bool coop_hunter(uintptr_t base, void *s, const uint8_t *p) {   // a co-op player of team 1
    if (!at<uint8_t>(base + 0x17d5c61) || !u3h_coop(base, s)) return false;
    return rd<int32_t>(p, 0x21c) == 1;
}
U3X_FN bool scene_free(uintptr_t base, void *s) {   // not a mode 11 game and player 0 not a co-op hunter
    const uint8_t *e = u3h_netgame_entry(base, s);
    if (e && rd<int32_t>(e, 0xc) == 11) return false;
    const uint8_t *p = at<const uint8_t *>(base + 0x17cb060 + 0x370);
    if (!p) return false;
    return !coop_hunter(base, s, p);
}
}  // namespace
extern "C" U3X_FN int u3x_scene_hunter(uintptr_t base, void *s, const uint8_t *p) { return coop_hunter(base, s, p); }
extern "C" U3X_FN int u3x_scene_free(uintptr_t base, void *s) { return scene_free(base, s); }
extern "C" U3X_FN int u3x_scene_character(uintptr_t scene) {
    return handle_object(rd<uint8_t **>(reinterpret_cast<const void *>(scene), 0x1460), rd<uint32_t>(reinterpret_cast<const void *>(scene), 0x1468)) != nullptr;
}
extern "C" U3X_FN int u3x_scene_busy(uintptr_t base) {
    uint8_t *o = at<uint8_t *>(base + 0x16fd088);
    if (!o || !*reinterpret_cast<void **>(o)) return 0;
    return virtual_call<bool>(o, 7 * 8);
}
extern "C" U3X_FN void u3x_lobby_scene(uintptr_t base, void *s) {
    uint32_t *eb = ext_eb();
    if (eb[1] & 8) return;
    const uintptr_t scene = base + 0x17bba90;
    const uint8_t *menu = reinterpret_cast<const uint8_t *>(base + 0x17bd080);
    uint8_t *camera = reinterpret_cast<uint8_t *>(base + 0x17f9938);
    ++eb[152];
    const bool active = rd<int32_t>(menu, 8) != 0 && rd<int32_t>(menu, 0xc) == 0;
    int reason = active ? 0 : 1;
    if (!reason) {
        const uintptr_t handle = (eb[1] & 0x100) ? 0x1bb9500 : 0x1b3ee38;
        const uint8_t *cam = handle_object(at<uint8_t **>(base + handle), at<uint32_t>(base + handle + 8));
        if (cam && rd<uint32_t>(cam, 0x30) == 0xb7b3c967u) reason = 2;
    }
    if (reason) {
        game_call<void>(base + 0x5b7040, scene, 1);
        ++eb[154];
        ++eb[reason == 1 ? 28 : 29];
        *u3h_scene_was() = 0;
    } else {
        game_call<void>(base + 0x5b4e60, scene);
        bool enter = true;
        if (camera[0xf70]) {
            const uint8_t *popup = rd<const uint8_t *>(menu, 0x898);
            if (popup && rd<uint32_t>(popup, 0x10) && (at<uint8_t>(base + 0x17d7761) || !scene_free(base, s))) enter = false;
            if (enter) {
                uint8_t *p = at<uint8_t *>(base + 0x17cb060 + 0x370);
                if (!p) {
                    enter = false;
                } else if (coop_hunter(base, s, p)) {
                    void *look = game_call<void *>(base + 0x784b20, p, 0u);
                    if (game_call<bool>(base + 0x996a60, static_cast<void *>(camera), look)) enter = false;
                }
            }
        }
        if (enter) {
            game_call<void>(base + 0x5b7020, scene);
            ++eb[153];
            if (at<int32_t>(base + 0x17d1868) == 2) ++eb[157];
        }
    }
    if (at<int32_t>(base + 0x17d1868) == 2) {
        game_call<void>(base + 0x5b6e40, scene);
        ++eb[155];
    }
}
extern "C" U3X_FN void u3x_lobby_scene_shutdown(uintptr_t base) {
    if (!base || (ext_eb()[1] & 8)) return;
    const uintptr_t scene = base + 0x17bba90;
    game_call<void>(base + 0x5b7040, scene, 1);
    game_call<void>(base + 0x5b6e40, scene);
    ++ext_eb()[231];
}

// b013: the connection monitor (PS3 1.19 0x286640): the match host drops members whose connection entry is gone or
// flagged; then the time sync request; then per remote player: refresh its entry's last-seen time, or mark it gone
// when it is the main player without a live entry; a player whose peer link timed out is kicked by the host (a
// guest flags the entry). Connection entries: 32 of 0x220 bytes {id, +4 active, +5 dropped, +6 lost, +0x34 handle,
// +0xe0 last seen}. eb[120] runs, eb[121..125] kicks, gone, refreshes, timeout kicks, timeout flags.
extern "C" U3X_FN uint8_t *u3x_entry_by_id(uintptr_t base, int32_t id) {
    for (int i = 0; i < 32; ++i) {
        uint8_t *e = u3h_cm_entry(base, i);
        if (rd<int32_t>(e, 0) == id) return e;
    }
    return nullptr;
}
// 2 while the entry is active with a peer link handle, else -1
extern "C" U3X_FN int32_t u3x_connection_state(uintptr_t base, int32_t id) {
    const uint8_t *e = u3x_entry_by_id(base, id);
    if (!e || !e[4] || !rd<int32_t>(e, 0x34)) return -1;
    return 2;
}
extern "C" U3X_FN int u3x_session_host(const uint8_t *s) { return !rd<uint64_t>(s, 0x18) || rd<uint32_t>(s, 0x12e4) == rd<uint32_t>(s, 0x12e8); }
extern "C" U3X_FN void u3x_session_kick(uint8_t *s, uint32_t id) {
    uint8_t *backend = rd<uint8_t *>(s, 8);
    if (!backend || !*reinterpret_cast<uintptr_t **>(backend)) return;
    virtual_call<int64_t>(backend, 9 * 8, s, id, 0);
}
namespace {
U3X_FN bool local_player(const uint8_t *mgr, const uint8_t *p) { return p == rd<const uint8_t *>(mgr, 0x370) || p == rd<const uint8_t *>(mgr, 0x378); }
U3X_FN bool main_player(const uint8_t *mgr, const uint8_t *p) {
    const uint8_t *m = rd<const uint8_t *>(mgr, 0x380);
    return m && net_player_id(m) == net_player_id(p);
}
int32_t clock_now(const uint8_t *clock) { return clock ? rd<int32_t>(clock, 0xc) : 0; }
}  // namespace
extern "C" U3X_FN void u3x_members_check(uintptr_t base, int arg) {
    uint8_t *s = reinterpret_cast<uint8_t *>(base + 0x17d6420);
    if (!u3x_session_host(s) || !arg) return;
    uint8_t *list[10];
    const uint32_t n = u3h_members(s, list);
    const uint8_t *self = rd<uint64_t>(s, 0x18) ? u3x_find_member(s, rd<uint32_t>(s, 0x12e4)) : nullptr;
    for (uint32_t k = 0; k < n; ++k) {
        const uint8_t *m = list[k];
        if (m == self) continue;
        const int32_t connection = rd<int32_t>(m, 0x34);
        const int32_t state = u3x_connection_state(base, connection);
        const uint8_t *e = u3x_entry_by_id(base, connection);
        if (e ? !e[5] : state != -1) continue;
        u3x_session_kick(s, rd<uint32_t>(m, 0x2c));
        ++ext_eb()[121];
    }
}
extern "C" U3X_FN void u3x_players_check(uintptr_t base) {
    uint32_t *eb = ext_eb();
    uint8_t *mgr = reinterpret_cast<uint8_t *>(base + 0x17cb060), *s = reinterpret_cast<uint8_t *>(base + 0x17d6420);
    const uint8_t *clock = at<const uint8_t *>(base + 0x17c3580);
    for (int i = 0; i < 10; ++i) {
        uint8_t *p = mgr + 0x390 + i * 0x9c0;
        if (local_player(mgr, p)) continue;
        if (!p[0xe1] || p[0x5e4]) continue;
        if (!net_player_id(p)) continue;
        const int32_t connection = rd<int32_t>(p, 0x5b4);
        if (connection == -1) continue;
        uint8_t *e = u3x_entry_by_id(base, connection);
        if (e && !e[6]) {
            *reinterpret_cast<int32_t *>(e + 0x40 + 0xa0) = clock_now(clock);
            ++eb[123];
        } else if (main_player(mgr, p)) {
            p[0x5e4] = 1;
            *reinterpret_cast<uint32_t *>(p + 0x1e4) = 0;
            ++eb[122];
        } else if (e) {
            *reinterpret_cast<int32_t *>(e + 0x40 + 0xa0) = clock_now(clock);
            ++eb[123];
        }
        if (!p[0xe1] || !net_player_id(p)) continue;
        if (!e) continue;
        const uint8_t *game = at<const uint8_t *>(base + 0x1b24b80);
        if (at<int32_t>(base + 0x17d5c58 + 0x40) && !(game && game[0x3a04])) continue;
        if (e[6]) continue;
        if (!u3h_link_timeout(base, e)) continue;
        if (local_player(mgr, p)) continue;
        if (p[0x1ec]) continue;
        if (u3x_session_host(s)) {
            u3x_session_kick(s, rd<uint32_t>(p, 0x1e4));
            ++eb[124];
        } else {
            e[6] = 1;
            ++eb[125];
        }
    }
}
extern "C" U3X_FN void u3x_connection_monitor(uintptr_t base, int arg) {
    ++ext_eb()[120];
    u3x_members_check(base, arg);
    u3h_sync_request(base);
    u3x_players_check(base);
}

// ===== v7.156: next batch, part 5 (player state sender, film cameras) =====

extern "C" {
U3H_IMPORT uint32_t u3h_player_fill(uintptr_t base, void *s, uint8_t *msg, uint8_t *player);
U3H_IMPORT uint8_t *u3h_camera_manager(uintptr_t base);
U3H_IMPORT uint64_t u3h_rp_fcall(uint64_t *r);
}

// c008: PS3 1.19 sends the local player's state (message type 0x10, 0x240 bytes, vtable 0x16d5b90) to remote players
// at a rate from their distance: 15 per second within 15 m down to 8 at 30 m (a split-screen pair counts its nearer
// player); the next send time per remote player and screen is at +0x5f0. The message is built in heap 0x14 inside
// the network allocator's context, filled (anim bits sent as whole 32-bit words) and released after the send; what
// the player rides goes through PS4's NPC sender. ec[0] bit 8 sends nothing. ec[60] runs, [61] built, [62]/[63] bit
// count last / most, [64] sent, [65] last mask, [73] ride sends.
namespace {
[[gnu::always_inline]] inline uint32_t f_root(uint32_t a) {
    uint32_t r;
    __asm__("movd %1,%%xmm0\n\tsqrtss %%xmm0,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(a) : "xmm0");
    return r;
}
// distance between two positions; 0 below FLT_MIN squared
U3X_FN uint32_t distance(const uint8_t *a, const uint8_t *b) {
    const uint32_t dx = f_sub(rd<uint32_t>(a, 0), rd<uint32_t>(b, 0)), dy = f_sub(rd<uint32_t>(a, 4), rd<uint32_t>(b, 4)),
                   dz = f_sub(rd<uint32_t>(a, 8), rd<uint32_t>(b, 8));
    const uint32_t d2 = f_add(f_add(f_mul(dx, dx), f_mul(dy, dy)), f_mul(dz, dz));
    return f_less(0x00800000u, d2) ? f_root(d2) : 0;
}
U3X_FN void send_player_state(uintptr_t base, void *s, uint8_t *player, uint32_t mask) {
    uint32_t *ec = ext_ec();
    {
        uint32_t heap = 0x14;
        game_call<void>(base + 0xda1590, &heap, base + 0x13ac250, uint64_t{0}, base + 0x13ac250);   // context push
    }
    uint32_t tag[2];
    tag[0] = 0x14;
    tag[1] = 0;
    uint8_t *m = game_call<uint8_t *>(base + 0xd9e990, uint64_t{0x240}, static_cast<void *>(tag), uint64_t{0x10});
    if (m) {
        // the message constructor's fields
        m[9] = 2;
        m[8] = 0x10;
        m[0xa] = 1;
        m[0xb] = 1;
        m[0xc] = 1;
        m[0xd] = 0;
        *reinterpret_cast<uint64_t *>(m) = base + 0x16d5b90;
        *reinterpret_cast<uint32_t *>(m + 0x170) = 0x7fccccccu;
        *reinterpret_cast<uint32_t *>(m + 0x174) = 0x7fccccccu;
        *reinterpret_cast<uint64_t *>(m + 0x178) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x180) = 0;
        for (int i = 0; i < 0x40; ++i) m[0x190 + i] = 0;
        *reinterpret_cast<uint32_t *>(m + 0x1ac) = 0x3f800000u;
        *reinterpret_cast<uint32_t *>(m + 0x1cc) = 0x3f800000u;
        *reinterpret_cast<uint32_t *>(m + 0x1d4) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x148) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x154) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x158) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x1d0) = 0;
        for (int i = 0; i < 0x10; ++i) m[0x1f0 + i] = 0;
        *reinterpret_cast<uint32_t *>(m + 0x200) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x204) = 1;
        *reinterpret_cast<uint32_t *>(m + 0x208) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x20c) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x1d8) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x15c) = 0;
        m[0x1de] = 0;
        *reinterpret_cast<uint16_t *>(m + 0x1dc) = 0;
        m[0x1e0] = 0;
        *reinterpret_cast<uint32_t *>(m + 0x1e4) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x1ec) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x1e8) = 0;
        *reinterpret_cast<uint32_t *>(m + 0x210) = 0;
        m[0x230] = 0;
        m[0x231] = 0;
        m[0x232] = 0xff;
        m[0x233] = 0xff;
        m[0x234] = 0;
        m[0x235] = 0;
        for (int i = 0; i < 0x10; ++i) m[0x220 + i] = 0;
        uint32_t bits = u3h_player_fill(base, s, m, player);
        if (bits) {   // whole words: PS4's packer keeps a partial word's bits in its high bytes
            bits = (bits + 31) & ~31u;
            *reinterpret_cast<uint32_t *>(m + 0x144) = bits;
        }
        ++ec[61];
        ec[62] = bits;
        if (bits > ec[63]) ec[63] = bits;
        if (bits) {
            game_call<void>(base + 0x7336e0, base + 0x17cb060, m, mask);
            ++ec[64];
            ec[65] = mask;
        }
        if (--m[0xa] == 0) virtual_call<void>(m, 8);
    }
    game_call<void>(base + 0xda18b0);   // context pop
}
}  // namespace
extern "C" U3X_FN void u3x_send_player_state(uintptr_t base, void *s, uint8_t *player, uint32_t mask) {
    if (player) send_player_state(base, s, player, mask);
}
extern "C" U3X_FN uint32_t u3x_distance(const uint8_t *a, const uint8_t *b) { return distance(a, b); }
extern "C" U3X_FN void u3x_replication(uintptr_t base, void *s, uint8_t *np, uint8_t *player) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 256) return;
    if (!player || !rd<uint8_t *>(player, 0x138)) return;
    ++ec[60];
    uint8_t *mgr = reinterpret_cast<uint8_t *>(base + 0x17cb060);
    uint32_t mask = 0;
    const uint32_t now = f_mul(f_from_int(at<int32_t>(base + 0x17b4ca4)), 0x3b5a740eu);   // net clock / 300
    const int32_t screen = rd<int32_t>(np, 0x1e8);
    for (int i = 0; i < 10; ++i) {
        if (!at<uint8_t>(base + 0x17d5c61)) continue;
        uint8_t *q = mgr + 0x390 + i * 0x9c0;
        if (!q[0xe1] || q[0x5e4]) continue;
        uint32_t d = distance(q, np);
        if (rd<int32_t>(q, 0x1e8) == 1) continue;   // a second local screen's player
        {   // its split-screen partner, when nearer
            const uint32_t low = rd<uint32_t>(q, 0x1e4) & 0xfff, partner = low | (rd<int32_t>(q, 0x1e8) == 0 ? 0x1000u : 0);
            if (partner)
                for (int j = 0; j < 10; ++j) {
                    const uint8_t *p2 = mgr + 0x390 + j * 0x9c0;
                    if (net_player_id(p2) == partner) {
                        const uint32_t d2 = distance(p2, np);
                        if (!f_less(f_sub(d, d2), 0)) d = d2;
                        break;
                    }
                }
        }
        uint32_t *slot = reinterpret_cast<uint32_t *>(q + 0x5f0 + 4 * static_cast<int64_t>(screen));
        if (screen < 0 || screen > 1) continue;
        if (f_less(now, *slot)) continue;
        mask |= 1u << i;
        uint32_t f = f_mul(f_sub(d, 0x41700000u), 0x3d888889u);   // (d - 15) / 15
        if (f_less(f, 0)) f = 0;
        if (!f_less(f, 0x3f800000u)) f = 0x3f800000u;
        *slot = f_add(now, f_div(0x3f800000u, f_add(f_mul(f_sub(0x3f800000u, f), 0x41700000u), f_mul(f, 0x41000000u))));
    }
    if (!mask) return;
    send_player_state(base, s, player, mask);
    uint8_t *ride = rd<uint8_t *>(player, 0x6c0);
    if (ride && virtual_call<uint8_t *>(ride, 0x58)) {
        uint8_t *o = virtual_call<uint8_t *>(ride, 0x58);
        game_call<void>(base + 0x6ae2a0, o, mask);
        ++ec[73];
    }
}

// b003 film cameras: spawning one (camera manager 0x101b530 with the type, a 0x60 parameter block: fov, locator when
// given, target handle), re-recording the camera into the re-record stream, and the two camera types PS4 dropped
// (kCameraCinemaFollow / kCameraCinemaFixed, built as PS4 builds kCameraCinema: name, class name hash, parameters,
// size 0x60, flags 0x20000000, 2).
namespace {
U3X_BSS uint8_t camera_types[2][0x28];
U3X_RO const char follow_type[] = "kCameraCinemaFollow", fixed_type[] = "kCameraCinemaFixed";
U3X_RO const char follow_class[] = "CameraControlCinemaFollow", fixed_class[] = "CameraControlCinemaFixed";
}
extern "C" U3X_FN void u3x_camera_spawn(uintptr_t base, uint8_t *own, uint64_t type, uint32_t fov, const void *locator, const uint8_t *target) {
    alignas(16) uint32_t params[24];
    for (uint32_t &w : params) w = 0;
    params[0] = 0x30000000u;
    params[3] = fov;
    params[4] = params[5] = 0xbf800000u;
    if (locator)
        for (int k = 0; k < 8; ++k) params[8 + k] = static_cast<const uint32_t *>(locator)[k];
    if (target) {
        *reinterpret_cast<uint64_t *>(params + 16) = rd<uint64_t>(target, 0x70);
        params[18] = rd<uint32_t>(target, 0x7c);
    }
    reinterpret_cast<uint8_t *>(params)[0x50] = locator ? 1 : 0;
    uint64_t ty = type, arg = 0x200000000ull;
    uint8_t *o = game_call<uint8_t *>(base + 0x101b530, u3h_camera_manager(base), &ty, &arg, static_cast<void *>(params));
    *reinterpret_cast<uint64_t *>(own + 0x1a0) = o ? rd<uint64_t>(o, 0x70) : 0;
    *reinterpret_cast<uint32_t *>(own + 0x1a8) = o ? rd<uint32_t>(o, 0x7c) : 0;
}
// the camera locator's frame through 0x6c1d60 (speed, mode, followed index, frame) into the re-record stream
extern "C" U3X_FN void u3x_camera_rerecord(uintptr_t base, uint8_t *own) {
    if (!at<uint32_t>(base + 0x17c5cf8 + 8)) return;
    alignas(16) uint8_t bf[0x60];
    u3x_frame_make(bf, reinterpret_cast<const uint8_t *>(base + 0x16ea550));
    at<uintptr_t>(base + 0x1b68840) = base + 0x17c3850;
    uint64_t r[14];
    for (uint64_t &w : r) w = 0;
    r[0] = base + 0x6c1d60;
    r[7] = at<uint32_t>(base + 0x16e716c);
    r[1] = static_cast<uint8_t>(at<int32_t>(base + 0x17c5a10));
    r[2] = own[0x1b8];
    r[3] = reinterpret_cast<uint64_t>(bf);
    u3h_rp_fcall(r);
    at<uintptr_t>(base + 0x1b68840) = 0;
}
extern "C" U3X_FN uint64_t u3x_camera_type(uintptr_t base, int fixed) {
    uint8_t *t = camera_types[fixed];
    if (!rd<uint64_t>(t, 0)) {
        *reinterpret_cast<uint64_t *>(t) = reinterpret_cast<uint64_t>(fixed ? fixed_type : follow_type);
        *reinterpret_cast<uint32_t *>(t + 8) = game_call<uint32_t>(base + 0xd94550, 0, fixed ? fixed_class : follow_class, 1);
        *reinterpret_cast<uint64_t *>(t + 0x10) = base + (fixed ? 0x1b5b6c0 : 0x1b5b4d0);
        *reinterpret_cast<uint32_t *>(t + 0x18) = 0x60;
        *reinterpret_cast<uint32_t *>(t + 0x1c) = 0x20000000u;
        *reinterpret_cast<uint32_t *>(t + 0x20) = 2;
    }
    return reinterpret_cast<uint64_t>(t);
}
// the Cinema camera's locator: position +0x1e0, rotation +0x1f0, set flag +0x224
extern "C" U3X_FN void u3x_camera_cinema_set(uint8_t *c, const void *locator) {
    const uint8_t *l = static_cast<const uint8_t *>(locator);
    for (int k = 0; k < 0x10; ++k) c[0x1e0 + k] = l[k];
    c[0x224] = 1;
    for (int k = 0; k < 0x10; ++k) c[0x1f0 + k] = l[0x10 + k];
}

// ===== v7.157: next batch, part 6 (the friend list and the presence data) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_friend_storage(int which);
U3H_IMPORT int32_t u3h_friend_count(uintptr_t base);
U3H_IMPORT int32_t u3h_friend_get(uintptr_t base, uint32_t i, uint8_t *id, uint8_t *presence);
U3H_IMPORT int32_t u3h_presence_set(uintptr_t base, const uint8_t *details, uint32_t options);
U3H_IMPORT void u3h_fr_sort(uint8_t *entries, int64_t n);
U3H_IMPORT void u3h_fr_strncpy(char *d, const char *s, uint32_t n);
U3H_IMPORT uint32_t u3h_fr_key(uintptr_t base);
U3H_IMPORT uint8_t *u3h_lg_profile(uintptr_t base, uint32_t user);
U3H_IMPORT uint8_t *u3h_lg_rec(uintptr_t base, void *s, uint8_t *profile);
U3H_IMPORT int u3h_match_end_time(uintptr_t base, void *s, int64_t *out);
}

// The friend list (PS3 1.19 NetFriends at 0x17caeb0): count +0, capacity +4, entries +8 (0x240 bytes: +0 refresh flag,
// +1 id, +0x28 presence details 0x190), players met +0x10/+0x14/+0x18, the list thread +0xc8, the published presence
// data +0x60 (0x60 bytes), the next presence time +0xc0. ea[1] bit 5 turns friends off. ea[64..71] init, adds,
// presence failures, refreshes, count failures, shutdowns, presence sets, presence failures.
namespace {
constexpr uint32_t entry_bytes = 0x240;
bool friends_off() { return (ext_ea()[1] & 0x20u) != 0; }
U3X_RO const uint16_t presence_stats[8] = {0x5a4, 0x5a8, 0x5ac, 0x5b0, 0x5b4, 0x5b8, 0x5c0, 0x5bc};   // profile record words
}  // namespace
// add a friend (or flag a known one for a refresh); full: nothing
extern "C" U3X_FN void u3x_friend_add(uintptr_t base, const uint8_t *id, const uint8_t *presence) {
    uint8_t *nf = friends(base);
    const uint32_t n = rd<uint32_t>(nf, 0);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t *e = u3h_fr_ent(base, i);
        if (u3h_fr_strcmp(base, id, e + 1) == 0) { e[0] = 1; return; }
    }
    if (n >= rd<uint32_t>(nf, 4)) return;
    *reinterpret_cast<uint32_t *>(nf) = n + 1;
    uint8_t *e = u3h_fr_ent(base, n);
    for (int k = 0; k < 0x24; ++k) e[1 + k] = id[k];
    if (presence)
        for (int k = 0; k < 0x190; ++k) e[0x28 + k] = presence[k];
    e[0] = presence == nullptr;
    ++ext_ea()[65];
}
extern "C" U3X_FN void u3x_friend_refresh_one(uintptr_t base, uint8_t *e) {
    for (int k = 0; k < 0x190; ++k) e[0x28 + k] = 0;
    if (u3h_presence(base, e + 1, e + 0x28) < 0) ++ext_ea()[66];
    e[0] = 0;
}
extern "C" U3X_FN void u3x_friend_refresh(uintptr_t base) {
    uint8_t *nf = friends(base);
    if (rd<uint64_t>(nf, 0xc8)) return;
    const uint32_t n = rd<uint32_t>(nf, 0);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t *e = u3h_fr_ent(base, i);
        if (e[0]) u3x_friend_refresh_one(base, e);
    }
    u3h_fr_sort(rd<uint8_t *>(nf, 8), static_cast<int64_t>(n));
    ++ext_ea()[67];
}
// the list thread's work: only while the list is empty, every friend (up to the capacity) with its presence
extern "C" U3X_FN void u3x_friend_list_thread(uintptr_t base) {
    uint8_t *nf = friends(base);
    if (rd<uint32_t>(nf, 0) == 0) {
        int32_t c = u3h_friend_count(base);
        if (c < 0) {
            ++ext_ea()[68];
            c = 0;
        }
        *reinterpret_cast<uint32_t *>(nf) = 0;
        uint32_t n = static_cast<uint32_t>(c);
        if (rd<uint32_t>(nf, 4) < n) n = rd<uint32_t>(nf, 4);
        for (uint32_t i = 0; i < n; ++i) {
            uint8_t id[0x24], pr[0x190];
            for (uint8_t &b : id) b = 0;
            for (uint8_t &b : pr) b = 0;
            u3h_friend_get(base, i, id, pr);
            u3x_friend_add(base, id, pr);
        }
    }
    *reinterpret_cast<uint64_t *>(nf + 0xc8) = 0;
}
// set up at NetInit: 100 friends, 50 players met, 16 new names, the two detail blocks; the list thread starts
extern "C" U3X_FN void u3x_friend_init(uintptr_t base) {
    if (friends_off()) return;
    uint8_t *nf = friends(base);
    uint8_t *ent = u3h_friend_storage(0), *met = u3h_friend_storage(1), *fresh = u3h_friend_storage(2), *b0 = u3h_friend_storage(3),
            *b1 = u3h_friend_storage(4);
    *reinterpret_cast<uint32_t *>(nf + 4) = 100;
    *reinterpret_cast<uint32_t *>(nf) = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        *reinterpret_cast<uint64_t *>(ent + i * entry_bytes + 0x1b8) = 0;
        ent[i * entry_bytes + 0x1c0] = 0;
    }
    *reinterpret_cast<uint8_t **>(nf + 8) = ent;
    *reinterpret_cast<uint32_t *>(nf + 0x10) = 0;
    *reinterpret_cast<uint32_t *>(nf + 0x14) = 50;
    for (uint32_t i = 0; i < 50; ++i) *reinterpret_cast<uint32_t *>(met + i * 0x28 + 0x24) = 0;
    *reinterpret_cast<uint8_t **>(nf + 0x18) = met;
    *reinterpret_cast<uint32_t *>(nf + 0xe8) = 0x10;
    *reinterpret_cast<uint32_t *>(nf + 0xec) = 0;
    *reinterpret_cast<uint64_t *>(nf + 0xe0) = 0;
    *reinterpret_cast<uint8_t **>(nf + 0xf0) = fresh;
    *reinterpret_cast<uint8_t **>(nf + 0x48) = b0;
    *reinterpret_cast<uint32_t *>(b0 + 0x1b0) = 0;
    *reinterpret_cast<uint8_t **>(nf + 0x50) = b1;
    *reinterpret_cast<uint32_t *>(b1 + 0x1b0) = 0;
    *reinterpret_cast<uint32_t *>(nf + 0x5c) = 0;
    *reinterpret_cast<uint32_t *>(nf + 0x58) = 0;
    for (int k = 0; k < 0x60; ++k) nf[0x60 + k] = 0;
    nf[0x20] = 1;
    *reinterpret_cast<uint32_t *>(nf + 0xc0) = 0;
    *reinterpret_cast<uint64_t *>(nf + 0xc8) = 1;   // the list thread runs in the next tick
    ++ext_ea()[64];
    *reinterpret_cast<uint64_t *>(nf + 0xf8) = 0;
}
namespace {
// presence details: the title text (0x80), then optional data
U3X_FN void title_details(uintptr_t base, void *s, uint8_t *det) {
    for (int k = 0; k < 0x190; ++k) det[k] = 0;
    const char *t = u3h_text(base, s, 0xfd2de693u);
    if (t) u3h_fr_strncpy(reinterpret_cast<char *>(det), t, 0x80);
    det[0x7f] = 0;
}
}  // namespace
// clear the published data and publish the title-only presence
extern "C" U3X_FN void u3x_presence_clear(uintptr_t base, void *s) {
    uint8_t *nf = friends(base);
    for (int k = 0; k < 0x60; ++k) nf[0x60 + k] = 0;
    uint8_t d[0x190];
    for (uint8_t &b : d) b = 0;
    const char *t = u3h_text(base, s, 0xfd2de693u);
    if (t) {
        u3h_fr_strncpy(reinterpret_cast<char *>(d), t, 0x80);
        d[0x7f] = 0;
        u3h_presence_set(base, d, 3);
    }
}
extern "C" U3X_FN void u3x_friend_shutdown(uintptr_t base, void *s) {
    if (friends_off()) return;
    uint8_t *nf = friends(base);
    if (!rd<uint8_t *>(nf, 8)) return;
    if (rd<uint64_t>(nf, 0xc8)) u3x_friend_list_thread(base);
    u3x_presence_clear(base, s);
    *reinterpret_cast<uint64_t *>(nf + 8) = 0;
    *reinterpret_cast<uint32_t *>(nf + 4) = 0;
    *reinterpret_cast<uint32_t *>(nf) = 0;
    *reinterpret_cast<uint64_t *>(nf + 0x18) = 0;
    *reinterpret_cast<uint32_t *>(nf + 0x14) = 0;
    *reinterpret_cast<uint32_t *>(nf + 0x10) = 0;
    *reinterpret_cast<uint64_t *>(nf + 0x48) = 0;
    *reinterpret_cast<uint64_t *>(nf + 0x50) = 0;
    for (int k = 0; k < 0x24; ++k) nf[0x21 + k] = 0;
    *reinterpret_cast<uint32_t *>(nf + 0x5c) = 0;
    ++ext_ea()[69];
}
// the 0x60-byte presence data every 1500 clock units while online: key, party lock, party id, party size, menu mode,
// rank and XP and 8 statistics of profile 0, and what the player is doing (0 menus / searching, 2 in a match with
// teams' scores, net game, map and the match end time, 3 between rounds); published only when it changed, a failed
// publish retries after 3000.
extern "C" U3X_FN void u3x_presence_tick(uintptr_t base, void *s) {
    uint8_t *nf = friends(base);
    const uint8_t *net = reinterpret_cast<const uint8_t *>(base + 0x17d5c58);
    if (net[8]) return;
    const int32_t now = at<int32_t>(base + 0x17b4a90 + 0x114);
    if (now < rd<int32_t>(nf, 0xc0)) return;
    *reinterpret_cast<int32_t *>(nf + 0xc0) = now + 1500;
    uint8_t d[0x60];
    for (uint8_t &b : d) b = 0;
    uint8_t *ses = reinterpret_cast<uint8_t *>(base + 0x17dc298);
    *reinterpret_cast<uint32_t *>(d) = u3h_fr_key(base);
    d[7] = (rd<uint32_t>(ses, 0x204) & 1) ? 1 : at<uint8_t>(base + 0x17c3570);
    *reinterpret_cast<uint64_t *>(d + 0x28) = rd<uint64_t>(ses, 0x18);
    d[5] = static_cast<uint8_t>(game_call<int>(base + 0x7a2b10, static_cast<void *>(ses)));
    d[8] = net[0x3e4];
    *reinterpret_cast<int32_t *>(d + 0x10) = 0;
    *reinterpret_cast<int32_t *>(d + 0xc) = 0;
    *reinterpret_cast<int32_t *>(d + 0x14) = -1;
    *reinterpret_cast<int32_t *>(d + 0x18) = -1;
    d[6] = 0;
    *reinterpret_cast<uint64_t *>(d + 0x20) = 0;
    {
        uint8_t *p = u3h_lg_profile(base, 0);
        const uint8_t *r = p ? u3h_lg_rec(base, s, p) : nullptr;
        const bool loaded = p && rd<uint32_t>(p, 0x7ae8) == 3;
        *reinterpret_cast<uint32_t *>(d + 0x30) = loaded ? rd<uint32_t>(r, 0x4fc) : 0;
        *reinterpret_cast<uint32_t *>(d + 0x34) = loaded ? rd<uint32_t>(r, 0x504) : 0;
        for (int k = 0; k < 8; ++k) *reinterpret_cast<uint32_t *>(d + 0x38 + 4 * k) = r ? rd<uint32_t>(r, presence_stats[k]) : 0;
    }
    *reinterpret_cast<uint64_t *>(d + 0x58) = 0;
    const uint8_t *ng = reinterpret_cast<const uint8_t *>(base + 0x17cb060);
    if (rd<int32_t>(ng, 0x6808) == 0) {
        d[4] = rd<int32_t>(net, 0x40) != 0;
    } else if (!ng[0x6a24]) {
        d[4] = 2;
        d[6] = static_cast<uint8_t>(game_call<int>(base + 0x727290, reinterpret_cast<void *>(base + 0x17cb060)));
        *reinterpret_cast<int32_t *>(d + 0x10) = at<int16_t>(base + 0x17e338c + 0x130 + 2);
        *reinterpret_cast<int32_t *>(d + 0xc) = at<int16_t>(base + 0x17e338c + 2);
        *reinterpret_cast<int32_t *>(d + 0x14) = rd<int16_t>(ng, 0x661c);
        *reinterpret_cast<int32_t *>(d + 0x18) = ng[0x6a48];
        int64_t end;
        if (u3h_match_end_time(base, s, &end)) *reinterpret_cast<int64_t *>(d + 0x20) = end;
    } else {
        *reinterpret_cast<int32_t *>(d + 0x14) = rd<int16_t>(ng, 0x661c);
        d[4] = 3;
        *reinterpret_cast<int32_t *>(d + 0x18) = ng[0x6a48];
    }
    bool changed = false;
    for (int k = 0; k < 0x60; ++k)
        if (nf[0x60 + k] != d[k]) { changed = true; break; }
    if (!changed) return;
    for (int k = 0; k < 0x60; ++k) nf[0x60 + k] = d[k];
    uint8_t det[0x190];
    title_details(base, s, det);
    for (int k = 0; k < 0x60; ++k) det[0x100 + k] = d[k];
    *reinterpret_cast<uint64_t *>(det + 0x180) = 0x60;
    ++ext_ea()[70];
    if (u3h_presence_set(base, det, 3) < 0) {
        for (int k = 0; k < 0x60; ++k) nf[0x60 + k] = 0;
        *reinterpret_cast<int32_t *>(nf + 0xc0) = at<int32_t>(base + 0x17b4a90 + 0x114) + 3000;
        ++ext_ea()[71];
    }
}

// ===== v7.158: next batch, part 7 (kill rewards from boosters, weapon-category medals) =====

extern "C" {
U3H_IMPORT uint32_t u3h_kr_statw(uintptr_t base, uint32_t index);
U3H_IMPORT void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np);
U3H_IMPORT uint8_t *u3h_kr_prec(uint8_t *S, uint8_t *np);
U3H_IMPORT uint8_t *u3h_kr_np_by_id(uintptr_t base, uint32_t id);
}

// v5.16: the kill history S (records of 0x28 bytes from +0xe4, count +0xe0: type +0, killer +2, victim +4, teams
// +6/+7, weapon stat index +0xc, +0x25 "not counted"). Boosters are asked through 0x689d20 (a float). ExtState
// +0x2d90 (krb): [0] booster asks, [1] ammo given, [2] grenades given, [3] kickback added. Switch bit 0x10000 at
// state +0x3d78 turns the booster rewards off.
namespace {
struct KillRecord {
    uint8_t type, pad1;
    uint16_t killer, victim;
    int8_t killer_team, victim_team;
    int32_t time;
    uint8_t weapon_index, pad2;
    int16_t pos[6];
    uint8_t pad3[2];
    int32_t f1c;
    uint8_t f20, f21, f22, f23, f24, uncounted, pad4[2];
};
static_assert(sizeof(KillRecord) == 0x28);
constexpr uint32_t no_boosters = 0x10000u;
uint32_t *ext_krb() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2d90); }
uint32_t kr_switches(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78); }
KillRecord *kill_record(uint8_t *S, uint32_t i) { return reinterpret_cast<KillRecord *>(S + 0xe4 + i * 0x28); }
uint32_t kill_count(const uint8_t *S) { return rd<uint32_t>(S, 0xe0); }
U3X_FN bool local_np(uintptr_t base, const uint8_t *np) {
    const uint8_t *ng = reinterpret_cast<const uint8_t *>(base + 0x17cb060);
    return np == rd<const uint8_t *>(ng, 0x370) || np == rd<const uint8_t *>(ng, 0x378);
}
// a booster's value for a player (float bits)
U3X_FN uint32_t booster(uintptr_t base, uint32_t id, uint8_t *np, void *out) {
    const uint32_t r = call_float(base + 0x689d20, base + 0x17c3b78, id, reinterpret_cast<uint64_t>(np), reinterpret_cast<uint64_t>(out));
    ++ext_krb()[0];
    return r;
}
U3X_FN void add_ammo(uint8_t *inventory, uint32_t weapon, int32_t amount, void *out) {
    struct { uint32_t w; int32_t a; uint8_t f, pad[7]; } give;
    give.w = weapon;
    give.a = amount;
    give.f = 0;
    for (uint8_t &b : give.pad) b = 0;
    virtual_call<void>(inventory, 0xe8, static_cast<void *>(&give), out);
    ++ext_krb()[1];
}
U3X_FN void give_grenades(uintptr_t base, uint8_t *inventory, int32_t n, void *out) {
    alignas(16) uint8_t st[0xa0];
    for (uint8_t &b : st) b = 0;
    for (int i = 0; i < 16; ++i) {   // the game's identity transform at 0x1402ac0, twice
        st[0x10 + i] = at<uint8_t>(base + 0x1402ac0 + i);
        st[0x30 + i] = at<uint8_t>(base + 0x1402ac0 + i);
    }
    st[0x6c] = 0xff;
    *reinterpret_cast<uint32_t *>(st + 0x78) = 0x0b8bd6d6u;
    *reinterpret_cast<int32_t *>(st + 0x7c) = n;
    st[0x90] = 0xff;
    virtual_call<void>(inventory, 0xf0, static_cast<void *>(st), 0u, 0u, out);
    ++ext_krb()[2];
}
U3X_FN void kickback_add(uintptr_t base, const uint8_t *np, int32_t v) {
    int32_t *m = reinterpret_cast<int32_t *>(base + 0x17d79f0 + static_cast<uint64_t>(rd<uint32_t>(np, 0x1e8)) * 0x20);
    const int32_t t = *m + v;
    *m = t < 0 ? 0 : t;
    ++ext_krb()[3];
}
// the weapon definition (DC 0x61d589d9: open-addressing map {n, keys @8, defs @0x10}); category at +8
U3X_FN uint8_t *weapon_definition(uintptr_t base, uint32_t w) {
    const uint32_t *t = game_call<const uint32_t *>(base + 0x11f2e30, 0x61d589d9u);
    if (!t || !t[0] || !w) return nullptr;
    const uint32_t n = t[0];
    const uint32_t *keys = *reinterpret_cast<uint32_t *const *>(t + 2);
    uint8_t *const *values = *reinterpret_cast<uint8_t *const *const *>(t + 4);
    uint32_t i = w % n;
    const uint32_t start = i;
    while (keys[i] != w) {
        if (!keys[i]) return nullptr;
        i = (i + 1) % n;
        if (i == start) return nullptr;
    }
    return values[i];
}
U3X_FN uint8_t *weapon_data(uintptr_t base, uint32_t w) { return game_call<uint8_t *>(base + 0xd415e0, w); }
U3X_FN bool weapon_data_type4(const uint8_t *e) {
    const uint8_t *d = e ? rd<const uint8_t *>(e, 0) : nullptr;
    return d && rd<int32_t>(d, 0x17c) == 4;
}
U3X_RO const uint32_t category_stats[6] = {0x6fec7d00u, 0x64ff0c12u, 0x8c1f8138u, 0, 0x799e5859u, 0xc315a8cbu};
}  // namespace
extern "C" U3X_FN uint8_t *u3x_weapon_definition(uintptr_t base, uint32_t w) { return weapon_definition(base, w); }
// rewards for the killer's linked local player (np +0x674): ammo for the held weapon (booster 0x0a1b799a) or, for a
// grenade kill, grenades (0xfca72e93); a kill with weapon 0xc58ae515 adds kickback points (0x8e5e947e)
extern "C" U3X_FN void u3x_booster_kill(uintptr_t base, void *s, uint8_t *knp, uint32_t flags, uint32_t w) {
    uint8_t *snp = u3h_kr_np_by_id(base, rd<uint32_t>(knp, 0x674));
    if (!snp || !local_np(base, snp) || (kr_switches(s) & no_boosters)) return;
    uint8_t *ch = handle_object(rd<uint8_t **>(snp, 0x10), rd<uint32_t>(snp, 0x18));
    if (ch) {
        uint8_t *inventory = rd<uint8_t *>(ch, 0x520);
        uint8_t out[8];
        for (uint8_t &b : out) b = 0;
        if (inventory) {
            if (!(flags & 0x20)) {
                const uint32_t d = booster(base, 0x0a1b799au, snp, out);
                uint8_t *weapon = f_less(0, d) ? virtual_call<uint8_t *>(inventory, 0x80) : nullptr;
                if (weapon) {
                    const uint8_t *def = game_call<const uint8_t *>(base + 0xd436b0, weapon);
                    if (def) add_ammo(inventory, rd<uint32_t>(weapon, 0x6ec), f_to_int(f_mul(f_from_int(rd<int32_t>(def, 0x58)), d)), out);
                }
            } else {
                const int32_t n = f_to_int(booster(base, 0xfca72e93u, snp, out));
                if (n) give_grenades(base, inventory, n, out);
            }
        }
    }
    if (w == 0xc58ae515u) kickback_add(base, snp, f_to_int(booster(base, 0x8e5e947eu, snp, nullptr)));
}
// boosters for a local killer's weapon (PS3 f_37d1cc)
extern "C" U3X_FN void u3x_weapon_boosters(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    if (kr_switches(s) & no_boosters) return;
    const KillRecord *r0 = kill_record(S, 0);
    const uint32_t w = u3h_kr_statw(base, r0->weapon_index);
    uint8_t *ch = handle_object(rd<uint8_t **>(knp, 0x10), rd<uint32_t>(knp, 0x18));
    if (!ch) return;
    uint8_t *inventory = rd<uint8_t *>(ch, 0x520);
    if (!inventory) return;
    uint8_t out[8];
    for (uint8_t &b : out) b = 0;
    if (w == 0xc92d29c1u || (w == 0x5136f850u && r0->type == 1)) {
        const int32_t a = f_to_int(booster(base, 0x966ccd32u, knp, out));
        if (a > 0) add_ammo(inventory, w, a, out);
    }
    if (w == 0xc58ae515u) {
        const int32_t n = f_to_int(booster(base, 0xed737cfbu, knp, out));
        if (n) {
            uint8_t *weapon = virtual_call<uint8_t *>(inventory, 0x78);
            if (weapon) {
                const uint8_t *def = game_call<const uint8_t *>(base + 0xd436b0, weapon);
                if (def) add_ammo(inventory, rd<uint32_t>(weapon, 0x6ec), n * rd<int32_t>(def, 0x58), out);
            }
        }
        const int32_t n2 = f_to_int(booster(base, 0x43d8284du, knp, out));
        if (n2) {
            uint8_t *g = virtual_call<uint8_t *>(inventory, 0x70, 2u);
            if (g && virtual_call<int32_t>(g, 0x398) > 0) add_ammo(inventory, rd<uint32_t>(g, 0x6ec), n2, out);
            else give_grenades(base, inventory, n2, out);
        }
    }
}
// weapon-category medals (PS3 f_37ae3c): the killer's counted kills of the weapon's category, its type-4 streak,
// kills with data +0x20, sniper 0x33e29a1b kills; every 3rd (5th in team modes) category 0 / 1 kill gives ammo
extern "C" U3X_FN void u3x_category_medals(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, const uint8_t *info, int team_mode) {
    const KillRecord *r0 = kill_record(S, 0);
    if (r0->killer_team == r0->victim_team) return;
    const uint32_t w = rd<uint32_t>(info, 0x38), id = rd<uint32_t>(info, 0x30);
    const uint8_t *wd = weapon_definition(base, w);
    const uint32_t category = wd ? rd<uint32_t>(wd, 8) : 0xfffffffdu;
    if (w == 0x159df996u) u3h_kr_stat(base, s, 0xa828aa55u, 1, knp);
    uint32_t same_category = 0, type4 = 0, type4_run = 0, with_data = 0, sniper = 0;
    bool broken = false;
    for (uint32_t i = 0; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->uncounted) continue;
        if (r->killer == static_cast<uint16_t>(id) && r->killer_team != r->victim_team) {
            const uint32_t ww = u3h_kr_statw(base, r->weapon_index);
            if (ww == w) ++same_category;
            else if (const uint8_t *d = weapon_definition(base, ww)) same_category += rd<uint32_t>(d, 8) == category;
            const uint8_t *e = weapon_data(base, ww);
            if (!e) {
                broken = true;
            } else {
                if (weapon_data_type4(e)) {
                    ++type4;
                    if (!broken) ++type4_run;
                } else {
                    broken = true;
                }
                with_data += rd<int32_t>(e, 0x20) != 0;
            }
            if (ww == 0x33e29a1bu) ++sniper;
        } else if (r->victim == static_cast<uint16_t>(id)) {
            broken = true;
        }
    }
    if (const uint8_t *e = weapon_data(base, w)) {
        if (weapon_data_type4(e)) {
            u3h_kr_stat(base, s, 0xceb95908u, type4, knp);
            u3h_kr_stat(base, s, 0xeadf6540u, type4_run, knp);
        }
        if (rd<int32_t>(e, 0x20)) u3h_kr_stat(base, s, 0xa1d3f722u, with_data, knp);
    }
    if (w == 0x33e29a1bu) u3h_kr_stat(base, s, 0xcc5567c6u, sniper, knp);
    if (category < 6 && category_stats[category]) {
        u3h_kr_stat(base, s, category_stats[category], same_category, knp);
        if (category < 2 && !(kr_switches(s) & no_boosters)) {
            const uint32_t every = team_mode ? 5 : 3;
            if (same_category % every == 0 && local_np(base, knp)) {
                uint8_t out[8];
                for (uint8_t &b : out) b = 0;
                const uint32_t d = booster(base, 0x314d02e8u, knp, out);
                if (f_less(0, d)) {
                    const uint8_t *e2 = weapon_data(base, w);
                    uint8_t *ch = handle_object(rd<uint8_t **>(knp, 0x10), rd<uint32_t>(knp, 0x18));
                    if (e2 && !rd<int32_t>(e2, 0x20) && rd<uint8_t *>(e2, 0) && ch && rd<uint8_t *>(ch, 0x520))
                        add_ammo(rd<uint8_t *>(ch, 0x520), w, f_to_int(f_mul(f_from_int(rd<int32_t>(rd<uint8_t *>(e2, 0), 4)), d)), out);
                }
            }
        }
    }
    if (w == 0x08be7587u) u3h_kr_stat(base, s, 0xb550b700u, 1, knp);
}
// three of each (explosive, category 0, category 1): the new full sets get the medal (PS3 f_3814d8)
extern "C" U3X_FN void u3x_three_of_each(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    const uint32_t me = u3x_net_player_id(knp);
    uint32_t explosive = 0, c0 = 0, c1 = 0;
    for (uint32_t i = 0; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->uncounted || r->killer != static_cast<uint16_t>(me) || r->killer == r->victim || r->killer_team == r->victim_team) continue;
        if (r->type == 6) { ++explosive; continue; }
        const uint8_t *d = weapon_definition(base, u3h_kr_statw(base, r->weapon_index));
        if (!d) continue;
        const uint32_t c = rd<uint32_t>(d, 8);
        if (c == 0) ++c0;
        else if (c == 1) ++c1;
    }
    uint32_t sets = c0 / 3;
    if (explosive / 3 < sets) sets = explosive / 3;
    if (c1 / 3 < sets) sets = c1 / 3;
    uint8_t *pr = u3h_kr_prec(S, knp);
    if (rd<uint16_t>(pr, 0x4c) < sets) {
        *reinterpret_cast<uint16_t *>(pr + 0x4c) = static_cast<uint16_t>(sets);
        u3h_kr_stat(base, s, 0xa6fa41cau, 1, knp);
    }
}

// ===== v7.159: next batch, part 8 (the 1.19 player-info fields) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_np119(void);
U3H_IMPORT void u3h_taunt_want(uintptr_t base, uint32_t id);
}

// c006: PS3 1.19's player info (message 0x3a) carries three more fields than PS4's: +0x69 tournament rank (0: no
// tournaments here), +0x6a hat stats off, +0xc0 taunt; and bit 3 of +0x25 (the profile's +0x2164 flag). The sender
// fills them, the serializer writes / reads them after PS4's own body (bit writer 0x1272b50 / reader 0x1272c00), and
// the receiver keeps them per net player (np119: taunt, rank, hat stats off, flag, set). ec[0] bit 5 turns it off;
// ec[37..45] sends, reads, writes, (trampolines 40 / 41), receives, last receive, last taunt, taunt asks.
namespace {
struct PlayerInfo119 { uint32_t taunt; uint8_t rank, hat_off, flag, set; };
static_assert(sizeof(PlayerInfo119) == 8);
U3X_RO const uint8_t bit_writer_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x44, 0x8b, 0x5f, 0x08, 0x45, 0x89};
U3X_RO const uint8_t bit_reader_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x8b, 0x57, 0x08, 0x89, 0xd3};
}  // namespace
extern "C" U3X_FN void u3x_pinfo_send(uintptr_t base, uint8_t *msg) {
    uint32_t *ec = ext_ec();
    ++ec[37];
    if (ec[0] & 32) return;
    const uint32_t screen = rd<uint32_t>(msg, 0x14) >> 12;
    uint8_t *profiles = at<uint8_t *>(base + 0x17dd610);
    if (!profiles || screen > 3) return;
    const uint8_t *pr = profiles + screen * 0x7b00u;
    if (rd<uint32_t>(pr, 0x2164)) msg[0x25] |= 8;
    msg[0x69] = 0;   // no tournament rank (tournaments are removed)
    msg[0x6a] = rd<uint32_t>(pr, 0x215c) != 0;
    *reinterpret_cast<uint32_t *>(msg + 0xc0) = rd<uint32_t>(pr, 0x784);
}
// kinds 106 (read) / 107 (write): PS4's own body through its trampoline, then the three fields (8, 1, 32 bits)
extern "C" U3X_FN int u3x_pinfo_serial(uintptr_t base, uint64_t *regs, int write) {
    uint32_t *ec = ext_ec();
    uint8_t *msg = reinterpret_cast<uint8_t *>(regs[4]), *bs = reinterpret_cast<uint8_t *>(regs[5]);
    const uint32_t trampoline = ec[40 + write];
    if ((ec[0] & 32) || !trampoline) return 0;
    if (!code_starts(base, 0x1272b50, bit_writer_prologue) || !code_starts(base, 0x1272c00, bit_reader_prologue)) return 0;
    game_call<void>(base + trampoline, msg, bs);
    if (write) {
        game_call<void>(base + 0x1272b50, bs, uint32_t{msg[0x69]}, 8u);
        game_call<void>(base + 0x1272c90, bs, uint32_t{msg[0x6a]});
        game_call<void>(base + 0x1272b50, bs, rd<uint32_t>(msg, 0xc0), 32u);
        ++ec[39];
    } else {
        msg[0x69] = static_cast<uint8_t>(game_call<uint32_t>(base + 0x1272c00, bs, 8u));
        msg[0x6a] = static_cast<uint8_t>(game_call<uint32_t>(base + 0x1272c00, bs, 1u));
        *reinterpret_cast<uint32_t *>(msg + 0xc0) = game_call<uint32_t>(base + 0x1272c00, bs, 32u);
        ++ec[38];
    }
    regs[8] = 0;
    return 1;
}
// kind 108: the receiving player's fields (found by its id as PS4 does)
extern "C" U3X_FN void u3x_pinfo_process(uintptr_t base, const uint8_t *msg) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 32) return;
    uint8_t *mgr = reinterpret_cast<uint8_t *>(base + 0x17cb060);
    const uint32_t id = rd<uint32_t>(msg, 0x14);
    PlayerInfo119 *info = reinterpret_cast<PlayerInfo119 *>(u3h_np119());
    for (int i = 0; i < 10; ++i) {
        uint8_t *np = mgr + 0x390 + i * 0x9c0;
        if (net_player_id(np) != id) continue;
        np[0x208] = msg[0x25] & 7;
        info[i].flag = (msg[0x25] >> 3) & 1;
        info[i].rank = msg[0x69];
        info[i].hat_off = msg[0x6a];
        info[i].taunt = rd<uint32_t>(msg, 0xc0);
        info[i].set = 1;
        ++ec[42];
        ec[43] = static_cast<uint32_t>(i) | (uint32_t{msg[0x6a]} << 8) | (static_cast<uint32_t>((msg[0x25] >> 3) & 1) << 9);
        ec[44] = info[i].taunt;
        return;
    }
}
// kinds 109 / 110: the player's taunt package is wanted (unless ec[0] bit 3 or 5)
extern "C" U3X_FN void u3x_np_taunt_want(uintptr_t base, uint8_t *np) {
    uint32_t *ec = ext_ec();
    ++ec[45];
    if ((ec[0] & (8 | 32)) || !np) return;
    const int i = player_slot(base, np);
    if (i < 0) return;
    u3h_taunt_want(base, reinterpret_cast<PlayerInfo119 *>(u3h_np119())[i].taunt);
}

// ===== v7.160: next batch, part 9 (emblems on characters) =====

// c011: the emblem textures each character instance shows. In team modes the team's emblem player (the first valid
// player of that team whose team record +0 is 1) or the object's own player gives the board (+0x7e8) and layer
// (+0x7f0) records; elsewhere the emblem editor's pair (manager +0x68 / +0x70) when the spawner has the tag
// 0x32f2d823. Each render table of the object (vt+0x2c0 tables, up to 8) gets its record's texture at +8; missing
// records use the empty default (+0x78). ec[0] bit 14 turns it off. ec[118] looks done, ec[119] textures written,
// ec[133..145] see the C part's notes.
namespace {
bool emblem_player_valid(const uint8_t *np) { return np[0xe1] && !np[0x5e4]; }
}
extern "C" U3X_FN uint8_t *u3x_emblem_team_player(uintptr_t b, int32_t team) {
    for (uint32_t i = 0; i < 10; ++i) {
        uint8_t *np = reinterpret_cast<uint8_t *>(b + 0x17cb3f0 + i * 0x9c0);
        if (!emblem_player_valid(np) || rd<int32_t>(np, 0x21c) != team) continue;
        const int16_t *st = reinterpret_cast<const int16_t *>(b + 0x1b7f114);
        const uint32_t count = at<uint32_t>(b + 0x17dd780);
        if (count) {
            const uint32_t key = net_player_id(np);
            for (uint32_t j = 0; j < count; ++j)
                if (at<uint32_t>(b + 0x17e2864 + j * 0xa4) == key) {
                    st = reinterpret_cast<const int16_t *>(b + 0x17e384c + j * 0x130);
                    break;
                }
        }
        if (*st == 1) return np;
    }
    return nullptr;
}
// the net player behind a Player (by screen) or a NetCharacter (by net id)
extern "C" U3X_FN uint8_t *u3x_emblem_object_player(uintptr_t b, const uint8_t *obj) {
    if (!obj || !at<uint8_t>(b + 0x17d5c61)) return nullptr;
    if (rd<uint32_t>(obj, 0x30) == 0xa5a63e9bu) return at<uint8_t *>(b + 0x17cb060 + 0x370 + static_cast<int64_t>(rd<int32_t>(obj, 0x1c68)) * 8);
    if (rd<uint32_t>(obj, 0x30) == 0xf2c86c22u && rd<uint32_t>(obj, 0x3f8)) {
        const uint32_t id = rd<uint32_t>(obj, 0x3f8);
        for (uint32_t i = 0; i < 10; ++i) {
            uint8_t *np = reinterpret_cast<uint8_t *>(b + 0x17cb3f0 + i * 0x9c0);
            if (net_player_id(np) == id) return np;
        }
    }
    return nullptr;
}
// a spawner's tag bit (tag table 0x1b3efd8: 0x101 buckets of 0x10 bytes, entries {next, ..., sid +0x10, bit +0x14};
// the spawner's bits from +0x20)
extern "C" U3X_FN int u3x_emblem_tag(uintptr_t b, const uint8_t *spawner, uint32_t sid) {
    const uint8_t *table = reinterpret_cast<const uint8_t *>(b + 0x1b3efd8);
    if (!table[0] || !spawner) return 0;
    const uint8_t *bucket = table + 8 + (sid % 0x101) * 0x10;
    for (const uint8_t *en = rd<const uint8_t *>(bucket, 0); en && en != bucket; en = rd<const uint8_t *>(en, 0))
        if (rd<uint32_t>(en, 0x10) == sid) {
            const uint32_t bit = rd<uint32_t>(en, 0x14);
            return static_cast<int>((rd<uint64_t>(spawner, 0x20 + (bit >> 6) * 8) >> (bit & 63)) & 1);
        }
    return 0;
}
// kind 124 (the NetCharacter look 0x6a5c50): regs[5] = the object; returns 1 (the look is done here)
extern "C" U3X_FN int u3x_emblem_look(uintptr_t b, const uint64_t *regs) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 0x4000u) return 0;
    uint8_t *obj = reinterpret_cast<uint8_t *>(regs[5]);
    const uint8_t *e = reinterpret_cast<const uint8_t *>(b + 0x17c3440);
    if (!obj) return 1;
    const uint64_t fallback = rd<uint64_t>(e, 0x78);
    uint64_t records[4] = {0, 0, fallback, 0};
    if (at<int32_t>(b + 0x17d1868) == 2) {
        const int32_t team = game_call<int32_t>(b + 0x737eb0, b + 0x17cb060);
        uint8_t *team_player = u3x_emblem_team_player(b, team), *own = u3x_emblem_object_player(b, obj);
        if (!own) {
            records[0] = fallback;
            records[1] = fallback;
            own = team_player;
        }
        if (own) {
            records[1] = rd<uint64_t>(own, 0x7f0);
            records[0] = rd<uint64_t>(own, 0x7e8);
        }
    } else {
        const int tagged = u3x_emblem_tag(b, rd<const uint8_t *>(obj, 0x38), 0x32f2d823u);
        records[1] = rd<uint64_t>(e, 0x70);
        records[0] = tagged ? rd<uint64_t>(e, 0x68) : records[1];
    }
    ++ec[133];
    ec[141] = rd<uint32_t>(obj, 0x30);
    ec[143] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(obj));
    if (at<int32_t>(b + 0x17d1868) == 2) ++ec[135];
    {
        const uint8_t *m = rd<const uint8_t *>(obj, 0x130);
        if (m && rd<uint32_t>(m, 0x18)) {
            const uint8_t *e0 = rd<const uint8_t *>(m, 0x10);
            if (e0 && e0[0x60] && rd<void *>(e0, 0)) {
                const uint8_t *ri = game_call<const uint8_t *>(b + 0xe48ea0, rd<void *>(e0, 0));
                if (ri) ec[144] = rd<uint32_t>(ri, 0xd4);
            }
        }
    }
    for (int32_t i = 0;; ++i) {
        const int32_t n = virtual_call<int32_t>(obj, 0x2c0);
        if (n <= i) break;
        ec[134] = static_cast<uint32_t>(n);
        uint64_t r = i < 4 ? records[i] : 0;
        ec[145] = r ? (r == rd<uint64_t>(e, 0x68) || r == rd<uint64_t>(e, 0x70) ? 2u : 1u) : 3u;
        if (!r) {
            r = rd<uint64_t>(e, 0x78);
            ++ec[142];
        }
        if (!r || !u3h_emb_is_record(r)) continue;
        uint8_t *table = rd<uint8_t *>(obj, 0x240 + 8 * static_cast<uint32_t>(i & 7));
        if (!table) { ++ec[136]; continue; }
        if (i > 7) continue;
        uint64_t *slot = reinterpret_cast<uint64_t *>(table + 8);
        const uint64_t texture = *reinterpret_cast<const uint64_t *>(r);
        if (!texture) continue;
        if (*slot) ++ec[140];
        *slot = texture;
        ++ec[119];
        ec[138] = static_cast<uint32_t>(texture);
        ec[139] = static_cast<uint32_t>(texture >> 32);
        const uint8_t *scene_character = at<const uint8_t *>(b + 0x17bba90 + 0x1480);
        if (scene_character && rd<uint8_t *>(scene_character, 0) == obj) ++ec[137];
    }
    ++ec[118];
    return 1;
}

// ===== v7.161: next batch, part 10 (the friends list rows) =====

extern "C" {
U3H_IMPORT void u3h_text_num(uintptr_t base, void *s, char *buf, uint32_t size, uint32_t id, int32_t v);
U3H_IMPORT int u3h_fr_in_session(uintptr_t base, uint8_t *e);
U3H_IMPORT int u3h_fr_in_title(uint8_t *e);
U3H_IMPORT uint8_t *u3h_fr_pdata(uintptr_t base, uint8_t *e);
U3H_IMPORT void u3h_fr_refresh(uintptr_t base);
}

namespace {
// a DC symbol's data through the state's lookup (+656)
U3X_FN uint8_t *dc_table(void *s, uint32_t id) {
    const uintptr_t lookup = *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 656);
    void **h = lookup ? reinterpret_cast<void **(*)(uint32_t)>(lookup)(id) : nullptr;
    return h ? static_cast<uint8_t *>(*h) : nullptr;
}
U3X_FN char *append_chars(char *o, char *end, const char *t) {
    while (*t && o < end) *o++ = *t++;
    return o;
}
U3X_BSS int32_t friends_selected;
U3X_BSS uint32_t friends_ops_seen;
U3X_RO const char fl_op[] = "Friends list: op ", fl_first[] = " asked (first time)";
U3X_RO const char facebook_prefix[] = "|@C0xa0a0a0||FB||@R0xffffffff| ";
constexpr uint32_t friends_tab = 0x6e83df53u;
}  // namespace
// "%s%s" into the next of the 4 rotating 0x100-byte buffers
extern "C" U3X_FN const char *u3x_friend_join(const char *a, const char *b) {
    char *o = u3h_fr_buffer(), *end = o + 0xff;
    char *p = append_chars(o, end, a);
    p = append_chars(p, end, b);
    *p = 0;
    return o;
}
// " - " and "[n] players in party" for a party bigger than 1 (else empty); buf holds 0x80 bytes
extern "C" U3X_FN void u3x_friend_party_suffix(uintptr_t base, void *s, char *buf, uint8_t size) {
    buf[0] = 0;
    for (int k = 1; k < 0x80; ++k) buf[k] = 0;
    if (size > 1) {
        buf[0] = ' ';
        buf[1] = '-';
        buf[2] = ' ';
        u3h_text_num(base, s, buf + 3, 0x7d, 0xba8db526u, size);
    }
}
// the text of a friend's activity (0 in the lobby, 1 matchmaking, 3 in a match, 2 in a party)
extern "C" U3X_FN uint32_t u3x_friend_activity(uint8_t a) {
    switch (a) {
    case 0: return 0x84041d79u;
    case 1: return 0xbd1d2c53u;
    case 3: return 0xea3bb70eu;
    case 2: return 0xcf292b57u;
    default: return 0;
    }
}
// an icon answer of type 0x22
extern "C" U3X_FN void u3x_friend_icon(int32_t *ev, int32_t v) {
    ev[2] = 0x22;
    ev[4] = 0;
    ev[5] = 0;
    ev[6] = v;
    ev[7] = 0;
    ev[8] = 0;
    ev[9] = -1;
    ev[10] = 0;
    ev[11] = -1;
    ev[12] = 0x3f800000;
}
// 2 in our session, else 1 online, 0 offline
extern "C" U3X_FN int32_t u3x_friend_state_icon(uintptr_t base, uint8_t *e) {
    const int32_t st = rd<int32_t>(e, 0x1b0);
    return u3h_fr_in_session(base, e) ? 2 : st != 0;
}
// the selected tab's text of the top page (DC 0xf1514340 rows of 0x20 bytes {page, LAN byte, tabs +0x10, selected +0x18})
extern "C" U3X_FN uint32_t u3x_page_tab_text(uintptr_t base, void *s) {
    const uint8_t *t = dc_table(s, 0xf1514340u);
    if (!t) return 0;
    const uint32_t page = u3x_menu_page(reinterpret_cast<const uint8_t *>(base + 0x17bd080));
    const uint8_t lan = at<uint8_t>(base + 0x17d5c60);
    const uint8_t *rows = rd<const uint8_t *>(t, 8);
    for (uint32_t i = 0; i < rd<uint32_t>(t, 0); ++i) {
        const uint8_t *x = rows + i * 0x20;
        if (rd<uint32_t>(x, 0) == page && rd<uint32_t>(x, 4) == lan) return rd<uint32_t>(rd<const uint8_t *>(x, 0x10), rd<uint32_t>(x, 0x18) * 0xc);
    }
    return 0;
}
// LIST_PSN_FRIENDS (enum 0xae, a RET on PS4), on the Friends tab only: name, Online / Offline / Campaign / activity
// with the party size, the state icon; op 9 refreshes. ea[85] calls.
extern "C" U3X_FN int u3x_friend_list(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414eaeu) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    uint8_t *nf = reinterpret_cast<uint8_t *>(base + 0x17caeb0);
    const int32_t op = ev[0];
    ++ext_ea()[85];
    if (op >= 0 && op < 32 && !(friends_ops_seen >> op & 1u) && u3x_page_tab_text(base, s) == friends_tab) {
        friends_ops_seen |= 1u << op;
        u3h_so_note(fl_op, nullptr, op, fl_first);
    }
    if (u3x_page_tab_text(base, s) != friends_tab) {
        if (op == 6) ev[1] = -1;
        return 1;
    }
    switch (op) {
    case 1: set_row_text(ev, empty_text); return 1;
    case 5: ev[1] = friends_selected; return 1;
    case 6: {
        int32_t c = 0;
        if (!rd<uint64_t>(nf, 0xc8)) {
            c = static_cast<int32_t>(rd<uint32_t>(nf, 0)) - 1;
            if (c < 0) c = 0;
        }
        ev[1] = c;
        return 1;
    }
    case 8: ev[1] = 1; return 1;
    case 9: u3h_fr_refresh(base); return 1;
    case 10: friends_selected = ev[1]; return 1;
    case 2: case 3: case 4: case 7: break;
    default: return 1;
    }
    const int32_t row = ev[1];
    if (rd<uint64_t>(nf, 0xc8) || row < 0 || static_cast<uint32_t>(row) >= rd<uint32_t>(nf, 0) || !rd<uint8_t *>(nf, 8)) return 1;
    uint8_t *e = u3h_fr_ent(base, static_cast<uint32_t>(row));
    if (op == 4) {
        if (static_cast<uint32_t>(ev[0x14]) != 0xf5a294f3u) return 1;
        u3x_friend_icon(ev, u3x_friend_state_icon(base, e));
        return 1;
    }
    if (op != 2) return 1;
    const uint32_t sub = static_cast<uint32_t>(ev[0x15]);
    if (sub == 0) {
        set_row_text(ev, reinterpret_cast<const char *>(e + 1));
        return 1;
    }
    if (sub == 2) {
        if (!e[0x1c0]) return 1;
        char *o = u3h_fr_buffer();
        char *p = append_chars(o, o + 0xff, facebook_prefix);
        p = append_chars(p, o + 0xff, reinterpret_cast<const char *>(e + 0x1c0));
        *p = 0;
        set_row_text(ev, o);
        return 1;
    }
    if (sub != 1) return 1;
    const int32_t st = rd<int32_t>(e, 0x1b0);
    set_row_text(ev, u3h_text(base, s, st != 0 ? 0x51663192u : 0x6ea711d6u));   // Online / Offline
    if (u3h_fr_in_title(e)) set_row_text(ev, u3h_text(base, s, 0xf461ef26u));   // Campaign
    if (!u3h_fr_pdata(base, e) || st != 1) return 1;
    const uint8_t *d = u3h_fr_pdata(base, e);
    char buf[0x80];
    u3x_friend_party_suffix(base, s, buf, d[5]);
    const uint32_t activity = u3x_friend_activity(d[4]);
    if (!activity) return 1;
    set_row_text(ev, u3x_friend_join(u3h_text(base, s, activity), buf));
    return 1;
}

// ===== v7.162: next batch, part 11 (the co-op hunter class list) =====

extern "C" {
U3H_IMPORT int u3h_net_coop_mode(uintptr_t base, void *s);
U3H_IMPORT int u3h_np_alive(void *s, uint8_t *p);
U3H_IMPORT uint32_t u3h_respawn_delay(void *s, uint8_t *p);
U3H_IMPORT int64_t u3h_f_ticks(uint32_t f, int64_t half);
}

// LIST_SPOILER_CLASS (enum 0xbf): the hunter classes of the current round (round entry 0x723060 +0x40 {count, ids})
// looked up in DC 0x2b6127a0 (0x28-byte entries: id, name +4, cost +0x1c); the cost against the user's kickback
// points (0x17d79f0 + user * 0x30); picking one while dead past the respawn delay asks the respawn with that class
// and closes the menu. Selection and the Buy toggle per user at ExtState +0x180 / +0x190. coop[7] calls, [8] picks,
// [9] last class.
namespace {
U3X_RO const uint8_t round_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0xbf, 0x7d};
U3X_RO const uint8_t class_list_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec};
U3X_RO const uint8_t popup_prologue[12] = {0x53, 0x48, 0x8d, 0x0d, 0x38, 0xca, 0x15, 0x01, 0x8b, 0x99, 0x7c, 0x08};
// the class level icons (PS3 1.19 0xda5d10), levels 0..10
U3X_RO const uint32_t level_icons[11] = {0x36190133u, 0x32d81c84u, 0x3f9b3a5du, 0x3b5a27eau, 0x251d77efu, 0x21dc6a58u,
                                          0x2c9f4c81u, 0x285e5136u, 0x1011ec8bu, 0x14d0f13cu, 0xd19ebf6eu};
U3X_BSS char points_text[24];
U3X_FN int32_t *selection() { return reinterpret_cast<int32_t *>(u3h_ext() + 0x180); }
U3X_FN uint8_t *buy_toggle() { return u3h_ext() + 0x190; }
U3X_FN uint32_t kickback_points(uintptr_t base, uint32_t user) { return at<uint32_t>(base + 0x17d79f0 + static_cast<uint64_t>(user) * 0x30); }
U3X_FN const uint8_t *round_classes(uintptr_t base) {
    if (!code_starts(base, 0x723060, round_prologue)) return nullptr;
    const uint8_t *round = game_call<const uint8_t *>(base + 0x723060, base + 0x17cb060);
    return round ? rd<const uint8_t *>(round, 0x40) : nullptr;
}
}  // namespace
extern "C" U3X_FN uint8_t *u3x_spoiler_entry(uintptr_t base, void *s, int32_t row, int32_t *index) {
    const int32_t *classes = game_call<const int32_t *>(state_native(s, 13), 0x2b6127a0u);
    if (!classes || !code_starts(base, 0x723060, round_prologue)) return nullptr;
    const uint8_t *ids = round_classes(base);
    if (!ids || row < 0 || row >= rd<int32_t>(ids, 0)) return nullptr;
    const uint32_t want = rd<const uint32_t *>(ids, 8)[row];
    uint8_t *e = *reinterpret_cast<uint8_t *const *>(classes + 2);
    for (int32_t i = 0; e && i < classes[0]; ++i)
        if (rd<uint32_t>(e, i * 0x28) == want) {
            if (index) *index = i;
            return e + static_cast<int64_t>(i) * 0x28;
        }
    return nullptr;
}
extern "C" U3X_FN int32_t u3x_spoiler_rows(uintptr_t base) {
    const uint8_t *ids = round_classes(base);
    return ids ? rd<int32_t>(ids, 0) : 0;
}
// a hunter may pick when dead past the respawn delay with no request pending
extern "C" U3X_FN int u3x_spoiler_can_pick(uintptr_t base, void *s, uint8_t *p) {
    if (!u3h_net_coop_mode(base, s) || rd<int32_t>(p, 0x21c) != 1 || u3h_np_alive(s, p) || p[0x420] || p[0x5e4]) return 0;
    const uint32_t d = u3h_respawn_delay(s, p);
    const bool positive = !(d >> 31) && (d & 0x7fffffff) && ((d >> 23) & 0xff) != 0xff;
    if (!positive) return 0;
    return u3h_f_ticks(d, 0) < at<int32_t>(base + 0x17b4ca4) - rd<int32_t>(p, 0x39c);
}
extern "C" U3X_FN int u3x_spoiler_list(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414ebf) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    ++ext_coop()[7];
    const uint32_t user = at<uint32_t>(base + 0x17c23a0) & 3;
    if (ev[0] == 1) {
        if (code_starts(base, 0x76b0f0, class_list_prologue)) game_call<void>(base + 0x76b0f0, ev);
        return 1;
    }
    if (!u3h_sym_ready(base, s, 0x2b6127a0u)) return 1;
    switch (ev[0]) {
    case 2: {
        const uint32_t column = static_cast<uint32_t>(ev[0x15]);
        if (column == 0) {
            const uint8_t *e = u3x_spoiler_entry(base, s, ev[1], nullptr);
            if (e) set_row_text(ev, u3h_text(base, s, rd<uint32_t>(e, 4)));
        } else if (column == 0xee5350e3u) {   // "x<points>"
            char digits[12];
            unsigned n = 0, d = 0;
            points_text[n++] = 'x';
            uint64_t v = kickback_points(base, user);
            do { digits[d++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
            while (d) points_text[n++] = digits[--d];
            points_text[n] = 0;
            set_row_text(ev, points_text);
        }
        break;
    }
    case 4:
        if (static_cast<uint32_t>(ev[0x14]) == 0xee5350e3u) u3h_set_icon(base, ev, 0xa879f3f8u);
        break;
    case 5: ev[1] = selection()[user]; break;
    case 6: ev[1] = u3x_spoiler_rows(base) - 1; break;
    case 9: {
        uint8_t *m = at<uint8_t *>(base + 0x16e3188);
        if (m && code_starts(base, 0x658050, popup_prologue)) {
            const int r = game_call<int>(base + 0x658050, m, 1, 0);
            if (r == 7) u3h_show(base, s, 0xf3026799u, 0);
            else if (r == 0xc) buy_toggle()[user] ^= 1;
        }
        break;
    }
    case 10: selection()[user] = ev[1]; break;
    case 14:
        if (ev[0x19] == 2) {   // column 2: the class level icon
            const uint8_t *e = u3x_spoiler_entry(base, s, ev[0x18], nullptr);
            const uint32_t level = e ? rd<uint32_t>(e, 0x1c) : 99;
            if (level < 11) u3h_set_icon(base, ev, level_icons[level]);
        }
        break;
    case 17: ev[1] = 3; break;
    case 21: {
        int ok = 0;
        if (!buy_toggle()[user]) {
            const uint8_t *e = u3x_spoiler_entry(base, s, ev[0x17], nullptr);
            ok = e && rd<uint32_t>(e, 0x1c) <= kickback_points(base, user);
        }
        ev[1] = ok;
        break;
    }
    case 22: {
        int32_t ci = 0;
        const uint8_t *e = u3x_spoiler_entry(base, s, ev[0x17], &ci);
        if (!e || kickback_points(base, user) < rd<uint32_t>(e, 0x1c)) break;
        uint8_t *p = at<uint8_t *>(base + 0x17cb060 + 0x370 + user * 8);
        if (!p || !u3x_spoiler_can_pick(base, s, p)) break;
        p[0x60c] = 1;
        *reinterpret_cast<int32_t *>(p + 0x608) = ci;
        ++ext_coop()[8];
        ext_coop()[9] = static_cast<uint32_t>(ci);
        uint8_t *m = at<uint8_t *>(base + 0x16e3188);
        u3x_menu_back(base, m, 0);
        u3x_menu_close(base, m);
        break;
    }
    case 23:
        if (ev[0xd]) selection()[user] = 0;
        break;
    case 24: ev[1] = 1; break;
    }
    return 1;
}

// ===== v7.163: next batch, part 12 (players met history, friend events) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_fr_find(uintptr_t base, const uint8_t *name);
U3H_IMPORT int32_t u3h_pm_add(uintptr_t base, const uint8_t *name, const char *title);
U3H_IMPORT int32_t u3h_pm_count(uintptr_t base);
U3H_IMPORT int32_t u3h_pm_get(uintptr_t base, uint32_t i, uint8_t *id);
U3H_IMPORT int32_t u3h_fr_event(uintptr_t base, uint8_t *id);
U3H_IMPORT int u3h_take_rebuild(void);
}

// Players met (NetFriends +0x10 count, +0x14 capacity, +0x18 entries of 0x28 {name 0x24, +0x24}) and the new players
// of a match (+0xec count, +0xe8 capacity 16, +0xf0 names of 0x24) that go to the history (AddPlayersHistory with the
// title text); +0xe0 history busy, +0xd0 fetch busy, +0x20 history changed. Friends are never listed. ea[72..76]
// history adds ok / failed, fetches, match lists, friend events.
namespace {
uint8_t *friend_block(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17caeb0); }
U3X_FN bool listed(uintptr_t base, const uint8_t *met, uint32_t count, const uint8_t *name) {
    for (uint32_t j = 0; j < count; ++j)
        if (u3h_fr_strcmp(base, met + j * 0x28, name) == 0) return true;
    return false;
}
}  // namespace
extern "C" U3X_FN void u3x_history_thread(uintptr_t base, void *s) {
    uint8_t *nf = friend_block(base);
    const uint8_t *v = rd<const uint8_t *>(nf, 0xf0);
    const char *title = u3h_text(base, s, 0xfd2de693u);
    for (uint32_t i = 0; i < rd<uint32_t>(nf, 0xec); ++i) {
        const uint8_t *name = v + i * 0x24;
        if (!name[0]) continue;
        if (!rd<uint64_t>(nf, 0xc8) && u3h_fr_find(base, name)) continue;
        ++ext_ea()[u3h_pm_add(base, name, title) < 0 ? 73 : 72];
    }
    *reinterpret_cast<uint64_t *>(nf + 0xe0) = 0;
    *reinterpret_cast<uint32_t *>(nf + 0xec) = 0;
}
// flag 0: names into the players-met list (no friends, no duplicates); flag 1: a match's new players (not friends,
// not already met; up to 16), then the history thread
extern "C" U3X_FN void u3x_players_met_add(uintptr_t base, void *s, const uint8_t *names, uint32_t n, int flag) {
    uint8_t *nf = friend_block(base);
    if (!n) return;
    if (!flag) {
        for (uint32_t k = 0; k < n; ++k, names += 0x24) {
            if (!names[0]) continue;
            if (!rd<uint64_t>(nf, 0xc8) && u3h_fr_find(base, names)) continue;
            const uint32_t c = rd<uint32_t>(nf, 0x10);
            if (c >= rd<uint32_t>(nf, 0x14)) continue;
            uint8_t *met = rd<uint8_t *>(nf, 0x18);
            if (listed(base, met, c, names)) continue;
            uint8_t *e = met + c * 0x28;
            for (int j = 0; j < 0x24; ++j) e[j] = names[j];
            *reinterpret_cast<uint32_t *>(e + 0x24) = 0;
            *reinterpret_cast<uint32_t *>(nf + 0x10) = c + 1;
        }
        return;
    }
    if (rd<uint64_t>(nf, 0xe0)) return;
    *reinterpret_cast<uint32_t *>(nf + 0xec) = 0;
    for (uint32_t k = 0; k < n; ++k, names += 0x24) {
        if (!names[0]) continue;
        if (!rd<uint64_t>(nf, 0xc8) && u3h_fr_find(base, names)) continue;
        const uint32_t c = rd<uint32_t>(nf, 0xec);
        if (c >= rd<uint32_t>(nf, 0xe8)) continue;
        if (listed(base, rd<uint8_t *>(nf, 0x18), rd<uint32_t>(nf, 0x10), names)) continue;
        uint8_t *to = rd<uint8_t *>(nf, 0xf0) + c * 0x24;
        for (int j = 0; j < 0x24; ++j) to[j] = names[j];
        *reinterpret_cast<uint32_t *>(nf + 0xec) = c + 1;
        nf[0x20] = 1;
    }
    if (rd<uint32_t>(nf, 0xec)) {
        *reinterpret_cast<uint64_t *>(nf + 0xe0) = 1;
        u3x_history_thread(base, s);
    }
}
// LIST_PLAYERSMET op 9: when the history changed and no fetch runs, the list is read back from the history
extern "C" U3X_FN void u3x_players_met_fetch(uintptr_t base, void *s) {
    uint8_t *nf = friend_block(base);
    if (rd<uint64_t>(nf, 0xd0) || !nf[0x20]) return;
    *reinterpret_cast<uint32_t *>(nf + 0x10) = 0;
    *reinterpret_cast<uint64_t *>(nf + 0xd0) = 1;
    int32_t c = u3h_pm_count(base);
    if (c < 0) c = 0;
    uint32_t n = static_cast<uint32_t>(c);
    if (rd<uint32_t>(nf, 0x14) <= n) n = rd<uint32_t>(nf, 0x14);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t id[0x24];
        for (uint8_t &b : id) b = 0;
        u3h_pm_get(base, i, id);
        u3x_players_met_add(base, s, id, 1, 0);
    }
    *reinterpret_cast<uint64_t *>(nf + 0xd0) = 0;
    nf[0x20] = 0;
    ++ext_ea()[74];
}
// match start: the remote players' names (+0x5b8) to the history, online only
extern "C" U3X_FN void u3x_match_players(uintptr_t base, void *s) {
    if ((ext_ea()[1] & 0x20u) || !rd<uint8_t *>(friend_block(base), 8)) return;
    uint8_t names[10 * 0x24];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 10; ++i) {
        const uint8_t *np = at<uint8_t>(base + 0x17d5c61) ? reinterpret_cast<const uint8_t *>(base + 0x17cb060 + 0x390) + i * 0x9c0 : nullptr;
        if (!np) continue;
        const uint8_t *t0 = at<const uint8_t *>(base + 0x17cb060 + 0x370), *t1 = at<const uint8_t *>(base + 0x17cb060 + 0x378);
        if (np == t0 || np == t1) continue;
        for (int j = 0; j < 0x24; ++j) names[n * 0x24 + j] = np[0x5b8 + j];
        ++n;
    }
    if (at<uint8_t>(base + 0x17d5c60)) return;
    if (rd<uint64_t>(friend_block(base), 0xc8)) u3x_friend_list_thread(base);
    u3x_players_met_add(base, s, names, n, 1);
    ++ext_ea()[75];
}
// friend events (offline 0 / presence 1, up to 64): the friend is added or flagged for a refresh; a friend added or
// lost on the account server rebuilds the list
extern "C" U3X_FN void u3x_friend_events(uintptr_t base) {
    uint8_t *nf = friend_block(base);
    if (u3h_take_rebuild() && !rd<uint64_t>(nf, 0xc8)) {
        *reinterpret_cast<uint32_t *>(nf) = 0;
        *reinterpret_cast<uint64_t *>(nf + 0xc8) = 1;
    }
    for (int k = 0; k < 64; ++k) {
        uint8_t id[0x24];
        for (uint8_t &b : id) b = 0;
        const int32_t ev = u3h_fr_event(base, id);
        if (ev < 0) return;
        if (ev == 0 || ev == 1) {
            if (rd<uint64_t>(nf, 0xc8)) u3x_friend_list_thread(base);
            u3x_friend_add(base, id, nullptr);
            ++ext_ea()[76];
        }
    }
}

// ===== v7.164: next batch, part 13 (lobby character re-dress, territory owner re-make) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_s16_char(uintptr_t base, uint8_t *cm);
U3H_IMPORT volatile uint32_t *u3h_s16_diag(void);
U3H_IMPORT uint32_t u3h_crc_sid(const char *p);
U3H_IMPORT void u3h_h01_tint(uintptr_t base, uint8_t *cm, uint8_t *ch, int32_t side);
U3H_IMPORT uint32_t *u3h_h08_state(int which);
}

// h001: the lobby character (the menu's character manager cm: side +0, wanted package sids +4 (50), part names
// +0xe00 + side * 0x58 (11), re-dress flag +0x104c) is re-dressed through Player::SetModel when a part differs from the
// character's (+0x1b80) once every wanted package is loaded; unloaded parts become "" (and are noted). A part named
// 'none' (hash 0xf114b39d) is no part. ec[1] bit 0 turns it off. s16_d: [1] re-dress asks, [2] differences, [3]
// re-dresses, [4] waits, [5] differing / unloaded parts, [6] skipped frames, [7] not a Player, [14] bit 31 just done.
namespace {
bool name_pointer(const void *p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47);
}
U3X_FN bool package_loaded(uintptr_t base, uint32_t sid) {
    if (!sid) return false;
    const uint8_t *a = game_call<const uint8_t *>(base + 0x10eb380, base + 0x1afcf10, sid);
    return a && aligned_pointer(a) && rd<uint32_t>(a, 0x30) == 8u;
}
U3X_FN uint32_t part_hash(uintptr_t base, const char *n) {
    if (!n || !name_pointer(n) || !*n) return 0;
    const uintptr_t hf = at<uintptr_t>(base + 0x1702720);
    const uint32_t h = reinterpret_cast<uint32_t (*)(int, const char *, int)>(hf)(0, n, 1);
    return h == 0xf114b39du ? 0u : h;
}
const char *part_name(const uint8_t *cm, int32_t side, int k) { return rd<const char *>(cm, 0xe00 + side * 0x58 + k * 8); }
}  // namespace
extern "C" U3X_FN int u3x_h01_loaded(uintptr_t base, uint32_t sid) { return package_loaded(base, sid); }
extern "C" U3X_FN uint32_t u3x_h01_hash(uintptr_t base, const char *n) { return part_hash(base, n); }
extern "C" U3X_FN void u3x_h01_redress(uintptr_t base, uint8_t *cm) {
    volatile uint32_t *d = u3h_s16_diag();
    cm[0x104c] = 0;
    uint8_t *ch = u3h_s16_char(base, cm);
    if (!ch || rd<uint32_t>(ch, 0x30) != 0xa5a63e9bu) { d[7] = d[7] + 1; return; }
    const int32_t side = rd<int32_t>(cm, 0);
    if (static_cast<uint32_t>(side) > 1u) return;
    if (ext_eb()[2] & 0x08000000u) {   // keep the character type index below 64 (PS4's GetDesc assert)
        const uint8_t *menu = reinterpret_cast<const uint8_t *>(base + 0x17bd080);
        const int32_t user = menu[0xdf5] ? 1 : rd<int32_t>(menu, 0x2694);
        if (static_cast<uint32_t>(game_call<int32_t>(base + 0x995ec0, static_cast<void *>(cm), user, side)) >= 0x40u) return;
    }
    game_call<void>(base + 0x9964d0, static_cast<void *>(cm));   // the look -> char +0x1c40
    const char *empty = reinterpret_cast<const char *>(base + 0x13ac250);
    const char *names[11];
    uint32_t unloaded = 0;
    for (int k = 0; k < 11; ++k) {
        const char *n = part_name(cm, side, k);
        const bool usable = n && name_pointer(n) && *n;
        if (usable && package_loaded(base, u3h_crc_sid(n))) {
            names[k] = n;
        } else {
            names[k] = empty;
            if (usable) unloaded |= 1u << k;
        }
    }
    d[5] = (d[5] & 0xffffu) | unloaded << 16;
    for (int k = 0; k < 10; ++k)
        game_call<void>(base + 0xbc8630, static_cast<void *>(ch), part_hash(base, names[k]) == 0xf114b39du ? empty : names[k], k);
    game_call<void>(base + 0xbcb7c0, static_cast<void *>(ch), names[10], 10);   // part 10 and the rebuild
    d[3] = d[3] + 1;
    uint8_t *o = virtual_call<uint8_t *>(ch, 0x1f8);
    if (o && aligned_pointer(o)) virtual_call<void>(o, 0x38, 0x80e056d1u);
    game_call<void>(base + 0x9965e0, static_cast<void *>(cm));
    d[14] = d[14] | 0x80000000u;
}
extern "C" U3X_FN void u3x_h01_tick(uintptr_t base, uint8_t *cm) {
    volatile uint32_t *d = u3h_s16_diag();
    if (ext_ec()[1] & 1u) return;
    if (!at<volatile uint8_t>(base + 0x17d5c61) || at<uint32_t>(base + 0x17d1868) == 2) return;   // the MP lobby only
    if (d[14] & 0x80000000u) {
        d[14] = d[14] & ~0x80000000u;
        d[6] = d[6] + 1;
        return;
    }
    if (!cm[0xf70]) return;
    if (cm[0x104c]) {
        d[1] = d[1] + 1;
        u3x_h01_redress(base, cm);
    }
    uint8_t *ch = u3h_s16_char(base, cm);
    if (!ch || rd<uint32_t>(ch, 0x30) != 0xa5a63e9bu) return;
    const int32_t side = rd<int32_t>(cm, 0);
    if (static_cast<uint32_t>(side) > 1u) return;
    bool ready = package_loaded(base, 0x67adb399u);
    for (int i = 0; i < 50 && ready; ++i) {
        const uint32_t v = rd<uint32_t>(cm, 4 + i * 4);
        if (v && !package_loaded(base, v)) ready = false;
    }
    for (int k = 0; k < 11 && ready; ++k) {   // every current part must be in the wanted list
        const char *n = part_name(cm, side, k);
        if (!n || !name_pointer(n) || !*n) continue;
        const uint32_t sid = u3h_crc_sid(n);
        if (sid == 0xf114b39du) continue;
        bool in = false;
        for (int i = 0; i < 50 && !in; ++i) in = rd<uint32_t>(cm, 4 + i * 4) == sid;
        if (!in) ready = false;
    }
    if (!ready) { d[4] = d[4] + 1; return; }
    uint32_t differ = 0;
    for (int k = 0; k < 11; ++k)
        if (part_hash(base, part_name(cm, side, k)) != part_hash(base, rd<const char *>(ch, 0x1b80 + k * 8))) {
            differ |= 1u << k;
            break;
        }
    if (!differ) {
        u3h_h01_tint(base, cm, ch, side);
        return;
    }
    d[2] = d[2] + 1;
    d[5] = (d[5] & 0xffff0000u) | differ;
    u3x_h01_redress(base, cm);
}

// h008: a territory's owner can change on the host without the local owner event, so its floor particles keep the old
// colour. Per territory (8 slots: object, owner, capture end, particle generation, change time) the helper re-makes the
// particles 0.5 s after an owner change the game did not re-make, and finishes a capture 1.5 s past its end time (owner
// = capturing team). An opt-in test aid (eb[3] bits 19..20) sets an unowned zone's owner after 10 s. ec[1] bit 5
// turns the re-make off. h08_d = ec[216..231]; n23 = ea[128..159].
namespace {
uint32_t *h08_diag() { return ext_ec() + 216; }
}
extern "C" U3X_FN int u3x_h08_on() { return !(ext_ec()[1] & 0x20u); }
// a particle handle {object, generation}: the object still carries the generation
extern "C" U3X_FN int u3x_h08_handle_ok(const uint8_t *h) {
    uint8_t **pp = rd<uint8_t **>(h, 0);
    const uint32_t g = rd<uint32_t>(h, 8);
    if (!pp || !g) return 0;
    const uint8_t *o = *pp;
    return o && rd<uint32_t>(o, 0) == g;
}
// kill a particle (flag +4 bit 0) under the particle manager's read lock (spinning up to 100000 times; a writer that
// keeps the lock leaves the handle alone), then clear the handle
extern "C" U3X_FN void u3x_h08_kill(uintptr_t base, uint8_t *h) {
    uint8_t *pm = reinterpret_cast<uint8_t *>(base + 0x1a4d910);
    uint64_t *lock = reinterpret_cast<uint64_t *>(pm + 0x10);
    uint8_t **pp = rd<uint8_t **>(h, 0);
    const uint32_t g = rd<uint32_t>(h, 8);
    if (!(pp && g && *pp && rd<uint32_t>(*pp, 0) == g)) return;
    bool got = false;
    for (int i = 0; i < 100000 && !got; ++i) {
        uint64_t c = __atomic_load_n(lock, __ATOMIC_RELAXED) & 0x7fffffffffffffffull;
        got = __atomic_compare_exchange_n(lock, &c, c + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }
    if (!got) return;
    if (pm[0xfbd]) {
        uint8_t *o = *pp;
        if (o && rd<uint32_t>(o, 0) == g) o[4] |= 1;
    }
    __atomic_fetch_sub(lock, 1ull, __ATOMIC_RELEASE);
    *reinterpret_cast<uint64_t *>(h + 8) = 0;
    *reinterpret_cast<uint64_t *>(h) = 0;
}
namespace {
U3X_FN void remake_particles(uintptr_t base, uint8_t *p) {
    for (int j = 0; j < 4; ++j) {
        u3x_h08_kill(base, p + 0x3b0 + 16 * j);
        u3x_h08_kill(base, p + 0x3f0 + 16 * j);
    }
}
}  // namespace
extern "C" U3X_FN void u3x_h08_one(uintptr_t base, uint8_t *p, int32_t team, uint32_t now, uint32_t *seen) {
    uintptr_t *objects = reinterpret_cast<uintptr_t *>(u3h_h08_state(0));
    uint32_t *gens = u3h_h08_state(1), *changed = u3h_h08_state(2), *ends = u3h_h08_state(4);
    int32_t *owners = reinterpret_cast<int32_t *>(u3h_h08_state(3));
    uint32_t *d = h08_diag(), *ea = ext_ea();
    const int32_t own = rd<int32_t>(p, 0x34c), capturer = rd<int32_t>(p, 0x350);
    const uint32_t end = rd<uint32_t>(p, 0x374);
    int k = -1, free_slot = -1;
    for (int i = 0; i < 8; ++i) {
        if (objects[i] == reinterpret_cast<uintptr_t>(p)) { k = i; break; }
        if (!objects[i] && free_slot < 0) free_slot = i;
    }
    const uint32_t gen = rd<uint32_t>(p, 0x3f0 + 8);
    if (k < 0 && free_slot >= 0) {
        k = free_slot;
        objects[k] = reinterpret_cast<uintptr_t>(p);
        owners[k] = own;
        ends[k] = 0;
        gens[k] = gen;
        changed[k] = 0;
        ea[147 + k] = now | 1u;
        ea[155] &= ~(1u << k);
    }
    if (k >= 0) *seen |= 1u << k;
    if (k >= 0 && owners[k] != own) {
        ++d[2];
        owners[k] = own;
        changed[k] = now | 1u;
        d[13] = static_cast<uint32_t>(own & 0xff) | (gens[k] == gen ? 0x100u : 0);
    }
    if (k >= 0 && !changed[k]) gens[k] = gen;
    d[4] = static_cast<uint32_t>(own & 0xff) | static_cast<uint32_t>(capturer & 0xff) << 8 | static_cast<uint32_t>(team & 0xff) << 16 | 1u << 24;
    d[5] = end;
    d[6] = now;
    d[7] = rd<uint32_t>(p, 0x37c);
    d[8] = static_cast<uint32_t>(u3x_h08_handle_ok(p + 0x3f0)) | static_cast<uint32_t>(u3x_h08_handle_ok(p + 0x3b0)) << 1 |
           static_cast<uint32_t>(rd<uint32_t>(p, 0x438) != 0) << 2;
    d[9] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p));
    if (!u3x_h08_on() || k < 0) return;
    {   // test aid (opt-in)
        const uint32_t test = (ext_eb()[3] >> 19) & 3u;
        if (test && own == -1 && (team == 0 || team == 1) && !(ea[155] & (1u << k)) && static_cast<int32_t>(now - (ea[147 + k] & ~1u)) >= 3000) {
            const int32_t new_owner = test & 1u ? team : (team ^ 1);
            ea[155] |= 1u << k;
            remake_particles(base, p);
            *reinterpret_cast<int32_t *>(p + 0x34c) = new_owner;
            *reinterpret_cast<uint64_t *>(p + 0x374) = 0;
            owners[k] = new_owner;
            game_call<void>(base + 0x7b2040, p);
            gens[k] = rd<uint32_t>(p, 0x3f0 + 8);
            changed[k] = 0;
            ++ea[156];
            ea[157] = static_cast<uint32_t>(new_owner & 0xff) | static_cast<uint32_t>(k) << 8 | static_cast<uint32_t>(team & 0xff) << 16;
            return;
        }
    }
    if (changed[k] && static_cast<int32_t>(now - (changed[k] & ~1u)) >= 150) {   // owner changed, particles not re-made
        if (gen == gens[k] && own != -1) {
            remake_particles(base, p);
            game_call<void>(base + 0x7b2040, p);
            ++d[12];
        }
        changed[k] = 0;
        gens[k] = rd<uint32_t>(p, 0x3f0 + 8);
    }
    if (end && capturer != -1 && own != capturer && static_cast<int32_t>(now - end) >= 450 && ends[k] != end) {   // finish the capture
        ends[k] = end;
        remake_particles(base, p);
        *reinterpret_cast<int32_t *>(p + 0x34c) = capturer;
        *reinterpret_cast<uint64_t *>(p + 0x374) = 0;
        owners[k] = capturer;
        game_call<void>(base + 0x7b2040, p);
        gens[k] = rd<uint32_t>(p, 0x3f0 + 8);
        changed[k] = 0;
        ++d[3];
        d[10] = static_cast<uint32_t>(capturer & 0xff) | static_cast<uint32_t>(own & 0xff) << 8;
    }
}

// ===== v7.165: next batch, part 14 (net time sync: samples, round start) =====

extern "C" {
U3H_IMPORT uint32_t *u3h_time_sync(int which);
U3H_IMPORT void u3h_sync_reset(void);
}

// b012: a guest asks the match host for its net time (the request itself, with its float timing, stays in C) until
// it has 8 samples {.., .., half round trip, delta}; then the 4 samples with the shortest round trips set the clock
// offset (0x17c3578 = now + their mean delta), and after a resync the players' time stamps move with the clock.
// State: [0] sample count, [1] the samples, [2] the random generator, [3] resync flag (byte), [4]..[6] the round start fix: host time, used time, pending (byte). eb[108] samples, eb[109] syncs, eb[110] last offset.
namespace {
struct TimeSample { int32_t a, b, half, delta; };
U3X_FN int32_t net_clock(uintptr_t base) {
    const uint8_t *c = at<const uint8_t *>(base + 0x17c3580);
    return c ? rd<int32_t>(c, 0xc) : 0;
}
U3X_RO const char synced_note[] = "net time synced with the match host, offset (1/300 s) ",
                  reset_note[] = "round start set back to the host's time ",
                  host_note[] = "round start (this PC is the match host), net time ",
                  from_host_note[] = "round start from the match host, its net time ", own_time_note[] = "  ... this PC's net time ",
                  samples_note[] = "  ... time samples so far ",
                  ahead_note[] = "  ... the start is ahead of this PC's clock (grenades would stay locked): this PC's time is used until the clocks are synced; ticks ahead ";
}  // namespace
extern "C" U3X_FN uint32_t u3x_sync_random(uint32_t *state) {
    *state = *state * 1103515245u + 12345u;
    return *state >> 8;
}
extern "C" U3X_FN int32_t u3x_net_clock(uintptr_t base) { return net_clock(base); }
// a sync finished: a round start that was set to this PC's time (the host's was ahead) goes back to the host's
extern "C" U3X_FN void u3x_time_synced(uintptr_t base, int32_t offset) {
    u3h_so_note(synced_note, nullptr, offset, nullptr);
    uint8_t &pending = *reinterpret_cast<uint8_t *>(u3h_time_sync(6));
    if (!pending) return;
    pending = 0;
    const int32_t host = static_cast<int32_t>(*u3h_time_sync(4)), used = static_cast<int32_t>(*u3h_time_sync(5));
    if (host - net_clock(base) > 0) return;   // still ahead: keep the local one
    uint8_t *mg = reinterpret_cast<uint8_t *>(base + 0x17cb060);
    if (rd<int32_t>(mg, 0x66ec) == used) *reinterpret_cast<int32_t *>(mg + 0x66ec) = host;
    if (rd<int32_t>(mg, 0x66f0) == used) *reinterpret_cast<int32_t *>(mg + 0x66f0) = host;
    if (rd<int32_t>(mg, 0x66f4) == used) *reinterpret_cast<int32_t *>(mg + 0x66f4) = host;
    if (at<int32_t>(base + 0x17d5c64) == used) at<int32_t>(base + 0x17d5c64) = host;
    u3h_so_note(reset_note, nullptr, host, nullptr);
}
// kind 236 (round start event, rdx = &start in the host's net ticks): a guest whose clock is behind the start uses its
// own time until synced, then resyncs. Returns 0: the call runs.
extern "C" U3X_FN int u3x_round_start(uintptr_t base, const uint64_t *r) {
    int32_t *t = reinterpret_cast<int32_t *>(static_cast<uintptr_t>(r[6]));
    if (!t) return 0;
    const uint8_t *ms = reinterpret_cast<const uint8_t *>(base + 0x17d6420);
    const bool host = !rd<uint64_t>(ms, 0x18) || rd<uint32_t>(ms, 0x12e4) == rd<uint32_t>(ms, 0x12e8);
    const int32_t now = net_clock(base), got = *t;
    if (host) {
        u3h_so_note(host_note, nullptr, got, nullptr);
        return 0;
    }
    u3h_so_note(from_host_note, nullptr, got, nullptr);
    u3h_so_note(own_time_note, nullptr, now, nullptr);
    u3h_so_note(samples_note, nullptr, static_cast<int32_t>(*u3h_time_sync(0)), nullptr);
    if (got - now > 0) {
        *t = now;
        *u3h_time_sync(4) = static_cast<uint32_t>(got);
        *u3h_time_sync(5) = static_cast<uint32_t>(now);
        *reinterpret_cast<uint8_t *>(u3h_time_sync(6)) = 1;
        u3h_so_note(ahead_note, nullptr, got - now, nullptr);
    }
    if (!(ext_eb()[0] & 8)) {
        *reinterpret_cast<uint8_t *>(u3h_time_sync(3)) = 1;
        u3h_sync_reset();
    }
    return 0;
}
// one answer {our send time t0, the host's time th}
extern "C" U3X_FN void u3x_time_sample(uintptr_t base, int32_t t0, int32_t th) {
    int32_t &count = *reinterpret_cast<int32_t *>(u3h_time_sync(0));
    TimeSample *smp = reinterpret_cast<TimeSample *>(u3h_time_sync(1));
    if (count >= 8) return;
    const int32_t i = count, now = net_clock(base), half = (now - t0) / 2;
    smp[i].half = half;
    if (half <= 0) return;
    smp[i].delta = (th - now) + half;
    uint32_t *eb = ext_eb();
    ++eb[108];
    if (i == 7) {
        for (int a = 0; a < 8; ++a)   // by round trip (the PS3 sort keeps equal ones in place)
            for (int b = 0; b + 1 < 8 - a; ++b)
                if (smp[b].half > smp[b + 1].half) {
                    const TimeSample t = smp[b];
                    smp[b] = smp[b + 1];
                    smp[b + 1] = t;
                }
        const int32_t sum = smp[0].delta + smp[1].delta + smp[2].delta + smp[3].delta;
        at<int32_t>(base + 0x17c3578) = now + sum / 4;
        ++eb[109];
        eb[110] = static_cast<uint32_t>(sum / 4);
        uint8_t &resync = *reinterpret_cast<uint8_t *>(u3h_time_sync(3));
        if (resync) {   // the stamps taken on the old clock move with it; the next send times restart
            resync = 0;
            for (int k = 0; k < 10; ++k) {
                uint8_t *np = reinterpret_cast<uint8_t *>(base + 0x17cb060 + 0x390) + k * 0x9c0;
                if (!np[0xe1]) continue;
                for (uint32_t o = 0x39c; o <= 0x3a8; o += 4) {
                    if (o == 0x3a4) continue;
                    if (rd<int32_t>(np, o)) *reinterpret_cast<int32_t *>(np + o) += sum / 4;
                }
                *reinterpret_cast<uint32_t *>(np + 0x5f0) = 0;
                *reinterpret_cast<uint32_t *>(np + 0x5f4) = 0;
            }
        }
    }
    ++count;
}

// ===== v7.166: next batch, part 15 (the friend header) =====

extern "C" {
U3H_IMPORT void u3h_set_icon_plain(uintptr_t base, int32_t *ev, uint32_t id);
}

// The friend header (enum 0x13) for the friend picked at 0x17c3871: name, state icon, the net game's icon and game
// type icon, and two lines: the friend's status ("Uncharted 3", "Playing another game") and what they are doing
// (lobby / searching with the party size, or the map, game type and difficulty / time left of their match). Without
// that friend: our own name, the title and the party lock state. ea[86] calls.
namespace {
U3X_BSS uint8_t zero_rule[0x58];
U3X_RO const char dash[] = "- ", slash[] = " / ", newline_dash[] = "\n- ";
U3X_FN const uint8_t *game_rule(void *s, const uint8_t *ng) {   // the rule set of the net game's type (0x58-byte entries)
    const uint8_t *rl = dc_table(s, 0xf985c7b6u);
    const int32_t g = rd<int32_t>(ng, 0xc);
    return (rl && g < rd<int32_t>(rl, 0)) ? rd<const uint8_t *>(rl, 8) + static_cast<int64_t>(g) * 0x58 : zero_rule;
}
U3X_FN const uint8_t *net_game(void *s, int32_t index) {
    const int16_t i = static_cast<int16_t>(index);
    const uint8_t *t = dc_table(s, 0x3b9a067du);
    if (!t || i < 0 || i >= rd<int32_t>(t, 0)) return nullptr;
    return rd<const uint8_t *>(t, 8) + static_cast<int64_t>(i) * 0xd0;
}
}  // namespace
extern "C" U3X_FN int u3x_friend_header(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414e13u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    uint8_t *e = u3h_fr_find(base, reinterpret_cast<const uint8_t *>(base + 0x17c3871));
    ++ext_ea()[86];
    const int32_t op = ev[0];
    if (!e) {
        if (op == 1) { set_row_text(ev, reinterpret_cast<const char *>(base + 0x17d6010)); return 1; }   // our own name
        if (op != 2) return 1;
        if (ev[1] == 0) { set_row_text(ev, u3h_text(base, s, 0xfd2de693u)); return 1; }
        if (ev[1] != 1) return 1;
        const uint32_t id = (at<uint8_t>(base + 0x17dc298 + 0x12ec) || at<uint8_t>(base + 0x17c3570)) ? 0x40d3b7a2u : 0x052ac89au;   // locked
        set_row_text(ev, u3h_text(base, s, id));
        return 1;
    }
    if (op == 1) { set_row_text(ev, reinterpret_cast<const char *>(e + 1)); return 1; }
    if (op == 4) {
        const uint8_t *d = u3h_fr_pdata(base, e);
        const uint8_t *ng = d ? net_game(s, rd<int32_t>(d, 0x14)) : nullptr;
        const uint32_t column = static_cast<uint32_t>(ev[0x14]);
        if (column == 0xb81e2263u) {   // avatar: none on PS4 entries
            ev[2] = 0; ev[4] = 0; ev[5] = 0; ev[6] = 0; ev[7] = 0; ev[8] = 0; ev[9] = -1; ev[10] = 0; ev[11] = -1; ev[12] = 0x3f800000;
            return 1;
        }
        if (column == 0x0a071025u) {
            if (!ng) return 1;
            ev[2] = 0x24; ev[4] = 0; ev[5] = 0; ev[6] = rd<int32_t>(ng, 4); ev[7] = 0; ev[8] = 0; ev[9] = -1; ev[10] = 0; ev[11] = -1; ev[12] = 0x3f800000;
            return 1;
        }
        if (column == 0x57135906u) {
            if (!ng) return 1;
            u3h_set_icon_plain(base, ev, rd<uint32_t>(game_rule(s, ng), 8));
            return 1;
        }
        if (column == 0x8e39ed40u) u3x_friend_icon(ev, u3x_friend_state_icon(base, e));
        return 1;
    }
    if (op != 2) return 1;
    if (ev[1] == 0) {
        if ((u3h_fr_pdata(base, e) && rd<int32_t>(e, 0x1b0) == 1) || u3h_fr_in_title(e)) { set_row_text(ev, u3h_text(base, s, 0xfd2de693u)); return 1; }
        if (rd<int32_t>(e, 0x1b0) == 0) return 1;
        set_row_text(ev, u3h_text(base, s, 0x953825d0u));   // "Playing another game"
        return 1;
    }
    if (ev[1] != 1) return 1;
    const uint8_t *d = u3h_fr_pdata(base, e);
    if (!d || rd<int32_t>(e, 0x1b0) != 1) return 1;
    char buf[0x80];
    u3x_friend_party_suffix(base, s, buf, d[5]);
    if (d[4] < 2) { set_row_text(ev, u3x_friend_join(u3h_text(base, s, 0x84041d79u), buf)); return 1; }
    if (d[4] == 3) { set_row_text(ev, u3x_friend_join(u3h_text(base, s, 0xea3bb70eu), buf)); return 1; }
    if (d[4] != 2) return 1;
    const uint8_t *ng = net_game(s, rd<int32_t>(d, 0x14));
    if (!ng) return 1;
    const uint8_t *maps = dc_table(s, 0x0dec97a4u), *map = nullptr;   // 0x98-byte entries, id +0
    if (maps) {
        const uint8_t *me = rd<const uint8_t *>(maps, 8);
        for (int32_t k = 0; k < rd<int32_t>(maps, 0); ++k)
            if (rd<uint32_t>(me, k * 0x98) == rd<uint32_t>(ng, 4)) { map = me + static_cast<int64_t>(k) * 0x98; break; }
    }
    if (!map) return 1;
    char time_left[0x80];
    time_left[0] = 0;
    const int64_t end = rd<int64_t>(d, 0x20);
    if (end) {   // "Match ends in [TIME]"
        const uintptr_t clock = ext_clock_fn();
        const int64_t now = clock ? reinterpret_cast<int64_t (*)(int64_t *)>(clock)(nullptr) : 0;
        const int64_t left = end - now;
        const int32_t v = left < 1 ? 0 : left > 0x34bbf ? 0x34bc0 : static_cast<int32_t>(left);
        game_call<void>(base + 0xf33010, base + 0x17026b8, time_left, 0x80u, 0x93ac6446u, static_cast<int64_t>(v), int64_t{0});
    }
    const char *map_name = u3h_text(base, s, rd<uint32_t>(map, 4)), *type_name = u3h_text(base, s, rd<uint32_t>(game_rule(s, ng), 0));
    char *o = u3h_fr_buffer(), *lim = o + 0xff;
    char *p = append_chars(o, lim, dash);
    p = append_chars(p, lim, map_name);
    p = append_chars(p, lim, slash);
    p = append_chars(p, lim, type_name);
    const uint32_t type = rd<uint32_t>(ng, 0xc);
    if (type <= 0x12 && ((1ull << type) & 0x10810)) {   // co-op: the difficulty
        p = append_chars(p, lim, slash);
        p = append_chars(p, lim, game_call<const char *>(base + 0x741600, rd<int32_t>(d, 0x18)));
    } else if (!(type <= 0x12 && ((1ull << type) & 0x62300))) {   // the rules' name and the time left
        p = append_chars(p, lim, slash);
        p = append_chars(p, lim, u3h_text(base, s, rd<uint32_t>(ng, 8)));
        p = append_chars(p, lim, newline_dash);
        p = append_chars(p, lim, time_left);
    }
    *p = 0;
    set_row_text(ev, o);
    return 1;
}

// ===== v7.167: next batch, part 16 (rank badges: prestige back, co-op star, ND paw) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_l01_state(int which);
U3H_IMPORT int32_t u3h_icon_index(uintptr_t base, uint32_t id);
}

// l001: a rank badge answer carries a marker (0x4c303142) and the rank word v (legacy level bits 16..29, paw bit 30,
// co-op star bit 31); the badge draw (kind 230, the type-2 icon quad of the item draw) then puts the legacy level's
// prestige back behind the rank art, the co-op star (a type-3 icon) over it and the ND paw in its lower-left quarter.
// eb[2] bit 20 turns it off. State: [0] l01_d (16), [1] l01_q (4), [2] l01_t (16), [3] backs (6 pointers), [4] paw
// texture, [5] star index + 1. l01_d: [5] tags, [6] draws, [7] backs, [8] stars, [9] paws, [10] star index, [11] missing
// textures, [12] last word, [13] stars without a layout. ea[212..214] back requests, loaded, missing.
namespace {
constexpr uint32_t badge_marker = 0x4c303142u;
U3X_FN bool badges_on() { return !(ext_eb()[2] & (1u << 20)); }
volatile uint32_t *badge_diag() { return reinterpret_cast<volatile uint32_t *>(u3h_l01_state(0)); }
void **badge_backs() { return reinterpret_cast<void **>(u3h_l01_state(3)); }
void *&paw_texture() { return *reinterpret_cast<void **>(u3h_l01_state(4)); }
int32_t &star_index() { return *reinterpret_cast<int32_t *>(u3h_l01_state(5)); }
U3X_RO const char back_name[] = "ranks/prestige-back-", paw_name[] = "ranks/ndpaw-icon";
U3X_FN void *find_texture(uintptr_t base, void *s, const char *name) {
    return reinterpret_cast<void *(*)(uintptr_t, const char *)>(state_native(s, 20))(base + 0x1b31128, name);
}
U3X_FN void back_texture_name(char *nm, uint32_t level) {
    int k = 0;
    while (back_name[k]) { nm[k] = back_name[k]; ++k; }
    nm[k] = static_cast<char>('0' + level);
    nm[k + 1] = 0;
}
// fn(a..f) with a float in xmm0
U3X_FN uint64_t call_with_float(uintptr_t fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint32_t x0) {
    register uint64_t r8 asm("r8") = e;
    register uint64_t r9 asm("r9") = f;
    asm volatile("movd %[x],%%xmm0\n\tpush %%rbp\n\tmov %%rsp,%%rbp\n\tand $-16,%%rsp\n\tcall *%%rax\n\tmov %%rbp,%%rsp\n\tpop %%rbp"
                 : "+a"(fn), "+D"(a), "+S"(b), "+d"(c), "+c"(d), "+r"(r8), "+r"(r9)
                 : [x] "m"(x0)
                 : "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                   "xmm14", "xmm15", "memory", "cc");
    return fn;
}
// quad corners (0x40 apart, x +0, y +4): (xa, ya), (xb, ya), (xb, yb), (xa, yb)
U3X_FN void quad_rect(uint8_t *vt, uint32_t xa, uint32_t ya, uint32_t xb, uint32_t yb) {
    *reinterpret_cast<uint32_t *>(vt + 0x00) = xa;
    *reinterpret_cast<uint32_t *>(vt + 0x04) = ya;
    *reinterpret_cast<uint32_t *>(vt + 0x40) = xb;
    *reinterpret_cast<uint32_t *>(vt + 0x44) = ya;
    *reinterpret_cast<uint32_t *>(vt + 0x80) = xb;
    *reinterpret_cast<uint32_t *>(vt + 0x84) = yb;
    *reinterpret_cast<uint32_t *>(vt + 0xc0) = xa;
    *reinterpret_cast<uint32_t *>(vt + 0xc4) = yb;
}
}  // namespace
// the rank word's star and paw bits from the record (star: all five co-op parts done, else the old bit)
extern "C" U3X_FN uint32_t u3x_rank_bits(const uint8_t *rec, uint32_t star_old) {
    const uint32_t star = ((rd<uint32_t>(rec, 0x1624) & 0x1fu) == 0x1fu) ? 1u : star_old;
    const uint32_t paw = rd<uint32_t>(rec, 0x1f1c) ? 1u : 0u;
    return (star << 31) | (paw << 30);
}
// mark a badge answer and look its textures up (again on every answer: the texture cache reuses entries)
extern "C" U3X_FN void u3x_rank_tag(uintptr_t base, void *s, int32_t *ev, uint32_t v) {
    if (!badges_on()) return;
    volatile uint32_t *d = badge_diag();
    const uint32_t level = (v >> 16) & 0x3fffu, star = v >> 31, paw = (v >> 30) & 1u;
    if (!(level >= 1 && level <= 5) && !star && !paw) return;
    if (!state_native(s, 20)) return;
    if (level >= 1 && level <= 5) {
        char nm[24];
        back_texture_name(nm, level);
        badge_backs()[level] = find_texture(base, s, nm);
        if (!badge_backs()[level]) d[11] = d[11] + 1;
    }
    if (paw) {
        paw_texture() = find_texture(base, s, paw_name);
        if (!paw_texture()) d[11] = d[11] + 1;
    }
    if (star) {
        star_index() = u3h_icon_index(base, 0x0242a063u) + 1;
        d[10] = static_cast<uint32_t>(star_index());
    }
    ev[6] = static_cast<int32_t>(badge_marker);
    ev[7] = static_cast<int32_t>(v);
    d[5] = d[5] + 1;
    d[12] = v;
}
// the prestige back of a legacy level, once it is loaded (a miss queues the load)
extern "C" U3X_FN void *u3x_rank_back(uintptr_t base, void *s, uint32_t level) {
    if (level < 1 || level > 5 || !state_native(s, 20)) return nullptr;
    char nm[24];
    back_texture_name(nm, level);
    uint8_t *t = static_cast<uint8_t *>(find_texture(base, s, nm));
    ++ext_ea()[212];
    if (!t || !t[0x10]) return nullptr;
    ++ext_ea()[213];
    badge_backs()[level] = t;
    return t;
}
// a texture's sampler: mip filter above 1 set to 1 (sharp menu art)
extern "C" U3X_FN void u3x_texture_sharp(void *t) {
    const uint8_t *o = static_cast<const uint8_t *>(t);
    if (!aligned_pointer(o) || (rd<uint32_t>(o, 0) & 2) != 2) return;
    uint8_t *m = rd<uint8_t *>(o, 8);
    if (!aligned_pointer(m)) return;
    uint32_t *w = reinterpret_cast<uint32_t *>(m + 0xc);
    if (((*w >> 16) & 15) > 1) *w = (*w & 0xfff0ffffu) | 0x10000u;
}
// kind 230: r = {ctx, a2, texture, vertices (4 x 0x40), item layout, colour, rbp of the item draw}; the original quad
// is always drawn
extern "C" U3X_FN int u3x_rank_draw(uintptr_t base, void *s, const uint64_t *r) {
    const uintptr_t quad = base + 0x63dc40;
    void *ctx = reinterpret_cast<void *>(r[0]), *a2 = reinterpret_cast<void *>(r[1]), *tex = reinterpret_cast<void *>(r[2]);
    uint8_t *vt = reinterpret_cast<uint8_t *>(r[3]);
    const uint8_t *layout = reinterpret_cast<const uint8_t *>(r[4]);
    if (!badges_on() || !aligned_pointer(layout) || rd<uint32_t>(layout, 0) != 2u || rd<uint32_t>(layout, 0x10) != badge_marker || !aligned_pointer(vt)) {
        game_call<void>(quad, ctx, a2, tex, static_cast<void *>(vt));
        return 1;
    }
    volatile uint32_t *d = badge_diag();
    uint32_t *q = reinterpret_cast<uint32_t *>(u3h_l01_state(1)), *t = reinterpret_cast<uint32_t *>(u3h_l01_state(2));
    const uint32_t v = rd<uint32_t>(layout, 0x14), level = (v >> 16) & 0x3fffu;
    d[6] = d[6] + 1;
    const uint32_t x0 = rd<uint32_t>(vt, 0), y0 = rd<uint32_t>(vt, 4), x1 = rd<uint32_t>(vt, 0x80), y1 = rd<uint32_t>(vt, 0x84);
    const uint32_t xm = f_mul(f_add(x0, x1), 0x3f000000u), ym = f_mul(f_add(y0, y1), 0x3f000000u);
    q[0] = x0;
    q[1] = y0;
    q[2] = x1;
    q[3] = y1;
    if (aligned_pointer(tex))
        for (int k = 0; k < 16; ++k) t[k] = static_cast<const uint32_t *>(tex)[k];
    if (level >= 1 && level <= 5) {
        void *back = u3x_rank_back(base, s, level);
        if (back) {
            game_call<void>(quad, ctx, a2, back, static_cast<void *>(vt));
            d[7] = d[7] + 1;
        } else {
            ++ext_ea()[214];
        }
    }
    game_call<void>(quad, ctx, a2, tex, static_cast<void *>(vt));   // the rank art
    if ((v >> 31) && star_index() > 0) {   // the co-op star, a type-3 icon item laid out over the badge
        uint32_t item[12];
        for (uint32_t &w : item) w = 0;
        item[0] = 3;
        item[5] = 0x0242a063u;
        item[6] = 1;
        item[7] = static_cast<uint32_t>(star_index() - 1);
        item[9] = 0xffffffffu;
        item[10] = 0x3f800000u;
        uint8_t laid[0x80];
        for (uint8_t &b : laid) b = 0;
        uint32_t box[2], zero16[4], pos[2];
        for (uint32_t &w : zero16) w = 0;
        box[0] = f_sub(x1, xm);
        box[1] = f_sub(y1, ym);
        pos[0] = xm;
        pos[1] = ym;
        call_with_float(base + 0x640ac0, reinterpret_cast<uint64_t>(laid), reinterpret_cast<uint64_t>(item), base + 0x13ac250,
                        reinterpret_cast<uint64_t>(box), reinterpret_cast<uint64_t>(zero16), 0xffffffffu, 0x42800000u);
        if (rd<uint64_t>(laid, 0x48) && rd<uint32_t>(laid, 0x3c)) {
            const uint32_t flag = r[6] ? rd<uint32_t>(reinterpret_cast<const uint8_t *>(r[6]) - 0x2080, 0) : 0;
            call_with_float(base + 0x641560, reinterpret_cast<uint64_t>(laid), reinterpret_cast<uint64_t>(a2), reinterpret_cast<uint64_t>(ctx),
                            reinterpret_cast<uint64_t>(pos), static_cast<uint32_t>(r[5]), flag, 0);
            d[8] = d[8] + 1;
        } else {
            d[13] = d[13] + 1;
        }
    }
    void *paw = paw_texture();
    if (((v >> 30) & 1u) && paw && static_cast<const uint8_t *>(paw)[0x10]) {   // the ND paw, lower-left quarter
        uint8_t corners[0x110];
        for (int k = 0; k < 0x110; ++k) corners[k] = vt[k];
        quad_rect(corners, x0, ym, xm, y1);
        game_call<void>(quad, ctx, a2, paw, static_cast<void *>(corners));
        d[9] = d[9] + 1;
    }
    return 1;
}

// ===== v7.168: next batch, part 17 (tab box quads) =====

extern "C" {
U3H_IMPORT uint32_t u3h_h13_mode(void);
U3H_IMPORT uint32_t *u3h_w03_diag(void);
}

// w003: PS4's box item (type 0x2d, the tab highlight) is drawn here as PS3 draws it: the gradient texture 0x6e9d9193
// as a quad with the item colour times the state colour, rotated uv by the item flags, aligned in the rect (left /
// centre / right, top / middle / bottom) plus the state offset, with a drop shadow (half alpha, dimmer for dark
// colours) when the state asks. ea[240] bit 2 turns it on. w03_d / ec[164..171]: calls, off, refused, quads,
// no texture, last colour, last position, last flags.
namespace {
U3X_RO const uint32_t corner_uv[8] = {0, 0, 0x3f800000u, 0, 0x3f800000u, 0x3f800000u, 0, 0x3f800000u};
U3X_FN uint64_t gradient_texture(uintptr_t b) {
    int32_t idx = -1;
    const uint8_t *t = at<const uint8_t *>(b + 0x1b24bc8);
    if (aligned_pointer(t)) {
        const uint8_t *tb = rd<const uint8_t *>(t, 0x20);
        if (aligned_pointer(tb)) {
            const uint32_t n = rd<uint16_t>(tb, 2);
            const uint8_t *e = rd<const uint8_t *>(tb, 8);
            if (n && n < 4096 && aligned_pointer(e))
                for (uint32_t i = 0; i < n; ++i)
                    if (rd<uint32_t>(e, 8 + i * 0x28) == 0x6e9d9193u) { idx = static_cast<int32_t>(i); break; }
        }
    }
    GameCall c;
    for (uint32_t k = 0; k < sizeof c; ++k) reinterpret_cast<uint8_t *>(&c)[k] = 0;
    c.fn = b + 0x11b89b0;
    c.i[0] = 0x6e9d9193u;
    c.i[1] = static_cast<uint32_t>(idx);
    return u3h_game_call(&c);
}
U3X_FN void box_quad(uintptr_t b, uint64_t dc, uint64_t tex, const uint8_t *it, uint32_t x, uint32_t y, uint32_t argb) {
    constexpr uint32_t inv255 = 0x3b808081u, one = 0x3f800000u;
    const uint32_t ic = rd<uint32_t>(it, 0x24);
    uint32_t c = 0;
    for (int s = 0; s < 32; s += 8) c |= ((((argb >> s) & 0xffu) * ((ic >> s) & 0xffu)) >> 8) << s;
    const uint32_t col[4] = {f_mul(f_from_int(static_cast<int32_t>((c >> 16) & 0xffu)), inv255), f_mul(f_from_int(static_cast<int32_t>((c >> 8) & 0xffu)), inv255),
                             f_mul(f_from_int(static_cast<int32_t>(c & 0xffu)), inv255), f_mul(f_from_int(static_cast<int32_t>(c >> 24)), inv255)};
    const uint32_t flags = rd<uint32_t>(it, 0x10), rot = (flags | flags >> 24) & 3u;
    const uint32_t w = rd<uint32_t>(it, 0x3c), h = rd<uint32_t>(it, 0x40), x1 = f_add(x, w), y1 = f_add(y, h);
    const uint32_t px[4] = {x, x1, x1, x}, py[4] = {y, y, y1, y1};
    alignas(16) uint32_t v[0x44];
    for (uint32_t &q : v) q = 0;
    for (int k = 0; k < 4; ++k) {
        uint32_t *q = v + k * 16;
        const uint32_t u = (rot + static_cast<uint32_t>(k)) & 3u;
        q[0] = px[k];
        q[1] = py[k];
        q[3] = corner_uv[u * 2];
        q[4] = corner_uv[u * 2 + 1];
        q[8] = col[0];
        q[9] = col[1];
        q[10] = col[2];
        q[11] = col[3];
        q[12] = q[13] = q[14] = q[15] = one;
    }
    uint64_t ctx[6], td[4];
    for (uint64_t &q : ctx) q = 0;
    ctx[0] = dc;
    td[0] = tex;
    td[1] = td[2] = td[3] = 0;
    if (u3h_h13_mode() == 3) {   // experiment: opaque magenta, untextured
        for (int k = 0; k < 4; ++k) {
            uint32_t *q = v + k * 16;
            q[8] = one;
            q[9] = 0;
            q[10] = one;
            q[11] = one;
        }
        td[0] = 0;
    }
    GameCall cc;
    for (uint32_t k = 0; k < sizeof cc; ++k) reinterpret_cast<uint8_t *>(&cc)[k] = 0;
    cc.fn = b + 0xe42310;
    cc.i[0] = reinterpret_cast<uint64_t>(ctx);
    cc.i[1] = reinterpret_cast<uint64_t>(td);
    cc.i[2] = reinterpret_cast<uint64_t>(v);
    u3h_game_call(&cc);
    uint32_t *d = u3h_w03_diag(), *ec = ext_ec();
    ++d[3];
    ++ec[167];
    ec[169] = d[5] = c;
    ec[170] = d[6] = (static_cast<uint32_t>(f_to_int(x)) & 0xffffu) | static_cast<uint32_t>(f_to_int(y)) << 16;
    ec[171] = d[7] = flags;
}
}  // namespace
extern "C" U3X_FN int u3x_w03_ok(const void *p) { return plausible(p); }
extern "C" U3X_FN uint64_t u3x_gradient_texture(uintptr_t b) { return gradient_texture(b); }
extern "C" U3X_FN void u3x_box_quad(uintptr_t b, uint64_t dc, uint64_t tex, const uint8_t *it, uint32_t x, uint32_t y, uint32_t argb) {
    box_quad(b, dc, tex, it, x, y, argb);
}
// the value stub at 0x63d6f0: r = {r11, r10, r9, r8, rdi, rsi, rdx, rcx, rax, rbp, ret, arg7, arg8}; 1 = drawn here
extern "C" U3X_FN int u3x_box_item(uintptr_t b, const uint64_t *r) {
    uint32_t *d = u3h_w03_diag(), *ec = ext_ec();
    const uint8_t *it = reinterpret_cast<const uint8_t *>(r[7]);
    if (!plausible(it) || rd<uint32_t>(it, 0) != 0x2du) return 0;
    ++d[0];
    ++ec[164];
    if (!(ext_ea()[240] & 4u)) { ++d[1]; ++ec[165]; return 0; }
    const uint32_t *st = reinterpret_cast<const uint32_t *>(r[6]), *rc = reinterpret_cast<const uint32_t *>(r[3]);
    if (!plausible(st) || !plausible(rc)) { ++d[2]; ++ec[166]; return 0; }
    if (at<uint32_t>(b + 0xe42310) != 0xe5894855u || at<uint32_t>(b + 0x11b89b0) != 0xe5894855u) { ++d[2]; ++ec[166]; return 0; }
    constexpr uint32_t f255 = 0x437f0000u, f001 = 0x3a83126fu, f015 = 0x3e19999au, half = 0x3f000000u, one = 0x3f800000u;
    const uint32_t alpha = st[4];
    if (static_cast<uint8_t>(f_to_int(f_mul(alpha, f255))) == 0 || f_less(st[0], f001)) return 1;   // nothing to draw
    uint32_t m = one;
    if (f_less(st[1], f015) && f_less(st[2], f015) && f_less(st[3], f015)) m = half;
    const int32_t ha = static_cast<int32_t>(r[2]), va = static_cast<int32_t>(r[11]);
    const uint32_t w = rd<uint32_t>(it, 0x3c), h = rd<uint32_t>(it, 0x40);
    uint32_t x, y;
    if (ha == 2) x = f_sub(f_add(rc[0], rc[2]), w);
    else if (ha == 0) x = rc[0];
    else x = f_add(f_mul(f_sub(rc[2], w), half), rc[0]);
    if (va == 2) y = f_sub(f_add(rc[1], rc[3]), h);
    else if (va == 0) y = rc[1];
    else y = f_add(f_mul(f_sub(rc[3], h), half), rc[1]);
    x = f_add(x, st[8]);
    y = f_add(y, st[9]);
    const uint64_t tex = gradient_texture(b);
    if (!tex) { ++d[4]; ++ec[168]; return 1; }
    uint64_t dc = r[5];
    if (u3h_h13_mode() == 1) dc = 0;   // experiment: the queued UI pass
    ec[175] = static_cast<uint32_t>(dc != 0);
    if (f_less(f001, st[5])) {   // drop shadow
        uint32_t s = f_mul(st[5], half);
        if (f_less(one, s)) s = one;
        const uint32_t a = static_cast<uint32_t>(f_to_int(f_mul(f_mul(f_mul(f_mul(m, s), alpha), half), f255)));
        box_quad(b, dc, tex, it, x, f_add(y, f_from_int(f_to_int(f_add(st[5], half)))), a << 24);
    }
    const uint32_t argb = (static_cast<uint32_t>(f_to_int(f_mul(st[2], f255))) & 0xffu) << 8 | (static_cast<uint32_t>(f_to_int(f_mul(st[1], f255))) & 0xffu) << 16 |
                          (static_cast<uint32_t>(f_to_int(f_mul(st[3], f255))) & 0xffu) | static_cast<uint32_t>(f_to_int(f_mul(alpha, f255))) << 24;
    box_quad(b, dc, tex, it, x, y, argb);
    return 1;
}

// ===== v7.169: next batch, part 18 (kill points: popups, scores, awards, point values) =====

extern "C" {
U3H_IMPORT int32_t u3h_kr_mode_type(uintptr_t base);
U3H_IMPORT int u3h_kr_30feec(uintptr_t base);
}

// v5.16 kill points (PS3 1.19's kill handler). ExtState +0x2d70 (krp): [0] player score adds, [1] team score adds,
// [2] awards, [3] awarded points, [4] popups asked, [5] last points, [6] popups shown. Switch bits at state +0x3d78:
// 0x4000 no points, 0x8000 no popups.
namespace {
constexpr uint32_t no_points = 0x4000u, no_popups = 0x8000u;
uint32_t *ext_krp() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2d70); }
int32_t *dc_value(uintptr_t base, uint32_t id) {
    uint64_t *p = game_call<uint64_t *>(base + 0x11f2bf0, id);
    return p ? reinterpret_cast<int32_t *>(*p) : nullptr;
}
}  // namespace
// the "+points" popup over the attacker (or, without one, the player at the kill position) on the player's HUD
extern "C" U3X_FN void u3x_kill_popup(uintptr_t base, void *s, uint8_t *np, uint8_t *attacker, const uint8_t *info, int32_t pts, uint32_t type, uint32_t dbl) {
    uint32_t *krp = ext_krp();
    ++krp[4];
    krp[5] = static_cast<uint32_t>(pts);
    if ((kr_switches(s) & (no_popups | no_points)) || !np) return;
    uint8_t *obj = reinterpret_cast<uint8_t *>(base + 0x1805fa0) + static_cast<uint64_t>(rd<uint32_t>(np, 0x1e8)) * 0x270;
    uint8_t *list = rd<uint8_t *>(obj, 0x238);
    if (!list) return;
    uint8_t *src = attacker ? handle_object(rd<uint8_t **>(attacker, 0x10), rd<uint32_t>(attacker, 0x18)) : nullptr;
    bool moved = false;
    if (!src) {
        src = handle_object(rd<uint8_t **>(np, 0x10), rd<uint32_t>(np, 0x18));
        moved = true;
    }
    if (!src) return;
    uint32_t frame[0x18];
    for (int i = 0; i < 0x18; ++i) frame[i] = rd<uint32_t>(src, 0xb0 + i * 4);
    if (moved) {
        frame[8] = rd<uint32_t>(info, 0x20);
        frame[9] = rd<uint32_t>(info, 0x24);
        frame[10] = rd<uint32_t>(info, 0x28);
    }
    frame[9] = f_add(frame[9], 0x3fc00000u);   // 1.5 up
    for (int k = 0; k < 12; ++k) {
        uint8_t *slot = list + 0x2690 + k * 0x70;
        if (rd<int32_t>(slot, 0x60) != -1) continue;
        at<uint8_t *>(base + 0x1806480) = obj;
        at<uint8_t *>(base + 0x1b24bd8) = obj;
        for (int i = 0; i < 0x18; ++i) reinterpret_cast<uint32_t *>(slot)[i] = frame[i];
        *reinterpret_cast<int32_t *>(slot + 0x60) = pts;
        *reinterpret_cast<uint32_t *>(slot + 0x64) = at<uint32_t>(base + 0x17b4a90 + 0x114);
        slot[0x68] = static_cast<uint8_t>(type);
        slot[0x69] = static_cast<uint8_t>(dbl);
        ++krp[6];
        return;
    }
}
// score +2 / +4 of the player's record (then the places update)
extern "C" U3X_FN void u3x_add_score(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts) {
    if (!np || (kr_switches(s) & no_points)) return;
    uint8_t *r = u3h_kr_prec(S, np);
    *reinterpret_cast<int16_t *>(r + 2) += static_cast<int16_t>(pts);
    *reinterpret_cast<int16_t *>(r + 4) += static_cast<int16_t>(pts);
    ++ext_krp()[0];
    game_call<void>(base + 0x7aa560, static_cast<void *>(S));
}
// the team record (S +0x574c, 0x130 each) +2 / +4
extern "C" U3X_FN void u3x_team_score(uintptr_t base, void *s, uint8_t *S, uint32_t team, int32_t pts) {
    if (kr_switches(s) & no_points) return;
    uint8_t *r = S + 0x574c + static_cast<uint64_t>(team & 1) * 0x130;
    *reinterpret_cast<int16_t *>(r + 2) += static_cast<int16_t>(pts);
    *reinterpret_cast<int16_t *>(r + 4) += static_cast<int16_t>(pts);
    ++ext_krp()[1];
    game_call<void>(base + 0x7aa560, static_cast<void *>(S));
}
// cash, XP, rank-up, missions and profile counters for the points (0x7abb70)
extern "C" U3X_FN void u3x_award(uintptr_t base, void *s, uint8_t *S, uint8_t *np, int32_t pts, uint32_t type, uint32_t flag) {
    if (kr_switches(s) & no_points) return;
    const int idx = np ? u3x_score_member(S, u3x_net_player_id(np)) : -1;
    uint32_t *krp = ext_krp();
    ++krp[2];
    krp[3] += static_cast<uint32_t>(pts);
    game_call<void>(base + 0x7abb70, static_cast<void *>(S), static_cast<uint32_t>(idx), static_cast<uint32_t>(static_cast<int16_t>(pts)), type, flag);
}
// mode type 0xf with a mode entry, and its DC 0xea5596b8 values
extern "C" U3X_FN int u3x_kr_34fc04(uintptr_t base) { return u3h_kr_mode_type(base) == 0xf; }
extern "C" U3X_FN int32_t u3x_kr_dc_int(uintptr_t base, uint32_t off) {
    const int32_t *p = dc_value(base, 0xea5596b8u);
    return p ? p[off / 4] : 0;
}
extern "C" U3X_FN int32_t u3x_kr_37c984(uintptr_t base) { return u3x_kr_34fc04(base) ? u3x_kr_dc_int(base, 4) : 3; }
extern "C" U3X_FN int32_t u3x_kr_37c914(uintptr_t base) { return u3x_kr_34fc04(base) ? u3x_kr_dc_int(base, 0xc) : 0; }
extern "C" U3X_FN int32_t u3x_kr_37c9f4(uintptr_t base) { return game_call<int32_t>(base + 0x7a8150); }
// a kill point getter of the game (0x6621a0 + 0x80 k: DC 0xc85e199d values times the booster multiplier)
extern "C" U3X_FN int32_t u3x_kill_points(uintptr_t base, uint32_t fn, uint8_t *np) { return game_call<int32_t>(base + fn, static_cast<void *>(nullptr), np); }
// the double-points event is on for the other team
extern "C" U3X_FN int u3x_double_points(uintptr_t base, const uint8_t *np) {
    return at<int32_t>(base + 0x17dd624) != 0 && at<int32_t>(base + 0x17dd634) != (np ? rd<int32_t>(np, 0x21c) : 0);
}
extern "C" U3X_FN int u3x_kr_30ffa8(uintptr_t base, const uint8_t *np, int b) { return u3h_kr_30feec(base) ? (np ? b : 0) : 1; }
// AI kill points: DC 0xa977dad3 {id, .., points} (default 100) plus the weapon bonus (DC 0x829f99c6 {weapon, bonus}),
// times f_30ffa8 and the difficulty (3: 1.5, 4: 2.0)
extern "C" U3X_FN int32_t u3x_ai_kill_points(uintptr_t base, uint32_t w, uint32_t id, uint8_t *knp, int b) {
    const int32_t *t = game_call<const int32_t *>(base + 0x11f2e30, 0xa977dad3u);
    int32_t pts = 100, extra = 0;
    const int32_t *e = nullptr;
    if (t) {
        const int32_t *a = *reinterpret_cast<int32_t *const *>(t + 2);
        for (int32_t i = 0; i < t[0]; ++i)
            if (a[i * 3] == static_cast<int32_t>(id)) { e = a + i * 3; break; }
    }
    if (!e) return pts;
    pts = e[2];
    const int32_t *t2 = game_call<const int32_t *>(base + 0x11f2e30, 0x829f99c6u);
    if (t2) {
        const uint8_t *a = *reinterpret_cast<const uint8_t *const *>(t2 + 2);
        for (int32_t i = 0; i < t2[0]; ++i)
            if (rd<uint32_t>(a, i * 0x24) == w) { extra = rd<int32_t>(a, i * 0x24 + 4); break; }
    }
    const int m = u3x_kr_30ffa8(base, knp, b);
    const uint8_t d = at<uint8_t>(base + 0x17cb060 + 0x6a48);
    const uint32_t f = d == 3 ? 0x3fc00000u : d == 4 ? 0x40000000u : 0x3f800000u;
    return f_to_int(f_mul(f_from_int(pts + extra), f_mul(f_from_int(m), f)));
}

// ===== v7.170: next batch, part 19 (co-op hunter spectate) =====

extern "C" {
U3H_IMPORT int u3h_cam_fixed(uintptr_t base, void *s, uint8_t *p, int32_t idx);
}

// v4.93 co-op: a hunter (team 1) who has not spawned or died over 5 s ago watches the selected hunter AI while the round
// runs (mgr +0x6808 == 2), cycles it with left / right on PAGE_SPOILER_SELECT, opens that page once until a class is
// picked (+0x60c), then asks the host for the AI (net event 0x5e, once: +0x5fd). The AI list: mgr +0x18 entries of
// 0x18 {handle, id +0x10}, count +0x318. coop[3] runs, [4] page opens, [5] requests sent, [6] cycles, [12] selected
// AI, [13] AI count.
namespace {
constexpr uint32_t spoiler_page = 0xd3a938b7u, pad_right = 0x20u, pad_left = 0x80u;
U3X_RO const uint8_t alloc_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
U3X_RO const uint8_t send_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x49};
U3X_RO const uint8_t handler_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x44, 0x8b, 0x4f, 0x14, 0x41};
U3X_FN uint32_t hunter_count(uintptr_t base) { return at<uint32_t>(base + 0x17cb060 + 0x318); }
U3X_FN int32_t hunter_id(uintptr_t base, uint32_t i) { return at<int32_t>(base + 0x17cb060 + 0x18 + static_cast<uint64_t>(i) * 0x18 + 0x10); }
U3X_FN uint32_t hunter_index(uintptr_t base, int32_t id) {
    const uint32_t n = hunter_count(base);
    uint32_t i = 0;
    for (; i < n && i < 32; ++i)
        if (hunter_id(base, i) == id) break;
    return i;
}
U3X_FN void camera_kill(uint8_t *p) {
    uint8_t *c = handle_object(rd<uint8_t **>(p, 0x388), rd<uint32_t>(p, 0x390));
    if (c) virtual_call<void>(c, 0x158);
}
U3X_FN uint32_t pad_pressed(uintptr_t base, int32_t screen) {
    const uint8_t *g = at<const uint8_t *>(base + 0x16e2fa0);
    return g ? rd<uint32_t>(g, (screen == 1 ? 0x9b8 : 0x7e0) + 0x94) : 0;
}
}  // namespace
// an entity's tag value: entity +0x68 -> {entries {tag, value*} sorted by tag, count}; value {type, .., data* +8}:
// 0xd sid, 1 int, 5 string (its sid)
extern "C" U3X_FN uint32_t u3x_entity_tag(const uint8_t *e, uint32_t tag, uint32_t fallback) {
    const uint8_t *tt = e ? rd<const uint8_t *>(e, 0x68) : nullptr;
    if (!tt) return fallback;
    const uint8_t *data = rd<const uint8_t *>(tt, 0);
    const uint32_t count = rd<uint32_t>(tt, 8);
    if (!data || !count) return fallback;
    uint32_t lo = 0, n = count;
    while (n) {
        const uint32_t h = n >> 1;
        if (rd<uint32_t>(data, (lo + h) * 0x10) < tag) {
            lo += h + 1;
            n -= h + 1;
        } else {
            n = h;
        }
    }
    if (lo >= count || rd<uint32_t>(data, lo * 0x10) != tag) return fallback;
    const int16_t *v = rd<const int16_t *>(data, lo * 0x10 + 8);
    if (!v) return fallback;
    const void *d = rd<const void *>(v, 8);
    if (!d) return fallback;
    if (*v == 0xd || *v == 1) return *static_cast<const uint32_t *>(d);
    if (*v == 5) {
        const char *str = *static_cast<const char *const *>(d);
        return (str && *str) ? u3h_crc_sid(str) : fallback;
    }
    return fallback;
}
extern "C" U3X_FN uint32_t u3x_hunter_count(uintptr_t base) { return hunter_count(base); }
extern "C" U3X_FN int32_t u3x_hunter_id(uintptr_t base, uint32_t i) { return hunter_id(base, i); }
extern "C" U3X_FN uint32_t u3x_hunter_index(uintptr_t base, int32_t id) { return hunter_index(base, id); }
// net event 0x5e {player code, class (+0x608), AI id (+0x600), chosen (+0x5fd)} to the host, then handled here too
extern "C" U3X_FN void u3x_send_spoiler(uintptr_t base, uint32_t code) {
    if (!code_starts(base, 0xd9e990, alloc_prologue) || !code_starts(base, 0x733d90, send_prologue) || !code_starts(base, 0x6d2e80, handler_prologue)) return;
    int32_t tag[2];
    tag[0] = 0x14;
    tag[1] = 0;
    uint8_t *m = game_call<uint8_t *>(base + 0xd9e990, uint64_t{0x28}, static_cast<void *>(tag), uint64_t{8});
    if (!m) return;
    m[9] = 0;
    m[8] = 0x5e;
    m[0xa] = 1;
    m[0xb] = 1;
    m[0xc] = 0;
    m[0xd] = 0;
    *reinterpret_cast<uint64_t *>(m) = base + 0x16d3770;
    *reinterpret_cast<uint32_t *>(m + 0x14) = code;
    *reinterpret_cast<uint32_t *>(m + 0x18) = 0;
    *reinterpret_cast<uint32_t *>(m + 0x1c) = 0;
    m[0x20] = 0;
    for (int i = 0; i < 10; ++i) {
        const uint8_t *q = reinterpret_cast<const uint8_t *>(base + 0x17cb060 + 0x390 + i * 0x9c0);
        if (net_player_id(q) == code) {
            *reinterpret_cast<uint32_t *>(m + 0x18) = rd<uint32_t>(q, 0x608);
            *reinterpret_cast<uint32_t *>(m + 0x1c) = rd<uint32_t>(q, 0x600);
            m[0x20] = q[0x5fd];
            break;
        }
    }
    game_call<void>(base + 0x733d90, base + 0x17cb060, m);
    virtual_call<void>(m, 4 * 8, 0);
    if (--m[0xa] == 0) virtual_call<void>(m, 8);
    ++ext_coop()[5];
}
// a living character within 1.0 of the spawner: the registry PS3 walks has no PS4 counterpart; only the spawner lookup
// is kept (0, as PS3 without one)
extern "C" U3X_FN int u3x_spawner_occupied(uintptr_t base, void *s, int32_t id) {
    game_call<void *>(state_native(s, 58), base + 0x1afcf10, static_cast<uint32_t>(id));
    return 0;
}
extern "C" U3X_FN void u3x_camera_kill(uint8_t *p) { camera_kill(p); }
extern "C" U3X_FN uint32_t u3x_pad_pressed(uintptr_t base, int32_t screen) { return pad_pressed(base, screen); }
extern "C" U3X_FN int u3x_coop_spectate(uintptr_t base, void *s, uint8_t *p, uint8_t *menu) {
    uint32_t *coop = ext_coop();
    int32_t *sel = reinterpret_cast<int32_t *>(p + 0x600);
    const uint32_t n = hunter_count(base);
    const int32_t screen = rd<int32_t>(p, 0x1e8);
    coop[13] = n;
    if (at<int32_t>(base + 0x17cb060 + 0x6808) == 2 && n) {
        ++coop[3];
        if (*sel == 0 || hunter_index(base, *sel) == n) {
            *sel = hunter_id(base, 0);
            camera_kill(p);
        }
        if (!handle_object(rd<uint8_t **>(p, 0x388), rd<uint32_t>(p, 0x390))) u3h_cam_fixed(base, s, p, 0);
        if (u3x_menu_page(menu) == spoiler_page && u3x_menu_open(menu)) {
            const uint32_t b = pad_pressed(base, screen);
            int32_t j = -2;
            if (b & pad_right) j = static_cast<int32_t>(hunter_index(base, *sel)) + 1;
            else if (b & pad_left) j = static_cast<int32_t>(hunter_index(base, *sel)) - 1;
            if (j != -2) {
                if (j < 0) j = static_cast<int32_t>(n) - 1;
                else if (static_cast<uint32_t>(j) >= n) j = 0;
                *sel = hunter_id(base, static_cast<uint32_t>(j));
                ++coop[6];
                u3h_cam_fixed(base, s, p, 0);
            }
        } else if (!p[0x60c] && menu && !u3x_menu_open(menu)) {
            u3x_menu_show(base, menu, spoiler_page);
            *reinterpret_cast<int32_t *>(menu + 0x2694) = screen;
            ++coop[4];
            u3h_cam_fixed(base, s, p, 0);
        } else if (!u3x_menu_open(menu) && !p[0x5fd] && p[0x60c] && !u3x_spawner_occupied(base, s, *sel)) {
            p[0x5fd] = 1;
            u3x_send_spoiler(base, net_player_id(p));
        }
        coop[12] = static_cast<uint32_t>(*sel);
        return 1;
    }
    if (u3x_menu_page(menu) == spoiler_page) {
        u3x_menu_back(base, menu, 0);
        u3x_menu_close(base, menu);
    }
    if (*sel && handle_object(rd<uint8_t **>(p, 0x388), rd<uint32_t>(p, 0x390))) camera_kill(p);
    *sel = 0;
    return 0;
}

// ===== v7.171: next batch, part 20 (the LAN backend at NetInit, per frame and at NetShutdown; party rows) =====

extern "C" {
U3H_IMPORT void *u3h_lnb_setup(uintptr_t base, void *s);
U3H_IMPORT void *u3h_lnb_object(uintptr_t base);
U3H_IMPORT int32_t u3h_gt_open(uintptr_t base);
U3H_IMPORT void u3h_gt_close(uintptr_t base);
U3H_IMPORT void u3h_cm_init_lan(uintptr_t base);
U3H_IMPORT void u3h_cm_shutdown(uintptr_t base);
U3H_IMPORT void *u3h_online_manager(uintptr_t base);
U3H_IMPORT int u3h_onb_lan_flag(void);
U3H_IMPORT void u3h_lnb_frame_parts(uintptr_t base, void *s, int which);
}

// b010: the LAN backend. NetInit: the backend object is (re)built (the C part: it knows the object's layout) and
// registered as the session manager, its Init runs (a failure fails NetInit unless U3MP_LAN_SOLO), the party session
// is initialised, both sessions attached, the party set to LAN state, the game sockets opened, the connection table
// set up. eb[64..66] the object, NetInits, Init result; eb[85] party updates, eb[90] shutdowns, eb[94] last search,
// eb[230] online re-attaches.
namespace {
bool lan_solo() { return !(ext_eb()[0] & 2); }
uint8_t *match_session(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17d6420); }
U3X_FN int64_t object_call(void *o, int slot) { return reinterpret_cast<int64_t (*)(void *)>((*static_cast<uintptr_t **>(o))[slot])(o); }
U3X_FN int64_t object_call2(void *o, int slot, void *a) { return reinterpret_cast<int64_t (*)(void *, void *)>((*static_cast<uintptr_t **>(o))[slot])(o, a); }
U3X_FN int32_t local_clock(uintptr_t base) { return at<int32_t>(base + 0x17b4ba4); }
}  // namespace
extern "C" U3X_FN void u3x_lan_netinit(uintptr_t base, void *s) {
    void *be = u3h_lnb_setup(base, s);
    const int64_t r = object_call(be, 0);
    ext_eb()[66] = static_cast<uint32_t>(r);
    if (r != 0 && !lan_solo()) return;
    uint8_t *ms = match_session(base), *ps = party_session(base);
    game_call<int64_t>(base + 0x7a2360, reinterpret_cast<uintptr_t>(ps));   // party session Init, then the party fields
    ps[0x1334] = 0;
    ps[0x1375] = 0;
    *reinterpret_cast<uint64_t *>(ps + 0x1340) = 0;
    *reinterpret_cast<uint32_t *>(ps + 0x1338) = 0;
    *reinterpret_cast<uint32_t *>(ps + 0x1370) = 0;
    ps[0x1348] = 0;
    ps[0x1374] = 0;
    object_call2(be, 3, ms);   // AttachSession: match, then party
    object_call2(be, 3, ps);
    if (at<uint8_t>(base + 0x17d5c60)) {
        if (!rd<uint32_t>(ps, 0x1338)) *reinterpret_cast<uint32_t *>(ps + 0x1338) = 1;
        ps[0x1374] = 0;
    }
    reinterpret_cast<int64_t (*)(void *, void *, int)>((*static_cast<uintptr_t **>(be))[8])(be, ps, 1);
    if (u3h_gt_open(base) != 0 && !lan_solo()) return;   // the game UDP sockets 3233 / 3232
    u3h_cm_init_lan(base);
}
// the LAN parts of NetUpdate each frame (multiplayer only): net frame, net time, the lobby scene and the HUD fixes (in
// the C part), the manager's Update (online -1 = lost), the party session update, the master channel, the online
// match session re-attach and the NAT probe
extern "C" U3X_FN void u3x_lan_tick(uintptr_t base, void *s) {
    if (!at<uint8_t>(base + 0x17d5c61)) return;
    u3h_lnb_frame_parts(base, s, 0);   // gt_frame, gt_net_time_block, the lobby scene, e003, f005, b042, b047, e001
    void *be = u3h_lnb_object(base);
    void *mgr = at<void *>(base + 0x17dd768);
    void *online = u3h_online_manager(base);
    if (!mgr || (mgr != be && mgr != online)) return;
    if (object_call(mgr, 1) != 0) at<uint8_t>(base + 0x17d5c80) = 1;
    if (at<uintptr_t>(base + 0x17dd610)) {
        ++ext_eb()[85];
        game_call<int64_t>(base + 0x77d270, static_cast<void *>(party_session(base)));
    }
    u3h_lnb_frame_parts(base, s, 1);   // the master channel
    uint8_t *ms = match_session(base), *ps = party_session(base);
    if (online && mgr == online && !u3h_onb_lan_flag() && rd<void *>(ps, 8) && !rd<void *>(ms, 8)) {
        object_call2(online, 3, ms);
        ++ext_eb()[230];
    }
    u3h_lnb_frame_parts(base, s, 2);   // the NAT probe
}
extern "C" U3X_FN void u3x_lan_shutdown(uintptr_t base) {
    void *be = u3h_lnb_object(base), *online = u3h_online_manager(base);
    if (!be && !online) return;
    ++ext_eb()[90];
    game_call<int64_t>(base + 0x7a2470, static_cast<void *>(party_session(base)));
    game_call<int64_t>(base + 0x7a2470, static_cast<void *>(match_session(base)));
    void *mgr = at<void *>(base + 0x17dd768);
    if (mgr && (mgr == be || mgr == online)) object_call(mgr, 2);
    at<void *>(base + 0x17dd768) = nullptr;
    u3h_gt_close(base);
    u3h_cm_shutdown(base);
}
// Find a Party: the backend's Search; the LAN Parties page searches again every 5 s (eb[0] bit 2 turns that off)
extern "C" U3X_FN void u3x_lan_find_party(uintptr_t base) {
    void *be = u3h_lnb_object(base);
    if (be) object_call(be, 5);
    ext_eb()[94] = static_cast<uint32_t>(local_clock(base));
}
extern "C" U3X_FN void u3x_lan_refresh(uintptr_t base) {
    uint32_t *eb = ext_eb();
    if (eb[0] & 4) return;
    void *be = u3h_lnb_object(base);
    if (!be || u3h_active_page(base) != 0xbb3595bcu) return;
    if (static_cast<uint32_t>(local_clock(base) - static_cast<int32_t>(eb[94])) >= 1500u) {
        object_call(be, 5);
        eb[94] = static_cast<uint32_t>(local_clock(base));
    }
}
// after a Start Game session Init cleared the match session's backend: put it back
extern "C" U3X_FN void u3x_lan_reattach(uintptr_t base) {
    void *be = u3h_lnb_object(base);
    if (be) *reinterpret_cast<void **>(match_session(base) + 8) = be;
}
// the party's size: 1 (2 in split-screen) without a party, else its players
extern "C" U3X_FN int u3x_lan_party_size(uintptr_t base) {
    const uint8_t *ps = party_session(base);
    if (!rd<uint64_t>(ps, 0x18)) return 1 + (at<uint8_t>(base + 0x17d7b06) || at<int32_t>(base + 0x17f2fc0) == 3);
    return u3x_party_players(ps);
}
// LIST_PLAYERS rows after our own: the party's other members
extern "C" U3X_FN const char *u3x_lan_row_name(uintptr_t base, int row) {
    uint8_t *ps = party_session(base);
    int r = 0;
    if (!rd<uint64_t>(ps, 0x18)) return nullptr;
    for (int i = 0; i < 10; ++i) {
        const uint8_t *m = ps + 0x680 + i * 0x120;
        if (!m[0x25] || rd<uint32_t>(m, 0x2c) == rd<uint32_t>(ps, 0x12e4)) continue;
        if (++r == row) return reinterpret_cast<const char *>(m);
    }
    return nullptr;
}
extern "C" U3X_FN int u3x_lan_rows(uintptr_t base) {
    const uint8_t *ps = party_session(base);
    if (!rd<uint64_t>(ps, 0x18)) return 1;
    int n = 0;
    for (int i = 0; i < 10; ++i)
        if (ps[0x680 + i * 0x120 + 0x25]) ++n;
    return n > 0 ? n : 1;
}

// ===== v7.172: next batch, part 21 (level wants at NetInit and NetShutdown) =====

extern "C" {
U3H_IMPORT int u3h_stage_unwant(uintptr_t base, uint32_t sid);
}

// v5.03 / v5.05: the level manager (0x1afcf10) keeps want lists of 0x18-byte entries {hash, sid +8, u16 +0xc, flags
// +0xe}: levels (+0x28 count, +0x30 entries) and actors (+0x38, +0x40), plus the pending actor list (+0x48 count,
// +0x4c capacity, +0x50 entries). At NetInit PS3 unwants the boot list (the single-player world levels, DC 0x1020810b
// entries {name, ..} of 0x10 bytes) and the title scene; at NetShutdown it wants the boot list again. ExtState lob
// (+0x1ff0): [0] NetInits, [1] title unwants, [2] boot list size, [3..10] its first sids, [11] wanted bits, [15] the
// title levels' states; lob2 (+0x2030): [0] boot unwants, [1] boot wants, [2] switches (bit 0 keep the boot list, bit 1
// unwant proto-sp), [3] proto-sp unwants, [4] boot list unwanted.
namespace {
U3X_RO const uint32_t title_levels[2] = {0xeef9ed7bu, 0xa69a074du};   // title-screen, sky-desert-teaser
uint32_t *ext_lob() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x1ff0); }
uint32_t *ext_lob2() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2030); }
}  // namespace
extern "C" U3X_FN uint8_t *u3x_level_want_entry(uintptr_t base, uint32_t sid) {
    const uint32_t count = at<uint32_t>(base + 0x1afcf38);
    uint8_t *a = at<uint8_t *>(base + 0x1afcf40);
    if (!aligned_pointer(a) || count > 4096) return nullptr;
    for (uint32_t i = 0; i < count; ++i)
        if (rd<uint32_t>(a, i * 0x18 + 8) == sid) return a + i * 0x18;
    return nullptr;
}
// a level's state byte (8 = loaded), 0xff without the level
extern "C" U3X_FN uint32_t u3x_level_state(uintptr_t base, void *s, uint32_t sid) {
    const uintptr_t find = state_native(s, 42);
    const uint8_t *l = find ? reinterpret_cast<const uint8_t *(*)(uintptr_t, uint32_t)>(find)(base + 0x1afcf10, sid) : nullptr;
    return aligned_pointer(l) ? (rd<uint32_t>(l, 0x30) & 0xff) : 0xffu;
}
// want an actor package without waiting for it (an entry {hash, sid, 0, flags 1} on the pending actor list): 1 when
// either list already has it, 2 when added, 0 when it cannot be
extern "C" U3X_FN int u3x_actor_want(uintptr_t base, void *s, const char *name) {
    const uintptr_t hash = state_native(s, 40);
    if (!hash) return 0;
    const uint32_t sid = u3h_crc_sid(name);
    for (int li = 0; li < 2; ++li) {
        const uint32_t count = at<uint32_t>(base + 0x1afcf10 + (li ? 0x38 : 0x28));
        const uint8_t *a = at<const uint8_t *>(base + 0x1afcf10 + (li ? 0x40 : 0x30));
        if (!aligned_pointer(a) || count > 4096) return 0;
        for (uint32_t i = 0; i < count; ++i)
            if (rd<uint32_t>(a, i * 0x18 + 8) == sid) return 1;
    }
    uint32_t &count = at<uint32_t>(base + 0x1afcf48);
    const uint32_t capacity = at<uint32_t>(base + 0x1afcf4c);
    uint8_t *a = at<uint8_t *>(base + 0x1afcf50);
    if (!aligned_pointer(a) || count >= capacity || capacity > 4096) return 0;
    uint8_t *e = a + count * 0x18;
    for (int k = 0; k < 0x18; ++k) e[k] = 0;
    *reinterpret_cast<uint64_t *>(e) = reinterpret_cast<uint64_t (*)(const char *)>(hash)(name);
    *reinterpret_cast<uint32_t *>(e + 8) = sid;
    e[0xe] = 1;
    ++count;
    return 2;
}
// the title scene out of the want list (safe to repeat)
extern "C" U3X_FN void u3x_title_unwant(uintptr_t base, void *s) {
    uint32_t *lob = ext_lob();
    for (int k = 0; k < 2; ++k)
        if (u3x_level_want_entry(base, title_levels[k])) lob[1] += static_cast<uint32_t>(u3h_stage_unwant(base, title_levels[k]));
    lob[15] = u3x_level_state(base, s, title_levels[0]) | u3x_level_state(base, s, title_levels[1]) << 8;
}
namespace {
U3X_FN bool name_ok(const char *n) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(n);
    return v >= 0x10000u && v < (1ull << 47);
}
}  // namespace
// NetInit: the boot list out (unless lob2[2] bit 0), proto-sp out (only with lob2[2] bit 1), the title scene out
extern "C" U3X_FN void u3x_net_init_levels(uintptr_t base, void *s) {
    uint32_t *lob = ext_lob(), *lob2 = ext_lob2();
    ++lob[0];
    const uintptr_t lookup = state_native(s, 13);
    const int32_t *t = lookup ? reinterpret_cast<const int32_t *(*)(uint32_t)>(lookup)(0x1020810bu) : nullptr;
    if (!aligned_pointer(t) || t[0] < 0 || t[0] > 256) {
        lob[2] = 0xffffffffu;
    } else {
        lob[2] = static_cast<uint32_t>(t[0]);
        lob[11] = 0;
        const uint8_t *e = *reinterpret_cast<const uint8_t *const *>(t + 2);
        for (int32_t i = 0; aligned_pointer(e) && i < t[0] && i < 32; ++i) {
            const char *n = rd<const char *>(e, i * 0x10);
            if (!name_ok(n)) continue;
            const uint32_t sid = u3h_crc_sid(n);
            if (i < 8) lob[3 + i] = sid;
            if (u3x_level_want_entry(base, sid)) lob[11] |= 1u << i;
            if (!(lob2[2] & 1)) {
                const int r = u3h_stage_unwant(base, sid);
                lob2[0] += static_cast<uint32_t>(r);
                if (r) lob2[4] = 1;
            }
        }
    }
    if (lob2[2] & 2) lob2[3] += static_cast<uint32_t>(u3h_stage_unwant(base, 0xe0fdf8f6u));
    u3x_title_unwant(base, s);
}
// NetShutdown: the boot list wanted again as boot does ({hash, sid, 4, flags 3}) when NetInit had unwanted it
extern "C" U3X_FN void u3x_net_shutdown_levels(uintptr_t base, void *s) {
    uint32_t *lob2 = ext_lob2();
    if (!lob2[4]) return;
    lob2[4] = 0;
    const uintptr_t lookup = state_native(s, 13), hash = state_native(s, 40), request = state_native(s, 41);
    const int32_t *t = lookup ? reinterpret_cast<const int32_t *(*)(uint32_t)>(lookup)(0x1020810bu) : nullptr;
    if (!hash || !request || !aligned_pointer(t) || t[0] < 0 || t[0] > 256) return;
    const uint8_t *e = *reinterpret_cast<const uint8_t *const *>(t + 2);
    for (int32_t i = 0; aligned_pointer(e) && i < t[0] && i < 32; ++i) {
        const char *n = rd<const char *>(e, i * 0x10);
        if (!name_ok(n)) continue;
        const uint32_t sid = u3h_crc_sid(n);
        if (u3x_level_want_entry(base, sid)) continue;
        uint8_t req[0x18];
        for (uint8_t &b : req) b = 0;
        *reinterpret_cast<uint64_t *>(req) = reinterpret_cast<uint64_t (*)(const char *)>(hash)(n);
        *reinterpret_cast<uint32_t *>(req + 8) = sid;
        *reinterpret_cast<uint16_t *>(req + 0xc) = 4;
        req[0xe] = 3;
        reinterpret_cast<void (*)(uintptr_t, void *)>(request)(base + 0x1afcf10, req);
        ++lob2[1];
    }
}

// ===== v7.173: next batch, part 22 (hat-stat icons on the player card) =====

// c010: PS3 1.19 shows the hat's two stat icons after the booster / kickback icons on the in-match player card. The
// hat's stats come from DC 0xb8a3581b (rows of 16 {item id, count, {stat, multiplier} pairs}) for the item of the
// player's look; each stat's icon from DC 0x53ed596a (0x28-byte entries {stat, "+" icon name +8, "-" icon name +0x18}),
// "+" or "-" by whether the multiplier helps (for some stats a smaller value helps). A player with hat stats off shows
// none. ec[0] bit 13 turns it off; ec[105] icon count, ec[106..109] the saved rect, ec[110] cards, [111] icons drawn,
// [112] last hat icon count.
namespace {
U3X_RO const uint8_t look_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xfb, 0x40, 0x0f};
U3X_RO const uint8_t item_prologue[12] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54};
}  // namespace
// fn(a0..a5, two stack words) with a float in xmm0
extern "C" U3X_FN uint64_t u3x_call8(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                                     uint64_t s0, uint64_t s1) {
    uint64_t st[2];
    st[0] = s0;
    st[1] = s1;
    register uint64_t rdi asm("rdi") = a0;
    register uint64_t rsi asm("rsi") = a1;
    register uint64_t rdx asm("rdx") = a2;
    register uint64_t rcx asm("rcx") = a3;
    register uint64_t r8 asm("r8") = a4;
    register uint64_t r9 asm("r9") = a5;
    register uint64_t rax asm("rax") = fn;
    register uint64_t r10 asm("r10") = reinterpret_cast<uint64_t>(st);
    register uint64_t r11 asm("r11") = f0;
    asm volatile("movd %%r11d,%%xmm0\n\tpush %%rbp\n\tmov %%rsp,%%rbp\n\tand $-16,%%rsp\n\tsub $16,%%rsp\n\tmov (%%r10),%%r11\n\t"
                 "mov %%r11,(%%rsp)\n\tmov 8(%%r10),%%r11\n\tmov %%r11,8(%%rsp)\n\tcall *%%rax\n\tmov %%rbp,%%rsp\n\tpop %%rbp"
                 : "+a"(rax), "+D"(rdi), "+S"(rsi), "+d"(rdx), "+c"(rcx), "+r"(r8), "+r"(r9), "+r"(r10), "+r"(r11)
                 :
                 : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
                   "memory", "cc");
    return rax;
}
// the icon texture for (stat, multiplier)
extern "C" U3X_FN uint8_t *u3x_stat_icon(uintptr_t base, void *s, uint32_t stat, uint32_t m) {
    const uint8_t *t = dc_table(s, 0x53ed596au);
    if (!t) return nullptr;
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *e = rd<const uint8_t *>(t, 8), *hit = nullptr;
    for (int32_t i = 0; i < n && e; ++i)
        if (rd<uint32_t>(e, i * 0x28) == stat) hit = e + static_cast<int64_t>(i) * 0x28;   // the last match counts
    if (!hit) return nullptr;
    int plus;
    switch (stat) {
    case 0x7da763c9u: case 0x2d57beb9u: case 0x8b73121au:   // shown as the inverse
        m = f_div(0x3f800000u, m);
        plus = f_less(m, 0x3f800000u) ? 0 : 1;
        break;
    case 0x2df81dfeu: case 0x93a5a7c2u: case 0xbe693215u: case 0x974e6226u: case 0xd0ab3f8bu: plus = 1; break;
    case 0x1d20653cu: case 0x4f6e6534u: case 0x63e03907u: case 0xbf8f5f50u: plus = f_less(m, 0x3f800000u) ? 1 : 0; break;   // lower helps
    default: plus = f_less(m, 0x3f800000u) ? 0 : 1; break;
    }
    const char *nm = rd<const char *>(hit, plus ? 8 : 0x18);
    if (!nm) return nullptr;
    return game_call<uint8_t *>(base + 0x1189430, reinterpret_cast<uint8_t *>(base + 0x1b31128), nm);
}
// the player's hat stat icon i (0 / 1)
extern "C" U3X_FN uint8_t *u3x_hat_icon(uintptr_t base, void *s, uint8_t *np, int32_t i) {
    if (!np || !code_starts(base, 0x784b20, look_prologue) || !code_starts(base, 0x9952a0, item_prologue)) return nullptr;
    const uint8_t *look = game_call<const uint8_t *>(base + 0x784b20, np, 0u);
    const uint8_t *t = dc_table(s, 0xb8a3581bu);
    const int k = player_slot(base, np);
    if ((k >= 0 && reinterpret_cast<const PlayerInfo119 *>(u3h_np119())[k].hat_off) || !t || !look) return nullptr;
    const uint32_t *item = game_call<const uint32_t *>(base + 0x9952a0, uint64_t{0}, uint32_t{look[0]}, uint32_t{look[1]}, 0u, uint32_t{look[2]});
    if (!item) return nullptr;
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *rows = rd<const uint8_t *>(t, 8);
    if (!rows) return nullptr;
    for (int32_t r = 0; r < n; ++r) {
        const uint8_t *row = rows + static_cast<int64_t>(r) * 16;
        if (rd<uint32_t>(row, 0) != *item) continue;
        if (i < rd<int32_t>(row, 4)) {
            const uint8_t *pairs = rd<const uint8_t *>(row, 8);
            if (!pairs) return nullptr;
            return u3x_stat_icon(base, s, rd<uint32_t>(pairs, i * 8), rd<uint32_t>(pairs, i * 8 + 4));
        }
        return nullptr;
    }
    return nullptr;
}
// kind 122: the card's icon count (PS4's own sum + the hat icons)
extern "C" U3X_FN void u3x_hat_icon_count(uintptr_t base, void *s, const uint64_t *regs) {
    uint32_t *ec = ext_ec();
    const uint8_t *rbp = reinterpret_cast<const uint8_t *>(regs[14]);
    uint8_t *np = rd<uint8_t *>(rbp - 0x12b0, 0);
    uint32_t n = static_cast<uint32_t>(regs[13]) + (rd<uint64_t>(rbp - 0x11e0, 0) != 0);
    if (!(ec[0] & 8192)) {
        const uint32_t hats = (u3x_hat_icon(base, s, np, 0) != nullptr) + (u3x_hat_icon(base, s, np, 1) != nullptr);
        n += hats;
        ++ec[110];
        ec[112] = hats;
    }
    ec[105] = n;
}
// kind 123: after the kickback icon, the hat icons in the next slots (laid out by 0x640ac0, drawn by 0x63d6f0)
extern "C" U3X_FN void u3x_hat_icon_draw(uintptr_t base, void *s, const uint64_t *regs) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 8192) return;
    uint8_t *rbp = reinterpret_cast<uint8_t *>(regs[14]);
    uint8_t *np = rd<uint8_t *>(rbp - 0x12b0, 0), *ctx = rd<uint8_t *>(rbp - 0x11f8, 0);
    uint32_t x = ec[106];
    const uint32_t y = ec[107], w = ec[108], h = ec[109];
    if (rd<uint64_t>(rbp - 0x11e0, 0)) x = f_add(x, f_add(w, 0x40800000u));   // the kickback icon took this slot
    for (int32_t i = 0; i < 2; ++i) {
        uint8_t *v = u3x_hat_icon(base, s, np, i);
        if (!v) continue;
        alignas(16) uint32_t item[12];
        for (uint32_t &q : item) q = 0;
        item[0] = 2;
        *reinterpret_cast<uint8_t **>(item + 2) = v;
        item[7] = 0xffffffffu;
        item[9] = 0xffffffffu;
        item[10] = 0x3f800000u;
        alignas(16) uint32_t rc[4];
        rc[0] = f_add(x, 0x40000000u);
        rc[1] = f_add(y, 0x40000000u);
        rc[2] = f_add(w, 0xc0800000u);
        rc[3] = f_add(h, 0xc0800000u);
        alignas(16) uint32_t size[2];
        size[0] = rc[2];
        size[1] = rc[3];
        alignas(16) uint32_t zero[4];
        for (uint32_t &q : zero) q = 0;
        u3x_call8(base + 0x640ac0, rd<uint32_t>(rbp - 0x450, 0), reinterpret_cast<uint64_t>(rbp - 0x420), reinterpret_cast<uint64_t>(item), 0,
                  reinterpret_cast<uint64_t>(size), reinterpret_cast<uint64_t>(zero), 0xffffffffu, 0, 0);
        u3x_call8(base + 0x63d6f0, 0, base + 0x17bd090, reinterpret_cast<uint64_t>(ctx), reinterpret_cast<uint64_t>(rbp - 0x450),
                  reinterpret_cast<uint64_t>(rbp - 0x420), reinterpret_cast<uint64_t>(rc), 0, 0, 0xffffffffu);
        ++ec[111];
        x = f_add(x, f_add(w, 0x40800000u));
    }
}

// ===== v7.174: next batch, part 23 (mission-complete HUD message, film render defaults) =====

// v5.23: PS3 1.19 shows "mission complete" on the HUD (message queue of the screen's menu: 4 slots, text +0x26a8 +
// i * 0x80, params +0x28a8 + i * 0x30, first slot clock +0x26a4) with the reward text and icon. ExtState +0x2da0 (ms):
// [0] missions, [1] cash messages, [2] reward messages, [3] completions, [4] last id, [5] cash, [6] already done, [7]
// full queue / too long.
namespace {
uint32_t *ext_ms() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2da0); }
U3X_RO const char tag_a[] = "[A]", tag_b[] = "[B]", tag_text[] = "[TEXT]";
U3X_FN int text_length(const char *a) {
    int n = 0;
    while (a[n]) ++n;
    return n;
}
U3X_FN int32_t icon_in(const uint8_t *t, uint32_t id) {
    if (!t || !rd<uint16_t>(t, 2)) return -1;
    const uint8_t *e = rd<const uint8_t *>(t, 8);
    if (!e) return -1;
    for (int i = 0; i < rd<uint16_t>(t, 2); ++i)
        if (rd<uint32_t>(e, i * 0x28 + 8) == id) return i;
    return -1;
}
}  // namespace
// a localised text; the empty text when missing
extern "C" U3X_FN const char *u3x_ms_text(uintptr_t base, uint32_t id) {
    const char *t = game_call<const char *>(base + 0xf32ae0, reinterpret_cast<void *>(base + 0x17026b8), id);
    return t ? t : at<const char *>(base + 0x17026b8 + 0x10);
}
extern "C" U3X_FN int u3x_ms_len(const char *a) { return text_length(a); }
// replace the first tag in buf (size n) with rep
extern "C" U3X_FN void u3x_ms_replace(char *buf, int n, const char *tag, const char *rep) {
    const int tl = text_length(tag), bl = text_length(buf), rl = text_length(rep);
    for (int i = 0; i + tl <= bl; ++i) {
        int k = 0;
        while (k < tl && buf[i + k] == tag[k]) ++k;
        if (k < tl) continue;
        char rest[0x100];
        int o = 0;
        for (int j = i + tl; j <= bl && o < static_cast<int>(sizeof rest) - 1; ++j) rest[o++] = buf[j];
        rest[o] = 0;
        o = i;
        for (int j = 0; j < rl && o < n - 1; ++j) buf[o++] = rep[j];
        for (int j = 0; rest[j] && o < n - 1; ++j) buf[o++] = rest[j];
        buf[o] = 0;
        return;
    }
}
extern "C" U3X_FN void u3x_ms_itoa(char *b, int32_t v) {
    char t[12];
    int n = 0;
    uint32_t u = v < 0 ? 0u - static_cast<uint32_t>(v) : static_cast<uint32_t>(v);
    do {
        t[n++] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u);
    int o = 0;
    if (v < 0) b[o++] = '-';
    while (n) b[o++] = t[--n];
    b[o] = 0;
}
extern "C" U3X_FN int32_t u3x_ms_icon_find(const uint8_t *t, uint32_t id) { return icon_in(t, id); }
// an icon {id, table (1 type-3 / 0 type-2), index}
extern "C" U3X_FN void u3x_ms_icon(uintptr_t base, uint32_t id, uint32_t *ic) {
    const uint8_t *tab = at<const uint8_t *>(base + 0x1b24bc8);
    ic[0] = id;
    ic[1] = 1;
    ic[2] = 0xffffffffu;
    if (!tab) return;
    int32_t i = icon_in(rd<const uint8_t *>(tab, 0x28), id);
    if (i < 0) {
        ic[1] = 0;
        i = icon_in(rd<const uint8_t *>(tab, 0x20), id);
    }
    ic[2] = static_cast<uint32_t>(i);
}
extern "C" U3X_FN int u3x_ms_hud(uintptr_t base, int user, const char *txt, uint32_t type, int32_t level, const uint32_t *ic) {
    uint8_t *m = reinterpret_cast<uint8_t *>(base + (user ? 0x17bfa10 : 0x17bd080));
    const int n = text_length(txt);
    if (n >= 0x80) { ++ext_ms()[7]; return 0; }
    for (int i = 0; i < 4; ++i) {
        char *d = reinterpret_cast<char *>(m + 0x26a8 + i * 0x80);
        if (*d) continue;
        for (int k = 0; k <= n; ++k) d[k] = txt[k];
        uint8_t *p = m + 0x28a8 + i * 0x30;
        *reinterpret_cast<uint32_t *>(p) = type;
        *reinterpret_cast<uint64_t *>(p + 8) = 0;
        *reinterpret_cast<int32_t *>(p + 0x10) = level;
        *reinterpret_cast<uint32_t *>(p + 0x14) = ic[0];
        *reinterpret_cast<uint32_t *>(p + 0x18) = ic[1];
        *reinterpret_cast<uint32_t *>(p + 0x1c) = ic[2];
        *reinterpret_cast<uint32_t *>(p + 0x20) = 0;
        *reinterpret_cast<uint32_t *>(p + 0x24) = 0xffffffffu;
        *reinterpret_cast<uint32_t *>(p + 0x28) = 0x3f800000u;
        if (i == 0) *reinterpret_cast<uint32_t *>(m + 0x26a4) = f_div(f_from_int(at<int32_t>(base + 0x17b4a90 + 0x114)), 0x43960000u);   // clock / 300
        return 1;
    }
    ++ext_ms()[7];
    return 0;
}
// a mission finished (PS3 f_34b91c): the current mission is cleared when it was this one; a cash mission (id < 0x100)
// shows its text with the cash and pays it, a reward mission shows "[A] / [B] [TEXT]" with the reward's icon (once);
// then the mission is marked done in the profile (ids < 0x400)
extern "C" U3X_FN void u3x_mission_done(uintptr_t base, const uint8_t *trk, const int32_t *def) {
    uint32_t *ms = ext_ms();
    const uint32_t id = static_cast<uint32_t>(def[0]);
    const int32_t user = rd<int32_t>(trk, 0x1004);
    ++ms[0];
    ms[4] = id;
    if (user < 0 || user > 1) return;
    uint8_t *prof = at<uint8_t *>(base + 0x17dd610);
    if (!prof) return;
    uint8_t *data = prof + static_cast<uint64_t>(user) * 0x7b00 + 0x248;
    const bool done_before = id < 0x400 && (data[0xc84 + (id >> 3)] >> (id & 7) & 1);
    if (!done_before && rd<uint32_t>(data, 0x161c) == id) {
        *reinterpret_cast<uint32_t *>(data + 0x161c) = 0;
        *reinterpret_cast<uint32_t *>(data + 0x1620) = 0;
        *reinterpret_cast<uint32_t *>(data + 0xd04) = 0;
    }
    char out[0x80];
    if (id < 0x100) {
        char cash[0x20];
        game_call<void>(base + 0xf32080, cash, 0x20, def[9]);
        game_call<void>(base + 0xf32bd0, reinterpret_cast<void *>(base + 0x17026b8), out, 0x80, 0xc423e4f5u, u3x_ms_text(base, static_cast<uint32_t>(def[1])),
                        static_cast<const char *>(cash), static_cast<const char *>(nullptr));
        uint32_t ic[3];
        ic[0] = 0;
        ic[1] = 0;
        ic[2] = 0xffffffffu;
        if (u3x_ms_hud(base, user, out, 0, 0, ic)) ++ms[1];
        game_call<void>(base + 0x660fb0, static_cast<void *>(nullptr), user, def[9]);
        ms[5] += static_cast<uint32_t>(def[9]);
    } else {
        const uint8_t *b = at<uint32_t>(base + 0x17c3b40 + 0x2c)
                               ? game_call<const uint8_t *>(base + 0x687d30, reinterpret_cast<void *>(base + 0x17c3b40), static_cast<uint32_t>(def[0xd]))
                               : nullptr;
        if (b) {
            if (id < 0x400 && (data[0xc84 + (id >> 3)] >> (id & 7) & 1)) { ++ms[6]; return; }
            const char *f = u3x_ms_text(base, 0x065ba916u);
            int k = 0;
            while (f[k] && k < 0x7f) { out[k] = f[k]; ++k; }
            out[k] = 0;
            char a[12], z[2];
            z[0] = '0';
            z[1] = 0;
            u3x_ms_itoa(a, def[7] + 2);
            u3x_ms_replace(out, 0x80, tag_a, a);
            u3x_ms_replace(out, 0x80, tag_b, z);
            u3x_ms_replace(out, 0x80, tag_text, u3x_ms_text(base, rd<uint32_t>(b, 8)));
            uint32_t ic[3];
            u3x_ms_icon(base, rd<uint32_t>(b, 0x28), ic);
            if (u3x_ms_hud(base, user, out, 0x2e, def[7] + 1, ic)) ++ms[2];
        }
        if (id >= 0x400) return;
    }
    data[0xc84 + (id >> 3)] |= static_cast<uint8_t>(1u << (id & 7));
    ++ms[3];
}

// b003: the film render settings' defaults from the current render variables (the settings struct *(0x16e2fb8)),
// into the film writer (+0x1e94..), and the copy kept at +0x1f14
extern "C" U3X_FN void u3x_render_defaults(uintptr_t base) {
    uint8_t *w = reinterpret_cast<uint8_t *>(base + 0x17c3ba8);
    const uint8_t *r = at<const uint8_t *>(base + 0x16e2fb8);
    *reinterpret_cast<uint32_t *>(w + 0x1fa0) = rd<uint32_t>(r, 0x0);
    *reinterpret_cast<uint32_t *>(w + 0x1fa4) = rd<uint32_t>(r, 0x4);
    *reinterpret_cast<uint32_t *>(w + 0x1fa8) = rd<uint32_t>(r, 0x8);
    *reinterpret_cast<uint32_t *>(w + 0x1ea8) = rd<uint32_t>(r, 0xc);
    *reinterpret_cast<uint32_t *>(w + 0x1eac) = 0;
    *reinterpret_cast<uint32_t *>(w + 0x1eb0) = 0;
    *reinterpret_cast<uint32_t *>(w + 0x1eb4) = 0;
    *reinterpret_cast<uint32_t *>(w + 0x1e94) = rd<uint32_t>(r, 0x898);   // saturation
    *reinterpret_cast<uint32_t *>(w + 0x1eb8) = rd<uint32_t>(r, 0x880);   // tint multiply
    *reinterpret_cast<uint32_t *>(w + 0x1ebc) = rd<uint32_t>(r, 0x884);
    *reinterpret_cast<uint32_t *>(w + 0x1ec0) = rd<uint32_t>(r, 0x888);
    *reinterpret_cast<uint32_t *>(w + 0x1e98) = 0;
    *reinterpret_cast<uint32_t *>(w + 0x1f94) = rd<uint32_t>(r, 0x230);
    *reinterpret_cast<uint32_t *>(w + 0x1f98) = rd<uint32_t>(r, 0x234);
    *reinterpret_cast<uint32_t *>(w + 0x1f9c) = rd<uint32_t>(r, 0x238);
    *reinterpret_cast<uint32_t *>(w + 0x1ec4) = 0x3f800000u;
    *reinterpret_cast<uint32_t *>(w + 0x1ea4) = 0;
    *reinterpret_cast<uint32_t *>(w + 0x1e9c) = 0;
    *reinterpret_cast<uint32_t *>(w + 0x1ea0) = 0;
    *reinterpret_cast<uint32_t *>(w + 0x1ec8) = rd<uint32_t>(r, 0xba4);
    *reinterpret_cast<uint32_t *>(w + 0x1ecc) = rd<uint32_t>(r, 0x4c);
    *reinterpret_cast<uint32_t *>(w + 0x1ed0) = rd<uint32_t>(r, 0x8c);
    *reinterpret_cast<uint32_t *>(w + 0x1ed4) = rd<uint32_t>(r, 0xcc);
    for (int k = 0; k < 0x80; ++k) w[0x1f14 + k] = w[0x1e94 + k];
}

// ===== v7.175: next batch, part 24 (the anim-state decoder's bit reader and lookups) =====

extern "C" {
U3H_IMPORT int32_t u3h_tab_find(uint8_t *t, uint32_t key);
U3H_IMPORT uint8_t *u3h_find_art_group_name(uintptr_t base, const char *name);
}

// c009 helpers. The bit reader {words, pos, cap, 0, lim, err}: n bits (most significant first in 32-bit words), a read
// past the message length (lim) stops the decode (err); a 0-bit read gives 0 without moving. Its start is PS4's own
// reader struct {buf, pos, cap, 0}, so PS4's frame / vector readers read from it too.
namespace {
struct BitReader { uint32_t *w; uint32_t pos, cap, z; uint32_t lim, err; };
static_assert(sizeof(BitReader) == 0x20 && __builtin_offsetof(BitReader, lim) == 0x14 && __builtin_offsetof(BitReader, err) == 0x18);
// the net anim package names (PS3 1.19 0x10f8100, 6 rows x 8)
U3X_RO const char anim_packages[6][8][24] = {
    {"proto", "anim-proto-multiplayer", "anim-soldier-mp", "anim-proto-shield", "", "", "", ""},
    {"npc-normal-base", "anim-soldier-mp", "anim-proto-multiplayer", "anim-npc-multiplayer", "anim-proto-shield", "", "", ""},
    {"npc-normal-base", "anim-soldier-mp", "anim-proto-shield", "anim-npc-multiplayer", "", "", "", ""},
    {"npc-normal-base", "anim-soldier-mp", "anim-civilian", "anim-npc-multiplayer", "", "", "", ""},
    {"anim-civillian-female", "", "", "", "", "", "", ""},
    {"anim-civillian-child", "", "", "", "", "", "", ""}};
U3X_FN const uint8_t *art_table(const uint8_t *g) {
    const uint8_t *a = g ? rd<const uint8_t *>(g, 0x30) : nullptr;
    return a ? rd<const uint8_t *>(a, 0x10) : nullptr;
}
}  // namespace
extern "C" U3X_FN int u3x_float_equal(uint32_t a, uint32_t b) { return float_equal(a, b); }
extern "C" U3X_FN uint64_t u3x_call_float_arg(uintptr_t fn, uint32_t f0, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t rdi asm("rdi") = a0;
    register uint64_t rsi asm("rsi") = a1;
    register uint64_t rdx asm("rdx") = a2;
    register uint64_t rcx asm("rcx") = a3;
    register uint64_t rax asm("rax") = fn;
    register uint64_t r8 asm("r8") = f0;
    asm volatile("movd %%r8d,%%xmm0\n\tpush %%rbp\n\tmov %%rsp,%%rbp\n\tand $-16,%%rsp\n\tcall *%%rax\n\tmov %%rbp,%%rsp\n\tpop %%rbp"
                 : "+a"(rax), "+D"(rdi), "+S"(rsi), "+d"(rdx), "+c"(rcx), "+r"(r8)
                 :
                 : "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13",
                   "xmm14", "xmm15", "memory", "cc");
    return rax;
}
extern "C" U3X_FN void u3x_bits_init(void *r_, uint8_t *buf, uint32_t nbytes) {
    BitReader *r = static_cast<BitReader *>(r_);
    r->w = reinterpret_cast<uint32_t *>(buf);
    r->pos = 0;
    r->cap = 300 * 8;
    r->z = 0;
    r->lim = nbytes * 8;
    r->err = 0;
}
extern "C" U3X_FN uint32_t u3x_bits_read(void *r_, uint32_t n) {
    BitReader *r = static_cast<BitReader *>(r_);
    const uint32_t p = r->pos;
    if (!n) return 0;
    if (r->err || n > 32 || p + n > r->lim) {
        r->err = 1;
        return 0;
    }
    const uint32_t sh = p & 31, room = 32 - sh;
    const uint32_t *w = r->w + (p >> 5);
    uint32_t v = static_cast<uint32_t>(static_cast<uint64_t>(w[0]) << sh) >> (32 - n);
    if (room < n) v |= w[1] >> (32 - (n - room));
    r->pos = p + n;
    return v;
}
// PS4's frame reader (0x7bf430) and vector reader (0x1274970) on the same stream
extern "C" U3X_FN void u3x_bits_frame(uintptr_t base, void *r_, uint8_t *f) {
    BitReader *r = static_cast<BitReader *>(r_);
    if (r->err) return;
    game_call<void>(base + 0x7bf430, r_, f);
    if (r->pos > r->lim) r->err = 1;
}
extern "C" U3X_FN void u3x_bits_vector(uintptr_t base, void *r_, uint8_t *v) {
    BitReader *r = static_cast<BitReader *>(r_);
    if (r->err) return;
    game_call<void>(base + 0x1274970, r_, v);
    if (r->pos > r->lim) r->err = 1;
}
// a fresh BoundFrame {0x7fcccccc x2, 0, 0, locator {0, (0,0,0,1)}, locator {0, (0,0,0,1)}}
extern "C" U3X_FN void u3x_frame_fresh(uint8_t *f) { u3x_frame_identity(f); }
// the art group with that sid in any loaded package (open addressing from sid % n, then from 0; an empty key ends)
extern "C" U3X_FN uint8_t *u3x_art_group_sid(uintptr_t base, uint32_t sid) {
    const uint8_t *mgr = at<const uint8_t *>(base + 0x1b24b80 + 0x38);
    if (!sid || !mgr) return nullptr;
    const uint8_t *slots = rd<const uint8_t *>(mgr, 0x38);
    if (!slots) return nullptr;
    for (uint32_t i = 0; i < 0x15e; ++i) {
        const uint8_t *sl = slots + i * 0x2c0;
        if (!rd<uint32_t>(sl, 0x20)) continue;
        const uint8_t *pkg = rd<const uint8_t *>(sl, 0x18);
        if (!pkg) continue;
        const int32_t n = rd<int32_t>(pkg, 0x2c);
        const uint8_t *tab = rd<const uint8_t *>(pkg, 0x30);
        if (n <= 0 || !tab) continue;
        const uint8_t *hit = nullptr;
        bool ended = false;
        for (uint32_t k = sid % static_cast<uint32_t>(n); k < static_cast<uint32_t>(n); ++k) {
            const uint32_t e = rd<uint32_t>(tab, k * 16);
            if (!e) { ended = true; break; }
            if (e == sid) { hit = tab + k * 16; break; }
        }
        if (!hit && !ended)
            for (uint32_t k = 0; k < static_cast<uint32_t>(n); ++k) {
                const uint32_t e = rd<uint32_t>(tab, k * 16);
                if (!e) { ended = true; break; }
                if (e == sid) { hit = tab + k * 16; break; }
            }
        if (ended) continue;
        if (hit) {
            uint8_t *item = rd<uint8_t *>(hit, 8);
            if (item) return item;
        }
    }
    return nullptr;
}
// the anim at an index / with a sid of the generator's art table (entries of 16 {sid, slot})
extern "C" U3X_FN uint8_t *u3x_anim_by_index(uint8_t *g, int32_t i) {
    const uint8_t *t = art_table(g);
    if (!t || static_cast<uint32_t>(i) >= rd<uint32_t>(t, 0)) return nullptr;
    const uint8_t *e = rd<const uint8_t *>(t, 8);
    if (!e) return nullptr;
    uint8_t **pp = rd<uint8_t **>(e, static_cast<uint32_t>(i) * 16 + 8);
    return pp ? *pp : nullptr;
}
extern "C" U3X_FN uint8_t *u3x_anim_by_sid(uint8_t *g, uint32_t sid) {
    uint8_t *t = const_cast<uint8_t *>(art_table(g));
    if (!t) return nullptr;
    const int32_t k = u3h_tab_find(t, sid);
    if (k < 0) return nullptr;
    uint8_t **pp = rd<uint8_t **>(rd<const uint8_t *>(t, 8), static_cast<uint32_t>(k) * 16 + 8);
    return pp ? *pp : nullptr;
}
// the net anim package (row, col); row > 5 or col > 7 is none (ec[89] bit 31)
extern "C" U3X_FN uint8_t *u3x_net_art_group(uintptr_t base, uint32_t row, uint32_t col) {
    if (row > 5 || col > 7) {
        ext_ec()[89] |= 0x80000000u;
        return nullptr;
    }
    return u3h_find_art_group_name(base, anim_packages[row][col]);
}
// the stream record whose id lists hold id (PS4 0xfd4c60's search: records 0x1d46090, count 0x1d46088, at most 0x32)
extern "C" U3X_FN uint8_t *u3x_stream_record(uintptr_t base, uint32_t id) {
    const int32_t n = at<int32_t>(base + 0x1d46088);
    for (int32_t i = 0; i < n && i < 0x32; ++i) {
        uint8_t *r = at<uint8_t *>(base + 0x1d46090 + 8 * static_cast<uint64_t>(i));
        if (!r) continue;
        const uint32_t lists = rd<uint32_t>(r, 0);
        const uint8_t *def = rd<const uint8_t *>(r, 0x30);
        if (!def) continue;
        const uint32_t *counts = rd<const uint32_t *>(def, 0);
        const uint32_t *const *ids = rd<const uint32_t *const *>(def, 8);
        if (!counts || !ids) continue;
        for (uint32_t u = 0; u < lists; ++u) {
            const uint32_t c = counts[u];
            if (!c) break;
            const uint32_t *list = ids[u];
            if (!list) continue;
            for (uint32_t k = 0; k < c; ++k)
                if (list[k] == id) return r;
        }
    }
    return nullptr;
}
// the generator's skeleton (its handle object +0x48 / +0x50, vt+0x190)
extern "C" U3X_FN int32_t u3x_generator_skeleton(uint8_t *g) {
    uint8_t *o = handle_object(rd<uint8_t **>(g, 0x48), rd<uint32_t>(g, 0x50));
    return o ? virtual_call<int32_t>(o, 0x190) : 0;
}
// the generator's snapshot pick (0x6844f0 with its time): index (-1 none) and blend
extern "C" U3X_FN int32_t u3x_generator_pick(uintptr_t base, uint8_t *g, uint32_t *blend) {
    int32_t idx = -1;
    *blend = 0x3f800000u;
    u3x_call_float_arg(base + 0x6844f0, rd<uint32_t>(g, 0x2180), reinterpret_cast<uint64_t>(g), reinterpret_cast<uint64_t>(&idx),
                       reinterpret_cast<uint64_t>(blend), 0);
    return idx;
}

// ===== v7.176: next batch, part 25 (PAGE_MULTIPLAYER events) =====

extern "C" {
U3H_IMPORT void u3h_mp_quit(uintptr_t base, void *s);
U3H_IMPORT int32_t u3h_server_notice(uintptr_t base, int32_t *arg);
U3H_IMPORT int u3h_chat_restricted(uintptr_t base);
}

// a012: PS3 1.19's PAGE_MULTIPLAYER handler (f_298e5c). ea[1] bit 3 keeps only the quit check. ea[40] page opens,
// [41] welcomes, [42] content notices, [43] server notices, [44] disconnects, [45] members sent to the lobby.
namespace {
U3X_BSS uint8_t server_notice_done, chat_notice_done;
}
extern "C" U3X_FN int u3x_party_leader(uintptr_t base) {
    const uint8_t *ps = reinterpret_cast<const uint8_t *>(base + 0x17dc298);
    return !rd<uint64_t>(ps, 0x18) || rd<uint32_t>(ps, 0x12e4) == rd<uint32_t>(ps, 0x12e8);
}
// the party session lock (the backend's slot 8), by the leader only
extern "C" U3X_FN void u3x_party_lock(uintptr_t base, int v) {
    uint8_t *ps = reinterpret_cast<uint8_t *>(base + 0x17dc298);
    uint8_t *be = rd<uint8_t *>(ps, 8);
    if (be && u3x_party_leader(base)) {
        const uintptr_t fn = (*reinterpret_cast<uintptr_t **>(be))[0x40 / 8];
        if (fn) reinterpret_cast<int64_t (*)(void *, void *, int)>(fn)(be, ps, v);
    }
}
extern "C" U3X_FN int u3x_split_screen(uintptr_t base) { return at<uint8_t>(base + 0x17d7b06) || at<int32_t>(base + 0x17f2fc0) == 3; }
extern "C" U3X_FN uint32_t u3x_popup_id(uintptr_t base) {
    const uint8_t *p = at<const uint8_t *>(base + 0x17bd080 + 0x898);
    return p ? rd<uint32_t>(p, 0x10) : 0;
}
extern "C" U3X_FN void u3x_send_player_info(uintptr_t base) {
    game_call<void>(base + 0x6b7b30, 0);
    game_call<void>(base + 0x6b7b30, 1);
}
extern "C" U3X_FN void u3x_page_show(uintptr_t base, void *s, uint32_t page, int a) {
    const uintptr_t show = state_show_page(s);
    if (show) reinterpret_cast<int (*)(void *, uint32_t, int, int)>(show)(reinterpret_cast<void *>(base + 0x17bd080), page, a, 0);
}
// one content notice: when the pack flag is set, each loaded profile without the record bit gets the cash (or the items)
// once, the HUD line and a save
extern "C" U3X_FN void u3x_content_notice(uintptr_t base, void *s, uint32_t pack, uint32_t dc, uint32_t bit, uint32_t txt, int items) {
    if (!at<uint8_t>(base + 0x17c3a40 + pack)) return;
    const uint8_t *tb = dc_table(s, dc);
    if (!tb) return;
    for (uint32_t u = 0; u < 2; ++u) {
        uint8_t *p = u3h_lg_profile(base, u);
        if (!p || rd<uint32_t>(p, 0x7ae8) != 3) continue;
        const uint32_t f = rd<uint32_t>(u3h_lg_rec(base, s, p), 0x1ad0);
        if (f & bit) continue;
        if (!items) {
            game_call<void>(base + 0x660fb0, static_cast<void *>(nullptr), static_cast<int32_t>(u), rd<int32_t>(tb, 0));
        } else {
            const uint32_t n = rd<uint32_t>(tb, 0);
            const uint32_t *it = rd<const uint32_t *>(tb, 8);
            for (uint32_t i = 0; it && i < n; ++i) {
                const uint32_t id = it[i];
                if (id < 0x400) ++u3h_lg_rec(base, s, p)[0x884 + id];
            }
        }
        *reinterpret_cast<uint32_t *>(u3h_lg_rec(base, s, p) + 0x1ad0) = f | bit;
        uint32_t ic[3];
        ic[0] = 0;
        ic[1] = 0;
        ic[2] = 0xffffffffu;
        u3x_ms_hud(base, 0, u3x_ms_text(base, txt), 0, 0, ic);
        u3x_publish(base, s, p);
        ++ext_ea()[42];
    }
}
// the page events: 0 open (party unlock, player info), 1 welcome (once per profile) and mode 0, 3 Leave Multiplayer?,
// 6 every frame (content notices, the server's notice or the chat restriction notice once, quit, disconnect, members
// follow the leader to the lobby)
extern "C" U3X_FN void u3x_multiplayer_page(uintptr_t base, void *s, int32_t *ev) {
    uint32_t *ea = ext_ea();
    if (ea[1] & 8u) {
        if (ev[0] == 6) u3h_mp_quit(base, s);
        return;
    }
    uint8_t *net = reinterpret_cast<uint8_t *>(base + 0x17d5c58);
    switch (ev[0]) {
    case 0:
        u3x_party_lock(base, 0);
        u3x_send_player_info(base);
        ++ea[40];
        return;
    case 1: {
        uint8_t *p = u3h_lg_profile(base, 0);
        if (p) {
            uint8_t *rec = u3h_lg_rec(base, s, p);
            const uint32_t f = rd<uint32_t>(rec, 0x12e8);
            if (!(f & 1)) {
                *reinterpret_cast<uint32_t *>(u3h_lg_rec(base, s, p) + 0x12e8) = f | 1;
                u3h_show_dialog(base, s, 0xa2a50fecu, 0xc6467f2du, 0, 0, 0, 0);
                ++ea[41];
            }
        }
        at<uint32_t>(base + 0x17d603c) = 0;
        if (u3x_party_leader(base)) {
            uint32_t v = 0;
            u3h_party_send(base, 0x110, &v, 4);
        }
        return;
    }
    case 3:
        if (!net[9]) return;
        reinterpret_cast<uint8_t *>(ev)[0x10] = 1;
        u3h_show_dialog(base, s, u3x_split_screen(base) ? 0x98ec2aa5u : 0xd1dab365u, 0x0260a263u, 0xafb7834cu, 0x075bc82d, 0x375737e9u, 0);
        return;
    case 6: break;
    default: return;
    }
    u3x_content_notice(base, s, 69, 0x1ab455f4u, 1, 0x43263d81u, 0);   // Subway $100,000
    u3x_content_notice(base, s, 82, 0xf6bd7840u, 2, 0x7c862ff1u, 0);   // beta participation $100,000
    u3x_content_notice(base, s, 81, 0xad9e0b32u, 4, 0xce81cfb2u, 1);   // Treasure Hunter Pack
    {
        bool skip = false;
        if (!server_notice_done) {
            int32_t arg = 0;
            const int32_t v = u3h_server_notice(base, &arg);
            const uint8_t *tb = dc_table(s, 0xffac56f2u);
            if (!u3x_popup_id(base) && v >= 0 && tb) {
                server_notice_done = 1;
                ++ea[43];
                if (v == 2) net[0x3e8] = 1;
                else if (v == 0) u3x_page_show(base, s, 0x37656388u, 1);
                game_call<int>(base + 0x65a000, reinterpret_cast<void *>(base + 0x17bd080), 0x587a7162u);   // PAGE_MULTIPLAYER_BAN popup
                skip = true;
            }
        }
        if (!skip && !chat_notice_done && u3h_chat_restricted(base) && !u3x_popup_id(base)) {
            chat_notice_done = 1;
            u3h_show_dialog(base, s, 0x62501513u, 0xc6467f2du, 0, 0, 0, 0xe1e040aau);
        }
    }
    u3h_mp_quit(base, s);
    if (net[0x28] && !at<uint8_t>(base + 0x1b68a48) && at<int32_t>(base + 0x17f2fc0) != 1) {   // disconnected
        u3x_page_show(base, s, 0x37656388u, 1);
        u3h_show_dialog(base, s, 0x61a3662eu, 0xc6467f2du, 0xd4326d90u, 0, 0, 0xe1e040aau);
        ++ea[44];
        return;
    }
    if (!u3x_party_leader(base)) {   // members follow the leader: PAGE_LOBBY
        u3x_page_show(base, s, 0xb99a3235u, 0);
        ++ea[45];
    }
}

// ===== v7.177: next batch, part 26 (the Cinema film list rows) =====

extern "C" {
U3H_IMPORT char *u3h_cin_cat(char *o, char *end, const char *t);
U3H_IMPORT char *u3h_cin_num(char *o, char *end, int64_t v, int width);
U3H_IMPORT const int32_t *u3h_cin_localtime(const int64_t *t);
U3H_IMPORT void u3h_cin_megabytes(uintptr_t base, void *s, char *b, const uint8_t *e);
U3H_IMPORT char *u3h_cin_buffer(int which);
}

// LIST_CINEMA (enum 0x39, retagged): the local film list or the online file list (the flag at 0x16e7168): title, per
// row "<net game> - <title>", user, index, duration m:ss, size in MB (the float formatting stays in C), date in the
// region's order and 12 / 24 h clock, map and game type names, the net game / game type images and the lock icon for
// protected films; selection and count. Only with net menu mode 4. cin[8] calls, cin[9] last local pick; eb[24] the
// online selection.
namespace {
U3X_RO const char sep_dash[] = " - ", colon[] = ":", slash_char[] = "/", space_char[] = " ", am[] = "AM", pm[] = "PM", megabytes[] = " MB";
U3X_FN int32_t ui_icon_index(uintptr_t base, uint32_t id, int which) {
    const uint8_t *ui = at<const uint8_t *>(base + 0x1b24bc8);
    if (!ui) return -1;
    const uint8_t *t = rd<const uint8_t *>(ui, which ? 0x28 : 0x20);
    if (!t) return -1;
    const uint32_t n = rd<uint16_t>(t, 2);
    const uint8_t *e = rd<const uint8_t *>(t, 8);
    for (uint32_t i = 0; e && i < n; ++i)
        if (rd<uint32_t>(e, i * 40 + 8) == id) return static_cast<int32_t>(i);
    return -1;
}
U3X_FN void image_answer(int32_t *ev, int32_t type, void *ptr, int32_t a4, int32_t a5, int32_t a6, int32_t a7) {
    ev[2] = type;
    *reinterpret_cast<void **>(ev + 4) = ptr;
    ev[6] = a4;
    ev[7] = a5;
    ev[8] = a6;
    ev[9] = a7;
    ev[10] = 0;
    ev[11] = -1;
    ev[12] = 0x3f800000;
}
}  // namespace
extern "C" U3X_FN int32_t u3x_cin_icon_index(uintptr_t base, uint32_t id, int which) { return ui_icon_index(base, id, which); }
extern "C" U3X_FN void u3x_cin_image(int32_t *ev, int32_t type, void *ptr, int32_t a4, int32_t a5, int32_t a6, int32_t a7) {
    image_answer(ev, type, ptr, a4, a5, a6, a7);
}
extern "C" U3X_FN uint8_t *u3x_cin_netgame(void *s, int idx) {
    const uint8_t *t = dc_table(s, 0x3b9a067du);
    if (!t || idx < 0 || idx >= rd<int32_t>(t, 0)) return nullptr;
    uint8_t *r = rd<uint8_t *>(t, 8);
    return r ? r + static_cast<int64_t>(idx) * 0xd0 : nullptr;
}
extern "C" U3X_FN int u3x_cinema_rows(uintptr_t base, int32_t *ev, void *s) {
    if (!cinema_on()) return 0;
    uint8_t *st = u3h_cin_state(base);
    ++ext_cin()[8];
    if (at<uint32_t>(base + 0x17d603c) != 4) {
        set_row_text(ev, empty_text);
        ev[1] = 0;
        return 1;
    }
    int32_t &online_sel = *reinterpret_cast<int32_t *>(&ext_eb()[24]);
    {
        const int32_t c = rd<int32_t>(st, 0x1e58);
        if (c != 0 && online_sel > c - 1) online_sel = c - 1;
    }
    const bool local = films_local(base) != 0;
    uint8_t *list = local ? film_list(st) : rd<uint8_t *>(st, 0x1e60);
    uint8_t *e = list ? list + static_cast<uint32_t>(ev[1]) * static_cast<uintptr_t>(film_entry) : nullptr;
    char *txt = u3h_cin_buffer(0), *tmp = u3h_cin_buffer(1);
    char *o = txt, *end = txt + 0x1ff;
    const int32_t local_count = film_count(st), online_count = rd<int32_t>(st, 0x1e58);
    switch (ev[0]) {
    case 1: set_row_text(ev, u3h_text(base, s, local ? 0xe626e948u : 0xdc27f8a0u)); break;
    case 2: {
        if (local ? local_count == 0 : online_count == 0) break;
        if (!e || static_cast<uint32_t>(ev[1]) >= 20u) break;
        const uint32_t column = static_cast<uint32_t>(ev[0x15]);
        const uint8_t *h = e + 0x218;
        const int game = static_cast<int16_t>(rd<uint32_t>(h, 8));
        if (column == 0) {   // "<net game> - <title>"
            const uint8_t *rec = u3x_cin_netgame(s, game);
            if (!rec) { set_row_text(ev, empty_text); break; }
            for (int k = 0; k < 0x100; ++k) tmp[k] = 0;
            game_call<int>(base + 0x741260, reinterpret_cast<void *>(base + 0x17cb060), tmp, 0x100, rec, reinterpret_cast<void *>(base + 0x17cb060 + 0x6a38), 1, 1, 1);
            o = u3h_cin_cat(o, end, tmp);
            o = u3h_cin_cat(o, end, sep_dash);
            u3h_cin_cat(o, end, reinterpret_cast<const char *>(h + 0x160));
            set_row_text(ev, txt);
        } else if (column == 0x8adf5c31u) {
            set_row_text(ev, reinterpret_cast<const char *>(h + 0x40));   // user
        } else if (column == 0x7cc44c67u) {
            u3h_cin_num(o, end, rd<int32_t>(h, 0x1e0), 0);
            set_row_text(ev, txt);
        } else if (column == 0x9586358cu) {   // m:ss
            const int32_t d = rd<int32_t>(h, 0x18);
            o = u3h_cin_num(o, end, d / 60, 0);
            o = u3h_cin_cat(o, end, colon);
            u3h_cin_num(o, end, d % 60, 2);
            set_row_text(ev, txt);
        } else if (column == 0x0ed4812eu) {   // "<size> MB"
            char b[0x40];
            u3h_cin_megabytes(base, s, b, e);
            o = u3h_cin_cat(o, end, b);
            u3h_cin_cat(o, end, megabytes);
            set_row_text(ev, txt);
        } else if (column == 0x8419ca2au) {   // the date: day/month/year (month first for English in region 1), the time
            const int32_t *t = u3h_cin_localtime(reinterpret_cast<const int64_t *>(h + 0x20));
            if (!t) break;
            const int32_t min = t[1], hour = t[2], mday = t[3], mon = t[4], year = t[5];
            const uint8_t *lt = reinterpret_cast<const uint8_t *>(base + 0x1acd198);
            const uint32_t lang = rd<uint32_t>(lt, 8 + 0x38 * static_cast<uint64_t>(rd<uint32_t>(lt, 0x604)));
            const uint32_t region = at<uint32_t>(base + 0x17b6368 + 0x3994);
            char tb[16], db[16], mb[16], yb[16];
            char *q;
            if (region == 1) {   // %I:%M%p
                int hh = hour % 12;
                if (!hh) hh = 12;
                q = u3h_cin_num(tb, tb + 15, hh, 2);
                q = u3h_cin_cat(q, tb + 15, colon);
                q = u3h_cin_num(q, tb + 15, min, 2);
                u3h_cin_cat(q, tb + 15, hour < 12 ? am : pm);
            } else {   // %H:%M
                q = u3h_cin_num(tb, tb + 15, hour, 2);
                q = u3h_cin_cat(q, tb + 15, colon);
                u3h_cin_num(q, tb + 15, min, 2);
            }
            u3h_cin_num(db, db + 15, mday, 2);
            u3h_cin_num(mb, mb + 15, mon + 1, 2);
            u3h_cin_num(yb, yb + 15, year + 1900, 4);
            const char *T = tb + (tb[0] == '0'), *D = db + (db[0] == '0'), *M = mb + (mb[0] == '0');
            const char *a = D, *b = M;
            if (lang == 0 && region == 1) {
                a = M;
                b = D;
            }
            o = u3h_cin_cat(o, end, a);
            o = u3h_cin_cat(o, end, slash_char);
            o = u3h_cin_cat(o, end, b);
            o = u3h_cin_cat(o, end, slash_char);
            o = u3h_cin_cat(o, end, yb);
            o = u3h_cin_cat(o, end, space_char);
            u3h_cin_cat(o, end, T);
            set_row_text(ev, txt);
        } else if (column == 0x16094dd2u) {   // the map name
            const uint8_t *rec = u3x_cin_netgame(s, game);
            if (!rec) break;
            const uint8_t *mt = game_call<const uint8_t *>(state_native(s, 13), 0x0dec97a4u);
            uint32_t id = 0;
            if (mt) {
                const uint32_t n = rd<uint32_t>(mt, 0);
                const uint8_t *me = rd<const uint8_t *>(mt, 8);
                for (uint32_t i = 0; me && i < n; ++i)
                    if (rd<uint32_t>(me, i * 0x98) == rd<uint32_t>(rec, 4)) { id = rd<uint32_t>(me, i * 0x98 + 4); break; }
            }
            set_row_text(ev, u3h_text(base, s, id));
        } else if (column == 0xa42c91c5u) {   // the game type name
            const uint8_t *rec = u3x_cin_netgame(s, game);
            if (!rec) break;
            const uint8_t *rule = game_call<const uint8_t *>(base + 0x741070, reinterpret_cast<void *>(base + 0x17cb060), rd<int32_t>(rec, 0xc));
            set_row_text(ev, u3h_text(base, s, rule ? rd<uint32_t>(rule, 0) : 0));
        }
        break;
    }
    case 4: {
        if (local ? local_count == 0 : online_count == 0) break;
        if (!e || static_cast<uint32_t>(ev[1]) >= 20u) break;
        const uint32_t slot = static_cast<uint32_t>(ev[0x14]);
        const uint8_t *h = e + 0x218;
        const int game = static_cast<int16_t>(rd<uint32_t>(h, 8));
        if (slot == 0x2138a9b2u) {
            const uint8_t *rec = u3x_cin_netgame(s, game);
            if (rec) image_answer(ev, 0x24, nullptr, rd<int32_t>(rec, 4), 0, 0, -1);
        } else if (slot == 0xa42c91c5u) {
            const uint8_t *rec = u3x_cin_netgame(s, game);
            if (!rec) break;
            const uint8_t *rule = game_call<const uint8_t *>(base + 0x741070, reinterpret_cast<void *>(base + 0x17cb060), rd<int32_t>(rec, 0xc));
            const char *nm = rule ? rd<const char *>(rule, 0x40) : nullptr;
            void *tex = (nm && state_native(s, 20)) ? reinterpret_cast<void *(*)(uintptr_t, const char *)>(state_native(s, 20))(base + 0x1b31128, nm) : nullptr;
            if (tex) image_answer(ev, 2, tex, 0, 0, 0, -1);
            else image_answer(ev, 0x10, nullptr, 0, 0, 0, -1);
        } else if (slot == 0 && (rd<uint32_t>(e, 0x214) & 0x180) != 0x180) {   // the lock
            image_answer(ev, 3, nullptr, 0, static_cast<int32_t>(0xea36b17fu), 0, ui_icon_index(base, 0xea36b17fu, 0));
        }
        break;
    }
    case 5: ev[1] = local ? film_selected(st) : online_sel; break;
    case 6: ev[1] = local ? local_count - 1 : online_count - 1; break;
    case 10:
        if (local) {
            ext_cin()[9] = static_cast<uint32_t>(ev[1]);
            u3x_film_select(base, s, ev[1]);
        } else {
            online_sel = ev[1];
        }
        break;
    }
    return 1;
}

// ===== v7.178: next batch, part 27 (Play Online: the entry, the sign-in wait, the online NetInit) =====

extern "C" {
U3H_IMPORT int u3h_onb_shell_route(uintptr_t base);
U3H_IMPORT int u3h_online_config(void *s);
U3H_IMPORT int32_t u3h_online_status(void);
U3H_IMPORT int u3h_smn_up(void);
U3H_IMPORT int u3h_rc_signing_in(void);
U3H_IMPORT int u3h_offline_ok(void);
U3H_IMPORT void u3h_go_offline(void);
U3H_IMPORT void u3h_set_onb_mode(int mode);
U3H_IMPORT int u3h_onb_mode_get(void);
U3H_IMPORT int u3h_k01_play_online(uintptr_t base, void *s);
U3H_IMPORT void u3h_k01_lan(void);
U3H_IMPORT uint32_t u3h_k01_back_page(void);
U3H_IMPORT void u3h_onp_name_apply(uintptr_t base);
U3H_IMPORT void u3h_k43_step(uintptr_t base, uint32_t v);
U3H_IMPORT int u3h_onp_pending(void);
U3H_IMPORT int u3h_srv_retry_pending(void);
U3H_IMPORT int32_t u3h_rc_state(void);
U3H_IMPORT uint32_t u3h_profile_state(int *has_memory);
U3H_IMPORT int u3h_rc_server_ready(void);
U3H_IMPORT int u3h_online_cfg_ok(void);
U3H_IMPORT void u3h_profile_retry(void);
U3H_IMPORT void *u3h_onb_setup(uintptr_t base);
U3H_IMPORT void u3h_cm_online_reset(uintptr_t base);
U3H_IMPORT void u3h_pl_open(uintptr_t base);
}

// b014 / b044: Play Online (PS3 1.19 f_296628): without sign-in settings, a running sign-in or the session, it is the
// network failure dialog unless offline mode is allowed (then the online lobby runs on the LAN backend); the profile
// page comes before NetInit. The sign-in wait (startup phase 1) waits up to 10 s for the community sign-in, 30 s for a
// running RPCN sign-in / session handshake / profile download, 150 s while the server sign-in is retried. eb[150] the
// result (1 waiting, 2 signed in, 3 failed, 4..6 NetInit steps, 7 offline, 8 no profile), eb[151] waits.
namespace {
U3X_BSS int32_t wait_since, wait_last, retry_asked;
U3X_BSS uint32_t online_rng;
U3X_RO const char no_settings_note[] = "Play Online stopped (No connection found): no sign-in settings and no sign-in running";
U3X_RO const char retry_note[] = "Play Online: the profile download failed earlier; trying it again (waits up to 30 s)";
U3X_RO const char timeout_note[] = "Play Online stopped (No connection found): sign-in not finished in time, RPCN state ";
U3X_FN bool online_matchmaking() { return (ext_eb()[0] & 0x10) != 0; }
}  // namespace
extern "C" U3X_FN void u3x_online_failed(uintptr_t base, void *s) { u3h_show_dialog(base, s, 0x88a082afu, 0xc6467f2du, 0, 0, 0, 0xe1e040aau); }
extern "C" U3X_FN void u3x_play_online(uintptr_t base, void *s) {
    const uint32_t ui = at<uint32_t>(base + 0x17bd080 + 8);
    if (ui == 7 || ui == 8) return;   // the menu is changing
    if (u3h_onb_shell_route(base)) return;
    const int configured = u3h_online_config(s);
    if ((!configured && u3h_smn_up() != 1 && !u3h_rc_signing_in()) || !online_matchmaking()) {
        if (!u3h_offline_ok()) {
            u3h_so_note(no_settings_note, nullptr, -1, nullptr);
            u3x_online_failed(base, s);
            return;
        }
        u3h_go_offline();
    } else {
        u3h_set_onb_mode(1);
    }
    at<uint8_t>(base + 0x17d6065) = 0;   // not split-screen co-op
    if (u3h_k01_play_online(base, s)) return;
    if (state_show_page(s)) u3h_show(base, s, 0x4608c1adu, 0);
}
extern "C" U3X_FN void u3x_lan_chosen() {
    u3h_set_onb_mode(0);
    u3h_k01_lan();
}
// 1 while waiting, 0 to go on, -1 after a failure (dialog shown, startup stopped)
extern "C" U3X_FN int u3x_signin_wait(uintptr_t base, void *s) {
    if (u3h_onb_mode_get() != 1 || !online_matchmaking()) return 0;
    const uint32_t selected = profile_selected(s), count = profile_count(s);
    if (selected && selected <= count) {   // the profile name is the player name
        char *dst = reinterpret_cast<char *>(base + 0x17d6010);
        const char *src = reinterpret_cast<const char *>(static_cast<uint8_t *>(s) + 136 + (selected - 1) * 64);
        unsigned n = 0;
        for (; n < 16 && src[n]; ++n) dst[n] = src[n];
        dst[n] = 0;
    }
    u3h_onp_name_apply(base);
    uint32_t *eb = ext_eb();
    const int32_t now = at<int32_t>(base + 0x17b4ba4);
    ++eb[151];
    if (eb[150] != 1 || now - wait_last > 3 * 300) {   // a wait left earlier starts over
        eb[150] = 1;
        wait_since = now;
    }
    wait_last = now;
    {
        int has_memory;
        const uint32_t pst = u3h_profile_state(&has_memory);   // the profile download (1 without the online memory)
        if ((u3h_online_status() == 2 || u3h_smn_up() == 1) && has_memory && u3h_rc_server_ready() && (pst == 0u || pst == 3u) &&
            now - wait_since < 30 * 300) {
            if (pst == 3u && now - retry_asked > 5 * 300) {
                retry_asked = now;
                u3h_profile_retry();
                u3h_so_note(retry_note, nullptr, -1, nullptr);
            }
            const uint32_t k = static_cast<uint32_t>(now - wait_since) / (3 * 300);
            u3h_k43_step(base, 8u + (k > 10u ? 10u : k));   // "Syncing live data"
            return 1;
        }
    }
    if (u3h_online_status() == 2 || (u3h_smn_up() == 1 && !u3h_onp_pending())) {
        eb[150] = 2;
        return 0;
    }
    if (((u3h_online_cfg_ok() && u3h_online_status() != 3 && u3h_online_status() != 4) || u3h_smn_up() == 2) && now - wait_since < 10 * 300) return 1;
    if ((u3h_rc_signing_in() || (u3h_smn_up() == 1 && u3h_onp_pending())) && now - wait_since < 30 * 300) return 1;
    if (u3h_srv_retry_pending() && now - wait_since < 150 * 300) return 1;
    if (u3h_offline_ok()) {
        eb[150] = 7;
        u3h_go_offline();
        return 0;
    }
    eb[150] = 3;
    *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 680) = 0;   // startup_phase
    u3h_so_note(timeout_note, nullptr, u3h_rc_state(), nullptr);
    if (state_show_page(s)) u3h_show(base, s, u3h_k01_back_page(), 1);
    u3x_online_failed(base, s);
    return -1;
}
// the online NetInit (PS3 1.19 worker, online branch): the online backend object (built in C), Init, the party session,
// both sessions attached, the party's game pick from DC 0x8653fbe2 (random), the game socket, the connection table in
// online mode, the peer link. eb[128] the object, eb[129] NetInits, eb[130] Init result.
extern "C" U3X_FN int u3x_online_netinit(uintptr_t base, void *s) {
    void *be = u3h_onb_setup(base);
    uint32_t *eb = ext_eb();
    const int64_t r = object_call(be, 0);
    eb[130] = static_cast<uint32_t>(r);
    if (r != 0) { eb[150] = 4; return -1; }
    uint8_t *ms = match_session(base), *ps = reinterpret_cast<uint8_t *>(base + 0x17dc298);
    game_call<int64_t>(base + 0x7a2360, reinterpret_cast<uintptr_t>(ps));
    ps[0x1334] = 0;
    ps[0x1375] = 0;
    *reinterpret_cast<uint64_t *>(ps + 0x1340) = 0;
    *reinterpret_cast<uint32_t *>(ps + 0x1338) = 0;
    *reinterpret_cast<uint32_t *>(ps + 0x1370) = 0;
    ps[0x1348] = 0;
    ps[0x1374] = 0;
    object_call2(be, 3, ms);
    object_call2(be, 3, ps);
    reinterpret_cast<int64_t (*)(void *, void *, int)>((*static_cast<uintptr_t **>(be))[8])(be, ps, 1);
    {
        const uint8_t *dc = dc_table(s, 0x8653fbe2u);
        if (dc && rd<uint32_t>(dc, 0) && rd<const uint32_t *>(dc, 8)) {
            if (!online_rng) online_rng = static_cast<uint32_t>(at<int32_t>(base + 0x17b4ba4)) | 1;
            online_rng = online_rng * 1103515245u + 12345u;
            const uint32_t rnd = (online_rng >> 8) & 0x7fffffff;
            *reinterpret_cast<uint32_t *>(ps + 0x14) = rd<const uint32_t *>(dc, 8)[rnd % rd<uint32_t>(dc, 0)];
        }
    }
    if (u3h_gt_open(base) != 0) { eb[150] = 5; return -1; }
    u3h_cm_online_reset(base);
    u3h_pl_open(base);
    eb[150] = 6;
    return 0;
}

// ===== v7.179: next batch, part 28 (the emblem manager state machine) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_emb_np(uintptr_t b, uint32_t i);
U3H_IMPORT void u3h_emb_new_records(uintptr_t b, uint32_t off, uint32_t size);
U3H_IMPORT void u3h_emb_set_record(uintptr_t b, uint8_t *np, int32_t k, uint64_t r);
U3H_IMPORT void u3h_emb_free_record(uintptr_t b, uint64_t r);
U3H_IMPORT uint8_t *u3h_emb_profile(uintptr_t b, uint32_t user);
U3H_IMPORT void u3h_emb_bind(uintptr_t b, void *s);
U3H_IMPORT void u3h_emb_frame_parts(uintptr_t b, void *s, int which);
U3H_IMPORT uint64_t u3h_emb_alloc_default(uintptr_t b);
U3H_IMPORT uint8_t *u3h_emb_records(uint8_t **flag);
}

// c011: PS3 1.19's emblem manager (0x17c3440: +0 editor mode, +1 frame toggle, +2 render failed, +4/+8 editor layers,
// +0x28/+0x2c player layers, +0x4c local user, +0x50/+0x58 player records, +0x60 player index, +0x68/+0x70 editor
// records, +0x78 default record, +0x84 editor countdown). Records: 24 of 0x50 bytes (pin +0x24, used +0x26). The
// render flag (PS3 +0x4a8) asks the compositor for a frame. ec[0] bit 14 turns it off; ec[120] editor requests, [121]
// records handed to players, [126] ticks, [127] NetInits, [128] shutdowns, [146] NetInit editor requests.
namespace {
constexpr uint32_t record_bytes_e = 0x50;
uint8_t *emblem_manager(uintptr_t b) { return reinterpret_cast<uint8_t *>(b + 0x17c3440); }
U3X_FN bool emblems_off() { return (ext_ec()[0] & 0x4000u) != 0; }
U3X_FN void unpin(uint64_t r) {
    if (r && u3h_emb_is_record(r)) *reinterpret_cast<uint16_t *>(r + 0x24) = 0;
}
U3X_FN void pin(uint64_t r) {
    if (r && u3h_emb_is_record(r)) *reinterpret_cast<uint16_t *>(r + 0x24) = 1;
}
U3X_FN uint8_t &render_flag() {
    uint8_t *flag;
    u3h_emb_records(&flag);
    return *flag;
}
U3X_FN uint32_t local_user(uintptr_t b) { return at<uint8_t>(b + 0x17bde75) ? 1u : at<uint32_t>(b + 0x17bf714); }
}  // namespace
// player mode (PS3 f_27d050): pick a player without records, copy its layers, ask for two 128 records and a frame;
// the frame after, hand them to the player (or free them when the render failed)
extern "C" U3X_FN void u3x_emblem_players(uintptr_t b, void *s) {
    (void)s;
    uint8_t *e = emblem_manager(b);
    const int32_t idx = rd<int32_t>(e, 0x60);
    render_flag() = 0;
    if (idx == -1) {
        const uint8_t *jc = at<const uint8_t *>(b + 0x1b31128 + 0x6408);
        if (jc && rd<uint64_t>(jc, 0x20)) return;   // a texture load job is pending
        for (uint32_t i = 0; i < 10; ++i) {
            const uint8_t *np = u3h_emb_np(b, i);
            if (!np || !emblem_player_valid(np) || (rd<uint64_t>(np, 0x7e8) && rd<uint64_t>(np, 0x7f0))) continue;
            *reinterpret_cast<int32_t *>(e + 0x60) = static_cast<int32_t>(i);
            const uint8_t *src = u3h_emb_np(b, i % 10);
            for (int k = 0; k < 32; ++k) e[0x2c + k] = src[0x980 + k];
            *reinterpret_cast<int32_t *>(e + 0x28) = 4;
            render_flag() = 1;
            e[2] = 0;
            e[1] = 1;
            u3h_emb_new_records(b, 0x50, 0x80);
            return;
        }
    } else if (!e[1]) {
        for (int k = 0; k < 2; ++k) {
            const uint64_t r = rd<uint64_t>(e, 0x50 + 8 * k);
            unpin(r);
            if (e[2] == 0) {
                uint8_t *np = u3h_emb_np(b, static_cast<uint32_t>(idx));
                if (np) {
                    u3h_emb_set_record(b, np, k, r);
                    ++ext_ec()[121];
                } else {
                    u3h_emb_free_record(b, r);
                }
            } else {
                u3h_emb_free_record(b, r);
            }
            *reinterpret_cast<uint64_t *>(e + 0x50 + 8 * k) = 0;
        }
        *reinterpret_cast<int32_t *>(e + 0x60) = -1;
    } else {
        e[1] = 0;
    }
}
// editor re-render request (PS3 f_27d790): the local profile's emblem as two 256 records, countdown 4
extern "C" U3X_FN void u3x_emblem_editor(uintptr_t b, void *s) {
    uint8_t *e = emblem_manager(b);
    if (!e[0]) return;
    ++ext_ec()[120];
    if (rd<int32_t>(e, 0x84) < 1) {
        *reinterpret_cast<int32_t *>(e + 0x84) = 4;
        *reinterpret_cast<int32_t *>(e + 0x60) = -1;
        const uint32_t u = local_user(b);
        *reinterpret_cast<uint32_t *>(e + 0x4c) = u;
        const uint8_t *p = u3h_emb_profile(b, u);
        if (p)
            for (int k = 0; k < 32; ++k) e[8 + k] = p[k];
        *reinterpret_cast<int32_t *>(e + 4) = 4;
        render_flag() = 1;
        e[1] = 1;
        u3h_emb_new_records(b, 0x68, 0x100);
        u3h_emb_bind(b, s);
    } else {
        *reinterpret_cast<int32_t *>(e + 0x84) = 4;
    }
}
// the update (PS3 f_27daa8): player mode outside the editor; in the editor a user change re-renders, the countdown
// keeps the records pinned and alternates the frame toggle
extern "C" U3X_FN void u3x_emblem_update(uintptr_t b, void *s) {
    uint8_t *e = emblem_manager(b);
    if (!e[0]) {
        for (int k = 0; k < 2; ++k) unpin(rd<uint64_t>(e, 0x68 + 8 * k));
        u3x_emblem_players(b, s);
        return;
    }
    const uint32_t u = local_user(b);
    if (rd<uint32_t>(e, 0x4c) != u) {
        game_call<void>(b + 0x66c190, e);
        *reinterpret_cast<uint32_t *>(e + 0x4c) = u;
        const uint8_t *p = u3h_emb_profile(b, u);
        if (p)
            for (int k = 0; k < 32; ++k) e[8 + k] = p[k];
        *reinterpret_cast<int32_t *>(e + 4) = 4;
        u3x_emblem_editor(b, s);
    }
    render_flag() = 0;
    if (!rd<uint64_t>(e, 0x68)) return;
    int32_t &countdown = *reinterpret_cast<int32_t *>(e + 0x84);
    if (countdown > 0) --countdown;
    const uint64_t r0 = rd<uint64_t>(e, 0x68), r1 = rd<uint64_t>(e, 0x70);
    if (e[2] == 0) {
        if (countdown < 2) {
            if (countdown != 0) return;
            unpin(r0);
            unpin(r1);
            return;
        }
    } else {
        countdown = 4;
    }
    pin(r0);
    pin(r1);
    const uint8_t c = e[1];
    if (!c) render_flag() = 1;
    e[1] = static_cast<uint8_t>(c == 0);
}
// records no slot references any more are freed (PS3 frees where PS4 only zeroes)
extern "C" U3X_FN void u3x_emblem_sweep(uintptr_t b) {
    uint8_t *flag;
    uint8_t *records = u3h_emb_records(&flag);
    uint8_t mark[24];
    for (uint8_t &m : mark) m = 0;
    const uint8_t *e = emblem_manager(b);
    uint64_t slots[25];
    int n = 0;
    slots[n++] = rd<uint64_t>(e, 0x50);
    slots[n++] = rd<uint64_t>(e, 0x58);
    slots[n++] = rd<uint64_t>(e, 0x68);
    slots[n++] = rd<uint64_t>(e, 0x70);
    slots[n++] = rd<uint64_t>(e, 0x78);
    for (uint32_t i = 0; i < 10; ++i) {
        const uint8_t *np = reinterpret_cast<const uint8_t *>(b + 0x17cb3f0 + i * 0x9c0);
        slots[n++] = rd<uint64_t>(np, 0x7e8);
        slots[n++] = rd<uint64_t>(np, 0x7f0);
    }
    for (int k = 0; k < n; ++k)
        if (slots[k] && u3h_emb_is_record(slots[k])) mark[(slots[k] - reinterpret_cast<uint64_t>(records)) / record_bytes_e] = 1;
    for (int k = 0; k < 24; ++k) {
        uint8_t *r = records + k * record_bytes_e;
        if (rd<uint16_t>(r, 0x26) && !mark[k]) {
            *reinterpret_cast<uint16_t *>(r + 0x24) = 0;
            u3h_emb_free_record(b, reinterpret_cast<uint64_t>(r));
        }
    }
}
// per frame: the fallback texture, then (unless off) the re-ticks, the sweep and, in multiplayer, the update and the
// render pass
extern "C" U3X_FN void u3x_emblem_tick(uintptr_t b, void *s) {
    u3h_emb_frame_parts(b, s, 0);   // the fallback (serves with the port off too)
    if (emblems_off()) return;
    u3h_emb_frame_parts(b, s, 1);   // re-ticks
    u3x_emblem_sweep(b);
    if (!at<uint8_t>(b + 0x17d5c61)) return;
    u3x_emblem_update(b, s);
    u3h_emb_frame_parts(b, s, 2);   // the render pass
    ++ext_ec()[126];
}
// NetInit: the default 4x4 record, the table binds, the editor request
extern "C" U3X_FN int u3x_emblem_netinit(uintptr_t b, void *s) {
    if (emblems_off()) return 0;
    uint8_t *e = emblem_manager(b);
    if (!rd<uint64_t>(e, 0x78)) *reinterpret_cast<uint64_t *>(e + 0x78) = u3h_emb_alloc_default(b);
    u3h_emb_bind(b, s);
    unpin(rd<uint64_t>(e, 0x78));
    u3h_emb_bind(b, s);
    ++ext_ec()[127];
    if (e[0]) {
        u3x_emblem_editor(b, s);
        ++ext_ec()[146];
    }
    return 1;
}
// NetShutdown: the editor records go
extern "C" U3X_FN void u3x_emblem_shutdown(uintptr_t b) {
    if (emblems_off()) return;
    uint8_t *e = emblem_manager(b);
    render_flag() = 0;
    for (int k = 0; k < 2; ++k) {
        const uint64_t r = rd<uint64_t>(e, 0x68 + 8 * k);
        if (r && u3h_emb_is_record(r)) {
            *reinterpret_cast<uint16_t *>(r + 0x24) = 0;
            u3h_emb_free_record(b, r);
        }
        *reinterpret_cast<uint64_t *>(e + 0x68 + 8 * k) = 0;
    }
    ++ext_ec()[128];
}

// ===== v7.180: third split, part 1 (the game packets' reliable header) =====

extern "C" {
U3H_IMPORT void u3h_gt_stamp(uintptr_t base, uint8_t *sub, int off);
U3H_IMPORT void u3h_gt_free(uintptr_t base, void *p);
U3H_IMPORT volatile uint32_t *u3h_bc_state(int which);
}

// b012: each game connection (PS4 sub-object at entry +0x40: out seq +8, in seq +0xc, ack bits +0x14, last acked +0x18,
// the 32 sent-record slots +0x1c, in-flight records +0x100..+0x108 {data id, seq +4, next +8, events +0x10}) writes a
// header {reliable 1 bit, seq 11 bits, ack 10 bits, ack bits 32}; a reliable packet is sent only while fewer than 31
// are in flight. Reading a header acks / loses the in-flight records up to the ack. eb[101] reliable writes, [118]
// unreliable writes, [102] reads, [103] acked, [104] lost, [116] expired events dropped.
namespace {
U3X_FN int64_t sub_call(void *sub, int slot, void *a, void *b) {
    return reinterpret_cast<int64_t (*)(void *, void *, void *)>((*static_cast<uintptr_t **>(sub))[slot])(sub, a, b);
}
U3X_FN int32_t game_ticks(uintptr_t base) { return rd<int32_t>(at<const uint8_t *>(base + 0x1b24b98), 0x114); }
}  // namespace
extern "C" U3X_FN int64_t u3x_gt_vcall(void *sub, int slot, void *a, void *b) { return sub_call(sub, slot, a, b); }
extern "C" U3X_FN void u3x_gt_header(const uint8_t *sub, void *b, uint32_t flag) {
    GtStream *s = static_cast<GtStream *>(b);
    u3x_gt_write(s, flag, 1);
    u3x_gt_write(s, rd<uint32_t>(sub, 8) & 0x7ff, 11);
    u3x_gt_write(s, rd<uint32_t>(sub, 0xc) & 0x3ff, 10);
    u3x_gt_write(s, rd<uint32_t>(sub, 0x14), 32);
}
extern "C" U3X_FN void u3x_gt_base_write(uintptr_t base, uint8_t *sub, void *b) {
    uint32_t out = rd<uint32_t>(sub, 8);
    uint32_t *eb = ext_eb();
    if (out - rd<uint32_t>(sub, 0x18) < 0x1f) {
        ++out;
        *reinterpret_cast<uint32_t *>(sub + 8) = out;
        u3x_gt_header(sub, b, 1);
        *reinterpret_cast<uint32_t *>(sub + 0x1c + (out & 0x1f) * 4) = rd<uint32_t>(sub, 0xc);
        u3h_gt_stamp(base, sub, 0xa4);
        uint8_t *rec = reinterpret_cast<uint8_t *(*)(void *)>((*reinterpret_cast<uintptr_t **>(sub))[0])(sub);
        if (!rec) return;
        *reinterpret_cast<uint32_t *>(rec + 4) = out;
        if (!rd<uint8_t *>(sub, 0x100)) *reinterpret_cast<uint8_t **>(sub + 0x100) = rec;
        else *reinterpret_cast<uint8_t **>(rd<uint8_t *>(sub, 0x108) + 8) = rec;
        *reinterpret_cast<uint8_t **>(sub + 0x108) = rec;
        *reinterpret_cast<uint32_t *>(rec) = rd<uint32_t>(sub, 0xa0);
        *reinterpret_cast<uint8_t **>(rec + 8) = nullptr;
        sub_call(sub, 2, b, rec);
        ++eb[101];
    } else {
        u3x_gt_header(sub, b, 0);
        ++eb[118];
    }
}
// 1 for a fresh reliable packet (its payload is read next); 0 for an old, unreliable or impossible one
extern "C" U3X_FN int u3x_gt_base_header(uintptr_t base, uint8_t *sub, void *b) {
    GtStream *st = static_cast<GtStream *>(b);
    const uint32_t flag = u3x_gt_read(st, 1), seq = u3x_gt_read(st, 11), ack = u3x_gt_read(st, 10);
    const uint32_t in = rd<uint32_t>(sub, 0xc);
    uint32_t s = seq | (in & 0xfffff800u);
    if (s < in) s += 0x800;
    if (s - in >= 0x20) return 0;
    const uint32_t last = rd<uint32_t>(sub, 0x18);
    uint32_t a = ack | (last & 0xfffffc00u);
    if (a < last) a += 0x400;
    if (a > rd<uint32_t>(sub, 8)) return 0;
    const uint32_t bits = u3x_gt_read(st, 32);
    const bool fresh = s != in;
    const uint32_t shift = s - in;
    const uint32_t mine = (shift >= 32 ? 0 : rd<uint32_t>(sub, 0x14) << shift) | (fresh && flag ? 1u : 0u);
    *reinterpret_cast<uint32_t *>(sub + 0x14) = mine;
    uint32_t *eb = ext_eb();
    for (uint32_t k = a - last; k-- > 0;) {
        uint8_t *rec = rd<uint8_t *>(sub, 0x100);
        if (!rec) break;
        *reinterpret_cast<uint8_t **>(sub + 0x100) = rd<uint8_t *>(rec, 8);
        if (bits & (1u << k)) {
            sub_call(sub, 3, rec, nullptr);
            ++*reinterpret_cast<uint32_t *>(sub + 0xb0);
            ++eb[103];
        } else {
            sub_call(sub, 4, rec, nullptr);
            ++*reinterpret_cast<uint32_t *>(sub + 0xac);
            ++eb[104];
        }
        u3h_gt_free(base, rec);
    }
    *reinterpret_cast<uint32_t *>(sub + 0xc) = s;
    *reinterpret_cast<uint32_t *>(sub + 0x18) = a;
    return fresh && flag;
}
extern "C" U3X_FN int u3x_gt_base_read(uintptr_t base, uint8_t *sub, void *b) {
    u3h_gt_stamp(base, sub, 0xa8);
    const int r = u3x_gt_base_header(base, sub, b);
    volatile uint32_t &where = *u3h_bc_state(0), &reads = *u3h_bc_state(1);
    if (r) {
        where = 3;
        reads = reads + 1;
        sub_call(sub, 1, b, nullptr);
        ++ext_eb()[102];
    }
    where = 2;
    return r;
}
// drop expired events from the unreliable queue (+0x120, next +0x20; event vt slot 7 "expired")
extern "C" U3X_FN void u3x_gt_drop_expired(uintptr_t base, uint8_t *sub) {
    uint8_t *prev = nullptr, *n = rd<uint8_t *>(sub, 0x120);
    while (n) {
        uint8_t *next = rd<uint8_t *>(n, 0x20), *ev = rd<uint8_t *>(n, 0);
        if (ev && (virtual_call<int64_t>(ev, 7 * 8) & 0xff)) {
            if (rd<uint8_t *>(sub, 0x120) == n) *reinterpret_cast<uint8_t **>(sub + 0x120) = next;
            else *reinterpret_cast<uint8_t **>(prev + 0x20) = next;
            if (--ev[10] == 0) virtual_call<void>(ev, 8);
            if (rd<uint8_t *>(n, 0x10)) u3h_gt_free(base, rd<uint8_t *>(n, 0x10));
            u3h_gt_free(base, n);
            ++ext_eb()[116];
            n = next;
            continue;
        }
        prev = n;
        n = next;
    }
    *reinterpret_cast<uint8_t **>(sub + 0x128) = prev;
}
// anything to send: an in-flight record still holding a non-type-2 event, the 0.5 s keep-alive, or queued events
extern "C" U3X_FN int u3x_gt_has_data(uintptr_t base, uint8_t *sub) {
    for (const uint8_t *rec = rd<const uint8_t *>(sub, 0x100); rec; rec = rd<const uint8_t *>(rec, 8))
        for (const uint8_t *n = rd<const uint8_t *>(rec, 0x10); n; n = rd<const uint8_t *>(n, 0x20)) {
            const uint8_t *ev = rd<const uint8_t *>(n, 0);
            if (ev && ev[9] != 2) return 1;
        }
    if (rd<int32_t>(sub, 0x164) <= game_ticks(base)) {
        *reinterpret_cast<int32_t *>(sub + 0x164) = game_ticks(base) + 150;
        return 1;
    }
    return rd<uint8_t *>(sub, 0x110) || rd<uint8_t *>(sub, 0x120);
}

// ===== v7.181: third split, part 2 (the respawn state machine) =====

extern "C" {
U3H_IMPORT int u3h_np_has_effect(void *s, uint8_t *p, uint32_t h);
U3H_IMPORT uint8_t *u3h_screen_player(uintptr_t base, uint8_t *p);
U3H_IMPORT int u3h_net_late_joining(uintptr_t base);
U3H_IMPORT void u3h_spawn_fade(uintptr_t base, void *s, uint8_t *p, int on);
U3H_IMPORT int u3h_spawn_locator(uintptr_t base, void *s, uint8_t *p, void *loc, uint32_t spawner, uint32_t index);
U3H_IMPORT void u3h_spawn_region_request(uintptr_t base, void *s, uint8_t *p, uint32_t spawner);
U3H_IMPORT int u3h_spawn_levels_ready(uintptr_t base, void *s, uint32_t spawner);
U3H_IMPORT int32_t u3h_netgame_type(uintptr_t base, void *s);
U3H_IMPORT int u3h_vis_oob_tick(uintptr_t base, void *s, uint8_t *p, uint8_t *pl, int32_t now, int test);
U3H_IMPORT void u3h_camera_tick(uintptr_t base, void *s, uint8_t *p, int idx);
}

// c002 / v4.86: the per-frame respawn handling of a local net player (PS3 1.19 f_363ed4 up to its camera half). State
// at state +0x3d40 (RS) and +0x3d78 (RS2; RS2[0] holds the debug switches). ec[0] bit 1 keeps the 1.10 late-join fade.
namespace {
U3X_RO const uint8_t hud_request_prologue[12] = {0x09, 0x77, 0x08, 0x8b, 0x47, 0x38, 0x85, 0xc0, 0x74, 0x4e, 0x48, 0x8d};
uint32_t *respawn_state(void *s) { return reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d40); }
uint32_t *respawn_state2(void *s) { return reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78); }
}  // namespace
// 1.19's late join: in Elimination a player with +0x5fa set gets fade 0 once 5 s of the round have passed
extern "C" U3X_FN int u3x_late_join_fade(uintptr_t base, void *s, const uint8_t *p) {
    uint32_t *ec = ext_ec();
    if (ec[0] & 2) return 1;
    if (u3h_netgame_type(base, s) != 10 || !p[0x5fa]) return 1;
    const uint8_t *c = at<const uint8_t *>(base + 0x17c3580);
    if (!c) return 1;
    const int32_t dt = rd<int32_t>(c, 0xc) - at<int32_t>(base + 0x17cb060 + 0x66e8);
    ++ec[17];
    return dt >= 1500 ? 0 : 1;
}
// without a Player: the late-join fade, or the spawn steps (0 region request, 1 wait for the spawner's levels, 2 spawn
// at the spawner through PS4's spawn body with force, the two times it would overwrite restored); with a Player: the
// HUD-alive request, the out-of-bounds kill timer (+0x848), the player state sends, and the respawn of a dead player
// past its delay (the process is killed); then the camera half
extern "C" U3X_FN void u3x_respawn_tick(uintptr_t base, void *s, uint8_t *p, int idx) {
    uint32_t *d = respawn_state(s), *rs2 = respawn_state2(s);
    if (!(state_switches(s) & 1) && u3h_np_alive(s, p) && code_starts(base, 0x11b7d60, hud_request_prologue)) {
        const int32_t screen = rd<int32_t>(p, 0x1e8);
        if (screen >= 0 && screen < 2) {
            game_call<void>(base + 0x11b7d60, reinterpret_cast<uint8_t *>(base + 0x1805fa0) + static_cast<int64_t>(screen) * 0x270, 0x100000u);
            ++reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x2040)[22];
        }
    }
    if (u3h_np_has_effect(s, p, 0xc054e78bu)) return;
    if (p[0x60]) return;
    const int32_t now = at<int32_t>(base + 0x17b4ca4);
    const uint32_t delay = u3h_respawn_delay(s, p);
    d[10] = delay;
    d[11] = static_cast<uint32_t>(now);
    const int32_t t344 = rd<int32_t>(p, 0x39c), t350 = rd<int32_t>(p, 0x3a8), team = rd<int32_t>(p, 0x21c);
    if (!u3h_screen_player(base, p)) {
        d[2 + idx] = 0x100 | rd<uint32_t>(p, 0x7c0);
        if (u3h_net_late_joining(base) && !t350 && (!u3h_net_coop_mode(base, s) || team != 1)) {
            u3h_spawn_fade(base, s, p, u3x_late_join_fade(base, s, p));
        } else if (t350 < t344 || p[0x61] || (t350 < 1 && u3h_net_coop_mode(base, s) && team == 1)) {
            u3h_spawn_fade(base, s, p, 0);
        } else {
            const uint32_t spawner = rd<uint32_t>(p, 0x3b4);
            alignas(16) uint32_t loc[16];
            for (uint32_t &w : loc) w = 0;
            if (!u3h_spawn_locator(base, s, p, loc, spawner, rd<uint32_t>(p, 0x424))) {
                ++d[9];
            } else {
                int32_t &step = *reinterpret_cast<int32_t *>(p + 0x7c0);
                if (step == 0) {
                    u3h_spawn_region_request(base, s, p, spawner);
                    u3h_spawn_fade(base, s, p, 1);
                    step = 1;
                } else if (step == 1) {
                    u3h_spawn_fade(base, s, p, 1);
                    if ((u3h_netgame_type(base, s) == 0xb && !p[0x5f9]) || u3h_spawn_levels_ready(base, s, spawner)) step = 2;
                } else if (step == 2) {
                    u3h_spawn_fade(base, s, p, 0);
                    p[0x420] = 0;
                    *reinterpret_cast<uint8_t *>(d) = 1;
                    const uint8_t r = reinterpret_cast<uint8_t (*)(uint8_t *, uint32_t, int)>(state_native(s, 53))(p, spawner, 1);
                    *reinterpret_cast<uint8_t *>(d) = 0;
                    *reinterpret_cast<int32_t *>(p + 0x39c) = t344;
                    *reinterpret_cast<int32_t *>(p + 0x3a8) = t350;
                    p[0x420] = 0;
                    step = 0;
                    *reinterpret_cast<int32_t *>(p + 0x3a0) = at<int32_t>(base + 0x17b4ca4);
                    ++d[4];
                    d[6] = r;
                    rs2[8] = r;
                    ++rs2[9];
                }
            }
        }
        u3h_camera_tick(base, s, p, idx);
        return;
    }
    d[2 + idx] = 0x200 | rd<uint32_t>(p, 0x7c0);
    {   // the out-of-bounds kill
        uint8_t *pl = u3h_screen_player(base, p);
        int32_t *t = reinterpret_cast<int32_t *>(p + 0x848);
        uint32_t *sp = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3dc0);
        const uint32_t id = at<uint32_t>(base + 0x1bb9508 + static_cast<int64_t>(rd<int32_t>(p, 0x1e8)) * 0x10);
        if (pl && sp[2 + idx] != id) {   // a new life: its start time
            sp[2 + idx] = id;
            sp[idx] = static_cast<uint32_t>(now);
        }
        if (rs2[0] & 4) {
            if (pl && now - static_cast<int32_t>(sp[idx]) > 15 * 300) {
                if (*t <= 0) *t = now + 1500;
            } else {
                *t = 0;
            }
        } else if (pl) {
            u3h_vis_oob_tick(base, s, p, pl, now, (rs2[0] & 0x20) && now - static_cast<int32_t>(sp[idx]) > 20 * 300);
        }
        if (pl && *t > 0 && *t <= now && u3h_np_alive(s, p)) {
            uint8_t *o = rd<uint8_t *>(pl, 0x598);
            if (o) {
                virtual_call<void>(o, 0x78);
                uint8_t *r = virtual_call<uint8_t *>(o, 0x118);
                if (r) {
                    *reinterpret_cast<uint64_t *>(r + 0x14) = 0x11a89844u;
                    virtual_call<void>(pl, 0x530);
                    ++rs2[10];
                    if (!(rs2[0] & 4)) ++reinterpret_cast<uint32_t *>(u3h_ext() + 0x2070)[9];
                }
            }
            *t = 0;
        }
    }
    u3x_replication(base, s, p, u3h_screen_player(base, p));
    if (!u3h_np_alive(s, p)) {
        const int32_t since = now - t344;
        const bool positive = !(delay >> 31) && (delay & 0x7fffffff) && ((delay >> 23) & 0xff) != 0xff;
        if (positive && (t344 < t350 || u3h_f_ticks(delay, 2) < since) && u3h_f_ticks(delay, 1) < since) {
            uint8_t *o = handle_object(rd<uint8_t **>(p, 0x10), rd<uint32_t>(p, 0x18));
            if (o) {
                reinterpret_cast<void (*)(uint8_t *)>(state_native(s, 57))(o);
                ++d[5];
            }
        }
        u3h_camera_tick(base, s, p, idx);
        return;
    }
    u3h_spawn_fade(base, s, p, 0);
    u3h_camera_tick(base, s, p, idx);
}

// ===== v7.182: third split, part 3 (Find Game: game type -> map -> net game records) =====

extern "C" {
U3H_IMPORT int u3h_m04_lan(uintptr_t base);
U3H_IMPORT volatile uint32_t *u3h_m04_diag(void);
}

// m004: the NetMenuHost table (ExtState +0xed000): count, then per game type (11 slots of 0x308 bytes: type, map count
// +4, maps from +0x10 of 0x18 bytes {map entry, count +8, record index low bytes +9 (8)}); the record index high bytes
// at +0xef200 ([11][32][8]). eb[2] bit 15 turns the 1.19 table off. m04_d: [0] refreshes, [1] types, [2] maps,
// [5] indices >= 256, [6] subtitle picks.
namespace {
U3X_FN uint8_t *nmh() { return u3h_ext() + 0xed000; }
U3X_FN uint8_t *nmh_high() { return u3h_ext() + 0xef200; }
U3X_FN bool m04_on() { return !(ext_eb()[2] & 0x8000u); }
}  // namespace
// the map entry of DC 0xdec97a4 with that id (0x98 bytes)
extern "C" U3X_FN uint8_t *u3x_m04_map(void *s, uint32_t id) {
    const uint8_t *t = dc_table(s, 0x0dec97a4u);
    if (!t) return nullptr;
    uint8_t *e = rd<uint8_t *>(t, 8);
    if (!e) return nullptr;
    for (int32_t i = 0; i < rd<int32_t>(t, 0); ++i)
        if (rd<uint32_t>(e, i * 0x98) == id) return e + static_cast<int64_t>(i) * 0x98;
    return nullptr;
}
// one game type's maps: each map once (not the hidden ones, map +0x94), up to 32, with up to 8 record indices each
extern "C" U3X_FN void u3x_m04_info(void *s, uint8_t *info, uint32_t type, const uint8_t *rec, int32_t n, uint8_t *hi) {
    volatile uint32_t *d = u3h_m04_diag();
    *reinterpret_cast<uint32_t *>(info) = type;
    info[4] = 0;
    for (int32_t i = 0; i < n; ++i) {
        const uint8_t *r = rec + static_cast<int64_t>(i) * 0xd0;
        if (rd<uint32_t>(r, 0xc) != type) continue;
        const uint32_t mid = rd<uint32_t>(r, 4);
        bool dup = false;
        for (uint32_t k = 0; k < info[4]; ++k) {
            const uint8_t *me = rd<const uint8_t *>(info, 8 + k * 0x18);
            if (me && rd<uint32_t>(me, 0) == mid) { dup = true; break; }
        }
        if (dup) continue;
        uint8_t *map = u3x_m04_map(s, mid);
        if (map && rd<uint32_t>(map, 0x94)) continue;
        if (info[4] >= 32) break;
        uint8_t *m = info + 8 + info[4] * 0x18;
        *reinterpret_cast<uint8_t **>(m) = map;
        m[8] = 0;
        if (!map) continue;
        for (int32_t j = 0; j < n && j < 0x10000; ++j) {
            const uint8_t *q = rec + static_cast<int64_t>(j) * 0xd0;
            if (rd<uint32_t>(q, 0xc) == type && rd<uint32_t>(q, 4) == mid && m[8] < 8) {
                m[9 + m[8]] = static_cast<uint8_t>(j);
                hi[info[4] * 8 + m[8]] = static_cast<uint8_t>(j >> 8);
                ++m[8];
                if (j >= 256) d[5] = d[5] + 1;
            }
        }
        ++info[4];
        d[2] = d[2] + 1;
    }
}
// the table again: the game types of the net games (LAN: co-op types only in co-op, the others only in competitive;
// never 0x10), at most 11, with Elimination (10) right after type 19 as in the 1.19 data
extern "C" U3X_FN void u3x_m04_refresh(uintptr_t base, void *s) {
    volatile uint32_t *d = u3h_m04_diag();
    uint8_t *t = nmh();
    t[0] = 0;
    d[0] = d[0] + 1;
    const uint8_t *ng = dc_table(s, 0x3b9a067du);
    if (!ng) return;
    const int32_t n = rd<int32_t>(ng, 0);
    const uint8_t *rec = rd<const uint8_t *>(ng, 8);
    if (!rec || n <= 0) return;
    const bool lan = u3h_m04_lan(base) != 0, coop = at<uint8_t>(base + 0x17d6065) != 0;
    uint32_t types[11];
    uint32_t nt = 0;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t ty = rd<uint32_t>(rec, i * 0xd0 + 0xc);
        if (lan) {
            const bool coop_type = ty == 0x11u || ty == 0xbu || ty == 8u;
            if (coop_type != coop) continue;
        }
        if (ty == 0x10u) continue;
        bool dup = false;
        for (uint32_t k = 0; k < nt; ++k)
            if (types[k] == ty) dup = true;
        if (dup) continue;
        if (nt >= 11) break;
        types[nt++] = ty;
    }
    {
        int p10 = -1, p19 = -1;
        for (uint32_t k = 0; k < nt; ++k) {
            if (types[k] == 10u) p10 = static_cast<int>(k);
            if (types[k] == 19u) p19 = static_cast<int>(k);
        }
        if (p10 > p19 + 1 && p19 >= 0) {
            for (int k = p10; k > p19 + 1; --k) types[k] = types[k - 1];
            types[p19 + 1] = 10u;
        }
    }
    d[5] = 0;
    for (uint32_t k = 0; k < nt; ++k) u3x_m04_info(s, t + 8 + k * 0x308, types[k], rec, n, nmh_high() + k * 0x100);
    t[0] = static_cast<uint8_t>(nt);
    d[1] = nt;
}
// the u16 record index of slot i of map j of game type k (the low byte only for another table or with the table off)
extern "C" U3X_FN int32_t u3x_m04_rec(const uint8_t *t, uint32_t k, uint32_t j, uint32_t i) {
    int32_t v = t[0x19 + k * 0x308 + j * 0x18 + i];
    if (m04_on() && t == nmh() && k < 11 && j < 32 && i < 8) v |= static_cast<int32_t>(nmh_high()[(k * 32 + j) * 8 + i]) << 8;
    return v;
}
extern "C" U3X_FN uint32_t u3x_m04_cur_type(const uint8_t *t, const uint8_t *ng, int32_t cur) {
    const uint32_t nt = t[0];
    if (!ng || !nt) return 0;
    const uint8_t *rec = rd<const uint8_t *>(ng, 8);
    if (!rec || cur < 0 || cur >= rd<int32_t>(ng, 0)) return 0;
    const uint32_t ty = rd<uint32_t>(rec, cur * 0xd0 + 0xc);
    for (uint32_t k = 0; k < nt; ++k)
        if (rd<uint32_t>(t, 8 + k * 0x308) == ty) return k;
    return 0;
}
extern "C" U3X_FN uint32_t u3x_m04_cur_map(const uint8_t *t, uint32_t k, int32_t cur) {
    const uint32_t nm = t[0xc + k * 0x308];
    for (uint32_t j = 0; j < nm && j < 32; ++j) {
        const uint32_t c = t[0x18 + k * 0x308 + j * 0x18];
        for (uint32_t i = 0; i < c && i < 8; ++i)
            if (u3x_m04_rec(t, k, j, i) == cur) return j;
    }
    return 0;
}
// pick subtitle v of the selected net game's map: the record of the same type and map with that subtitle (0x7511f0)
extern "C" U3X_FN void u3x_m04_select_var(uintptr_t base, void *s, int32_t v) {
    const uint8_t *t = nmh(), *ng = dc_table(s, 0x3b9a067du);
    if (!ng) return;
    const uint8_t *rec = rd<const uint8_t *>(ng, 8);
    const int32_t n = rd<int32_t>(ng, 0), cur = at<int16_t>(base + 0x17cb060 + 0x661c);
    if (!rec || cur < 0 || cur >= n) return;
    const uint32_t k = u3x_m04_cur_type(t, ng, cur), j = u3x_m04_cur_map(t, k, cur);
    const uint32_t c = t[0x18 + k * 0x308 + j * 0x18];
    if (j >= t[0xc + k * 0x308] || v < 0 || static_cast<uint32_t>(v) >= c || v >= 8) return;
    const int32_t idx = u3x_m04_rec(t, k, j, static_cast<uint32_t>(v));
    if (idx >= n) return;
    uint32_t key[3];
    key[0] = rd<uint32_t>(rec, cur * 0xd0 + 0xc);
    key[1] = rd<uint32_t>(rec, cur * 0xd0 + 4);
    key[2] = rd<uint32_t>(rec, idx * 0xd0 + 8);
    game_call<void>(base + 0x7511f0, static_cast<void *>(key));
    volatile uint32_t *d = u3h_m04_diag();
    d[6] = d[6] + 1;
    u3x_m04_refresh(base, s);
}
// the row of the selected net game in its map's subtitle list (+1 solo); scans every entry as PS3 does
extern "C" U3X_FN uint32_t u3x_m04_var_value(uintptr_t base, int solo) {
    const uint8_t *t = nmh();
    const int32_t cur = at<int16_t>(base + 0x17cb060 + 0x661c);
    const uint32_t b = solo ? 1u : 0u;
    for (uint32_t k = 0; k < t[0] && k < 11; ++k) {
        const uint32_t nm = t[0xc + k * 0x308];
        for (uint32_t j = 0; j < nm && j < 32; ++j) {
            const uint32_t c = t[0x18 + k * 0x308 + j * 0x18];
            if (!c) continue;
            for (uint32_t i = 0; i < c && i < 8; ++i)
                if (u3x_m04_rec(t, k, j, i) == cur) return i + b;
        }
    }
    return b;
}

// ===== v7.183: third split, part 4 (the lobby menu list and the party list) =====

extern "C" {
U3H_IMPORT int u3h_profile_new_marker(uintptr_t base, int32_t *ev);
U3H_IMPORT const char *u3h_onp_id(void *s);
U3H_IMPORT int u3h_onm_players(uintptr_t base, int32_t *ev, void *s, const char *name);
U3H_IMPORT int u3h_i01_party(uintptr_t base, int32_t *ev, void *s, const char *name);
}

// LIST_MAIN_MENU (0xb3): the lobby rows {text, icon, action}: no title, row text, row icon (and the Profile row's "!"
// new-content marker; every other icon request answers no icon), selection (state +692), last row; op 11 queries
// answer 0 as PS3 (FUNCTIONS bit 2: the old 1). LIST_PLAYERS (0xad): the party: the real matchmaking's members, the
// spoofed matchmaking list, the 1.19 party list (eb[1] bit 16 off), else the interim view (our profile with "[tag]",
// party members by row, our rank badge, the custom-game row colour). Icon / colour diagnostics in sw / rb / fsw and at
// state +0x2300.
namespace {
U3X_RO const char party_title[] = "PARTY", tag_open[] = "[", tag_close[] = "] ";
U3X_FN uint32_t &lobby_selected(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 692); }
}  // namespace
extern "C" U3X_FN int u3x_lobby_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source) return 0;
    const uint32_t tag = rd<uint32_t>(source, 0x1c);
    if (tag != 0x4c414eb3 && tag != 0x4c414ead) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    const uint32_t index = static_cast<uint32_t>(ev[1]);
    uint32_t *fsw = reinterpret_cast<uint32_t *>(u3h_ext() + 0x3a70), *sw = reinterpret_cast<uint32_t *>(u3h_ext() + 0x2130),
             *rb = reinterpret_cast<uint32_t *>(u3h_ext() + 0x31b0);
    if (tag == 0x4c414eb3) {
        uint32_t rows;
        const uint32_t *lan_rows = u3h_lobby_rows(base, &rows);   // {text, icon, action}
        switch (ev[0]) {
        case 1: set_row_text(ev, empty_text); break;
        case 2:
            if (ev[0x15] == 0 && index < rows) set_row_text(ev, u3h_text(base, s, lan_rows[index * 3]));
            break;
        case 4: {
            uint32_t *dg = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x2300);
            ++dg[0];
            dg[1] = static_cast<uint32_t>(ev[0x14]);
            dg[2] = index;
            const uint8_t *ui = at<const uint8_t *>(base + 0x1b24bc8);
            const uint8_t *t = ui ? rd<const uint8_t *>(ui, 0x28) : nullptr;
            dg[4] = t ? rd<uint16_t>(t, 2) : 0xffffffffu;
            if (index < rows) dg[3] = static_cast<uint32_t>(u3h_icon_index(base, lan_rows[index * 3 + 1]));
            dg[5 + (dg[0] & 7)] = static_cast<uint32_t>(ev[0x14]);
            if (static_cast<uint32_t>(ev[0x14]) == 0xf5a294f3u && index < rows) u3h_set_icon_plain(base, ev, lan_rows[index * 3 + 1]);
            else if (ev[0x14] == 0 && index < rows && lan_rows[index * 3 + 2] == 0xc4acc827u && u3h_profile_new_marker(base, ev)) {}
            else ev[2] = 0;
            break;
        }
        case 5: ev[1] = static_cast<int32_t>(lobby_selected(s)); break;
        case 6: ev[1] = static_cast<int32_t>(rows) - 1; break;
        case 7: ev[1] = 0; break;
        case 10:
            if (index < rows) lobby_selected(s) = index;
            break;
        case 11:
            fsw[1] = static_cast<uint32_t>(ev[0x13]);
            ++fsw[2];
            if (fsw[0] & 4) ev[14] = index < rows;
            break;
        case 23:
            if (lobby_selected(s) >= rows) lobby_selected(s) = 0;
            break;
        }
        return 1;
    }
    at<uint32_t>(base + 0x1b68a80) = 0;   // the party model row: player 0, local
    at<uint8_t>(base + 0x1b68a84) = 1;
    const char *name = empty_text;
    const uint32_t selected = profile_selected(s), count = profile_count(s);
    if (ev[0] == 1) {
        name = party_title;
    } else if (selected && selected <= count) {
        name = u3h_onp_id(s);
        const uint8_t *pr = player_profile(base);
        if (pr && pr[0xac0]) {   // "[tag] name"
            char *o = reinterpret_cast<char *>(static_cast<uint8_t *>(s) + 0x2b80), *end = o + 100;
            char *const start = o;
            o = append_chars(o, end, tag_open);
            for (int k = 0; k < 4 && pr[0xac0 + k] && o < end; ++k) *o++ = static_cast<char>(pr[0xac0 + k]);
            o = append_chars(o, end, tag_close);
            o = append_chars(o, end, name);
            *o = 0;
            name = start;
        }
    }
    if (u3h_onm_players(base, ev, s, name)) return 1;
    if (u3x_m03_players(base, ev, s, name)) return 1;
    if (!(ext_eb()[1] & 0x10000u) && u3h_i01_party(base, ev, s, name)) return 1;
    if (ev[0] != 1 && index > 0) {
        const char *rn = u3x_lan_row_name(base, static_cast<int>(index));
        name = rn ? rn : empty_text;
    }
    if (ev[0] == 4) {
        ++sw[8];
        sw[9] = static_cast<uint32_t>(ev[0x14]);
        sw[10] = index;
        const uint32_t slot = static_cast<uint32_t>(ev[0x14]);
        bool seen = false;
        for (int k = 0; k < 4; ++k)
            if (sw[11 + k] == slot) seen = true;
        if (!seen)
            for (int k = 0; k < 4; ++k)
                if (!sw[11 + k]) { sw[11 + k] = slot; break; }
    }
    if (ev[0] == 4 && static_cast<uint32_t>(ev[0x14]) == 0xf5a294f3u && index == 0) {   // our rank badge
        const uintptr_t rank = state_native(s, 21);
        uint8_t *pr = player_profile(base);
        sw[15] = (rank ? 1u : 0u) | (pr ? 2u : 0u) | ((rank && pr) ? (reinterpret_cast<uint32_t (*)(void *)>(rank)(pr) << 8) : 0u);
        if (rank && pr) {
            ev[2] = 0x29;
            *reinterpret_cast<uint64_t *>(ev + 4) = 0;
            ev[6] = static_cast<int32_t>(reinterpret_cast<uint32_t (*)(void *)>(rank)(pr));
            ev[7] = 0;
            ev[8] = 0;
            ev[9] = -1;
            ev[10] = 0;
            ev[11] = -1;
            ev[12] = 0x3f800000;
            ++rb[4];
            u3h_rank_icon(base, s, ev);
        }
        return 1;
    }
    if (ev[0] == 20 && at<uint32_t>(base + 0x17d603c) != 2) {   // row colour only in custom-game mode
        if (ev[0x16] == 2) return 1;
    } else if (ev[0] == 20 && ev[0x16] == 2 && !(*reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78) & 0x2000000) && index < 5) {
        uint32_t out[8];
        for (uint32_t &w : out) w = 0;
        game_call<void *>(base + 0x7418e0, static_cast<void *>(out), base + 0x17cb060, static_cast<int>(index), 0);
        ev[15] = static_cast<int32_t>((out[3] & 0xffffffu) | 0x80000000u);
        rb[7] += 0x10000;
        return 1;
    }
    switch (ev[0]) {
    case 1: set_row_text(ev, name); break;
    case 2: set_row_text(ev, ev[0x15] == 0 ? name : empty_text); break;
    case 5: ev[1] = 0; break;
    case 6:
        ev[1] = u3x_lan_rows(base) - 1;
        u3x_clan_tag_poll(base, s);
        u3h_hud_alias_names(base, s);
        u3h_stage_tick(base, s);
        u3h_loadout_tick(base, s);
        break;
    case 7: ev[1] = 0; break;
    case 21: ev[1] = 0; break;
    case 11: ev[14] = index == 0; break;
    }
    return 1;
}

// ===== v7.184: third split, part 5 (leaving multiplayer and the lobby stage) =====

extern "C" {
U3H_IMPORT void *u3h_onb_mgr(uintptr_t base);
U3H_IMPORT void u3h_net_shutdown_parts(uintptr_t base, void *s);
U3H_IMPORT void u3h_multi_all(uintptr_t base, void *s, int keep);
U3H_IMPORT int u3h_stage_logout_hold(uintptr_t base, void *s, const char *stage);
U3H_IMPORT void u3h_match_stage_cleanup(uintptr_t base, void *s, void *find, void *showl);
}

// v5.60 / t003: PS3 leaves multiplayer through NetShutdown: clear the net game's LAN and multiplayer-active bytes
// (0x17d5c60 / 0x17d5c61), drop the lobby stage and want the boot levels again. It runs when a page before NetInit is
// shown (root tiles, title, MP choice, co-op choice) and from "Leave Multiplayer?" Yes (value stub 130 at 0x66e520, then
// PS4's ShowPage). Not during the NetInit work or in a match; state +0x20c0 bit 8 (U3MP_NET_SHUTDOWN=0) keeps the flags.
// The lobby stage (state +0x3e10): with MP active the stage named at mgr +0x74b8 (else the retail City of Brass stage)
// is wanted, made visible and shown, and the lobby camera runs at its menu spawner once it is loaded.
namespace {
U3X_BSS uint32_t leave_mp_counts[2];   // [0] calls, [1] shutdowns run
U3X_RO const char retail_stage[] = "san-upper-multi-stage", proto_actor[] = "anim-proto-multiplayer";
U3X_FN uint32_t &startup_phase(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 680); }
U3X_FN bool shutdown_blocked(uintptr_t base, void *s) {
    return (state_switches(s) & 0x100) || startup_phase(s) == 1 || startup_phase(s) == 2 || at<uint32_t>(base + 0x17d1868) == 2;
}
}  // namespace
extern "C" U3X_FN int u3x_mp_lan_or_online(uintptr_t base) { return at<uint8_t>(base + 0x17d5c60) || u3h_onb_mgr(base); }
extern "C" U3X_FN void u3x_stage_tick(uintptr_t base, void *s) {
    uint32_t *st = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3e10);
    uint32_t *lob = reinterpret_cast<uint32_t *>(u3h_ext() + 0x1ff0);
    void *find = reinterpret_cast<void *>(state_native(s, 42));
    char *stage = reinterpret_cast<char *>(base + 0x17cb060 + 0x74b8);
    if (!u3x_mp_lan_or_online(base)) {   // MP over: the stage and multi-all go, the boot levels come back
        if (*st) {
            if (stage[0]) {
                lob[14] += static_cast<uint32_t>(u3h_stage_unwant(base, u3h_crc_sid(stage)));
                stage[0] = 0;
            }
            u3h_multi_all(base, s, 0);
            u3x_net_shutdown_levels(base, s);
            st[0] = 0;
            st[1] = 0;
        }
        uint64_t *tc = reinterpret_cast<uint64_t *>(static_cast<uint8_t *>(s) + 0x26c0);   // the title gets its camera back
        if (tc[1] && tc[0] && state_native(s, 52)) {
            tc[1] = 0;
            reinterpret_cast<void (*)(uint64_t)>(state_native(s, 52))(tc[0]);
        }
        return;
    }
    const uintptr_t hash = state_native(s, 40), request = state_native(s, 41), showl = state_native(s, 45);
    if (!find || !hash || !request || !showl) return;
    u3h_multi_all(base, s, 1);
    if (u3h_stage_logout_hold(base, s, stage)) return;
    if (at<uint32_t>(base + 0x17d1868) == 2) {   // in a match: other maps' stages out (RS2 0x400000 skips it)
        if (!(*reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78) & 0x400000)) u3h_match_stage_cleanup(base, s, find, reinterpret_cast<void *>(showl));
        return;
    }
    if (!(*st & 0x10000u) && state_native(s, 46)) {
        reinterpret_cast<void (*)(uintptr_t)>(state_native(s, 46))(base + 0x17bba90);
        *st |= 0x10000u;
    }
    if (!(*st & 0x20000u)) {
        u3x_actor_want(base, s, proto_actor);
        *st |= 0x20000u;
    }
    u3x_title_unwant(base, s);
    if (!stage[0]) {
        for (int j = 0; retail_stage[j]; ++j) stage[j] = retail_stage[j];
        stage[sizeof(retail_stage) - 1] = 0;
        *st |= 1u;
    }
    const uint32_t sid = u3h_crc_sid(stage);
    uint8_t *e = u3x_level_want_entry(base, sid);
    if (!e) {   // as 0x743420: {hash, sid, 0, flags 3}
        uint8_t req[0x18];
        for (uint8_t &b : req) b = 0;
        *reinterpret_cast<uint64_t *>(req) = reinterpret_cast<uint64_t (*)(const char *)>(hash)(stage);
        *reinterpret_cast<uint32_t *>(req + 8) = sid;
        req[0xe] = 3;
        reinterpret_cast<void (*)(uintptr_t, void *)>(request)(base + 0x1afcf10, req);
        e = u3x_level_want_entry(base, sid);
    }
    if (e && (e[0xe] & 3) != 3) {   // a stage first wanted hidden as a prefetch becomes visible
        e[0xe] |= 3;
        ++lob[12];
    }
    lob[13] = e ? e[0xe] : 0xffu;
    uint8_t *l = reinterpret_cast<uint8_t *(*)(uintptr_t, uint32_t)>(find)(base + 0x1afcf10, sid);
    const bool loaded = l && rd<uint32_t>(l, 0x30) == 8;
    if (loaded && st[1] != sid) {
        reinterpret_cast<void (*)(void *, int)>(showl)(l, 1);
        l[2] |= 0x10;
    }
    st[4] = l ? (rd<uint16_t>(l, 2) | (rd<uint32_t>(l, 0x30) << 16)) : 0xffffffffu;
    ++st[5];
    if (l && rd<uint32_t>(l, 0x30) == 8 && state_native(s, 47)) {   // read again after the show
        if (ext_eb()[1] & 8) reinterpret_cast<void (*)(uintptr_t)>(state_native(s, 47))(base + 0x17bba90);
        st[1] = sid;
    }
}
extern "C" U3X_FN void u3x_net_shutdown_do(uintptr_t base, void *s) {
    u3h_net_shutdown_parts(base, s);
    at<uint8_t>(base + 0x17d5c61) = 0;
    at<uint8_t>(base + 0x17d5c60) = 0;
    startup_phase(s) = 0;
    ++*reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x20c4);
    u3x_stage_tick(base, s);   // the page tick that normally runs it is gone on these pages
}
extern "C" U3X_FN void u3x_net_shutdown_tick(uintptr_t base, void *s) {
    if (!base || !u3x_mp_lan_or_online(base) || shutdown_blocked(base, s)) return;
    const uint32_t pg = u3h_active_page(base);
    if (pg != 0xd45dfccau && pg != 0x37656388u && pg != 0x1e66a744u && pg != 0xb0e67b0eu) return;
    u3x_net_shutdown_do(base, s);
}
// 0: PS4's ShowPage(PAGE_ROOT_MAIN) runs next
extern "C" U3X_FN int u3x_leave_mp_yes(uintptr_t base, void *s) {
    ++leave_mp_counts[0];
    if (!u3x_mp_lan_or_online(base) || shutdown_blocked(base, s)) return 0;
    u3x_net_shutdown_do(base, s);
    ++leave_mp_counts[1];
    return 0;
}

// ===== v7.185: third split, part 6 (the co-op weapon list) =====

extern "C" {
U3H_IMPORT int u3h_j11_rowimg(int32_t *ev, uint32_t weapon);
}

// Co-op weapons: DC symbol 0x556e3fb5 {count, entries of 0x18: +0 id, +8 mods (0x14 each, +0 mod bit), +0x10 count};
// weapon defs: script symbol 0x61d589d9, an open-addressed hash {u32 size, keys +8, defs +0x10}, def +4 name. The mod
// name / description ids by mod bit: PS4 has them only as immediates inside 0x7680f0, so they stay listed here.
namespace {
U3X_RO const uint32_t mod_names[13] = {0x9fe22c98, 0xeebca10f, 0x317982c3, 0x868a039f, 0xcf42ac41, 0x75ac8784, 0x2cb4e03f,
                                       0xca8044df, 0x90a93da0, 0x2be72ef2, 0xc2fb6973, 0xb6ab5db4, 0x815211c8};
U3X_RO const uint32_t mod_descs[13] = {0x00ac24a8, 0xaa3e1660, 0xe2fa14a6, 0x2d14e5b5, 0x6ee89d56, 0xd099ee2f, 0xb3fae80f,
                                       0x611ea2f5, 0x3b37db8a, 0x8079c8d8, 0x67ce00d8, 0xe5f1eb97, 0xefad05da};
U3X_RO const char money_colour[] = "|@Cff00ff00|";
// the id of mod bit `bit` in `ids`, or `fallback` for no single known bit
U3X_FN uint32_t mod_text(const uint32_t *ids, uint32_t bit, uint32_t fallback) {
    for (int i = 0; i < 13; ++i)
        if (bit == 1u << i) return ids[i];
    return fallback;
}
// a number, optionally with a comma every three digits
U3X_FN char *append_number(char *o, char *end, int64_t v, bool commas) {
    char digits[24];
    int n = 0;
    uint64_t u = v < 0 ? static_cast<uint64_t>(-v) : static_cast<uint64_t>(v);
    do {
        if (commas && n && n % 4 == 3) digits[n++] = ',';
        digits[n++] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u && n < 22);
    if (v < 0 && o < end) *o++ = '-';
    while (n && o < end) *o++ = digits[--n];
    return o;
}
U3X_FN int32_t &coop_selected(void *s) { return *reinterpret_cast<int32_t *>(static_cast<uint8_t *>(s) + 0x3900); }
}  // namespace
extern "C" U3X_FN uint8_t *u3x_coop_weapon(void *s, uint32_t i) {
    uint8_t *t = dc_table(s, 0x556e3fb5u);
    if (!t || i >= rd<uint32_t>(t, 0)) return nullptr;
    return rd<uint8_t *>(t, 8) + static_cast<uintptr_t>(i) * 0x18;
}
extern "C" U3X_FN int32_t u3x_coop_weapon_count(void *s) {
    const uint8_t *t = dc_table(s, 0x556e3fb5u);
    return t ? rd<int32_t>(t, 0) : 0;
}
extern "C" U3X_FN uint8_t *u3x_weapon_def(void *s, uint32_t id) {
    const uintptr_t lookup = state_native(s, 13);
    const uint32_t *h = lookup ? reinterpret_cast<uint32_t *(*)(uint32_t)>(lookup)(0x61d589d9u) : nullptr;
    if (!h || !h[0]) return nullptr;
    const uint32_t *keys = rd<const uint32_t *>(h, 8);
    uint8_t *const *defs = rd<uint8_t *const *>(h, 0x10);
    const uint32_t start = id % h[0];
    uint32_t i = start;
    while (keys[i] != id) {
        if (!keys[i]) return nullptr;
        i = (i + 1) % h[0];
        if (i == start) return nullptr;
    }
    return defs[i];
}
extern "C" U3X_FN uint32_t u3x_mod_name(uint32_t bit) { return mod_text(mod_names, bit, 0x58840ecdu); }
extern "C" U3X_FN uint32_t u3x_mod_desc(uint32_t bit) { return mod_text(mod_descs, bit, 0xd90c7fa3u); }
// the icon of mod bit `bit`: DC symbol 0x36a4c63b {count, icons +8}, indexed by bit number
extern "C" U3X_FN uint32_t u3x_mod_icon(void *s, uint32_t bit) {
    const uint8_t *t = dc_table(s, 0x36a4c63bu);
    if (!t) return 0;
    for (uint32_t i = 0; i < rd<uint32_t>(t, 0) && i < 32; ++i)
        if (1u << i == bit) return rd<const uint32_t *>(t, 8)[i];
    return 0;
}
// 0 owned, 1 buyable, ...; 3 without the unlock natives
extern "C" U3X_FN int u3x_unlock_status(uintptr_t base, void *s, uint32_t user, uint32_t id, uint32_t mod) {
    const uintptr_t find = state_native(s, 24), status = state_native(s, 35);
    if (!find || !status) return 3;
    void *entry = reinterpret_cast<void *(*)(uintptr_t, int, int)>(find)(base + 0x17f3068, static_cast<int>(id), static_cast<int>(mod));
    return static_cast<uint8_t>(reinterpret_cast<int (*)(uintptr_t, uint32_t, void *)>(status)(base + 0x17f3068, user, entry));
}
// "<cash text>|@Cff00ff00|1,234" (the profile's money, +0x74c) in state +0x2c80
extern "C" U3X_FN void u3x_money_text(uintptr_t base, void *s, int32_t *ev) {
    const uint8_t *pr = player_profile(base);
    char *const buf = static_cast<char *>(s) + 0x2c80, *end = buf + 250;
    char *o = append_chars(buf, end, u3h_text(base, s, 0xba6d58f7u));
    o = append_chars(o, end, money_colour);
    o = append_number(o, end, pr ? rd<uint32_t>(pr, 0x74c) : 0, true);
    *o = 0;
    set_row_text(ev, buf);
}
// LIST_COOP_WEAPON (0xa6): weapon name rows, row gun image, the "new" lock overlay, the first two fitted mods' icons;
// picking a row stores the weapon (0x17d9998), refreshes the 3D preview and opens the co-op weapon mods page
extern "C" U3X_FN int u3x_coop_weapon_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414ea6u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 760) = base;
    const uint32_t user = local_user(base) & 3;
    if (!u3h_sym_ready(base, s, 0x556e3fb5u) || !u3h_sym_ready(base, s, 0x61d589d9u)) {
        if (ev[0] == 6) ev[1] = -1;
        return 1;
    }
    const int32_t row = ev[1];
    uint8_t *w;
    switch (ev[0]) {
    case 2:
        if (ev[0x15] == 0 && (w = u3x_coop_weapon(s, static_cast<uint32_t>(row)))) {
            const uint8_t *d = u3x_weapon_def(s, rd<uint32_t>(w, 0));
            if (d) set_row_text(ev, u3h_text(base, s, rd<uint32_t>(d, 4)));
        }
        break;
    case 4: {
        const uint32_t slot = static_cast<uint32_t>(ev[0x14]);
        if (slot == 0) {
            if ((w = u3x_coop_weapon(s, static_cast<uint32_t>(row)))) u3h_j11_rowimg(ev, rd<uint32_t>(w, 0));
        } else if (slot == 0xbb36238eu) {
            const uintptr_t is_new = state_native(s, 37);
            if ((w = u3x_coop_weapon(s, static_cast<uint32_t>(row))) && is_new &&
                reinterpret_cast<uint8_t (*)(uintptr_t, uint32_t, uint32_t)>(is_new)(base + 0x17f3068, user, rd<uint32_t>(w, 0)))
                u3h_lock_overlay(ev);
        } else if (slot == 0x5095aa91u || slot == 0x5454b726u) {
            const uint8_t *pr = player_profile(base);
            const uint32_t mask = pr ? pr[0x1534 + static_cast<int64_t>(row)] : 0;
            if (!mask || !(w = u3x_coop_weapon(s, static_cast<uint32_t>(row)))) break;
            uint32_t want = slot == 0x5095aa91u ? 0 : 1;
            const uint32_t n = rd<uint32_t>(w, 0x10);
            const uint8_t *mods = rd<const uint8_t *>(w, 8);
            for (uint32_t i = 0; mods && i < n && i < 8; ++i) {
                if (!(mask >> i & 1)) continue;
                if (want == 0) {
                    const uint32_t icon = u3x_mod_icon(s, rd<uint32_t>(mods, i * 0x14));
                    if (u3h_icon_index(base, icon) >= 0) u3h_set_icon(base, ev, icon);
                    break;
                }
                --want;
            }
        }
        break;
    }
    case 12:
        if (!(ext_ea()[0] & 32)) {
            ev[0x1f] = 0x43800000;
            ev[0x20] = 0x43000000;
        }
        break;
    case 5: ev[1] = coop_selected(s); break;
    case 6: ev[1] = u3x_coop_weapon_count(s) - 1; break;
    case 10: coop_selected(s) = row; break;
    case 0x11: ev[1] = 4; break;
    case 0x16:
        at<int32_t>(base + 0x17d9998) = ev[0x17];
        game_call<void>(base + 0x5b3e40, base + 0x17bba90, 0, 0, 1);
        at<int32_t>(base + 0x1b6ae68) = 0;
        if (state_show_page(s)) u3h_show(base, s, 0xba7ae3b4u, 0);
        break;
    }
    return 1;
}

// ===== v7.186: third split, part 7 (the multiplayer startup: NetInit) =====

extern "C" {
U3H_IMPORT int u3h_register_lobby_click(uintptr_t base);
U3H_IMPORT int u3h_onb_signin_wait(uintptr_t base, void *s);
U3H_IMPORT void u3h_ngm_init(uintptr_t base);
U3H_IMPORT void u3h_netinit_services(uintptr_t base, void *s);
U3H_IMPORT void u3h_lnb_netinit(uintptr_t base, void *s);
U3H_IMPORT int u3h_onb_netinit(uintptr_t base, void *s);
U3H_IMPORT void u3h_mp_profile_start(uintptr_t base, void *s);
U3H_IMPORT int u3h_onp_fail_take(void);
U3H_IMPORT int u3h_emb_netinit(uintptr_t base, void *s);
U3H_IMPORT int u3h_k43_gate(uintptr_t base, void *s);
}

// The PS3 NetInit sequence as the helper runs it. begin_startup (picking a profile): register the lobby click, phase 1,
// show the loading page 0x9e1863b1. startup_tick (each menu frame, not while the UI state is 7 or 8), phase 1: the
// online sign-in wait, the net.bin symbol 0xbd3bb3b9 (loaded through tag 0x12 if missing), the net pools, the net game's
// LAN and multiplayer-active bytes, "Give all Store Items" (0x17b2a94), the "net" level request, NetGameManager init,
// the title levels out, the profile name in net +0x3b8, film / friends / leaderboards / invites, the network allocators
// 0x14 / 0x15 backed by a general heap (state +0x20c0 bit 3 keeps them empty), the net clock (RS2 bit 31: a helper-owned
// copy of clock 3 at state +0x2000, else engine clock 7), the abort flag and match session reset, the MP character DC
// handle, the LAN or online backend, the profile load (a failed online download stops here with the failure dialog), the
// emblem tables; phase 2. Phase 2 waits for the "net" level, then shows the choice page and the lobby (phase 3).
// Errors (state +684): 1 no loading page, 2 no net.bin, 3 no pools, 4 no lobby page; phase 4 stops.
namespace {
U3X_RO const char net_level[] = "net";
U3X_RO const char profile_download_failed[] = "Play Online stopped (No connection found): the online profile could not be downloaded, state ";
U3X_RO const uint8_t heap_picks[4] = {9, 5, 7, 0x13};   // wrapped malloc first
U3X_FN uint32_t &startup_error(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 684); }
U3X_FN uint32_t &startup_ticks(void *s) { return *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 688); }
U3X_FN uintptr_t state_word(void *s, int off) { return *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + off); }
U3X_FN void **net_symbol(void *s) { return reinterpret_cast<void **(*)(uint32_t)>(state_word(s, 656))(0xbd3bb3b9u); }
// network allocators 0x14 / 0x15 (never registered on PS4) take the first present of heap_picks; probe at state +1280
U3X_FN void alias_net_heaps(uintptr_t base, void *s) {
    uintptr_t *alloc = reinterpret_cast<uintptr_t *>(base + 0x18417b0);
    uint32_t *probe = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 1280);
    probe[48] = probe[49] = 0;   // allocator mask
    probe[0] = 0x424f5250;       // 'PROB'
    probe[1] = 1;
    for (int i = 0; i < 64; ++i)
        if (alloc[i]) probe[48 + i / 32] |= 1u << (i & 31);
    for (uint8_t pick : heap_picks) {
        if (!alloc[pick]) continue;
        if (!alloc[0x14]) {
            alloc[0x14] = alloc[pick];
            probe[50] = pick;
        }
        if (!alloc[0x15]) alloc[0x15] = alloc[pick];
        break;
    }
}
U3X_FN void set_net_clock(uintptr_t base, void *s) {
    if (!(*reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78) & 0x80000000u)) {
        at<uintptr_t>(base + 0x17c3580) = base + 0x17b4c98;
        return;
    }
    uint8_t *nc = static_cast<uint8_t *>(s) + 0x2000;
    const uint8_t *engine = reinterpret_cast<const uint8_t *>(base + 0x17b4b98);
    for (int k = 0; k < 0x40; ++k) nc[k] = engine[k];
    for (int k = 0xc; k < 0x14; ++k) nc[k] = 0;   // time
    *reinterpret_cast<uint32_t *>(nc + 0x18) = 0;
    *reinterpret_cast<uint16_t *>(nc + 0x1c) = 0;
    for (int k = 0x20; k < 0x30; k += 4) *reinterpret_cast<uint32_t *>(nc + k) = 0;
    at<uint8_t *>(base + 0x17c3580) = nc;
}
// phase 1 (returns early to wait for the sign-in, or with phase 4 / the failure dialog)
U3X_FN void netinit_step(uintptr_t base, void *s) {
    if (u3h_onb_signin_wait(base, s)) return;
    u3h_k43_step(base, 0);
    void **symbol = net_symbol(s);
    if (!symbol || !*symbol) {
        uint32_t tag = 0x12;
        reinterpret_cast<void (*)(uint32_t, uint32_t *)>(state_word(s, 664))(0x4240ef2eu, &tag);
        symbol = net_symbol(s);
    }
    if (!symbol || !*symbol) {
        startup_error(s) = 2;
        startup_phase(s) = 4;
        return;
    }
    u3h_k43_step(base, 5);
    if (!reinterpret_cast<int (*)(uintptr_t)>(state_word(s, 672))(base)) {
        startup_error(s) = 3;
        startup_phase(s) = 4;
        return;
    }
    at<uint8_t>(base + 0x17d5c60) = static_cast<uint8_t>(u3h_onb_lan_flag());
    at<uint8_t>(base + 0x17d5c61) = 1;
    at<uint8_t>(base + 0x17b2a94) = 1;
    {   // the "net" level as 0x678d14 requests "multi-menu": {hash, sid, 0, 0, flags 0x0a, 0}
        const uintptr_t find = state_native(s, 42), hash = state_native(s, 40), request = state_native(s, 41);
        if (find && hash && request && !reinterpret_cast<void *(*)(uintptr_t, uint32_t)>(find)(base + 0x1afcf10, 0x4240ef2eu)) {
            uint8_t req[0x18];
            for (uint8_t &b : req) b = 0;
            *reinterpret_cast<uint64_t *>(req) = reinterpret_cast<uint64_t (*)(const char *)>(hash)(net_level);
            *reinterpret_cast<uint32_t *>(req + 8) = 0x4240ef2eu;
            req[0xe] = 0x0a;
            reinterpret_cast<void (*)(uintptr_t, void *)>(request)(base + 0x1afcf10, req);
        }
    }
    u3h_ngm_init(base);
    u3x_net_init_levels(base, s);
    if (profile_selected(s) && profile_selected(s) <= profile_count(s)) {   // the LAN profile name as the online id
        char *dst = reinterpret_cast<char *>(base + 0x17d6010);
        const char *src = static_cast<const char *>(s) + 136 + (profile_selected(s) - 1) * 64;
        unsigned n = 0;
        for (; n < 16 && src[n]; ++n) dst[n] = src[n];
        dst[n] = 0;
    }
    u3h_onp_name_apply(base);
    u3h_cin_netinit(base, s);
    u3h_netinit_services(base, s);
    if (uint8_t *pr = player_profile(base))   // clan tags saved with '\' get a space
        for (int k = 0; k < 4; ++k)
            if (pr[0xac0 + k] == '\\') pr[0xac0 + k] = ' ';
    if (!(state_switches(s) & 8)) alias_net_heaps(base, s);
    set_net_clock(base, s);
    at<uint8_t>(base + 0x16e7170) = 0;
    if (const uintptr_t reset = state_native(s, 12)) reinterpret_cast<int64_t (*)(uintptr_t)>(reset)(base + 0x17d6420);
    if (!at<uint32_t>(base + 0x17c3b40 + 0x2c)) {   // the MP character list for the DC handle
        at<uint32_t>(base + 0x17c3b40 + 0x2c) = 0x916e03b0u;
        at<uint64_t>(base + 0x17c3b40 + 0x30) = 0;
        at<int32_t>(base + 0x17c3b40 + 0x28) = -1;
    }
    u3h_k43_step(base, 7);
    if (u3h_onb_lan_flag()) u3h_lnb_netinit(base, s);
    else u3h_onb_netinit(base, s);
    u3h_k43_step(base, 43);
    u3h_mp_profile_start(base, s);
    if (u3h_onp_fail_take()) {
        ext_eb()[150] = 8;
        u3h_so_note(profile_download_failed, nullptr, static_cast<int32_t>(ext_ec()[101]), nullptr);
        u3x_net_shutdown_do(base, s);
        if (state_show_page(s)) u3h_show(base, s, u3h_k01_back_page(), 1);
        u3x_online_failed(base, s);
        return;
    }
    if (!u3h_emb_netinit(base, s) && !(*reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78) & 0x40000000u)) {
        if (const uintptr_t einit = state_native(s, 11)) {
            at<uint8_t>(base + 0x17c3440) = 1;
            reinterpret_cast<void (*)(uintptr_t)>(einit)(base + 0x17c3440);
        }
    }
    u3h_k43_step(base, 44);
    startup_phase(s) = 2;
}
}  // namespace
extern "C" U3X_FN void u3x_begin_startup(uintptr_t base, void *s) {
    if (!state_show_page(s) || startup_phase(s) || !profile_selected(s) || profile_selected(s) > profile_count(s)) return;
    u3h_register_lobby_click(base);
    startup_phase(s) = 1;
    startup_ticks(s) = 0;
    startup_error(s) = 0;
    u3h_k43_step(base, 0);
    if (!u3h_show(base, s, 0x9e1863b1u, 0)) {
        startup_phase(s) = 0;
        startup_error(s) = 1;
    }
}
extern "C" U3X_FN void u3x_startup_tick(uintptr_t base, void *s) {
    if (!startup_phase(s) || startup_phase(s) >= 3) return;
    const uint32_t ui = at<uint32_t>(base + 0x17bd080 + 8);
    if (ui == 7 || ui == 8) return;
    ++startup_ticks(s);
    if (startup_phase(s) == 1) {
        netinit_step(base, s);
        return;
    }
    if (u3h_k43_gate(base, s)) return;   // until the "net" level is loaded
    startup_phase(s) = 3;
    *reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 76) = 0;   // the profile dialog phase
    if (!u3h_show(base, s, u3h_k01_back_page(), 1) || !u3h_show(base, s, 0x53cbb5f2u, 0)) {
        startup_error(s) = 4;
        startup_phase(s) = 4;
    }
}

// ===== v7.187: third split, part 8 (the LAN room server) =====

// b010: the party host's TCP room server (PS3 f_945e70 .. f_9477a8). Up to 16 clients; a kicked client's address is
// refused afterwards. Messages in: 0xcf join {nat +4, name +0x10, host +0x34, data length +0x38, data +0x3c} (a taken name
// gets an 0xc9 with room 0), 0xcd member data, 0xd1 kick, 0xd2 / 0xd3 relayed. Out: 0xc9 join reply (room, capacity, up to
// 10 member records), 0xca joined, 0xce member data, 0xcb left, 0xcc kicked. Member records are 0x70 bytes: name, ip
// +0x24, id +0x28, nat +0x2a, data length +0x2b, "you" +0x2c, host +0x2d, data +0x2e. eb[76] accepts, eb[77] messages.
namespace {
struct RoomClient {
    char name[0x24];
    uint32_t id, nat, pad2c, pad30, len;
    uint8_t data[0x40];
    uint8_t active, joined, host, pad7b;
    Sock sock;
    uint32_t rxlen;
    uint8_t rx[0x800];
};
struct RoomServer {
    uint32_t counter, maxp;
    uint64_t arg, room;
    Sock listen;
    uint32_t state;
    int32_t kicked[16];
    RoomClient c[16];
};
static_assert(sizeof(RoomClient) == 0x88c && sizeof(RoomServer) == 0x8928);
U3X_BSS uint32_t room_counter;     // the room id's low bits
U3X_BSS uint8_t room_reply[0x478];
U3X_FN void zero_bytes(void *d, uint32_t n) {
    for (uint8_t *p = static_cast<uint8_t *>(d); n--;) *p++ = 0;
}
U3X_FN void copy_bytes(void *d, const void *s, uint32_t n) {
    const uint8_t *b = static_cast<const uint8_t *>(s);
    for (uint8_t *a = static_cast<uint8_t *>(d); n--;) *a++ = *b++;
}
U3X_FN bool room_member(const RoomClient &c) { return c.active && c.joined; }
U3X_FN void room_record(uint8_t *o, const RoomClient &c, int you) {
    copy_bytes(o, c.name, 0x24);
    *reinterpret_cast<uint32_t *>(o + 0x24) = c.sock.ip;
    *reinterpret_cast<uint16_t *>(o + 0x28) = static_cast<uint16_t>(c.id);
    o[0x2a] = static_cast<uint8_t>(c.nat);
    o[0x2b] = static_cast<uint8_t>(c.len);
    o[0x2c] = static_cast<uint8_t>(you);
    o[0x2d] = c.host;
    copy_bytes(o + 0x2e, c.data, 0x40);
}
U3X_FN void room_bcast(RoomServer *v, int except, const void *msg, uint32_t n) {
    for (int j = 0; j < 16; ++j)
        if (j != except && room_member(v->c[j])) u3x_sock_send(&v->c[j].sock, msg, n, 1);
}
U3X_FN void room_drop(RoomServer *v, int k) {   // the client is gone: 0xcb to the others
    RoomClient &c = v->c[k];
    uint8_t o[0x38];
    zero_bytes(o, sizeof(o));
    *reinterpret_cast<uint32_t *>(o) = 0xcb;
    *reinterpret_cast<uint16_t *>(o + 4) = static_cast<uint16_t>(c.id);
    *reinterpret_cast<uint64_t *>(o + 8) = v->room;
    copy_bytes(o + 0x10, c.name, 0x24);
    room_bcast(v, k, o, 0x38);
    u3x_sock_close(&c.sock);
    c.active = 0;
}
U3X_FN uint32_t room_join(RoomServer *v, int k, const uint8_t *m) {
    RoomClient &c = v->c[k];
    for (int j = 0; j < 16; ++j)
        if (room_member(v->c[j]) && same_name(v->c[j].name, reinterpret_cast<const char *>(m + 0x10))) {   // name taken
            zero_bytes(room_reply, sizeof(room_reply));
            *reinterpret_cast<uint32_t *>(room_reply) = 0xc9;
            *reinterpret_cast<uint16_t *>(room_reply + 0x10) = static_cast<uint16_t>(v->maxp);
            u3x_sock_send(&c.sock, room_reply, sizeof(room_reply), 1);
            return 0x80;
        }
    c.nat = rd<uint32_t>(m, 4);
    copy_bytes(c.name, m + 0x10, 0x24);
    c.joined = 1;
    c.len = rd<uint32_t>(m, 0x38);
    c.host = m[0x34];
    copy_bytes(c.data, m + 0x3c, 0x40);
    zero_bytes(room_reply, sizeof(room_reply));
    *reinterpret_cast<uint32_t *>(room_reply) = 0xc9;
    *reinterpret_cast<uint64_t *>(room_reply + 8) = v->room;
    *reinterpret_cast<uint16_t *>(room_reply + 0x10) = static_cast<uint16_t>(v->maxp);
    uint16_t cnt = 0;
    for (int j = 0; j < 16; ++j)
        if (room_member(v->c[j]) && cnt < 10) {
            room_record(room_reply + 0x18 + cnt * 0x70, v->c[j], j == k);
            ++cnt;
        }
    *reinterpret_cast<uint16_t *>(room_reply + 0x12) = cnt;
    u3x_sock_send(&c.sock, room_reply, sizeof(room_reply), 1);
    uint8_t o[0x80];
    zero_bytes(o, sizeof(o));
    *reinterpret_cast<uint32_t *>(o) = 0xca;
    *reinterpret_cast<uint64_t *>(o + 8) = v->room;
    room_record(o + 0x10, c, 0);
    room_bcast(v, k, o, 0x80);
    return 0x80;
}
}  // namespace
extern "C" U3X_FN void u3x_lnb_srv_ctor(void *v_) {
    RoomServer *v = static_cast<RoomServer *>(v_);
    u3x_sock_init(&v->listen);
    for (RoomClient &c : v->c) {
        u3x_sock_init(&c.sock);
        c.active = 0;
        c.rxlen = 0;
    }
    v->counter = 1;
    for (int32_t &q : v->kicked) q = -1;
}
extern "C" U3X_FN int u3x_lnb_srv_running(const void *v) { return static_cast<const RoomServer *>(v)->state == 1; }
extern "C" U3X_FN uint32_t u3x_lnb_srv_count(const void *v_) {
    const RoomServer *v = static_cast<const RoomServer *>(v_);
    uint32_t n = 0;
    for (const RoomClient &c : v->c) n += c.active != 0;
    return n;
}
// room id: our address (byte-swapped) in the high half, the port and a running number in the low
extern "C" U3X_FN int32_t u3x_lnb_srv_start(void *v_, uint64_t arg, uint32_t port, uint32_t maxp) {
    RoomServer *v = static_cast<RoomServer *>(v_);
    const uint32_t ip = u3x_sock_local_ip(), n = room_counter++;
    v->room = static_cast<uint64_t>(__builtin_bswap32(ip)) << 32 | static_cast<uint64_t>(port << 16) | n;
    v->arg = arg;
    v->maxp = maxp;
    const int32_t r = u3x_sock_listen(&v->listen, port);
    if (r < 0) u3x_sock_close(&v->listen);
    else v->state = 1;
    return r;
}
extern "C" U3X_FN void u3x_lnb_srv_stop(void *v_) {
    RoomServer *v = static_cast<RoomServer *>(v_);
    v->state = 0;
    for (RoomClient &c : v->c)
        if (c.active) {
            u3x_sock_close(&c.sock);
            c.active = 0;
        }
    u3x_sock_close(&v->listen);
}
// one message of client k: the bytes it used, 0 to wait for more (or an unknown message)
extern "C" U3X_FN uint32_t u3x_lnb_srv_msg(void *v_, int k) {
    RoomServer *v = static_cast<RoomServer *>(v_);
    RoomClient &c = v->c[k];
    const uint8_t *m = c.rx;
    const uint32_t n = c.rxlen, id = rd<uint32_t>(m, 0);
    ++ext_eb()[77];
    switch (id) {
    case 0xcf: return n < 0x80 ? 0 : room_join(v, k, m);
    case 0xcd: {
        if (n < 0x50) return 0;
        c.len = rd<uint32_t>(m, 4);
        copy_bytes(c.data, m + 0x10, 0x40);
        uint8_t o[0x58];
        zero_bytes(o, sizeof(o));
        *reinterpret_cast<uint32_t *>(o) = 0xce;
        *reinterpret_cast<uint16_t *>(o + 4) = static_cast<uint16_t>(c.id);
        *reinterpret_cast<uint64_t *>(o + 8) = v->room;
        *reinterpret_cast<uint32_t *>(o + 0x10) = rd<uint32_t>(m, 4);
        copy_bytes(o + 0x14, m + 0x10, 0x40);
        room_bcast(v, k, o, 0x58);
        return 0x50;
    }
    case 0xd3:
        if (n < 0x10) return 0;
        room_bcast(v, k, m, 0x10);
        return 0x10;
    case 0xd2: {   // relayed with the sender's id
        if (n < 0x20) return 0;
        uint8_t o[0x20];
        copy_bytes(o, m, 0x20);
        *reinterpret_cast<uint32_t *>(o + 4) = c.id;
        room_bcast(v, k, o, 0x20);
        return 0x20;
    }
    case 0xd1:   // kick member id +4: note its address, tell it (0xcc with +8)
        if (n < 0x10) return 0;
        for (RoomClient &t : v->c) {
            if (!room_member(t) || t.id != rd<uint32_t>(m, 4)) continue;
            for (int32_t &q : v->kicked)
                if (q == 0) {
                    q = static_cast<int32_t>(t.sock.ip);
                    break;
                }
            uint8_t o[0x10];
            zero_bytes(o, sizeof(o));
            *reinterpret_cast<uint32_t *>(o) = 0xcc;
            *reinterpret_cast<uint64_t *>(o + 8) = rd<uint64_t>(m, 8);
            u3x_sock_send(&t.sock, o, 0x10, 1);
            break;
        }
        return 0x10;
    }
    return 0;
}
// accept one connection (not twice from the same address and port, not from a kicked address, not past capacity), then
// read each client and handle its complete messages
extern "C" U3X_FN void u3x_lnb_srv_update(void *v_) {
    RoomServer *v = static_cast<RoomServer *>(v_);
    if (v->state != 1) return;
    uint32_t ip = 0, port = 0;
    const int32_t fd = u3x_sock_accept(&v->listen, &ip, &port);
    if (fd != 0) {
        Sock ns;
        u3x_sock_adopt(&ns, fd, ip, port);
        ++ext_eb()[76];
        bool dup = false;
        for (const RoomClient &c : v->c)
            if (c.active && c.sock.ip == ns.ip && c.sock.port == ns.port) dup = true;
        bool kicked = false;
        if (!dup && u3x_lnb_srv_count(v) < v->maxp)
            for (int32_t q : v->kicked)
                if (q == static_cast<int32_t>(ns.ip)) kicked = true;
        if (dup || kicked || u3x_lnb_srv_count(v) >= v->maxp) {
            u3x_sock_close(&ns);
        } else {
            for (RoomClient &c : v->c)
                if (!c.active) {
                    c.sock = ns;
                    c.id = v->counter++;
                    c.active = 1;
                    c.joined = 0;
                    c.rxlen = 0;
                    break;
                }
        }
    }
    for (int k = 0; k < 16; ++k) {
        RoomClient &c = v->c[k];
        if (!c.active) continue;
        const int32_t p = u3x_sock_poll(&c.sock);
        if (p < 0) {
            room_drop(v, k);
            continue;
        }
        if (p == 0) continue;
        const int32_t r = u3x_sock_recv(&c.sock, c.rx + c.rxlen, 0x800 - c.rxlen, 1);
        if (r == 0) {
            room_drop(v, k);
            continue;
        }
        if (r > 0) c.rxlen += static_cast<uint32_t>(r);
        while (c.rxlen) {
            const uint32_t used = u3x_lnb_srv_msg(v, k);
            if (!used) break;
            for (uint32_t j = used; j < c.rxlen; ++j) c.rx[j - used] = c.rx[j];
            c.rxlen -= used;
        }
    }
}

// ===== v7.188: third split, part 9 (the emblem compositor) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_emb_h(uintptr_t b, void *s, uintptr_t handle);
U3H_IMPORT uint8_t *u3h_emb_cache(uintptr_t b, const char *name);
U3H_IMPORT void *u3h_emb_alloc(uintptr_t b, uint64_t n);
U3H_IMPORT void u3h_emb_free(uintptr_t b, void *p);
U3H_IMPORT int u3h_emb_dds(uintptr_t b, const char *name, void *dds);
U3H_IMPORT void u3h_emb_quad(uint8_t *pix, uint32_t size, const void *t, const uint32_t *uv, const void *mask, const uint32_t *c16);
U3H_IMPORT void u3h_emb_decal(uint8_t *pix, uint32_t size, const void *t, const uint32_t *q, const uint32_t *c16);
U3H_IMPORT int u3h_emb_texcreate(uintptr_t b, void *rec, const uint8_t *pix, uint32_t size);
U3H_IMPORT void *u3h_emb_ralloc(uint32_t size);
U3H_IMPORT uint32_t u3h_h16q16(uint32_t f);
}

// c011: PS3 1.19's emblem compositor (f_2800c8, run from the render frame f_64f288). The emblem object (0x17c3440) holds
// the four layers {icon, angle, scale, colour, ...} 8 bytes each at +8 (or +0x2c while +0x60 >= 0), the record slots
// +0x50 / +0x58 (128 px) and +0x68 / +0x70 (256 px), and +2 "try again". Each layer: its icon texture (0x66bc30) rotated
// and scaled into the target, tinted by the palette colour, through the spray mask in slot 1; slot 1 icons with decals
// also get the drips at the decal points. A record keeps a 40-byte key of its inputs and is not redrawn while they are
// unchanged. ec[113] composites, [114] retries, [122] last failure, [124] skipped as unchanged, [125] size,
// [149] / [150] covered pixels per slot; ec[0] bit 15 flips the rows.
namespace {
struct EmbRec {
    uint64_t tex, map;
    uint8_t ok, pad0[3];
    uint32_t w, h, fmt, size;
    uint16_t pin, used;
    uint8_t key[40];
};
static_assert(sizeof(EmbRec) == 80);
struct EmbDds {
    uint8_t *file;
    uint32_t w, h, maxl, bs;
    uint32_t off[16];
};
static_assert(sizeof(EmbDds) == 88);
U3X_RO const char spray_mask[] = "emblems/spray-01", drips[] = "emblems/emblem-16-drips";
constexpr uintptr_t emblem_object = 0x17c3440;
constexpr uint32_t f_half = 0x3f000000u, f_eighth = 0x3e000000u, f_quarter = 0x3e800000u, f_sixteenth = 0x3d800000u;
U3X_FN uint8_t *emblem(uintptr_t b) { return reinterpret_cast<uint8_t *>(b + emblem_object); }
U3X_FN bool loaded(const uint8_t *p) { return p && p[0x10]; }
// the four corner (u, 1 - v) pairs of a layer of scale s at angle (cs, sn), centred on (0.5, 0.5)
U3X_FN void layer_uv(uint32_t *uv, uint32_t s, uint32_t cs, uint32_t sn) {
    const uint32_t a = f_div(f_half, s), na = f_div(0xbf000000u, s);
    const uint32_t ac = f_mul(a, cs), as = f_mul(a, sn), nas = f_mul(na, sn), nac = f_mul(na, cs);
    const uint32_t as_h = f_add(as, f_half), ac_h = f_add(ac, f_half), nas_h = f_add(nas, f_half), nac_h = f_add(nac, f_half);
    uv[0] = f_add(as, nac_h);
    uv[1] = f_sub(F_ONE, f_add(nac, nas_h));
    uv[2] = f_add(nas, nac_h);
    uv[3] = f_sub(F_ONE, f_add(ac, nas_h));
    uv[4] = f_add(nas, ac_h);
    uv[5] = f_sub(F_ONE, f_add(ac, as_h));
    uv[6] = f_add(as, ac_h);
    uv[7] = f_sub(F_ONE, f_add(nac, as_h));
}
// the drips at decal point en {cell, x, y} (cell: column bits 0-1, row 3 - bits 2-3 of the 4x4 drip sheet) of a layer
U3X_FN void drip_quad(uint32_t *q, const uint8_t *en, uint32_t grid, uint32_t s, uint32_t cs, uint32_t sn, uint32_t half) {
    const uint32_t cell = rd<uint32_t>(en, 0);
    const uint32_t uc = f_madd(f_from_int(static_cast<int32_t>(cell & 3)), f_quarter, f_eighth);
    const uint32_t vc = f_madd(f_from_int(static_cast<int32_t>(3 - (cell >> 2))), f_quarter, f_eighth);
    uint32_t y = f_div(f_from_int(rd<int32_t>(en, 8)), grid);
    y = f_sub(f_add(y, y), F_ONE);
    uint32_t x = f_div(f_from_int(rd<int32_t>(en, 4)), grid);
    x = f_sub(f_add(x, x), F_ONE);
    const uint32_t ys = f_mul(y, s), xs = f_mul(x, s);
    uint32_t yr = f_madd(xs, sn, f_mul(ys, cs)), xr = f_madd(xs, cs, f_mul(ys, sn) ^ 0x80000000u);
    yr = f_madd(yr, f_half, f_half);
    xr = f_madd(xr, f_half, f_half);
    q[0] = f_sub(xr, half);
    q[1] = f_add(xr, half);
    q[2] = f_sub(yr, half);
    q[3] = f_add(yr, half);
    q[4] = f_sub(uc, f_eighth);
    q[5] = f_add(uc, f_eighth);
    q[6] = f_sub(F_ONE, f_add(vc, f_eighth));   // the top edge has v = vc + 1/8
    q[7] = f_sub(F_ONE, f_sub(vc, f_eighth));
}
}  // namespace
extern "C" U3X_FN uint8_t *u3x_emb_rec_slot(uintptr_t b, uint32_t sl) {
    const uint8_t *e = emblem(b);
    uint64_t r = rd<uint64_t>(e, 0x50 + 8 * sl);
    if (!r) r = rd<uint64_t>(e, 0x68 + 8 * sl);
    return reinterpret_cast<uint8_t *>(r);
}
extern "C" U3X_FN uint32_t u3x_emb_size(uintptr_t b) {
    const uint8_t *e = emblem(b);
    if (rd<uint64_t>(e, 0x50) || rd<uint64_t>(e, 0x58)) return 0x80;
    return (rd<uint64_t>(e, 0x68) || rd<uint64_t>(e, 0x70)) ? 0x100 : 0;
}
extern "C" U3X_FN void u3x_emb_render(uintptr_t b, void *s, uint32_t sl, void *rec_, uint32_t size) {
    EmbRec *rec = static_cast<EmbRec *>(rec_);
    uint32_t *ec = ext_ec();
    uint8_t *e = emblem(b);
    if (sl == 0) e[2] = 0;
    uint8_t plan[4], *ly[4], *tx[4], *dec[4], *mk = nullptr, *dr = nullptr;
    uint32_t col[4][3], sc[4], ang[4];
    for (uint32_t i = 0; i < 4; ++i) {   // what each layer needs (bit 0 drawn, bit 1 drips) and whether it is loaded
        plan[i] = 0;
        uint8_t *L = rd<int32_t>(e, 0x60) < 0 ? e + 8 + 8 * i : e + 0x2c + 8 * i;
        ly[i] = L;
        uint8_t *icons = u3h_emb_h(b, s, 0x1b67e60u + i * 0x18);
        const int32_t n = icons ? rd<int32_t>(icons, 0) : 0, icon = n > 0 ? static_cast<int32_t>(L[0]) % n : 0;
        uint8_t *pal = u3h_emb_h(b, s, 0x1b67ec0u);
        const int32_t pc = pal ? rd<int32_t>(pal, 0) : 0, ci = pc > 0 ? static_cast<int32_t>(L[4]) % pc : 0;
        const uint8_t *pe = pal ? rd<const uint8_t *>(pal, 8) : nullptr;
        for (int k = 0; k < 3; ++k) col[i][k] = pe ? rd<uint32_t>(pe, ci * 12 + 4 * k) : 0;
        const uint8_t *range = u3h_emb_h(b, s, 0x1b67ee0u + i * 0x18);
        const uint32_t lo = range ? rd<uint32_t>(range, 0) : 0, hi = range ? rd<uint32_t>(range, 4) : 0;
        sc[i] = f_madd(f_sub(hi, lo), f_mul(f_from_int(L[2]), 0x3b808081u), lo);   // lo + (hi - lo) * L[2] / 255
        ang[i] = f_mul(f_from_int(L[1]), 0x3cc9d9b5u);                             // L[1] * 2 pi / 255
        tx[i] = game_call<uint8_t *>(b + 0x66bc30, b + emblem_object, static_cast<int32_t>(i));   // requests the load
        mk = u3h_emb_cache(b, spray_mask);
        dec[i] = nullptr;
        if (!icon) continue;
        if (!loaded(tx[i]) || (sl == 1 && !loaded(mk))) {
            e[2] = 1;
            continue;
        }
        plan[i] = 1;
        dec[i] = (icons && rd<uint8_t *>(icons, 8)) ? rd<uint8_t *>(rd<uint8_t *>(icons, 8), icon * 0x18 + 0x10) : nullptr;
        dr = u3h_emb_cache(b, drips);
        if (sl == 1 && dec[i]) {
            if (!loaded(dr)) e[2] = 1;
            else plan[i] |= 2;
        }
    }
    uint8_t key[40];   // the inputs: the same as the record's last composite gives the same pixels
    for (int k = 0; k < 32; ++k) key[k] = ly[k >> 3][k & 7];
    key[32] = static_cast<uint8_t>(sl);
    key[33] = static_cast<uint8_t>(size >> 8);
    key[34] = static_cast<uint8_t>(plan[0] | plan[1] << 2 | plan[2] << 4 | plan[3] << 6);
    key[35] = 1;
    for (int k = 36; k < 40; ++k) key[k] = 0;
    bool same = rec->map != 0;
    for (int k = 0; k < 40 && same; ++k) same = rec->key[k] == key[k];
    if (same) {
        ++ec[124];
        return;
    }
    const uint64_t bytes = static_cast<uint64_t>(size) * size * 4;
    uint8_t *pix = static_cast<uint8_t *>(u3h_emb_alloc(b, bytes));
    if (!pix) {
        ec[122] = 4;
        e[2] = 1;
        return;
    }
    for (uint64_t k = 0; k < bytes; ++k) pix[k] = 0;
    EmbDds mask, drip;
    mask.file = nullptr;
    drip.file = nullptr;
    bool ok = true;
    for (uint32_t i = 0; i < 4; ++i) {
        if (!(plan[i] & 1)) continue;
        EmbDds t;
        if (!u3h_emb_dds(b, reinterpret_cast<const char *>(tx[i] + 0x20), &t)) {
            e[2] = 1;
            ok = false;
            continue;
        }
        if (sl == 1 && !mask.file && !u3h_emb_dds(b, reinterpret_cast<const char *>(mk + 0x20), &mask)) {
            u3h_emb_free(b, t.file);
            e[2] = 1;
            ok = false;
            continue;
        }
        const uint32_t sv = sc[i], cs = u3h_sincos(ang[i], 1), sn = u3h_sincos(ang[i], 0);
        uint32_t uv[8];
        layer_uv(uv, sv, cs, sn);
        uint32_t c16[3];   // the colour as the fp16 register holds it, blue first
        c16[0] = u3h_h16q16(col[i][2]);
        c16[1] = u3h_h16q16(col[i][1]);
        c16[2] = u3h_h16q16(col[i][0]);
        u3h_emb_quad(pix, size, &t, uv, sl == 1 ? &mask : nullptr, c16);
        u3h_emb_free(b, t.file);
        if (!(plan[i] & 2)) continue;
        if (!drip.file && !u3h_emb_dds(b, reinterpret_cast<const char *>(dr + 0x20), &drip)) {
            e[2] = 1;
            ok = false;
            continue;
        }
        const uint8_t *d = dec[i];
        const int32_t count = rd<int32_t>(d, 4);
        const uint8_t *points = rd<const uint8_t *>(d, 8);
        const uint32_t half = f_madd(sv, f_sixteenth, f_sixteenth), grid = f_from_int(rd<int32_t>(d, 0));
        for (int32_t k = 0; points && k < count; ++k) {
            uint32_t q[8];
            drip_quad(q, points + static_cast<int64_t>(k) * 12, grid, sv, cs, sn, half);
            u3h_emb_decal(pix, size, &drip, q, c16);
        }
    }
    if (mask.file) u3h_emb_free(b, mask.file);
    if (drip.file) u3h_emb_free(b, drip.file);
    if (ec[0] & 0x8000u)   // rows upside down (off by default)
        for (uint32_t y = 0; y < size / 2; ++y) {
            uint32_t *r0 = reinterpret_cast<uint32_t *>(pix + static_cast<uint64_t>(y) * size * 4);
            uint32_t *r1 = reinterpret_cast<uint32_t *>(pix + static_cast<uint64_t>(size - 1 - y) * size * 4);
            for (uint32_t x = 0; x < size; ++x) {
                const uint32_t t = r0[x];
                r0[x] = r1[x];
                r1[x] = t;
            }
        }
    {
        uint32_t covered = 0;
        for (uint64_t k = 3; k < bytes; k += 4) covered += pix[k] != 0;
        ec[149 + (sl & 1)] = covered;
    }
    if (u3h_emb_texcreate(b, rec, pix, size)) {
        for (int k = 0; k < 40; ++k) rec->key[k] = ok ? key[k] : 0;
        ++ec[113];
        ec[125] = size;
    } else {
        e[2] = 1;
    }
    u3h_emb_free(b, pix);
}
// PS3 f_64f288's emblem part: each record slot that holds a record is composited
extern "C" U3X_FN void u3x_emb_pass(uintptr_t b, void *s) {
    uint8_t *flag = nullptr;
    u3h_emb_records(&flag);
    if (!*flag) return;   // PS3 render flag
    for (uint32_t sl = 0; sl < 2; ++sl) {
        uint8_t *r = u3x_emb_rec_slot(b, sl);
        if (!r || !u3h_emb_is_record(reinterpret_cast<uint64_t>(r))) continue;
        const uint32_t size = u3x_emb_size(b);
        if (!size) continue;
        u3x_emb_render(b, s, sl, r, size);
        if (emblem(b)[2]) ++ext_ec()[114];
    }
}
// net player i (0..9) while multiplayer is active
extern "C" U3X_FN uint8_t *u3x_emb_np(uintptr_t b, uint32_t i) {
    if (!at<uint8_t>(b + 0x17d5c61) || i > 9) return nullptr;
    return reinterpret_cast<uint8_t *>(b + 0x17cb3f0 + static_cast<uintptr_t>(i) * 0x9c0);
}
extern "C" U3X_FN int u3x_emb_valid(const uint8_t *np) { return np[0xe1] && !np[0x5e4]; }
extern "C" U3X_FN uint32_t u3x_emb_local(uintptr_t b) { return local_user(b); }
// user u's emblem in the profiles (+0x1510 of each 0x7b00-byte profile)
extern "C" U3X_FN uint8_t *u3x_emb_prof(uintptr_t b, uint32_t u) {
    uint8_t *p = at<uint8_t *>(b + 0x17dd610);
    return p ? p + static_cast<uintptr_t>(u) * 0x7b00 + 0x1510 : nullptr;
}
// net player record k: the old one is unpinned and freed first, then the game's setter (0x78fa70) stores the new one
extern "C" U3X_FN void u3x_emb_setrec(uintptr_t b, uint8_t *np, int32_t k, uint64_t r) {
    const uint64_t old = rd<uint64_t>(np, 0x7e8 + 8 * k);
    if (old && u3h_emb_is_record(old)) {
        reinterpret_cast<EmbRec *>(old)->pin = 0;
        u3h_emb_free_record(b, old);
    }
    game_call<void>(b + 0x78fa70, np, k, r);
}
// fill the two record slots at emblem +off that are empty with new pinned records of that size
extern "C" U3X_FN void u3x_emb_newrecs(uintptr_t b, uint32_t off, uint32_t size) {
    uint8_t *e = emblem(b);
    for (int k = 0; k < 2; ++k) {
        uint64_t &slot = *reinterpret_cast<uint64_t *>(e + off + 8 * k);
        if (slot) continue;
        EmbRec *r = static_cast<EmbRec *>(u3h_emb_ralloc(size));
        if (r) r->pin = 1;
        slot = reinterpret_cast<uint64_t>(r);
    }
}

// ===== v7.189: third split, part 10 (the missions list) =====

extern "C" {
U3H_IMPORT uint32_t u3h_current_tab(uintptr_t base, void *s);
}

// x039: LIST_MISSIONS (0xd9) on PAGE_MISSIONS 0xdfdb36fa. The page's tab picks the mission set (DC 0x4360c988, sets of
// 0x20: count +8, missions +0x18 of 0x4c: id, name +4, goal text +8, goal kind +0x10, goal count +0x14, hours +0x18,
// level +0x20, reward +0x24, cost +0x44). The missions object (0x17da200 + user * 0x1018): set +0x100c, selection +0x1010;
// the profile: active mission +0x1864, its start time +0x1868, progress +0xf4c, done bits +0xecc, money +0x74c.
// Level and cost show red when not met; the active mission shows its time left of the 24 hours. Picking a row offers
// to cancel the active one or to take another (when the game allows it).
namespace {
U3X_RO const char mission_ok[] = "|@Cff7cce96|", mission_short[] = "|@Cfffe7c66|", dollar[] = "$";
U3X_FN int32_t &mission_set_index(void *s) { return *reinterpret_cast<int32_t *>(static_cast<uint8_t *>(s) + 0x3904); }
U3X_FN uint32_t menu_user(uintptr_t base) { return at<uint32_t>(base + 0x17c23a0) & 3; }
}  // namespace
extern "C" U3X_FN uint8_t *u3x_mission_obj(uintptr_t base) {
    return reinterpret_cast<uint8_t *>(base + 0x17da200) + static_cast<uintptr_t>(menu_user(base)) * 0x1018;
}
extern "C" U3X_FN uint8_t *u3x_mission_profile(uintptr_t base) {
    uint8_t *p = at<uint8_t *>(base + 0x17dd610);
    return p ? p + static_cast<uintptr_t>(menu_user(base)) * 0x7b00 : nullptr;
}
extern "C" U3X_FN uint8_t *u3x_mission_set(uintptr_t base, void *s, int32_t set) {
    if (!u3h_sym_ready(base, s, 0x4360c988u)) return nullptr;
    const uintptr_t lookup = state_native(s, 13);
    uint8_t *t = lookup ? reinterpret_cast<uint8_t *(*)(uint32_t)>(lookup)(0x4360c988u) : nullptr;
    if (!t || set < 0 || static_cast<uint32_t>(set) >= rd<uint32_t>(t, 0)) return nullptr;
    return rd<uint8_t *>(t, 8) + static_cast<uintptr_t>(set) * 0x20;
}
extern "C" U3X_FN int u3x_mission_finished(const uint8_t *pr, uint32_t id) {
    return pr && id < 0x400 && (pr[0xecc + (id >> 3)] >> (id & 7) & 1);
}
// the level override (0x17d7abc, level + 1) or the profile's rank
extern "C" U3X_FN int32_t u3x_player_level(uintptr_t base, void *s, uint8_t *pr) {
    const int32_t forced = at<int32_t>(base + 0x17d7abc);
    if (forced) return forced - 1;
    const uintptr_t rank = state_native(s, 21);
    return (rank && pr) ? static_cast<int32_t>(reinterpret_cast<uint32_t (*)(void *)>(rank)(pr)) : 0;
}
// seconds left of the active mission's 24 hours (0 when never started or without a clock)
extern "C" U3X_FN int32_t u3x_mission_seconds_left(const uint8_t *pr) {
    const uint32_t start = pr ? rd<uint32_t>(pr, 0x1868) : 0;
    const uintptr_t now = *reinterpret_cast<uintptr_t *>(&ext_eb()[4]);
    if (!start || !now) return 0;
    const int64_t d = 86400 - (reinterpret_cast<int64_t (*)(int64_t *)>(now)(nullptr) - static_cast<int64_t>(start));
    return d < 0 ? 0 : (d > 86400 ? 86400 : static_cast<int32_t>(d));
}
// PS4's texts lost PS3's "[A] minutes": a text without a digit gets "v " in front
extern "C" U3X_FN void u3x_x79_num(char *o, int32_t v) {
    for (const char *c = o; *c; ++c)
        if (*c >= '0' && *c <= '9') return;
    char t[64];
    int n = 0;
    for (; o[n] && n < 40; ++n) t[n] = o[n];
    t[n] = 0;
    char *q = append_number(o, o + 16, v, false);
    *q++ = ' ';
    for (int i = 0; t[i]; ++i) *q++ = t[i];
    *q = 0;
}
// "h hours m minutes" with the singular / plural texts (PS3 f_7aba74 without days)
extern "C" U3X_FN void u3x_mission_time_text(uintptr_t base, void *s, int32_t t, char *o) {
    const uintptr_t tn = state_native(s, 38);
    const int32_t h = t / 3600, m = (t / 60) % 60;
    uint32_t id;
    int64_t a = m, b = 0;
    o[0] = 0;
    if (!tn) return;
    if (h == 0) {
        id = m == 1 ? 0x9d59ec8au : 0x38e8ba93u;
    } else if (m == 0) {
        id = h == 1 ? 0x669b8ea8u : 0x5879aefbu;
        a = h;
    } else {
        a = h;
        b = m;
        id = h == 1 ? (m == 1 ? 0x7ce24100u : 0x422d0b5du) : (m == 1 ? 0xe8f6e6abu : 0x6093ed1fu);
    }
    reinterpret_cast<void (*)(uintptr_t, char *, uint32_t, uint32_t, int64_t, int64_t)>(tn)(base + 0x17026b8, o, 0x40, id, a, b);
    if (h == 0 || m == 0) u3x_x79_num(o, static_cast<int32_t>(a));
}
extern "C" U3X_FN int u3x_missions_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414ed9u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 760) = base;
    uint8_t *mo = u3x_mission_obj(base), *pr = u3x_mission_profile(base);
    int32_t &selected = *reinterpret_cast<int32_t *>(mo + 0x1010);
    if (u3h_active_page(base) == 0xdfdb36fau) {   // the tab picks the set
        const uint32_t tab = u3h_current_tab(base, s);
        const int32_t set = tab == 0x102fc82eu ? 1 : tab == 0x35fa8d17u ? 2 : 0;
        if (mission_set_index(s) != set) {
            selected = 0;
            mission_set_index(s) = set;
        }
    }
    *reinterpret_cast<int32_t *>(mo + 0x100c) = mission_set_index(s);
    const uint8_t *set = u3x_mission_set(base, s, mission_set_index(s));
    if (!set) {
        if (ev[0] == 6) ev[1] = -1;
        return 1;
    }
    const uint32_t count = rd<uint32_t>(set, 8);
    uint8_t *list = rd<uint8_t *>(set, 0x18);
    const int32_t row = ev[1];
    const uint8_t *m = (list && row >= 0 && static_cast<uint32_t>(row) < count) ? list + static_cast<uintptr_t>(row) * 0x4c : nullptr;
    const uint32_t active = pr ? rd<uint32_t>(pr, 0x1864) : 0;
    int32_t level = u3x_player_level(base, s, pr);
    {   // the override must not lock missions below the real rank; LAN unlocks them all (74)
        const uintptr_t rank = state_native(s, 21);
        if (rank && pr && static_cast<int32_t>(reinterpret_cast<uint32_t (*)(void *)>(rank)(pr)) > level)
            level = static_cast<int32_t>(reinterpret_cast<uint32_t (*)(void *)>(rank)(pr));
        if (at<uint8_t>(base + 0x17d5c60) && level < 74) level = 74;
    }
    const uint32_t money = pr ? rd<uint32_t>(pr, 0x74c) : 0;
    char *const buf = static_cast<char *>(s) + 0x2c80, *end = buf + 250;
    char *o = buf;
    *o = 0;
    switch (ev[0]) {
    case 10: selected = row; break;
    case 2: {
        if (!m) break;
        const uint32_t sub = static_cast<uint32_t>(ev[0x15]);
        if (sub == 0) {
            set_row_text(ev, u3h_text(base, s, rd<uint32_t>(m, 4)));
        } else if (sub == 0xbee56a68u) {   // level
            o = append_chars(o, end, level + 1 < rd<int32_t>(m, 0x20) ? mission_short : mission_ok);
            o = append_number(o, end, rd<int32_t>(m, 0x20), false);
            *o = 0;
            set_row_text(ev, buf);
        } else if (sub == 0xed9203d6u) {   // cost
            o = append_chars(o, end, money < rd<uint32_t>(m, 0x44) ? mission_short : mission_ok);
            o = append_chars(o, end, dollar);
            o = append_number(o, end, rd<uint32_t>(m, 0x44), true);
            *o = 0;
            set_row_text(ev, buf);
        } else if (sub == 0xa6ed5ba5u) {   // reward
            o = append_chars(o, end, mission_ok);
            o = append_chars(o, end, dollar);
            o = append_number(o, end, rd<int32_t>(m, 0x24), true);
            *o = 0;
            set_row_text(ev, buf);
        } else if (sub == 0x8dc65186u) {   // hours
            const int32_t g = rd<int32_t>(m, 0x18);
            u3h_text_num(base, s, buf, 0x40, g == 1 ? 0x9d59ec8au : 0x38e8ba93u, g);
            u3x_x79_num(buf, g);
            set_row_text(ev, buf);
        } else if (sub == 0x7046d624u) {   // goal
            u3h_text_num(base, s, buf, 0x100, rd<uint32_t>(m, 8), rd<int32_t>(m, 0x14));
            set_row_text(ev, buf);
        } else if (sub == 0x4bdd7c07u) {   // goal count (items as a text)
            if (rd<uint32_t>(m, 0x10) == 0x5c494554u) {
                u3h_text_num(base, s, buf, 0x80, 0x4afc6214u, rd<int32_t>(m, 0x14));
            } else {
                o = append_number(o, end, rd<int32_t>(m, 0x14), false);
                *o = 0;
            }
            set_row_text(ev, buf);
        } else if (sub == 0xdda1fdb4u && rd<uint32_t>(m, 0) == active) {   // time left
            u3x_mission_time_text(base, s, u3x_mission_seconds_left(pr), buf);
            set_row_text(ev, buf);
        }
        break;
    }
    case 4: {
        if (!m || ev[0x14] != 0) break;
        uint32_t icon = 0;
        if (u3x_mission_finished(pr, rd<uint32_t>(m, 0))) icon = 0x08208d0au;
        else if (rd<uint32_t>(m, 0) == active) icon = 0xe2315f64u;
        else if (level + 1 < rd<int32_t>(m, 0x20)) icon = 0x7bebc95fu;
        if (icon && u3h_icon_index(base, icon) >= 0) u3h_set_icon(base, ev, icon);
        break;
    }
    case 5: ev[1] = selected; break;
    case 6: ev[1] = static_cast<int32_t>(count) - 1; break;
    case 11: {
        if (!m) break;
        const uint32_t q = static_cast<uint32_t>(ev[0x13]);
        if (q == 0xe0bf7062u) ev[14] = rd<uint32_t>(m, 0) == active;
        else if (q == 0x4bdd7c07u) ev[14] = rd<int32_t>(m, 0x14);
        else if (q == 0x63f35e57u) ev[14] = (rd<uint32_t>(m, 0) == active && active) ? rd<int32_t>(pr, 0xf4c) : 0;
        else if (q == 0x464129abu) ev[14] = u3x_mission_finished(pr, rd<uint32_t>(m, 0));
        break;
    }
    case 0x16: {   // the active one: offer to cancel it; another: offer to take it
        const uint32_t sel_row = static_cast<uint32_t>(selected);
        uint8_t *sel = list && sel_row < count ? list + static_cast<uintptr_t>(sel_row) * 0x4c : nullptr;
        if (!sel) break;
        if (rd<uint32_t>(sel, 0) == active) {
            u3h_show_dialog(base, s, 0x84538677u, 0x0260a263u, 0xe6452a77u, 0x075bc82d, 0x1bc6f0e5u, 0);
            break;
        }
        const uintptr_t can = state_native(s, 39);
        if (!can || !reinterpret_cast<uint8_t (*)(uint8_t *, uint8_t *, int)>(can)(mo, sel, 0)) break;
        u3h_show_dialog(base, s, active ? 0x2aac16e8u : 0x6748cb03u, 0x0260a263u, 0xea1f40c5u, 0x075bc82d, 0x1dd02cc2u, 0);
        break;
    }
    }
    return 1;
}

// ===== v7.190: third split, part 11 (the anim decoder's visitors) =====

extern "C" {
U3H_IMPORT uint8_t *u3h_stream_root(uintptr_t base, uint8_t *anim, uint32_t *phase);
}
extern "C" uint32_t u3x_v_frames(const uint8_t *an);

// c009: the three visitors the 1.19 anim decoder drives (vtables 0x10f5148 state, 0x10f50e8 commands, 0x10f50b8
// locator; Visitor.kind 0 / 1 / 2). State: the snapshot's anim entries (up to 22 of 0x18 at +0x210 count). Commands: the
// PS4 command buffer {words, capacity +8, position +0xc, highest index +0x10}: 8 pose, 9 blend, 0xa sub, 0xc / 0xe ice,
// 0xd bind, 0xf copy; depth past 6 stops. Locator: up to 4 sampled locators {scale, quat, trans}, blended in place.
// A PS3 trap becomes Visitor.error (the first one; 12 empty counts ec[104], others go to ec[82]); ec[102] full state
// list, ec[103] ice commands.
namespace {
U3X_FN Visitor &vis(void *v) { return *static_cast<Visitor *>(v); }
U3X_FN void locator_identity(uint8_t *loc) {   // scale (1,1,1,1), quat (0,0,0,1), trans 0
    uint32_t *l = reinterpret_cast<uint32_t *>(loc);
    for (int k = 0; k < 12; ++k) l[k] = 0;
    l[0] = l[1] = l[2] = l[3] = F_ONE;
    l[7] = F_ONE;
}
// the anim's frame for phase ph: ph * (frames - 1)
U3X_FN uint32_t anim_frame(uint32_t ph, const uint8_t *an) { return f_mul(ph, f_sub(f_from_int(static_cast<int32_t>(u3x_v_frames(an))), F_ONE)); }
}  // namespace
extern "C" U3X_FN void u3x_v_error(void *v_, int code) {
    Visitor &v = vis(v_);
    if (v.error) return;
    v.error = code;
    if (code == 12) ++ext_ec()[104];
    else ext_ec()[82] = static_cast<uint32_t>(code);
}
// room for a command of n words (type, n) or 0 (past the capacity: error 7)
extern "C" U3X_FN uint32_t *u3x_v_command(void *v_, uint16_t type, uint16_t n) {
    Visitor &v = vis(v_);
    uint8_t *cb = v.commands;
    const uint32_t pos = rd<uint32_t>(cb, 0xc), cap = rd<uint32_t>(cb, 8);
    if (v.error) return nullptr;
    if (pos + n > cap) {
        u3x_v_error(v_, 7);
        return nullptr;
    }
    uint32_t *w = rd<uint32_t *>(cb, 0) + pos;
    reinterpret_cast<uint16_t *>(w)[0] = type;
    reinterpret_cast<uint16_t *>(w)[1] = n;
    *reinterpret_cast<uint32_t *>(cb + 0xc) = pos + n;
    return w;
}
extern "C" U3X_FN void u3x_v_max(void *v, uint32_t i) {
    uint32_t &m = *reinterpret_cast<uint32_t *>(vis(v).commands + 0x10);
    if (m <= i) m = i;
}
extern "C" U3X_FN void u3x_v_pose(void *v, uint32_t f, uint8_t *an, uint32_t i) {
    uint32_t *w = u3x_v_command(v, 8, 5);
    if (!w) return;
    *reinterpret_cast<uint8_t **>(w + 1) = an;
    w[3] = i;
    w[4] = f;
    u3x_v_max(v, i);
}
extern "C" U3X_FN void u3x_v_blend_cmd(void *v, uint32_t f, uint32_t a, uint32_t b, uint32_t c, uint32_t m) {
    uint32_t *w = u3x_v_command(v, 9, 6);
    if (!w) return;
    w[1] = a;
    w[2] = b;
    w[3] = c;
    w[4] = m;
    w[5] = f;
    u3x_v_max(v, a);
    u3x_v_max(v, b);
    u3x_v_max(v, c);
}
extern "C" U3X_FN void u3x_v_sub(void *v, uint32_t i) {
    uint32_t *w = u3x_v_command(v, 0xa, 2);
    if (!w) return;
    w[1] = i;
    u3x_v_max(v, i);
}
extern "C" U3X_FN void u3x_v_bind(void *v, uint32_t i) {
    uint32_t *w = u3x_v_command(v, 0xd, 2);
    if (!w) return;
    w[1] = i;
    u3x_v_max(v, i);
}
extern "C" U3X_FN void u3x_v_copy(void *v, uint32_t a, uint32_t b) {
    uint32_t *w = u3x_v_command(v, 0xf, 3);
    if (!w) return;
    w[1] = a;
    w[2] = b;
    u3x_v_max(v, a);
    u3x_v_max(v, b);
}
// ice command (0xc or 0xe): three 16-byte aligned pointers (PS3 traps otherwise: error 10)
extern "C" U3X_FN void u3x_v_ice(void *v, uint16_t type, uint32_t i, uint64_t p1, uint64_t p2, uint64_t p3) {
    if ((p1 | p2 | p3) & 0xf) {
        u3x_v_error(v, 10);
        return;
    }
    uint32_t *w = u3x_v_command(v, type, 8);
    if (!w) return;
    w[1] = i;
    *reinterpret_cast<uint64_t *>(w + 2) = p1;
    *reinterpret_cast<uint64_t *>(w + 4) = p2;
    *reinterpret_cast<uint64_t *>(w + 6) = p3;
    u3x_v_max(v, i);
}
extern "C" U3X_FN uint32_t u3x_v_frames(const uint8_t *an) {
    const uint8_t *clip = rd<const uint8_t *>(an, 0x28);
    return clip ? rd<uint16_t>(clip, 0x22) : 0;
}
extern "C" U3X_FN void u3x_v_begin(void *v_) {
    Visitor &v = vis(v_);
    u3x_frame_fresh(v.frame);
    v.abort = 0;
    if (v.kind == 0) {
        v.key = 0;
    } else if (v.kind == 2) {
        for (auto &loc : v.loc) locator_identity(loc);
        v.active = 0;
    }
}
// state: note the anim (its stream root, if any) with its phase under the key of its first element
extern "C" U3X_FN void u3x_v_push(void *v_, uint32_t ph, uint8_t *an) {
    Visitor &v = vis(v_);
    v.key = 0;
    if (!an) return;
    const uint8_t *el = rd<const uint8_t *>(an, 0x48);
    if (!el || !rd<uint32_t>(el, 4) || !rd<uint64_t>(an, 0x28)) return;
    v.key = rd<uint32_t>(el, 0);
    {
        uint32_t phase;
        if (uint8_t *root = u3h_stream_root(v.base, an, &phase)) an = root;
    }
    uint32_t &count = *reinterpret_cast<uint32_t *>(v.entries + 0x210);
    if (count > 21) {
        ++ext_ec()[102];
        return;
    }
    uint8_t *e = v.entries + static_cast<uintptr_t>(count) * 0x18;
    ++count;
    *reinterpret_cast<uint32_t *>(e) = v.key;
    *reinterpret_cast<uint8_t **>(e + 8) = an;
    e[0x15] = 0;
    *reinterpret_cast<uint32_t *>(e + 0x10) = ph;
    e[0x14] = 1;
}
// locator: sample the channel of the anim at phase ph into locator dep (0xfbaca0; identity when it fails)
extern "C" U3X_FN void u3x_v_sample(void *v_, uint32_t ph, uint8_t *an, uint32_t dep) {
    Visitor &v = vis(v_);
    if (!v.active) return;
    if (dep >= 4) {
        u3x_v_error(v_, 8);
        return;
    }
    if (!an) {
        u3x_v_error(v_, 4);
        return;
    }
    alignas(16) uint8_t req[0x28];
    for (uint8_t &b : req) b = 0;
    *reinterpret_cast<uint8_t **>(req + 8) = an;
    *reinterpret_cast<uint32_t *>(req + 0x10) = v.channel;
    *reinterpret_cast<uint32_t *>(req + 0x14) = ph;
    const uint8_t ok = static_cast<uint8_t>(game_call<uint64_t>(v.base + 0xfbaca0, 0u, static_cast<uint8_t *>(req), static_cast<uint8_t *>(v.loc[dep])));
    if (!ok) locator_identity(v.loc[dep]);
}
// slots 1 / 2: a blend command, or the locators blended (0xfd3c60: c = blend(a, b, w))
extern "C" U3X_FN void u3x_v_blend(void *v_, uint32_t w, uint32_t a, uint32_t b, uint32_t c, uint32_t m) {
    Visitor &v = vis(v_);
    const uint32_t o = static_cast<uint32_t>(v.v1);
    if (v.kind == 1) {
        u3x_v_blend_cmd(v_, w, a + o, b + o, c + o, m);
    } else if (v.kind == 2 && v.active) {
        if (a >= 4 || b >= 4 || c >= 4) {
            u3x_v_error(v_, 8);
            return;
        }
        u3x_call_float_arg(v.base + 0xfd3c60, w, reinterpret_cast<uintptr_t>(v.loc[c]), reinterpret_cast<uintptr_t>(v.loc[a]),
                           reinterpret_cast<uintptr_t>(v.loc[b]), 0);
    }
}
// slot 3: an anim
extern "C" U3X_FN void u3x_v_anim(void *v_, uint32_t ph, uint8_t *an, uint32_t dep) {
    Visitor &v = vis(v_);
    if (v.kind == 0) {
        u3x_v_push(v_, ph, an);
    } else if (v.kind == 1) {
        if (dep + static_cast<uint32_t>(v.v2) > 6) {
            u3x_v_error(v_, 6);
            return;
        }
        const uint32_t i = dep + static_cast<uint32_t>(v.v1);
        if (!an) u3x_v_bind(v_, i);
        else u3x_v_pose(v_, anim_frame(ph, an), an, i);
    } else {
        u3x_v_sample(v_, ph, an, dep);
    }
}
// slot 4: state marks the newest entry with the key; commands a sub command
extern "C" U3X_FN void u3x_v_sub0(void *v_, uint32_t x) {
    Visitor &v = vis(v_);
    if (v.kind == 0) {
        if (!v.key) return;
        for (int32_t i = static_cast<int32_t>(rd<uint32_t>(v.entries, 0x210)) - 1; i >= 0; --i) {
            uint8_t *e = v.entries + static_cast<uintptr_t>(i) * 0x18;
            if (rd<uint32_t>(e, 0) == v.key) {
                e[0x15] = 1;
                return;
            }
        }
    } else if (v.kind == 1) {
        u3x_v_sub(v_, x + static_cast<uint32_t>(v.v1));
    }
}
// slot 5
extern "C" U3X_FN void u3x_v_sub3(void *v_, uint32_t dep) {
    Visitor &v = vis(v_);
    if (v.kind != 1) return;
    if (dep + static_cast<uint32_t>(v.v2) > 6) {
        u3x_v_error(v_, 6);
        return;
    }
    u3x_v_bind(v_, dep + static_cast<uint32_t>(v.v1));
}
// slot 6: copy the layer above
extern "C" U3X_FN void u3x_v_sub4(void *v_) {
    Visitor &v = vis(v_);
    if (v.kind == 1) {
        u3x_v_copy(v_, static_cast<uint32_t>(v.v1) + 1, static_cast<uint32_t>(v.v1));
    } else if (v.kind == 2 && v.active) {
        const uint32_t i = static_cast<uint32_t>(v.v1);
        if (i + 1 >= 4) {
            u3x_v_error(v_, 8);
            return;
        }
        for (int k = 0; k < 0x30; ++k) v.loc[i][k] = v.loc[i + 1][k];
    }
}
// slot 7: the locator visitor starts at the first marker and stops at the second
extern "C" U3X_FN void u3x_v_sub2(void *v_) {
    Visitor &v = vis(v_);
    if (v.kind != 2) return;
    if (v.active) v.abort = 1;
    else v.active = 1;
}
// slot 8: a marker anim (commands: an ice command when 0xdc4240 finds the marker, else a bind)
extern "C" U3X_FN void u3x_v_mark(void *v_, uint32_t ph, uint8_t *an, uint32_t dep, int32_t p7) {
    Visitor &v = vis(v_);
    if (v.kind == 0) {
        u3x_v_push(v_, ph, an);
    } else if (v.kind == 1) {
        if (dep + static_cast<uint32_t>(v.v2) > 6) {
            u3x_v_error(v_, 6);
            return;
        }
        if (!an) {
            u3x_v_error(v_, 4);
            return;
        }
        uint64_t o1 = 0, o2 = 0;
        const uint8_t ok = static_cast<uint8_t>(u3x_call_float_arg(v.base + 0xdc4240, anim_frame(ph, an), reinterpret_cast<uintptr_t>(an),
                                                                    static_cast<uint32_t>(p7), reinterpret_cast<uintptr_t>(&o1),
                                                                    reinterpret_cast<uintptr_t>(&o2)));
        const uint32_t i = dep + static_cast<uint32_t>(v.v1);
        if (ok) {
            ++ext_ec()[103];
            u3x_v_ice(v_, 0xc, i, o1, 0, o2);
        } else {
            u3x_v_bind(v_, i);
        }
    } else {
        u3x_v_sample(v_, ph, an, dep);
    }
}

// ===== v7.191: third split, part 12 (the boosters list) =====

extern "C" {
U3H_IMPORT void u3h_j05_inc(int i);
U3H_IMPORT void u3h_booster_icons(uintptr_t base, void *s, int32_t *ev, uint8_t *l, uint32_t slot, uint32_t user, int32_t loadout, uint8_t *pr);
U3H_IMPORT int u3h_k46_op4(uintptr_t base, void *s, int32_t *ev, uint8_t *l, uint32_t slot, uint32_t user, int32_t loadout, uint8_t *pr);
U3H_IMPORT void u3h_k49_booster(uintptr_t base, void *s, uint8_t *l, uint32_t slot, uint32_t user, int32_t loadout, uint8_t *pr, int32_t row);
}

// a004 / a021: LIST_BOOSTERS (0x43). The booster lists (state +0x3000: 4 lists of 0x210 {slot, defs +8, count +0x208},
// selected per user +0x840, built bits +0x850) are built by the game (0x7596a0) once per slot. Booster def: id +4, name
// +8, loadout slot +0xc, tier count +0x10, tiers +0x18 (0x10 each, text +0), cost +0x20. The PAGE_BOOSTERS page branch
// (event[0x24] 0x89e96364) answers the "View Medal" prompt; the list rows show name, "Level n\ntier", cost or the unlock
// text, cash and the three tiers; picking a row equips the booster (PS3 f_29dea0, stripped on PS4) and saves the profile.
namespace {
U3X_RO const char no_text[] = "", one_space[] = " ", newline[] = "\n", colon_space[] = ": ", too_expensive[] = "|@Cffff0000|",
                  affordable[] = "|@Cff7cce96|", cash_colour[] = "|@Cff00ff00|";
struct Boosters {
    uint8_t lists[4][0x210];
    int32_t selected[4];
    uint32_t built;
};
U3X_FN Boosters &boosters(void *s) { return *reinterpret_cast<Boosters *>(static_cast<uint8_t *>(s) + 0x3000); }
// the booster's level for the user (0x6882b0)
U3X_FN int booster_level(uintptr_t fn, uintptr_t base, uint32_t user, uint8_t *d) {
    return reinterpret_cast<int (*)(uintptr_t, uint32_t, void *)>(fn)(base + 0x17c3b40, user, d);
}
U3X_FN uint8_t *profile_object(uintptr_t base, uint32_t user) {
    uint8_t *a = at<uint8_t *>(base + 0x17dd610);
    return a && user < 4 ? a + static_cast<uintptr_t>(user) * 0x7b00 : nullptr;
}
}  // namespace
// the list of the current loadout slot (0x17d9990, 0..3)
extern "C" U3X_FN uint8_t *u3x_booster_list(uintptr_t base, void *s) {
    uint32_t slot = at<uint32_t>(base + 0x17d9990);
    if (slot > 3) slot = 0;
    uint8_t *l = boosters(s).lists[slot];
    l[0] = static_cast<uint8_t>(slot);
    return l;
}
extern "C" U3X_FN uint8_t *u3x_booster_def(uint8_t *l, int32_t i) {
    if (i < 0 || i >= rd<int32_t>(l, 0x208)) return nullptr;
    return rd<uint8_t *>(l, 8 + static_cast<uintptr_t>(i) * 8);
}
extern "C" U3X_FN int u3x_booster_unlocked(void *s, uint32_t user, uint32_t id) {
    const uintptr_t find = state_native(s, 24), is = state_native(s, 25);
    const uintptr_t base = *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 760);
    if (!find || !is) return 0;
    void *entry = reinterpret_cast<void *(*)(uintptr_t, int, int)>(find)(base + 0x17f3068, static_cast<int>(id), 0);
    return static_cast<uint8_t>(reinterpret_cast<uint64_t (*)(uintptr_t, uint32_t, void *)>(is)(base + 0x17f3068, user, entry)) != 0;
}
extern "C" U3X_FN int u3x_boosters_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414e43u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 760) = base;
    uint32_t *ea = ext_ea();
    const bool page = ev[0x24] == static_cast<int32_t>(0x89e96364u);
    if (ev[0] == 1 && page && !(ea[0] & 8) && !(ext_eb()[1] & 0x400000u)) {   // the page title is "" before the table check
        set_row_text(ev, no_text);
        u3h_j05_inc(0);
        return 1;
    }
    if (!u3h_sym_ready(base, s, 0x916e03b0u)) {
        if (ev[0] == 6) ev[1] = -1;
        if (ev[0] == 1) u3h_j05_inc(1);
        return 1;
    }
    Boosters &bs = boosters(s);
    uint8_t *l = u3x_booster_list(base, s);
    const uint32_t user = local_user(base) & 3, slot = l[0];
    const int32_t loadout = at<int32_t>(base + 0x17d9988);
    uint8_t *pr = player_profile(base);
    const uint32_t money = pr ? rd<uint32_t>(pr, 0x74c) : 0;
    const uintptr_t level_fn = state_native(s, 23), build = state_native(s, 29);
    if (!(bs.built & (1u << slot)) && build) {
        reinterpret_cast<void (*)(void *, uint32_t)>(build)(l, loadout == 4);
        bs.built |= 1u << slot;
    }
    const int32_t row = ev[1];
    uint8_t *d;
    if (page && !(ea[0] & 8)) {   // PAGE_BOOSTERS itself: the "View Medal" prompt while the next tier is open
        ++ea[15];
        switch (ev[0]) {
        case 6: {
            ev[1] = -1;
            if (!(d = u3x_booster_def(l, bs.selected[user]))) break;
            const int lv = level_fn ? booster_level(level_fn, base, user, d) : -1;
            const int32_t tiers = rd<int32_t>(d, 0x10);
            if (!(static_cast<uint32_t>(lv) < static_cast<uint32_t>(tiers - 1))) break;
            if (u3x_booster_unlocked(s, user, rd<uint32_t>(d, 4))) ev[1] = 0;
            break;
        }
        case 2:
            if (ev[0x15] == 0) set_row_text(ev, u3h_text(base, s, 0x2239ab55u));
            break;
        case 11: ev[14] = 8; break;
        case 19: ev[14] = static_cast<int32_t>(0x92f7fefau); break;
        case 1: set_row_text(ev, no_text); break;   // no title, as on PS3
        }
        return 1;
    }
    if (ev[0] == 4 && u3h_k46_op4(base, s, ev, l, slot, user, loadout, pr)) return 1;
    if (ev[0] == 0x16) u3h_k49_booster(base, s, l, slot, user, loadout, pr, ev[0x17]);
    char *const buf = static_cast<char *>(s) + 0x2c80, *end = buf + 250;
    switch (ev[0]) {
    case 1: {
        const uint32_t id = slot == 1 ? 0x73b984c2u : slot == 3 ? 0xf917b294u : slot == 0 ? 0x77789975u : 0;
        set_row_text(ev, id ? u3h_text(base, s, id) : no_text);
        break;
    }
    case 2: {
        if (!(d = u3x_booster_def(l, row))) break;
        const uint32_t sub = static_cast<uint32_t>(ev[0x15]), id = rd<uint32_t>(d, 4);
        const int lv = level_fn ? booster_level(level_fn, base, user, d) : -1;
        const int32_t ntiers = rd<int32_t>(d, 0x10);
        const uint8_t *tiers = rd<const uint8_t *>(d, 0x18);
        char *o = buf;
        *o = 0;
        if (sub == 0) {
            set_row_text(ev, u3h_text(base, s, rd<uint32_t>(d, 8)));
            break;
        }
        if (sub == 3) {   // "Level n\ntier"
            if (lv < 0 || lv >= ntiers || !tiers) break;
            o = append_chars(o, end, u3h_text(base, s, 0xadfc7da6u));
            o = append_chars(o, end, one_space);
            o = append_number(o, end, lv + 1, false);
            o = append_chars(o, end, newline);
            o = append_chars(o, end, u3h_text(base, s, rd<uint32_t>(tiers, lv * 0x10)));
            *o = 0;
            set_row_text(ev, buf);
            break;
        }
        if (sub == 4) {   // cost, or what unlocks it
            if (!u3x_booster_unlocked(s, user, id)) {
                const uintptr_t item_text = state_native(s, 26);
                set_row_text(ev, item_text ? reinterpret_cast<const char *(*)(uint32_t, uint32_t)>(item_text)(id, 0) : no_text);
                break;
            }
            const int32_t cost = rd<int32_t>(d, 0x20);
            const uintptr_t online = state_native(s, 27);
            if (cost < 1 || !online || !static_cast<uint8_t>(reinterpret_cast<uint64_t (*)(uintptr_t)>(online)(base + 0x17cb060))) {
                set_row_text(ev, no_text);
                break;
            }
            o = append_chars(o, end, u3h_text(base, s, 0x6ba044a1u));
            o = append_chars(o, end, money < static_cast<uint32_t>(cost) ? too_expensive : affordable);
            o = append_number(o, end, cost, true);
            *o = 0;
            set_row_text(ev, buf);
            break;
        }
        if (sub == 8) {   // cash
            o = append_chars(o, end, u3h_text(base, s, 0xba6d58f7u));
            o = append_chars(o, end, cash_colour);
            o = append_number(o, end, money, true);
            *o = 0;
            set_row_text(ev, buf);
            break;
        }
        const int t = sub == 0x80d4cc7bu ? 0 : sub == 0xadacdbf3u ? 1 : sub == 0xb684d68bu ? 2 : -1;
        if (t < 0 || !tiers || t >= ntiers) break;
        if (t == 0 && ntiers == 1) {
            set_row_text(ev, u3h_text(base, s, rd<uint32_t>(tiers, 0)));
            break;
        }
        o = append_chars(o, end, u3h_text(base, s, 0xadfc7da6u));   // "Level n: tier"
        o = append_chars(o, end, one_space);
        o = append_number(o, end, t + 1, false);
        o = append_chars(o, end, colon_space);
        o = append_chars(o, end, u3h_text(base, s, rd<uint32_t>(tiers, t * 0x10)));
        *o = 0;
        set_row_text(ev, buf);
        break;
    }
    case 4:
        if (!(ea[0] & 32)) u3h_booster_icons(base, s, ev, l, slot, user, loadout, pr);
        break;
    case 12:
        if (!(ea[0] & 32)) {
            ev[0x1f] = 0x43000000;
            ev[0x20] = 0x43000000;
        }
        break;
    case 5: ev[1] = bs.selected[user]; break;
    case 6: ev[1] = rd<int32_t>(l, 0x208) - 1; break;
    case 9:
    case 0x17: {   // rebuild; the selected booster is no longer new
        if (build) {
            reinterpret_cast<void (*)(void *, uint32_t)>(build)(l, loadout == 4);
            bs.built |= 1u << slot;
        }
        if ((d = u3x_booster_def(l, bs.selected[user])))
            if (const uintptr_t seen = state_native(s, 32))
                reinterpret_cast<void (*)(uintptr_t, uint32_t, uint32_t, uint32_t)>(seen)(base + 0x17f3068, user, rd<uint32_t>(d, 4), 0);
        break;
    }
    case 10:
        if (u3x_booster_def(l, row)) bs.selected[user] = row;
        break;
    case 11: {
        if (!(d = u3x_booster_def(l, row))) break;
        const uint32_t q = static_cast<uint32_t>(ev[0x13]);
        if (q == 0x67a48eb9u) {
            if (u3x_booster_unlocked(s, user, rd<uint32_t>(d, 4)) && level_fn) ev[14] = booster_level(level_fn, base, user, d) + 1;
        } else if (q == 0x18567c73u) {
            if (const uintptr_t next = state_native(s, 31))
                ev[14] = reinterpret_cast<void *(*)(uintptr_t, uint32_t)>(next)(base + 0x17da200 + static_cast<uintptr_t>(user) * 0x1018, rd<uint32_t>(d, 4)) != nullptr;
        }
        break;
    }
    case 0x11: ev[1] = 8; break;
    case 0x16: {   // equip
        if (!(d = u3x_booster_def(l, ev[0x17]))) break;
        const uint32_t id = rd<uint32_t>(d, 4);
        if (!u3x_booster_unlocked(s, user, id)) break;
        const int32_t st = rd<int32_t>(d, 0xc);
        if (st >= 0 && st < 4) at<uint32_t>(base + 0x17c3b48 + static_cast<uintptr_t>(st) * 4) = 0;
        if (st >= 0 && st < 3 && pr) {
            if (const uintptr_t set = state_native(s, 28)) reinterpret_cast<void (*)(void *, int, uint32_t, int32_t)>(set)(pr, st + 4, id, loadout);
        } else if (st == 3) {
            at<uint32_t>(base + 0x17c3b40 + static_cast<uintptr_t>(user & 1) * 4) = id;
        }
        u3h_publish(base, s, profile_object(base, user));
        break;
    }
    }
    return 1;
}

// ===== contributor section =====
// The other contributor's C++ goes between this line and the "end of contributor section" line below (newest last).
// Claude's parts are appended at the end of the file, after the three lines that follow the end line, so a patch for
// this section and Claude's own parts never touch the same lines.
// ===== end of contributor section =====
// (keep these three lines unchanged: they separate the contributor section from the parts appended below)
// (-)
// (-)

// ===== v7.193: third split, part 13 (the out-of-bounds grid) =====

// v5.07: PS3's VIS_CHEAT reader (f_74615c / f_745b64). The killvis level's grid (item data at 0x1b3ae78, converted to
// the PS4 layout: {type, min/max x,y,z, axis order +0x1c, scale +0x20, [type 2: origin +0x30], runs at H (0x40 type 2,
// 0x30 type 1) {count, rows +8}, slice count H+8, slices H+0x10 {u16 n, u16 entries {repeat << 14 | row}}}; RLE rows:
// first byte the value, then run lengths, the value flips after each run shorter than 255). A cell is out of bounds
// when its byte is non-zero. A player whose 8 corners around (position + 1 m up) all sit in such cells, and who is not in
// one of three character states, gets the 5 s kill timer (+0x848). ExtState oob (+0x2070): [0] checks, [1] grid,
// [2] corners, [3] timers started, [4] no map gate, [5..7] test point, [8] state skips, [10] killvis sid, [13..15] the
// U3MP_OOB_TEST cell; oob2 (+0x20b0): [0] its grid, [1] test ticks.
namespace {
U3X_FN uint32_t *oob() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x2070); }
U3X_FN uint32_t *oob2() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x20b0); }
U3X_FN uint32_t grid_header(const uint8_t *g) { return rd<uint32_t>(g, 0) == 2 ? 0x40u : 0x30u; }
}  // namespace
extern "C" U3X_FN int u3x_vis_ok(const void *p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47) && !(v & 1);
}
// floor and ceil cell of one axis: trunc((round((p - o) * s) / s + o - o) * s), single precision in PS3's order
extern "C" U3X_FN void u3x_vis_axis(uint32_t p, uint32_t sc, uint32_t o, int64_t out[2]) {
    int64_t lo, hi;
    __asm__("movd %2,%%xmm0\n\tmovd %3,%%xmm3\n\tmovd %4,%%xmm4\n\t"
            "subss %%xmm4,%%xmm0\n\tmulss %%xmm3,%%xmm0\n\t"
            "roundss $9,%%xmm0,%%xmm1\n\troundss $10,%%xmm0,%%xmm2\n\t"
            "divss %%xmm3,%%xmm1\n\taddss %%xmm4,%%xmm1\n\tsubss %%xmm4,%%xmm1\n\tmulss %%xmm3,%%xmm1\n\tcvttss2si %%xmm1,%0\n\t"
            "divss %%xmm3,%%xmm2\n\taddss %%xmm4,%%xmm2\n\tsubss %%xmm4,%%xmm2\n\tmulss %%xmm3,%%xmm2\n\tcvttss2si %%xmm2,%1"
            : "=&r"(lo), "=&r"(hi) : "r"(p), "r"(sc), "r"(o) : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "cc");
    out[0] = lo;
    out[1] = hi;
}
// one cell: 2 outside the grid, 1 inside and empty (or unreadable), 0 out of bounds
extern "C" U3X_FN int u3x_vis_cell(const uint8_t *g, const int64_t c[3]) {
    const uint32_t H = grid_header(g);
    int64_t idx[3], dim[3];
    for (int i = 0; i < 3; ++i) {
        const int64_t mn = rd<int32_t>(g, 4 + 8 * i), mx = rd<int32_t>(g, 8 + 8 * i);
        if (c[i] < mn || c[i] >= mx) return 2;
        idx[i] = c[i] - mn;
        dim[i] = mx - mn;
    }
    const uint32_t a0 = g[0x1c], a1 = g[0x1d], a2 = g[0x1e];
    if (a0 > 2 || a1 > 2 || a2 > 2) return 2;
    const uint8_t *runs = rd<const uint8_t *>(g, H);
    const uint32_t slices = rd<uint32_t>(g, H + 8);
    if (!u3x_vis_ok(runs) || static_cast<uint64_t>(idx[a2]) >= slices) return 2;
    const uint8_t *sl = rd<const uint8_t *>(g, H + 0x10 + idx[a2] * 8);
    if (!u3x_vis_ok(sl)) return 2;
    const uint32_t rows = rd<uint32_t>(runs, 0), n = rd<uint16_t>(sl, 0);
    const int64_t want = idx[a1], col = idx[a0];
    int64_t r = 0;
    for (uint32_t j = 0; j < n; ++j) {
        const uint32_t e = rd<uint16_t>(sl, 2 + j * 2);
        const int64_t rep = e >> 14;
        if (want <= r + rep) {
            if ((e & 0x3fffu) >= rows) return 1;
            const uint8_t *row = rd<const uint8_t *>(runs, 8 + static_cast<uint64_t>(e & 0x3fffu) * 8);
            if (!u3x_vis_ok(row)) return 1;
            uint8_t v = row[0];
            int64_t pos = 0;
            for (int64_t k = 1; pos < dim[a0] && k <= dim[a0] * 2 + 2; ++k) {
                const uint32_t len = row[k];
                if (col < pos + len) return v == 0;
                pos += len;
                if (len != 255) v = static_cast<uint8_t>(~v);
            }
            return 1;   // a malformed row: PS3 read scratch memory; the cell counts as empty
        }
        r += rep + 1;
    }
    return 1;
}
// how many of the 8 corners around pos are out of bounds (0 without a grid of type 1 or 2)
extern "C" U3X_FN int u3x_vis_cheat_corners(uintptr_t base, const uint32_t pos[3]) {
    const uint8_t *g = at<const uint8_t *>(base + 0x1b3ae78);
    oob()[1] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g));
    if (!u3x_vis_ok(g)) return 0;
    const uint32_t t = rd<uint32_t>(g, 0);
    if (t != 1 && t != 2) return 0;
    int64_t c[3][2];
    for (int i = 0; i < 3; ++i) u3x_vis_axis(pos[i], rd<uint32_t>(g, 0x20 + 4 * i), t == 2 ? rd<uint32_t>(g, 0x30 + 4 * i) : 0u, c[i]);
    int n = 0;
    for (int k = 0; k < 8; ++k) {
        int64_t q[3];
        q[0] = c[0][k & 1];
        q[1] = c[1][(k >> 1) & 1];
        q[2] = c[2][(k >> 2) & 1];
        if (u3x_vis_cell(g, q) == 0) ++n;
    }
    return n;
}
// the object's world position (parent position + parent rotation applied to the local position) plus 1 m up
extern "C" U3X_FN void u3x_vis_test_point(uint8_t *o, uint32_t out[3]) {
    alignas(16) uint32_t r[4];
    __asm__("movups 0xe0(%1),%%xmm1\n\tmovups 0xf0(%1),%%xmm2\n\t"
            "movaps %%xmm1,%%xmm3\n\tshufps $0xc9,%%xmm3,%%xmm3\n\tmovaps %%xmm2,%%xmm4\n\tshufps $0xd2,%%xmm4,%%xmm4\n\tmulps %%xmm4,%%xmm3\n\t"
            "movaps %%xmm1,%%xmm4\n\tshufps $0xd2,%%xmm4,%%xmm4\n\tmovaps %%xmm2,%%xmm5\n\tshufps $0xc9,%%xmm5,%%xmm5\n\tmulps %%xmm5,%%xmm4\n\t"
            "subps %%xmm4,%%xmm3\n\taddps %%xmm3,%%xmm3\n\t"
            "movaps %%xmm1,%%xmm4\n\tshufps $0xc9,%%xmm4,%%xmm4\n\tmovaps %%xmm3,%%xmm5\n\tshufps $0xd2,%%xmm5,%%xmm5\n\tmulps %%xmm5,%%xmm4\n\t"
            "movaps %%xmm1,%%xmm5\n\tshufps $0xd2,%%xmm5,%%xmm5\n\tmovaps %%xmm3,%%xmm6\n\tshufps $0xc9,%%xmm6,%%xmm6\n\tmulps %%xmm6,%%xmm5\n\t"
            "subps %%xmm5,%%xmm4\n\t"
            "movaps %%xmm1,%%xmm5\n\tshufps $0xff,%%xmm5,%%xmm5\n\tmulps %%xmm3,%%xmm5\n\t"
            "addps %%xmm5,%%xmm2\n\taddps %%xmm4,%%xmm2\n\tmovups 0xd0(%1),%%xmm0\n\taddps %%xmm2,%%xmm0\n\tmovaps %%xmm0,(%0)"
            :: "r"(r), "r"(o) : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "memory");
    out[0] = r[0];
    out[2] = r[2];
    out[1] = f_add(r[1], F_ONE);
}
// the process's current state id (obj +0x80 -> [0] -> +0x10 -> +0x18) is h
extern "C" U3X_FN int u3x_obj_state_is(uint8_t *o, uint32_t h) {
    const uint8_t *a = rd<const uint8_t *>(o, 0x80);
    if (!u3x_vis_ok(a)) return 0;
    const uint8_t *b = rd<const uint8_t *>(a, 0);
    if (!u3x_vis_ok(b)) return 0;
    const uint8_t *c = rd<const uint8_t *>(b, 0x10);
    if (!u3x_vis_ok(c)) return 0;
    return rd<uint32_t>(c, 0x18) == h;
}
// the map list (0xdec97a4) entry of the current net game's map
extern "C" U3X_FN uint8_t *u3x_map_entry_cur(uintptr_t base, void *s) {
    const uint8_t *ng = u3h_netgame(base, s);
    if (!ng) return nullptr;
    const int32_t *ml = reinterpret_cast<const int32_t *>(reinterpret_cast<void *(*)(uint32_t)>(state_native(s, 13))(0xdec97a4u));
    if (!ml) return nullptr;
    uint8_t *e = rd<uint8_t *>(ml, 8);
    const uint32_t map = rd<uint32_t>(ng, 4);
    for (int32_t i = 0; e && i < ml[0] && i < 128; ++i, e += 0x98)
        if (rd<uint32_t>(e, 0) == map) return e;
    return nullptr;
}
// U3MP_OOB_TEST: the first out-of-bounds cell of the grid (cell coordinates), scanning slices, rows and runs
extern "C" U3X_FN int u3x_vis_find_kill(const uint8_t *g, int32_t c[3]) {
    const uint32_t H = grid_header(g);
    int32_t mn[3], dim[3];
    for (int i = 0; i < 3; ++i) {
        mn[i] = rd<int32_t>(g, 4 + 8 * i);
        dim[i] = rd<int32_t>(g, 8 + 8 * i) - mn[i];
        if (dim[i] <= 0) return 0;
    }
    const uint32_t a0 = g[0x1c], a1 = g[0x1d], a2 = g[0x1e];
    if (a0 > 2 || a1 > 2 || a2 > 2) return 0;
    const uint8_t *runs = rd<const uint8_t *>(g, H);
    const uint32_t slices = rd<uint32_t>(g, H + 8);
    if (!u3x_vis_ok(runs)) return 0;
    const uint32_t rows = rd<uint32_t>(runs, 0);
    for (uint32_t k = 0; k < slices && k < 4096; ++k) {
        const uint8_t *sl = rd<const uint8_t *>(g, H + 0x10 + static_cast<uint64_t>(k) * 8);
        if (!u3x_vis_ok(sl)) continue;
        const uint32_t n = rd<uint16_t>(sl, 0);
        int32_t r = 0;
        for (uint32_t j = 0; j < n; ++j) {
            const uint32_t e = rd<uint16_t>(sl, 2 + j * 2);
            const int32_t rep = static_cast<int32_t>(e >> 14);
            if ((e & 0x3fffu) < rows) {
                const uint8_t *row = rd<const uint8_t *>(runs, 8 + static_cast<uint64_t>(e & 0x3fffu) * 8);
                if (u3x_vis_ok(row)) {
                    uint8_t v = row[0];
                    int32_t pos = 0;
                    for (int32_t q = 1; pos < dim[a0] && q <= dim[a0] * 2 + 2; ++q) {
                        const uint32_t len = row[q];
                        if (v && len) {
                            int32_t idx[3];
                            idx[a0] = pos;
                            idx[a1] = r + rep / 2;
                            idx[a2] = static_cast<int32_t>(k);
                            for (int i = 0; i < 3; ++i) c[i] = idx[i] + mn[i];
                            return 1;
                        }
                        pos += static_cast<int32_t>(len);
                        if (len != 255) v = static_cast<uint8_t>(~v);
                    }
                }
            }
            r += rep + 1;
        }
    }
    return 0;
}
// cell / scale + origin as float bits (the point whose 8 corners all fall in that cell)
extern "C" U3X_FN uint32_t u3x_vis_cell_point(int32_t c, uint32_t sc, uint32_t o) { return f_add(f_div(f_from_int(c), sc), o); }
// PS3 f_363ed4's out-of-bounds condition (with a Player): 1 when the map has a killvis level (the caller then skips its
// debug trigger), 0 without one
extern "C" U3X_FN int u3x_vis_oob_tick(uintptr_t base, void *s, uint8_t *p, uint8_t *pl, int32_t now, int test) {
    int32_t &timer = *reinterpret_cast<int32_t *>(p + 0x848);
    uint32_t *d = oob();
    const uint8_t *me = u3x_map_entry_cur(base, s);
    d[10] = me ? rd<uint32_t>(me, 0x40) : 0;
    if (!me || !rd<uint32_t>(me, 0x40)) {
        ++d[4];
        return 0;
    }
    ++d[0];
    uint32_t pos[3];
    u3x_vis_test_point(pl, pos);
    if (test) {   // U3MP_OOB_TEST: the test point moves into the grid's first out-of-bounds cell
        const uint8_t *g = at<const uint8_t *>(base + 0x1b3ae78);
        if (u3x_vis_ok(g) && (rd<uint32_t>(g, 0) == 1 || rd<uint32_t>(g, 0) == 2)) {
            uint32_t *d2 = oob2();
            ++d2[1];
            if (d2[0] != static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g))) {
                d2[0] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g));
                d[13] = d[14] = d[15] = 0;
            }
            if (!d[13] && !d[14] && !d[15]) {
                int32_t c[3];
                if (u3x_vis_find_kill(g, c)) {
                    d[13] = static_cast<uint32_t>(c[0]);
                    d[14] = static_cast<uint32_t>(c[1]);
                    d[15] = static_cast<uint32_t>(c[2]);
                } else {
                    d[13] = 0x80000000u;
                }
            }
            if (d[13] != 0x80000000u) {
                const uint32_t t = rd<uint32_t>(g, 0);
                for (int i = 0; i < 3; ++i)
                    pos[i] = u3x_vis_cell_point(static_cast<int32_t>(d[13 + i]), rd<uint32_t>(g, 0x20 + 4 * i), t == 2 ? rd<uint32_t>(g, 0x30 + 4 * i) : 0u);
            }
        }
    }
    d[5] = pos[0];
    d[6] = pos[1];
    d[7] = pos[2];
    const int n = u3x_vis_cheat_corners(base, pos);
    d[2] = static_cast<uint32_t>(n);
    if (u3x_obj_state_is(pl, 0xb713a9fdu) || u3x_obj_state_is(pl, 0x50b12ba3u) || u3x_obj_state_is(pl, 0xc388fdabu)) {
        ++d[8];
        timer = 0;
        return 1;
    }
    if (n == 8) {
        if (timer <= 0) {
            timer = now + 5 * 300;
            ++d[3];
        }
    } else {
        timer = 0;
    }
    return 1;
}

// ===== v7.194: third split, part 14 (the saved online profile upload) =====

extern "C" {
U3H_IMPORT void u3h_onp_be2le(uint8_t *d, const uint8_t *s);
U3H_IMPORT void u3h_pu_seal(uint8_t *record);
U3H_IMPORT int u3h_pf_unseal(uint8_t *record);
U3H_IMPORT const char *u3h_pu_key(int secret);
U3H_IMPORT uint64_t u3h_nl_connect(uint32_t ip, uint32_t port);
U3H_IMPORT int u3h_nl_sendall(uint64_t f, const uint8_t *p, int n);
U3H_IMPORT int u3h_sock_recv(uint64_t f, void *b, int n);
U3H_IMPORT void u3h_sock_close(uint64_t f);
U3H_IMPORT char *u3h_pu_why(void);
U3H_IMPORT void *u3h_rc_mem(void);
}

// v7.64 pu01: PS3 f_364a80. After a save the game thread queues the record (pu_queue); the RPCN thread waits until no
// further save came for 8 passes, then swaps it to big-endian, seals it (Blowfish + HMAC-SHA1 with the player's own EBOOT
// keys), packs it with ND's LZ and checks the round trip, and PUTs it as /u3.final.prod/profiles/<online id>/profile.86
// with an S3 signature, in PS3 libhttp's header order (3 tries 300 ms apart). Never before the sign-in's GET succeeded.
// The upload host's address is the one the community DNS answered (ExtState room[9]).
namespace {
// RcMem fields (offsets checked against the C struct)
enum : uint32_t {
    ro_me = 125132, ro_pf_rec = 799792, ro_pf_state = 810072, ro_pu_rec = 810088, ro_pu_work = 820368, ro_pu_be = 830648,
    ro_pu_chk = 840928, ro_pu_out = 851208, ro_pu_head = 859400, ro_pu_req = 875784, ro_pu_seq = 876424,
    ro_pu_busy = 876428, ro_pu_done = 876432, ro_pu_seen = 876436, ro_pu_quiet = 876440, ro_pu_ok = 876444,
    ro_pu_fail = 876448, ro_pu_skip = 876452, ro_pu_http = 876456, ro_pu_len = 876460
};
constexpr uint32_t pu_record_bytes = 0x2828, pu_record_version = 0x56;
U3X_RO const char b64_digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
U3X_RO const char pu_days[] = "SunMonTueWedThuFriSat", month_abbr[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
U3X_RO const char put_start[] = "PUT /u3.final.prod/profiles/", put_key[] = "/profile.86 HTTP/1.1\r\nHost: ",
                  s3_host[] = "s3.amazonaws.com",
                  put_head[] = "\r\nConnection: close\r\nUser-Agent: DNTG-HTTPC/1.1\r\nContent-Type: text/plain\r\nDate: ",
                  put_auth[] = "\r\nAuthorization: AWS ", put_length[] = "\r\nContent-Length: ", put_end[] = "\r\n\r\n",
                  sign_start[] = "PUT\n\ntext/plain\n", sign_path[] = "\n/u3.final.prod/profiles/", sign_key[] = "/profile.86",
                  code_tag[] = "<Code>";
U3X_FN uint8_t *pu_field(void *m, uint32_t off) { return static_cast<uint8_t *>(m) + off; }
U3X_FN uint32_t &pu_word(void *m, uint32_t off) { return *reinterpret_cast<uint32_t *>(pu_field(m, off)); }
U3X_FN uint32_t hash3(const uint8_t *p) { return (static_cast<uint32_t>(p[0] | p[1] << 8 | p[2] << 16) * 2654435761u) >> 20; }
U3X_FN void copy_n(uint8_t *d, const uint8_t *s, uint32_t n) {
    while (n--) *d++ = *s++;
}
// literal runs of up to 32 bytes: count - 1, then the bytes
U3X_FN bool lz_literals(const uint8_t *s, uint32_t &ls, uint32_t to, uint8_t *d, uint32_t &o, uint32_t cap) {
    while (ls < to) {
        uint32_t k = to - ls;
        if (k > 32) k = 32;
        if (o + 1 + k > cap) return false;
        d[o++] = static_cast<uint8_t>(k - 1);
        copy_n(d + o, s + ls, k);
        o += k;
        ls += k;
    }
    return true;
}
// the record header PS3 writes in front: version 0x56, then 0x28 0x18
U3X_FN void record_header(uint8_t *r) {
    r[0] = r[1] = r[2] = 0;
    r[3] = static_cast<uint8_t>(pu_record_version);
    r[4] = r[5] = 0;
    r[6] = 0x28;
    r[7] = 0x18;
}
U3X_FN int pu_text(uint8_t *q, int n, const char *t) {
    while (*t) q[n++] = static_cast<uint8_t>(*t++);
    return n;
}
U3X_FN int append_two(char *o, int n, uint32_t v) {
    o[n++] = static_cast<char>('0' + v / 10);
    o[n++] = static_cast<char>('0' + v % 10);
    return n;
}
// "Sun, 01 Jan 2026 12:00:00 +0000" (RFC 1123 in UTC, as PS3 libhttp)
U3X_FN int pu_http_date(char *ds) {
    uint16_t st[8];
    for (uint16_t &v : st) v = 0;
    u3h_utc(st);
    int n = 0;
    for (int i = 0; i < 3; ++i) ds[n++] = pu_days[(st[2] % 7) * 3 + i];
    ds[n++] = ',';
    ds[n++] = ' ';
    n = append_two(ds, n, st[3]);
    ds[n++] = ' ';
    for (int i = 0; i < 3; ++i) ds[n++] = month_abbr[((st[1] + 11) % 12) * 3 + i];
    ds[n++] = ' ';
    const uint32_t y = st[0];
    ds[n++] = static_cast<char>('0' + y / 1000 % 10);
    ds[n++] = static_cast<char>('0' + y / 100 % 10);
    n = append_two(ds, n, y % 100);
    ds[n++] = ' ';
    n = append_two(ds, n, st[4]);
    ds[n++] = ':';
    n = append_two(ds, n, st[5]);
    ds[n++] = ':';
    n = append_two(ds, n, st[6]);
    ds[n++] = ' ';
    ds[n++] = '+';
    for (int i = 0; i < 4; ++i) ds[n++] = '0';
    return n;
}
}  // namespace
// ND LZ as PS3 f_7a593c reads it (c <= 0x1f: c + 1 literals; else length c >> 5 (7: + next byte) + 2 at offset
// ((c & 0x1f) << 8 | next) + 1). Greedy: the hash candidate or the previous byte (runs), matches of 3..264 bytes within
// 8192. The packed length, 0 when it does not fit.
extern "C" U3X_FN uint32_t u3x_pu_lz(const uint8_t *s, uint32_t n, uint8_t *d, uint32_t cap, int32_t *head) {
    for (int i = 0; i < 4096; ++i) head[i] = -1;
    uint32_t i = 0, o = 0, ls = 0;
    while (i < n) {
        uint32_t best = 0, boff = 0;
        if (i + 3 <= n) {
            const uint32_t h = hash3(s + i);
            for (int k = 0; k < 2; ++k) {
                const int32_t c = k ? static_cast<int32_t>(i) - 1 : head[h];
                if (c < 0 || static_cast<uint32_t>(c) >= i || i - static_cast<uint32_t>(c) > 8192u) continue;
                uint32_t len = 0;
                while (i + len < n && len < 264u && s[static_cast<uint32_t>(c) + len] == s[i + len]) ++len;
                if (len > best) {
                    best = len;
                    boff = i - static_cast<uint32_t>(c);
                }
            }
            head[h] = static_cast<int32_t>(i);
        }
        if (best < 3) {
            ++i;
            continue;
        }
        if (!lz_literals(s, ls, i, d, o, cap)) return 0;
        const uint32_t l = best - 2, of = boff - 1;
        if (o + 3 > cap) return 0;
        if (l < 7) {
            d[o++] = static_cast<uint8_t>((l << 5) | (of >> 8));
        } else {
            d[o++] = static_cast<uint8_t>(0xe0u | (of >> 8));
            d[o++] = static_cast<uint8_t>(l - 7);
        }
        d[o++] = static_cast<uint8_t>(of);
        for (uint32_t k = 1; k < best && i + k + 3 <= n; ++k) head[hash3(s + i + k)] = static_cast<int32_t>(i + k);
        i += best;
        ls = i;
    }
    if (!lz_literals(s, ls, n, d, o, cap)) return 0;
    return o;
}
extern "C" U3X_FN int u3x_pu_b64(char *o, const uint8_t *p, int n) {
    int k = 0;
    for (int i = 0; i < n; i += 3) {
        const uint32_t v = static_cast<uint32_t>(p[i]) << 16 | (i + 1 < n ? static_cast<uint32_t>(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o[k++] = b64_digits[v >> 18 & 63];
        o[k++] = b64_digits[v >> 12 & 63];
        o[k++] = i + 1 < n ? b64_digits[v >> 6 & 63] : '=';
        o[k++] = i + 2 < n ? b64_digits[v & 63] : '=';
    }
    return k;
}
// the work record -> pu_out: big-endian, sealed, packed, then unpacked and unsealed again to check it. The packed
// length, 0 = refused (the XP block +0x500 must survive the round trip).
extern "C" U3X_FN uint32_t u3x_pu_pack(void *m) {
    uint8_t *r = pu_field(m, ro_pu_be), *chk = pu_field(m, ro_pu_chk);
    u3h_onp_be2le(r, pu_field(m, ro_pu_work));
    record_header(r);
    uint8_t plain[20];
    copy_n(plain, r + 0x500, 20);
    u3h_pu_seal(r);
    const uint32_t n = u3x_pu_lz(r, pu_record_bytes, pu_field(m, ro_pu_out), 8096u, reinterpret_cast<int32_t *>(pu_field(m, ro_pu_head)));
    if (!n) return 0;
    if (u3x_lz_decode(pu_field(m, ro_pu_out), n, chk, pu_record_bytes) != pu_record_bytes) return 0;
    for (uint32_t i = 0; i < pu_record_bytes; ++i)
        if (chk[i] != r[i]) return 0;
    if (!u3h_pf_unseal(chk)) return 0;
    for (int i = 0; i < 20; ++i)
        if (chk[0x500 + i] != plain[i]) return 0;
    return n;
}
// one PUT (PS3 f_9403a8 -> 0x93da6c, S3 op 4): the HTTP status, 0 = no connection or no reply. What the server said
// (<Code> of the body, else the status line's reason) goes to pu_why.
extern "C" U3X_FN uint32_t u3x_pu_put1(void *m, uint32_t ip, uint32_t bl) {
    const char *me = reinterpret_cast<const char *>(pu_field(m, ro_me));
    char ds[40];
    const int dl = pu_http_date(ds);
    uint8_t sg[160];   // StringToSign: PUT, no MD5, type, date, /bucket/key
    int sn = pu_text(sg, 0, sign_start);
    for (int i = 0; i < dl; ++i) sg[sn++] = static_cast<uint8_t>(ds[i]);
    sn = pu_text(sg, sn, sign_path);
    for (int i = 0; me[i] && i < 16; ++i) sg[sn++] = static_cast<uint8_t>(me[i]);
    sn = pu_text(sg, sn, sign_key);
    uint8_t mac[20];
    u3x_hmac_sha1(reinterpret_cast<const uint8_t *>(u3h_pu_key(1)), 40, sg, static_cast<uint32_t>(sn), mac);
    char sig[32];
    const int sl = u3x_pu_b64(sig, mac, 20);
    const uint64_t f = u3h_nl_connect(ip, 80);
    if (f == ~0ull) return 0;
    uint8_t *q = pu_field(m, ro_pu_req);
    int n = pu_text(q, 0, put_start);
    for (int i = 0; me[i] && i < 16; ++i) q[n++] = static_cast<uint8_t>(me[i]);
    n = pu_text(q, n, put_key);
    n = pu_text(q, n, s3_host);
    n = pu_text(q, n, put_head);
    for (int i = 0; i < dl; ++i) q[n++] = static_cast<uint8_t>(ds[i]);
    n = pu_text(q, n, put_auth);
    n = pu_text(q, n, u3h_pu_key(0));
    q[n++] = ':';
    for (int i = 0; i < sl; ++i) q[n++] = static_cast<uint8_t>(sig[i]);
    n = pu_text(q, n, put_length);
    {
        char dg[12];
        int k = 0;
        uint32_t v = bl;
        do {
            dg[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v);
        while (k) q[n++] = static_cast<uint8_t>(dg[--k]);
    }
    n = pu_text(q, n, put_end);
    if (!u3h_nl_sendall(f, q, n) || !u3h_nl_sendall(f, pu_field(m, ro_pu_out), static_cast<int>(bl))) {
        u3h_sock_close(f);
        return 0;
    }
    uint8_t r[1024];
    uint32_t got = 0;
    for (;;) {
        const int k = u3h_sock_recv(f, r + got, static_cast<int>(sizeof(r) - 1 - got));
        if (k <= 0) break;
        got += static_cast<uint32_t>(k);
        if (got >= sizeof(r) - 1) break;
    }
    u3h_sock_close(f);
    uint32_t status = 0, i = 0;
    while (i < got && r[i] != ' ') ++i;
    ++i;
    while (i < got && r[i] >= '0' && r[i] <= '9') status = status * 10 + static_cast<uint32_t>(r[i++] - '0');
    char *why = u3h_pu_why();
    uint32_t a = 0, k = 0;
    for (uint32_t j = 0; j + 6 <= got; ++j) {
        bool tag = true;
        for (int t = 0; t < 6; ++t)
            if (r[j + t] != static_cast<uint8_t>(code_tag[t])) tag = false;
        if (tag) {
            a = j + 6;
            break;
        }
    }
    if (!a) {
        a = i;
        while (a < got && r[a] == ' ') ++a;
    }
    for (; a < got && k < 40 && r[a] != '<' && r[a] != '\r' && r[a] != '\n'; ++a) {
        const uint8_t c = r[a];
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.';
        why[k++] = static_cast<char>(safe ? c : '?');
    }
    why[k] = 0;
    return status;
}
// RPCN thread, each idle pass (~250 ms)
extern "C" U3X_FN void u3x_pu_tick(void *m) {
    const uint32_t q = __atomic_load_n(&pu_word(m, ro_pu_seq), __ATOMIC_ACQUIRE);
    if (q == pu_word(m, ro_pu_done)) return;
    if (q != pu_word(m, ro_pu_seen)) {
        pu_word(m, ro_pu_seen) = q;
        pu_word(m, ro_pu_quiet) = 0;
        return;
    }
    if (++pu_word(m, ro_pu_quiet) < 8 || __atomic_load_n(&pu_word(m, ro_pu_busy), __ATOMIC_ACQUIRE)) return;
    const uint32_t ps = __atomic_load_n(&pu_word(m, ro_pf_state), __ATOMIC_ACQUIRE);
    if (ps != 1u && ps != 2u) {   // the sign-in's GET did not succeed: never overwrite the server's record
        pu_word(m, ro_pu_done) = q;
        ++pu_word(m, ro_pu_skip);
        return;
    }
    const uint32_t host = reinterpret_cast<uint32_t *>(u3h_ext() + 0xbc0)[9];
    if (!pu_field(m, ro_me)[0] || !host) return;
    copy_n(pu_field(m, ro_pu_work), pu_field(m, ro_pu_rec), pu_record_bytes);
    if (__atomic_load_n(&pu_word(m, ro_pu_seq), __ATOMIC_ACQUIRE) != q || __atomic_load_n(&pu_word(m, ro_pu_busy), __ATOMIC_ACQUIRE)) return;
    pu_word(m, ro_pu_done) = q;
    const uint32_t n = u3x_pu_pack(m);
    pu_word(m, ro_pu_len) = n;
    if (!n) {
        ++pu_word(m, ro_pu_skip);
        return;
    }
    for (int k = 0; k < 3; ++k) {
        if (k) u3h_sleep_ms(300);
        const uint32_t st = u3x_pu_put1(m, reinterpret_cast<uint32_t *>(u3h_ext() + 0xbc0)[9], n);
        pu_word(m, ro_pu_http) = st;
        if (st == 200u || st == 204u) {   // the server holds this record now: the copy every MP entry loads follows it
            ++pu_word(m, ro_pu_ok);
            uint8_t *r = pu_field(m, ro_pu_be);
            u3h_onp_be2le(r, pu_field(m, ro_pu_work));
            record_header(r);
            copy_n(pu_field(m, ro_pf_rec), r, pu_record_bytes);
            return;
        }
    }
    ++pu_word(m, ro_pu_fail);
}
// game thread (mp_publish): the newest saved record for the next upload
extern "C" U3X_FN void u3x_pu_queue(const uint8_t *rec) {
    void *m = u3h_rc_mem();
    if (!m) return;
    __atomic_store_n(&pu_word(m, ro_pu_busy), 1u, __ATOMIC_RELEASE);
    copy_n(pu_field(m, ro_pu_rec), rec, pu_record_bytes);
    __atomic_store_n(&pu_word(m, ro_pu_busy), 0u, __ATOMIC_RELEASE);
    __atomic_add_fetch(&pu_word(m, ro_pu_seq), 1u, __ATOMIC_RELEASE);
}

// ===== v7.195: third split, part 15 (the death / spectate camera) =====

// v5.09 / v4.93: the camera half of PS3 f_363ed4 (after the respawn half, u3x_respawn_tick). While the local player is
// dead: fade to black while the round has not started, all players are not in yet, or a late join waits; then follow a
// player (0x7868a0) or sit on the nearest fixed camera locator; right / left step through fixed cameras or followed
// players. While following, the per-screen damage effect gets its full-health update (blood fades) and the follow camera
// is re-aimed at the target's camera frame every frame. ExtState fcam (+0x20f0): [0] effect updates, [1] last flag,
// [2] character aims, [3] player aims, [4] no frame, [5] debug camera on, [6] not a fixed camera, [7] fov, [8] other type.
namespace {
constexpr uintptr_t fixed_camera_type = 0x1ae95e0;
constexpr uint32_t spoiler_select_page = 0xd3a938b7u, pad_step_right = 0x20u, pad_step_left = 0x80u;
U3X_FN uint32_t *fcam() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0x20f0); }
U3X_FN bool handle_valid(const uint8_t *h) {
    uint8_t *const *pp = rd<uint8_t *const *>(h, 0);
    const uint32_t id = rd<uint32_t>(h, 8);
    return pp && id && *pp && rd<uint32_t>(*pp, 0x7c) == id;
}
U3X_FN uint8_t *handle_target(const uint8_t *h) { return handle_valid(h) ? *rd<uint8_t *const *>(h, 0) : nullptr; }
U3X_FN uint8_t *camera_manager(uintptr_t base, int32_t screen) {
    return handle_target(reinterpret_cast<const uint8_t *>(base + 0x1d52b80) + static_cast<int64_t>(screen) * 0x10);
}
U3X_FN uint8_t *net_player_at(uintptr_t base, int i) {
    return at<uint8_t>(base + 0x17d5c61) ? reinterpret_cast<uint8_t *>(base + 0x17cb060 + 0x390 + i * 0x9c0) : nullptr;
}
U3X_FN bool net_player_in_game(const uint8_t *q) { return q && q[0xe1] && !q[0x5e4]; }
// (ax - bx)^2 + (az - bz)^2 as float bits, sign cleared
U3X_FN uint32_t xz_distance2(const uint32_t *a, const uint32_t *b) {
    const uint32_t dx = f_sub(a[0], b[0]), dz = f_sub(a[2], b[2]);
    return f_add(f_mul(dx, dx), f_mul(dz, dz)) & 0x7fffffffu;
}
U3X_FN uint8_t follow_player(uintptr_t base, uint8_t *p, int dir, int first) {
    return game_call<uint8_t>(base + 0x7868a0, p, dir, first);
}
}  // namespace
// a bound frame's parent handle (+8, id +0x10) still names its object, as 0x7872cd..0x787308 checks it
extern "C" U3X_FN int u3x_bf_handle_valid(uintptr_t base, const uint8_t *bf) {
    uint8_t *const *h = rd<uint8_t *const *>(bf, 8);
    const uint32_t id = rd<uint32_t>(bf, 0x10);
    if (!h || !id) return 0;
    const uint8_t *o = *h;
    if (!o) return 0;
    const uint32_t oid = rd<uint32_t>(o, 0x50);
    if (oid != id && (id > 0xfffe || (oid & 0xffff) != id)) return 0;
    const uintptr_t table = at<uintptr_t>(base + 0x16e2f30);
    return rd<uint16_t>(o, 0x54) == static_cast<uint16_t>((reinterpret_cast<uintptr_t>(h) - table) >> 3);
}
// the screen's damage effect update (vt slot 5, 0xc1ccc0) with full health; the flag is "still hurt" as PS4's Player
// update builds it (the hurt timer +0x9a0 running, effect 0x116b3043, or byte 0x17d7a50+0xb5)
extern "C" U3X_FN void u3x_follow_fx(uintptr_t base, void *s, uint8_t *p) {
    const int32_t sc = rd<int32_t>(p, 0x1e8);
    if (sc < 0 || sc > 1) return;
    uint8_t *o = at<uint8_t *>(base + 0x16fd020 + static_cast<uint64_t>(sc) * 8);
    if (!o) return;
    const uintptr_t *vt = rd<const uintptr_t *>(o, 0);
    if (!vt || vt[0x28 / 8] != base + 0xc1ccc0) return;
    if (!code_starts(base, 0xc1ccc0, usual_prologue)) return;
    uint32_t flag = 1;
    const uint8_t *clock = at<const uint8_t *>(base + 0x17c3580);
    if (!clock || rd<int32_t>(p, 0x9a0) - rd<int32_t>(clock, 0xc) < 1)
        flag = u3h_np_has_effect(s, p, 0x116b3043u) ? 1u : (at<uint8_t>(base + 0x17d7a50 + 0xb5) != 0);
    u3x_call_float_arg(base + 0xc1ccc0, F_ONE, reinterpret_cast<uint64_t>(o), flag, 0, 0);
    ++fcam()[0];
    fcam()[1] = flag;
}
// aim the follow camera (a fixed camera at p+0x388) at q's camera frame: a NetCharacter's through 0x685b80, a Player's
// screen camera locator (camera manager +0x4c0); fov starts at 75. A stale parent handle keeps the cached locator.
extern "C" U3X_FN void u3x_follow_aim(uintptr_t base, void *, uint8_t *p, uint8_t *q) {
    uint32_t *d = fcam();
    uint8_t *ch = handle_target(q + 0x10);
    if (!ch) {
        ++d[4];
        return;
    }
    alignas(16) uint32_t fr[24];   // a fresh bound frame
    for (uint32_t &w : fr) w = 0;
    fr[0] = fr[1] = 0x7fccccccu;
    fr[15] = F_ONE;
    fr[23] = F_ONE;
    uint32_t fov = 0x42960000u;
    const uint32_t type = rd<uint32_t>(ch, 0x30);
    if (type == 0xf2c86c22u) {
        if (!code_starts(base, 0x685b80, usual_prologue) || !game_call<uint8_t>(base + 0x685b80, ch + 0x400, static_cast<void *>(fr), static_cast<void *>(&fov))) {
            ++d[4];
            return;
        }
        ++d[2];
    } else if (type == 0xa5a63e9bu) {
        const uint8_t *cm = camera_manager(base, rd<int32_t>(q, 0x1e8));
        if (!cm) {
            ++d[4];
            return;
        }
        for (int i = 0; i < 8; ++i) fr[16 + i] = rd<uint32_t>(cm, 0x4c0 + i * 4);
        ++d[3];
    } else {
        ++d[8];   // PS3 aims at the identity frame then
    }
    if (at<uint8_t>(base + 0x17d7b10)) {
        ++d[5];
        return;
    }
    uint8_t *cam = handle_target(p + 0x388);
    if (!cam) return;
    if (rd<uintptr_t>(cam, 0xf0) != base + fixed_camera_type) {
        ++d[6];
        return;
    }
    if ((fr[2] || fr[3]) && !u3x_bf_handle_valid(base, reinterpret_cast<const uint8_t *>(fr))) {
        fr[2] = fr[3] = 0;
        fr[4] = 0;
    }
    uint32_t *cb = reinterpret_cast<uint32_t *>(cam + 0x230);
    for (int i = 2; i < 24; ++i) cb[i] = fr[i];   // the header words stay
    *reinterpret_cast<uint32_t *>(cam + 0x294) = fov;
    d[7] = fov;
}
// following: keep aiming at the followed player while it may be followed (any team in Elimination or with the "all"
// option, else the same team, or the co-op hunters' team 1), else pick the next player or fall back to the fixed camera
extern "C" U3X_FN void u3x_follow_update(uintptr_t base, void *s, uint8_t *p) {
    if (!p[0x381] || !handle_valid(p + 0x388)) return;
    if (!code_starts(base, 0x7868a0, usual_prologue)) return;
    u3x_follow_fx(base, s, p);
    const bool any = u3h_netgame_type(base, s) == 10 || at<uint8_t>(base + 0x17cb060 + 0x34e);
    const int32_t qi = rd<int32_t>(p, 0x37c);
    uint8_t *q = (qi >= 0 && qi < 10) ? net_player_at(base, qi) : nullptr;
    if (q && net_player_in_game(q) && u3h_np_alive(s, q) &&
        (any || rd<int32_t>(q, 0x21c) == rd<int32_t>(p, 0x21c) || (u3h_net_coop_mode(base, s) && rd<int32_t>(p, 0x21c) == 1))) {
        u3x_follow_aim(base, s, p, q);
        return;
    }
    if (!follow_player(base, p, 1, 0)) {
        p[0x381] = 0;
        p[0x380] = 0;
        u3h_cam_fixed(base, s, p, rd<int32_t>(p, 0x378));
    }
    ++ext_coop()[16];
}
extern "C" U3X_FN void u3x_camera_tick(uintptr_t base, void *s, uint8_t *p, int) {
    uint8_t *mgr = reinterpret_cast<uint8_t *>(base + 0x17cb060);
    const int32_t now = at<int32_t>(base + 0x17b4ca4), t344 = rd<int32_t>(p, 0x39c), team = rd<int32_t>(p, 0x21c);
    const int coop = u3h_net_coop_mode(base, s);
    bool late = u3h_net_late_joining(base) && (!coop || team != 1);
    if (mgr[0x6a7c]) u3h_spawn_fade(base, s, p, 1);
    if (late && !(ext_ec()[0] & 2) && u3h_netgame_type(base, s) == 10) late = false;   // 1.19: no late-join fade in Elimination
    if (late) u3h_spawn_fade(base, s, p, 1);
    if (code_starts(base, 0x746ab0, usual_prologue) && (!mgr[0x31e] || !game_call<uint8_t>(base + 0x746ab0, mgr))) {   // not all in yet
        u3h_spawn_fade(base, s, p, 1);
        ++reinterpret_cast<uint32_t *>(u3h_ext() + 0x2070)[12];
    }
    uint8_t *menu = u3x_screen_menu(base, rd<int32_t>(p, 0x1e8));
    if (u3h_np_alive(s, p) || late || mgr[0x6a24]) {   // alive: the spoiler page closes
        if (u3x_menu_page(menu) == spoiler_select_page) {
            u3x_menu_back(base, menu, 0);
            u3x_menu_close(base, menu);
        }
        return;
    }
    if ((!t344 || now - t344 > 0x5dc) && coop && team == 1 && u3x_coop_spectate(base, s, p, menu)) return;
    if (!code_starts(base, 0x7868a0, usual_prologue)) return;
    if (!camera_manager(base, rd<int32_t>(p, 0x1e8))) return;   // PS3 asserts without one (pre-load state)
    const bool gone = now - t344 > 0x5dc || !handle_valid(p + 0x10);
    if (!p[0x381]) {
        bool following = false;
        if (gone) {
            if (follow_player(base, p, 1, 1)) {
                p[0x380] = 0;
                ++reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3d78)[12];
            }
            following = p[0x381] != 0;
        }
        if (!following && handle_valid(p + 0x388)) following = true;
        if (!following) {
            if (!gone) return;
            // someone else (not a co-op hunter) is alive, or only one player is in: the nearest fixed camera (XZ)
            bool alive_other = false;
            int count = 0;
            for (int i = 0; i < 10; ++i) {
                const uint8_t *q = net_player_at(base, i);
                if (!net_player_in_game(q)) continue;
                ++count;
                if (!(coop && rd<int32_t>(q, 0x21c) == 1) && rd<int32_t>(q, 0x39c) <= rd<int32_t>(q, 0x3a0)) alive_other = true;
            }
            if (!alive_other && count != 1) return;
            if (!code_starts(base, 0x73a4e0, usual_prologue) || !code_starts(base, 0x10e6f30, usual_prologue)) return;
            game_call<void>(base + 0x73a4e0, mgr);
            const int32_t n = rd<int32_t>(mgr, 0x6a20);
            int32_t best = 0;
            uint32_t best_d = 0x7f7fffffu;
            for (int32_t i = 0; i < n && i < 0x41; ++i) {
                alignas(16) uint32_t l[8];
                uint8_t *e = rd<uint8_t *>(mgr, 0x6820 + i * 8);
                if (!e) continue;
                game_call<void>(base + 0x10e6f30, static_cast<void *>(l), e);
                const uint32_t dd = xz_distance2(reinterpret_cast<const uint32_t *>(p), l);
                if (dd < best_d) {
                    best_d = dd;
                    best = i;
                }
            }
            if (u3h_cam_fixed(base, s, p, best)) {
                p[0x381] = 0;
                *reinterpret_cast<int32_t *>(p + 0x37c) = -1;
                p[0x380] = 0;
                if (uint8_t *o = handle_target(p + 0x10)) reinterpret_cast<void (*)(uint8_t *)>(state_native(s, 57))(o);
            }
            return;
        }
    }
    const bool fol = p[0x381] != 0;   // following a player on arrival here
    if (p[0x380] && now - rd<int32_t>(p, 0x384) >= 0x5dd) {
        u3h_cam_fixed(base, s, p, rd<int32_t>(p, 0x378) + 1);
    } else {
        const uint8_t *m0 = at<const uint8_t *>(base + 0x16e3188);
        if (m0 && rd<uint32_t>(m0, 8) == 0) {   // no menu up: right / left step the camera or the followed player
            const uint32_t b = u3x_pad_pressed(base, rd<int32_t>(p, 0x1e8));
            const bool r = (b & pad_step_right) != 0, l = (b & pad_step_left) != 0;
            int dir = 0;
            if (r && !fol) {
                p[0x381] = 0;
                p[0x380] = 0;
                u3h_cam_fixed(base, s, p, rd<int32_t>(p, 0x378) + 1);
                ++ext_coop()[14];
            } else if (!l) {
                if (r) dir = -1;
            } else if (!fol) {
                p[0x381] = 0;
                p[0x380] = 0;
                u3h_cam_fixed(base, s, p, rd<int32_t>(p, 0x378) - 1);
                ++ext_coop()[14];
            } else {
                dir = 1;
            }
            if (dir) {
                p[0x380] = 0;
                if (follow_player(base, p, dir, 0)) p[0x381] = 1;
                ++ext_coop()[15];
            }
        }
    }
    u3x_follow_update(base, s, p);
}

// ===== v7.196: third split, part 16 (the lobby's package want list: kits and taunts) =====

extern "C" {
U3H_IMPORT void u3h_s19_count(int what, uint32_t v);
}

// v4.69 / v4.95: PS3 f_411444 every frontend frame: the wanted kit models of both local players (PlayerCustomManager
// 0x17f9938: names at +0xe00 + player * 0x58, 11 slots) go into its package list (+4 sids / +0xcc names of 64 bytes,
// 50 entries), entries no current look or taunt slot owns are dropped first (U3MP_PKG_PRUNE, eb[2] bit 28), and the list is
// requested (0x996db0) when it changed (or every 64 ticks while the lobby shows). Optional (ec[0] bit 4): the equipped
// taunts' packages ("multi-celebrate-<name>": the 29 taunts 1.19 added, which PS4 has no table of). The DLC packs whose
// content ships in the overlay are marked installed (U3MP_DLC_PACKS=0, state +0x20c0 bit 7, leaves them).
// State +0x3e40: [0] ticks, [1] list changes, [2] requests, [3] used entries, [4..47] per-slot / first failing slot.
// ExtState taunt (+0xb80): {index + 1, sid} for the three taunt slots, [8] added, [9] / [10] unknown taunt, [11] dropped
// failing packs, [12..15] packs that failed to load.
namespace {
U3X_RO const char taunt_names[] =
    "carlton\0fffuuuuu-\0arm-wave\0mj-1\0throat-slit\0mj-2\0haka\0heisman\0air-guitar\0facepalm\0take-a-bow\0"
    "high-kick\0hillbilly-shuffle\0mic-drop\0school-fight\0come-at-me-bro\0backflip-squat\0jump-freeze\0"
    "river-dance\0cossack-dance\0six-flags\0carlton-2\0bolt\0freddie\0dougie\0cabbage-patch\0cheerleader\0gangnam\0"
    "top-model\0";
U3X_RO const char taunt_prefix[] = "multi-celebrate-";
U3X_RO const uint8_t dlc_packs[] = {19,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,
                                    41,  42,  43,  44,  45,  46,  47,  48,  50,  70,  53,  54,  56,  57,  58,  59,  60,  72,  73,
                                    74,  75,  76,  77,  78,  95,  97,  99,  20,  55,  66,  67,  90,  15,  79,  80,  104, 105, 106,
                                    107, 108, 109, 111, 112, 113, 114, 115, 116, 117, 118, 119, 121, 122, 123, 124};
constexpr uint32_t no_part_sid = 0xf114b39du;
U3X_FN uint32_t *taunt_state() { return reinterpret_cast<uint32_t *>(u3h_ext() + 0xb80); }
U3X_FN bool pointer_ok(const void *p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    return v >= 0x10000u && v < (1ull << 47);
}
U3X_FN uint32_t &list_sid(uint8_t *m, int i) { return *reinterpret_cast<uint32_t *>(m + 4 + i * 4); }
U3X_FN char *list_name(uint8_t *m, int i) { return reinterpret_cast<char *>(m + 0xcc + i * 0x40); }
U3X_FN void list_put(uint8_t *m, int i, const char *n, uint32_t sid) {
    char *dst = list_name(m, i);
    int j = 0;
    for (; j < 63 && n[j]; ++j) dst[j] = n[j];
    dst[j] = 0;
    list_sid(m, i) = sid;
}
U3X_FN uint8_t *find_actor(uintptr_t afind, uintptr_t base, uint32_t sid) {
    return reinterpret_cast<uint8_t *(*)(uintptr_t, uint32_t)>(afind)(base + 0x1afcf10, sid);
}
}  // namespace
// the package name of taunt sid (one of the 29 newer taunts), 0 for none, the default taunt or a pack that failed
extern "C" U3X_FN const char *u3x_pkg_taunt_name(uint32_t sid, char out[64]) {
    uint32_t *ts = taunt_state();
    if (!sid || sid == 0x0e69839du) return nullptr;
    for (int i = 12; i < 16; ++i)
        if (ts[i] == sid) return nullptr;
    for (const char *t = taunt_names; *t;) {
        int n = 0;
        for (; taunt_prefix[n]; ++n) out[n] = taunt_prefix[n];
        int k = 0;
        for (; t[k] && n < 63; ++k) out[n++] = t[k];
        out[n] = 0;
        if (u3h_crc_sid(out) == sid) return out;
        t += k + 1;
    }
    ++ts[9];
    ts[10] = sid;
    return nullptr;
}
// slot = {list index + 1, sid}: the entry this source owns, rewritten in place. 1 when the list changed.
extern "C" U3X_FN int u3x_pkg_taunt_slot(uint8_t *m, uint32_t *slot, const char *n) {
    const uint32_t sid = n ? u3h_crc_sid(n) : 0;
    int own = -1;
    if (slot[0] && slot[0] <= 50 && list_sid(m, static_cast<int>(slot[0]) - 1) == slot[1]) own = static_cast<int>(slot[0]) - 1;
    if (own >= 0 && slot[1] == sid) return 0;
    int i = 50, free = -1;
    if (sid)
        for (i = 0; i < 50; ++i) {
            const uint32_t v = list_sid(m, i);
            if (v == sid && i != own) break;
            if (!v && free < 0) free = i;
        }
    int changed = 0;
    if (own >= 0) {
        list_sid(m, own) = 0;
        list_name(m, own)[0] = 0;
        changed = 1;
        if (free < 0 || own < free) free = own;
    }
    slot[0] = slot[1] = 0;
    if (!sid || i < 50 || free < 0) return changed;   // none, already listed by another source, or the list is full
    list_put(m, free, n, sid);
    slot[0] = static_cast<uint32_t>(free) + 1;
    slot[1] = sid;
    ++taunt_state()[8];
    return 1;
}
extern "C" U3X_FN int u3x_pkg_taunt_tick(uintptr_t base, void *s, uint8_t *m) {
    uint32_t *ts = taunt_state();
    const uintptr_t afind = state_native(s, 50);
    int changed = 0;
    char nb[64];
    if (!(ext_ec()[0] & 16)) {   // off (1.19's per-player taunt want covers it): release the slots
        for (int k = 0; k < 3; ++k) changed |= u3x_pkg_taunt_slot(m, ts + k * 2, nullptr);
        return changed;
    }
    if (afind)   // a pack that failed to load (state 0xc) would hold the scene's package wait: drop it for good
        for (int k = 0; k < 3; ++k) {
            uint32_t *sl = ts + k * 2;
            if (!sl[1]) continue;
            const uint8_t *a = find_actor(afind, base, sl[1]);
            if (pointer_ok(a) && rd<uint32_t>(a, 0x30) == 12) {
                int f = 12;
                while (f < 15 && ts[f]) ++f;
                ts[f] = sl[1];
                ++ts[11];
                changed |= u3x_pkg_taunt_slot(m, sl, nullptr);
            }
        }
    const uint8_t *a = at<const uint8_t *>(base + 0x17dd610);
    for (int p = 0; p < 2; ++p) {
        const char *n = nullptr;
        if (pointer_ok(a) && rd<uint32_t>(a, static_cast<uintptr_t>(p) * 0x7b00u + 0x7ae8) == 3)
            n = u3x_pkg_taunt_name(rd<uint32_t>(a, static_cast<uintptr_t>(p) * 0x7b00u + 0x784), nb);
        changed |= u3x_pkg_taunt_slot(m, ts + p * 2, n);
    }
    changed |= u3x_pkg_taunt_slot(m, ts + 4, nullptr);   // the old hovered-row slot, released
    return changed;
}
extern "C" U3X_FN void u3x_loadout_tick(uintptr_t base, void *s) {
    uint32_t *d = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(s) + 0x3e40);
    uint8_t *m = reinterpret_cast<uint8_t *>(base + 0x17f9938);
    const uintptr_t req = state_native(s, 49), afind = state_native(s, 50);
    if (!req) return;
    at<uint8_t>(base + 0x1afcf0a) = 1;
    if (at<int32_t>(base + 0x17cb3b0) == 0) {   // the saved rand seed, in case a spawn comes before NetInit
        at<int32_t>(base + 0x17cb3b0) = -1;
        ++ext_ec()[30];
    }
    if (!(state_switches(s) & 0x80))
        for (uint8_t pack : dlc_packs) at<uint8_t>(base + 0x17c3a40 + pack) = 1;
    ++d[0];
    int changed = 0;
    const bool lobby = m[0xf70] != 0;
    if (lobby && !(ext_eb()[2] & 0x10000000u)) {   // drop entries no current look and no taunt slot owns
        uint32_t cur[22];
        int nc = 0;
        for (int p = 0; p < 2; ++p)
            for (int k = 0; k < 11; ++k) {
                const char *n = rd<const char *>(m, 0xe00 + p * 0x58 + k * 8);
                if (n && *n) {
                    const uint32_t sid = u3h_crc_sid(n);
                    if (sid != no_part_sid) cur[nc++] = sid;
                }
            }
        const uint32_t *ts = taunt_state();
        for (int i = 0; i < 50; ++i) {
            const uint32_t v = list_sid(m, i);
            if (!v) continue;
            bool keep = false;
            for (int c = 0; c < nc && !keep; ++c) keep = cur[c] == v;
            for (int k = 0; k < 3 && !keep; ++k) keep = ts[k * 2] == static_cast<uint32_t>(i) + 1 && ts[k * 2 + 1] == v;
            if (keep) continue;
            list_sid(m, i) = 0;
            list_name(m, i)[0] = 0;
            changed = 1;
            u3h_s19_count(0, 0);
        }
    }
    if (lobby)   // the current parts in
        for (int p = 0; p < 2; ++p)
            for (int k = 0; k < 11; ++k) {
                const char *n = rd<const char *>(m, 0xe00 + p * 0x58 + k * 8);
                if (!n || !*n) continue;
                const uint32_t sid = u3h_crc_sid(n);
                if (sid == no_part_sid) continue;
                int i, free = -1;
                for (i = 0; i < 50; ++i) {
                    const uint32_t v = list_sid(m, i);
                    if (v == sid) break;
                    if (!v && free < 0) free = i;
                }
                if (i < 50) continue;
                if (free < 0) {   // the list is full: this part is never wanted
                    u3h_s19_count(1, 0);
                    continue;
                }
                list_put(m, free, n, sid);
                changed = 1;
            }
    if (m[0xf70] && u3x_pkg_taunt_tick(base, s, m)) changed = 1;
    uint32_t used = 0;
    for (int i = 0; i < 50; ++i)
        if (list_sid(m, i)) ++used;
    d[3] = used;
    u3h_s19_count(2, used);
    // PS3 asks again only while +0xf70 is set; state +0x20c0 bit 1 (U3MP_KEEP_LOADOUT) also during the match
    if (changed || (used && (d[0] & 63) == 0 && (m[0xf70] || (state_switches(s) & 2)))) {
        if (changed) ++d[1];
        reinterpret_cast<void (*)(uint8_t *)>(req)(m);
        ++d[2];
    }
    if (afind)   // per player / slot {sid, level state | request statuses << 8 | request count << 24}
        for (int p = 0; p < 2; ++p)
            for (int k = 0; k < 11; ++k) {
                uint32_t *e = d + 4 + (p * 11 + k) * 2;
                const char *n = m[0xf70] ? rd<const char *>(m, 0xe00 + p * 0x58 + k * 8) : nullptr;
                if (n && *n) e[0] = u3h_crc_sid(n);
                const uint8_t *a = e[0] ? find_actor(afind, base, e[0]) : nullptr;
                const uint32_t st = a ? rd<uint32_t>(a, 0x30) : 0xffu, cnt = a ? rd<uint32_t>(a, 0x78) : 0;
                uint32_t rq = 0xffffu;
                for (uint32_t q = 0; q < 2 && q < cnt; ++q) {
                    const uint8_t *r = rd<const uint8_t *>(a, 0x80 + q * 8);
                    const uint32_t v = r ? (rd<uint32_t>(r, 0x20) & 0xff) : 0xfe;
                    rq = (rq & ~(0xffu << (q * 8))) | v << (q * 8);
                }
                e[1] = (st & 0xff) | (rq & 0xffff) << 8 | (cnt & 0xff) << 24;
            }
    if (afind)   // [44..47]: the first slot that failed to load
        for (int i = 0; i < 20; ++i) {
            const uint32_t *e = d + 4 + i * 2;
            if ((e[1] & 0xff) != 12) continue;
            const uint8_t *a = find_actor(afind, base, e[0]);
            if (!a) break;
            const uint32_t cnt = rd<uint32_t>(a, 0x78);
            uint32_t lo = 0, hi = 0;
            for (uint32_t q = 0; q < cnt && q < 16; ++q) {
                const uint8_t *r = rd<const uint8_t *>(a, 0x80 + q * 8);
                const uint32_t v = r ? (rd<uint32_t>(r, 0x20) & 0xf) : 0xf;
                if (q < 8) lo |= v << (q * 4);
                else hi |= v << ((q - 8) * 4);
            }
            const uint8_t *def = rd<const uint8_t *>(a, 0x110);
            d[44] = e[0];
            d[45] = lo;
            d[46] = hi | (cnt << 24);
            d[47] = def ? (0x80000000u | rd<uint16_t>(def, 0x14) | static_cast<uint32_t>(rd<uint16_t>(def, 0x18)) << 16) : 0;
            break;
        }
}

// ===== v7.197: fourth split, part 1 (the anim state decoder) =====

// c009: 1.19 0x3869cc. The anim bitstream of a player state message drives one of the visitors (u3x_v_*): a header
// (wide ids, an optional 13-bit id, the bound frame, with flags bit 0 also a camera frame, fov byte and aim point), the
// anim list (n ids of 12 or 32 bits and their art group types, 7 = a 32-bit art group sid), then commands of 2 bits:
// 0 / 1 blend of the two newest (0 with an 8-bit weight), 2 anim, 3 with a 3-bit sub command (0 sub, 1 end, 2 marker,
// 3 bind, 4 copy, 5 marker anim). An anim's phase is 4, 8 or 16 bits by its frame count (16 for wide ids); a streamed
// anim (+0x24 bit 2) takes the stream's root and phase (0xfd4c60). PS3 traps become Visitor.error: 1 read past the end,
// 2 blend without two anims, 3 no art group, 4 no anim, 5 sub past the depth, 11 index past the list, 12 empty.
// ec[101] the bits read.
extern "C" U3X_FN void u3x_decode_anim(void *v_, uint8_t *bits, uint32_t nbytes, uint32_t flags, uint8_t **tabs, int32_t a4, int32_t a5,
                                       int32_t a6, int32_t a7) {
    Visitor &v = *static_cast<Visitor *>(v_);
    const uintptr_t base = v.base;
    u3x_v_begin(v_);
    v.v1 = a4;
    v.v2 = a5;
    if (!nbytes) {   // a sender without anim bits (PS4 / older helpers); PS3 never sends one
        u3x_v_error(v_, 12);
        return;
    }
    BitReader r;
    u3x_bits_init(&r, bits, nbytes);
    const bool wide = u3x_bits_read(&r, 1) != 0;
    v.v60 = u3x_bits_read(&r, 1) ? u3x_bits_read(&r, 13) : 0;
    u3x_bits_frame(base, &r, v.frame);
    if (flags & 1) {   // camera frame, fov byte, aim point: read past
        alignas(16) uint8_t tf[0x60];
        alignas(16) uint8_t tv[0x10];
        u3x_frame_fresh(tf);
        u3x_bits_frame(base, &r, tf);
        u3x_bits_read(&r, 8);
        u3x_bits_vector(base, &r, tv);
    }
    const uint32_t n = u3x_bits_read(&r, 5);
    const bool direct = u3x_bits_read(&r, 1) != 0;   // the anims come in list order
    uint32_t ids[32], types[32];
    for (uint32_t i = 0; i < n; ++i) ids[i] = u3x_bits_read(&r, wide ? 32 : 12);
    if (u3x_bits_read(&r, 1)) {   // one type for all
        uint32_t t = u3x_bits_read(&r, 3);
        if (t == 7) t = u3x_bits_read(&r, 32);
        for (uint32_t i = 0; i < n; ++i) types[i] = t;
    } else {
        for (uint32_t i = 0; i < n; ++i) {
            types[i] = u3x_bits_read(&r, 3);
            if (types[i] == 7) types[i] = u3x_bits_read(&r, 32);
        }
    }
    const uint32_t index_bits = n <= 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : n <= 32 ? 5 : 0;
    uint32_t sub = 6, code = u3x_bits_read(&r, 2);
    if (code == 3) sub = u3x_bits_read(&r, 3);
    uint32_t ix = 0, dep = 0;
    while (!v.abort && sub != 1) {
        if (r.err) {
            u3x_v_error(v_, 1);
            break;
        }
        if (v.error) break;
        if (code == 2 || (code == 3 && sub == 5)) {   // anim / marker anim
            if (!direct) ix = u3x_bits_read(&r, index_bits);
            if (ix >= n) {
                u3x_v_error(v_, 11);
                break;
            }
            const uint32_t t = types[ix], id = ids[ix];
            uint8_t *group = t < 7 ? tabs[t] : u3x_art_group_sid(base, t);
            if (!group) {
                u3x_v_error(v_, 3);
                break;
            }
            uint8_t *an;
            if (id == 0xfff) an = tabs[0] ? u3x_anim_by_index(tabs[0], a6) : nullptr;
            else if (!wide) an = u3x_anim_by_index(group, static_cast<int32_t>(id));
            else an = u3x_anim_by_sid(group, id);
            if (!an || !rd<uint8_t *>(an, 0x28)) {
                u3x_v_error(v_, 4);
                break;
            }
            uint32_t ph;
            if (!wide) {   // 4, 8 or 16 bits of phase by the frame count
                const uint32_t fr = u3x_v_frames(an);
                if (fr <= 8) ph = f_mul(f_from_int(static_cast<int32_t>(u3x_bits_read(&r, 4))), 0x3d888889u);
                else if (fr <= 64) ph = f_mul(f_from_int(static_cast<int32_t>(u3x_bits_read(&r, 8))), 0x3b808081u);
                else ph = f_mul(f_from_int(static_cast<int32_t>(u3x_bits_read(&r, 16))), 0x37800080u);
            } else {
                ph = f_mul(f_from_int(static_cast<int32_t>(u3x_bits_read(&r, 16))), 0x37800080u);
            }
            if (rd<uint32_t>(an, 0x24) & 4) {   // a streamed anim
                const uint32_t id4 = rd<uint32_t>(an, 0x18);
                const uint8_t *rec = u3x_stream_record(base, id4);
                if (rec) {
                    ph = rd<uint32_t>(rec, 0x20);
                    an = game_call<uint8_t *>(base + 0xfd4c60, an, id4, &ph);
                } else {
                    ph = 0;
                    an = nullptr;
                }
            }
            if (r.err) {
                u3x_v_error(v_, 1);
                break;
            }
            ++ix;
            if (code == 2) u3x_v_anim(v_, ph, an, dep);
            else u3x_v_mark(v_, ph, an, dep, a7);
            ++dep;
        } else if (code < 2) {   // blend
            if (dep < 2) {
                u3x_v_error(v_, 2);
                break;
            }
            const bool b1 = u3x_bits_read(&r, 1) != 0;
            const uint32_t a = b1 ? dep - 1 : dep - 2, b = b1 ? dep - 2 : dep - 1;
            uint32_t c;
            bool lower;
            if (u3x_bits_read(&r, 1) != 0) {
                c = a;
                lower = a < b;
            } else {
                c = b;
                lower = b < a;
            }
            if (lower) --dep;
            const uint32_t m = u3x_bits_read(&r, 2), mode = m == 1 ? 2 : (m == 2 ? 6 : 1);
            const uint32_t w = code == 0 ? f_mul(f_from_int(static_cast<int32_t>(u3x_bits_read(&r, 8))), 0x3b808081u) : F_ONE;
            if (r.err) {
                u3x_v_error(v_, 1);
                break;
            }
            u3x_v_blend(v_, w, a, b, c, mode);
        } else if (code == 3 && sub < 6) {   // sub commands
            if (sub == 0) {
                const uint32_t d = u3x_bits_read(&r, 2);
                if (dep - 1 < d) {
                    u3x_v_error(v_, 5);
                    break;
                }
                u3x_v_sub0(v_, dep - d - 1);
            } else if (sub == 2) {
                u3x_v_sub2(v_);
            } else if (sub == 3) {
                u3x_v_sub3(v_, dep);
                ++dep;
            } else if (sub == 4) {
                --dep;
                u3x_v_sub4(v_);
            }
        }
        code = u3x_bits_read(&r, 2);
        if (code == 3) sub = u3x_bits_read(&r, 3);
    }
    if (r.err && !v.error) u3x_v_error(v_, 1);
    ext_ec()[101] = r.pos;
}

// ===== v7.198: fourth split, part 2 (the custom game's Advanced Settings) =====

extern "C" {
U3H_IMPORT void u3h_cs_tick(uintptr_t base, void *s);
U3H_IMPORT uint8_t *u3h_custom_map(uintptr_t base, void *s, int32_t *gametype);
}

// v5.45: 1.19's Advanced Settings (PAGE_LOBBY's button, datasource tag 0x4c414f38, and PAGE_CUSTOMRULES2's rows
// 0x4c414fe7..ef). The custom-game values 1.19 keeps at NetGameManager +0x5544 live in ExtState adv (+0x31f0):
// {selection + 1, respawn time f32, health multiplier f32, weapon mode, toggles (f1: bit 1 buddy spawn, bit 0 sprint;
// f2: bit 7 kickbacks, 6 grenade rethrow, 5 boosters), game type, weapon mode count, counters (calls low 16, sets high)}.
// Defaults as 1.19's net game select: the net game's respawn time (+0x20), health 1.0, weapon mode 0, all toggles on;
// re-applied whenever the custom game selection (0x17d167c) changes. Rows are empty outside custom-game mode or for
// a party guest; titles hide for the game types that do not use them. U3MP_ADV=0 (fsw[0] bit 0) answers none of them.
namespace {
struct AdvRules {
    uint32_t sel1, respawn, health, weapon;
    uint8_t f1, f2, pad[2];
    uint32_t gt, count, stats;
};
static_assert(sizeof(AdvRules) == 32);
U3X_RO const char adv_value_tag[] = "[A]", adv_empty[] = "";
U3X_FN AdvRules &adv_rules() { return *reinterpret_cast<AdvRules *>(u3h_ext() + 0x31f0); }
U3X_FN bool adv_off() { return reinterpret_cast<const uint32_t *>(u3h_ext() + 0x3a70)[0] & 1; }
U3X_FN void adv_count_set(AdvRules &a) { a.stats += 0x10000; }
}  // namespace
extern "C" U3X_FN void u3x_adv_defaults(uintptr_t base, void *s, int32_t gt) {
    AdvRules &a = adv_rules();
    const int32_t sel = at<int16_t>(base + 0x17d167c);
    if (a.sel1 == static_cast<uint32_t>(sel + 1)) return;
    const uint8_t *ng = dc_table(s, 0x3b9a067du);
    uint32_t respawn = f_from_int(10);
    if (ng && sel >= 0 && static_cast<uint32_t>(sel) < rd<uint32_t>(ng, 0)) respawn = rd<uint32_t>(rd<const uint8_t *>(ng, 8), static_cast<uintptr_t>(sel) * 0xd0 + 0x20);
    a.respawn = respawn;
    a.health = F_ONE;
    a.weapon = 0;
    a.f1 = 3;
    a.f2 = 0xe0;
    a.gt = static_cast<uint32_t>(gt);
    a.sel1 = static_cast<uint32_t>(sel + 1);
}
// t with "[A]" replaced by v
extern "C" U3X_FN void u3x_adv_subst(char *o, char *end, const char *t, int64_t v) {
    while (*t && o < end) {
        if (t[0] == adv_value_tag[0] && t[1] == adv_value_tag[1] && t[2] == adv_value_tag[2]) {
            o = append_number(o, end, v, false);
            t += 3;
        } else {
            *o++ = *t++;
        }
    }
    *o = 0;
}
// every frame: the co-op stage log, then the defaults on a selection change in custom-game mode
extern "C" U3X_FN void u3x_adv_tick(uintptr_t base, void *s) {
    u3h_cs_tick(base, s);
    if (adv_off()) return;
    if (at<uint32_t>(base + 0x17d603c) != 2 || !dc_table(s, 0x3b9a067du)) return;
    u3x_adv_defaults(base, s, static_cast<int32_t>(adv_rules().gt));
}
extern "C" U3X_FN int u3x_adv_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source) return 0;
    const uint32_t tag = rd<uint32_t>(source, 0x1c);
    if ((tag >> 8) != 0x4c414fu) return 0;
    const uint32_t en = tag & 255;
    if (en != 0x38 && (en < 0xe7 || en > 0xef || en == 0xeb)) return 0;
    if (adv_off()) return 0;
    AdvRules &a = adv_rules();
    a.stats = (a.stats & 0xffff0000u) | ((a.stats + 1) & 0xffffu);
    const int op = ev[0];
    if (u3x_party_guest(reinterpret_cast<const uint8_t *>(base + 0x17dc298)) && tag == 0x4c414f38u) {   // a guest gets no button
        if (op == 1) set_row_text(ev, adv_empty);
        else if (op == 8) ev[1] = 0;
        return 1;
    }
    const bool custom = at<uint32_t>(base + 0x17d603c) == 2;
    int32_t gt = -1;
    if (custom) u3h_custom_map(base, s, &gt);
    if (custom) u3x_adv_defaults(base, s, gt);
    const char *title = u3h_text(base, s, rd<uint32_t>(source, 8));
    if (en == 0x38) {   // the button: not in co-op (types 4, 8, 9, 11, 13, 16, 17, 18)
        if (!custom) {
            set_row_text(ev, adv_empty);
            ev[1] = 0;
            return 1;
        }
        const uint32_t ct = static_cast<uint32_t>(gt) - 4u;
        const bool coop = ct < 15 && ((0x72b1u >> ct) & 1);
        if (op == 1) set_row_text(ev, coop ? adv_empty : title);
        else if (op == 8) ev[1] = 1;
        return 1;
    }
    if ((en == 0xe7 || en == 0xe9) && !custom) {
        set_row_text(ev, adv_empty);
        ev[1] = 0;
        return 1;
    }
    int bit = -1;
    uint8_t *fb = nullptr;
    switch (en) {
    case 0xe8: fb = &a.f1; bit = 1; break;
    case 0xea: fb = &a.f1; bit = 0; break;
    case 0xed: fb = &a.f2; bit = 7; break;
    case 0xee: fb = &a.f2; bit = 6; break;
    case 0xef: fb = &a.f2; bit = 5; break;
    }
    if (op == 1) {   // respawn hides for type 10, buddy for 10 / 14, kickbacks and boosters for 19
        const bool hide = (en == 0xe7 && gt == 10) || (en == 0xe8 && (gt == 10 || gt == 14)) || ((en == 0xed || en == 0xef) && gt == 19);
        set_row_text(ev, hide ? adv_empty : title);
        return 1;
    }
    if (op == 8) {   // editable: the LAN host leads its party
        ev[1] = 1;
        return 1;
    }
    char *buf = static_cast<char *>(s) + 0x2c80, *bend = buf + 120;
    if (fb) {   // on / off rows
        if (op == 5) {
            ev[1] = (*fb >> bit) & 1;
        } else if (op == 6) {
            ev[1] = 1;
        } else if (op == 10) {
            if (ev[1] == 1) *fb |= static_cast<uint8_t>(1u << bit);
            else *fb &= static_cast<uint8_t>(~(1u << bit));
            adv_count_set(a);
        } else if (op == 2 && ev[0x15] == 0) {
            set_row_text(ev, u3h_text(base, s, ev[1] == 1 ? 0xd1831446u : 0x32066ec9u));
        }
        return 1;
    }
    if (en == 0xe7) {   // respawn: index * 2 + 3 seconds, 29 entries
        if (op == 5) {
            ev[1] = (f_to_int(a.respawn) - 3) / 2;
        } else if (op == 6) {
            ev[1] = 28;
        } else if (op == 10) {
            a.respawn = f_from_int(ev[1] * 2 + 3);
            adv_count_set(a);
        } else if (op == 2 && ev[0x15] == 0) {
            u3x_adv_subst(buf, bend, u3h_text(base, s, 0x61275961u), ev[1] * 2 + 3);
            set_row_text(ev, buf);
        }
        return 1;
    }
    if (en == 0xe9) {   // health 20..400 %, stored as (v + 0.3) * 0.01
        if (op == 5) {
            ev[1] = (f_to_int(f_mul(a.health, 0x42c80000u)) - 10) / 20;
        } else if (op == 6) {
            ev[1] = 19;
        } else if (op == 10) {
            a.health = f_mul(f_add(f_from_int(ev[1] * 20 + 20), 0x3e99999au), 0x3c23d70au);
            adv_count_set(a);
        } else if (op == 2 && ev[0x15] == 0) {
            char *o = append_number(buf, bend, ev[1] * 20 + 20, false);
            *o = 0;
            set_row_text(ev, buf);
        }
        return 1;
    }
    if (en == 0xec) {   // weapon mode: DC 0x2982116f {count, {value, text id}}
        const uint8_t *t = dc_table(s, 0x2982116fu);
        if (!t) return 1;
        const uint32_t n = rd<uint32_t>(t, 0);
        const uint32_t *e = rd<const uint32_t *>(t, 8);
        a.count = n;
        if (!e) return 1;
        if (op == 5) {
            ev[1] = 0;
            for (uint32_t i = 0; i < n; ++i)
                if (e[i * 2] == a.weapon) ev[1] = static_cast<int32_t>(i);
        } else if (op == 6) {
            ev[1] = static_cast<int32_t>(n) - 1;
        } else if (op == 10) {
            if (static_cast<uint32_t>(ev[1]) < n) {
                a.weapon = e[ev[1] * 2];
                adv_count_set(a);
            }
        } else if (op == 2 && ev[0x15] == 0) {
            if (static_cast<uint32_t>(ev[1]) < n) set_row_text(ev, u3h_text(base, s, e[ev[1] * 2 + 1]));
        }
        return 1;
    }
    return 1;
}

// ===== v7.199: fourth split, part 3 (the hat stat lines) =====

// x097 / x102: 1.19 f_217000's stat lines on PAGE_HATS (0x211019f2): up to two "+n% <stat>" lines (stat table DC
// 0x53ed596a {stat, ..., name +0x24}; per-hat pairs from DC 0xb8a3581b) joined under the slot 4 text with the game's
// line break, then their stat icons drawn left of the lines (0x640ac0 measure / build, 0x63d6f0 draw), placed by
// measuring the text up to each line.
namespace {
U3X_RO const char hs_plus_space[] = "+ ", hs_plus[] = "+", hs_minus[] = "-", hs_space[] = " ", hs_percent[] = "% ",
                  hs_break[] = "\\", hs_indent[] = "      ", hs_one[] = "A", hs_indented_one[] = "      A";
U3X_BSS char hs_text_buf[384];
U3X_BSS uint32_t hs_line_start[2];
U3X_BSS uint32_t hs_icon[2][2];
U3X_BSS int32_t hs_lines;
U3X_BSS int32_t hs_has_top;
constexpr uint32_t hs_line_height = 0x41c00000u, hs_icon_size = 0x41c00000u;   // 24.0 each
}  // namespace
// PPC fctiwz: NaN -> 0x80000000, out of range saturates, else truncation
extern "C" U3X_FN uint32_t u3x_ppc_ctiwz(uint32_t f) {
    const uint32_t e = (f >> 23) & 0xff;
    if (e == 0xff && (f & 0x7fffff)) return 0x80000000u;
    if (e >= 0x9e) return (f >> 31) ? 0x80000000u : 0x7fffffffu;
    return static_cast<uint32_t>(f_to_int(f));
}
// one stat line "+n% name" (m = the multiplier), 0 for an unknown stat
extern "C" U3X_FN int u3x_hs_text(uintptr_t base, void *s, uint32_t stat, uint32_t m, char *b, uint32_t cap) {
    const uint8_t *t = dc_table(s, 0x53ed596au);
    if (!t) return 0;
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *e = rd<const uint8_t *>(t, 8), *hit = nullptr;
    if (!e || n <= 0 || n > 512) return 0;
    for (int32_t i = 0; i < n; ++i)
        if (rd<uint32_t>(e, static_cast<uintptr_t>(i) * 0x28) == stat) hit = e + static_cast<uintptr_t>(i) * 0x28;
    if (!hit) return 0;
    const char *name = u3h_text(base, s, rd<uint32_t>(hit, 0x24));
    if (!name) return 0;
    char *o = b, *end = b + cap - 1;
    if (stat == 0x974e6226u || stat == 0xd0ab3f8bu) {   // flag stats: "+ name"
        o = append_chars(o, end, hs_plus_space);
    } else if (stat == 0x2df81dfeu || stat == 0x93a5a7c2u || stat == 0xbe693215u) {   // counts: "+n name"
        o = append_chars(o, end, hs_plus);
        o = append_number(o, end, static_cast<int32_t>(u3x_ppc_ctiwz(m)), false);
        o = append_chars(o, end, hs_space);
    } else {   // percentages; three stats are stored inverted
        if (stat == 0x7da763c9u || stat == 0x2d57beb9u || stat == 0x8b73121au) m = f_div(F_ONE, m);
        const bool lower = f_less(m, F_ONE);
        o = append_chars(o, end, lower ? hs_minus : hs_plus);
        o = append_number(o, end, static_cast<int32_t>(u3x_ppc_ctiwz(f_mul(lower ? f_sub(F_ONE, m) : f_sub(m, F_ONE), 0x42c80000u))), false);
        o = append_chars(o, end, hs_percent);
    }
    o = append_chars(o, end, name);
    *o = 0;
    return 1;
}
// kind 249 (0x61a402, r15 = regs[13] = the slot 4 text): the stat lines of the hat in row regs[3] joined to it
extern "C" U3X_FN void u3x_hs_save(uintptr_t base, void *s, uint64_t *regs) {
    if (u3h_active_page(base) != 0x211019f2u) return;
    const int32_t row = static_cast<int32_t>(regs[3]);
    const uint32_t cnt = at<uint32_t>(base + 0x1b6a7c0);
    if (row < 0 || static_cast<uint32_t>(row) >= cnt || cnt > 96) return;
    const uint32_t idx = at<uint32_t>(base + 0x2208000 + static_cast<uintptr_t>(row) * 4);
    const uint8_t *lt = at<const uint8_t *>(base + 0x1b6a7c8);
    if (!lt || idx >= rd<uint32_t>(lt, 0)) return;
    const uint8_t *lr = rd<const uint8_t *>(lt, 8);
    if (!lr) return;
    const uint32_t id = rd<uint32_t>(lr, static_cast<uintptr_t>(idx) * 0x38);
    const uint8_t *t = dc_table(s, 0xb8a3581bu);
    if (!t) return;
    const int32_t n = rd<int32_t>(t, 0);
    const uint8_t *rows = rd<const uint8_t *>(t, 8);
    if (!rows || n <= 0 || n > 4096) return;
    const char *src = reinterpret_cast<const char *>(regs[13]);
    char *o = hs_text_buf, *end = hs_text_buf + sizeof(hs_text_buf) - 1;
    int line = 0;
    if (src && *src) o = append_chars(o, end, src);
    for (int32_t r = 0; r < n; ++r) {
        const uint8_t *w = rows + static_cast<uintptr_t>(r) * 16;
        if (rd<uint32_t>(w, 0) != id) continue;
        const int32_t c = rd<int32_t>(w, 4);
        const uint8_t *pp = rd<const uint8_t *>(w, 8);
        if (!pp) continue;
        for (int32_t k = 0; k < c && line < 2; ++k) {
            char b[96];
            const uint32_t stat = rd<uint32_t>(pp, static_cast<uintptr_t>(k) * 8), m = rd<uint32_t>(pp, static_cast<uintptr_t>(k) * 8 + 4);
            if (!u3x_hs_text(base, s, stat, m, b, 96)) continue;
            if (o != hs_text_buf) o = append_chars(o, end, hs_break);   // the game text's line break
            hs_line_start[line] = static_cast<uint32_t>(o - hs_text_buf);
            o = append_chars(o, end, hs_indent);
            o = append_chars(o, end, b);
            hs_icon[line][0] = stat;
            hs_icon[line][1] = m;
            ++line;
        }
    }
    *o = 0;
    if (line) {
        regs[13] = reinterpret_cast<uint64_t>(hs_text_buf);
        hs_lines = line;
        hs_has_top = (src && *src) ? 1 : 0;
    }
}
// the size of text t at scale sc in a w x h box (0x640ac0), flag 1 = the full height
extern "C" U3X_FN void u3x_hs_meas(uintptr_t base, uint32_t sc, const char *t, uint32_t w, uint32_t h, uint32_t fl, uint32_t *ow, uint32_t *oh) {
    alignas(16) uint32_t item[12];
    alignas(16) uint32_t sz[2];
    alignas(16) uint32_t z[4];
    alignas(16) uint8_t layout[0x80];
    for (uint32_t &v : item) v = 0;
    sz[0] = w;
    sz[1] = h;
    for (uint32_t &v : z) v = 0;
    item[0] = 1;
    item[4] = fl;
    item[7] = 0xffffffffu;
    item[9] = 0xffffffffu;
    item[10] = F_ONE;
    u3x_call8(base + 0x640ac0, sc, reinterpret_cast<uint64_t>(layout), reinterpret_cast<uint64_t>(item), reinterpret_cast<uint64_t>(t),
              reinterpret_cast<uint64_t>(sz), reinterpret_cast<uint64_t>(z), 0xffffffffu, 0, 0);
    *ow = rd<uint32_t>(layout, 0x3c);
    *oh = rd<uint32_t>(layout, 0x40);
}
// kind 250 (0x61a638): the stat icons left of the lines (rbp = regs[14], r13 = regs[2])
extern "C" U3X_FN void u3x_hs_draw(uintptr_t base, void *s, uint64_t *regs) {
    const int n = hs_lines;
    hs_lines = 0;
    if (n <= 0) return;
    uint8_t *rbp = reinterpret_cast<uint8_t *>(regs[14]), *r13 = reinterpret_cast<uint8_t *>(regs[2]);
    const uint32_t *rt = reinterpret_cast<const uint32_t *>(rbp - 0x180), *ts = reinterpret_cast<const uint32_t *>(rbp - 0xa8);
    alignas(16) uint32_t st[10];
    for (int k = 0; k < 10; ++k) st[k] = reinterpret_cast<const uint32_t *>(rbp - 0x58)[k];
    st[1] = st[2] = st[3] = F_ONE;
    st[5] = 0;
    const uint32_t sc = f_mul(f_add(f_mul(f_mul(f_mul(ts[6], ts[7]), ts[4]), 0x3d4ccccdu), F_ONE), ts[0]);
    uint32_t w0, lh, w1, d;
    u3x_hs_meas(base, sc, hs_one, rt[2], rt[3], 1, &d, &lh);
    u3x_hs_meas(base, sc, hs_one, rt[2], rt[3], 0, &w0, &d);
    u3x_hs_meas(base, sc, hs_indented_one, rt[2], rt[3], 0, &w1, &d);
    uint32_t indent = f_sub(w1, w0);
    if (f_less(lh, 0x41000000u) || f_less(0x42800000u, lh)) lh = hs_line_height;
    if (f_less(indent, 0x40800000u) || f_less(0x42800000u, indent)) indent = hs_icon_size;
    const uint32_t ic = f_less(f_sub(indent, 0x40000000u), lh) ? f_sub(indent, 0x40000000u) : lh;
    const uint32_t x = f_add(rt[0], f_mul(f_sub(indent, ic), 0x3f000000u));
    for (int i = 0; i < n; ++i) {
        uint8_t *v = u3x_stat_icon(base, s, hs_icon[i][0], hs_icon[i][1]);
        if (!v) continue;
        char pre[390];
        const uint32_t c = hs_line_start[i];
        if (c > sizeof(pre) - 2) continue;
        for (uint32_t k = 0; k < c; ++k) pre[k] = hs_text_buf[k];
        pre[c] = 'A';
        pre[c + 1] = 0;
        uint32_t hp;
        u3x_hs_meas(base, sc, pre, rt[2], rt[3], 1, &d, &hp);
        const uint32_t y = f_add(f_add(rt[1], f_sub(hp, lh)), f_mul(f_sub(lh, ic), 0x3f000000u));
        alignas(16) uint32_t item[12];
        for (uint32_t &w : item) w = 0;
        item[0] = 2;
        *reinterpret_cast<uint8_t **>(item + 2) = v;
        item[7] = 0xffffffffu;
        item[9] = 0xffffffffu;
        item[10] = F_ONE;
        alignas(16) uint32_t rc[4];
        alignas(16) uint32_t sz[2];
        alignas(16) uint32_t z[4];
        rc[0] = x;
        rc[1] = y;
        rc[2] = rc[3] = ic;
        sz[0] = sz[1] = ic;
        for (uint32_t &w : z) w = 0;
        u3x_call8(base + 0x640ac0, st[0], reinterpret_cast<uint64_t>(rbp - 0x118), reinterpret_cast<uint64_t>(item), 0,
                  reinterpret_cast<uint64_t>(sz), reinterpret_cast<uint64_t>(z), 0xffffffffu, 0, 0);
        u3x_call8(base + 0x63d6f0, 0, rd<uint64_t>(r13, 0), rd<uint64_t>(rbp - 0x198, 0), reinterpret_cast<uint64_t>(st),
                  reinterpret_cast<uint64_t>(rbp - 0x118), reinterpret_cast<uint64_t>(rc), 0, 0, 0xffffffffu);
    }
}

// ===== v7.200: fourth split, part 4 (the film camera's HUD helpers) =====

// b003: the integer HUD pieces of the film's followed-player camera (1.19 f_99479c / f_65e280 / f_65e0c4 as PS4 inlines
// them). The rest of that camera (death look, scope view, follow / fixed camera) does native float maths in the C part
// and stays there. HUD of screen 0 at 0x1805fa0.
// flag f: HUD flags +8 |= f; each element of the list (+0x40, count +0x38) with +8 == f is shown (+0xc = 1) and, unless it
// already was, stamped with the game time (game +0x94)
extern "C" U3X_FN void u3x_rp_hud_flag(uintptr_t base, uint32_t f) {
    uint8_t *h = reinterpret_cast<uint8_t *>(base + 0x1805fa0);
    *reinterpret_cast<uint32_t *>(h + 8) |= f;
    uint32_t n = rd<uint32_t>(h, 0x38);
    uint8_t *const *e = reinterpret_cast<uint8_t *const *>(h + 0x40);
    for (; n; --n, ++e) {
        uint8_t *p = *e;
        if (!p || rd<uint32_t>(p, 8) != f) continue;
        *reinterpret_cast<uint32_t *>(p + 0xc) = 1;
        if (rd<uint32_t>(p, 0x10) != 1) *reinterpret_cast<int32_t *>(p + 0x18) = rd<int32_t>(at<const uint8_t *>(base + 0x1b24b98), 0x94);
    }
}
// the HUD fade object (+0x220): {+4 i, +8 byte, +0xc a, +0x10 b, +0x14 c}
extern "C" U3X_FN void u3x_rp_hud_fade(uintptr_t base, uint32_t a, uint32_t b, uint32_t c, uint32_t i, uint8_t by) {
    uint8_t *f = at<uint8_t *>(base + 0x1805fa0 + 0x220);
    *reinterpret_cast<uint32_t *>(f + 0x14) = c;
    *reinterpret_cast<uint32_t *>(f + 4) = i;
    f[8] = by;
    *reinterpret_cast<uint32_t *>(f + 0xc) = a;
    *reinterpret_cast<uint32_t *>(f + 0x10) = b;
}
// the HUD shows weapon w (its id +0x6ec) through 0xc4a480
extern "C" U3X_FN void u3x_rp_hud_weapon(uintptr_t base, const uint8_t *w) { game_call<void>(base + 0xc4a480, base + 0x1805fa0, rd<uint32_t>(w, 0x6ec)); }

// ===== v7.201: fourth split, part 5 (the film player's accessors and script overrides) =====

extern "C" {
}

// b003: the small pieces the film player uses, as PS4 inlines them. Handles {ptr, id} (object id +0x7c); net player
// trackers 0x17cb3f0 + i * 0x9c0 while multiplayer is active; the is-a registry 0x1b3efd8 (257 buckets, entry hash +0x10,
// bit +0x14; type bits at the object's type info +0x20); pad 0's pressed buttons; script variable overrides keyed by an
// owner (0x12284e0 adds one, 0x1228b80 releases it). Calls with float arguments go through the C part's u3h_rp_fcall:
// r[0] fn, r[1..6] rdi..r9, r[7..10] xmm0..3, r[11..12] stack, returns rax.
extern "C" U3X_FN void u3x_rp_assert(uintptr_t base, uintptr_t a, uintptr_t b, uintptr_t c, uint32_t line) {
    const uintptr_t h = at<uintptr_t>(base + 0x1bf27b8);
    if (h) reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t, int)>(h)(base + a, base + b, base + c, base + 0x13ac250, line, 0);
    __asm__ volatile("int $0x41");
}
extern "C" U3X_FN uint8_t *u3x_rp_h(const uint8_t *h) {
    uint8_t *const *pp = rd<uint8_t *const *>(h, 0);
    const uint32_t id = rd<uint32_t>(h, 8);
    return (pp && id && *pp && rd<uint32_t>(*pp, 0x7c) == id) ? *pp : nullptr;
}
// the screen 0 camera manager (PS4 asserts "pMgr != NULL" without one)
extern "C" U3X_FN uint8_t *u3x_rp_cammgr(uintptr_t base) {
    uint8_t *m = u3x_rp_h(reinterpret_cast<const uint8_t *>(base + 0x1d52b80));
    if (!m) u3x_rp_assert(base, 0x149e6ab, 0x149e5b7, 0x14aae33, 0x143);
    return m;
}
extern "C" U3X_FN uint8_t *u3x_rp_trk(uintptr_t base, uint32_t i) {
    return at<uint8_t>(base + 0x17d5c61) ? reinterpret_cast<uint8_t *>(base + 0x17cb3f0 + static_cast<uintptr_t>(i) * 0x9c0) : nullptr;
}
extern "C" U3X_FN int u3x_rp_trk_live(uint8_t *t) { return u3x_rp_h(t + 0x10) || u3x_rp_h(t + 0x20); }
extern "C" U3X_FN int u3x_rp_dead(const uint8_t *t) { return rd<int32_t>(t, 0x3a0) < rd<int32_t>(t, 0x39c); }
// the tracker's network id: low 12 bits (+0x800 when +0x1ec is set and they are non-zero) + the high part << 12
extern "C" U3X_FN uint32_t u3x_rp_trk_id(const uint8_t *tr) {
    const uint32_t lo = rd<uint32_t>(tr, 0x1e4) & 0xfffu;
    return ((tr[0x1ec] && lo) ? lo + 0x800 : lo) + (rd<uint32_t>(tr, 0x1e8) << 12);
}
// the tracker with that id among the 10 (no net game check, as PS3)
extern "C" U3X_FN uint8_t *u3x_rp_trk_by_id(uintptr_t base, uint32_t id) {
    if (!id) return nullptr;
    uint8_t *t = at<uint8_t *>(base + 0x16e2f50) + 0x390;
    for (int i = 0; i < 10; ++i, t += 0x9c0)
        if (u3x_rp_trk_id(t) == id) return t;
    return nullptr;
}
extern "C" U3X_FN int u3x_rp_isa(uintptr_t base, uint8_t *obj, uint32_t hash) {
    uint8_t *reg = reinterpret_cast<uint8_t *>(base + 0x1b3efd8);
    if (!reg[0]) return 0;
    uint8_t *head = reg + 8 + static_cast<uintptr_t>(hash % 257u) * 16;
    for (uint8_t *e = rd<uint8_t *>(head, 0); e != head; e = rd<uint8_t *>(e, 0)) {
        if (rd<uint32_t>(e, 0x10) != hash) continue;
        const uint32_t b = rd<uint32_t>(e, 0x14);
        const uint8_t *ti = rd<const uint8_t *>(obj, 0x38);
        return static_cast<int>((rd<uint64_t>(ti, static_cast<uintptr_t>(b >> 6) * 8 + 0x20) >> (b & 63)) & 1);
    }
    return 0;
}
extern "C" U3X_FN uint32_t *u3x_rp_pad(uintptr_t base) { return reinterpret_cast<uint32_t *>(at<uint8_t *>(base + 0x16e2fa0) + 0x7e0 + 0x94); }
extern "C" U3X_FN int u3x_rp_enter_cross(uintptr_t base) { return rd<uint8_t>(at<const uint8_t *>(base + 0x1b24b80), 0x39b4) != 0; }
extern "C" U3X_FN int u3x_rp_menu_up(uintptr_t base) {
    const uint8_t *m = reinterpret_cast<const uint8_t *>(base + 0x17bd080);
    return rd<uint32_t>(m, 8) && rd<uint32_t>(m, 0xc) == 1;
}
// send message msg to target without arguments (0x1197380)
extern "C" U3X_FN void u3x_rp_send(uintptr_t base, uint32_t msg, uint8_t *target) {
    uint64_t a[4][3], ret[4], r[14];
    for (auto &row : a)
        for (uint64_t &w : row) w = 0;
    for (uint64_t &w : ret) w = 0;
    for (uint64_t &w : r) w = 0;
    r[0] = base + 0x1197380;
    r[1] = reinterpret_cast<uint64_t>(ret);
    r[3] = msg;
    r[4] = reinterpret_cast<uint64_t>(target);
    r[5] = reinterpret_cast<uint64_t>(a[0]);
    r[6] = reinterpret_cast<uint64_t>(a[1]);
    r[11] = reinterpret_cast<uint64_t>(a[2]);
    r[12] = reinterpret_cast<uint64_t>(a[3]);
    u3h_rp_fcall(r);
}
extern "C" U3X_FN uint8_t *u3x_rp_self(uint8_t *own) { return u3x_rp_h(own + 0x70); }
// an override of addr for the owner, blended over t, held for frames + 1 frames (+0x49 bit 1)
extern "C" U3X_FN void u3x_ovr_blend(uintptr_t base, uint32_t t, uint8_t *addr, const void *val, int32_t frames, uint8_t *owner) {
    if (!owner) return;
    uint64_t r[14];
    for (uint64_t &w : r) w = 0;
    r[0] = base + 0x12284e0;
    r[7] = t;
    r[2] = reinterpret_cast<uint64_t>(addr);
    r[3] = reinterpret_cast<uint64_t>(val);
    r[4] = 4;
    r[5] = base + 0x1228b20;
    r[6] = base + 0x1228b30;
    r[11] = reinterpret_cast<uint64_t>(owner);
    uint8_t *o = reinterpret_cast<uint8_t *>(u3h_rp_fcall(r));
    if (o) {
        *reinterpret_cast<int32_t *>(o + 0x28) = frames + 1;
        o[0x49] |= 2;
    }
}
extern "C" U3X_FN void u3x_ovr_release(uintptr_t base, uint8_t *addr, uint8_t *owner) { game_call<void>(base + 0x1228b80, 0u, addr, owner); }
// the scale override (hash 0x309861e2, weight 1.0, +0x49 bit 3)
extern "C" U3X_FN void u3x_ovr_scale(uintptr_t base, uint8_t *addr, const void *val) {
    uint64_t r[14];
    for (uint64_t &w : r) w = 0;
    r[0] = base + 0x12284e0;
    r[7] = F_ONE;
    r[1] = 0x309861e2u;
    r[2] = reinterpret_cast<uint64_t>(addr);
    r[3] = reinterpret_cast<uint64_t>(val);
    r[4] = 4;
    r[5] = base + 0x1228b20;
    r[6] = base + 0x1228ca0;
    uint8_t *o = reinterpret_cast<uint8_t *>(u3h_rp_fcall(r));
    if (o) {
        *reinterpret_cast<int32_t *>(o + 0x28) = 0;
        o[0x49] |= 8;
    }
}
// a one-byte override for frames + 1 frames, timed game +0x94 + 9 + time
extern "C" U3X_FN void u3x_ovr_byte(uintptr_t base, uint32_t hash, uint8_t *addr, const uint8_t *val, int32_t frames, int32_t time) {
    uint64_t r[14];
    for (uint64_t &w : r) w = 0;
    r[0] = base + 0x12284e0;
    r[7] = F_ONE;
    r[1] = hash;
    r[2] = reinterpret_cast<uint64_t>(addr);
    r[3] = reinterpret_cast<uint64_t>(val);
    r[4] = 1;
    r[5] = base + 0x12284c0;
    r[6] = base + 0x12284d0;
    uint8_t *o = reinterpret_cast<uint8_t *>(u3h_rp_fcall(r));
    if (o) {
        o[0x49] = static_cast<uint8_t>((o[0x49] & 0xdb) | 4);
        *reinterpret_cast<int32_t *>(o + 0x28) = frames + 1;
        *reinterpret_cast<int32_t *>(o + 0x10) = rd<int32_t>(at<const uint8_t *>(base + 0x1b24b98), 0x94) + 9 + time;
    }
}
extern "C" U3X_FN uint8_t *u3x_rp_rs(uintptr_t base) { return at<uint8_t *>(base + 0x16e2fb8); }

// ===== v7.202: fourth split, part 6 (the treasure lists) =====

// LIST_TREASURES (0xd4, PS3 f_340bd0) on PAGE_NET_TREASURE_SETS (0xda577fac) and the treasure list page 0xecfa738a, and
// the treasure items list (0xd5, PS4's own 0x759ba0 fed with our filtered list). Sets: DC 0xf1f296cb {count, 0xc: id,
// name +4, group +8}; treasures: DC 0xb114fe00 (0x68 each: name +0, description +0x18, set +0x2c, map +0x34, game type
// +0x38, difficulty +0x3c); owned = profile byte +0xacc + index. The list state (state +0x3a00): set indices, treasure
// indices, row, chosen set. x066: the "found in" line; x069: the treasure rows sit at the bottom, with room for 4.
namespace {
struct TreasureState {
    uint8_t sets[256];
    uint32_t nsets;
    uint16_t items[256];
    uint32_t nitems;
    int32_t row;
    uint32_t set_id;
};
static_assert(sizeof(TreasureState) == 784);
U3X_RO const char tr_slash[] = "/", tr_empty[] = "", tr_text1[] = "[TEXT", tr_none[] = "";
U3X_FN TreasureState &treasures(void *s) { return *reinterpret_cast<TreasureState *>(static_cast<uint8_t *>(s) + 0x3a00); }
U3X_FN bool owned(const uint8_t *pr, uint32_t i) { return pr && i < 0x400 && pr[0xacc + i]; }
U3X_FN const char *set_name_of(uintptr_t base, void *s, uint32_t set_id) {   // the set's name, 0 when not found
    const char *name = nullptr;
    const uint8_t *t = dc_table(s, 0xf1f296cbu);
    if (!t) return nullptr;
    const uint32_t n = rd<uint32_t>(t, 0);
    const uint8_t *e = rd<const uint8_t *>(t, 8);
    for (uint32_t i = 0; e && i < n; ++i)
        if (rd<uint32_t>(e, i * 0xc) == set_id) name = u3h_text(base, s, rd<uint32_t>(e, i * 0xc + 4));
    return name;
}
}  // namespace
// a DC symbol's data (the state's lookup)
extern "C" U3X_FN uint8_t *u3x_sym_table(void *s, uint32_t id) { return dc_table(s, id); }
// the sets of group g (PS3 f_32c734)
extern "C" U3X_FN void u3x_treasure_build_sets(void *s, int32_t group) {
    TreasureState &ts = treasures(s);
    const uint8_t *t = dc_table(s, 0xf1f296cbu);
    ts.nsets = 0;
    if (!t) return;
    const uint32_t n = rd<uint32_t>(t, 0);
    const uint8_t *e = rd<const uint8_t *>(t, 8);
    for (uint32_t i = 0; e && i < n && ts.nsets < 256; ++i)
        if (rd<int32_t>(e, i * 0xc + 8) == group) ts.sets[ts.nsets++] = static_cast<uint8_t>(i);
}
// the treasures of set id (PS3 f_32c624)
extern "C" U3X_FN void u3x_treasure_build_items(void *s, uint32_t set) {
    TreasureState &ts = treasures(s);
    const uint8_t *t = dc_table(s, 0xb114fe00u);
    ts.nitems = 0;
    if (!t) return;
    const uint32_t n = rd<uint32_t>(t, 0);
    const uint8_t *e = rd<const uint8_t *>(t, 8);
    for (uint32_t i = 0; e && i < n && ts.nitems < 256; ++i)
        if (rd<uint32_t>(e, static_cast<uintptr_t>(i) * 0x68 + 0x2c) == set) ts.items[ts.nitems++] = static_cast<uint16_t>(i);
}
extern "C" U3X_FN uint8_t *u3x_treasure_set_entry(void *s, int32_t row) {
    const TreasureState &ts = treasures(s);
    uint8_t *t = dc_table(s, 0xf1f296cbu);
    if (!t || row < 0 || static_cast<uint32_t>(row) >= ts.nsets || ts.sets[row] >= rd<uint32_t>(t, 0)) return nullptr;
    return rd<uint8_t *>(t, 8) + static_cast<uintptr_t>(ts.sets[row]) * 0xc;
}
extern "C" U3X_FN uint8_t *u3x_treasure_entry(void *s, uint32_t idx) {
    uint8_t *t = dc_table(s, 0xb114fe00u);
    if (!t || idx >= rd<uint32_t>(t, 0)) return nullptr;
    return rd<uint8_t *>(t, 8) + static_cast<uintptr_t>(idx) * 0x68;
}
// the current tab of PAGE_NET_TREASURE_SETS (tab table DC 0xf1514340, LAN or online entry)
extern "C" U3X_FN uint8_t *u3x_treasure_tab(uintptr_t base, void *s) {
    const uint8_t *t = dc_table(s, 0xf1514340u);
    if (!t) return nullptr;
    const uint32_t n = rd<uint32_t>(t, 0);
    const uint8_t *e = rd<const uint8_t *>(t, 8);
    for (uint32_t i = 0; e && i < n; ++i, e += 0x20)
        if (rd<uint32_t>(e, 0) == 0xda577facu && rd<uint32_t>(e, 4) == at<uint8_t>(base + 0x17d5c60)) {
            uint8_t *tabs = rd<uint8_t *>(e, 0x10);
            const int32_t cur = rd<int32_t>(e, 0x18);
            return (tabs && cur >= 0) ? tabs + static_cast<uintptr_t>(cur) * 0xc : nullptr;
        }
    return nullptr;
}
// t with [TEXT], [TEXT2], [TEXT3] replaced by a, b, c
extern "C" U3X_FN void u3x_x66_subst(char *o, char *end, const char *t, const char *a, const char *b, const char *c) {
    while (*t && o < end) {
        bool tag = true;
        for (int k = 0; k < 5; ++k)
            if (t[k] != tr_text1[k]) tag = false;
        if (tag) {
            const char *v = t[5] == ']' ? a : t[5] == '2' ? b : c;
            o = append_chars(o, end, v);
            t += t[5] == ']' ? 6 : 7;
        } else {
            *o++ = *t++;
        }
    }
    *o = 0;
}
// x066: where the first treasure of the set is found: "<rule> on <map> (<difficulty>)", Siege, or a game type
extern "C" U3X_FN void u3x_x66_found(uintptr_t base, int32_t *ev, void *s) {
    const TreasureState &ts = treasures(s);
    if (!ts.nitems) return;
    const uint8_t *e = u3x_treasure_entry(s, ts.items[0]);
    if (!e) return;
    const uint32_t map_id = rd<uint32_t>(e, 0x34);
    const int32_t gt = rd<int32_t>(e, 0x38), diff = rd<int32_t>(e, 0x3c);
    const uint8_t *mt = dc_table(s, 0x0dec97a4u), *rt = dc_table(s, 0xf985c7b6u), *map = nullptr, *rule = nullptr;
    if (mt) {
        const uint32_t n = rd<uint32_t>(mt, 0);
        const uint8_t *m = rd<const uint8_t *>(mt, 8);
        for (uint32_t i = 0; m && i < n; ++i)
            if (rd<uint32_t>(m, static_cast<uintptr_t>(i) * 0x98) == map_id) {
                map = m + static_cast<uintptr_t>(i) * 0x98;
                break;
            }
    }
    if (rt && gt >= 0 && gt < rd<int32_t>(rt, 0)) rule = rd<const uint8_t *>(rt, 8) + static_cast<uintptr_t>(gt) * 0x58;
    char *const buf = static_cast<char *>(s) + 0x2b00, *end = buf + 230;
    if (map) {
        const uint32_t d = (diff == 1 || diff == 2) ? 0x14956c33u : (diff == 3 || diff == 4) ? 0x87d40e88u : 0;
        char ta[64], tb[64];
        char *q = append_chars(ta, ta + 63, rule ? u3h_text(base, s, rd<uint32_t>(rule, 0)) : tr_none);
        *q = 0;
        q = append_chars(tb, tb + 63, u3h_text(base, s, rd<uint32_t>(map, 4)));
        *q = 0;
        u3x_x66_subst(buf, end, u3h_text(base, s, 0xdcdd1012u), ta, tb, d ? u3h_text(base, s, d) : tr_none);
    } else if (gt == 21) {
        set_row_text(ev, u3h_text(base, s, 0x35561665u));
        return;
    } else {
        const uint32_t id = gt == 20 ? 0xd5fc28f0u : ((gt == 17 || gt == 18) && rule) ? rd<uint32_t>(rule, 0) : 0x85a5bbbeu;
        u3x_x66_subst(buf, end, u3h_text(base, s, 0x2b3ffa5bu), u3h_text(base, s, id), tr_none, tr_none);
    }
    set_row_text(ev, buf);
}
extern "C" U3X_FN int u3x_treasures_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414ed4u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 760) = base;
    TreasureState &ts = treasures(s);
    if (!dc_table(s, 0xf1f296cbu) || !dc_table(s, 0xb114fe00u)) {
        if (ev[0] == 6) ev[1] = -1;
        return 1;
    }
    const uint8_t *pr = player_profile(base);
    int32_t row = ev[1];
    uint8_t *se;
    if (ev[0x24] == 1 && !(ext_ea()[0] & 8)) {   // the treasure list page asks for its title: the chosen set's name
        if (ev[0] == 1)
            if (const char *name = set_name_of(base, s, ts.set_id)) set_row_text(ev, name);
        return 1;
    }
    switch (ev[0]) {
    case 2: {
        const uint32_t sub = static_cast<uint32_t>(ev[0x15]);
        if (sub == 0) {
            if ((se = u3x_treasure_set_entry(s, row))) set_row_text(ev, u3h_text(base, s, rd<uint32_t>(se, 4)));
        } else if (sub == 1) {   // owned / total of the set
            if (!(se = u3x_treasure_set_entry(s, row))) break;
            const uint8_t *t = dc_table(s, 0xb114fe00u);
            const uint32_t n = rd<uint32_t>(t, 0);
            uint32_t own = 0, total = 0;
            const uint8_t *e = rd<const uint8_t *>(t, 8);
            for (uint32_t i = 0; e && i < n; ++i)
                if (rd<uint32_t>(e, static_cast<uintptr_t>(i) * 0x68 + 0x2c) == rd<uint32_t>(se, 0)) {
                    ++total;
                    if (owned(pr, i)) ++own;
                }
            char *const buf = static_cast<char *>(s) + 0x2c80, *end = buf + 60;
            char *o = append_number(buf, end, own, false);
            o = append_chars(o, end, tr_slash);
            o = append_number(o, end, total, false);
            *o = 0;
            set_row_text(ev, buf);
        } else if (sub == 4) {
            u3x_x66_found(base, ev, s);
        } else if (sub == 0x843d4adu || sub == 0x0a56c0abu) {   // name / description, rows at the bottom
            if (ts.nitems < 4) row -= 4 - static_cast<int32_t>(ts.nitems);
            if (row < 0) {
                set_row_text(ev, tr_empty);
                break;
            }
            if (static_cast<uint32_t>(row) >= ts.nitems) break;
            const uint8_t *e = u3x_treasure_entry(s, ts.items[row]);
            if (e) set_row_text(ev, u3h_text(base, s, rd<uint32_t>(e, sub == 0x843d4adu ? 0 : 0x18)));
        }
        break;
    }
    case 4:
        if (static_cast<uint32_t>(ev[0x14]) == 0xf5a294f3u) {
            const uint8_t *tab = u3x_treasure_tab(base, s);
            if (tab && u3h_icon_index(base, rd<uint32_t>(tab, 4)) >= 0) u3h_set_icon(base, ev, rd<uint32_t>(tab, 4));
        } else if (static_cast<uint32_t>(ev[0x14]) == 0x21981e34u) {   // the owned check
            if (ts.nitems < 4) row -= 4 - static_cast<int32_t>(ts.nitems);
            if (row >= 0 && static_cast<uint32_t>(row) < ts.nitems && owned(pr, ts.items[row]) && u3h_icon_index(base, 0x08208d0au) >= 0)
                u3h_set_icon(base, ev, 0x08208d0au);
        }
        break;
    case 5: ev[1] = ts.row; break;
    case 6: ev[1] = static_cast<int32_t>(ts.nsets) - 1; break;
    case 9: {   // the sets of the current tab, then the treasures of the highlighted set
        const uint8_t *tab = u3x_treasure_tab(base, s);
        if (!tab) break;
        const uint32_t id = rd<uint32_t>(tab, 0);
        u3x_treasure_build_sets(s, id == 0x49d4b22du ? 2 : id == 0x5baab3cdu ? 3 : 1);
        if ((se = u3x_treasure_set_entry(s, ts.row))) u3x_treasure_build_items(s, rd<uint32_t>(se, 0));
        else ts.nitems = 0;
        break;
    }
    case 10: ts.row = row; break;
    case 11:
        if (static_cast<uint32_t>(ev[0x13]) == 0xbcf5ea24u) ev[14] = ts.nitems && ts.nitems < 4 ? 4 : static_cast<int32_t>(ts.nitems);
        break;
    case 0x14:
        if (ev[0x16] == 1) ev[0xf] = -1;
        break;
    case 0x16:
        if ((se = u3x_treasure_set_entry(s, ev[0x17]))) {
            ts.set_id = rd<uint32_t>(se, 0);
            u3x_treasure_build_items(s, ts.set_id);
            if (state_show_page(s)) u3h_show(base, s, 0xecfa738au, 0);
        }
        break;
    }
    return 1;
}
// 0xd5: PS4's own list (0x759ba0) on our filtered treasures; op 1 is the chosen set's name, op 9 refills the list
extern "C" U3X_FN int u3x_treasure_items_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414ed5u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    TreasureState &ts = treasures(s);
    uint8_t *t = dc_table(s, 0xb114fe00u);
    if (ev[0] == 1) {
        set_row_text(ev, tr_empty);
        if (ev[0x24] != 1) return 1;
        if (dc_table(s, 0xf1f296cbu))
            if (const char *name = set_name_of(base, s, ts.set_id)) set_row_text(ev, name);
        return 1;
    }
    if (ev[0] == 9) {
        if (!t) return 1;
        u3x_treasure_build_items(s, ts.set_id);
        at<uint8_t *>(base + 0x1b6aa38) = t;
        uint16_t *o = reinterpret_cast<uint16_t *>(base + 0x1b6aa40);
        for (uint32_t i = 0; i < ts.nitems; ++i) o[i] = ts.items[i];
        at<uint32_t>(base + 0x1b6ac40) = ts.nitems;
        return 1;
    }
    game_call<void>(base + 0x759ba0, ev);
    return 1;
}
// the treasure sets list starts at its first row again
extern "C" U3X_FN void u3x_treasure_first_row(void *s) { treasures(s).row = 0; }

// ===== v7.203: fourth split, part 7 (the film player's update) =====

extern "C" {
U3H_IMPORT void u3h_rp_cam_mode12(uintptr_t base, uint8_t *st, uint8_t *own);
U3H_IMPORT void u3h_rp_render_vars(uintptr_t base, uint8_t *own);
U3H_IMPORT void u3h_rp_cam_target(uintptr_t base, uint8_t *own);
}

// b003: PS3 1.19 f_279d6c (RePlaying::Update), called by the value stub in place of PS4's (which then returns). The
// film writer is CIN_W (0x17c3ba8), the re-record reader CIN_R (0x17c5cf8); the owner (state +8) holds the camera handle
// +0x1a0, the followed player +0x1b8, HUD toggles +0x1bc / +0x1bd, the screenshot state +0x1b4 and the bookmarks
// (+0x1f8, 0x30 each, count +0x6a8). Its float steps are written as in the C part (x87: this part is built without SSE too).
namespace {
constexpr uintptr_t cin_writer = 0x17c3ba8, cin_reader = 0x17c5cf8;
constexpr uint32_t pb_up = 0x10u, pb_down = 0x40u, pb_tri = 0x1000u, pb_cross = 0x4000u, pb_square = 0x8000u, pb_circle = 0x2000u, pb_r2 = 0x200u;
U3X_FN int32_t &rp_mode(uintptr_t base) { return at<int32_t>(base + 0x17c5a10); }
U3X_FN uint8_t &rp_pcur(uintptr_t base) { return at<uint8_t>(base + 0x17c3570); }
U3X_FN uint8_t &rp_preq(uintptr_t base) { return at<uint8_t>(base + 0x17c3571); }
U3X_FN uint8_t &rp_p3(uintptr_t base) { return at<uint8_t>(base + 0x17c3572); }
U3X_FN uint32_t &rp_speed(uintptr_t base) { return at<uint32_t>(base + 0x16e716c); }
U3X_FN uint8_t rp_mpeg(uintptr_t base) { return at<uint8_t>(base + 0x17c5b70); }
U3X_FN uint8_t rp_b2c(uintptr_t base) { return at<uint8_t>(base + 0x17d1d10); }
U3X_FN uint8_t rp_b2d(uintptr_t base) { return at<uint8_t>(base + 0x17d1d11); }
U3X_FN uint8_t *rp_clock(uintptr_t base) { return at<uint8_t *>(base + 0x17c3580); }
U3X_FN uint8_t *rp_bookmark(uint8_t *own, int32_t i) { return own + 0x1f8 + static_cast<uintptr_t>(i) * 0x30; }
// x87 float helpers (this part is built without SSE, like the C): constants load from an immediate so nothing lands in
// .rodata.cst4
[[gnu::always_inline]] inline float x87_const(uint32_t u) {
    float r;
    __asm__("flds %1" : "=t"(r) : "m"(u));
    return r;
}
[[gnu::always_inline]] inline float x87_float(uint32_t u) {
    float r;
    __builtin_memcpy(&r, &u, 4);
    return r;
}
[[gnu::always_inline]] inline uint32_t x87_bits(float f) {
    uint32_t u;
    __builtin_memcpy(&u, &f, 4);
    return u;
}
U3X_FN void call_slot(uint8_t *obj, uint32_t byte_offset) { reinterpret_cast<void (*)(uint8_t *)>(rd<uintptr_t *>(obj, 0)[byte_offset / 8])(obj); }
// pad k's rumble off (an override of its byte +0x1d0 for 2 frames)
U3X_FN void rp_rumble_off(uintptr_t base, int k) {
    const uint8_t zero = 0;
    u3x_ovr_byte(base, 0x35b0e776u, at<uint8_t *>(base + 0x1b24b98) + 0x7e0 + static_cast<uintptr_t>(k) * 0x1d8 + 0x1d0, &zero, 2, 0x1e);
}
// the screenshot request: PS4 has no photo utility, so every step takes PS3's error branch
U3X_FN int rp_screenshot_step(uintptr_t base, uint8_t *st, uint8_t *own) {
    const bool photo_cb = false;   // set only by the cellPhotoUtility callbacks
    own[0x1bc] = 0;
    rp_p3(base) = 1;
    rp_preq(base) = 1;
    uint32_t z = 0;
    u3x_ovr_scale(base, reinterpret_cast<uint8_t *>(base + 0x17b4b1c), &z);   // clocks 1 and 0 stop
    u3x_ovr_scale(base, reinterpret_cast<uint8_t *>(base + 0x17b4adc), &z);
    int32_t &ss = *reinterpret_cast<int32_t *>(own + 0x1b4);
    switch (ss) {
    case 0: ss = 1; break;
    case 1: ss = 10; break;
    case 10:
    case 12:
    case 14:
        own[0x1b2] = 0;
        ++ss;
        break;
    case 11:
    case 13:
        if (photo_cb) ++ss;
        break;
    case 15:
        if (photo_cb) {
            own[0x1bc] = 1;
            own[0x1b2] = 0;
            rp_p3(base) = st[0x20];
            rp_preq(base) = st[0x20];
        }
        break;
    default: break;
    }
    *u3x_rp_pad(base) = 0;
    return 1;
}
// the next followed player: step dir through the 10 trackers to a live one; another target drops the camera
U3X_FN void rp_cycle_player(uintptr_t base, uint8_t *own, int32_t dir, const uint8_t *cur) {
    for (int k = 0; k < 10; ++k) {
        int32_t &sel = *reinterpret_cast<int32_t *>(own + 0x1b8);
        const int32_t v = sel + dir;
        sel = v;
        if (v < 0) sel = v + 10;
        const uint32_t i = static_cast<uint32_t>(sel) % 10u;
        uint8_t *p = u3x_rp_trk(base, i);
        if (!u3x_rp_trk_live(p)) continue;
        if (p != cur) {
            if (uint8_t *c = u3x_rp_h(own + 0x1a0)) {
                uint32_t a = 0;
                reinterpret_cast<void (*)(uint8_t *, uint32_t *, int)>(rd<uintptr_t *>(c, 0)[0x160 / 8])(c, &a, 2);
            }
            *reinterpret_cast<uint64_t *>(own + 0x1a0) = 0;
            *reinterpret_cast<uint32_t *>(own + 0x1a8) = 0;
        }
        sel = static_cast<int32_t>(i);
        break;
    }
}
}  // namespace
extern "C" U3X_FN int u3x_rp_update(uintptr_t base, uint8_t *st) {
    uint8_t *own = rd<uint8_t *>(st, 8);
    uint8_t *W = reinterpret_cast<uint8_t *>(base + cin_writer);
    uint32_t *hud = reinterpret_cast<uint32_t *>(base + 0x1805fa8);
    ++ext_eb()[40];
    if (static_cast<uint32_t>(rp_mode(base) - 1) < 2u) u3h_rp_cam_mode12(base, st, own);
    u3h_rp_render_vars(base, own);
    if (own[0x1b2]) return rp_screenshot_step(base, st, own);
    game_call<void>(base + 0x7230f0, base + 0x17cb060, own, 0);
    const bool menu = u3x_rp_menu_up(base) != 0;
    const uint32_t pad = *u3x_rp_pad(base);
    *hud |= 0x70000u;
    if (!menu && !rp_mpeg(base)) {   // O / X (by the enter button) toggles the HUD, square the controls help
        if (pad & (u3x_rp_enter_cross(base) ? pb_circle : pb_cross)) own[0x1bd] ^= 1;
        if (pad & pb_square) own[0x1bc] ^= 1;
    } else if (own[0x1bf] == 0) {
        u3x_render_defaults(base);
        own[0x1bf] = 1;
    }
    if (own[0x1bd] == 0) *hud &= 0xfff8d7ffu;
    else *hud |= 0x72800u;
    if (own[0x1bc] == 0 || rp_mpeg(base)) {
        *hud &= 0xfff7ffffu;
        if (!menu && !rp_mpeg(base)) {
            if (pad & (u3x_rp_enter_cross(base) ? pb_cross : pb_circle)) u3x_rp_send(base, rp_preq(base) ? 0x953b8f2au : 0x43348860u, u3x_rp_self(own));   // play / pause
            if (pad & pb_tri) {   // camera mode
                uint32_t m;
                if (rp_mode(base) == 0) m = 0x887c7d6eu;
                else if (rp_mode(base) == 2 && !rp_b2d(base)) m = 0x806c9824u;
                else m = 0xff502160u;
                u3x_rp_send(base, m, u3x_rp_self(own));
            }
        }
    } else {
        *hud |= 0x80000u;
    }
    if (rp_mode(base) == 1 && rp_b2d(base)) u3x_rp_send(base, 0xff502160u, u3x_rp_self(own));
    if (own[0x1b0] && !rp_b2c(base)) {   // following a player: up / down cycle it
        int32_t dir = 0;
        if (!menu && !rp_mpeg(base)) dir = (pad & pb_down) ? -1 : ((pad & pb_up) ? 1 : 0);
        const uint8_t *cur = u3x_rp_trk(base, rd<uint32_t>(own, 0x1b8));
        const bool ok = cur && cur[0xe1] && !cur[0x5e4];
        if (!ok) dir = 1;
        if (!ok || dir) rp_cycle_player(base, own, dir, cur);
    }
    game_call<void>(base + 0x6a28c0, base + cin_writer);   // the per-frame stream updates
    game_call<void>(base + 0x6a28c0, base + cin_reader);
    if (at<uint64_t>(base + 0x17b4a68) == 0) game_call<void>(base + 0x1019550, u3x_rp_cammgr(base));
    {   // play speed: the world clocks run at it (B2C caps it at 1.0, B2D at 3.0)
        const float sp = x87_float(rp_speed(base));
        const int32_t t = static_cast<int32_t>(sp * x87_const(0x41a00000u));   // * 20
        *reinterpret_cast<int32_t *>(at<uint8_t *>(base + 0x1b377d8) + 0x90) = t + 20;
        float v = sp;
        if (rp_b2c(base)) v = (1.0f - sp >= 0.0f) ? sp : 1.0f;   // PPC fsel: NaN selects 1.0
        if (rp_b2d(base) && v > x87_const(0x40400000u)) {   // > 3.0
            rp_speed(base) = x87_bits(x87_const(0x40400000u));
            v = x87_const(0x40400000u);
        }
        uint32_t vb = x87_bits(v);
        u3x_ovr_scale(base, reinterpret_cast<uint8_t *>(base + 0x17b4b1c), &vb);
        u3x_ovr_scale(base, reinterpret_cast<uint8_t *>(base + 0x17b4adc), &vb);
    }
    if (at<uint64_t>(base + 0x17b4a68) == 0 && !menu) {
        rp_rumble_off(base, 2);
        rp_rumble_off(base, 0);
        if (rp_preq(base)) {
            if (own[0x1b1]) {
                rp_preq(base) = 0;
                rp_p3(base) = 1;
                own[0x1b1] = 0;
            }
            if (rp_preq(base) && (pad & pb_r2)) {
                rp_preq(base) = 0;
                rp_p3(base) = 1;
            }
        }
    } else {
        rp_preq(base) = 1;
        rp_p3(base) = 1;
    }
    if (rp_pcur(base) != rp_preq(base)) {   // pause state changed
        const uint8_t v = rp_preq(base);
        at<uint8_t>(base + 0x17b4a5d) = v;
        for (int k = 0; k < 2; ++k) {
            uint16_t &c = at<uint16_t>(base + 0x17b4af4 + static_cast<uintptr_t>(k == 0 ? 1 : 0) * 0x40);
            if (v) ++c;
            else c = c ? static_cast<uint16_t>(c - 1) : 0;
        }
        game_call<void>(base + 0x560280, static_cast<uintptr_t>(0), static_cast<int>(v));
        rp_pcur(base) = v;
    }
    {   // past the film's end: the clock goes to its start + duration
        const int32_t elapsed = game_call<int32_t>(base + 0x6a2700, base + cin_writer), dur = rd<int32_t>(W, 0x158);
        if (dur < elapsed) {
            const int32_t t = rd<int32_t>(W, 0x1dbc) + static_cast<int32_t>(static_cast<float>(dur) * x87_const(0x43960000u));   // * 300
            u3x_clock_reset(rp_clock(base), rd<uint32_t>(at<const uint8_t *>(base + 0x1b24b98), 0x20), t);
        }
    }
    const int32_t now = rd<int32_t>(rp_clock(base), 0xc);
    {
        uint32_t heap = 0x14;
        game_call<void>(base + 0xda1590, &heap, base + 0x13ac250, static_cast<uint64_t>(0), base + 0x13ac250);
    }
    uint8_t *rdr = reinterpret_cast<uint8_t *>(base + 0x17c3838), *rr = reinterpret_cast<uint8_t *>(base + 0x17c3850);
    for (;;) {   // the film's events up to now (and their copy into a re-recording)
        const uint32_t pos = rd<uint32_t>(rdr, 8);
        const int32_t t = static_cast<int32_t>(u3x_stream_read(base, rdr, 32));
        if (t == 0) {
            if (rp_mpeg(base)) {
                call_slot(own, 0x1d8);
            } else {
                *reinterpret_cast<uint32_t *>(rdr + 8) = pos;
                rp_preq(base) = 1;
                rp_p3(base) = 1;
                game_call<void>(base + 0x6a3130, base + cin_reader);
            }
            break;
        }
        if (t > now) {
            *reinterpret_cast<uint32_t *>(rdr + 8) = pos;
            break;
        }
        const uint8_t ty = static_cast<uint8_t>(u3x_stream_read(base, rdr, 8));
        uint8_t *ev = game_call<uint8_t *>(base + 0x6c2b50, static_cast<uint32_t>(ty));
        reinterpret_cast<void (*)(uint8_t *, uint8_t *)>(rd<uintptr_t *>(ev, 0)[2])(ev, rdr);   // read
        reinterpret_cast<void (*)(uint8_t *, int)>(rd<uintptr_t *>(ev, 0)[4])(ev, 0);           // apply
        if (at<uint32_t>(base + cin_reader + 8)) {   // re-recording
            if (((rd<uint32_t>(rr, 8) + 7) >> 3 & 0x1fffffffu) > 0x1fc00u) {
                game_call<void>(base + 0xda18b0);
                ++ext_eb()[41];
                return 1;
            }
            u3x_stream_write(base, rr, static_cast<uint32_t>(t), 32);
            u3x_stream_write(base, rr, ev[8], 8);
            reinterpret_cast<void (*)(uint8_t *, uint8_t *)>(rd<uintptr_t *>(ev, 0)[3])(ev, rr);   // write
        }
        if (--ev[0xa] == 0) reinterpret_cast<void (*)(uint8_t *)>(rd<uintptr_t *>(ev, 0)[1])(ev);
    }
    if (!rp_b2c(base) && now - rd<int32_t>(own, 0x6ac) > 3000) {   // a bookmark every 10 s; past 24 keep every other one
        int32_t &count = *reinterpret_cast<int32_t *>(own + 0x6a8);
        if (count > 24) {
            count = 12;
            for (int k = 0; k < 6; ++k) u3x_bookmark_move(rp_bookmark(own, k), rp_bookmark(own, 2 * k));
            for (int j = 6; j < count; ++j) u3x_bookmark_move(rp_bookmark(own, j), rp_bookmark(own, j + 7));
            for (int j = count; j <= 24; ++j) {
                uint8_t *b = rp_bookmark(own, j);
                *reinterpret_cast<uint32_t *>(b + 0x10) = 0;
                *reinterpret_cast<uint32_t *>(b + 8) = 0;
                *reinterpret_cast<uint32_t *>(b + 0x18) = 0;
                *reinterpret_cast<uint32_t *>(b + 0x1c) = 0;
                *reinterpret_cast<uint64_t *>(b + 0x28) = 0;
            }
        }
        *reinterpret_cast<int32_t *>(own + 0x6b0) = count;
        u3x_bookmark_create(base, rp_bookmark(own, count), rd<int32_t>(rp_clock(base), 0xc));
        ++count;
        *reinterpret_cast<int32_t *>(W + 0x1fc4) = count;
        *reinterpret_cast<int32_t *>(own + 0x6ac) = now;
        ++ext_eb()[42];
    }
    at<int32_t>(base + 0x17c5a34) = -1;   // the followed player index of the writer
    if (!rp_b2c(base)) {
        u3h_rp_cam_target(base, own);
    } else {   // no camera
        uint64_t type = base + 0x1ae95e0, arg = 0;
        game_call<uint8_t *>(base + 0x101b530, u3x_rp_cammgr(base), static_cast<void *>(&type), static_cast<void *>(&arg), static_cast<void *>(nullptr));
        *reinterpret_cast<uint32_t *>(own + 0x198) = F_ONE;
    }
    if (own[0x1be]) {
        rp_preq(base) = 1;
        rp_p3(base) = 1;
        own[0x1be] = 0;
    }
    if (at<uint8_t>(base + 0x16e7170)) {
        call_slot(own, 0x1d8);
    } else if (own[0x1c8] && rd<int32_t>(rp_clock(base), 0xc) - rd<int32_t>(own, 0x1c4) > 750) {
        *reinterpret_cast<uint32_t *>(own + 0x19c) = F_ONE;
        *reinterpret_cast<uint32_t *>(own + 0x198) = F_ONE;
        call_slot(own, 0x1c0);
    } else {
        const int32_t t = rd<int32_t>(own, 0x6b4);
        if (static_cast<float>(t) * x87_const(0x3b5a740eu) > 0.0f && t <= rd<int32_t>(at<const uint8_t *>(base + 0x1b24b98), 0x114)) call_slot(own, 0x1c8);
    }
    game_call<void>(base + 0x125c770, st);   // the base update
    game_call<void>(base + 0xda18b0);
    return 1;
}

// ===== v7.204: fourth split, part 8 (the medal sub-checks) =====

extern "C" {
U3H_IMPORT void u3h_kr_stat(uintptr_t base, void *s, uint32_t id, uint32_t amount, uint8_t *np);
U3H_IMPORT uint8_t *u3h_kr_prec(uint8_t *S, uint8_t *np);
}

// v5.16: the medal checks PS3 runs after a kill (0x379xxx..0x381xxx). They walk the score keeper's kill records
// (S +0xe0 count, records of 0x28 from +0xe4, newest first) and report stat events (u3h_kr_stat). The tuning values come
// from DC symbols (0x47efb8e4 chain window, 0xee602459 grenade window, 0x42b7081b long range, 0xa84641cc all enemies,
// 0xdd9356ec streak). info: the kill message {time, ..., killer pos +0x10, victim pos +0x20, killer id +0x30, victim id
// +0x34, weapon +0x38, flags +0x60}. Float maths through the SSE helpers, as in the C part.
namespace {
constexpr uint32_t med_1_300 = 0x3b5a740eu, med_1_16 = 0x3d800000u;
[[gnu::always_inline]] inline bool f_less_equal(uint32_t a, uint32_t b) {   // a <= b as floats (false when either is NaN)
    int r;
    __asm__("movd %1,%%xmm0\n\tmovd %2,%%xmm1\n\txorl %0,%0\n\tucomiss %%xmm0,%%xmm1\n\tsetae %b0" : "=&r"(r) : "r"(a), "r"(b) : "xmm0", "xmm1", "cc");
    return r != 0;
}
U3X_FN uint8_t *med_ng(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17cb060); }
U3X_FN uint32_t med_pid(const uint8_t *np) { return u3x_net_player_id(np); }
U3X_FN uint8_t *med_np_at(uintptr_t base, uint32_t i) {
    if (!at<uint8_t>(base + 0x17d5c61) || i > 9) return nullptr;
    return med_ng(base) + 0x390 + static_cast<uint64_t>(i) * 0x9c0;
}
U3X_FN uint32_t med_np_count(uintptr_t base) {
    uint32_t c = 0;
    for (int i = 0; i < 10; ++i) c += u3x_net_player_in_game(med_ng(base) + 0x390 + static_cast<uint64_t>(i) * 0x9c0) != 0;
    return c;
}
U3X_FN int med_np_index(uintptr_t base, uint32_t id) {
    if (!id) return -1;
    for (int i = 0; i < 10; ++i)
        if (med_pid(med_ng(base) + 0x390 + static_cast<uint64_t>(i) * 0x9c0) == id) return i;
    return -1;
}
U3X_FN int32_t med_team(const uint8_t *np) { return rd<int32_t>(np, 0x21c); }
U3X_FN int32_t med_mode_type(uintptr_t base) {   // the current mode entry's type (0x721050), -1 without one
    const uint8_t *m = game_call<uint8_t *>(base + 0x721050, med_ng(base));
    return m ? rd<int32_t>(m, 0xc) : -1;
}
U3X_FN bool med_friendly(uintptr_t base, const uint8_t *a, const uint8_t *b) {   // same player, or same team (never in type 0xe)
    if (a == b) return true;
    if (med_mode_type(base) == 0xe) return false;
    return a && b && med_team(a) == med_team(b);
}
U3X_FN void *med_dc(uintptr_t base, uint32_t id) {
    const uint64_t *p = game_call<uint64_t *>(base + 0x11f2bf0, id);
    return p ? reinterpret_cast<void *>(*p) : nullptr;
}
U3X_FN const uint32_t *med_weapon_list(uintptr_t base) { return static_cast<const uint32_t *>(med_dc(base, 0x9e354807u)); }
U3X_FN uint32_t med_stat_weapon(uintptr_t base, uint32_t idx) {
    return u3x_weapon_for_stat(reinterpret_cast<const uint32_t *>(base + 0x13a6170), idx < 0x13 ? nullptr : med_weapon_list(base), idx);
}
U3X_FN uint32_t med_stat_index(uintptr_t base, uint32_t sid) {
    return u3x_weapon_stat_number(reinterpret_cast<const uint32_t *>(base + 0x13a6170), med_weapon_list(base), sid);
}
U3X_FN bool med_enemy(const KillRecord *r) { return r->killer_team != r->victim_team; }
U3X_FN uint32_t med_seconds(int32_t t) { return f_mul(f_from_int(t), med_1_300); }
// squared distance between the killer and victim positions of a record (stored * 16)
U3X_FN uint32_t med_distance2(const KillRecord *r) {
    uint32_t s = 0;
    for (int i = 0; i < 3; ++i) {
        const uint32_t d = f_mul(f_sub(f_from_int(r->pos[i]), f_from_int(r->pos[3 + i])), med_1_16);
        s = f_add(s, f_mul(d, d));
    }
    return s;
}
}  // namespace
// f_379c18: a kill from well above or below (|dy| >= 5 and the steep part of the direction)
extern "C" U3X_FN void u3x_kr_379c18(uintptr_t base, void *s, uint8_t *knp, uint8_t *info) {
    const uint32_t *k = reinterpret_cast<const uint32_t *>(info + 0x10), *v = reinterpret_cast<const uint32_t *>(info + 0x20);
    const uint32_t dx = f_sub(v[0], k[0]), dy = f_sub(v[1], k[1]), dz = f_sub(v[2], k[2]);
    if (!f_less_equal(0x40a00000u, dy) && !f_less_equal(dy, 0xc0a00000u)) return;
    const uint32_t len = f_root(f_add(f_add(f_mul(dz, dz), f_mul(dx, dx)), f_mul(dy, dy)));
    const uint32_t n = f_div(dy, len);
    if (f_less(0x3f5db3d7u, n)) u3h_kr_stat(base, s, 0x427bfc52u, 1, knp);
    else if (f_less(n, 0xbf5db3d7u)) u3h_kr_stat(base, s, 0x8a14a6a4u, 1, knp);
}
// f_37952c: deaths since the last kill
extern "C" U3X_FN void u3x_kr_37952c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    const uint32_t id = med_pid(knp);
    uint32_t c = 0;
    for (uint32_t i = 1; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->killer == static_cast<uint16_t>(id)) break;
        if (r->victim == static_cast<uint16_t>(id)) ++c;
    }
    u3h_kr_stat(base, s, 0x5852e7edu, c, knp);
}
// f_3817b8: explosive multi-kill (the same weapon within 1 s); the player's record keeps the best
extern "C" U3X_FN void u3x_kr_3817b8(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info, int team_mode) {
    if (!(rd<uint32_t>(info, 0x60) & 0x20)) return;
    const uint32_t w = rd<uint32_t>(info, 0x38), killer = rd<uint32_t>(info, 0x30);
    const int32_t t = rd<int32_t>(info, 0);
    uint32_t c = 0;
    for (uint32_t i = 0; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (!r->uncounted && r->killer == static_cast<uint16_t>(killer) && med_enemy(r) && f_less(med_seconds(r->time - t) & 0x7fffffffu, F_ONE))
            c += med_stat_weapon(base, r->weapon_index) == w;
    }
    uint32_t id = 0x5bc68f9cu;
    bool emit = true;
    if (team_mode) {
        if (w == 0x159df996u) emit = false;
        else if (w == 0x7525aa5cu) id = 0x2de62476u;
        else if (w == 0x08be7587u) id = 0x55917637u;
    }
    if (emit) u3h_kr_stat(base, s, id, c & 0xff, knp);
    uint8_t *pr = u3h_kr_prec(S, knp);
    if ((c & 0xff) > pr[0x52]) pr[0x52] = static_cast<uint8_t>(c);
}
// f_37992c: kills with the weapon and with the pistol group, and the pistol-only streak
extern "C" U3X_FN void u3x_kr_37992c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info) {
    const KillRecord *r0 = kill_record(S, 0);
    if (!med_enemy(r0)) return;
    const uint32_t id = rd<uint32_t>(info, 0x30), w = rd<uint32_t>(info, 0x38);
    uint32_t c7 = 0, c4 = 0, c3 = 0;
    for (uint32_t i = 0; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->uncounted || r->killer != id || !med_enemy(r)) continue;
        const uint32_t x = med_stat_weapon(base, r->weapon_index);
        if (x == w) {
            ++c7;
            ++c4;
        } else if (u3x_weapon_group(x) == 6) {
            ++c4;
        }
    }
    u3h_kr_stat(base, s, 0xb934b2b2u, c4, knp);
    for (uint32_t i = 0; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->killer == id && med_enemy(r)) {
            if (u3x_weapon_group(med_stat_weapon(base, r->weapon_index)) != 6) break;
            ++c3;
        } else if (r->victim == id) {
            break;
        }
    }
    u3h_kr_stat(base, s, 0xd2b04848u, c3, knp);
    if (w == 0x472bca9au || w == 0x16980e31u || w == 0xc58ae515u || w == 0xb17d9450u) u3h_kr_stat(base, s, w, c7, knp);
    else if (w == 0x22bf34f2u || w == 0xbe729d23u) u3h_kr_stat(base, s, 0x7fdbcbbfu, c7, knp);
}
// f_37ab00: kills chained within a time window (all weapons, then the two grenades)
extern "C" U3X_FN void u3x_kr_37ab00(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info) {
    const uint32_t *window = static_cast<const uint32_t *>(med_dc(base, 0x47efb8e4u));
    const KillRecord *r0 = kill_record(S, 0);
    const uint32_t id = med_pid(knp);
    if (!window) return;
    if (med_enemy(r0)) {
        int32_t t = rd<int32_t>(info, 0);
        uint32_t c = 1;
        for (uint32_t i = 1; i < kill_count(S); ++i) {
            const KillRecord *r = kill_record(S, i);
            if (r->uncounted) continue;
            if (f_less(*window, f_sub(med_seconds(t), med_seconds(r->time)))) break;
            if (r->killer == static_cast<uint16_t>(id) && med_enemy(r)) {
                ++c;
                t = r->time;
            }
        }
        u3h_kr_stat(base, s, 0xf2c76c63u, c, knp);
    }
    const uint32_t w1 = med_stat_index(base, 0xbe281563u), w2 = med_stat_index(base, 0x33e29a1bu);
    const uint32_t *window2 = static_cast<const uint32_t *>(med_dc(base, 0xee602459u));
    if (window2 && (r0->weapon_index == w1 || r0->weapon_index == w2) && med_enemy(r0)) {
        int32_t t = rd<int32_t>(info, 0);
        uint32_t c = 1;
        for (uint32_t i = 1; i < kill_count(S); ++i) {
            const KillRecord *r = kill_record(S, i);
            if (f_less(*window2, f_sub(med_seconds(t), med_seconds(r->time)))) break;
            if (r->killer == static_cast<uint16_t>(id) && med_enemy(r) && (r->weapon_index == w1 || r->weapon_index == w2)) {
                ++c;
                t = r->time;
            }
        }
        u3h_kr_stat(base, s, 0x4bae48b4u, c, knp);
    }
}
// f_37a4bc: long-range kills
extern "C" U3X_FN void u3x_kr_37a4bc(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    const uint32_t *range = static_cast<const uint32_t *>(med_dc(base, 0x42b7081bu));
    if (!range) return;
    const KillRecord *r0 = kill_record(S, 0);
    const uint32_t id = med_pid(knp);
    if (r0->killer != static_cast<uint16_t>(id) || !med_enemy(r0)) return;
    const uint32_t R2 = f_mul(*range, *range);
    if (!f_less_equal(R2, med_distance2(r0))) return;
    uint32_t c = 1;
    for (uint32_t i = 1; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (!r->uncounted && r->killer == static_cast<uint16_t>(id) && med_enemy(r) && f_less_equal(R2, med_distance2(r))) ++c;
    }
    u3h_kr_stat(base, s, 0xa8932480u, c, knp);
}
// f_379ed8: an objective stat chain (record 0's +0x1c) for every player of that team
extern "C" U3X_FN void u3x_kr_379ed8(uintptr_t base, void *s, uint8_t *S) {
    const KillRecord *r0 = kill_record(S, 0);
    if (!r0->f1c) return;
    uint32_t c = 1;
    const uint16_t teams = *reinterpret_cast<const uint16_t *>(&r0->killer_team);
    for (uint32_t i = 1; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (*reinterpret_cast<const uint16_t *>(&r->killer_team) != teams) continue;
        if (r->f1c != r0->f1c) break;
        ++c;
    }
    const uint32_t n = med_np_count(base);
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t *np = med_np_at(base, i);
        if (np && med_team(np) == r0->killer_team) u3h_kr_stat(base, s, static_cast<uint32_t>(r0->f1c), c, np);
    }
}
// f_379ff0: the fewest kills on any enemy
extern "C" U3X_FN void u3x_kr_379ff0(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    uint8_t cnt[10];
    for (uint8_t &b : cnt) b = 0;
    const uint32_t id = med_pid(knp);
    for (uint32_t i = 0; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->killer != static_cast<uint16_t>(id)) continue;
        const int j = med_np_index(base, r->victim);
        if (j >= 0) ++cnt[j];
    }
    uint8_t m = 0x10;
    const uint32_t n = med_np_count(base);
    for (uint32_t i = 0; i < n && i < 10; ++i) {
        const uint8_t *np = med_np_at(base, i);
        if (!med_friendly(base, knp, np) && cnt[i] <= m) m = cnt[i];
    }
    u3h_kr_stat(base, s, 0x610ae399u, m, knp);
}
// f_379430: the betrayal chain (record flag +0x21)
extern "C" U3X_FN void u3x_kr_379430(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    const KillRecord *r0 = kill_record(S, 0);
    const uint16_t k = r0->killer;
    if (k != static_cast<uint16_t>(med_pid(knp)) || r0->victim == k || !r0->f21) return;
    uint32_t c = 1;
    for (uint32_t i = 1; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->killer == k && r->victim != k) c += r->f21 != 0;
    }
    u3h_kr_stat(base, s, 0x8d70ef29u, c, knp);
}
// f_3791ec: trade kills (the killer died after the victim's death, by an explosive; or the victim's explosive kill at the
// same moment)
extern "C" U3X_FN void u3x_kr_3791ec(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *vnp, uint8_t *info) {
    const KillRecord *r0 = kill_record(S, 0);
    const int32_t now = rd<int32_t>(info, 0);
    const uint32_t fl = rd<uint32_t>(info, 0x60);
    if (med_enemy(r0)) {
        int32_t t = 0;
        for (uint32_t i = 0; i < kill_count(S); ++i) {
            const KillRecord *r = kill_record(S, i);
            if (!r->uncounted && r->victim == static_cast<uint16_t>(rd<uint32_t>(info, 0x30))) {
                t = r->time;
                break;
            }
        }
        if (rd<int32_t>(knp, 0x3a0) < t && t < now && (fl & 0x20)) u3h_kr_stat(base, s, 0x9d715fdau, 1, knp);
    }
    for (uint32_t i = 0; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->time < now) break;
        if (r->killer == static_cast<uint16_t>(rd<uint32_t>(info, 0x34)) && med_enemy(r) && r->type == 6) {
            u3h_kr_stat(base, s, 0x9d715fdau, 1, vnp);
            return;
        }
    }
}
// f_3590b0: the player is not counted (effect 0xc054e78b, dead with a character, or +0x770)
extern "C" U3X_FN int u3x_kr_3590b0(uintptr_t base, uint8_t *np) {
    if (game_call<uint8_t>(base + 0x793280, np, 0xc054e78bu, 0u)) return 1;
    if (handle_valid(np + 0x10) && rd<int32_t>(np, 0x39c) <= rd<int32_t>(np, 0x3a0)) return 1;
    return np[0x770];
}
// f_37a914: killed every enemy that is alive (at least DC 0xa84641cc of them)
extern "C" U3X_FN void u3x_kr_37a914(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    const int32_t *minimum = static_cast<const int32_t *>(med_dc(base, 0xa84641ccu));
    if (!minimum) return;
    bool all = true;
    int32_t c = 0;
    const uint32_t me = med_pid(knp);
    for (uint32_t i = 0; i < 10; ++i) {
        uint8_t *np = med_np_at(base, i);
        if (!np || !u3x_net_player_in_game(np) || med_friendly(base, knp, np)) continue;
        if (u3x_kr_3590b0(base, np)) return;
        const uint32_t id = med_pid(np);
        for (uint32_t j = 0; j < kill_count(S); ++j) {
            const KillRecord *r = kill_record(S, j);
            if (r->victim != static_cast<uint16_t>(id)) continue;
            if (r->killer == static_cast<uint16_t>(me)) ++c;
            else all = false;
            break;
        }
    }
    if (all && *minimum <= c) u3h_kr_stat(base, s, 0x4a9da8a5u, 1, knp);
}
// f_37a16c: ended someone's streak (at least DC 0xdd9356ec kills)
extern "C" U3X_FN void u3x_kr_37a16c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp) {
    const uint32_t *streak = static_cast<const uint32_t *>(med_dc(base, 0xdd9356ecu));
    const KillRecord *r0 = kill_record(S, 0);
    if (!streak) return;
    const uint16_t v = r0->victim;
    const bool ffa = med_mode_type(base) == 0xe;
    if (r0->killer == v) return;
    if (!(med_enemy(r0) || ffa || r0->f22 || r0->f21)) return;
    uint32_t c = 0;
    for (uint32_t i = 1; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->uncounted) continue;
        if (r->victim == v) break;
        if (r->killer == v) {
            if (r->victim_team == r0->victim_team && !ffa && !r0->f22 && !r0->f21) break;
            ++c;
        }
    }
    if (*streak <= c) u3h_kr_stat(base, s, 0xf69e35a3u, 1, knp);
}
// f_37a32c: revenge on the player who last killed you
extern "C" U3X_FN void u3x_kr_37a32c(uintptr_t base, void *s, uint8_t *S, uint8_t *knp, uint8_t *info) {
    const KillRecord *r0 = kill_record(S, 0);
    const uint32_t v = rd<uint32_t>(info, 0x34), k = rd<uint32_t>(info, 0x30);
    const bool ffa = med_mode_type(base) == 0xe;
    if (!(med_enemy(r0) || ffa) || kill_count(S) < 2) return;
    if (rd<int32_t>(knp, 0x3a0) < rd<int32_t>(knp, 0x39c)) return;   // the killer is dead
    for (uint32_t i = 1; i < kill_count(S); ++i) {
        const KillRecord *r = kill_record(S, i);
        if (r->uncounted) continue;
        if (r->victim == static_cast<uint16_t>(v) && r->killer == static_cast<uint16_t>(k)) return;
        if (r->victim == static_cast<uint16_t>(k) && (r->killer_team == r0->victim_team || ffa)) {
            if (r->killer == static_cast<uint16_t>(v)) u3h_kr_stat(base, s, 0x6623a3f4u, 1, knp);
            return;
        }
    }
}

// ===== v7.205: fourth split, part 9 (the medal kickback list) =====

extern "C" {
U3H_IMPORT const char *u3h_fmt(void *s, const char *f, int64_t a0, const char *s0, const char *s1, const char *s2);
U3H_IMPORT void u3h_k49_kickback(uintptr_t base, void *s, uint32_t user, int coop, int32_t row);
}
extern "C" void *u3x_unlock_entry(uintptr_t base, void *s, uint32_t id);

// LIST_MEDAL_STREAKS (0x44, PS3 f_345d20): row 0 "None", then the kickbacks of the loadout (DC 0xd9f6ada2 {count,
// entries of 0x28: id, name +4, medal cost +8, tier count +0xc, tiers +0x18 (0x18 each: icon, description +4, duration
// f32 +8), flags +0x20: 1 competitive, 2 co-op}). The selection is 0x17d998c, the equipped kickback loadout item 7.
// a005 row icons: the current tier's icon, the equipped check, "!" when new, else the unlock status icon. Picking a row
// equips it when it is owned (buying is not ported). ExtState ea: [16] row icons, [17] checks, [18] "!", [19] status.
namespace {
U3X_RO const char kb_space[] = " ", kb_break[] = "\\", kb_cash_colour[] = "|@Cff00ff00|", kb_empty[] = "";
constexpr uint32_t kb_table = 0xd9f6ada2u;
U3X_FN uint8_t *kb_symbol(void *s) {
    const uintptr_t lookup = state_native(s, 13);
    return lookup ? reinterpret_cast<uint8_t *(*)(uint32_t)>(lookup)(kb_table) : nullptr;
}
U3X_FN void kb_icon(uintptr_t base, int32_t *ev, uint32_t id) {
    if (u3h_icon_index(base, id) >= 0) u3h_set_icon(base, ev, id);
}
// the kickback's level for the user (0x74eba0 reads only the user at state +0xc), at least 0
U3X_FN int kb_level(void *s, uint32_t user, uint32_t id) {
    const uintptr_t lvl = state_native(s, 33);
    uint32_t kst[8];
    for (uint32_t &w : kst) w = 0;
    kst[3] = user;
    const int lv = lvl ? reinterpret_cast<int (*)(void *, uint32_t)>(lvl)(kst, id) : 0;
    return lv < 0 ? 0 : lv;
}
U3X_FN int kb_status(uintptr_t base, void *s, uint32_t user, uint32_t id) {   // callers check the native first
    const uintptr_t status = state_native(s, 35);
    return reinterpret_cast<int (*)(uintptr_t, uint32_t, void *)>(status)(base + 0x17f3068, user, u3x_unlock_entry(base, s, id));
}
}  // namespace
// the n-th kickback of the competitive (or co-op) list
extern "C" U3X_FN uint8_t *u3x_kickback_nth(void *s, int32_t n, int coop) {
    uint8_t *t = kb_symbol(s);
    if (!t || n < 0) return nullptr;
    const uint32_t cnt = rd<uint32_t>(t, 0), flag = coop ? 2 : 1;
    uint8_t *e = rd<uint8_t *>(t, 8);
    for (uint32_t i = 0; e && i < cnt; ++i, e += 0x28)
        if (rd<uint32_t>(e, 0x20) & flag) {
            if (n == 0) return e;
            --n;
        }
    return nullptr;
}
extern "C" U3X_FN int32_t u3x_kickback_count(void *s, int coop) {
    const uint8_t *t = kb_symbol(s);
    if (!t) return 0;
    const uint32_t cnt = rd<uint32_t>(t, 0), flag = coop ? 2 : 1;
    uint32_t n = 0;
    const uint8_t *e = rd<const uint8_t *>(t, 8);
    for (uint32_t i = 0; e && i < cnt; ++i, e += 0x28)
        if (rd<uint32_t>(e, 0x20) & flag) ++n;
    return static_cast<int32_t>(n);
}
extern "C" U3X_FN void *u3x_unlock_entry(uintptr_t base, void *s, uint32_t id) {
    const uintptr_t find = state_native(s, 24);
    return find ? reinterpret_cast<void *(*)(uintptr_t, int, int)>(find)(base + 0x17f3068, static_cast<int>(id), 0) : nullptr;
}
extern "C" U3X_FN uint8_t *u3x_kickback_by_id(void *s, uint32_t id) {
    uint8_t *t = kb_symbol(s);
    if (!t) return nullptr;
    const uint32_t cnt = rd<uint32_t>(t, 0);
    uint8_t *e = rd<uint8_t *>(t, 8);
    for (uint32_t i = 0; e && i < cnt; ++i, e += 0x28)
        if (rd<uint32_t>(e, 0) == id) return e;
    return nullptr;
}
extern "C" U3X_FN void u3x_kickback_icons(uintptr_t base, void *s, int32_t *ev, int32_t row, int coop, uint32_t user, uint8_t *pr, int32_t loadout) {
    uint32_t *ea = ext_ea();
    const uint8_t *e = u3x_kickback_nth(s, row - 1, coop);
    const uint32_t slot = static_cast<uint32_t>(ev[0x14]);
    if (slot == 0xf5a294f3u) {   // the row icon: the current tier's
        ++ea[16];
        if (row == 0 || !e) {
            kb_icon(base, ev, 0x19d9ea44u);
            return;
        }
        const int lv = kb_level(s, user, rd<uint32_t>(e, 0));
        const uint8_t *tiers = rd<const uint8_t *>(e, 0x18);
        const uint32_t icon = tiers ? rd<uint32_t>(tiers, static_cast<uintptr_t>(lv) * 0x18) : 0;
        if (icon && u3h_icon_index(base, icon) >= 0) u3h_set_icon(base, ev, icon);
        else kb_icon(base, ev, 0x91fb026bu);
        return;
    }
    if (slot != 0) return;
    const uintptr_t get = state_native(s, 34);
    const uint8_t *cur = (pr && get) ? u3x_kickback_by_id(s, reinterpret_cast<uint32_t (*)(void *, int, int32_t)>(get)(pr, 7, loadout)) : nullptr;
    if (e == cur) {   // equipped
        kb_icon(base, ev, 0x08208d0au);
        ++ea[17];
        return;
    }
    if (!e) return;
    const uintptr_t is_new = state_native(s, 36);
    if (is_new && static_cast<uint8_t>(reinterpret_cast<uint64_t (*)(uintptr_t, uint32_t, uint32_t, uint32_t)>(is_new)(base + 0x17f3068, user, rd<uint32_t>(e, 0), 0))) {
        ev[2] = 0x2f;   // "!"
        ev[4] = ev[5] = ev[6] = ev[7] = ev[8] = 0;
        ev[9] = -1;
        ev[10] = 0;
        ev[11] = -1;
        ev[12] = 0x3f800000;
        ++ea[18];
        return;
    }
    if (!state_native(s, 35)) return;
    const int st = kb_status(base, s, user, rd<uint32_t>(e, 0));
    const uint32_t id = st == 1 ? 0xc2bb8920u : st == 2 ? 0x83817313u : st == 3 ? 0x7bebc95fu : 0;
    if (id) {
        kb_icon(base, ev, id);
        ++ea[19];
    }
}
extern "C" U3X_FN int u3x_kickback_source(uintptr_t base, int32_t *ev, void *s) {
    const uint8_t *source = rd<const uint8_t *>(ev, 0x98);
    if (!source || rd<uint32_t>(source, 0x1c) != 0x4c414e44u) return 0;
    if (u3h_gate_closed(base, ev)) return 1;
    *reinterpret_cast<uintptr_t *>(static_cast<uint8_t *>(s) + 760) = base;
    if (!u3h_sym_ready(base, s, kb_table)) {
        if (ev[0] == 6) ev[1] = -1;
        return 1;
    }
    const int32_t loadout = at<int32_t>(base + 0x17d9988);
    const int coop = loadout == 4;
    const uint32_t user = local_user(base) & 3;
    uint8_t *pr = player_profile(base);
    const uint32_t money = pr ? rd<uint32_t>(pr, 0x74c) : 0;
    const int32_t row = ev[1];
    const uint8_t *e;
    char *const buf = static_cast<char *>(s) + 0x2c80, *end = buf + 250;
    char *o = buf;
    *o = 0;
    if (ev[0] == 0x16) u3h_k49_kickback(base, s, user, coop, ev[0x17]);
    switch (ev[0]) {
    case 1:
        set_row_text(ev, u3h_text(base, s, 0x87b43182u));
        [[fallthrough]];   // PS3 also marks the selected kickback as seen
    case 9:
        if ((e = u3x_kickback_nth(s, at<int32_t>(base + 0x17d998c) - 1, coop)))
            if (const uintptr_t seen = state_native(s, 32))
                reinterpret_cast<void (*)(uintptr_t, uint32_t, uint32_t, uint32_t)>(seen)(base + 0x17f3068, user, rd<uint32_t>(e, 0), 0);
        break;
    case 2: {
        const uint32_t sub = static_cast<uint32_t>(ev[0x15]);
        e = u3x_kickback_nth(s, row - 1, coop);
        if (sub == 0) {
            if (row == 0) set_row_text(ev, u3h_text(base, s, 0x9d3c6e80u));
            else if (e) set_row_text(ev, u3h_text(base, s, rd<uint32_t>(e, 4)));
            break;
        }
        if (sub == 3) {   // "Level n\tier text\Costs [A] Medals"
            if (row == 0) {
                set_row_text(ev, u3h_text(base, s, 0x034c8e1eu));
                break;
            }
            if (!e) break;
            const int lv = kb_level(s, user, rd<uint32_t>(e, 0));
            const uint8_t *tiers = rd<const uint8_t *>(e, 0x18);
            char tmp[128];
            tmp[0] = 0;
            u3h_text_num(base, s, tmp, sizeof tmp, 0x69443c24u, rd<int32_t>(e, 8));
            o = append_chars(o, end, u3h_text(base, s, 0xadfc7da6u));
            o = append_chars(o, end, kb_space);
            o = append_number(o, end, lv + 1, false);
            o = append_chars(o, end, kb_break);
            if (tiers && lv < rd<int32_t>(e, 0xc)) o = append_chars(o, end, u3h_text(base, s, rd<uint32_t>(tiers, static_cast<uintptr_t>(lv) * 0x18 + 4)));
            o = append_chars(o, end, kb_break);
            o = append_chars(o, end, tmp);
            *o = 0;
            set_row_text(ev, buf);
            break;
        }
        if (sub == 4) {   // owned, or what unlocks it
            if (row == 0 || !e) break;
            if (u3x_booster_unlocked(s, user, rd<uint32_t>(e, 0))) {
                set_row_text(ev, u3h_text(base, s, 0xecf338f6u));
            } else {
                const uintptr_t item_text = state_native(s, 26);
                set_row_text(ev, item_text ? reinterpret_cast<const char *(*)(uint32_t, uint32_t)>(item_text)(rd<uint32_t>(e, 0), 0) : kb_empty);
            }
            break;
        }
        if (sub == 8) {   // cash
            o = append_chars(o, end, u3h_text(base, s, 0xba6d58f7u));
            o = append_chars(o, end, kb_cash_colour);
            o = append_number(o, end, money, true);
            *o = 0;
            set_row_text(ev, buf);
            break;
        }
        if (!e) break;
        const uint8_t *tiers = rd<const uint8_t *>(e, 0x18);
        if (sub == 0x80d4cc7bu) {
            if (tiers) set_row_text(ev, u3h_text(base, s, rd<uint32_t>(tiers, 4)));
            break;
        }
        if (sub == 0xadacdbf3u) {   // "Duration n seconds" (the first tier's), or "until death"
            if (!tiers) break;
            const float dur = x87_float(rd<uint32_t>(tiers, 8));
            o = append_chars(o, end, u3h_text(base, s, 0x3badfdd5u));
            o = append_chars(o, end, kb_space);
            if (dur <= 0.0f) {
                o = append_chars(o, end, u3h_text(base, s, 0xa80e2d11u));
            } else {
                o = append_number(o, end, static_cast<int64_t>(dur + x87_const(0x3f000000u)), false);
                o = append_chars(o, end, kb_space);
                o = append_chars(o, end, u3h_text(base, s, 0xe7624faau));
            }
            *o = 0;
            set_row_text(ev, buf);
            break;
        }
        if (sub == 0xb684d68bu) set_row_text(ev, u3h_fmt(s, u3h_text(base, s, 0x819103ccu), rd<int32_t>(e, 8), nullptr, nullptr, nullptr));
        break;
    }
    case 3:   // not unlocked: result 2
        if (row == 0 || !(e = u3x_kickback_nth(s, row - 1, coop))) break;
        if (!u3x_booster_unlocked(s, user, rd<uint32_t>(e, 0))) ev[14] = 2;
        break;
    case 4:
        if (!(ext_ea()[0] & 16)) u3x_kickback_icons(base, s, ev, row, coop, user, pr, loadout);
        break;
    case 12:
        if (!(ext_ea()[0] & 32)) {
            ev[0x1f] = 0x43000000;
            ev[0x20] = 0x43000000;
        }
        break;
    case 5: ev[1] = at<int32_t>(base + 0x17d998c); break;
    case 6: ev[1] = u3x_kickback_count(s, coop); break;
    case 10: at<int32_t>(base + 0x17d998c) = row; break;
    case 11:
        if (static_cast<uint32_t>(ev[0x13]) == 0x67a48eb9u) ev[14] = 4;
        break;
    case 0x11: ev[1] = 8; break;
    case 0x16: {   // equip when owned (PS3 f_328a40, stripped on PS4); buying is not ported
        const uintptr_t set = state_native(s, 28);
        if (!set || !pr) break;
        const int32_t r = ev[0x17];
        if (r <= 0) {
            reinterpret_cast<void (*)(void *, int, uint32_t, int32_t)>(set)(pr, 7, 0, loadout);
            break;
        }
        if (!(e = u3x_kickback_nth(s, r - 1, coop))) break;
        if (state_native(s, 35) && kb_status(base, s, user, rd<uint32_t>(e, 0)) == 0)
            reinterpret_cast<void (*)(void *, int, uint32_t, int32_t)>(set)(pr, 7, rd<uint32_t>(e, 0), loadout);
        break;
    }
    }
    return 1;
}

// ===== v7.206: fourth split, part 10 (the leaderboards) =====

extern "C" {
U3H_IMPORT void *u3h_ona(void);
U3H_IMPORT uint8_t *u3h_lb_buf(int which);
U3H_IMPORT int u3h_m02_lb_local(uintptr_t base, void *s, uint8_t *m, uint32_t board, int32_t req);
U3H_IMPORT uint32_t u3h_fr_lb_names(uintptr_t base, char *out, uint32_t max);
U3H_IMPORT void u3h_pm119(uintptr_t base, void *s, uint8_t *param);
}

// a013: PS3 1.19's leaderboard manager (mgr = 0x17d6230, f_328870 init, f_328a28 query, f_328614 submit, f_329058 the
// worker) that PS4 cut; the widget update tail (kind 66) asks for a page. The network goes through the online interface
// (lb_get / lb_range / lb_update / lb_poll tickets, polled each frame; offline = PS3's "cannot connect"); without a
// community server the local profile's own row is shown (m002). Manager: own NpId +0, friends-row count +0x24, rows +0x28,
// blobs +0x30, names +0x38, temp rows +0x40, temp blobs +0x48, flag +0x50, busy +0x58, board +0x60, requested page +0x64,
// row count +0x70, page +0x74, reset +0x78, board index +0x7c, friends +0x80, refresh +0x81. Rows {name 0x5c, rank,
// ..., score +0x70} of 0x80; 11 per page. Boards above 0x513 move by 4 x the week (DC 0x8466d21a). U3MP_LB119=0
// (ea[1] bit 4) turns it off. ExtState ea: [48] inits, [49] queries, [50] rows, [51] submits, [52] dropped, [53] errors,
// [54] done, [55] widget asks, [56] no week; the 4 boards {board, column set, title} at ea[200..211].
namespace {
struct LbRow {
    char name[0x5c];
    uint32_t rank;
    uint8_t pad0[0x10];
    int64_t score;
    uint8_t pad1[8];
};
static_assert(sizeof(LbRow) == 0x80);
struct LbWork { int32_t st, ticket, page, n; };
struct LbSlot {
    int32_t used, ticket;
    uint32_t board;
    int64_t score;
    uint8_t blob[0x40];
};
struct OnlineIface {
    uint32_t init;
    void *party_send, *server_notice, *chat_restricted;
    int32_t (*lb_get)(uintptr_t base, uint32_t board, const char *names, uint32_t n, uint8_t *rows, uint8_t *blobs, uint32_t max);
    int32_t (*lb_range)(uintptr_t base, uint32_t board, uint32_t first, uint32_t last, uint8_t *rows, uint8_t *blobs, uint32_t max);
    int32_t (*lb_update)(uintptr_t base, uint32_t board, const char *name, int64_t score, const uint8_t *blob64);
    int32_t (*lb_poll)(uintptr_t base, int32_t ticket);   // -2 pending, -1 failed / offline, else rows
    uint32_t (*session_names)(uintptr_t base, char *out, uint32_t max);
    uint32_t (*friend_names)(uintptr_t base, char *out, uint32_t max);
};
U3X_BSS LbWork lb_work;
U3X_BSS LbSlot lb_slots[5];
U3X_FN uint8_t *lb_mgr(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17d6230); }
U3X_FN OnlineIface *ona() { return static_cast<OnlineIface *>(u3h_ona()); }
U3X_FN bool same_npid(const char *a, const char *b) {   // strcmp == 0
    int i = 0;
    while (a[i] && a[i] == b[i]) ++i;
    return a[i] == b[i];
}
}  // namespace
extern "C" U3X_FN int u3x_lb_off(void) { return (ext_ea()[1] & 0x10u) != 0; }
extern "C" U3X_FN uint32_t u3x_lb_week_board(void *s, uint32_t board) {
    if (board > 0x513) {
        if (const uint8_t *w = dc_table(s, 0x8466d21au)) board += static_cast<uint32_t>(rd<int32_t>(w, 0) * 4);
        else ++ext_ea()[56];
    }
    return board;
}
// f_328870 (NetInit): own NpId (the profile name at net +0x3b8), the buffers, the fields
extern "C" U3X_FN void u3x_lb_init(uintptr_t base, void *) {
    if (u3x_lb_off()) return;
    uint8_t *m = lb_mgr(base);
    const char *me = reinterpret_cast<const char *>(base + 0x17d6010);
    for (int i = 0; i < 0x24; ++i) m[i] = 0;
    for (int i = 0; i < 16 && me[i]; ++i) m[i] = static_cast<uint8_t>(me[i]);
    *reinterpret_cast<uint32_t *>(m + 0x24) = 0;
    *reinterpret_cast<uint8_t **>(m + 0x28) = u3h_lb_buf(0);
    *reinterpret_cast<uint8_t **>(m + 0x30) = u3h_lb_buf(1);
    *reinterpret_cast<uint8_t **>(m + 0x38) = u3h_lb_buf(2);
    *reinterpret_cast<uint8_t **>(m + 0x40) = u3h_lb_buf(3);
    *reinterpret_cast<uint8_t **>(m + 0x48) = u3h_lb_buf(4);
    for (int i = 0; i < 0x160; ++i) m[0x88 + i] = 0;
    for (LbSlot &k : lb_slots) k.used = 0;
    lb_work.st = 0;
    *reinterpret_cast<uint64_t *>(m + 0x58) = 0;
    *reinterpret_cast<uint64_t *>(m + 0x68) = 0;
    *reinterpret_cast<uint64_t *>(m + 0x60) = 0;
    *reinterpret_cast<int32_t *>(m + 0x74) = -1;
    m[0x81] = 0;
    m[0x78] = 1;
    m[0x80] = 0;
    *reinterpret_cast<uint32_t *>(m + 0x50) = 0;
    *reinterpret_cast<uint32_t *>(m + 0x7c) = 0;
    *reinterpret_cast<uint32_t *>(m + 0x24) = 0;
    *reinterpret_cast<uint32_t *>(m + 0x70) = 0;
    ++ext_ea()[48];
}
// f_328a28: start the page query unless one is running
extern "C" U3X_FN void u3x_lb_query(uintptr_t base, void *s, uint32_t board, int32_t page) {
    uint8_t *m = lb_mgr(base);
    if (rd<uint64_t>(m, 0x58)) return;
    board = u3x_lb_week_board(s, board);
    *reinterpret_cast<int32_t *>(m + 0x64) = page;
    *reinterpret_cast<uint32_t *>(m + 0x60) = board;
    *reinterpret_cast<uint64_t *>(m + 0x58) = 1;
    lb_work.st = 1;
    ++ext_ea()[49];
}
// f_328614: one of 5 score slots, else dropped (-1)
extern "C" U3X_FN int u3x_lb_submit(uintptr_t base, void *s, uint32_t board, int64_t score, const uint8_t *blob) {
    board = u3x_lb_week_board(s, board);
    for (LbSlot &k : lb_slots) {
        if (k.used) continue;
        for (int j = 0; j < 0x40; ++j) k.blob[j] = blob[j];
        k.score = score;
        k.board = board;
        k.ticket = ona()->lb_update(base, board, reinterpret_cast<const char *>(lb_mgr(base)), score, k.blob);
        k.used = k.ticket >= 0;
        ++ext_ea()[51];
        return 0;
    }
    ++ext_ea()[52];
    return -1;
}
extern "C" U3X_FN void u3x_lb_net_error(uintptr_t base) {   // f_325618(net, 1)
    at<uint8_t>(base + 0x17d5c58 + 0x28) = 1;
    ++ext_ea()[53];
}
extern "C" U3X_FN int u3x_lb_me(uintptr_t base, const char *name) { return same_npid(reinterpret_cast<const char *>(lb_mgr(base)), name); }
// f_329058's steps, one per frame while a ticket is pending: my rank -> my page, a page range, or the friends board
extern "C" U3X_FN void u3x_lb_tick(uintptr_t base, void *s) {
    if (u3x_lb_off()) return;
    OnlineIface *o = ona();
    uint8_t *m = lb_mgr(base);
    uint8_t *rows = u3h_lb_buf(0), *blobs = u3h_lb_buf(1), *names = u3h_lb_buf(2), *trows = u3h_lb_buf(3), *tblobs = u3h_lb_buf(4);
    for (LbSlot &k : lb_slots)   // finished score submits free their slot
        if (k.used && o->lb_poll(base, k.ticket) != -2) k.used = 0;
    if (!lb_work.st) return;
    const uint32_t board = rd<uint32_t>(m, 0x60);
    const int32_t req = rd<int32_t>(m, 0x64);
    uint32_t &count = *reinterpret_cast<uint32_t *>(m + 0x70);
    int32_t &page_now = *reinterpret_cast<int32_t *>(m + 0x74);
    bool done = false, range = false;
    if (lb_work.st == 1) {
        if (u3h_m02_lb_local(base, s, m, board, req)) {   // no community server: the local profile's row
            done = true;
        } else {
            *reinterpret_cast<uint32_t *>(m + 0x24) = 0;
            if (!m[0x80]) {   // the global board
                count = 0;
                if (req == -1) {   // my rank first
                    for (int i = 0; i < 0x24; ++i) names[i] = m[i];
                    lb_work.ticket = o->lb_get(base, board, reinterpret_cast<const char *>(names), 1, trows, tblobs, 102);
                    lb_work.st = 2;
                    if (lb_work.ticket >= 0) return;
                    lb_work.page = 0;
                    u3x_lb_net_error(base);
                } else {
                    lb_work.page = req;
                }
                range = true;
            } else {   // friends: session members, friends not in it, me
                uint32_t n = u3h_fr_lb_names(base, reinterpret_cast<char *>(names), 101);
                char fr[100 * 0x24];
                uint32_t c = o->friend_names(base, fr, 100);
                if (c > 100) c = 100;
                for (uint32_t i = 0; i < c && n <= 100; ++i) {
                    const char *f = fr + i * 0x24;
                    bool dup = false;
                    for (uint32_t k = 0; k < n && !dup; ++k) dup = same_npid(reinterpret_cast<const char *>(names) + k * 0x24, f);
                    if (dup) continue;
                    for (int j = 0; j < 0x24; ++j) names[n * 0x24 + j] = static_cast<uint8_t>(f[j]);
                    ++n;
                }
                for (int j = 0; j < 0x24; ++j) names[n * 0x24 + j] = m[j];
                lb_work.n = static_cast<int32_t>(n) + 1;
                lb_work.ticket = o->lb_get(base, board, reinterpret_cast<const char *>(names), n + 1, trows, tblobs, 102);
                lb_work.st = 4;
                if (lb_work.ticket >= 0) return;
                *reinterpret_cast<int32_t *>(m + 0x24) = -1;
                u3x_lb_net_error(base);
                done = true;
            }
        }
    } else if (lb_work.st == 2) {   // my rank -> my page
        const int32_t r = o->lb_poll(base, lb_work.ticket);
        if (r == -2) return;
        if (r < 0) {
            lb_work.page = 0;
            u3x_lb_net_error(base);
        } else {
            lb_work.page = r > 0 ? static_cast<int32_t>((reinterpret_cast<const LbRow *>(trows)[0].rank + 1u - 1u) / 11u) : 0;
        }
        range = true;
    } else if (lb_work.st == 3) {   // the page's rows
        int32_t r = o->lb_poll(base, lb_work.ticket);
        if (r == -2) return;
        if (r > 0) {
            if (r > 11) r = 11;
            for (int32_t i = 0; i < r; ++i) reinterpret_cast<LbRow *>(rows)[i].rank = static_cast<uint32_t>(lb_work.page * 11 + i + 1);
            count = static_cast<uint32_t>(r);
            ext_ea()[50] += static_cast<uint32_t>(r);
        }
        done = true;
    } else if (lb_work.st == 4) {   // the friends' rows, sorted by rank, my page
        const int32_t r = o->lb_poll(base, lb_work.ticket);
        if (r == -2) return;
        if (r < 0) {
            *reinterpret_cast<int32_t *>(m + 0x24) = -1;
            u3x_lb_net_error(base);
        } else {
            *reinterpret_cast<int32_t *>(m + 0x24) = r;
            if (r == 0) {
                count = 0;
            } else {
                LbRow *R = reinterpret_cast<LbRow *>(trows);
                for (int32_t i = 0; i < r; ++i) R[i].rank += 1;   // rank = rank0 + 1
                bool swapped;
                do {   // bubble sort by rank, rows and blobs together
                    swapped = false;
                    for (int32_t i = 0; i + 1 < r; ++i) {
                        if (!(R[i + 1].rank < R[i].rank)) continue;
                        const LbRow t = R[i];
                        R[i] = R[i + 1];
                        R[i + 1] = t;
                        for (int j = 0; j < 0x40; ++j) {
                            const uint8_t b = tblobs[i * 0x40 + j];
                            tblobs[i * 0x40 + j] = tblobs[(i + 1) * 0x40 + j];
                            tblobs[(i + 1) * 0x40 + j] = b;
                        }
                        swapped = true;
                    }
                } while (swapped);
                uint32_t me = 0;
                for (int32_t i = 0; i < r; ++i)
                    if (u3x_lb_me(base, R[i].name)) me = static_cast<uint32_t>(i);
                int32_t page;
                if (req == -1) {
                    page = static_cast<int32_t>(me / 11u);
                } else {
                    int32_t mx = page_now;
                    if (count > 10) ++mx;
                    page = req;
                    if (mx < req) {
                        page = page_now;
                        if (count > 10) ++page;
                    }
                }
                count = 0;
                page_now = page;
                for (int32_t i = 0; i < r; ++i) {
                    if (static_cast<uint32_t>(i) < static_cast<uint32_t>(page) * 11u || count >= 11) continue;
                    reinterpret_cast<LbRow *>(rows)[count] = R[i];
                    for (int j = 0; j < 0x40; ++j) blobs[count * 0x40 + j] = tblobs[i * 0x40 + j];
                    ++count;
                }
                ext_ea()[50] += count;
            }
        }
        done = true;
    } else {
        return;
    }
    if (range && !done) {   // the range of the page
        page_now = lb_work.page;
        count = 0;
        lb_work.ticket = o->lb_range(base, board, static_cast<uint32_t>(lb_work.page) * 11u, static_cast<uint32_t>(lb_work.page) * 11u + 10u, rows, blobs, 11);
        if (lb_work.ticket >= 0) {
            lb_work.st = 3;
            return;
        }
        done = true;
    }
    if (done) {
        lb_work.st = 0;
        *reinterpret_cast<uint64_t *>(m + 0x58) = 0;
        ++ext_ea()[54];
    }
}
// f_2551c0's tail (kind 66, 0x63a426): the widget's board index -> its query and column set (4 boards)
extern "C" U3X_FN void u3x_lb_widget_tail(uintptr_t base, void *s, uint8_t *w, int32_t idx) {
    *reinterpret_cast<int32_t *>(w + 0x20) = idx;
    if (idx < 0 || idx > 3) idx = 0;
    const uint32_t *tb = &ext_ea()[200 + idx * 3];
    if (!u3x_lb_off()) u3x_lb_query(base, s, tb[0], rd<int32_t>(w, 0x1c));
    *reinterpret_cast<uint32_t *>(w + 0x10) = tb[1];
    ++ext_ea()[55];
}
extern "C" U3X_FN int u3x_lb_kind(uintptr_t base, void *s, uint64_t *ev, int kind) {
    if (kind == 66) {
        u3x_lb_widget_tail(base, s, reinterpret_cast<uint8_t *>(ev[0]), static_cast<int32_t>(ev[2]));
        return 1;
    }
    if (kind == 67) {
        u3h_pm119(base, s, reinterpret_cast<uint8_t *>(ev[0]));
        return 1;
    }
    return 0;
}

// ===== v7.207: fourth split, part 11 (the LAN backend class) =====

extern "C" {
U3H_IMPORT void u3h_lnb_follow_tick(void *be, uintptr_t base, void *s);
U3H_IMPORT void u3h_lnb_cli_update(uintptr_t base, void *bind);
U3H_IMPORT void u3h_lnb_send_join(void *be, void *bind, uint8_t *ss, int host, int32_t *res);
U3H_IMPORT void u3h_lnb_rank_stamp(void *be, uint8_t *md, uint32_t len);
U3H_IMPORT uint64_t *u3h_lnb_match_room(void);
U3H_IMPORT uint8_t *u3h_lnb_udpbuf(void);
}

// b010: PS3's LAN backend class (vtable 0x10f8018) behind the PS4 session code: slot 1 Update (LAN discovery: 0xc8 asks,
// 0xc9 answers with the rooms of the sessions this PC hosts; then each binding's room server and client), 2 Shutdown,
// 3 AttachSession, 4 Create (host a room server on 0x2333 (party) / 0x2332 (match), join it through 127.0.0.1, wait up to
// 3 s for the room reply), 5 Search (broadcast 0xc8 on 0x2334), 6 Join, 7 Leave, 9 Kick, 12 MemberData, and with
// U3MP_LAN_SOLO 11 (create a net player per member) / 14 (the host raises the start-load flag); the other slots do nothing.
// Up to 4 session bindings {session, room server, client socket, receive buffer}; the Find a Party list (20 rooms of
// 0x38) at +0x58. Without sockets (or without a room reply) U3MP_LAN_SOLO (eb[0] bit 1 clear) makes a one-member room
// locally. ExtState eb: [64..93] backend counters (see the C part's b010 notes).
namespace {
struct LanBind {
    uint8_t *session;
    RoomServer srv;
    Sock cli;
    uint32_t rxlen;
    uint8_t rx[0x800];
};
static_assert(sizeof(LanBind) == 0x9140 && __builtin_offsetof(LanBind, cli) == 0x8930 && __builtin_offsetof(LanBind, rx) == 0x8940);
struct LanBackend {
    void **vt;
    char name[0x24];
    uint8_t pad2c[0x24];
    int32_t nrooms;
    uint32_t pad54;
    uint8_t rooms[20][0x38];
    LanBind b[4];
    uint32_t uport;
    Sock udp;
    uintptr_t base;
    void *state;
};
static_assert(sizeof(LanBackend) == 0x249d8 && __builtin_offsetof(LanBackend, b) == 0x4b8 && __builtin_offsetof(LanBackend, udp) == 0x249bc &&
              __builtin_offsetof(LanBackend, base) == 0x249c8);
constexpr uint32_t lan_loopback = 0x0100007fu;   // 127.0.0.1 as stored in a sockaddr
U3X_RO const char default_player[] = "Player";
U3X_FN LanBackend &lan_be(void *be) { return *static_cast<LanBackend *>(be); }
U3X_FN bool lan_solo_on() { return !(ext_eb()[0] & 2); }
U3X_FN uint8_t *lan_match_ss(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17d6420); }
U3X_FN uint8_t *lan_party_ss(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17dc298); }
U3X_FN void clear_members(uintptr_t base, uint8_t *ss) { game_call<int64_t>(base + 0x7a25f0, ss); }
U3X_FN void usleep_us(uint32_t us) {   // sys_timer_usleep, when the builder found it
    const uintptr_t f = *reinterpret_cast<const uintptr_t *>(&ext_eb()[10]);
    if (f) reinterpret_cast<int (*)(uint32_t)>(f)(us);
}
// the slot call counters and the last 32 slots (state +1900 / +1904), probe +0xb0 / +0xb4
U3X_FN void note_slot(LanBackend &be, int slot) {
    uint32_t *eb = ext_eb();
    ++eb[67];
    eb[68] = static_cast<uint32_t>(slot);
    if (!be.state) return;
    uint8_t *st = static_cast<uint8_t *>(be.state);
    ++*reinterpret_cast<uint32_t *>(st + 1280 + 0xb0);
    *reinterpret_cast<uint32_t *>(st + 1280 + 0xb4) = static_cast<uint32_t>(slot);
    uint32_t &h = *reinterpret_cast<uint32_t *>(st + 1900);
    st[1904 + h % 32] = static_cast<uint8_t>(slot);
    ++h;
}
U3X_FN int bind_of(LanBackend &be, const uint8_t *ss) {
    for (int i = 0; i < 4; ++i)
        if (be.b[i].session == ss) return i;
    return -1;
}
U3X_FN bool session_hosting(uint8_t *ss) { return (u3h_lnb_call(ss, 0) & 0xff) != 0; }
}  // namespace
extern "C" U3X_FN int64_t u3x_lnb_s12(void *be, uint8_t *ss, const uint8_t *data, uint32_t len);
// slot 1, Update (f_38199c)
extern "C" U3X_FN int64_t u3x_lnb_s1(void *be_) {
    LanBackend &be = lan_be(be_);
    const uintptr_t base = be.base;
    uint32_t *eb = ext_eb();
    ++eb[84];
    u3h_lnb_follow_tick(be_, be.base, be.state);
    uint8_t *buf = u3h_lnb_udpbuf();
    if (u3x_sock_open(&be.udp) && net_fn(net_recvfrom))
        for (;;) {
            for (int k = 0; k < 0x1000; ++k) buf[k] = 0;
            uint32_t ip = 0, port = 0;
            const int32_t n = u3x_sock_recvfrom(&be.udp, buf, 0xfff, &ip, &port);
            if (n < 1) break;
            ByteStream rd_s{buf, 0, static_cast<uint32_t>(n)};
            const uint32_t id = static_cast<uint32_t>(u3x_stream_get(&rd_s, 1));
            if (id == 0xc8) {   // someone looks for rooms: answer with ours
                if (ip == be.udp.ip) continue;   // our own broadcast
                uint8_t out[0x800];
                ByteStream w{out, 0, 0x800};
                u3x_stream_put(&w, 0xc9, 1);
                uint32_t cnt = 0;
                for (const LanBind &b : be.b)
                    if (b.session && session_hosting(b.session) && u3x_lnb_srv_running(&b.srv)) ++cnt;
                u3x_stream_put(&w, cnt, 4);
                for (LanBind &b : be.b) {
                    if (!b.session || !session_hosting(b.session) || !u3x_lnb_srv_running(&b.srv)) continue;
                    uint8_t blk[0x80];
                    for (uint8_t &x : blk) x = 0;
                    for (int k = 0; k < 0x24; ++k) blk[k] = static_cast<uint8_t>(be.name[k]);
                    u3x_stream_put(&w, b.srv.listen.port & 0xffff, 2);
                    u3x_stream_put(&w, b.srv.room, 8);
                    u3x_stream_put(&w, u3x_lnb_srv_count(&b.srv), 2);
                    u3x_stream_put(&w, b.srv.maxp & 0xffff, 2);
                    for (int k = 0; k < 0x80; ++k) u3x_stream_put(&w, blk[k], 1);
                }
                u3x_sock_sendto(&be.udp, out, w.pos < w.max ? w.pos : w.max, ip, port);
                ++eb[74];
            } else if (id == 0xc9) {   // an answer: its rooms go into the Find a Party list
                const uint32_t cnt = static_cast<uint32_t>(u3x_stream_get(&rd_s, 4));
                for (uint32_t i = 0; i < cnt && rd_s.pos < 0x1000; ++i) {
                    const uint32_t rport = static_cast<uint32_t>(u3x_stream_get(&rd_s, 2));
                    const uint64_t room = u3x_stream_get(&rd_s, 8);
                    const uint32_t members = static_cast<uint32_t>(u3x_stream_get(&rd_s, 2)), cap = static_cast<uint32_t>(u3x_stream_get(&rd_s, 2));
                    uint8_t blk[0x80];
                    for (uint8_t &x : blk) x = static_cast<uint8_t>(u3x_stream_get(&rd_s, 1));
                    if (be.nrooms >= 20) continue;
                    uint8_t *e = be.rooms[be.nrooms];
                    *reinterpret_cast<uint64_t *>(e) = room;
                    *reinterpret_cast<uint32_t *>(e + 8) = ip;
                    *reinterpret_cast<uint16_t *>(e + 0xc) = static_cast<uint16_t>(rport);
                    *reinterpret_cast<uint16_t *>(e + 0xe) = static_cast<uint16_t>(members);
                    *reinterpret_cast<uint16_t *>(e + 0x10) = static_cast<uint16_t>(cap);
                    for (int k = 0; k < 0x24; ++k) e[0x12 + k] = blk[k];
                    ++be.nrooms;
                    ++eb[75];
                }
            } else {
                ++eb[83];   // PS3 traps here
            }
        }
    for (LanBind &b : be.b) {
        if (!b.session) continue;
        u3x_lnb_srv_update(&b.srv);
        u3h_lnb_cli_update(base, &b);
    }
    return 0;
}
// slot 2, Shutdown (f_3818d0)
extern "C" U3X_FN int64_t u3x_lnb_s2(void *be_) {
    LanBackend &be = lan_be(be_);
    note_slot(be, 2);
    int32_t r = u3x_sock_close(&be.udp);
    for (LanBind &b : be.b)
        if (b.session && u3x_sock_open(&b.cli)) r = u3x_sock_close(&b.cli);
    return r;
}
// slot 3, AttachSession (f_382720)
extern "C" U3X_FN int64_t u3x_lnb_s3(void *be_, uint8_t *ss) {
    LanBackend &be = lan_be(be_);
    note_slot(be, 3);
    int i = bind_of(be, ss);
    if (i >= 0) return i;
    for (i = 0; i < 4; ++i)
        if (!be.b[i].session) {
            be.b[i].session = ss;
            *reinterpret_cast<void **>(ss + 8) = be_;
            return i;
        }
    return -1;
}
// U3MP_LAN_SOLO: a one-member room with this PC hosting (no sockets, or no room reply)
extern "C" U3X_FN void u3x_lnb_solo_create(void *be_, uint8_t *ss, uint32_t maxp) {
    LanBackend &be = lan_be(be_);
    ++ext_eb()[82];
    if (be.state) ++*reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(be.state) + 1280 + 0xac);
    *reinterpret_cast<void **>(ss + 8) = be_;
    uint8_t *m = ss + 0x680;
    for (int k = 0; k < 0x120; ++k) m[k] = 0;
    const char *name = be.state ? u3h_onp_id(be.state) : default_player;
    for (int i = 0; i < 31 && name[i]; ++i) m[i] = static_cast<uint8_t>(name[i]);
    m[0x25] = 1;
    *reinterpret_cast<uint32_t *>(m + 0x2c) = 1;
    *reinterpret_cast<uint32_t *>(m + 0x34) = 0;
    *reinterpret_cast<uint32_t *>(ss + 0x18) = 1;
    *reinterpret_cast<uint32_t *>(ss + 0x12e4) = 1;
    *reinterpret_cast<uint32_t *>(ss + 0x12e8) = 1;
    *reinterpret_cast<uint32_t *>(ss + 0x20c) = maxp;
    *reinterpret_cast<int32_t *>(ss + 0xa4) = 0;
    ss[0xa1] = 1;
}
// slot 4, Create (f_382d9c): our room server, joined through 127.0.0.1; up to 30 x 100 ms for the room reply
extern "C" U3X_FN int64_t u3x_lnb_s4(void *be_, uint8_t *ss, uint32_t maxp) {
    LanBackend &be = lan_be(be_);
    uint32_t *eb = ext_eb();
    note_slot(be, 4);
    ++eb[69];
    const uintptr_t base = be.base;
    const int i = bind_of(be, ss);
    if (i < 0) {
        eb[70] = static_cast<uint32_t>(-1);
        return -1;
    }
    LanBind &b = be.b[i];
    const uint32_t port = session_hosting(ss) ? 0x2333 : 0x2332;
    const int32_t sr = u3x_lnb_srv_start(&b.srv, 1, port, maxp);
    if (sr < 0 && lan_solo_on()) {
        u3x_lnb_solo_create(be_, ss, maxp);
        eb[70] = 0;
        return 0;
    }
    clear_members(base, ss);
    *reinterpret_cast<uint32_t *>(ss + 0x20c) = maxp;
    b.rxlen = 0;
    u3x_sock_connect(&b.cli, lan_loopback, port, 0, 0);   // the result is unused, as on PS3
    u3h_lnb_call(be_, 1);
    int32_t r;
    u3h_lnb_send_join(be_, &b, ss, 1, &r);
    eb[92] = static_cast<uint32_t>(r);
    ss[0xa1] = 0;
    for (int k = 0; k < 30; ++k) {
        usleep_us(100000);
        u3h_lnb_call(be_, 1);
        if (!ss[0xa1]) continue;
        ss[0xa1] = 1;
        if (!rd<uint64_t>(ss, 0x18)) *reinterpret_cast<int32_t *>(ss + 0xa4) = -1;
        eb[70] = static_cast<uint32_t>(r);
        if (ss == lan_match_ss(base) && rd<uint64_t>(ss, 0x18)) {   // x010: the party learns the match room
            uint8_t *ps = lan_party_ss(base);
            *u3h_lnb_match_room() = rd<uint64_t>(ss, 0x18);
            if (rd<uint64_t>(ps, 0x18) && rd<uint32_t>(ps, 0x12e4) == rd<uint32_t>(ps, 0x12e8) && rd<uint32_t>(ps, 0x12f0) == 0x40) {
                u3x_lnb_s12(be_, ps, ps + 0x12f4, 0x40);
                ++reinterpret_cast<uint32_t *>(u3h_ext() + 0xbc0)[24];
            }
        }
        return r;
    }
    u3x_lnb_srv_stop(&b.srv);
    if (lan_solo_on()) {
        u3x_sock_close(&b.cli);
        u3x_lnb_solo_create(be_, ss, maxp);
        eb[70] = 0;
        return 0;
    }
    eb[70] = static_cast<uint32_t>(-1);
    return -1;
}
// slot 5, Search (f_3817ec): broadcast 0xc8
extern "C" U3X_FN int64_t u3x_lnb_s5(void *be_) {
    LanBackend &be = lan_be(be_);
    note_slot(be, 5);
    be.nrooms = 0;
    uint8_t out[0x10];
    ByteStream w{out, 0, 0x10};
    u3x_stream_put(&w, 200, 1);
    if (u3x_sock_open(&be.udp) && net_fn(net_sendto)) {
        u3x_sock_sendto(&be.udp, out, w.pos, 0xffffffffu, be.uport & 0xffff);
        ++ext_eb()[73];
    }
    return 0;
}
// slot 6, Join (f_382af0): connect to the room's host, send the join, wait up to 30 x 100 ms
extern "C" U3X_FN int64_t u3x_lnb_s6(void *be_, uint8_t *ss, uint64_t room) {
    LanBackend &be = lan_be(be_);
    uint32_t *eb = ext_eb();
    note_slot(be, 6);
    ++eb[71];
    const uintptr_t base = be.base;
    const int i = bind_of(be, ss);
    if (i < 0) {
        eb[72] = static_cast<uint32_t>(-1);
        return -1;
    }
    LanBind &b = be.b[i];
    u3x_sock_close(&b.cli);
    u3x_lnb_srv_stop(&b.srv);
    clear_members(base, ss);
    b.rxlen = 0;
    int32_t r = u3x_sock_connect(&b.cli, __builtin_bswap32(static_cast<uint32_t>(room >> 32)), static_cast<uint32_t>(room >> 16) & 0xffff, 0, 0);
    if (r < 0) {
        *reinterpret_cast<int32_t *>(ss + 0xbc) = r;
        ss[0xb8] = 1;
        eb[72] = static_cast<uint32_t>(r);
        return r;
    }
    u3h_lnb_call(be_, 1);
    u3h_lnb_send_join(be_, &b, ss, 0, &r);
    *reinterpret_cast<uint64_t *>(ss + 0x18) = room;
    ss[0xb8] = 0;
    *reinterpret_cast<int32_t *>(ss + 0xbc) = 0;
    for (int k = 0;; ++k) {
        if (!rd<uint64_t>(ss, 0x18)) {
            *reinterpret_cast<int32_t *>(ss + 0xbc) = -1;
            break;
        }
        if (k == 30) {
            u3x_lnb_srv_stop(&b.srv);
            eb[72] = static_cast<uint32_t>(-1);
            return -1;
        }
        usleep_us(100000);
        u3h_lnb_call(be_, 1);
        if (ss[0xb8]) {
            if (!rd<uint64_t>(ss, 0x18)) *reinterpret_cast<int32_t *>(ss + 0xbc) = -1;
            break;
        }
    }
    ss[0xb8] = 1;
    eb[72] = static_cast<uint32_t>(r);
    return r;
}
// slot 7, Leave (f_382a18)
extern "C" U3X_FN int64_t u3x_lnb_s7(void *be_, uint8_t *ss) {
    LanBackend &be = lan_be(be_);
    note_slot(be, 7);
    const int i = bind_of(be, ss);
    if (i < 0) return -1;
    if (ss == lan_match_ss(be.base)) *u3h_lnb_match_room() = 0;
    u3x_sock_close(&be.b[i].cli);
    u3x_lnb_srv_stop(&be.b[i].srv);
    *reinterpret_cast<uint64_t *>(ss + 0x18) = 0;
    clear_members(be.base, ss);
    return 0;
}
// slot 9, Kick (f_382950): 0xd1 to the room server; room[16..23] keep who asked (x009)
extern "C" U3X_FN int64_t u3x_lnb_s9(void *be_, uint8_t *ss, uint32_t id) {
    LanBackend &be = lan_be(be_);
    note_slot(be, 9);
    const int i = bind_of(be, ss);
    if (i < 0) return -1;
    {
        uint32_t *room = reinterpret_cast<uint32_t *>(u3h_ext() + 0xbc0);
        const uintptr_t *fp = static_cast<const uintptr_t *>(__builtin_frame_address(0));
        ++room[16];
        room[17] = id;
        for (int k = 0; k < 6 && fp && (reinterpret_cast<uintptr_t>(fp) & 7) == 0; ++k) {
            room[18 + k] = static_cast<uint32_t>(fp[1] - be.base);
            fp = reinterpret_cast<const uintptr_t *>(fp[0]);
        }
    }
    if (rd<uint64_t>(ss, 0x18)) {
        uint8_t o[0x10];
        *reinterpret_cast<uint32_t *>(o) = 0xd1;
        *reinterpret_cast<uint32_t *>(o + 4) = id;
        *reinterpret_cast<uint64_t *>(o + 8) = rd<uint64_t>(ss, 0x18);
        u3x_sock_send(&be.b[i].cli, o, 0x10, 1);
        ++ext_eb()[88];
    }
    return 0;
}
// slot 12, MemberData (f_3827ec): up to 0x40 bytes, stamped with our rank, sent as 0xcd
extern "C" U3X_FN int64_t u3x_lnb_s12(void *be_, uint8_t *ss, const uint8_t *data, uint32_t len) {
    LanBackend &be = lan_be(be_);
    note_slot(be, 12);
    if (len > 0x40) return -1;   // PS3 traps
    *reinterpret_cast<uint32_t *>(ss + 0x12f0) = len;
    for (uint32_t k = 0; k < len; ++k) ss[0x12f4 + k] = data[k];
    const int i = bind_of(be, ss);
    if (i < 0) return -1;
    if (rd<uint64_t>(ss, 0x18)) {
        uint8_t o[0x50];
        for (uint8_t &x : o) x = 0;
        *reinterpret_cast<uint32_t *>(o) = 0xcd;
        *reinterpret_cast<uint32_t *>(o + 4) = len;
        *reinterpret_cast<uint64_t *>(o + 8) = rd<uint64_t>(ss, 0x18);
        for (uint32_t k = 0; k < len; ++k) o[0x10 + k] = data[k];
        u3h_lnb_rank_stamp(be_, o + 0x10, len);
        u3x_sock_send(&be.b[i].cli, o, 0x50, 1);
        ++ext_eb()[89];
    }
    return 0;
}
// slot 11 with U3MP_LAN_SOLO: when the match starts (lock), a net player per session member (0x72eca0), team -1, not ready
extern "C" U3X_FN int64_t u3x_lnb_s11(void *be_, uint8_t *ss, int32_t lock) {
    LanBackend &be = lan_be(be_);
    note_slot(be, 11);
    if (!lan_solo_on()) return 0;
    const uintptr_t base = be.base;
    if (!be.state) return 0;
    const uintptr_t create = state_native(be.state, 8);
    if (!lock || !ss || !create || at<uintptr_t>(base + 0x17cb060 + 0x380)) return 0;
    for (int i = 0; i < 10; ++i) {
        uint8_t *m = ss + 0x680 + i * 0x120;
        if (!m[0x25]) continue;
        uint8_t *pl = reinterpret_cast<uint8_t *(*)(uintptr_t, uint8_t *, uint8_t *, uint32_t, uint32_t)>(create)(base + 0x17cb060, ss, m, 0, 0);
        if (pl) {
            *reinterpret_cast<int32_t *>(pl + 0x21c) = -1;
            pl[0x365] = 0;
            ++ext_eb()[93];
        }
    }
    return 0;
}
// slot 14 with U3MP_LAN_SOLO: every member connected: the host raises the start-load flag
extern "C" U3X_FN int64_t u3x_lnb_s14(void *be_, uint8_t *ss) {
    LanBackend &be = lan_be(be_);
    note_slot(be, 14);
    if (lan_solo_on() && ss && rd<uint32_t>(ss, 0x12e4) == rd<uint32_t>(ss, 0x12e8)) ss[0x1338] = 1;
    return 0;
}
extern "C" U3X_FN int64_t u3x_lnb_s_nop(void *) {
    ++ext_eb()[67];
    return 0;
}
// f_38305c: the bindings and the UDP socket closed, the vtable set
extern "C" U3X_FN void u3x_lnb_ctor(void *be_, void **vt) {
    LanBackend &be = lan_be(be_);
    for (LanBind &b : be.b) {
        u3x_lnb_srv_ctor(&b.srv);
        u3x_sock_init(&b.cli);
        b.session = nullptr;
        b.rxlen = 0;
    }
    u3x_sock_init(&be.udp);
    be.vt = vt;
}
// the vtable: slot 0 (Open, the C part's) given, the slots above, every other slot a no-op (addresses taken RIP-relative:
// the helper link has no absolute relocations)
extern "C" U3X_FN void u3x_lnb_vtable(void **vt, void *slot0) {
    void *fp;
    __asm__ volatile("leaq u3x_lnb_s_nop(%%rip),%0" : "=r"(fp));
    for (int i = 0; i < 32; ++i) vt[i] = fp;
    vt[0] = slot0;
    __asm__ volatile("leaq u3x_lnb_s1(%%rip),%0" : "=r"(fp));
    vt[1] = fp;
    __asm__ volatile("leaq u3x_lnb_s2(%%rip),%0" : "=r"(fp));
    vt[2] = fp;
    __asm__ volatile("leaq u3x_lnb_s3(%%rip),%0" : "=r"(fp));
    vt[3] = fp;
    __asm__ volatile("leaq u3x_lnb_s4(%%rip),%0" : "=r"(fp));
    vt[4] = fp;
    __asm__ volatile("leaq u3x_lnb_s5(%%rip),%0" : "=r"(fp));
    vt[5] = fp;
    __asm__ volatile("leaq u3x_lnb_s6(%%rip),%0" : "=r"(fp));
    vt[6] = fp;
    __asm__ volatile("leaq u3x_lnb_s7(%%rip),%0" : "=r"(fp));
    vt[7] = fp;
    __asm__ volatile("leaq u3x_lnb_s9(%%rip),%0" : "=r"(fp));
    vt[9] = fp;
    __asm__ volatile("leaq u3x_lnb_s11(%%rip),%0" : "=r"(fp));
    vt[11] = fp;
    __asm__ volatile("leaq u3x_lnb_s12(%%rip),%0" : "=r"(fp));
    vt[12] = fp;
    __asm__ volatile("leaq u3x_lnb_s14(%%rip),%0" : "=r"(fp));
    vt[14] = fp;
}

// ===== v7.208: fourth split, part 12 (the round results overlay) =====

extern "C" {
U3H_IMPORT int32_t u3h_pm_type(uintptr_t base, void *s);
U3H_IMPORT int u3h_pm_coop(uintptr_t base, void *s);
U3H_IMPORT void u3h_ol01_tick(uintptr_t base);
U3H_IMPORT void u3h_rc_auto_tick(uintptr_t base);
}

// b019 / b020: PS3 1.19 f_376b14 (state) and f_3768cc (draw) of the two per-screen results objects (0x17dd670 +
// 0x74 * screen: +0 dead, 4 slots of 24 bytes from +8 {on, alpha f32 +4, speed f32 +8, dim +12, page +16}, previous
// state +0x68, +0x6c), which PS4 lost, run from the menu draw (kind 77). Slot 0 is the Select-toggled scoreboard, 2 the
// RESULTS page, 3 the scoreboard of type-10 games; slot pages draw through f_26748c (0x624500 / 0x627520 and the
// children's draw slot). Float maths in x87 as the C part (this part is built without SSE). U3MP_RESULTS_SLOT0=1
// (eb[0] bit 7) only lets slot 0 switch on. ExtState eb: [180] frames, [181] state runs, [182] pages drawn, [183] draws
// refused, [184] last page, [185] Select toggles, [186] screen-1 draws, [187] dim requests, [188] held-back slots,
// [189] callee check failures, [190] slot-0 alpha bits, [191] slot-3 pages set.
namespace {
U3X_RO const uint32_t r19_pad_cmds[10] = {0xa2f8d3cdu, 0xa399fa92u, 0x2ee96703u, 0x187bb094u, 0x34c51705u, 0xee1dac26u, 0x91a1a1d3u, 0xb7c467f4u, 0xc1e54d53u, 0};
U3X_RO const uint32_t r19_fn[8] = {0x624500u, 0x627520u, 0x658050u, 0x10dda70u, 0x796640u, 0x1197380u, 0x63c430u, 0};
U3X_RO const uint32_t r19_pro[8] = {0xe5894855u, 0xe5894855u, 0x0d8d4853u, 0xe5894855u, 0xe5894855u, 0xe5894855u, 0xe5894855u, 0};
U3X_BSS uint8_t r19_ok;
U3X_BSS uint8_t r20_init[2];
U3X_BSS uint8_t r19_out[0x320];
U3X_FN uint8_t *r19_proc(uintptr_t base) { return u3x_rp_h(reinterpret_cast<const uint8_t *>(base + 0x1b3ee38)); }
U3X_FN void r19_ident(float *t) {   // f_263fd4
    for (int i = 0; i < 10; ++i) t[i] = x87_const(0);
    t[0] = t[1] = t[2] = t[3] = t[4] = t[7] = x87_const(0x3f800000u);
}
U3X_FN void r19_mul(float *a, const float *b) {   // f_2642e8
    a[0] *= b[0];
    a[1] *= b[1];
    a[2] *= b[2];
    a[3] *= b[3];
    a[4] *= b[4];
    a[5] += b[5];
    a[6] += b[6];
    a[7] *= b[7];
    a[8] += b[8];
    a[9] += b[9];
}
U3X_FN uint8_t *r19_vt(uint8_t *o, uint32_t off) {   // the object's vtable entry, when everything is a valid pointer
    uint8_t *v = ptr_ok(o) ? rd<uint8_t *>(o, 0) : nullptr;
    return ptr_ok(v) && ptr_ok(rd<void *>(v, off)) ? rd<uint8_t *>(v, off) : nullptr;
}
// f_23704c as PS4 inlines it: the children drawn, the focused one last over the screen fade
U3X_FN void r19_children(uintptr_t base, uint8_t *page, uint8_t *ctx) {
    int32_t n = rd<int32_t>(page, 0xac);
    uint8_t **a = rd<uint8_t **>(page, 0xb0);
    if (n <= 0) n = 0;
    if (n > 64 || (n && !ptr_ok(a))) {
        ++ext_eb()[183];
        return;
    }
    uint8_t *focus = nullptr;
    for (int32_t i = 0; i < n; ++i)
        if (ptr_ok(a[i]) && a[i][8]) {
            focus = a[i];
            break;
        }
    for (int32_t i = 0; i < n; ++i) {
        uint8_t *c = a[i];
        if (c == focus) continue;
        if (uint8_t *f = r19_vt(c, 0x18)) reinterpret_cast<void (*)(uint8_t *, void *, uint8_t *)>(f)(c, nullptr, ctx);
    }
    const uint32_t saved = at<uint32_t>(base + 0x16e8110);
    at<uint32_t>(base + 0x16e8110) = 0x3f800000u;
    game_call<void>(base + 0x63c430, static_cast<void *>(nullptr), static_cast<void *>(nullptr));
    if (focus)
        if (uint8_t *f = r19_vt(focus, 0x18)) reinterpret_cast<void (*)(uint8_t *, void *, uint8_t *)>(f)(focus, nullptr, ctx);
    at<uint32_t>(base + 0x16e8110) = saved;
}
// f_26748c(menu, page id, alpha, offset): the page drawn over the game (floats passed as bits)
U3X_FN void r19_page_draw(uintptr_t base, uint8_t *menu, uint32_t id, uint32_t alpha_bits, uint32_t ofs_x_bits, uint32_t ofs_y_bits) {
    uint32_t *eb = ext_eb();
    if (rd<uint32_t>(menu, 8)) {   // a menu page is up
        ++eb[183];
        return;
    }
    uint8_t *page = nullptr;
    for (int i = 0; i <= 0x9f; ++i) {
        uint8_t *p = rd<uint8_t *>(menu, 0x390 + 8 * i);
        if (!p) break;
        if (ptr_ok(p) && rd<uint32_t>(p, 0x10) == id) {
            page = p;
            break;
        }
    }
    if (!page || !r19_vt(page, 0x10) || !r19_vt(page, 0x30)) {
        ++eb[183];
        return;
    }
    const uint32_t g = menu[0xdf5];
    at<uint32_t>(base + 0x17c23a0) = g;
    *reinterpret_cast<uint32_t *>(menu + 0x2694) = g;
    alignas(16) uint8_t ctx[0x50];
    for (uint8_t &b : ctx) b = 0;
    float t1[10];
    r19_ident(t1);
    t1[4] = x87_float(alpha_bits);
    *reinterpret_cast<uint8_t **>(ctx) = menu + 0x10;
    for (int i = 0; i < 10; ++i) reinterpret_cast<float *>(ctx + 8)[i] = t1[i];
    uint32_t *box = reinterpret_cast<uint32_t *>(ctx + 0x30);
    box[0] = 0;
    box[1] = 0;
    box[2] = 0x44a00000u;
    box[3] = 0x44340000u;
    *reinterpret_cast<float *>(ctx + 0x40) = static_cast<float>(at<int32_t>(base + 0x17b4a90 + 0x114)) / x87_const(0x43960000u);
    ctx[0x44] = rd<uint32_t>(menu, 0xc) == 1;
    *reinterpret_cast<uint32_t *>(ctx + 0x48) = rd<uint32_t>(menu, 0x13fc);
    ctx[0x44] = 0;   // PS3 clears it again
    float t2[10];
    r19_ident(t2);
    t2[8] = x87_float(ofs_x_bits);
    t2[9] = x87_float(ofs_y_bits);
    r19_mul(reinterpret_cast<float *>(ctx + 8), t2);
    int32_t st = game_call<int32_t>(base + 0x658050, menu, 0, 0);
    if (st == 8) st = 5;
    else if (static_cast<uint32_t>(st - 1) > 1u) st = 0;
    {
        const uint32_t idx = 2u + (g > 0);
        uint8_t *pads = at<uint8_t *>(base + 0x16e2fa0);
        if (ptr_ok(pads)) game_call<void>(base + 0x10dda70, pads + 0x7e0 + idx * 0x1d8, idx, 2u, r19_pad_cmds, 0x24u);
    }
    reinterpret_cast<void (*)(uint8_t *, int)>(r19_vt(page, 0x30))(page, 1);
    struct {
        uint8_t *menu;
        int32_t st;
        uint32_t dt;
    } u;
    u.menu = menu;
    u.st = st;
    u.dt = at<uint32_t>(base + 0x17b4a90 + 0x11c);
    reinterpret_cast<void (*)(uint8_t *, void *)>(r19_vt(page, 0x10))(page, &u);
    for (uint8_t &b : r19_out) b = 0;
    game_call<void>(base + 0x624500, page, static_cast<uint8_t *>(ctx), static_cast<uint8_t *>(r19_out));
    alignas(16) uint8_t rect[16];
    for (int i = 0; i < 16; ++i) rect[i] = r19_out[i];
    game_call<void>(base + 0x627520, page, static_cast<void *>(nullptr), static_cast<uint8_t *>(ctx), static_cast<uint8_t *>(ctx + 8), static_cast<uint8_t *>(rect));
    r19_children(base, page, ctx);
    ++eb[182];
    eb[184] = id;
}
// f_37666c = 0x796640(obj, slot, on, sound)
U3X_FN void r19_set(uintptr_t base, uint8_t *obj, uint32_t slot, bool on, bool snd) {
    if (slot && on && (ext_eb()[0] & 0x80)) {   // U3MP_RESULTS_SLOT0=1
        ++ext_eb()[188];
        return;
    }
    game_call<void>(base + 0x796640, obj, slot, on ? 1u : 0u, snd ? 1u : 0u);
}
U3X_FN void r19_clr4(uint8_t *o) {
    o[8 + 0x48] = 0;
    o[8] = 0;
    o[8 + 0x18] = 0;
    o[8 + 0x30] = 0;
}
U3X_FN uint32_t r19_state_id(uint8_t *o) {   // f_959844: the process's state id
    if (!ptr_ok(o)) return 0;
    uint8_t *a = rd<uint8_t *>(o, 0x80);
    if (!ptr_ok(a)) return 0;
    uint8_t *b = rd<uint8_t *>(a, 0);
    if (!ptr_ok(b)) return 0;
    uint8_t *c = rd<uint8_t *>(b, 0x10);
    if (!ptr_ok(c)) return 0;
    return rd<uint32_t>(c, 0x18);
}
U3X_FN void r19_query(uintptr_t base, uint32_t msg, uint64_t *ret) {   // f_9555d0(ret, 0, msg, the game process, no args)
    uint64_t a0[3], a1[3], a2[3], a3[3];
    for (int k = 0; k < 3; ++k) a0[k] = a1[k] = a2[k] = a3[k] = 0;
    ret[0] = ret[1] = ret[2] = 0;
    game_call<void>(base + 0x1197380, static_cast<void *>(ret), static_cast<void *>(nullptr), msg, static_cast<void *>(r19_proc(base)),
                    static_cast<void *>(a0), static_cast<void *>(a1), static_cast<void *>(a2), static_cast<void *>(a3));
}
U3X_FN bool r19_dead(const uint8_t *t) { return rd<int32_t>(t, 0x3a0) < rd<int32_t>(t, 0x39c); }
U3X_FN bool r19_menu_up(const uint8_t *m) { return rd<uint32_t>(m, 8) && rd<uint32_t>(m, 0xc) == 1; }
}  // namespace
extern "C" U3X_FN int u3x_r19_callees(uintptr_t base) {
    if (r19_ok) return r19_ok == 1;
    r19_ok = 1;
    for (int i = 0; r19_fn[i]; ++i)
        if (at<uint32_t>(base + r19_fn[i]) != r19_pro[i]) {
            r19_ok = 2;
            ++ext_eb()[189];
        }
    return r19_ok == 1;
}
// f_376b14(obj): which slots show, then the slots' alpha moves toward on / off
extern "C" U3X_FN void u3x_r19_tick(uintptr_t base, void *s, uint8_t *obj, int scr) {
    uint8_t *mgr = reinterpret_cast<uint8_t *>(base + 0x17cb060);
    uint32_t *eb = ext_eb();
    const bool mp = at<uint8_t>(base + 0x17d5c61) != 0;
    const bool split = mp && (at<uint8_t>(base + 0x17d7a50 + 182) || at<int32_t>(base + 0x17f2fc0) == 3);
    if (!split && scr) return;
    ++eb[181];
    const int32_t r26 = rd<int32_t>(mgr, 0x6808);
    if (u3h_pm_type(base, s) == 12) {
        r19_clr4(obj);
        goto L078;
    }
    if (r26 != 2) goto L54;
    {
        uint8_t *trk = rd<uint8_t *>(mgr, 0x370 + 8 * scr);
        if (!ptr_ok(trk)) trk = nullptr;
        if (mgr[0x6a24]) {
            obj[0] = 0;
            uint64_t r[3];
            r19_query(base, 0x8a87d1dau, r);
            if (r[2]) {
                const uint32_t i = static_cast<uint32_t>(r[0]);
                const uint8_t *p = (mp && i <= 9) ? mgr + 0x390 + static_cast<uintptr_t>(i) * 0x9c0 : nullptr;
                if (p && r19_dead(p)) obj[0] = 1;
            }
            r19_query(base, 0xf498c2b5u, r);
            if (!r[2] || !static_cast<uint8_t>(r[0])) obj[0] = 0;
            if (r19_state_id(r19_proc(base)) == 0x1f574facu) obj[0] = 0;
        } else {
            obj[0] = (!trk || r19_dead(trk)) ? 1 : 0;
        }
        if (trk && mp && u3h_pm_coop(base, s) && rd<int32_t>(trk, 0x21c) == 1 && rd<int32_t>(trk, 0x600)) obj[0] = 0;
        if (rd<int32_t>(obj, 0x68) == 5) {
            r19_set(base, obj, 3, false, false);
            r19_set(base, obj, 0, false, false);
        }
        if (!at<uint8_t>(base + 0x17d7a50 + 100) && (!mgr[0x6a24] || !at<uint8_t>(base + 0x17c5b70))) {   // Select toggles slot 0
            uint8_t *menu = reinterpret_cast<uint8_t *>(base + (scr ? 0x17bfa10u : 0x17bd080u));
            uint8_t *pads = at<uint8_t *>(base + 0x16e2fa0);
            const uint8_t *pad = ptr_ok(pads) ? pads + 0x7e0 + static_cast<uintptr_t>(scr) * 0x1d8 : nullptr;
            if (pad && !r19_menu_up(menu) && !(rd<uint32_t>(pad, 0x9c) & 0x2u) && (rd<uint32_t>(pad, 0x94) & 0x100000u) && !at<uint64_t>(base + 0x17b4a68)) {
                *reinterpret_cast<uint32_t *>(obj + 16) = 0x40000000u;
                r19_set(base, obj, 0, !obj[8], true);
                ++eb[185];
                goto L3fc;
            }
        }
        goto L0d4;
    }
L54:
    if (r26 != 5) goto L39c;
    if (r19_state_id(r19_proc(base)) == 0x1f574facu) goto L39c;
    if (rd<int32_t>(mgr, 0x6810) != 3 && !u3h_pm_coop(base, s)) {
        r19_set(base, obj, 0, true, false);
        goto L0d4;
    }
    if (u3h_pm_type(base, s) != 10) goto L0d4;
    r19_set(base, obj, 3, true, false);
    goto L0d4;
L39c:
    {
        uint8_t *proc = r19_proc(base);
        const int32_t md = rd<int32_t>(mgr, 0x680c);
        r19_set(base, obj, 0, proc && r19_state_id(proc) == 0x9f514949u && (md == 3 || md == 8), false);
    }
L078:
    if (r26 != 3) goto L0d4;
    if (!obj[0x6c]) goto L3fc;
    obj[8] = 0;
    obj[8 + 0x18] = 0;
    obj[8 + 0x48] = 0;
    r19_set(base, obj, 2, true, false);
    goto L3d8;
L3fc:
    r19_set(base, obj, 2, false, false);
    goto L0f0;
L0d4:
    r19_set(base, obj, 2, false, false);
    if (r26 == 4) goto L110;
L0f0:
    if (r26 != 5 || !u3h_pm_coop(base, s)) goto L3d8;
L110:
    if (u3h_pm_type(base, s) == 10) {
        r19_set(base, obj, 3, true, false);
        goto L148;
    }
    r19_clr4(obj);
    goto L148;
L3d8:
    r19_set(base, obj, 3, false, false);
L148:
    if (at<uint8_t>(base + 0x17d7a50 + 84)) {
        obj[8] = 0;
        obj[8 + 0x18] = 0;
        obj[8 + 0x48] = 0;
        r19_set(base, obj, 2, true, false);
    }
    {
        uint64_t r[3];
        r19_query(base, 0xe68ae001u, r);
        if (r[2] && static_cast<uint8_t>(r[0])) r19_clr4(obj);
    }
    {   // each slot's alpha moves by 4 x dt x its speed toward on / off, clamped to 0..1
        const float f30 = *reinterpret_cast<const float *>(base + 0x17b4a90 + 0x11c) * x87_const(0x40800000u);
        for (int i = 0; i < 4; ++i) {
            uint8_t *sl = obj + 8 + 24 * i;
            float d = f30 * *reinterpret_cast<const float *>(sl + 8);
            if (!sl[0]) d = -d;
            float v = *reinterpret_cast<const float *>(sl + 4) + d;
            if (!(v >= x87_const(0))) v = x87_const(0);
            if (v - x87_const(0x3f800000u) >= x87_const(0)) v = x87_const(0x3f800000u);
            *reinterpret_cast<float *>(sl + 4) = v;
        }
    }
    *reinterpret_cast<int32_t *>(obj + 0x68) = r26;
    eb[190] = rd<uint32_t>(obj, 0xc);
}
// f_3768cc(obj): the highest visible slot's page, sliding in with its alpha (screen 0 only: PS4 draws screen 0's menus)
extern "C" U3X_FN void u3x_r19_draw(uintptr_t base, void *, uint8_t *obj, int scr) {
    if (at<uint8_t>(base + 0x17b4a48 + 0x17)) return;
    uint8_t *mgr = reinterpret_cast<uint8_t *>(base + 0x17cb060);
    for (int i = 3; i >= 0; --i) {
        uint8_t *sl = obj + 8 + 24 * i;
        const float a = *reinterpret_cast<const float *>(sl + 4);
        if (a == x87_const(0)) continue;
        if (scr) {
            ++ext_eb()[186];
            return;
        }
        const float f11 = x87_const(0x3f800000u);
        float f31 = a * a, f12 = x87_const(0x3f800000u);
        if (sl[0]) {
            const float f0 = x87_const(0x3f800000u) - a;
            f12 = x87_const(0xbf800000u);
            f31 = x87_const(0x3f800000u) - f0 * f0;
        }
        uint8_t *menu = reinterpret_cast<uint8_t *>(base + 0x17bd080u);
        r19_page_draw(base, menu, rd<uint32_t>(sl, 16), x87_bits(a * f11), x87_bits(x87_const(0)),
                      x87_bits((x87_const(0x3f800000u) - f31) * (f12 * x87_const(0x41a00000u))));
        if (!sl[12]) return;
        if (mgr[0x6cb0]) return;
        if (!menu[0xdd4]) {   // the background dims with the slot
            *reinterpret_cast<float *>(menu + 0xdcc) = f31;
            menu[0xdd4] = 1;
            ++ext_eb()[187];
        }
        return;
    }
}
// kind 77 (0x571235, the menu draw call): every frame the ol01 / rc02 ticks, then while MP is active screens 1 and 0
extern "C" U3X_FN int u3x_r19_frame(uintptr_t base, void *s) {
    ++ext_eb()[180];
    u3h_ol01_tick(base);
    u3h_rc_auto_tick(base);
    if (!u3x_r19_callees(base)) return 0;
    if (!at<uint8_t>(base + 0x17d5c61)) return 0;
    for (int i = 1; i >= 0; --i) {
        uint8_t *obj = reinterpret_cast<uint8_t *>(base + 0x17dd670u + 0x74u * static_cast<uint32_t>(i));
        if (!r20_init[i]) {   // slot 3's page, as 1.19 f_3767a0 sets it
            r20_init[i] = 1;
            if (!rd<uint32_t>(obj, 0x60)) {
                *reinterpret_cast<uint32_t *>(obj + 0x60) = 0xf650325du;
                ++ext_eb()[191];
            }
        }
        u3x_r19_tick(base, s, obj, i);
        u3x_r19_draw(base, s, obj, i);
    }
    return 0;
}

// ===== v7.209: fourth split, part 13 (the emblem records and the CPU rasterizer) =====

extern "C" {
U3H_IMPORT void *u3h_emb_fallback(void);
U3H_IMPORT void *u3h_emb_retire_queue(void);
U3H_IMPORT const uint8_t *u3h_emb_zero(void);
}

// c011 / c014: what PS3's GPU emblem pass did, done on the CPU. Layer icons are DXT1 / DXT5 DDS files
// ("<root>/build/<platform>/vtex1/ps4/<name>.dds", loaded with 0xf42fa0 from engine temp memory), sampled bilinearly
// between two mip levels (bias -1) and blended into an A8R8G8B8 target with fp16-rounded colours; decals are
// axis-aligned quads. Records (24 + the shared 4x4 transparent fallback, kept in the C part) always hold a valid texture;
// a replaced or freed texture is released 3 ticks later. sin / cos are the Cephes sinf / cosf polynomials PS3 1.19 uses
// (f_d574b4). The DC handles of the layers, scales and palette are bound as PS4's 0x66b510 binds them.
namespace {
struct EmbRetire {
    uint64_t tex, map;
    uint32_t age, pad;
};
struct EmbLod { uint32_t l0, l1, f; };
U3X_RO const char emb_build[] = "/build/", emb_vtex[] = "/vtex1/ps4/", emb_ext[] = ".dds";
U3X_RO const uint32_t emb_bind_table[18] = {0x1b67e60u, 0xc7f8567cu, 0x1b67e78u, 0xe2311588u, 0x1b67e90u, 0x03ffae77u, 0x1b67ea8u, 0,
                                            0x1b67ee0u, 0x88f9bd5cu, 0x1b67ef8u, 0x83d670b8u, 0x1b67f10u, 0x4cfe4557u, 0x1b67f28u, 0,
                                            0x1b67ec0u, 0xbcbbdfbdu};
U3X_FN EmbRec &emb_fb() { return *static_cast<EmbRec *>(u3h_emb_fallback()); }
U3X_FN EmbRetire *emb_retq() { return static_cast<EmbRetire *>(u3h_emb_retire_queue()); }
U3X_FN uint8_t *emb_allocstack(uintptr_t b) { return game_call<uint8_t *>(b + 0xda0d60); }
U3X_FN EmbRec *emb_records() {
    uint8_t *flag;
    return reinterpret_cast<EmbRec *>(u3h_emb_records(&flag));
}
U3X_FN bool f_greater(uint32_t a, uint32_t b) { return f_less(b, a); }
}  // namespace
extern "C" U3X_FN uint32_t u3x_sincos119(uint32_t x, uint32_t cosine) {
    if ((x & 0x7fffffffu) > 0x7f800000u) return 0x7fc00000u;   // NaN
    if ((x & 0x7fffffffu) == 0x7f800000u) return x;            // infinity
    if (!(x & 0x7fffffffu)) return cosine ? F_ONE : x;         // zero
    uint32_t t = f_mul(x, 0x3f22f983u);                         // x * 2/pi, rounded to the nearest quadrant
    t = f_greater(t, 0) ? f_add(t, 0x3f000000u) : f_sub(t, 0x3f000000u);
    const int32_t n = f_to_int(t);
    const uint32_t q = (static_cast<uint32_t>(n) & 3) + cosine, nf = f_from_int(n);
    uint32_t r = f_madd(nf, 0xbfc90fdau, x);   // x - n * pi/2 in two parts
    r = f_madd(nf, 0xb3a22169u, r);
    if (f_less(0xb9800000u, r) && f_less(r, 0x39800000u)) {
        if (q & 1) r = F_ONE;
    } else {
        const uint32_t r2 = f_mul(r, r);
        if (!(q & 1)) {   // sin polynomial
            const uint32_t r3 = f_mul(r, r2);
            uint32_t p = f_madd(r2, 0xb94c8c6eu, 0x3c088342u);
            p = f_madd(r2, p, 0xbe2aaaa1u);
            r = f_madd(r3, p, r);
        } else {   // cos polynomial
            uint32_t p = f_madd(r2, 0xbab24993u, 0x3d2aa036u);
            p = f_madd(r2, p, 0xbeffffdfu);
            r = f_madd(r2, p, F_ONE);
        }
    }
    return (q & 2) ? r ^ 0x80000000u : r;
}
// float -> Q32, rounded
extern "C" U3X_FN int64_t u3x_fq32(uint32_t f) {
    const int32_t e = static_cast<int32_t>((f >> 23) & 0xff);
    if (!e) return 0;
    const uint64_t m = (f & 0x7fffffu) | 0x800000u;
    int32_t sh = e - 127 - 23 + 32;
    int64_t v;
    if (sh >= 0) {
        if (sh > 38) sh = 38;
        v = static_cast<int64_t>(m << sh);
    } else if (sh < -40) {
        v = 0;
    } else {
        v = static_cast<int64_t>((m + ((1ull << (-sh)) >> 1)) >> (-sh));
    }
    return (f >> 31) ? -v : v;
}
// round(256 * log2 f), f > 0
extern "C" U3X_FN int32_t u3x_log2q8(uint32_t f) {
    const int32_t e = static_cast<int32_t>((f >> 23) & 0xff) - 127;
    uint64_t m = (f & 0x7fffffu) | 0x800000u;
    uint32_t r = 0;
    for (int i = 0; i < 9; ++i) {
        m = (m * m) >> 23;
        r <<= 1;
        if (m >= (1ull << 24)) {
            m >>= 1;
            r |= 1;
        }
    }
    return e * 256 + static_cast<int32_t>((r + 1) >> 1);
}
// fp32 -> fp16 (the H register: round to nearest even) -> value * 65536
extern "C" U3X_FN uint32_t u3x_h16q16(uint32_t f) {
    if ((f >> 31) || !(f & 0x7fffffffu)) return 0;
    const int32_t e = static_cast<int32_t>((f >> 23) & 0xff) - 127;
    if (e < -25) return 0;
    if (e > 15) return 0x7fffffffu;
    const uint32_t m = (f & 0x7fffffu) | 0x800000u;
    int32_t sh = 13;
    if (e < -14) sh += -14 - e;
    uint32_t q = m >> sh;
    const uint32_t rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
    if (rem > half || (rem == half && (q & 1))) ++q;
    const int32_t p = e - 23 + sh + 16;
    const uint64_t v = p >= 0 ? (static_cast<uint64_t>(q) << p) : (static_cast<uint64_t>(q) >> (-p));
    return v > 0x7fffffffu ? 0x7fffffffu : static_cast<uint32_t>(v);
}
// engine temp memory: PushAllocator(0x30) (0xda1590), allocate from the stack top, PopAllocator
extern "C" U3X_FN void u3x_emb_pop(uintptr_t b) {
    uint8_t *st = emb_allocstack(b);
    if (!st) return;
    uint32_t &c = *reinterpret_cast<uint32_t *>(st + 0x730);
    if (!c) return;
    uint64_t *e = reinterpret_cast<uint64_t *>(st + static_cast<uint64_t>(c - 1) * 0x28);
    e[0] = e[1] = e[2] = e[3] = e[4] = 0;
    --c;
}
extern "C" U3X_FN void u3x_emb_push(uintptr_t b) {
    uint32_t id[2];
    id[0] = 0x30u;
    id[1] = 0;
    game_call<void>(b + 0xda1590, static_cast<uint32_t *>(id), b + 0x13ac250, 0u, b + 0x13ac250);
}
extern "C" U3X_FN void *u3x_emb_alloc(uintptr_t b, uint64_t n) {
    u3x_emb_push(b);
    void *p = nullptr;
    uint8_t *st = emb_allocstack(b);
    if (st) {
        const uint32_t c = rd<uint32_t>(st, 0x730);
        if (c) {
            uintptr_t *a = rd<uintptr_t *>(st, static_cast<uint64_t>(c - 1) * 0x28);
            if (a) p = reinterpret_cast<void *(*)(uintptr_t *, uint64_t, uint64_t, uintptr_t, uint32_t, uintptr_t)>(rd<uintptr_t>(reinterpret_cast<const void *>(*a), 0x48))(a, n, 0x10, b + 0x147d6bb, 0, b + 0x147d6bb);
        }
    }
    u3x_emb_pop(b);
    return p;
}
extern "C" U3X_FN void u3x_emb_free(uintptr_t b, void *p) {
    if (p) game_call<void>(b + 0xda0dc0, p, b + 0x147d6bb, 0u, b + 0x147d6bb);
}
// the DDS file name.dds of the platform's vtex folder, loaded whole (0xf42fa0)
extern "C" U3X_FN uint8_t *u3x_emb_file(uintptr_t b, const char *name, uint64_t *size) {
    char path[0x200];
    uint32_t n = 0;
    const uint8_t *g = at<const uint8_t *>(b + 0x1b24b80);
    if (!g || !name) return nullptr;
    const char *parts[5];
    parts[0] = reinterpret_cast<const char *>(g + 0x408);
    parts[1] = emb_build;
    parts[2] = reinterpret_cast<const char *>(g + 0x479c);
    parts[3] = emb_vtex;
    parts[4] = name;
    for (const char *part : parts)
        for (const char *p = part; *p && n < 0x1f8; ++p) path[n++] = *p;
    for (int k = 0; k < 4; ++k) path[n++] = emb_ext[k];
    path[n] = 0;
    uint64_t buf[2];
    buf[0] = buf[1] = 0;
    u3x_emb_push(b);
    const uint8_t ok = static_cast<uint8_t>(game_call<uint64_t>(b + 0xf42fa0, static_cast<uint64_t *>(buf), static_cast<const char *>(path)));
    u3x_emb_pop(b);
    if (!ok || !buf[0]) return nullptr;
    *size = buf[1];
    return reinterpret_cast<uint8_t *>(buf[0]);
}
extern "C" U3X_FN uint8_t *u3x_emb_cache(uintptr_t b, const char *name) { return game_call<uint8_t *>(b + 0x1189430, b + 0x1b31128, name); }
// a DC handle (0x18: ptr, version, sid, module): cached while the module's version (+0x58) is unchanged, else looked up
extern "C" U3X_FN uint8_t *u3x_emb_h(uintptr_t b, void *s, uintptr_t ha) {
    uint8_t *h = reinterpret_cast<uint8_t *>(b + ha);
    const uint32_t sid = rd<uint32_t>(h, 0xc);
    if (!sid) return nullptr;
    uint8_t *m = rd<uint8_t *>(h, 0x10);
    const uintptr_t find = state_native(s, 9);
    if (!m) {
        if (!find) return nullptr;
        m = reinterpret_cast<uint8_t *(*)(uint32_t, int)>(find)(sid, 1);
        if (m == reinterpret_cast<uint8_t *>(b + 0x1b3af40)) m = nullptr;
        *reinterpret_cast<uint8_t **>(h + 0x10) = m;
        if (!m) return nullptr;
    } else if (rd<uint32_t>(h, 8) == rd<uint32_t>(m, 0x58)) {
        return rd<uint8_t *>(h, 0);
    }
    const uint8_t *t = rd<const uint8_t *>(m, 0x48);
    if (!t) return nullptr;
    int32_t lo = 0, hi = rd<int32_t>(t, 0x30) - 1;
    const uint8_t *ent = rd<const uint8_t *>(t, 0x38);
    while (lo <= hi) {
        const int32_t md = (lo + hi) / 2;
        const uint32_t v = rd<uint32_t>(ent, static_cast<int64_t>(md) * 0x10);
        if (v < sid) {
            lo = md + 1;
        } else if (v > sid) {
            hi = md - 1;
        } else {
            *reinterpret_cast<uint8_t **>(h) = rd<uint8_t *>(ent, static_cast<int64_t>(md) * 0x10 + 8);
            *reinterpret_cast<uint32_t *>(h + 8) = rd<uint32_t>(m, 0x58);
            return rd<uint8_t *>(h, 0);
        }
    }
    return nullptr;
}
extern "C" U3X_FN void u3x_emb_bind1(uintptr_t b, void *s, uintptr_t ha, uint32_t sid) {   // PS3 f_27d368
    uint8_t *h = reinterpret_cast<uint8_t *>(b + ha);
    const uintptr_t find = state_native(s, 9);
    uint8_t *m = find ? reinterpret_cast<uint8_t *(*)(uint32_t, int)>(find)(sid, 1) : nullptr;
    if (m == reinterpret_cast<uint8_t *>(b + 0x1b3af40)) m = nullptr;
    *reinterpret_cast<uint8_t **>(h + 0x10) = m;
    *reinterpret_cast<uint64_t *>(h) = 0;
    *reinterpret_cast<uint32_t *>(h + 8) = 0xffffffffu;
    *reinterpret_cast<uint32_t *>(h + 0xc) = sid;
    if (m && rd<uint8_t *>(m, 0x48)) u3x_emb_h(b, s, ha);
}
// layers, layer 3 = a copy of 2, scales, scale 3 = a copy of 2, palette
extern "C" U3X_FN void u3x_emb_dcbind(uintptr_t b, void *s) {
    for (int i = 0; i < 18; i += 2) {
        if (emb_bind_table[i + 1]) {
            u3x_emb_bind1(b, s, emb_bind_table[i], emb_bind_table[i + 1]);
        } else {
            for (int k = 0; k < 0x18; ++k) at<uint8_t>(b + emb_bind_table[i] + k) = at<uint8_t>(b + emb_bind_table[i - 2] + k);
        }
    }
}
extern "C" U3X_FN int u3x_emb_isrec(uint64_t p) {
    const uint64_t a = reinterpret_cast<uint64_t>(emb_records());
    return p >= a && p < a + 24 * sizeof(EmbRec) && !((p - a) % sizeof(EmbRec));
}
extern "C" U3X_FN void u3x_emb_release(uintptr_t b, uint64_t *tex, uint64_t *map) {
    const uint32_t frame = *at<uint32_t *>(b + 0x1c1f308);   // main thread: never a job worker
    if (*tex && *tex != emb_fb().tex) game_call<void>(b + 0xe73490, tex);
    if (*map) game_call<void>(b + 0xe4e8f0, b + 0x18c8960, *map, frame);
    *tex = 0;
    *map = 0;
}
extern "C" U3X_FN void u3x_emb_retire(uintptr_t b, uint64_t *tex, uint64_t *map) {
    if (*tex == emb_fb().tex && !*map) {   // the fallback is shared, never released
        *tex = 0;
        return;
    }
    if (!*tex && !*map) return;
    EmbRetire *q = emb_retq();
    for (int k = 0; k < 16; ++k)
        if (!q[k].tex && !q[k].map) {
            q[k].tex = *tex;
            q[k].map = *map;
            q[k].age = 0;
            *tex = 0;
            *map = 0;
            return;
        }
    u3x_emb_release(b, tex, map);
}
extern "C" U3X_FN void u3x_emb_retick(uintptr_t b) {
    EmbRetire *q = emb_retq();
    for (int k = 0; k < 16; ++k)
        if ((q[k].tex || q[k].map) && ++q[k].age >= 3) u3x_emb_release(b, &q[k].tex, &q[k].map);
}
// point a record at the fallback (the new value is stored before the old one is retired)
extern "C" U3X_FN void u3x_emb_rfb(uintptr_t b, void *r_) {
    EmbRec &r = *static_cast<EmbRec *>(r_);
    uint64_t ot = r.tex, om = r.map;
    r.tex = emb_fb().tex;
    r.map = 0;
    r.ok = emb_fb().tex ? 1 : 0;
    r.fmt = 0x57u;
    if (om) u3x_emb_retire(b, &ot, &om);
}
extern "C" U3X_FN void *u3x_emb_ralloc(uint32_t size) {
    EmbRec *recs = emb_records();
    for (int k = 0; k < 24; ++k) {
        if (recs[k].used) continue;
        EmbRec *r = &recs[k];
        uint8_t *p = reinterpret_cast<uint8_t *>(r);
        for (uint32_t i = 0; i < sizeof(*r); ++i) p[i] = 0;
        r->used = 1;
        r->size = size;
        r->w = size;
        r->h = size;
        r->tex = emb_fb().tex;
        r->ok = emb_fb().tex ? 1 : 0;
        r->fmt = 0x57u;
        ++ext_ec()[116];
        return r;
    }
    ext_ec()[122] = 1;
    return nullptr;
}
extern "C" U3X_FN void u3x_emb_rfree(uintptr_t b, void *r_) {
    EmbRec *r = static_cast<EmbRec *>(r_);
    if (!r || !u3x_emb_isrec(reinterpret_cast<uint64_t>(r)) || !r->used) return;
    u3x_emb_rfb(b, r);
    r->used = 0;
    r->pin = 0;
    ++ext_ec()[117];
}
// the composite becomes the record's texture (CreateDdsTextureFromMemory's path, A8R8G8B8 = ndgi 0x57); the old texture
// is retired only after the new one is in place
extern "C" U3X_FN int u3x_emb_texcreate(uintptr_t b, void *r_, const uint8_t *pix, uint32_t size) {
    EmbRec &r = *static_cast<EmbRec *>(r_);
    int32_t ctx = 0xe;
    uint64_t desc[2], sub[2], second = 0;
    desc[0] = static_cast<uint64_t>(size) | (static_cast<uint64_t>(size) << 16) | (1ull << 32) | (1ull << 40) | (0x57ull << 48);
    desc[1] = 0x800010001ull;
    sub[0] = reinterpret_cast<uint64_t>(pix);
    sub[1] = static_cast<uint64_t>(size * 4) | (static_cast<uint64_t>(size * size * 4) << 32);
    uint64_t map = game_call<uint64_t>(b + 0xe76cb0, &ctx, static_cast<uint64_t *>(desc), static_cast<uint64_t *>(sub), &second);
    if (!map) {
        ext_ec()[122] = 2;
        return 0;
    }
    const uint64_t tex = game_call<uint64_t>(b + 0xe77c50, map, 0x57u);
    if (!tex) {
        uint64_t z = 0;
        u3x_emb_release(b, &z, &map);
        ext_ec()[122] = 3;
        return 0;
    }
    uint64_t ot = r.tex, om = r.map;
    r.tex = tex;
    r.map = map;
    r.w = size;
    r.h = size;
    r.fmt = 0x57u;
    r.ok = 1;
    ++ext_ec()[115];
    if (om) u3x_emb_retire(b, &ot, &om);
    return 1;
}
// the shared fallback (4x4, transparent), made once the graphics device exists; published at ec[158..159]
extern "C" U3X_FN void u3x_emb_fb(uintptr_t b) {
    EmbRec &fb = emb_fb();
    if (fb.tex || !at<uint64_t>(b + 0x1cf5330)) return;
    EmbRec t;
    uint8_t *p = reinterpret_cast<uint8_t *>(&t);
    for (uint32_t i = 0; i < sizeof(t); ++i) p[i] = 0;
    if (!u3x_emb_texcreate(b, &t, u3h_emb_zero(), 4)) return;
    fb = t;
    fb.map = 0;   // never released
    ext_ec()[158] = static_cast<uint32_t>(t.tex);
    ext_ec()[159] = static_cast<uint32_t>(t.tex >> 32);
}
// a DXT1 / DXT5 DDS (as PS3's loader accepts): size, block size, mip offsets
extern "C" U3X_FN int u3x_emb_dds(uintptr_t b, const char *name, void *t_) {
    EmbDds &t = *static_cast<EmbDds *>(t_);
    uint64_t n = 0;
    t.file = u3x_emb_file(b, name, &n);
    if (!t.file) {
        ++ext_ec()[123];
        return 0;
    }
    const uint8_t *d = t.file;
    const uint32_t four = rd<uint32_t>(d, 84);
    if (n < 128 || rd<uint32_t>(d, 0) != 0x20534444u || (four != 0x31545844u && four != 0x35545844u)) {
        u3x_emb_free(b, t.file);
        t.file = nullptr;
        ++ext_ec()[123];
        return 0;
    }
    t.h = rd<uint32_t>(d, 12);
    t.w = rd<uint32_t>(d, 16);
    const uint32_t mips = rd<uint32_t>(d, 28);
    uint32_t nl = mips ? mips : 1;
    if (nl > 16) nl = 16;
    t.bs = four == 0x31545844u ? 8 : 16;
    uint64_t o = 128;
    for (uint32_t l = 0; l < nl; ++l) {
        uint32_t w = t.w >> l, h = t.h >> l;
        if (!w) w = 1;
        if (!h) h = 1;
        t.off[l] = static_cast<uint32_t>(o);
        o += static_cast<uint64_t>((w + 3) / 4) * ((h + 3) / 4) * t.bs;
    }
    if (o > n || !t.w || !t.h) {
        u3x_emb_free(b, t.file);
        t.file = nullptr;
        ++ext_ec()[123];
        return 0;
    }
    t.maxl = mips < nl - 1 ? mips : nl - 1;
    return 1;
}
// the R channel (5 bits widened to 8) of texel (x, y) of level l, clamped to the edge
extern "C" U3X_FN uint32_t u3x_emb_dxt(const void *t_, uint32_t l, int32_t x, int32_t y) {
    const EmbDds &t = *static_cast<const EmbDds *>(t_);
    int32_t w = static_cast<int32_t>(t.w >> l), h = static_cast<int32_t>(t.h >> l);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (x < 0) x = 0;
    else if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    else if (y >= h) y = h - 1;
    const uint8_t *k = t.file + t.off[l] + (static_cast<uint32_t>(y >> 2) * static_cast<uint32_t>((w + 3) >> 2) + static_cast<uint32_t>(x >> 2)) * t.bs + (t.bs == 16 ? 8 : 0);
    const uint32_t c0 = k[0] | static_cast<uint32_t>(k[1]) << 8, c1 = k[2] | static_cast<uint32_t>(k[3]) << 8;
    const uint32_t ix = ((k[4] | static_cast<uint32_t>(k[5]) << 8 | static_cast<uint32_t>(k[6]) << 16 | static_cast<uint32_t>(k[7]) << 24) >> (((y & 3) * 4 + (x & 3)) * 2)) & 3;
    uint32_t r0 = c0 >> 11, r1 = c1 >> 11;
    r0 = (r0 << 3) | (r0 >> 2);
    r1 = (r1 << 3) | (r1 >> 2);
    if (ix == 0) return r0;
    if (ix == 1) return r1;
    if (c0 > c1 || t.bs == 16) return ix == 2 ? (2 * r0 + r1) / 3 : (r0 + 2 * r1) / 3;
    return ix == 2 ? (r0 + r1) / 2 : 0;
}
// the two mip levels and their blend for rho2 = (texels per pixel)^2, bias -1
extern "C" U3X_FN void u3x_emb_lod(const void *t_, uint32_t rho2, void *L_) {
    const EmbDds &t = *static_cast<const EmbDds *>(t_);
    EmbLod &L = *static_cast<EmbLod *>(L_);
    L.l0 = L.l1 = L.f = 0;
    if (!(rho2 & 0x7fffffffu) || (rho2 >> 31)) return;
    const int32_t lam = ((u3x_log2q8(rho2) + 1) >> 1) - 256;
    if (lam <= 0) return;
    uint32_t d = static_cast<uint32_t>(lam);
    const uint32_t mx = t.maxl << 8;
    if (d > mx) d = mx;
    L.l0 = d >> 8;
    L.f = d & 255;
    L.l1 = L.l0 + 1;
    if (L.l1 > t.maxl) {
        L.l1 = t.maxl;
        L.f = 0;
    }
}
// bilinear R * 65536 at (U, V) in Q32
extern "C" U3X_FN uint32_t u3x_emb_bil(const void *t_, uint32_t l, int64_t U, int64_t V) {
    const EmbDds &t = *static_cast<const EmbDds *>(t_);
    int64_t w = static_cast<int64_t>(t.w >> l), h = static_cast<int64_t>(t.h >> l);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    const int64_t tu = U * w - (1ll << 31), tv = V * h - (1ll << 31);
    const int32_t x = static_cast<int32_t>(tu >> 32), y = static_cast<int32_t>(tv >> 32);
    const uint32_t fx = static_cast<uint32_t>(tu >> 24) & 255, fy = static_cast<uint32_t>(tv >> 24) & 255;
    const uint32_t a = u3x_emb_dxt(t_, l, x, y), c = u3x_emb_dxt(t_, l, x + 1, y), d = u3x_emb_dxt(t_, l, x, y + 1), e = u3x_emb_dxt(t_, l, x + 1, y + 1);
    return (a * (256 - fx) + c * fx) * (256 - fy) + (d * (256 - fx) + e * fx) * fy;
}
// the filtered R as Q32 in [0, 1]
extern "C" U3X_FN uint64_t u3x_emb_a32(const void *t, const void *L_, int64_t U, int64_t V) {
    const EmbLod &L = *static_cast<const EmbLod *>(L_);
    const uint64_t r0 = u3x_emb_bil(t, L.l0, U, V);
    const uint64_t r = L.f ? r0 * (256 - L.f) + static_cast<uint64_t>(u3x_emb_bil(t, L.l1, U, V)) * L.f : r0 * 256;
    return (r << 8) / 255;
}
// pixel B, G, R, A over by colour c16 (b, g, r * 65536) with coverage a32
extern "C" U3X_FN void u3x_emb_blend(uint8_t *px, const uint32_t *c16, uint64_t a32) {
    if (a32 > (1ull << 32)) a32 = 1ull << 32;
    const uint64_t ia = (1ull << 32) - a32;
    for (int k = 0; k < 3; ++k) {
        const uint64_t v = (255ull * c16[k] * a32 + static_cast<uint64_t>(px[k]) * 65536ull * ia + (1ull << 47)) >> 48;
        px[k] = static_cast<uint8_t>(v > 255 ? 255 : v);
    }
    const uint64_t v = (255ull * 65536ull * a32 + static_cast<uint64_t>(px[3]) * 65536ull * ia + (1ull << 47)) >> 48;
    px[3] = static_cast<uint8_t>(v > 255 ? 255 : v);
}
// (du tw / size)^2 + (dv th / size)^2
extern "C" U3X_FN uint32_t u3x_emb_rho2(uint32_t du, uint32_t dv, uint32_t tw, uint32_t th, uint32_t size) {
    const uint32_t fs = f_from_int(static_cast<int32_t>(size));
    const uint32_t a = f_div(f_mul(du, f_from_int(static_cast<int32_t>(tw))), fs), c = f_div(f_mul(dv, f_from_int(static_cast<int32_t>(th))), fs);
    return f_add(f_mul(a, a), f_mul(c, c));
}
extern "C" U3X_FN uint32_t u3x_emb_fmax(uint32_t a, uint32_t b) { return f_less(a, b) ? b : a; }
// one layer quad over the target (scissor 1..size-2), optionally through a mask over pixel / size
extern "C" U3X_FN void u3x_emb_quad(uint8_t *pix, uint32_t size, const void *t_, const uint32_t *uv, const void *mk_, const uint32_t *c16) {
    const EmbDds &t = *static_cast<const EmbDds *>(t_);
    const EmbDds *mk = static_cast<const EmbDds *>(mk_);
    const int64_t U1 = u3x_fq32(uv[2]), V1 = u3x_fq32(uv[3]);
    const int64_t DUX = u3x_fq32(uv[4]) - U1, DVX = u3x_fq32(uv[5]) - V1, DUY = u3x_fq32(uv[0]) - U1, DVY = u3x_fq32(uv[1]) - V1;
    EmbLod L, Lm;
    const uint32_t rx = u3x_emb_rho2(f_sub(uv[4], uv[2]), f_sub(uv[5], uv[3]), t.w, t.h, size);
    const uint32_t ry = u3x_emb_rho2(f_sub(uv[0], uv[2]), f_sub(uv[1], uv[3]), t.w, t.h, size);
    u3x_emb_lod(t_, u3x_emb_fmax(rx, ry), &L);
    if (mk) {
        const uint32_t k = f_div(f_from_int(static_cast<int32_t>(mk->w)), f_from_int(static_cast<int32_t>(size)));
        u3x_emb_lod(mk, f_mul(k, k), &Lm);
    }
    const uint32_t sh = size == 256 ? 9 : 8;
    for (uint32_t py = 1; py + 1 < size; ++py)
        for (uint32_t px = 1; px + 1 < size; ++px) {
            const int64_t ox = 2 * static_cast<int64_t>(px) + 1, oy = 2 * static_cast<int64_t>(py) + 1;
            const int64_t u = U1 + ((DUX * ox + DUY * oy) >> sh), v = V1 + ((DVX * ox + DVY * oy) >> sh);
            uint64_t a = u3x_emb_a32(t_, &L, u, v);
            if (mk) {
                const uint64_t m = u3x_emb_a32(mk, &Lm, ox << (32 - sh), oy << (32 - sh));
                a = static_cast<uint64_t>((static_cast<unsigned __int128>(a) * m) >> 32);
            }
            if (a) u3x_emb_blend(pix + (static_cast<uintptr_t>(py) * size + px) * 4, c16, a);
        }
}
// one decal quad, axis-aligned in pixels; q: x lo / hi, y lo / hi (normalized), u lo / hi, v top / bottom
extern "C" U3X_FN void u3x_emb_decal(uint8_t *pix, uint32_t size, const void *t_, const uint32_t *q, const uint32_t *c16) {
    const EmbDds &t = *static_cast<const EmbDds *>(t_);
    const uint32_t fs = f_from_int(static_cast<int32_t>(size));
    const uint32_t x0 = f_mul(q[0], fs), x1 = f_mul(q[1], fs), y0 = f_mul(q[2], fs), y1 = f_mul(q[3], fs);
    const uint32_t dx = f_sub(x1, x0), dy = f_sub(y1, y0);
    if (!f_greater(dx, 0) || !f_greater(dy, 0)) return;
    const uint32_t du = f_sub(q[5], q[4]), dv = f_sub(q[7], q[6]);
    const uint32_t rxf = f_div(f_mul(du, f_from_int(static_cast<int32_t>(t.w))), dx), ryf = f_div(f_mul(dv, f_from_int(static_cast<int32_t>(t.h))), dy);
    EmbLod L;
    u3x_emb_lod(t_, u3x_emb_fmax(f_mul(rxf, rxf), f_mul(ryf, ryf)), &L);
    const int64_t X0 = u3x_fq32(x0), X1 = u3x_fq32(x1), Y0 = u3x_fq32(y0), Y1 = u3x_fq32(y1), U0 = u3x_fq32(q[4]), V0 = u3x_fq32(q[6]);
    const int64_t DUDX = u3x_fq32(f_div(du, dx)), DVDY = u3x_fq32(f_div(dv, dy));
    for (uint32_t py = 1; py + 1 < size; ++py) {
        const int64_t cy = static_cast<int64_t>(2 * py + 1) << 31;
        if (cy < Y0 || cy >= Y1) continue;
        const int64_t v = V0 + ((DVDY * ((cy - Y0) >> 16)) >> 16);
        for (uint32_t px = 1; px + 1 < size; ++px) {
            const int64_t cx = static_cast<int64_t>(2 * px + 1) << 31;
            if (cx < X0 || cx >= X1) continue;
            const int64_t u = U0 + ((DUDX * ((cx - X0) >> 16)) >> 16);
            const uint64_t a = u3x_emb_a32(t_, &L, u, v);
            if (a) u3x_emb_blend(pix + (static_cast<uintptr_t>(py) * size + px) * 4, c16, a);
        }
    }
}

// ===== v7.210: fourth split, part 14 (the post-match handler) =====

extern "C" {
U3H_IMPORT void u3h_n13_rearm(uintptr_t base, void *s);
}

// a013 / a019 / a014 / n013: PS3 1.19 f_29a068, the post-match handler (kind 67), replacing PS4's 1.10 copy 0x678b00:
// the in-match flag and the profile publish, the menu reset and the "multi-menu" level, the leave routing (the page stack
// and the party messages), then per local user the profile counters of the match (co-op, unranked or competitive: kills,
// scores, wins / ties / losses with 1.19's weekly fields, map and game-type challenge stats), the publish and the
// leaderboard submits, and the return pages (the lobby, matchmaking, or the spoofed Find Game re-arm). The challenge table
// is DC 0xb630d01c (1.19 medals.bin). ExtState ea: [57] challenge calls (low) / updates (high), [59] runs, [60] submits,
// [61] done, [90..94] the n013 return routing.
namespace {
U3X_RO const char pm_title1[] = "BCET70043", pm_title2[] = "NPUA70180", pm_title3[] = "NPHA80158";
U3X_FN uint8_t *pm_rec(uintptr_t base, void *s, uint32_t u) { return u3h_lg_rec(base, s, u3h_lg_profile(base, u)); }
U3X_FN void pm_add(uintptr_t base, void *s, uint32_t u, uint32_t off, int32_t v) { *reinterpret_cast<int32_t *>(pm_rec(base, s, u) + off) += v; }
U3X_FN uint32_t &n13_flag() { return ext_ea()[94]; }
U3X_FN bool pm_title_differs(uintptr_t scmp, const void *tid, const char *title) { return reinterpret_cast<int (*)(const void *, const char *)>(scmp)(tid, title) != 0; }
}  // namespace
extern "C" U3X_FN uint8_t *u3x_pm_ngentry(uintptr_t base, void *s) {
    const int16_t i = at<int16_t>(base + 0x17cb060 + 0x661c);
    uint8_t *t = dc_table(s, 0x3b9a067du);
    if (!t || i < 0 || i >= rd<int32_t>(t, 0)) return nullptr;
    uint8_t *e = rd<uint8_t *>(t, 8);
    return e ? e + static_cast<uintptr_t>(i) * 0xd0 : nullptr;
}
extern "C" U3X_FN int32_t u3x_pm_type(uintptr_t base, void *s) {   // f_30e004
    const uint8_t *e = u3x_pm_ngentry(base, s);
    return e ? rd<int32_t>(e, 0xc) : 0;
}
extern "C" U3X_FN int u3x_pm_coop(uintptr_t base, void *s) {   // f_310c68: types 4 8 9 11 13 16 17 18
    const uint8_t *e = u3x_pm_ngentry(base, s);
    if (!e) return 0;
    const uint32_t t = rd<uint32_t>(e, 0xc) - 4;
    return t < 0xf && ((0x72b1u >> t) & 1);
}
// f_315240 (PS4 0x7270f0 without the 1.10 abort-flag wrapper)
extern "C" U3X_FN int u3x_pm_ranked(uintptr_t base, void *s) {
    const bool b = u3x_pm_coop(base, s) && u3x_pm_type(base, s) != 0x12;
    const uintptr_t scmp = base + 0x139c710;
    const void *tid = reinterpret_cast<const void *>(base + 0x17b9ca0);
    const uint32_t w = at<uint32_t>(base + 0x17b9d10);
    if (!b && at<int32_t>(base + 0x17d603c) != 1 && !at<uint8_t>(base + 0x17d7b27) && pm_title_differs(scmp, tid, pm_title1) && pm_title_differs(scmp, tid, pm_title2) && pm_title_differs(scmp, tid, pm_title3) &&
        w != 0xc0e2aff8u && w != 0x2c3d7d0fu)
        return 0;
    if (at<uint8_t>(base + 0x17cb060 + 0x6a24)) return 0;
    if (at<uint8_t>(base + 0x17d5c60)) return at<uint8_t>(base + 0x17d6065) != 0;
    return 1;
}
extern "C" U3X_FN int u3x_pm_leader_of(uintptr_t base, uintptr_t obj) {   // f_37f1f8
    return !at<uint64_t>(base + obj + 0x18) || at<uint32_t>(base + obj + 0x12e4) == at<uint32_t>(base + obj + 0x12e8);
}
// the party backend's slot off (obj, party[, arg])
extern "C" U3X_FN void u3x_pm_party_vcall(uintptr_t base, uint32_t off, int arg, int three) {
    uint8_t *pa = reinterpret_cast<uint8_t *>(base + 0x17d6420);
    uint8_t *o = rd<uint8_t *>(pa, 8);
    if (!o) return;
    const uintptr_t fn = rd<uintptr_t *>(o, 0)[off / 8];
    if (!fn) return;
    if (three) reinterpret_cast<void (*)(void *, void *, int)>(fn)(o, pa, arg);
    else reinterpret_cast<void (*)(void *, void *)>(fn)(o, pa);
}
extern "C" U3X_FN void u3x_pm_pg_clear(uintptr_t base) { at<int32_t>(base + 0x17bd080 + 0xef8) = 0; }
extern "C" U3X_FN void u3x_pm_pg_push(uintptr_t base, uint32_t id) {   // f_264820 as PS4 inlines it
    uint8_t *m = reinterpret_cast<uint8_t *>(base + 0x17bd080);
    int32_t n = rd<int32_t>(m, 0xef8);
    if (n > 0x9f || rd<uint32_t>(m, 0xefc + static_cast<uintptr_t>(n) * 4) == id) return;
    ++n;
    *reinterpret_cast<int32_t *>(m + 0xef8) = n;
    *reinterpret_cast<uint32_t *>(m + 0xefc + static_cast<uintptr_t>(n) * 4) = id;
}
extern "C" U3X_FN void u3x_pm_menu_reset(uintptr_t base, uintptr_t menu) {   // f_26e978 as PS4 inlines it
    uint8_t *m = reinterpret_cast<uint8_t *>(base + menu);
    const uint32_t st = rd<uint32_t>(m, 8);
    if (st <= 6 && ((0x61u >> st) & 1)) return;
    uint8_t buf[0x80];
    game_call<void>(base + 0xf23970, static_cast<void *>(buf), 0, 0);
    game_call<void>(base + 0x6534e0, static_cast<void *>(m), static_cast<void *>(buf), 3);
    *reinterpret_cast<uint32_t *>(m + 8) = 5;
    *reinterpret_cast<uint32_t *>(m + 0xdb4) = 0;
    *reinterpret_cast<uint32_t *>(m + 0xdb8) = 0x3f800000u;
    game_call<void>(base + 0x6534e0, static_cast<void *>(m), static_cast<void *>(buf), 0);
}
extern "C" U3X_FN uint32_t u3x_pm_map(uintptr_t base, void *s) {   // f_304214
    if (at<uint8_t>(base + 0x17bb670)) return 0xb70ed28u;
    return rd<uint32_t>(pm_rec(base, s, 0), 0x52c);
}
extern "C" U3X_FN int u3x_pm_map_is(uintptr_t base, void *s, uint32_t id) {
    const uint32_t *e = game_call<uint32_t *>(base + 0x73f7d0, reinterpret_cast<void *>(base + 0x17cb060), u3x_pm_map(base, s));
    return e && *e == id;
}
// a019: challenge stat id (0x755..0x795) of user u += v, or |= the net game's bit
extern "C" U3X_FN void u3x_pm_challenge(uintptr_t base, void *s, uint32_t u, uint32_t id, int32_t v, int bit) {
    uint32_t *ea = ext_ea();
    ++ea[57];
    uint8_t *p = u3h_lg_profile(base, u);
    if (!p || p[0x7afc]) return;
    if (id < 0x755 || id > 0x795) return;   // PS3 traps
    if (at<int32_t>(base + 0x17d5c58 + 0x40) == 0 && !u3x_pm_coop(base, s)) return;
    if (at<uint8_t>(base + 0x17cb060 + 0x6a24)) return;
    const uint8_t *t = dc_table(s, 0xb630d01cu);
    if (!t) return;
    const uint8_t *hit = u3x_challenge_entry(t, static_cast<int32_t>(id - 0x755u));
    if (!hit || !rd<uint32_t>(hit, 0x20)) return;   // the entry's DLC mask (every DLC is on)
    uint32_t *c = reinterpret_cast<uint32_t *>(u3h_lg_rec(base, s, p) + static_cast<uintptr_t>(id) * 4 + 8);
    if (bit) {
        const uint8_t *ng = u3x_pm_ngentry(base, s);
        if (!ng) return;
        const uint32_t sh = rd<uint32_t>(ng, 0xb8) & 0x3f;
        *c |= sh < 32 ? 1u << sh : 0u;
    } else {
        *c += static_cast<uint32_t>(v);
    }
    ea[57] += 0x10000u;
}
// f_365c5c: the game type's win challenge (and Plunder's 5-0)
extern "C" U3X_FN void u3x_pm_365c5c(uintptr_t base, void *s, uint32_t u, int32_t mine, int32_t theirs) {
    const int32_t t = u3x_pm_type(base, s);
    uint32_t id;
    if (t == 2) {
        if (mine == 5 && theirs == 0) u3x_pm_challenge(base, s, u, 0x776, 1, 0);
        id = 0x768;
    } else if (t == 0) {
        id = 0x76f;
    } else if (t == 1) {
        id = 0x77c;
    } else if (t == 0xf) {
        id = 0x780;
    } else if (t == 10) {
        id = 0x777;
    } else {
        if (t == 0x13) pm_add(base, s, u, 0x1d78, 1);
        return;
    }
    u3x_pm_challenge(base, s, u, id, 1, 0);
}
// f_365da8: co-op map counters and the wave challenges
extern "C" U3X_FN void u3x_pm_365da8(uintptr_t base, void *s, uint32_t u, uint32_t map, int32_t a3) {
    if (map < 0x20) pm_add(base, s, u, (map + 0x58a) * 4 + 8, 1);
    else if (map - 0x20 < 0x20) pm_add(base, s, u, (map + 0x715) * 4 + 8, 1);
    const uint32_t x = at<uint32_t>(base + 0x17cb060 + 0x6a48);
    const int32_t t = u3x_pm_type(base, s);
    if (!a3) {
        if (t == 0xb) {
            if (x > 1) u3x_pm_challenge(base, s, u, 0x75d, 1, 0);
            if (x > 2) u3x_pm_challenge(base, s, u, 0x769, 1, 0);
            if (x >= 4) u3x_pm_challenge(base, s, u, 0x773, 1, 0);
        } else if (t == 0x11) {
            if (x > 3) u3x_pm_challenge(base, s, u, 0x77b, 1, 0);
            if (x >= 3) u3x_pm_challenge(base, s, u, 0x76a, 1, 0);
        } else if (t == 8) {
            if (x >= 2) u3x_pm_challenge(base, s, u, 0x75e, 1, 0);
        }
    }
    if (x != 4) return;
    if (t == 0xb) u3x_pm_challenge(base, s, u, 0x77f, 1, 1);
    else if (t == 0x11) u3x_pm_challenge(base, s, u, 0x771, 1, 1);
}
// f_324f9c + f_325154 per score entry + f_325094: the weapon stats of the match into 0x17d3630
extern "C" U3X_FN void u3x_pm_weapon_stats(uintptr_t base) {
    uint8_t *obj = reinterpret_cast<uint8_t *>(base + 0x17d3630);
    const uint8_t *sm = reinterpret_cast<const uint8_t *>(base + 0x17dd778);
    *reinterpret_cast<uint32_t *>(obj + 0x24a0) = 0;
    const uint32_t n = rd<uint32_t>(sm, 0xe8);
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t *e = sm + 0xec + static_cast<uintptr_t>(i) * 0x28;
        if (!rd<uint16_t>(e, 2) || !rd<uint16_t>(e, 4)) continue;
        const uint32_t b = e[0xc];
        const int32_t *mp = game_call<int32_t *>(base + 0x11f2e30, 0x9e354807u);
        if (b >= 0x13 && (!mp || b - 0x13 >= static_cast<uint32_t>(mp[0]))) continue;   // PS4 asserts here
        const uint32_t id = b < 0x13 ? at<uint32_t>(base + 0x13a6170 + b * 4) : reinterpret_cast<const uint32_t *>(*reinterpret_cast<const uintptr_t *>(mp + 2))[b - 0x13];
        game_call<void>(base + 0x748ef0, static_cast<void *>(obj), id);
    }
    for (uint32_t i = 0; i < rd<uint32_t>(obj, 0x24a0); ++i) {
        uint8_t *o = obj + static_cast<uintptr_t>(i) * 0xa0;
        *reinterpret_cast<uint64_t *>(o + 0x1118) = 0x2d6e6f706165775full;   // "weapon-stats"
        *reinterpret_cast<uint16_t *>(o + 0x1124) = 0x73;
        *reinterpret_cast<uint32_t *>(o + 0x1120) = 0x74617473u;
        *reinterpret_cast<uint32_t *>(o + 0x112c) = static_cast<uint32_t>(obj[0x24a4 + i * 4]) + 100;
        *reinterpret_cast<uint64_t *>(o + 0x1130) = rd<uint64_t>(obj, 0x2528 + static_cast<uintptr_t>(i) * 8);
    }
}
// 1.19 0x29b954..0x29be48: the leaderboard blob and submits (co-op: 0x517 / 0x515; ranked: 0x514 / 0x515 / 0x516)
extern "C" U3X_FN void u3x_pm_lb_submits(uintptr_t base, void *s, uint32_t u) {
    uint8_t blob[0x40];
    for (uint8_t &b : blob) b = 0;
    uint8_t *p = u3h_lg_profile(base, u);
    const uint8_t *r = pm_rec(base, s, u);
    *reinterpret_cast<uint32_t *>(blob) = game_call<uint32_t>(base + 0x781ea0, static_cast<void *>(p));
    *reinterpret_cast<uint32_t *>(blob + 8) = rd<uint32_t>(r, 0x1c58);
    *reinterpret_cast<uint32_t *>(blob + 0xc) = rd<uint32_t>(r, 0x1f24);
    *reinterpret_cast<uint32_t *>(blob + 0x10) = rd<uint32_t>(r, 0x1eac);
    *reinterpret_cast<int32_t *>(blob + 4) = rd<int32_t>(r, 0x1f30) + rd<int32_t>(r, 0x1f28);
    if (!u3x_lb_off()) {
        if (u3x_pm_coop(base, s)) {
            u3x_lb_submit(base, s, 0x517, rd<int32_t>(r, 0x1f2c), blob);
            u3x_lb_submit(base, s, 0x515, rd<int32_t>(r, 0x1f38), blob);
        } else if (u3x_pm_ranked(base, s)) {
            u3x_lb_submit(base, s, 0x514, rd<int32_t>(r, 0x1c54), blob);
            u3x_lb_submit(base, s, 0x515, rd<int32_t>(r, 0x1f38), blob);
            u3x_lb_submit(base, s, 0x516, rd<int32_t>(r, 0x1f34), blob);
        }
    }
    ++ext_ea()[60];
}
extern "C" U3X_FN int u3x_n13_on(void) { return !(ext_ec()[3] & 0x100u); }
// one local user's counters (0x29a3fc..0x29be64)
namespace {
U3X_FN void pm_user(uintptr_t base, void *s, uint32_t u) {
    uint8_t *net = reinterpret_cast<uint8_t *>(base + 0x17d5c58), *ngm = reinterpret_cast<uint8_t *>(base + 0x17cb060), *party = reinterpret_cast<uint8_t *>(base + 0x17d6420);
    const uint8_t flag = at<uint8_t>(base + 0x16e7170);
    const uint8_t *tr = rd<const uint8_t *>(ngm, 0x370 + static_cast<uintptr_t>(u) * 8);
    if (!tr) return;
    const uint8_t *sm = reinterpret_cast<const uint8_t *>(base + 0x17dd778);
    const uint16_t *st = reinterpret_cast<const uint16_t *>(base + 0x1b7f114);
    const uint8_t *tm = reinterpret_cast<const uint8_t *>(base + 0x1b7f070);
    if (rd<uint32_t>(sm, 8)) {   // this player's score and member records (by network id)
        const uint32_t lo = rd<uint32_t>(tr, 0x1e4) & 0xfff;
        uint32_t k = lo + 0x800;
        if (!(lo && tr[0x1ec])) k = lo;
        k += rd<uint32_t>(tr, 0x1e8) << 12;
        bool found = false;
        for (uint32_t i = 0; i < rd<uint32_t>(sm, 8); ++i)
            if (rd<uint32_t>(sm, 0x50ec + static_cast<uintptr_t>(i) * 0xa4) == k) {
                st = reinterpret_cast<const uint16_t *>(sm + 0x60d4 + static_cast<uintptr_t>(i) * 0x130);
                found = true;
                break;
            }
        if (!found) st = reinterpret_cast<const uint16_t *>(base + 0x1b7f114);
        for (uint32_t i = 0; i < rd<uint32_t>(sm, 8); ++i)
            if (rd<uint32_t>(sm, 0x50ec + static_cast<uintptr_t>(i) * 0xa4) == k) {
                tm = sm + 0x50ec + static_cast<uintptr_t>(i) * 0xa4;
                break;
            }
    }
    const uint8_t team = tm[7];
    const uint8_t *T = reinterpret_cast<const uint8_t *>(base + 0x17e338c);   // the 3 team blocks
    int16_t sc[3];
    for (int i = 0; i < 3; ++i) sc[i] = rd<int16_t>(T, i * 0x130 + 2);
    int32_t cnt[4];
    for (int32_t &c : cnt) c = 0;
    for (int i = 0; i < 10; ++i) {
        const uint8_t *pl = net[9] ? ngm + 0x390 + static_cast<uintptr_t>(i) * 0x9c0 : nullptr;
        if (pl && pl[0xe1]) {
            const uint32_t k = rd<uint32_t>(pl, 0x21c);
            if (k < 4) ++cnt[k];
        }
    }
    for (int i = 0; i < 2; ++i) {
        const int32_t r = game_call<int32_t>(base + 0x688160, reinterpret_cast<void *>(base + 0x17c3b40), static_cast<uint64_t>(u), i, -1);
        pm_add(base, s, u, static_cast<uint32_t>(r + 0x4fd) * 4 + 8, 1);
    }
    {
        const int32_t r = game_call<int32_t>(base + 0x74e6d0, reinterpret_cast<void *>(base + 0x17d79f0 + static_cast<uintptr_t>(u) * 0xc), -1, u3x_pm_coop(base, s));
        pm_add(base, s, u, static_cast<uint32_t>(r + 0x53d) * 4 + 8, 1);
    }
    bool publish = false;
    const int32_t st24 = *reinterpret_cast<const int32_t *>(reinterpret_cast<const uint8_t *>(st) + 0x24);
    if (u3x_pm_coop(base, s) && u3x_pm_type(base, s) != 0x12) {   // co-op
        pm_add(base, s, u, 0x5bc, st24);
        pm_add(base, s, u, 0x1f2c, st24);
        pm_add(base, s, u, 0x5c0, rd<int32_t>(tm, 0x14));
        pm_add(base, s, u, 0x1f28, rd<int32_t>(tm, 0x14));
        at<uint32_t>(base + 0x1b68a18 + u * 4) = 1;
        if (!party[0x1339]) {
            pm_add(base, s, u, 0x15fc, 1);
            pm_add(base, s, u, 0x65c, st[5]);
            if (rd<int32_t>(ngm, 0x680c) == 4) {
                pm_add(base, s, u, 0x1600, 1);
                if (const uint8_t *e = u3x_pm_ngentry(base, s)) u3x_pm_365da8(base, s, u, rd<uint32_t>(e, 0xb8), st[6]);
                switch (rd<uint32_t>(ngm, 0x6a48)) {
                case 1: pm_add(base, s, u, 0x660, 1); break;
                case 2: pm_add(base, s, u, 0x664, 1); break;
                case 3: pm_add(base, s, u, 0x668, 1); break;
                case 4: pm_add(base, s, u, 0x66c, 1); break;
                }
            }
        }
        publish = true;
    } else if (!u3x_pm_ranked(base, s)) {
        publish = true;
    } else {   // competitive
        if (u3x_pm_leader_of(base, 0x17d6420) && u == 0) u3x_pm_weapon_stats(base);
        at<uint32_t>(base + 0x1b68a18 + u * 4) = 1;
        if ((party[0x1339] || flag) && !u3x_pm_coop(base, s)) {
            pm_add(base, s, u, 0x5b0, 1);
            pm_add(base, s, u, 0x1f24, 1);
        }
        pm_add(base, s, u, 0x1604, 1);
        if (!flag && u3x_pm_map_is(base, s, 0x5242014bu)) u3x_pm_challenge(base, s, u, 0x78b, 1, 0);
        const int32_t ty = u3x_pm_type(base, s);
        if (ty != 0x12 && ty != 0xf && ty != 2) {
            pm_add(base, s, u, 0x5a4, st[4]);
            pm_add(base, s, u, 0x5a8, st[6]);
        }
        if (u3x_pm_map(base, s) != 0x5242014bu && (game_call<int>(base + 0x729b10, static_cast<void *>(ngm)) || u3x_pm_type(base, s) == 10 || u3x_pm_type(base, s) == 0xe) &&
            u3x_pm_type(base, s) != 0xf && u3x_pm_type(base, s) != 2) {
            pm_add(base, s, u, 0x1c54, st[4]);
            pm_add(base, s, u, 0x1c58, st[6]);
        }
        pm_add(base, s, u, 0x5b4, st[0x24]);
        pm_add(base, s, u, 0x5b8, rd<int32_t>(tm, 0x14));
        pm_add(base, s, u, 0x1f30, rd<int32_t>(tm, 0x14));
        pm_add(base, s, u, 0x65c, st[5]);
        if (u3x_pm_type(base, s) == 0xf) {
            pm_add(base, s, u, 0x658, static_cast<int16_t>(st[1]));
            u3x_pm_challenge(base, s, u, 0x765, static_cast<int16_t>(st[1]), 0);
        } else if (u3x_pm_type(base, s) == 2) {
            pm_add(base, s, u, 0x654, static_cast<int16_t>(st[1]));
        }
        const uint32_t mode = rd<uint32_t>(ngm, 0x680c);
        if (u3x_pm_map(base, s) != 0x5242014bu && mode <= 9 && ((1u << mode) & 0x244u)) {   // win / tie / loss
            const uint8_t *my = T + static_cast<uintptr_t>(team) * 0x130;
            const int32_t tmode = game_call<int32_t>(base + 0x7210a0, static_cast<void *>(ngm));
            int path = 0;   // 1 win, 2 tie, 3 loss (team), 4 loss (free for all)
            if (tmode == 2) {
                if (cnt[0] && cnt[1]) {
                    if (sc[0] == sc[1]) path = 2;
                    else if (rd<int16_t>(my, 0) != 1) path = 3;
                    else path = 1;
                } else {
                    path = 1;
                }
                if (path == 1) {
                    if (ngm[0x34e]) pm_add(base, s, u, 0x61c, 1);
                    else if (ngm[0x34d]) pm_add(base, s, u, 0x614, 1);
                } else if (path == 3) {
                    if (ngm[0x34e]) pm_add(base, s, u, 0x620, 1);
                    else if (ngm[0x34d]) pm_add(base, s, u, 0x618, 1);
                }
            } else if (tmode == 3 && team <= 2) {
                if (rd<int16_t>(my, 0) != 1) {
                    path = 4;
                } else {   // first place: a tie when another team has the same score
                    const int32_t a[3] = {sc[0], sc[1], sc[2]};
                    bool tie = false;
                    if (team) tie = sc[0] == a[team];
                    if (team != 1 && a[1] == a[team]) tie = true;
                    if (team != 2 && a[2] == a[team]) path = 2;
                    else path = tie ? 2 : 1;
                }
            }
            if (path == 1) {
                pm_add(base, s, u, 0x5ac, 1);
                pm_add(base, s, u, 0x1f34, 1);
                const uint8_t *other = T + (team == 0 ? 0x130 : 0);
                u3x_pm_365c5c(base, s, u, rd<int16_t>(my, 2), rd<int16_t>(other, 2));
            } else if (path == 2) {
                pm_add(base, s, u, 0x60c, 1);
            } else if (path == 3 || path == 4) {
                pm_add(base, s, u, 0x5b0, 1);
                pm_add(base, s, u, 0x1f24, 1);
            }
        }
    }
    if (publish) u3h_publish(base, s, u3h_lg_profile(base, u));
    u3x_pm_lb_submits(base, s, u);
}
U3X_FN uint32_t cvt_i64_float(int64_t v) {   // int64 -> float bits (cvtsi2ss)
    uint32_t r;
    __asm__("cvtsi2ss %1,%%xmm0\n\tmovd %%xmm0,%0" : "=r"(r) : "r"(v) : "xmm0");
    return r;
}
}  // namespace
extern "C" U3X_FN void u3x_pm119(uintptr_t base, void *s, uint8_t *param) {
    uint32_t *ea = ext_ea();
    uint8_t *net = reinterpret_cast<uint8_t *>(base + 0x17d5c58), *ngm = reinterpret_cast<uint8_t *>(base + 0x17cb060), *party = reinterpret_cast<uint8_t *>(base + 0x17d6420);
    uint8_t &flag = at<uint8_t>(base + 0x16e7170);
    ++ea[59];
    const bool n13 = u3x_n13_on() && n13_flag();   // n013: a spoofed matchmade match
    n13_flag() = 0;
    ea[93] = rd<uint32_t>(net, 0x40) << 8 | static_cast<uint32_t>(n13) << 4 | flag;
    // 0x29a088..0x29a3f8
    *reinterpret_cast<uint32_t *>(pm_rec(base, s, 0) + 0x558) = 0;
    u3h_publish(base, s, u3h_lg_profile(base, 0));
    *reinterpret_cast<uint32_t *>(param + 0x1c) = 0;
    game_call<void>(base + 0x74b290);
    game_call<void>(base + 0x6a3130, reinterpret_cast<void *>(base + 0x17c3ba8));
    {
        uint8_t *q = rd<uint8_t *>(param, 8);
        *reinterpret_cast<uint32_t *>(q + 0x1a0) = 0x3f800000u;
        *reinterpret_cast<uint32_t *>(param + 0x18) = at<uint32_t>(base + 0x17b4a90 + 0x114);
        *reinterpret_cast<uint32_t *>(q + 0x19c) = 0x3f800000u;
    }
    u3x_pm_menu_reset(base, 0x17bd080);
    u3x_pm_menu_reset(base, 0x17bfa10);
    at<uint8_t>(base + 0x17f9938 + 0x104b) = 0;
    {   // f_587b0(.., 1) as PS4 inlines it
        uint8_t *t = reinterpret_cast<uint8_t *>(base + 0x17bb860);
        if (*t != 1) {
            *t = 1;
            const uint32_t c = f_div(f_from_int(at<int32_t>(base + 0x17b4a90 + 0x114)), at<uint32_t>(base + 0x1443b34));
            *reinterpret_cast<uint32_t *>(t + 4) = f_add(c, at<uint32_t>(base + 0x17b6368 + 0x39b0));
            *reinterpret_cast<uint32_t *>(t + 8) = f_add(c, at<uint32_t>(base + 0x1443b38));
        }
    }
    if (party[0x1339]) {
        if (at<uint64_t>(base + 0x17dc298 + 0x18)) at<uint8_t>(base + 0x17dc298 + 0x1334) = 1;
        flag = 1;
    }
    *reinterpret_cast<uint32_t *>(ngm + 0x6808) = 0;
    at<uint32_t>(base + 0x17c3b40) = 0;
    at<uint32_t>(base + 0x17c3b40 + 0x14) = 0;
    at<uint32_t>(base + 0x17c3b40 + 4) = 0;
    at<uint8_t>(base + 0x1b68a20) = 0;
    {   // the "multi-menu" level
        const char *nm = reinterpret_cast<const char *>(base + 0x145e7f2);
        uint8_t rq[0x18];
        for (uint8_t &b : rq) b = 0;
        *reinterpret_cast<uint64_t *>(rq) = game_call<uint64_t>(base + 0x10e95b0, nm);
        *reinterpret_cast<uint32_t *>(rq + 8) = (*reinterpret_cast<uint32_t (**)(int, const char *, int)>(base + 0x1702720))(0, nm, 0);
        rq[0xe] = 0x0a;
        game_call<void>(base + 0x10eb990, reinterpret_cast<void *>(base + 0x1afcf10), static_cast<void *>(rq));
    }
    if (flag) {   // leaving: the page stack back to the lobby, the party told
        u3x_pm_pg_clear(base);
        u3x_pm_pg_push(base, 0x37656388u);
        u3x_pm_pg_push(base, 0x53cbb5f2u);
        if (!rd<uint32_t>(net, 0x40) && !n13) u3x_pm_pg_push(base, 0xb99a3235u);
        else if (n13) ++ea[91];
        u3x_pm_pg_push(base, 0xb808d03cu);
        if (u3x_pm_leader_of(base, 0x17dc298)) {
            if (!rd<uint32_t>(net, 0x40)) reinterpret_cast<int (*)(uintptr_t, uint32_t, const void *, uint32_t)>(rd<uintptr_t>(u3h_ona(), 8))(base, 0x117, net + 0x78, 8);
            else at<uint8_t>(base + 0x17dc298 + 0x1334) = 1;
        } else if (!rd<uint8_t>(rd<uint8_t *>(param, 8), 0x198)) {
            at<uint8_t>(base + 0x17dc298 + 0x1334) = 1;
        }
        game_call<void>(base + 0x74d1f0, party[0xc8] ? rd<uint64_t>(party, 0x18) : static_cast<uint64_t>(0));
        u3x_pm_party_vcall(base, 0x38, 0, 0);
        at<uint8_t>(base + 0x1b68a20) = 1;
        *reinterpret_cast<uint32_t *>(net + 0x40) = 0;
        at<uint32_t>(base + 0x1b68810) = 0x26;
        at<uint32_t>(base + 0x1b68814) = at<uint32_t>(base + 0x17b4a90 + 0x114);
    } else {
        if (uint8_t *ob = rd<uint8_t *>(party, 8))
            if (const uintptr_t fn = rd<uintptr_t *>(ob, 0)[0x58 / 8]) reinterpret_cast<void (*)(void *, void *, int)>(fn)(ob, party, 0);
        const uint8_t *host = rd<const uint8_t *>(ngm, 0x380), *t0 = rd<const uint8_t *>(ngm, 0x370);
        if (t0 && host) {   // tell the host we are back (0x105)
            const uint32_t lo = rd<uint32_t>(t0, 0x1e4) & 0xfff;
            uint32_t k = lo + 0x800;
            if (!(lo && t0[0x1ec])) k = lo;
            uint8_t b[0x10];
            *reinterpret_cast<uint32_t *>(b) = (rd<uint32_t>(t0, 0x1e8) << 12) + k;
            *reinterpret_cast<uint32_t *>(b + 4) = 0;
            *reinterpret_cast<uint64_t *>(b + 8) = rd<uint64_t>(net, 0x78);
            game_call<void>(base + 0x6b85a0, rd<uint32_t>(host, 0x5b4), 0x105, static_cast<void *>(b), 0x10);
        }
    }
    for (uint32_t u = 0; u < 2; ++u) pm_user(base, s, u);
    // 0x29be68..
    if (flag) return;
    if (n13) {   // PS3's net +0x40 != 0 branch, on the spoof
        u3h_n13_rearm(base, s);
        ++ea[92];
        ++ea[61];
        return;
    }
    if (!rd<uint32_t>(net, 0x40)) {   // back to the Custom Game lobby
        at<uint8_t>(base + 0x16e7171) = 1;
        at<uint8_t>(base + 0x16e7172) = 1;
        u3x_pm_party_vcall(base, 0x38, 0, 0);
        u3x_pm_pg_clear(base);
        u3x_pm_pg_push(base, 0x37656388u);
        u3x_pm_pg_push(base, 0x53cbb5f2u);
        u3x_pm_pg_push(base, 0xb99a3235u);
        game_call<void>(base + 0x751aa0, 0xb99a3235u, 0x5ef96af1u);
        const uint32_t md = rd<uint32_t>(ngm, 0x680c);
        if (u3x_pm_ranked(base, s) && md != 0 && md != 8 && md != 3 && !party[0x1339]) u3x_pm_pg_push(base, 0xc3a3716u);
        at<uint32_t>(base + 0x1b68810) = u3x_pm_leader_of(base, 0x17d6420) ? 3u : 0x12u;
        at<uint32_t>(base + 0x1b68814) = at<uint32_t>(base + 0x17b4a90 + 0x114);
    } else {   // back to matchmaking
        const uint64_t tsc = __builtin_ia32_rdtsc(), fr = at<uint64_t>(base + 0x16e7918);
        const uint64_t q = fr ? tsc / fr : 0, r = tsc - q * fr;
        const uint32_t fq = cvt_i64_float(static_cast<int64_t>(q)), frf = cvt_i64_float(static_cast<int64_t>(fr)), frr = cvt_i64_float(static_cast<int64_t>(r));
        at<uint32_t>(base + 0x17d79e0) = f_add(f_add(fq, f_div(frr, frf)), at<uint32_t>(base + 0x1443b3c));
        u3x_pm_pg_clear(base);
        u3x_pm_pg_push(base, 0x37656388u);
        u3x_pm_pg_push(base, 0x53cbb5f2u);
        u3x_pm_pg_push(base, 0xe35f0f1au);
        game_call<void>(base + 0x751aa0, 0xe35f0f1au, 0x5ef96af1u);
        if (u3x_shows_match_stats(rd<uint32_t>(ngm, 0x680c), party[0x1339])) u3x_pm_pg_push(base, 0xc3a3716u);
        if (u3x_pm_leader_of(base, 0x17d6420)) {
            u3x_pm_party_vcall(base, 0x68, 0, 1);
            u3x_pm_party_vcall(base, 0x40, 0, 1);
        }
    }
    ++ea[61];
}
// ===== v7.211: fourth split, part 15 (the online backend's room messages) =====

extern "C" {
U3H_IMPORT void *u3h_onb_object(void);
U3H_IMPORT uint8_t *u3h_onb_msg(void);
U3H_IMPORT uint64_t *u3h_onb_pj(void);
U3H_IMPORT void **u3h_onb_vt(void);
U3H_IMPORT int u3h_smn_send(const void *m, uint32_t n);
U3H_IMPORT void u3h_smn_attach(int on);
U3H_IMPORT int u3h_ol_up(void);
U3H_IMPORT int u3h_ol_frame(uint32_t type, const void *m, uint32_t n);
U3H_IMPORT void u3h_fw_line(const char *a, uint32_t hex, int dig, const char *b, int32_t v);
U3H_IMPORT const char *u3h_fw_sess(uintptr_t base, uint8_t *ss);
U3H_IMPORT uint32_t u3h_fw_game_ra(uintptr_t base, uintptr_t ra, uintptr_t fp);
U3H_IMPORT int32_t u3h_fw_host_link(uintptr_t base);
U3H_IMPORT void u3h_cg_log(const char *a, int32_t v);
U3H_IMPORT void *u3h_fl_state(void);
U3H_IMPORT void u3h_ng_stamp(uintptr_t base, void *s, uint8_t *md);
U3H_IMPORT void u3h_lnb_marks(uint8_t *guest_wait, uint8_t *host_playing, uint8_t *at_start, uint64_t *followed);
}

// b014: the online backend (PS3 1.19's matchmaking client object, f_3831dc .. f_385540). The object (u3h_onb_object) is
// the vtable, the online id, the four bound sessions (match, party, ...; the cookie of a binding is its index + 1), the
// next ping time (float seconds), the receive buffer and the image base. Each slot builds one little-endian room
// message (type at +0, 303..323) in the shared message buffer and sends it over the PS3 session socket while that is
// live, else as a community frame 0x0400 + (type - 300) over the online link. Slot 1 (Update, the receive side) stays in
// C beside the private session-wire block. ExtState eb: [128] object, [130] Init result, [131] slot calls, [132] last
// slot, [133] messages sent, [134] last sent type, [137] pings, [138] creates, [139] joins, [140] searches, [141]
// leaves, [147] receive overflows, [148] unknown cookies, [93] net players made at match start.
namespace {
struct OnbBe {
    void *vt;
    char name[0x24];
    uint8_t *bind[4];
    uint32_t next_ping;   // float bits
    uint32_t rxlen;
    uint8_t rx[0x1000];
    uintptr_t base;
};
static_assert(sizeof(OnbBe) == 0x1060, "the C OnbBe");
OnbBe &onb_be(void *be) { return *static_cast<OnbBe *>(be); }
uint8_t *onb_match_ss(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17d6420); }
uint8_t *onb_party_ss(uintptr_t base) { return reinterpret_cast<uint8_t *>(base + 0x17dc298); }
uint64_t &onb_room(uint8_t *ss) { return *reinterpret_cast<uint64_t *>(ss + 0x18); }
template <typename T> T &onb_put(uint8_t *m, uint32_t off) { return *reinterpret_cast<T *>(m + off); }
// seconds on the PS3 timebase (a 1/1200 s tick count); x87 like the C
[[gnu::always_inline]] inline float onb_seconds(uintptr_t base) { return static_cast<float>(at<int32_t>(base + 0x17b4ba4)) * x87_const(0x3b5a740eu); }
// a log line: text capped at 120 characters (member names up to 150), as the C log builders do
struct OnbText {
    char t[184];
    int n;
};
U3X_FN void onb_cat(OnbText &l, const char *s) {
    while (*s && l.n < 120) l.t[l.n++] = *s++;
}
U3X_FN void onb_dec(OnbText &l, uint32_t v) {
    char d[12];
    int k = 0;
    do {
        d[k++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    while (k && l.n < 120) l.t[l.n++] = d[--k];
}
U3X_FN uint8_t *onb_clear_msg(uint32_t n) {
    uint8_t *m = u3h_onb_msg();
    zero_bytes(m, n);
    return m;
}
U3X_FN void onb_clear_members(uintptr_t base, uint8_t *ss) { game_call<int64_t>(base + 0x7a25f0, ss); }   // f_381670
U3X_RO const char onb_t_session[] = " session, room ", onb_t_create[] = "Create (303): ", onb_t_old[] = " (the old one), cookie ",
                  onb_t_join[] = "Join (304) asked by the ", onb_t_cookie[] = ", cookie ", onb_t_leave[] = "Leave (307) of the ",
                  onb_t_game[] = " room asked by the game at ",
                  onb_t_outside[] = " room asked from outside the game image (helper or runtime), first return ",
                  onb_t_state[] = "  ... net state then ", onb_t_link[] = ", link to the match host + 10: ",
                  onb_t_kick[] = "Kick (311) of member ", onb_t_from[] = " from the ", onb_t_entry[] = " room, its link state+1 ",
                  onb_t_flags[] = ", entry/active/flag5/flag6 ", onb_t_asked[] = ", asked at ", onb_t_slash[] = "/",
                  onb_t_gone[] = "Kick (311) asked for an id that is not in the room: not sent; so far ",
                  onb_t_lead[] = "Kick (311) not sent: this PC does not lead that room, or the member is this PC; so far ",
                  onb_t_made[] = "match start (slot 11): net players created ", onb_t_local[] = "  ... local net player present ",
                  onb_t_there[] = "match start (slot 11): local net player already there",
                  onb_t_loaded[] = "every player connected (slot 14): start-load flag raised, players ",
                  onb_t_notonb[] = "party session is not on the online backend: member data not sent",
                  onb_t_mdsent[] = "party member data sent: room-follow marks, at start ";
U3X_BSS uint32_t onb_kick_gone, onb_kick_lead, onb_md_count;
U3X_BSS uint8_t onb_md_once;
}  // namespace

extern "C" U3X_FN void *u3x_onb_obj(uintptr_t base) {
    void *o = at<void *>(base + 0x17dd760);
    return o && at<void *>(base + 0x17dd768) == o ? o : nullptr;
}
extern "C" U3X_FN void u3x_onb_strcpy(char *d, const char *s, uint32_t n) {   // f_d78874
    uint32_t i = 0;
    for (; i + 1 < n && s[i]; ++i) d[i] = s[i];
    d[i] = 0;
}
extern "C" U3X_FN void u3x_onb_note(int slot) {
    ++ext_eb()[131];
    ext_eb()[132] = static_cast<uint32_t>(slot);
}
extern "C" U3X_FN int u3x_onb_bound(void *be, uint8_t *ss) {
    for (int i = 0; i < 4; ++i)
        if (onb_be(be).bind[i] == ss) return i;
    return -1;
}
extern "C" U3X_FN uint8_t *u3x_onb_by_room(void *be, uint64_t room) {
    for (uint8_t *s : onb_be(be).bind)
        if (s && onb_room(s) == room) return s;
    return nullptr;
}
extern "C" U3X_FN uint8_t *u3x_onb_by_cookie(void *be, uint32_t c) {
    if (c >= 1 && c <= 4 && onb_be(be).bind[c - 1]) return onb_be(be).bind[c - 1];
    ++ext_eb()[148];
    return nullptr;
}
extern "C" U3X_FN uint32_t u3x_onb_cookie(void *be, uint8_t *ss) {
    const int i = u3x_onb_bound(be, ss);
    return i < 0 ? 0 : static_cast<uint32_t>(i) + 1;
}
// f_94a1c8(conn, msg, n, 1): the PS3 socket (big-endian, swapped there) or a community frame
extern "C" U3X_FN int32_t u3x_onb_send(const void *msg, uint32_t n) {
    const uint32_t t = *static_cast<const uint32_t *>(msg);
    if (t < 300 || t > 0x3ff) return -1;
    if (u3h_smn_up()) {
        if (u3h_smn_send(msg, n) < 0) return -1;
    } else if (!u3h_ol_up() || !u3h_ol_frame(0x0400u + (t - 300u), msg, n)) {
        return -1;
    }
    ++ext_eb()[133];
    ext_eb()[134] = t;
    return static_cast<int32_t>(n);
}
// 0xC4xx from the b006 receive path, appended to the receive buffer (PS3: recv into +0x23cf8)
extern "C" U3X_FN void u3x_onb_rx(const uint8_t *p, uint32_t n) {
    OnbBe &be = onb_be(u3h_onb_object());
    if (u3h_smn_up()) return;   // the PS3 stream owns the buffer
    if (be.rxlen + n > sizeof be.rx) {
        ++ext_eb()[147];
        return;
    }
    copy_bytes(be.rx + be.rxlen, p, n);
    be.rxlen += n;
}
// the ping clock: restart (now + 30 s) and due (next ping before now)
extern "C" U3X_FN void u3x_onb_ping_restart(void *be) {
    OnbBe &o = onb_be(be);
    o.next_ping = x87_bits(onb_seconds(o.base) + x87_const(0x41f00000u));
}
extern "C" U3X_FN int u3x_onb_ping_due(void *be) {
    OnbBe &o = onb_be(be);
    return x87_float(o.next_ping) < onb_seconds(o.base);
}
// sprintf("%s.%lld", online id, time)
extern "C" U3X_FN void u3x_onb_name_time(uintptr_t base, char *out) {
    const char *id = reinterpret_cast<const char *>(base + 0x17d6010);
    uint32_t k = 0;
    for (; k < 0x24 && id[k]; ++k) out[k] = id[k];
    out[k++] = '.';
    uint64_t t = static_cast<uint32_t>(at<int32_t>(base + 0x17b4ba4));
    char d[24];
    int nd = 0;
    do {
        d[nd++] = static_cast<char>('0' + t % 10);
        t /= 10;
    } while (t);
    while (nd) out[k++] = d[--nd];
    out[k] = 0;
}
// f_32562c(net, &f124, &f112): the two location words of net +0x30 / +0x34, truncated
extern "C" U3X_FN void u3x_onb_geo(uintptr_t base, uint16_t *a, uint16_t *b) {
    const float f124 = x87_float(at<uint32_t>(base + 0x17d5c58 + 0x30)), f112 = x87_float(at<uint32_t>(base + 0x17d5c58 + 0x34));
    *a = static_cast<uint16_t>(static_cast<int32_t>(f112));
    *b = static_cast<uint16_t>(static_cast<int32_t>(f124));
}
// "U3MP online: custom game: <what><session> session, room 0x<low 32 bits><b><v>"
extern "C" U3X_FN void u3x_onb_wire(uintptr_t base, const char *what, uint8_t *ss, uint64_t room, const char *b, int32_t v) {
    OnbText l;
    l.n = 0;
    onb_cat(l, what);
    onb_cat(l, u3h_fw_sess(base, ss));
    onb_cat(l, onb_t_session);
    l.t[l.n] = 0;
    u3h_fw_line(l.t, static_cast<uint32_t>(room), 8, b, v);
}

// ---- slots ----
extern "C" U3X_FN int64_t u3x_onb_s0(void *be_) {   // f_3831dc Init
    OnbBe &be = onb_be(be_);
    u3x_onb_note(0);
    be.rxlen = 0;
    u3x_onb_ping_restart(be_);
    const int64_t r = (u3h_smn_up() == 1 || u3h_ol_up()) ? 0 : -1;   // a live session = PS3 connect >= 0
    ext_eb()[130] = static_cast<uint32_t>(r);
    if (!r) u3h_smn_attach(1);
    return r;
}
extern "C" U3X_FN int64_t u3x_onb_s2(void *be_) {   // f_3831a0 Shutdown
    u3x_onb_note(2);
    onb_be(be_).rxlen = 0;
    u3h_smn_attach(0);
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s3(void *be_, uint8_t *ss) {   // f_3835f8 AttachSession
    OnbBe &be = onb_be(be_);
    u3x_onb_note(3);
    int i = u3x_onb_bound(be_, ss);
    if (i < 0)
        for (i = 0; i < 4 && be.bind[i]; ++i) {}
    if (i >= 4) return -1;
    be.bind[i] = ss;
    *reinterpret_cast<void **>(ss + 8) = be_;   // a re-attach restores session +8 too
    return i;
}
extern "C" U3X_FN int64_t u3x_onb_s4(void *be_, uint8_t *ss, uint32_t cap, uint32_t a4, uint32_t a5, uint32_t a6) {   // f_3836c0 Create
    OnbBe &be = onb_be(be_);
    u3x_onb_note(4);
    ++ext_eb()[138];
    const int i = u3x_onb_bound(be_, ss);
    if (i < 0) return -1;
    u3h_onb_pj()[i] = 0;
    u3x_onb_wire(be.base, onb_t_create, ss, onb_room(ss), onb_t_old, static_cast<int32_t>(i + 1));
    onb_clear_members(be.base, ss);
    uint8_t *m = onb_clear_msg(232);
    onb_put<uint32_t>(m, 0) = 303;
    onb_put<uint32_t>(m, 8) = static_cast<uint32_t>(i + 1);
    onb_put<uint32_t>(m, 12) = rd<uint32_t>(ss, 0x14);
    onb_put<uint32_t>(m, 20) = rd<uint32_t>(ss, 0xf8) | (a6 ? 0x8000u : 0);   // +16: region | languages (no NP) stays 0
    onb_put<uint16_t>(m, 28) = static_cast<uint16_t>(a5);
    u3x_onb_geo(be.base, &onb_put<uint16_t>(m, 32), &onb_put<uint16_t>(m, 34));
    onb_put<uint16_t>(m, 36) = static_cast<uint16_t>(cap);
    m[38] = static_cast<uint8_t>(rd<uint32_t>(ss, 0x12f0));
    m[39] = a4 ? 4 : 0;
    ss[0x12ec] = 0;
    u3x_onb_name_time(be.base, reinterpret_cast<char *>(m + 40));
    copy_bytes(m + 168, ss + 0x12f4, 64);
    *reinterpret_cast<uint32_t *>(ss + 0x20c) = cap;
    ss[0xa1] = 0;
    *reinterpret_cast<uint32_t *>(ss + 0xa4) = 0;
    u3x_onb_send(m, 232);
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s5(void *be_, uint8_t *ss, uint32_t a3, uint32_t, int32_t a5, int32_t a6, int32_t a7, uint32_t a8) {   // f_385394 Search
    OnbBe &be = onb_be(be_);
    u3x_onb_note(5);
    ++ext_eb()[140];
    uint8_t *m = onb_clear_msg(36);
    onb_put<uint32_t>(m, 0) = 309;
    onb_put<uint32_t>(m, 8) = u3x_onb_cookie(be_, ss);
    onb_put<uint32_t>(m, 12) = rd<uint32_t>(ss, 0x14);
    *reinterpret_cast<uint32_t *>(ss + 0x218) = 0;
    onb_put<uint32_t>(m, 16) = rd<uint32_t>(ss, 0xf8) | (a8 ? 0x8000u : 0);
    u3x_onb_geo(be.base, &onb_put<uint16_t>(m, 20), &onb_put<uint16_t>(m, 22));
    onb_put<uint16_t>(m, 24) = static_cast<uint16_t>(a3);   // +32: the a4 region | languages (no NP) stays 0
    if (a5 >= 0) {
        const int32_t lo = a5 - a6;
        onb_put<uint16_t>(m, 28) = static_cast<uint16_t>(lo < 0 ? 0 : lo);
        onb_put<uint16_t>(m, 30) = static_cast<uint16_t>(a5 + a7);
    }
    u3x_onb_send(m, 36);
    ss[0xb0] = 0;
    *reinterpret_cast<uint32_t *>(ss + 0xb4) = 0;
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s6(void *be_, uint8_t *ss, uint64_t room) {   // f_3842b0 Join
    OnbBe &be = onb_be(be_);
    u3x_onb_note(6);
    ++ext_eb()[139];
    const int i = u3x_onb_bound(be_, ss);
    if (i < 0) return -1;
    u3h_onb_pj()[i] = room;   // the room this binding's pending Join asked for
    u3x_onb_wire(be.base, onb_t_join, ss, room, onb_t_cookie, static_cast<int32_t>(i + 1));
    onb_clear_members(be.base, ss);
    uint8_t *m = onb_clear_msg(88);
    onb_put<uint32_t>(m, 0) = 304;
    onb_put<uint32_t>(m, 8) = static_cast<uint32_t>(i + 1);
    m[12] = static_cast<uint8_t>(rd<uint32_t>(ss, 0x12f0));
    onb_put<uint64_t>(m, 16) = room;
    copy_bytes(m + 24, ss + 0x12f4, 64);
    *reinterpret_cast<uint32_t *>(ss + 0xbc) = 0;
    ss[0xb8] = 0;
    u3x_onb_send(m, 88);
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s7(void *be_, uint8_t *ss) {   // f_3841ac Leave
    OnbBe &be = onb_be(be_);
    const uintptr_t base = be.base;
    u3x_onb_note(7);
    ++ext_eb()[141];
    {   // who asked: the game (image-relative return) or something outside the image (helper or runtime)
        const uintptr_t ra0 = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        const uint32_t g = u3h_fw_game_ra(base, ra0, reinterpret_cast<uintptr_t>(__builtin_frame_address(0)));
        OnbText l;
        l.n = 0;
        onb_cat(l, onb_t_leave);
        onb_cat(l, u3h_fw_sess(base, ss));
        onb_cat(l, g ? onb_t_game : onb_t_outside);
        l.t[l.n] = 0;
        if (g)
            u3h_fw_line(l.t, g, 7, nullptr, -1);
        else
            u3h_fw_line(l.t, static_cast<uint32_t>(ra0 - base), 8, nullptr, -1);
        u3h_fw_line(onb_t_state, at<uint32_t>(base + 0x1b68810), 2, onb_t_link, u3h_fw_host_link(base) + 10);   // 12 up, 9 none, 8 no match room, 7 no host
    }
    const int i = u3x_onb_bound(be_, ss);
    if (i < 0) return -1;
    u3h_onb_pj()[i] = 0;
    const uint64_t room = onb_room(ss);
    ss[0xc8] = 1;
    if (ss == onb_match_ss(base)) *u3h_lnb_match_room() = 0;
    if (!room) return 0;
    *reinterpret_cast<uint32_t *>(ss + 0xcc) = 0;
    uint8_t *m = onb_clear_msg(16);
    onb_put<uint32_t>(m, 0) = 307;
    onb_put<uint64_t>(m, 8) = room;
    u3x_onb_send(m, 16);
    onb_room(ss) = 0;
    onb_clear_members(base, ss);
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s8(void *be_, uint8_t *ss, uint32_t v) {   // f_38407c SetAttr
    OnbBe &be = onb_be(be_);
    u3x_onb_note(8);
    if (u3x_onb_bound(be_, ss) < 0) return -1;
    v &= 0xff;
    if ((v != 0) == (ss[0x12ec] != 0)) return -1;   // no change
    ss[0x12ec] = static_cast<uint8_t>(v);
    const uint64_t room = onb_room(ss);
    if (!room) return 0;
    uint8_t *m = onb_clear_msg(16);
    onb_put<uint32_t>(m, 0) = 318;
    onb_put<uint64_t>(m, 8) = room;
    m[5] = 3;
    m[4] = at<uint32_t>(be.base + 0x17d603c) == 2 ? static_cast<uint8_t>(v) : (v ? 3 : 0);   // net +0x3d4 (custom game mode)
    u3x_onb_send(m, 16);
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s9(void *be_, uint8_t *ss, uint32_t id, uint32_t flag) {   // f_383fb0 Kick
    OnbBe &be = onb_be(be_);
    const uintptr_t base = be.base;
    u3x_onb_note(9);
    if (u3x_onb_bound(be_, ss) < 0) return -1;
    const uint8_t *mb = u3x_find_member(ss, id);
    if (!mb) {   // not in the room: nothing is sent
        if (!(onb_kick_gone++ % 1800u)) u3h_fw_line(onb_t_gone, 0, 0, nullptr, static_cast<int32_t>(onb_kick_gone));
        return 0;
    }
    if (rd<uint32_t>(ss, 0x12e4) != rd<uint32_t>(ss, 0x12e8) || id == rd<uint32_t>(ss, 0x12e4)) {   // only the room's leader kicks others
        if (!(onb_kick_lead++ % 1800u)) u3h_fw_line(onb_t_lead, 0, 0, nullptr, static_cast<int32_t>(onb_kick_lead));
        return 0;
    }
    const int32_t conn = rd<int32_t>(mb, 0x34);
    const uint8_t *e = u3x_entry_by_id(base, conn);
    OnbText l;
    l.n = 0;
    onb_cat(l, onb_t_kick);
    for (int q = 0; q < 16 && mb[q] && l.n < 150; ++q) l.t[l.n++] = static_cast<char>(mb[q]);
    onb_cat(l, onb_t_from);
    onb_cat(l, u3h_fw_sess(base, ss));
    onb_cat(l, onb_t_entry);
    onb_dec(l, static_cast<uint32_t>(u3x_connection_state(base, conn) + 1));
    onb_cat(l, onb_t_flags);
    onb_dec(l, e ? 1u : 0u);
    for (int k = 4; k <= 6; ++k) {
        onb_cat(l, onb_t_slash);
        onb_dec(l, e ? e[k] : 0u);
    }
    onb_cat(l, onb_t_asked);
    l.t[l.n] = 0;
    u3h_fw_line(l.t, u3h_fw_game_ra(base, reinterpret_cast<uintptr_t>(__builtin_return_address(0)), reinterpret_cast<uintptr_t>(__builtin_frame_address(0))), 7, nullptr, -1);
    const uint64_t room = onb_room(ss);
    if (!room) return 0;
    uint8_t *m = onb_clear_msg(16);
    onb_put<uint32_t>(m, 0) = 311;
    onb_put<uint16_t>(m, 4) = static_cast<uint16_t>(id);
    onb_put<uint16_t>(m, 6) = static_cast<uint16_t>(flag & 0xff);
    onb_put<uint64_t>(m, 8) = room;
    u3x_onb_send(m, 16);
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s10(void *be_, uint8_t *ss, uint32_t id) {   // f_383eec Promote
    u3x_onb_note(10);
    if (u3x_onb_bound(be_, ss) < 0) return -1;
    const uint64_t room = onb_room(ss);
    if (!room) return 0;
    uint8_t *m = onb_clear_msg(16);
    onb_put<uint32_t>(m, 0) = 316;
    onb_put<uint16_t>(m, 4) = static_cast<uint16_t>(id);
    onb_put<uint64_t>(m, 8) = room;
    u3x_onb_send(m, 16);
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s11(void *be_, uint8_t *ss, uint32_t v) {   // f_383de8 RoomFlags
    const uintptr_t base = onb_be(be_).base;
    u3x_onb_note(11);
    const uintptr_t mg = base + 0x17cb060;
    if (v && ss == onb_match_ss(base)) {   // the match starts: PS3 state 0x11's member loop makes the net players
        if (!at<uintptr_t>(mg + 0x380)) {
            int made = 0;
            for (int i = 0; i < 10; ++i) {
                uint8_t *m = ss + 0x680 + i * 0x120;
                if (!m[0x25]) continue;
                uint8_t *pl = game_call<uint8_t *>(base + 0x72eca0, mg, ss, m, 0u, 0u);
                if (!pl) continue;
                *reinterpret_cast<int32_t *>(pl + 0x21c) = -1;
                pl[0x365] = 0;
                ++made;
                ++ext_eb()[93];
            }
            u3h_cg_log(onb_t_made, made);
            u3h_cg_log(onb_t_local, at<uintptr_t>(mg + 0x380) != 0);
        } else {
            u3h_cg_log(onb_t_there, -1);
        }
    }
    if (u3x_onb_bound(be_, ss) < 0) return -1;
    const uint64_t room = onb_room(ss);
    if (room) {
        uint8_t *m = onb_clear_msg(16);
        onb_put<uint32_t>(m, 0) = 320;
        onb_put<uint16_t>(m, 4) = static_cast<uint16_t>(v);
        onb_put<uint64_t>(m, 8) = room;
        u3x_onb_send(m, 16);
    } else if (u3x_session_host(ss)) {   // f_37f1f8
        *reinterpret_cast<uint32_t *>(ss + 0x204) = v;
        ss[0xf0] = 1;
    }
    return 0;
}
// the party's member data as lnb_rank_stamp writes it on LAN: rank (bit 29 = stamped), match room, mode and follow marks
extern "C" U3X_FN void u3x_onb_md_stamp(uintptr_t base, uint8_t *md) {
    void *s = u3h_fl_state();
    const uintptr_t rank = s ? state_native(s, 21) : 0;   // NATIVE_PROFILE_RANK
    uint8_t *pr = player_profile(base);
    if (rank && pr) *reinterpret_cast<uint32_t *>(md + 0x3c) = reinterpret_cast<uint32_t (*)(void *)>(rank)(pr) | 0x20000000u;
    const uint64_t room = *u3h_lnb_match_room();
    uint8_t guest_wait, host_playing, at_start;
    uint64_t followed;
    u3h_lnb_marks(&guest_wait, &host_playing, &at_start, &followed);
    *reinterpret_cast<uint64_t *>(md + 0x34) = room;
    *reinterpret_cast<int16_t *>(md + 0x30) = at<int16_t>(base + 0x17d167c);
    md[0x32] = u3x_member_status(at<uint32_t>(base + 0x17d603c) == 2, guest_wait, host_playing);
    md[0x33] = u3x_member_start(at_start, followed, room);
    u3h_ng_stamp(base, s, md);
}
extern "C" U3X_FN int64_t u3x_onb_s12(void *be_, uint8_t *ss, const uint8_t *data, uint32_t len) {   // f_383c88 MemberData
    OnbBe &be = onb_be(be_);
    u3x_onb_note(12);
    if (len > 64) return -1;   // PS3 traps (twu)
    *reinterpret_cast<uint32_t *>(ss + 0x12f0) = len;
    copy_bytes(ss + 0x12f4, data, len);
    if (u3x_onb_bound(be_, ss) < 0) return -1;
    const uint64_t room = onb_room(ss);
    if (!room) return 0;
    uint8_t *m = onb_clear_msg(80);
    onb_put<uint32_t>(m, 0) = 314;
    m[4] = static_cast<uint8_t>(len);
    onb_put<uint64_t>(m, 8) = room;
    copy_bytes(m + 16, data, len);
    if (ss == onb_party_ss(be.base) && len == 0x40) u3x_onb_md_stamp(be.base, m + 16);
    u3x_onb_send(m, 80);
    return 0;
}
// member data to the party, on LAN (lan_be) or online
extern "C" U3X_FN void u3x_fl_md(void *lan_be, uint8_t *ps) {
    if (lan_be) {
        u3x_lnb_s12(lan_be, ps, ps + 0x12f4, 0x40);
        return;
    }
    void *ob = rd<void *>(ps, 8);
    if (!ob || onb_be(ob).vt != u3h_onb_vt()) {
        if (!onb_md_once) {
            onb_md_once = 1;
            u3h_cg_log(onb_t_notonb, -1);
        }
        return;
    }
    *reinterpret_cast<uint32_t *>(ps + 0x12f0) = 0x40;
    u3x_onb_s12(ob, ps, ps + 0x12f4, 0x40);
    if (onb_md_count++ < 40) {
        uint8_t guest_wait, host_playing, at_start;
        uint64_t followed;
        u3h_lnb_marks(&guest_wait, &host_playing, &at_start, &followed);
        u3h_cg_log(onb_t_mdsent, at_start);
    }
}
extern "C" U3X_FN int64_t u3x_onb_s13(void *be_, uint8_t *ss, uint32_t v) {   // f_385540 SetAttr4
    u3x_onb_note(13);
    if (u3x_onb_bound(be_, ss) < 0) return -1;
    const uint64_t room = onb_room(ss);
    if (!room) return 0;
    uint8_t *m = onb_clear_msg(16);
    onb_put<uint32_t>(m, 0) = 318;
    m[4] = (v & 0xff) ? 4 : 0;
    m[5] = 4;
    onb_put<uint64_t>(m, 8) = room;
    u3x_onb_send(m, 16);
    return 0;
}
extern "C" U3X_FN int64_t u3x_onb_s14(void *be_, uint8_t *ss, const uint16_t *rank, uint32_t n) {   // f_383b28 HostRank
    const uintptr_t base = onb_be(be_).base;
    u3x_onb_note(14);
    if (ss == onb_match_ss(base) && at<uint32_t>(base + 0x1b68810) == 0x30u && !ss[0x1338] && rd<uint32_t>(ss, 0x12e4) == rd<uint32_t>(ss, 0x12e8)) {
        ss[0x1338] = 1;   // the host raises the start-load flag, as lnb_s14
        u3h_cg_log(onb_t_loaded, static_cast<int32_t>(n));
    }
    if (u3x_onb_bound(be_, ss) < 0) return -1;
    const uint64_t room = onb_room(ss);
    if (!room) {
        ss[0xe0] = 1;
        return 0;
    }
    if (n > (0x100 - 16) / 2) n = (0x100 - 16) / 2;   // PS3: alloca of 16 + 2n
    uint8_t *m = onb_clear_msg(16 + 2 * n);
    onb_put<uint32_t>(m, 0) = 322;
    onb_put<uint16_t>(m, 4) = static_cast<uint16_t>(n);
    onb_put<uint64_t>(m, 8) = room;
    copy_bytes(m + 16, rank, 2 * n);
    u3x_onb_send(m, 16 + 2 * n);
    return 0;
}
namespace {
U3X_FN int64_t onb_send_name(uint8_t *ss) {   // 323: the room name of ss +0x20
    const uint64_t room = onb_room(ss);
    if (!room) return 0;
    uint8_t *m = onb_clear_msg(144);
    onb_put<uint32_t>(m, 0) = 323;
    onb_put<uint64_t>(m, 8) = room;
    u3x_onb_strcpy(reinterpret_cast<char *>(m + 16), reinterpret_cast<const char *>(ss + 0x20), 128);
    u3x_onb_send(m, 144);
    return 0;
}
}  // namespace
extern "C" U3X_FN int64_t u3x_onb_s15(void *be_, uint8_t *ss) {   // f_3852b4 RoomNameAuto
    u3x_onb_note(15);
    ss[0xe8] = 1;
    u3x_onb_name_time(onb_be(be_).base, reinterpret_cast<char *>(ss + 0x20));
    return onb_send_name(ss);
}
extern "C" U3X_FN int64_t u3x_onb_s16(void *, uint8_t *ss) {   // f_383110 RoomName
    u3x_onb_note(16);
    ss[0xe8] = 1;
    return onb_send_name(ss);
}
