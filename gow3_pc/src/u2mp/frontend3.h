// u2mp frontend3: what the frontend files share for the matchmaking port (frontend3.cpp), the
// session layer (session.cpp) and the playlists (menus.cpp). See u2mp/docs/PORT-FRONTEND3.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace u2mp {

// menuimages.cpp: a net image's material by its PS3 id (PS4 net.pak first, then net-u2mp), NULL when
// none; and a textured quad drawn into the frame's deferred 2D arena (screen units, white, alpha).
uint8_t *menu_net_material(uint32_t id);
void menu_draw_quad(uint8_t *material, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                    float alpha, float rgb = 1.0f);
// loading.cpp: the multiplayer loading screen (PS3 0x4ae638) with its card systems; see PORT-FRONTEND5.md.
void install_loading(int &installed);

// menus.cpp: the game types of the chosen matchmaking playlist (NetPlayerData+0x104). False when no
// playlist of ours is chosen.
bool playlist_game_types(std::vector<int32_t> &types);

namespace session {
// Session message types >= kFrontendMessages are frontend3's (session.cpp forwards them).
constexpr uint8_t kFrontendMessages = 0x10;
// Sends a session message (body[0] = type). slot -1: every member but `except` (host: the clients;
// a client only reaches the host, slot 0).
void send_message(int slot, const std::vector<uint8_t> &body, int except = -1);
// The net clock (PS3 [0xf116d4]+0xc): 300 ticks per second; 0 while not in Multiplayer.
uint32_t net_ticks();
// Host: creates the match of the current net game and takes the party along (what Start Match
// does in a custom game; matchmaking calls it when the vote countdown ends).
void create_match();
// The rank (badge index) a party member published; 0 when unknown.
uint32_t member_rank(uint32_t net_id);
}  // namespace session

namespace matchmaking {
// True when the matchmaking page handler is ours (PAGE_MULTIPLAYER_MATCHMAKING 0x64e910).
bool installed();
// Party leader, Start Match in matchmaking mode: PS3 0x43aab0 -> START_MATCHMAKING 0x43e99c.
void leader_start();
// A session message of type >= session::kFrontendMessages from transport slot `from`.
void on_message(uint8_t from, const uint8_t *data, size_t size);
}  // namespace matchmaking

}  // namespace u2mp
