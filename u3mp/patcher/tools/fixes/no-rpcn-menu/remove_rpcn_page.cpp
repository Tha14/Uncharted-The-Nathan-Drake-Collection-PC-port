// Recipe step remove-rpcn-page (recipe 1.16.0): no RPCN page in the game's menus.   u3fix remove-rpcn-page --project P [--apply]
//
// Local builds used to add a menu page 0x52504e01 to overlay-r3 menu.bin (a copy of the Options page that the
// multiplayer front end opened with L2). The launcher now signs in and shows the RPCN section, the helper (v7.105)
// never opens the page, and the step that added it is gone. This step takes the page out of older imports: the page
// table (DC symbol 0x74ffceeb, 0x80-byte entries, page id at +8) is rewritten without it (append-only: a new array,
// the old bytes stay), and the menu .bin size word of modules.bin (module found by name) follows the new size.
// Dry run by default; --apply writes (menu.bin and modules.bin backed up once to work/backup-no-rpcn-page/).
// Idempotent: 'already applied' when menu.bin has no such page.
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {

int remove_rpcn_page(const std::vector<std::string> &argv) {
    constexpr uint32_t PAGES = 0x74ffceeb, PAGE = 0x80, PAGE_ID = 0x52504e01;
    Args a(argv, {"--apply"}, {"--project"});
    std::string project = a.need("--project");
    fs::path dir = upath(project + "/work/overlay-r3/u3data/build/main/dc1/ps4/");
    fs::path bk = upath(project + "/work/backup-no-rpcn-page/");
    fs::path menu = dir / "menu.bin", mods = dir / "modules.bin";

    Dc c(menu);
    std::vector<uint64_t> found;
    for (const Sym &s : c.syms)
        if (s.sid == PAGES) found.push_back(s.off);
    if (found.size() != 1) throw Stop(fmt("menu.bin: page table symbol %08x found %d times", PAGES, int(found.size())));
    uint64_t o = found[0];
    uint32_t n = c.u32(o);
    uint64_t arr = c.ptr(o + 8);
    std::vector<uint32_t> keep;
    bool has = false;
    for (uint32_t k = 0; k < n; k++) {
        if (c.u32(arr + uint64_t(k) * PAGE + 8) == PAGE_ID) has = true;
        else keep.push_back(k);
    }
    if (!has) {
        std::printf("already applied: menu.bin has no page %08x (%u pages)\n", PAGE_ID, n);
        return 0;
    }
    DcEdit e(menu);
    uint64_t na = e.alloc(size_t(PAGE) * keep.size(), 16);
    for (size_t j = 0; j < keep.size(); j++) e.copy_words(arr + uint64_t(keep[j]) * PAGE, na + j * PAGE, PAGE);
    e.put32(o, uint32_t(keep.size()));
    e.putptr(o + 8, na);
    Bytes out = e.build();

    uint64_t slot = module_size_slot(Dc(mods), "menu");
    Bytes md = read_file(mods);
    uint64_t old = rd64le(&md[slot]);
    uint64_t menu_size = fs::file_size(menu);
    if (old != menu_size)
        throw Stop(fmt("modules.bin: menu size %llu is not menu.bin's %llu", (unsigned long long)old, (unsigned long long)menu_size));
    wr64le(&md[slot], out.size());
    std::printf("page %08x removed: %u -> %d pages; menu.bin %llu -> %llu bytes, modules.bin menu size updated\n", PAGE_ID, n,
                int(keep.size()), (unsigned long long)old, (unsigned long long)out.size());
    if (!a.has("--apply")) {
        std::printf("dry run: nothing written\n");
        return 0;
    }
    fs::create_directories(bk);
    copy_once(menu, bk / "menu.bin");
    copy_once(mods, bk / "modules.bin");
    write_file_replace(menu, out);
    write_file_replace(mods, md);
    std::printf("applied\n");
    return 0;
}

}  // namespace u3
