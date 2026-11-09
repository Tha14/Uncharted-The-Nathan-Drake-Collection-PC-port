// u2mp hud: the multiplayer HUD panel PS4 1.02 stripped, ported from PS3 1.10 (notes:
// u2mp/docs/PORT-HUD.md).
//
// PS3: the HUD manager (game/hud/hud.cpp, init 0x8a13e8 = PS4 0x552c70) creates a multiplayer element
// (ctor 0x8ac330 = PS4 0x55aa80, kept at hud+0x320, element bits 0x2000) with the vtable PS3 0xd87db8 =
// PS4 0x103f550: slot 0 Update 0x8ac1e8, slot 1 Draw 0x8aef68, slot 3 Reset 0x8a9b90.
//   - PS4 slot 0 0x55abe0 is the whole Update (sub-update +0x40, player names +0x860, the HUD flag bits
//     0xe0000 while the match runs, the fade +0x1920): intact.
//   - PS4 slot 1 0x55c5b0 is `ret`: the multiplayer Draw and every helper only it called are gone
//     (the text formats "%i:" / "%02i" and the DC ids it used are not in the PS4 image). The HUD
//     loop 0x554710 still calls slot 1 for this element every frame (bits 0x2000 in the flags), so
//     nothing draws the score, team scores, limit or time.
// This file puts a C++ Draw in slot 1 with the PS3 0x8aef68 panel logic:
//   - not multiplayer, no local tracker or no *net-games* record: nothing (PS3 0x8af000..0x8af114);
//   - competitive (game type not 4/8/9/12, PS3 0x4a0bcc) and not type 11, flag 0x40000
//     (PS3 rlwinm 13,13) set:
//       type 10 (elimination, PS3 0x8af648): frame, own / other team score, label e1167dfa, round
//         ngm+0x1d70 + 1, round time left from the match start ngm+0x1d60;
//       other types (PS3 0x8af220): frame, own / other team score, label d38a7d18, score limit
//         ngm+0x21b4, time left from the round start ngm+0x1d5c;
//       time left (PS3 0x49e0e8): limit = ngm+0x21b8 seconds * 300, shown as "%i:" minutes and "%02i"
//         seconds when > 0 (PS3 0x8af878 / 0x8af7a0);
//       type 7 (PS3 0x8af960) draws a per-player list instead and is not ported (section 3 of the notes).
//   Team scores: u16 at [0x104ae70] + 0x3dee + team * 0x34 (PS3 0xf2efc4 + 15840 + 14 + team * 52, the
//   table CheckGameOver 0x63f3f7 reads), team = tracker+0x134 (PS3 +300), other team = (team == 0).
// The co-op panels (type 9 money/wave, boosters, recording indicator, kill feed +0x40, world markers
// +0x1560, player list +0x1a90) are in the PS3 tail 0x8af150.. and not ported yet (notes, section 3).
//
// PS4 pieces (PS3 -> PS4): HUD settings record LookupPointer(9353ed16) (0x151d4c -> 0xa15e70); its
// multiplayer widget set is the 15th pointer: PS3 rec+0x58 -> PS4 rec+0x90 (the PS4 dc1 hud-settings has
// the same 16-pointer list, the SP timer element 0x559770 reads the 14th at +0x88 = PS3 +0x54). Widget
// pointers in the set: PS3 +4k -> PS4 +8k for the ones used here (+4, +8, +24, +28, +32, +36, +44 ->
// +0x08, +0x10, +0x30, +0x38, +0x40, +0x48, +0x58). Text: PS3 0x8a0f04(widget, pos, scale, fmt, ...) ->
// PS4 0x5555a0(widget, pos, fmt, float scale, ...) (variadic; xmm0 is the fixed scale, as the SP timer
// calls it). Frame image: PS3 0x8a10fc(widget, pos, scale) -> PS4 0x5559f0(draw ctx, widget, pos, scale).
// Texts: TextDb::Find(0x1066660, id) (PS3 0x9294c4(0x109d3f0, id)). HUD flags: [[0x1130008 + 0x60]]
// (PS3 [[0x1026a88 + 56]]).
//
// UNCHARTED_U2MP_HUD=0 leaves the stub alone (test switch). The first draw and every 600th are logged.
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
void install_hud(int &installed);

namespace {

constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kFrames = 0x1116180 + 0x204;    // 300 Hz frame counter (PS3 [0xe60a00+460])
constexpr uint64_t kHud = 0x1130008;               // HUD manager (PS3 0x1026a88)
constexpr uint64_t HUD_FLAGS_PTR = 0x60;           // -> u32 visibility flags (PS3 +56)
constexpr uint64_t kTeamScorePtr = 0x104ae70;      // pointer slot; + 0x3dee: u16 team scores, stride 0x34
constexpr uint64_t kTextDb = 0x1066660;
constexpr uint64_t fLookup = 0xa15e70;             // ScriptManager LookupPointer(id)
constexpr uint64_t fText = 0x5555a0;               // HUD text (widget, pos, fmt, scale, ...)
constexpr uint64_t fImage = 0x5559f0;              // HUD image (ctx, widget, pos, scale)
constexpr uint64_t fTextFind = 0xcddc00;           // TextDb::Find(db, id)
constexpr uint32_t kHudSettings = 0x9353ed16, kNetGames = 0x3b9a067d;
constexpr uint32_t kLabelScoreLimit = 0xd38a7d18, kLabelRound = 0xe1167dfa;
constexpr uint64_t vtNetHud = 0x103f550, hDrawStub = 0x55c5b0;
constexpr uint64_t NGM_GAME = 0x1ca0, NGM_ROUND_START = 0x1d5c, NGM_MATCH_START = 0x1d60, NGM_ROUND = 0x1d70,
                   NGM_SCORE_LIMIT = 0x21b4, NGM_TIME_LIMIT = 0x21b8;
constexpr uint64_t T_TEAM = 0x134;
constexpr uint64_t REC_MP_WIDGETS = 0x90;
constexpr uint64_t W_SCORE = 0x08, W_SCORE_OTHER = 0x10, W_VALUE = 0x30, W_LABEL = 0x38, W_MINUTES = 0x40,
                   W_SECONDS = 0x48, W_FRAME = 0x58;
constexpr uint32_t kFlagScorePanel = 0x40000;      // PS3 rlwinm 0,25,0,13,13
constexpr float kScale = 1.0f;                     // PS3 0xdd09ec
constexpr float kFramesPerSecond = 300.0f;         // PS3 0xdb1ec8

struct Pos {
    float x = 0.0f, y = 0.0f;   // PS3: the two zero words at sp+N before every call
};

using LookupFn = const uint8_t *(U2MP_GUEST *)(uint32_t);
using TextFn = void(U2MP_GUEST *)(const void *, const Pos *, const char *, float, ...);
using ImageFn = void(U2MP_GUEST *)(void *, const void *, const Pos *, float);
using FindFn = const char *(U2MP_GUEST *)(void *, uint32_t);

uint64_t g_draws = 0;

template <typename T> T G(uint64_t off) {
    T v;
    std::memcpy(&v, image.base + off, sizeof v);
    return v;
}
template <typename T> T R(const uint8_t *p, uint64_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}
const uint8_t *ptr_at(const uint8_t *p, uint64_t off) {
    return reinterpret_cast<const uint8_t *>(uintptr_t(R<uint64_t>(p, off)));
}

const uint8_t *lookup(uint32_t id) { return reinterpret_cast<LookupFn>(image.base + fLookup)(id); }

const uint8_t *net_game_record() {
    const uint8_t *t = lookup(kNetGames);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    if (!t || game < 0 || game >= R<int32_t>(t, 0)) return nullptr;
    const uint8_t *records = ptr_at(t, 8);
    return records ? records + 0x90 * game : nullptr;
}

unsigned team_score(int team) {
    const uint64_t table = G<uint64_t>(kTeamScorePtr);
    if (!table || team < 0 || team > 7) return 0;
    uint16_t s;
    std::memcpy(&s, reinterpret_cast<const void *>(uintptr_t(table + 0x3dee + 0x34 * uint64_t(team))), 2);
    return s;
}

void text(const uint8_t *set, uint64_t field, const char *fmt, int value) {
    const uint8_t *w = ptr_at(set, field);
    if (!w) return;
    const Pos pos;
    reinterpret_cast<TextFn>(image.base + fText)(w, &pos, fmt, kScale, value);
}
void label(const uint8_t *set, uint64_t field, uint32_t id) {
    const uint8_t *w = ptr_at(set, field);
    if (!w) return;
    const char *s = reinterpret_cast<FindFn>(image.base + fTextFind)(image.base + kTextDb, id);
    if (!s) return;
    const Pos pos;
    reinterpret_cast<TextFn>(image.base + fText)(w, &pos, "%s", kScale, s);
}
void frame_image(void *ctx, const uint8_t *set, uint64_t field) {
    const uint8_t *w = ptr_at(set, field);
    if (!w || !ctx) return;
    const Pos pos;
    reinterpret_cast<ImageFn>(image.base + fImage)(ctx, w, &pos, kScale);
}

// PS3 0x49e0e8 + 0x8af878: time left as "%i:" minutes, "%02i" seconds, when the game has a time limit.
void time_left(const uint8_t *set, uint64_t start_field) {
    const int32_t limit = int32_t(G<float>(kNgm + NGM_TIME_LIMIT) * kFramesPerSecond);   // fctiwz
    if (limit <= 0) return;
    const int32_t elapsed = G<int32_t>(kFrames) - G<int32_t>(kNgm + start_field);
    const int32_t secs = int32_t(float(limit - elapsed) * (1.0f / kFramesPerSecond));
    const int32_t mins = secs / 60;
    text(set, W_MINUTES, "%i:", mins - (mins / 60) * 60);
    text(set, W_SECONDS, "%02i", secs - mins * 60);
}

// Slot 1 of the multiplayer HUD element (PS3 0x8aef68): (element, draw context).
U2MP_GUEST void net_hud_draw(void *element, void *ctx) {
    (void)element;
    if (G<int32_t>(kNetState) < 2) return;                       // PS3 IsMultiplayer 0x4384b0
    const uint64_t flags_ptr = G<uint64_t>(kHud + HUD_FLAGS_PTR);
    if (!flags_ptr) return;
    uint32_t flags;
    std::memcpy(&flags, reinterpret_cast<const void *>(uintptr_t(flags_ptr)), 4);
    const uint8_t *rec = lookup(kHudSettings);
    const uint8_t *set = rec ? ptr_at(rec, REC_MP_WIDGETS) : nullptr;
    const uint8_t *local = reinterpret_cast<const uint8_t *>(uintptr_t(G<uint64_t>(kNgm)));   // PS3 0x4919d4
    const uint8_t *game = net_game_record();                                                  // PS3 0x499908
    if (!set || !local || !game) return;
    const int32_t type = R<int32_t>(game, 0xc);
    const bool coop = type == 4 || type == 8 || type == 9 || type == 12;   // PS3 0x4a0bcc
    const int32_t team = R<int32_t>(local, T_TEAM);
    if (++g_draws == 1 || g_draws % 600 == 0)
        log("hud: multiplayer draw #%llu: type %d, flags 0x%x, team %d, scores %u/%u, limit %d, time limit %.0f s",
            (unsigned long long)g_draws, type, flags, team, team_score(team), team_score(team == 0),
            G<int32_t>(kNgm + NGM_SCORE_LIMIT), double(G<float>(kNgm + NGM_TIME_LIMIT)));
    if (coop || type == 11 || !(flags & kFlagScorePanel) || type == 7) return;
    frame_image(ctx, set, W_FRAME);
    text(set, W_SCORE, "%i", int(team_score(team)));
    text(set, W_SCORE_OTHER, "%i", int(team_score(team == 0)));
    if (type == 10) {
        label(set, W_LABEL, kLabelRound);
        text(set, W_VALUE, "%i", G<int32_t>(kNgm + NGM_ROUND) + 1);
        time_left(set, NGM_MATCH_START);
    } else {
        label(set, W_LABEL, kLabelScoreLimit);
        text(set, W_VALUE, "%i", G<int32_t>(kNgm + NGM_SCORE_LIMIT));
        time_left(set, NGM_ROUND_START);
    }
}

bool layout_matches() {
    static const unsigned char text_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48,
                                               0x81, 0xec, 0x10, 0x02, 0x00, 0x00, 0x84, 0xc0};
    static const unsigned char image_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                                0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec};
    // ctor 0x55aab2: lea rax,[rip+..] (-> 0x103f550); element bits 0x2000 at 0x55ab94
    static const unsigned char ctor_bytes[] = {0x48, 0x8d, 0x05, 0x97, 0x4a, 0xae, 0x00};
    static const unsigned char bits_bytes[] = {0xc7, 0x43, 0x08, 0x00, 0x20, 0x00, 0x00};
    if (!image.matches(fText, text_bytes, sizeof text_bytes) || !image.matches(fImage, image_bytes, sizeof image_bytes) ||
        !image.matches(0x55aab2, ctor_bytes, sizeof ctor_bytes) || !image.matches(0x55ab94, bits_bytes, sizeof bits_bytes) ||
        !is_ret_stub(hDrawStub)) {
        log("hud: text / image / element bytes differ, multiplayer HUD not installed");
        return false;
    }
    return vtNetHud + 0x10 <= image.size && kTeamScorePtr + 8 <= image.size;
}

}  // namespace

void install_hud(int &installed) {
    if (!image.base) return;
    if (const char *env = std::getenv("UNCHARTED_U2MP_HUD"); env && env[0] == '0') {
        log("hud: UNCHARTED_U2MP_HUD=0, multiplayer HUD draw left as the PS4 stub");
        return;
    }
    if (!layout_matches()) return;
    uint64_t &slot = *image.at<uint64_t>(vtNetHud + 8);
    if (slot != uint64_t(uintptr_t(image.base + hDrawStub))) {
        log("hud: vtable 0x%llx slot 1 is not the stub 0x%llx, left alone", (unsigned long long)vtNetHud,
            (unsigned long long)hDrawStub);
        return;
    }
    slot = uint64_t(uintptr_t(&net_hud_draw));
    log("hud: multiplayer HUD draw (PS3 0x8aef68) -> C++ (vtable 0x%llx slot 1)", (unsigned long long)vtNetHud);
    ++installed;
}

}  // namespace u2mp
