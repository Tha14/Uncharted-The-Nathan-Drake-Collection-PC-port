// u2mp online: Uncharted 2 online play over the room service of u2_online.dll (u2_online_abi.h), the way the
// PS3 game used NP Matching2 rooms and NP Signaling. online.cpp implements it; u2mp/docs/PORT-ONLINE.md
// describes the flow. Every function here is a no-op (false / 0) while online is off: no DLL
// (UNCHARTED_U2MP_ONLINE_DLL), no relay (UNCHARTED_U2MP_RELAY), or a LAN join address (UNCHARTED_U2MP_JOIN).
// Then the session layer plays over LAN exactly as before.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "net.h"
#include "u2_online_abi.h"

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

// ---- host migration and promote (PS3 RoomOwnerChanged / GrantRoomOwner) ----
// A client lost its host outside a running match: true when online takes it (the room's new owner hosts
// the party; online then calls session::online_reopen / online_migrated, or online_host_gone).
bool on_host_lost();
// This PC's NpId, and the NpId behind a transport address of the online link ("" when unknown).
std::string my_npid_text();
std::string npid_at(const net::Address &a);
// The leader hands the party to the member with this net id. False: not possible (offline, no room).
bool promote(uint32_t net_id);

// ---- friends and invites (RPCN; PS3 NP Basic) ----
int friend_count();
const char *friend_row(int i);       // "name - status"
std::string friend_name(int i);
bool friend_online(int i);
bool friend_joinable(int i);         // in this game, in a party / lobby with room for us
bool join_friend(int i);             // Join Party (0ace8463)
bool invite_friend(int i);           // Invite To Party (45a9c65b)
int invite_count();                  // invites waiting (shown on BUTTON_MULTIPLAYER_INVITES)
enum { kRowFriend = 0, kRowRequestIn = 1, kRowRequestOut = 2 };
int friend_kind(int i);              // what row i is (-1 none)
bool friend_add(const std::string &npid);   // send a friend request (or accept one)
bool accept_request(int i);
bool remove_friend(int i);           // unfriend / decline / cancel
std::string profile_text(int i);     // View Profile: name, state, presence

// ---- cinema files to and from friends (PS3 fe0e7de7 / 6b2970e2) ----
bool send_cinema(const std::string &npid);   // the selected cinema of the cinema page
int cinema_received();                       // files friends sent us this session

// ---- rankings (RPCN score boards; PS3 NP Score) ----
bool linked();                       // the online link is up (also outside Multiplayer)
bool board_request(uint32_t board, int mode, uint32_t start, uint32_t count);   // mode 0 ranks, 1 friends, 2 me
int board_state();                   // 1 loading, 0 ready, -1 failed
int board_rows();
uint32_t board_total();
const U2oScore *board_row(int i);
bool record_score(uint32_t board, int64_t score, const char *comment, const uint8_t *info = nullptr, uint32_t info_len = 0);

// ---- stranger matchmaking (frontend3.cpp; PS3 0x4b0a58 search / join / create) ----
enum { kMmIdle = 0, kMmBusy = 1, kMmHosting = 2, kMmJoined = 3, kMmFailed = 4 };
bool mm_online();                    // online with a world: matchmaking finds strangers
void mm_begin(uint32_t playlist);    // host in NET_SM 19: search, then join a room or create one
int mm_step();
void mm_reset();
void mm_research();                  // nobody came: leave our gather room, search again
void mm_leave();                     // matchmaking ends for this PC
int mm_players();                    // players in our matchmaking room

}  // namespace u2mp::online

namespace u2mp {
// remaining.cpp: a one-button message box with this text (online notices: invites, migration).
void show_notice(const char *text);
}  // namespace u2mp

namespace u2mp::session {
// online.cpp -> session.cpp: closes the transport and opens it again on `link`, as the host of a new
// party (owner NULL) or as a client of the room owner at `owner`.
void online_reopen(net::Link *link, const net::Address *owner);
// online.cpp -> session.cpp: the match room exists (room_id) or could not be made (0): finish Start Match.
void online_match_ready(uint64_t room_id);
// online.cpp -> session.cpp: this player's name is the RPCN online name.
void set_local_name(const std::string &name);
// online.cpp -> session.cpp: the transport address of the member with this net id.
bool member_address(uint32_t net_id, net::Address &out);
// online.cpp -> session.cpp: the party has a new leader (host = this PC); matchmaking: the room goes on.
void online_migrated(bool host, bool matchmaking);
// online.cpp -> session.cpp: the host is gone and nobody took over: the old "host lost" path.
void online_host_gone();
// A running match may go on under a new host (PS3 HostMigrate at the match start, not in co-op).
bool match_migration_ok();
// The net id of the player with this NpId (from the host's kIds), 0 unknown.
uint32_t net_id_of(const std::string &npid);
// Match host migration: every player keeps its net id; owner NULL = this PC hosts now.
void online_rehost(net::Link *link, const net::Address *owner, uint32_t new_host_net_id);
// Session message type of online.cpp (below session::kFrontendMessages).
constexpr uint8_t kOnlineMessage = 7;
}  // namespace u2mp::session
