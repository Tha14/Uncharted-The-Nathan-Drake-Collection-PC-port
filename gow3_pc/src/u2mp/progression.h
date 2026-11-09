// u2mp progression: NetPlayerData persistence, local leaderboards. See progression.cpp.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace u2mp {

// Installs the progression hooks (byte-checked; each one is skipped when its bytes differ).
void install_progression(int &installed);

// Writes the multiplayer progression to the local store if it changed since the last write.
// For the session layer to call when the player leaves Multiplayer (before NET_SM drops to 0, after
// which the game clears the in-memory copy, as PS3 GetVolatileData 0x4c4f74 did).
void progression_publish();

// The players this PC met in recent matches (local store), most recently seen first, at most `max`.
std::vector<std::string> progression_recent_players(size_t max);

// clantag.cpp: BUTTON_CLANTAG's text box (NetPlayerData +0x1a8).
bool edit_clan_tag();

}  // namespace u2mp
