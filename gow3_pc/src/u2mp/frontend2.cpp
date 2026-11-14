// u2mp frontend2: the "select a game to join" button of the join list.
//
// The PS4 build of that callback reads its join entry from a stack array nobody filled in and
// dereferences it. The PC never joins that way: it joins a LAN host by address, or an online room
// through online.cpp. So the callback is replaced: the selected row goes to the online layer, and when
// that layer is not interested the press is only logged.
//
// Reimplemented from the behaviour description u2mp/docs/spec/frontend.md.

#include <cstring>

#include "online.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t kSelectJoinGame = 0x609680;

U2MP_GUEST void select_join_game(void *arg, void *, void *) {
    // The button argument carries the selected row as a u32 at +4.
    uint32_t row = 0;
    if (arg) std::memcpy(&row, static_cast<const uint8_t *>(arg) + 4, sizeof row);
    if (online::select_join(int(row))) return;
    log("join game list: row %u selected; the PC joins by address (UNCHARTED_U2MP_JOIN), nothing to join", row);
}

}  // namespace

void install_frontend2(int &installed) {
    // Two runs must match: the function's start, and the place where it reads the row and indexes the
    // uninitialised stack array. Together they make sure this is the broken callback we replace.
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x81, 0xec,
                                             0x58, 0x06, 0x00, 0x00, 0x48, 0x8b, 0x1d};
    static const unsigned char array_read[] = {0x8b, 0x47, 0x04, 0x48, 0x8d, 0x04, 0x80, 0x48, 0xc1, 0xe0,
                                               0x04, 0x48, 0x8b, 0x84, 0x05, 0xf8, 0xf9, 0xff, 0xff};
    if (!image.matches(kSelectJoinGame, prologue, sizeof prologue) ||
        !image.matches(kSelectJoinGame + 0x1a, array_read, sizeof array_read)) {
        log("BUTTON_MULTIPLAYER_SELECT_JOIN_GAME at 0x%llx: not the function this port expects, left alone",
            static_cast<unsigned long long>(kSelectJoinGame));
        return;
    }
    if (replace_function(kSelectJoinGame, 5, reinterpret_cast<void *>(&select_join_game),
                         "BUTTON_MULTIPLAYER_SELECT_JOIN_GAME"))
        installed += 1;
}

}  // namespace u2mp
