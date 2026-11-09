// u2mp rosternames: the player name in the team lists (loading-screen rosters, in-match scoreboard,
// results), which test 48 showed empty: portrait, rank badge and kills, no name.
//
// Where the name comes from (PS4 1.02 image offsets, PS3 1.10 in brackets):
//   * The team list class (vtable 0x100a280, ctor 0x5b0ea0; slot 2 = row build 0x5b0f70, slot 14 =
//     cell text 0x5b1e90) reads its rows from NetStatGame 0x1147bd0: table T = 0x1147bd8 (or the
//     previous-game copy 0x1147bd0+0x571c), count at T, player records of 0x80 bytes. For a player row
//     (type 2), the name column (column id cde4b742, 0x5b24ed) prints "%s" of T + 0x3918 + i * 0x80:
//     the 0x40-byte DISPLAY name, not the 0x20-byte nick at T + 0x38f8.
//   * NetStatGame start (0x66c600 [inlined in 0x49b858]) fills both from the NetGameManager trackers
//     (0x113b760 + 0x10 + slot * 0x2c0): nick <- tracker +0x20 (0x66c97d, 0x20 bytes), display name <-
//     tracker +0x60 (0x66c998, 0x40 bytes).
//   * Tracker +0x60 is written only by AddOrUpdateNetPlayer 0x642370 [0x4e05e0, formatter 0x49ada8]:
//     snprintf(+0x60, 0x40, "%s", name), or in matchmaking with a skill entry
//     "|@S||@R8080FF|%i|@RFFFFFF||@N| %s" (level, name). PS4 0x642370 is dead (no callers; its name
//     source folded to NULL, PORT-NULLSITES.md), and session.cpp's port of it (add_net_player) sets
//     the nick +0x20 but not +0x60. So the display name stays empty in the trackers and in NetStatGame,
//     and every team list draws an empty name.
//
// The fix: before the team list builds its rows (slot 2) and before it asks for a cell (slot 14), every
// tracker with a nick and no display name gets display = "%s" nick (0x642370's custom-game path), and
// every NetStatGame record (both tables) with a nick and no display name gets the same, so lists of a
// match already started are fixed too. The matchmaking skill prefix is not made (no skill level on PC).
// The other columns of the player rows (kills, deaths, money, co-op points: tallies +0x3de6.. of the
// same table; badge and colour from 0x64aeb0) were already filled, as the test 48 rows show.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t vtTeamList = 0x100a280;
constexpr uint64_t fRowBuild = 0x5b0f70, fCellText = 0x5b1e90;
constexpr uint64_t kNgm = 0x113b760, NGM_TRACKERS = 0x10, NGM_TRACKER_SIZE = 0x2c0;
constexpr uint64_t T_NICK = 0x20, T_DISPLAY = 0x60, kTrackerNickMax = 0x3f;   // both 0x40 bytes (0x642427..0x64243d)
constexpr int kTrackers = 10;
constexpr uint64_t kStatGame = 0x1147bd0;
constexpr uint64_t kTables[] = {kStatGame + 8, kStatGame + 0x571c};   // as 0x5b1ec0..0x5b1ef7 picks
constexpr uint64_t R_NICK = 0x38f8, R_DISPLAY = 0x3918, R_SIZE = 0x80, kRecordNickMax = 0x1f;   // nick 0x20, display 0x40
constexpr uint32_t kMaxRecords = 10;

using SlotFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
SlotFn row_build_original = nullptr, cell_text_original = nullptr;
int fills_logged = 0;

// Copies the nick into an empty display field (0x642370: snprintf(display, 0x40, "%s", name)).
bool fill(char *display, const char *nick, size_t nick_max) {
    if (!nick[0] || display[0]) return false;
    const size_t n = strnlen(nick, nick_max);
    std::memcpy(display, nick, n);
    display[n] = 0;
    return true;
}

void fill_names() {
    int filled = 0;
    for (int i = 0; i < kTrackers; ++i) {
        char *t = image.at<char>(kNgm + NGM_TRACKERS + uint64_t(i) * NGM_TRACKER_SIZE);
        filled += fill(t + T_DISPLAY, t + T_NICK, kTrackerNickMax);
    }
    for (const uint64_t table : kTables) {
        const uint32_t count = *image.at<uint32_t>(table);
        for (uint32_t i = 0; i < count && i < kMaxRecords; ++i) {
            char *r = image.at<char>(table + uint64_t(i) * R_SIZE);
            filled += fill(r + R_DISPLAY, r + R_NICK, kRecordNickMax);
        }
    }
    if (filled && fills_logged < 8) {
        ++fills_logged;
        log("rosternames: %d display name(s) filled from the nick (trackers +0x60, NetStatGame +0x3918)", filled);
    }
}

U2MP_GUEST uint64_t row_build(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    fill_names();
    return row_build_original(a0, a1, a2, a3, a4, a5);
}

U2MP_GUEST uint64_t cell_text(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    fill_names();
    return cell_text_original(a0, a1, a2, a3, a4, a5);
}

bool swap_slot(unsigned index, uint64_t expected, SlotFn host, SlotFn *original, const char *name) {
    const uint64_t at = vtTeamList + 8 * uint64_t(index);
    if (at + 8 > image.size) return false;
    uint64_t &slot = *image.at<uint64_t>(at);
    if (slot != uint64_t(uintptr_t(image.base + expected))) {
        log("rosternames: team list vtable 0x%llx slot %u is not 0x%llx, left alone",
            (unsigned long long)vtTeamList, index, (unsigned long long)expected);
        return false;
    }
    *original = reinterpret_cast<SlotFn>(image.base + expected);
    slot = uint64_t(uintptr_t(host));
    log("rosternames: team list %s 0x%llx -> C++ (vtable 0x%llx slot %u)", name, (unsigned long long)expected,
        (unsigned long long)vtTeamList, index);
    return true;
}

}  // namespace

void install_rosternames(int &installed) {
    if (!image.base) return;
    // the cell text's name case: lea rsi,[r12+r15+0x3918] (0x5b24fc)
    static const unsigned char name_read[] = {0x4b, 0x8d, 0xb4, 0x3c, 0x18, 0x39, 0x00, 0x00};
    // NetStatGame start copies tracker +0x60 into the record's display name: lea rdi,[r13+rbx+0x3920]
    static const unsigned char display_copy[] = {0x49, 0x8d, 0xbc, 0x1d, 0x20, 0x39, 0x00, 0x00};
    if (!image.matches(0x5b24fc, name_read, sizeof name_read) ||
        !image.matches(0x66c998, display_copy, sizeof display_copy)) {
        log("rosternames: team list / NetStatGame bytes differ, left alone");
        return;
    }
    if (swap_slot(2, fRowBuild, &row_build, &row_build_original, "row build")) ++installed;
    if (swap_slot(14, fCellText, &cell_text, &cell_text_original, "cell text")) ++installed;
}

}  // namespace u2mp
