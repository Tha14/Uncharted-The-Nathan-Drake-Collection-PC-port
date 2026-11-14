// u2mp rosternames: fills the empty player display names the team lists print.
//
// Rosters on the loading screen, the in-match scoreboard and the results all show each player's
// 0x40-byte display name. The PS4 code that used to set it is gone, so every row is blank. The
// nick is still there, so before a team list builds its rows or fetches a cell's text we copy the
// nick into any display name that is still empty: in the ten NetGameManager trackers and in both
// NetStatGame tables (current game and the previous-game copy).
//
// Reimplemented from the behaviour description u2mp/docs/spec/gameplay.md.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
void install_rosternames(int &installed);

namespace {

constexpr uint64_t kTeamListVtable = 0x100a280;
constexpr uint64_t kRowBuild = 0x5b0f70;    // slot 2
constexpr uint64_t kCellText = 0x5b1e90;    // slot 14

constexpr uint64_t kTrackers = 0x113b770;
constexpr uint64_t kTrackerStride = 0x2c0;
constexpr int kTrackerCount = 10;

constexpr uint64_t kStatTables[2] = {0x1147bd8, 0x114d2ec};
constexpr uint64_t kRecordStride = 0x80;
constexpr uint64_t kRecordNick = 0x38f8;
constexpr uint64_t kRecordDisplay = 0x3918;
constexpr uint32_t kMaxRecords = 10;

constexpr int kMaxLogLines = 8;
int log_lines = 0;

// Copies `nick` into `display` when the nick has text and the display name has none.
// Returns true when it copied. `limit` is the longest name the destination holds.
bool fill_one(const unsigned char *nick, unsigned char *display, size_t limit) {
    if (nick[0] == 0 || display[0] != 0) return false;
    const size_t n = strnlen(reinterpret_cast<const char *>(nick), limit);
    std::memcpy(display, nick, n);
    display[n] = 0;
    return true;
}

void fill_names() {
    int filled = 0;
    for (int i = 0; i < kTrackerCount; ++i) {
        unsigned char *tracker = image.at<unsigned char>(kTrackers + uint64_t(i) * kTrackerStride);
        if (fill_one(tracker + 0x20, tracker + 0x60, 0x3f)) ++filled;
    }
    for (uint64_t table_offset : kStatTables) {
        unsigned char *table = image.at<unsigned char>(table_offset);
        uint32_t count = 0;
        std::memcpy(&count, table, 4);
        if (count > kMaxRecords) count = kMaxRecords;
        for (uint32_t i = 0; i < count; ++i) {
            unsigned char *record = table + uint64_t(i) * kRecordStride;
            if (fill_one(record + kRecordNick, record + kRecordDisplay, 0x1f)) ++filled;
        }
    }
    if (filled > 0 && log_lines < kMaxLogLines) {
        ++log_lines;
        log("rosternames: %d display name(s) filled from the nick (trackers +0x60, NetStatGame +0x3918)", filled);
    }
}

using ListFn = uint64_t(U2MP_GUEST *)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

U2MP_GUEST uint64_t row_build_hook(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    fill_names();
    return reinterpret_cast<ListFn>(image.base + kRowBuild)(a, b, c, d, e, f);
}

U2MP_GUEST uint64_t cell_text_hook(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    fill_names();
    return reinterpret_cast<ListFn>(image.base + kCellText)(a, b, c, d, e, f);
}

void hook_slot(unsigned slot, uint64_t expected, void *host, const char *what, int &installed) {
    const uint64_t slot_offset = kTeamListVtable + 8ull * slot;
    if (slot_offset + 8 > image.size) return;
    uint64_t *entry = image.at<uint64_t>(slot_offset);
    if (*entry != uint64_t(uintptr_t(image.base + expected))) {
        log("rosternames: team list vtable 0x%llx slot %u is not 0x%llx, left alone",
            (unsigned long long)kTeamListVtable, slot, (unsigned long long)expected);
        return;
    }
    *entry = uint64_t(uintptr_t(host));
    log("rosternames: team list %s 0x%llx -> C++ (vtable 0x%llx slot %u)", what, (unsigned long long)expected,
        (unsigned long long)kTeamListVtable, slot);
    installed += 1;
}

}  // namespace

void install_rosternames(int &installed) {
    if (!image.base) return;
    // The cell text reads the display name at record +0x3918, and NetStatGame's start copies the
    // tracker's +0x60 into it: the two places that make the offsets above right.
    static const unsigned char cell_reads_name[] = {0x4B, 0x8D, 0xB4, 0x3C, 0x18, 0x39, 0x00, 0x00};
    static const unsigned char stat_copies_name[] = {0x49, 0x8D, 0xBC, 0x1D, 0x20, 0x39, 0x00, 0x00};
    if (!image.matches(0x5b24fc, cell_reads_name, sizeof cell_reads_name) ||
        !image.matches(0x66c998, stat_copies_name, sizeof stat_copies_name)) {
        log("rosternames: team list / NetStatGame bytes differ, left alone");
        return;
    }
    hook_slot(2, kRowBuild, reinterpret_cast<void *>(&row_build_hook), "row build", installed);
    hook_slot(14, kCellText, reinterpret_cast<void *>(&cell_text_hook), "cell text", installed);
}

}  // namespace u2mp
