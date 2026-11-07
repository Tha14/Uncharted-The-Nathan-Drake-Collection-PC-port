// u2mp progression: NetPlayerData persistence, local leaderboards. See progression.cpp.
#pragma once

namespace u2mp {

// Installs the progression hooks (byte-checked; each one is skipped when its bytes differ).
void install_progression(int &installed);

// Writes the multiplayer progression to the local store if it changed since the last write.
// For the session layer to call when the player leaves Multiplayer (before NET_SM drops to 0, after
// which the game clears the in-memory copy, as PS3 GetVolatileData 0x4c4f74 did).
void progression_publish();

}  // namespace u2mp
