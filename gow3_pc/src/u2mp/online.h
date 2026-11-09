// u2mp online: Uncharted 2 online play over the room service of u2_online.dll (u2_online_abi.h), the way the
// PS3 game used NP Matching2 rooms and NP Signaling. online.cpp implements it; u2mp/docs/PORT-ONLINE.md
// describes the flow. Every function here is a no-op (false / 0) while online is off: no DLL
// (UNCHARTED_U2MP_ONLINE_DLL), no relay (UNCHARTED_U2MP_RELAY), or a LAN join address (UNCHARTED_U2MP_JOIN).
// Then the session layer plays over LAN exactly as before.
#pragma once
#include <cstddef>
#include <cstdint>

#include "net.h"

namespace u2mp::online {

// ---- called by the session layer (session.cpp) ----
// Multiplayer was entered: loads the DLL and opens the link (once), and returns the link the transport
// hosts the party on. NULL: online is off, use LAN.
net::Link *start();
// True while the session's transport runs over the online link.
bool active();
// Once per game frame: room service events, link state, room state (hide / 0x53), timers.
void frame();
// Start Match in a custom game (host): makes the match room. True: the room is being made and
// session::online_match_ready() follows; false: go on at once (no room needed or online off).
bool create_match_room();
// The match room this PC is in (0 none): sent with the follow so that party members join it too.
uint64_t match_room();
// A client got the host's follow carrying the match room id: join that room as a member.
void follow_match_room(uint64_t room_id);
// The transport reached the room owner (client), or a peer joined / left it (host).
void on_host_reached(uint32_t net_id);
// The transport lost its host while online is switching it to another room on purpose (follow).
bool host_change_expected();
// The session closed the transport (kicked, host lost, Multiplayer left): leave every room.
void on_transport_closed(const char *why);
// A session message of type session kOnlineMessage (from transport slot `from`).
void on_message(uint8_t from, const uint8_t *data, size_t size);

// ---- menus ----
// BUTTON_MULTIPLAYER_INVITES while online: opens the join list (PAGE_MULTIPLAYER_JOIN) and searches.
bool open_join_list();
// The join list (menus.cpp cond 3): row count and texts; -1 rows = online off (the LAN answer applies).
int join_rows();
const char *join_row(int row);
const char *join_title();
// BUTTON_MULTIPLAYER_SELECT_JOIN_GAME while online: join the room of that row. False when online is off.
bool select_join(int row);

// ---- progression.cpp (0x64e810, PS3 0x4af240): NetPlayerData changed, publish the member blob 0x59 ----
void member_data_changed();

}  // namespace u2mp::online

namespace u2mp::session {
// online.cpp -> session.cpp: closes the transport and opens it again on `link`, as the host of a new
// party (owner NULL) or as a client of the room owner at `owner`.
void online_reopen(net::Link *link, const net::Address *owner);
// online.cpp -> session.cpp: the match room exists (room_id) or could not be made (0): finish Start Match.
void online_match_ready(uint64_t room_id);
// Session message type of online.cpp (below session::kFrontendMessages).
constexpr uint8_t kOnlineMessage = 7;
}  // namespace u2mp::session
