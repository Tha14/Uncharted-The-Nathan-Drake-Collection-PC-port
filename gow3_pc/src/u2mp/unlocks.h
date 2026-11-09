// u2mp unlocks: every multiplayer unlockable reported unlocked / owned, and the GOTY skin and booster
// tables the PS4 net.bin lacks. See unlocks.cpp and u2mp/docs/UNLOCKS.md.
#pragma once
#include <cstdint>

namespace u2mp {

// Hooks the unlock-condition check (0xa4bb40) and the co-op weapon upgrade level (0xa4d280).
void install_unlocks(int &installed);

// Binds the GOTY skin (5a4731d2) and booster (916e03b0) tables from net-patch9 over the PS4 stubs and
// lists every booster in 0x113a788. Idempotent and cheap once done; only acts while NET_SM > 1 (call it
// whenever the multiplayer menus are up: progression.cpp does, from 0x64e810).
void unlocks_refresh();

// Text of a skin name id that no text bank has (the GOTY DLC skins whose names came from the PS3 DLC
// banks): a label made from the skin's actor package name. NULL for other ids. For the TextDb miss path.
const char *unlocks_text(uint32_t id);

}  // namespace u2mp
