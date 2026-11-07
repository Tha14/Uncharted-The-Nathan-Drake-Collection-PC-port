// u2mp: the multiplayer frontend (menu callbacks and page handlers), ported from the PS3 game
// (NPUA80698 1.10 addresses in the comments; see u2mp/docs/FUNCTIONS-FRONTEND-NET.md).
#include "u2mp_internal.h"

namespace u2mp {
namespace {

using ShowPageFn = void(U2MP_GUEST *)(void *menu, uint32_t page, uint32_t flags);

// Guest ShowPage on the guest menu manager (the frontend's own call, e.g. PAGE_PROFILE at 0x5c9bb3).
void show_page(uint32_t page) {
    reinterpret_cast<ShowPageFn>(image.base + addr::show_page)(image.base + addr::menu_manager, page, 0);
}

// BUTTON_MULTIPLAYER (PS3 0x43b150; PS4 0x5eb160 is a bare ret).
// PS3: content permission check (0x93904), then the multiplayer bootstrap 0x438bc8: NetCtl checks,
// PAGE_MULTIPLAYER_CONNECTING, the NetInit thread; on failure a message and cleanup 0x4386d0.
// Here, until the PC session layer replaces NetInit: straight to the multiplayer page.
U2MP_GUEST void multiplayer_button(void *, void *, void *) {
    log("Multiplayer selected");
    show_page(sid("PAGE_MULTIPLAYER"));
}

}  // namespace

void install_frontend(int &installed) {
    if (replace_function(addr::cb_multiplayer, 5, reinterpret_cast<void *>(&multiplayer_button),
                         "BUTTON_MULTIPLAYER callback"))
        ++installed;
}

}  // namespace u2mp
