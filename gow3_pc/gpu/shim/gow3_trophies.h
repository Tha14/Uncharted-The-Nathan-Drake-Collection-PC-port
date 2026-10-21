// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 21 Oct 2026: trophies. The runtime (runtime_services.c) saves what a game earns and passes
// the running game's list here: its trophy pack (sce_sys/trophy/trophyNN.trp, a host path), the
// list number and the earned flags. Pictures come from the pack (plain PNG files); the names are
// encrypted with a console key and are not read. A newly earned trophy shows as a popup
// (Gow3Overlay::ShowTrophy) with a short chime; the menu's Trophies tab lists them.

#pragma once

#include <string>
#include <vector>
#include "common/types.h"
#include "gow3_png.h"

namespace Gow3Trophies {

/// The running game's list: the pack's host path, its number (1-3) and the earned flags.
void SetList(const std::string& pack_path, int list, const u32 earned[4]);
/// A newly earned trophy: popup and chime.
void Earned(int id);

struct Summary {
    std::string game;          ///< empty: no list yet
    int list = 0;
    std::vector<int> ids;      ///< the pack's trophies (TROPnnn.PNG), in order
    std::vector<bool> earned;  ///< per entry of ids
};
Summary List();
/// A trophy's picture (TROPnnn.PNG of the current pack); false when there is none.
bool Picture(int id, Gow3Png::Image& out);

} // namespace Gow3Trophies
