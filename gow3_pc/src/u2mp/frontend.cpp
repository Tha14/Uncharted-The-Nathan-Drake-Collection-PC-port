// u2mp frontend: the guest page opener that many files of the module use, and the frontend install
// entry point (which only hands over to the session layer).
//
// Reimplemented from the behaviour description u2mp/docs/spec/frontend.md.

#include "u2mp_internal.h"

namespace u2mp {

namespace session {
// session.cpp: installs the party hooks. It has no header of its own.
void install(int &installed);
}  // namespace session

void show_page(uint32_t page) {
    // The game opens pages through its menu manager; flags 0 is the plain "push this page" request.
    using ShowPageFn = void(U2MP_GUEST *)(void *menu, uint32_t page, uint32_t flags);
    auto show = reinterpret_cast<ShowPageFn>(image.base + addr::show_page);
    show(image.base + addr::menu_manager, page, 0);
}

void install_frontend(int &installed) {
    // The frontend's own hooks live in frontend2/3/6; this step only brings up the session layer.
    session::install(installed);
}

}  // namespace u2mp
