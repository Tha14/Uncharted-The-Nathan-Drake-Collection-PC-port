// u2mp matchend: watch the match-end conditions while Playing runs, and an optional short time limit
// for test runs (notes: u2mp/docs/PORT-GAMEPLAY3.md section 6).
//
// The PS4 host checks game over inside NetGameManager::Update 0x63e6a0 (PS3 CheckGameOver 0x4a49e8,
// HostUpdate 0x4a8af8), from 0x63f31d:
//   - not before 1500 frames (5 s) after the match start ngm+0x1d60;
//   - time limit: game type (record +0xc) != 10, ngm+0x21b8 (float seconds, SetNetGame 0x63dc30 copies
//     the *net-games* record +0x20) > 0 and debug byte 0x104f4f8 == 0:
//     frames - ngm+0x1d5c (round start) > limit * 300 -> reason 6;
//   - score limit ngm+0x21b4 (record +0x14/+0x18/+0x1c by player count, rewritten each frame at
//     0x63e74c): any of the ngm+0x1c9c team scores (u16 at [0x104ae70] + 0x3dee, stride 0x34) >= limit
//     -> reason 2;
//   - round limits for type 10 (elimination) from 0x63f6e4 (ngm+0x21c0 rounds, +0x1d74 played).
//   The reason goes to the EndGame sender 0x60c7f0(reason, &clock) (PS3 0x489ff8); its Apply 0x62c9f0
//   sets ngm+0x1e84 / +0x1e88, and Playing::Update 0x5f14d0 then goes to ShowResults (+0x188),
//   ShowRoundResults (+0x190) or PreResetting (+0x170).
// Test run 41 left every match through the pause menu before the limit (1-2.5 minutes), so none of
// this has been seen working. This file logs those values every 10 s and on every change of the end
// state, so the next run shows where the chain stops.
//
// After the original update it also runs visibility_kick() (visibility.cpp, PS3 KickVisibilityJob), the
// job PS3 PostProcessUpdate 0x21db8 kicked at the end of every multiplayer frame.
//
// UNCHARTED_U2MP_TIME_LIMIT=<seconds> (test aid, off by default): while Playing runs, the time limit
// ngm+0x21b8 is set to that value, so a solo match reaches the time-limit game over quickly.
//
// Install: wraps the Playing::Update slot 0x100ab98 around whatever is there. It must run after
// session.cpp wrapped the match states (that install checks the slots still hold the PS4 functions),
// so it only installs when the slot no longer holds 0x5f14d0.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
void install_matchend(int &installed);
void visibility_kick();   // visibility.cpp: PS3 KickVisibilityJob 0x494c38, once per frame after the update

namespace {

constexpr uint64_t kNgm = 0x113b760;          // g_netGameManager (PS3 0xf1dc80)
constexpr uint64_t kNetState = 0x14ce97c;     // NET_SM
constexpr uint64_t kFrames = 0x1116180 + 0x204;   // frame counter, 300 Hz (PS3 [0xe60a00+0x1cc])
constexpr uint64_t kTeamScorePtr = 0x104ae70; // pointer slot; + 0x3dee: u16 team scores, stride 0x34
constexpr uint64_t kTimeLimitOff = 0x104f4b0 + 0x48;   // debug byte: time limit off when set
constexpr uint64_t kNpdCash = 0x11474f8 + 0xd8;   // NetPlayerData money (session.cpp reads the same)
constexpr uint64_t fLookup = 0xa15e70;        // ScriptManager LookupPointer(id)
constexpr uint32_t kNetGames = 0x3b9a067d;    // *net-games*: {s32 count; rec* +8}, stride 0x90
constexpr uint64_t NGM_GAME = 0x1ca0, NGM_TEAMS = 0x1c9c, NGM_ROUND_START = 0x1d5c, NGM_MATCH_START = 0x1d60,
                   NGM_ROUNDS_PLAYED = 0x1d74, NGM_STATE = 0x1e80, NGM_END_A = 0x1e84, NGM_END_B = 0x1e88,
                   NGM_SCORE_LIMIT = 0x21b4, NGM_TIME_LIMIT = 0x21b8, NGM_ROUND_LIMIT = 0x21c0;
constexpr uint64_t sPlayingUpdate = 0x100ab80 + 0x18, hPlayingUpdate = 0x5f14d0;

using UpdateFn = uint64_t(U2MP_GUEST *)(void *);
UpdateFn previous_update = nullptr;
float test_time_limit = 0.0f;
uint64_t next_log_ms = 0;
int32_t last_end_a = -1, last_end_b = -1, last_state = -1;

template <typename T> T G(uint64_t off) {
    T v;
    std::memcpy(&v, image.base + off, sizeof v);
    return v;
}

int32_t game_type() {
    using Lookup = const uint8_t *(U2MP_GUEST *)(uint32_t);
    const uint8_t *t = reinterpret_cast<Lookup>(image.base + fLookup)(kNetGames);
    const int16_t game = G<int16_t>(kNgm + NGM_GAME);
    if (!t || game < 0) return -1;
    int32_t count;
    uint64_t records;
    std::memcpy(&count, t, 4);
    std::memcpy(&records, t + 8, 8);
    if (game >= count || !records) return -1;
    int32_t type;
    std::memcpy(&type, reinterpret_cast<const uint8_t *>(uintptr_t(records)) + 0x90 * game + 0xc, 4);
    return type;
}

void log_conditions(const char *why) {
    const int32_t now = G<int32_t>(kFrames);
    const int32_t teams = G<int32_t>(kNgm + NGM_TEAMS);
    char scores[96] = "";
    const uint64_t table = G<uint64_t>(kTeamScorePtr);
    if (table) {
        size_t n = 0;
        for (int32_t i = 0; i < teams && i < 8 && n < sizeof scores - 8; ++i) {
            uint16_t s;
            std::memcpy(&s, reinterpret_cast<const void *>(uintptr_t(table + 0x3dee + 0x34 * i)), 2);
            n += size_t(std::snprintf(scores + n, sizeof scores - n, "%s%u", i ? "/" : "", unsigned(s)));
        }
    }
    log("match end watch (%s): type %d, %.1f s of round (match %.1f s), time limit %.0f s%s, score limit %d, "
        "teams %d scores %s, rounds %d/%d, ngm state %d end %d/%d, NET_SM %d, cash %d",
        why, game_type(), double(now - G<int32_t>(kNgm + NGM_ROUND_START)) / 300.0,
        double(now - G<int32_t>(kNgm + NGM_MATCH_START)) / 300.0, double(G<float>(kNgm + NGM_TIME_LIMIT)),
        G<uint8_t>(kTimeLimitOff) ? " (debug off)" : "", G<int32_t>(kNgm + NGM_SCORE_LIMIT), teams,
        scores[0] ? scores : "-", G<int32_t>(kNgm + NGM_ROUNDS_PLAYED), G<int32_t>(kNgm + NGM_ROUND_LIMIT),
        G<int32_t>(kNgm + NGM_STATE), G<int32_t>(kNgm + NGM_END_A), G<int32_t>(kNgm + NGM_END_B),
        G<int32_t>(kNetState), G<int32_t>(kNpdCash));
}

U2MP_GUEST uint64_t playing_update(void *state) {
    if (test_time_limit > 0.0f && G<float>(kNgm + NGM_TIME_LIMIT) != test_time_limit) {
        log("match end watch: time limit %.0f s -> %.0f s (UNCHARTED_U2MP_TIME_LIMIT)",
            double(G<float>(kNgm + NGM_TIME_LIMIT)), double(test_time_limit));
        std::memcpy(image.base + kNgm + NGM_TIME_LIMIT, &test_time_limit, 4);
    }
    const uint64_t result = previous_update(state);
    visibility_kick();
    const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const int32_t a = G<int32_t>(kNgm + NGM_END_A), b = G<int32_t>(kNgm + NGM_END_B),
                  s = G<int32_t>(kNgm + NGM_STATE);
    if (a != last_end_a || b != last_end_b || s != last_state) {
        last_end_a = a, last_end_b = b, last_state = s;
        log_conditions("end state changed");
    } else if (now_ms >= next_log_ms) {
        log_conditions("10 s");
    } else {
        return result;
    }
    next_log_ms = now_ms + 10000;
    return result;
}

}  // namespace

void install_matchend(int &installed) {
    if (!image.base || sPlayingUpdate + 8 > image.size) return;
    if (const char *env = std::getenv("UNCHARTED_U2MP_TIME_LIMIT")) {
        const float v = std::strtof(env, nullptr);
        if (v >= 10.0f && v <= 3600.0f) test_time_limit = v;
        log("match end watch: UNCHARTED_U2MP_TIME_LIMIT=%s -> %s", env, test_time_limit > 0.0f ? "used" : "ignored (10..3600)");
    }
    uint64_t &slot = *image.at<uint64_t>(sPlayingUpdate);
    const uint64_t base = uint64_t(uintptr_t(image.base));
    if (!slot || slot == base + hPlayingUpdate) {
        log("match end watch: Playing::Update slot is not wrapped by session.cpp yet, left alone");
        return;
    }
    previous_update = reinterpret_cast<UpdateFn>(uintptr_t(slot));
    slot = uint64_t(uintptr_t(&playing_update));
    log("match end watch: Playing::Update slot 0x%llx wrapped", (unsigned long long)sPlayingUpdate);
    ++installed;
}

}  // namespace u2mp
