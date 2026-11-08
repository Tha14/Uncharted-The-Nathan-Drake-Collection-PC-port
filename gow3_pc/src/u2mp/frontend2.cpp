// u2mp frontend2: the PS4 1.02 frontend functions the function map marks "partial" (PS4 kept the
// function but dropped calls), compared one by one with their PS3 1.10 counterparts. Per-function
// audit and outcome: u2mp/docs/PORT-FRONTEND2.md.
//
// Most of them need nothing: the PS4 compiler inlined the "missing" callees (IsHost 0x4d2394, the
// tracker accessors 0x4919b0/0x4919d4, the net-game / level / game-type record lookups, TextDb),
// or what PS3 did there is PSN / Matching2 / party-message work whose PS4 receiver is gone and
// whose PC role session.cpp already plays (host/join by address, mode broadcast, party follow).
//
// Installed here:
// - BUTTON_MULTIPLAYER_SELECT_JOIN_GAME 0x609680 (PS3 0x465808). PS3 took the chosen Matching2
//   search result (0x4d5940), filled the join request and activated signaling (0x4e9060), then
//   NET_SM 7 (CLIENT_CONNECT_TO_HOST). PS4 reads the entry from an uninitialised stack array
//   ([rbp + row*0x50 - 0x608]), dereferences it (+0x20, +0x78, +0x80), and sets state 7, whose
//   JOINING code (0x609790) only fails. The PC joins by address (session.cpp) and the list query
//   (cond 3, menus.cpp) has no rows, so the callback should never run; if it does it now only logs
//   instead of reading garbage.
#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t cbSelectJoinGame = 0x609680;   // PS3 0x465808

// Menu callback (same arguments as the other button callbacks): the row selected is at arg+4.
U2MP_GUEST void select_join_game(void *arg, void *, void *) {
    const uint32_t row = arg ? *reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(arg) + 4) : 0;
    log("join game list: row %u selected; the PC joins by address (UNCHARTED_U2MP_JOIN), nothing to join", row);
}

}  // namespace

void install_frontend2(int &installed) {
    // 0x609680: push rbp; mov rbp,rsp; push rbx; sub rsp,0x658; mov rbx,[rip+..] (stack guard);
    // mov rax,[rbx]; mov [rbp-0x10],rax; mov eax,[rdi+4]; lea rax,[rax+rax*4]; shl rax,4;
    // mov rax,[rbp+rax-0x608]  <- the uninitialised array read
    static const unsigned char select_join_bytes[] = {
        0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x81, 0xec, 0x58, 0x06, 0x00, 0x00, 0x48, 0x8b, 0x1d};
    static const unsigned char select_join_tail[] = {
        0x8b, 0x47, 0x04, 0x48, 0x8d, 0x04, 0x80, 0x48, 0xc1, 0xe0, 0x04, 0x48, 0x8b, 0x84, 0x05, 0xf8, 0xf9, 0xff, 0xff};
    if (!image.matches(cbSelectJoinGame, select_join_bytes, sizeof select_join_bytes) ||
        !image.matches(cbSelectJoinGame + 0x1a, select_join_tail, sizeof select_join_tail)) {
        log("BUTTON_MULTIPLAYER_SELECT_JOIN_GAME at 0x%llx: not the function this port expects, left alone",
            (unsigned long long)cbSelectJoinGame);
    } else if (replace_function(cbSelectJoinGame, 5, reinterpret_cast<void *>(&select_join_game),
                                "BUTTON_MULTIPLAYER_SELECT_JOIN_GAME")) {
        ++installed;
    }
}

}  // namespace u2mp
