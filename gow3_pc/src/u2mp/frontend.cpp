// u2mp: frontend helpers shared by the session layer, and the module's install order.
#include "u2mp_internal.h"

namespace u2mp {

namespace session { void install(int &installed); }

using ShowPageFn = void(U2MP_GUEST *)(void *menu, uint32_t page, uint32_t flags);

// Guest ShowPage on the guest menu manager (the frontend's own call, e.g. PAGE_PROFILE at 0x5c9bb3).
void show_page(uint32_t page) {
    reinterpret_cast<ShowPageFn>(image.base + addr::show_page)(image.base + addr::menu_manager, page, 0);
}

void install_frontend(int &installed) {
    session::install(installed);
}

}  // namespace u2mp
