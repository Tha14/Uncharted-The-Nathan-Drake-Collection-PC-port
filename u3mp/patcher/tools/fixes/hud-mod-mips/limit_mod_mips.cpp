// hud-mod-mips/limit_mod_mips.cpp: u3fix.exe hud-mod-mips, the weapon mod icons sharp in the in-match HUD (as on PS3).
//
// The HUD's weapon panel draws the mod icons (clip size, reload speed, damage, accuracy, ...) from overlay-mp net.pak,
// PS3's own 128x128 DXT1 textures with 8 mip levels. Measured against PS3 screenshots (2026-11-09): at the HUD's ~33 px
// the PS3 shows them at mip level ~1.6, this port at ~2.5 (the 32 and 16 px copies), while every other HUD element is as
// sharp as on the PS3; changing the UI samplers (helper v7.92-v7.95) did not reach this draw. This step stores those
// icons with their two largest levels only (VRAM_DESC mip count 8 -> 2; texel data unchanged), so no draw can sample
// below the 64 px level. Menus draw them larger and are unaffected.
//
//   u3fix hud-mod-mips --project <recipe out> [--apply] [--pak <net.pak>]
//   default: dry run on <project>/work/overlay-mp/u3data/build/main/pak23/ps4/net.pak; --apply writes it (same size, only
//   the mip count words change). Idempotent.
#include "ndpak.hpp"
#include "u3fix.hpp"

#include <cstdio>

namespace u3 {

int hud_mod_mips(const std::vector<std::string> &argv) {
    const Args a(argv, {"--apply"}, {"--project", "--pak"});
    const fs::path path = a.val.count("--pak") ? upath(a.need("--pak"))
                                               : upath(a.need("--project")) / "work" / "overlay-mp" / "u3data" / "build" / "main" / "pak23" / "ps4" / "net.pak";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) {
        std::printf("SKIP: no %s\n", utf8(path).c_str());
        return 0;
    }
    static const char *NAMES[] = {"net/clipsize.tga",   "net/reload-speed.tga", "net/major-damage.tga",       "net/major-damage_trans.tga",
                                  "net/accuracy.tga",   "net/max-ammo.tga",     "net/blindfire-accuracy.tga", "net/scope-zoom.tga"};
    const uint32_t MIPS = 2;
    const Pak p(path);
    Bytes data = p.d;
    int changed = 0, found = 0;
    for (const PakTexture &t : p.textures()) {
        const size_t k = t.path.rfind("/art/");
        const std::string name = k == std::string::npos ? t.path : t.path.substr(k + 5);
        bool ours = false;
        for (const char *n : NAMES) ours = ours || ("/" + name).find("/" + std::string(n) + "/") != std::string::npos;
        if (!ours) continue;
        found++;
        const uint64_t at = p.pbase(t.page) + t.off + 0x20 + 0x24;   // VRAM_DESC body + 0x24 = mip count
        const uint32_t cur = rd32le(data, at);
        if (cur != t.mips || t.width != 128) {
            std::printf("ERROR: unexpected descriptor for %s (mips %u / %u, width %u)\n", name.c_str(), cur, t.mips, t.width);
            return 1;
        }
        if (cur > MIPS) {
            wr32le(&data[at], MIPS);
            changed++;
        }
        std::printf("%-58s mips %u -> %u\n", name.substr(0, 58).c_str(), cur, std::min(cur, MIPS));
    }
    if (found != int(std::size(NAMES))) {
        std::printf("ERROR: %d of %zu mod icons found in %s\n", found, std::size(NAMES), utf8(path).c_str());
        return 1;
    }
    if (!changed) {
        std::printf("already applied\n");
        return 0;
    }
    if (a.has("--apply")) {
        write_file_replace(path, data);
        std::printf("written: %d textures\n", changed);
    } else
        std::printf("dry run: %d textures would change (--apply writes)\n", changed);
    return 0;
}

}  // namespace u3
