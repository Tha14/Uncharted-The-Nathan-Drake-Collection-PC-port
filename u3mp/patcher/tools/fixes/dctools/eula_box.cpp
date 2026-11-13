// Recipe step eula-menu-box: the six EULA page boxes of overlay-r3 menu.bin.   u3fix eula-box --project P [--apply]
//
// Six u16 layout values (the EULA page boxes) go from 100 to 200, as written on 2026-11-01 together with the EULA page
// textures. The offsets lie in the original part of menu.bin (later steps only append), so they hold for the converted
// menu.bin whatever else changed. All six 200: 'already applied'. Anything other than six 100s stops the step. Dry run
// without --apply; --apply backs menu.bin up once to work/backup-eula-box/. Replaces eula_box.py (same messages).
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {

int eula_box(const std::vector<std::string> &argv) {
    static const uint64_t OFFS[] = {0xdabc, 0xddfc, 0xe004, 0xe20c, 0xe414, 0xe61c};
    const Args a(argv, {"--apply"}, {"--project"});
    const std::string project = a.need("--project");
    const fs::path menu = upath(project + "/work/overlay-r3/u3data/build/main/dc1/ps4/menu.bin");
    const fs::path bk = upath(project + "/work/backup-eula-box/");
    Bytes m = read_file(menu);
    bool all200 = true, all100 = true;
    std::string vals;
    for (uint64_t o : OFFS) {
        const uint16_t v = rd16le(m, o);
        all200 = all200 && v == 200;
        all100 = all100 && v == 100;
        vals += (vals.empty() ? "" : ", ") + fmt("'%s=%u'", pyhex(o).c_str(), v);
    }
    if (all200) {
        std::printf("already applied: EULA boxes are 200\n");
        return 0;
    }
    if (!all100) throw Stop("AssertionError: [" + vals + "]");
    for (uint64_t o : OFFS) {
        m[o] = 200;
        m[o + 1] = 0;
    }
    if (!a.has("--apply")) {
        std::printf("dry run: 6 values would change\n");
        return 0;
    }
    fs::create_directories(bk);
    if (!fs::exists(bk / "menu.bin")) copy2(menu, bk / "menu.bin");
    write_file_replace(menu, m);
    std::printf("box ok\n");
    return 0;
}

}  // namespace u3
