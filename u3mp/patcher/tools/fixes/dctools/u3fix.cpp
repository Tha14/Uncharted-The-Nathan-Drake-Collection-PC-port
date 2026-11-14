// dctools/u3fix.cpp: u3fix.exe, the C++ recipe steps of the Uncharted 3 import.
//
//   u3fix <step> [options]
//     remove-lab        --project P [--actors lab_actors.txt] [--apply]       (tools/fixes/no-lab)
//     remove-rpcn-page  --project P [--apply]                                 (tools/fixes/no-rpcn-menu)
//     netgame-order     --project P --net18 F [--apply | --verify] [...]      (tools/fixes/netgame-order)
//     emblem-cap3       --project P [--apply]                                 (tools/fixes/wall-emblem-test)
//     eula-box          --project P [--apply]                                 (tools/fixes/dctools)
//     enemy-stall       --project P [--apply | --restore] [--only-tag-on-enter]  (tools/fixes/enemy-stall)
//     coop-desc         --project P [--apply]                                 (tools/fixes/coop-chars)
//     vram-gap          --project P [--apply] [--restore] [--dir D]           (tools/fixes/coop-tab)
//     igc3-retargets    --ps4 D --job D                                       (tools/fixes/airport-igc3)
//     igc3-install      --project P --job D [--apply] [--restore]             (tools/fixes/airport-igc3)
//     text1-merge       --ps4 D --ps3-disc D --project P [--apply]           (tools/fixes/text1-merge)
//     coop-audit        --project P [--ps3-disc D] [--ps3-update D] [--ps3-data2 D] [--job D] [--apply | --restore]   (tools/fixes/coop-audit)
//     registry-levels   --project P --job J --ps4 D --ps3-update D --ps3-disc D overlay ...   (tools/fixes/registry)
//     registry-final    --project P --job J --ps4 D --ps3-update D --ps3-dlcmaps D --ps3-disc D --ps3-data2 D [--apply] [overlay ...]   (tools/fixes/pak23-registry)
//     hair-key          --project P [--also-hair-kind] [--apply] [net.bin]  (tools/fixes/hat-chloe)
//     levels            <port|r2|r3> --project P --job J --ps4 D --ps3-* D [--apply] [--only RE] [--strict] [--jobs N] [--keep-work]   (tools/fixes/pak23-levels)
//     actors            --project P --job J --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D [--apply] [--overlays ...] [--only ...] [--allow-equivalent] [--jobs N]   (tools/fixes/actor33)
//     rebuild-am        <am|hats|igc2> --project P --job J --ps4 D --ps3-disc D --ps3-update D --ps3-data2 D [--only a,b] [--jobs N]   (tools/fixes/actor33)
//     install-am        <am|hats|igc2> --project P [--work DIR] [--only a,b] [--apply | --restore]   (tools/fixes/actor33)
//     pakinfo           PAK ... [--textures] [--noitems]             (dctools/ndpak.cpp, a maintainer check)
//
// The package builds it into u3mp/bin (tools/package_windows.sh), which the importer puts on PATH; a recipe step runs
// it as ["u3fix.exe", "<step>", ...]. Every step is a dry run without --apply and exits 0 'already applied' when
// there is nothing to do; a problem prints its reason and exits 1 with nothing written.
#include "u3fix.hpp"

#include <cstdio>
#include <windows.h>
#include <tlhelp32.h>

namespace {
std::string narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace

int wmain(int argc, wchar_t **argv) {
    std::vector<std::string> a;
    for (int i = 2; i < argc; i++) a.push_back(narrow(argv[i]));
    std::string step = argc > 1 ? narrow(argv[1]) : "";
    try {
        if (step == "remove-lab") return u3::remove_lab(a);
        if (step == "remove-rpcn-page") return u3::remove_rpcn_page(a);
        if (step == "netgame-order") return u3::netgame_order(a);
        if (step == "emblem-cap3") return u3::emblem_cap3(a);
        if (step == "eula-box") return u3::eula_box(a);
        if (step == "enemy-stall") return u3::enemy_stall(a);
        if (step == "coop-desc") return u3::coop_desc(a);
        if (step == "vram-gap") return u3::vram_gap(a);
        if (step == "igc3-retargets") return u3::igc3_retargets(a);
        if (step == "igc3-install") return u3::igc3_install(a);
        if (step == "text1-merge") return u3::text1_merge(a);
        if (step == "coop-audit") return u3::coop_audit(a);
        if (step == "registry-levels") return u3::registry_levels(a);
        if (step == "registry-final") return u3::registry_final(a);
        if (step == "hair-key") return u3::hair_key(a);
        if (step == "levels") return u3::levels(a);
        if (step == "actors") return u3::actors(a);
        if (step == "rebuild-am") return u3::rebuild_am(a);
        if (step == "install-am") return u3::install_am(a);
        if (step == "convert-dc1") return u3::convert_dc1(a);
        if (step == "dc-recipes") return u3::dc_recipes(a);
        if (step == "sound-banks") return u3::sound_banks(a);
        if (step == "part-modules") return u3::part_modules(a);
        if (step == "actors-hk") return u3::actors_hk(a);
        if (step == "coop-vo") return u3::coop_vo(a);
        if (step == "coop-voice2") return u3::coop_voice2(a);
        if (step == "elim-pic") return u3::elim_pic(a);
        if (step == "classic") return u3::classic(a);
        if (step == "skins-net-names") return u3::skins_net_names(a);
        if (step == "shade-npc") return u3::shade_npc(a);
        if (step == "facility-collapse") return u3::facility_collapse(a);
        if (step == "eula-pages") return u3::eula_pages(a);
        if (step == "wall-emblem") return u3::wall_emblem(a);
        if (step == "wall-emblem-actors") return u3::wall_emblem_actors(a);
        if (step == "outdoor-jacket") return u3::outdoor_jacket(a);
        if (step == "lab-registration") return u3::lab_registration(a);
        if (step == "maps-batch-dc") return u3::maps_batch_dc(a);
        if (step == "water-jitter") return u3::water_jitter(a);
        if (step == "grass-leaf") return u3::grass_leaf(a);
        if (step == "london-fade") return u3::london_fade(a);
        if (step == "map-lighting") return u3::map_lighting(a);
        if (step == "map-colors") return u3::map_colors(a);
        if (step == "hud-mod-mips") return u3::hud_mod_mips(a);
        if (step == "pakinfo") return u3::pakinfo(a);
        std::fprintf(stderr, "usage: u3fix <step> [options] (the steps: see u3fix.cpp)\n");
        return 2;
    } catch (const u3::Stop &e) {
        std::fflush(stdout);
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    } catch (const std::exception &e) {
        std::fflush(stdout);
        std::fprintf(stderr, "%s: %s\n", step.c_str(), e.what());
        return 1;
    }
}
