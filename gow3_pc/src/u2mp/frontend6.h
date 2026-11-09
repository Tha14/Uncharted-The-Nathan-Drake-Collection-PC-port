// u2mp frontend6: the 5 s "Starting in [A]..." popup before a match (PS3 0x42a0b8 and its callers in
// Start Match 0x43aab0 / mailbox 0x114) and the level preload of the matchmaking vote (PS3 0x4a12e8
// callers 0x4b0840 / 0x4b0a28 / 0x4affe0 / 0x4b1fd4). See u2mp/docs/PORT-FRONTEND6.md.
#pragma once
#include <cstdint>

namespace u2mp {

void install_frontend6(int &installed);

namespace frontend6 {

// Session message (frontend3's range, session::kFrontendMessages and up): PS3 party mailbox 0x114,
// leader -> members: "the leader pressed Start Match", show the popup.
constexpr uint8_t kStartingMessage = 0x17;

// True when the popup and its done callbacks are installed.
bool installed();
// Party leader, Start Match (PS3 0x43aab0): mailbox 0x114 to the party and the popup with done
// 4055f45f / cancel bbb649ef (matchmaking) or done e2707dbe / cancel bf6ce4d5 (custom game). False when
// not installed: the caller then starts right away, as before.
bool leader_starting(bool matchmaking);
// Party member on kStartingMessage from the leader (PS3 0x43a444 -> 0x43a8ec): the popup with done
// ee8f5f9b (a blr on both) and no cancel.
void member_starting();
// True while the popup page 0x453470dd is the menu's overlay page.
bool popup_up();
// NetGameManager level preload (PS3 0x4a12e8 = PS4 0x641690)(ngm, request, keep_menu, flags_only).
void preload_net_game(bool request, bool keep_menu, bool flags_only);

}  // namespace frontend6
}  // namespace u2mp
