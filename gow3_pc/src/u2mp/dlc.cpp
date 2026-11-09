// u2mp DLC maps: Facility and DLC Ruins (map pack 1), Highrise, Museum and Museum-EVA (map pack 2)
// are offered in the custom game map list and picked by matchmaking only when the import made their
// files. Static analysis only (the game was not run). Notes: u2mp/docs/DLC.md.
//
// How the PS4 1.02 executable decides (addresses are PS4 image offsets):
//
// 1. Entitlement list, g_entitlements at 0x1244e14 ("ndlib\net\entitlements.cpp").
//    Refresh 0xb887c0 (called by game-loading init 0x3f76b2, and by 0x3fbccc when +0x2c88 is set;
//    the addcont event handler 0xb892c0 sets it). It returns early for three region ids in
//    [0x1059640+0x39a8], else asks sceAppContentGetAddcontInfoList (stub 0xde7890) for the title's
//    add-ons (0x18-byte SceAppContentAddcontInfo: 17-byte label, status at +0x14):
//      status 0 (entitlement only) or 4 (installed) -> label copied to the list: count +0,
//                                     records {char label[16]; u32} of 0x14 bytes from +4;
//      status 4                     -> also sceAppContentAddcontMount (0xde78a0) into +0x2808
//                                     (0x24-byte records, count +0x2804), then MountDLCArchives
//                                     0xb88ba0(this, mount point);
//      status 1                     -> 0xde78b0 (the other AppContent import).
//    MountDLCArchives 0xb88ba0: for i = 1..8, unless bit i-1 of +0x2c8c is set, sprintf
//    "%s/dlc-map%d.psarc" (mount point, i) and mount it with 0xb72f00 on the file system 0x1115a30
//    (table 0x15d29b0, g_NumDLCArchives 0x15d2a70, assert "g_NumDLCArchives < kMaxDLCArchives" at 8).
//    PS3 1.10 had the same archives as build/main/dlc-map%i.psarc plus promo1/map-dlc%i.edat
//    licences (NetInit area 0x4e7e58 / 0x4e83c8).
//    On PC: the runtime's AppContent provider (runtime_content.c) reports zero add-ons, so the list
//    stays empty, AddcontMount (not even resolved by the runtime) is never called and
//    MountDLCArchives is never reached. The import writes loose files that the /app0 overlay
//    serves directly, so there is no dlc-map psarc to mount and nothing to suppress or redirect.
//
// 2. Readers of the entitlement list: only the unlock-condition evaluator 0xa4bcb0 (bp_ucctweaks
//    area), condition type 12 = "entitlement": the condition's string (+0x10) without its last 5
//    characters (".edat") must equal a label in the list, or the tweak byte "Fake DLC Entitlement"
//    ([0x172b9c0] + 0x2da3, debug menu 0xd4bd60) must be set. 0xa4bb40 evaluates an item's condition
//    list (item +0x18 -> {count, conditions of 0x18 bytes, any/all +0x10}). In the PS4 dc data only
//    five store.bin costume items use type 12 (label NATHANDRAKEPACK1.edat); no net.bin item, no map.
//    The helper 0xb88da0 (same test on a list) has no callers. GetDlcRequired (0x5ab090, the trophy
//    record's +0x20, net-player-data.cpp) only describes trophies. The net.bin licence table
//    (sid 0xf5899ce4: golden-weapons.edat .. dlc1.edat, skin-dlc2/3.edat with bits 0x10..0x8000) is a
//    PS3 leftover: no PS4 code looks it up.
//
// 3. Map table, dc symbol 0x0dec97a4 in net.bin: {s32 count; rec*} at +8, 25 records of 0x38 bytes
//    {+0 map id, +4/+8/+0xc text and image ids, +0x10 s32 map pack, +0x18 pak name, +0x20 pak id,
//    +0x30 co-op, +0x34 ...}. Map pack 1 = facility (bdb1db2a, 7069f592) and ruins (d9be0a6d,
//    37be4d69); map pack 2 = highrise (fde0cbdd, b89e363e) and museum (6577a65a, ee1d78ce); 0 = base.
//    *net-games* (0x3b9a067d) records name their map by id at +4.
//    The only PS4 reader of the map pack field is GetRandomNetGameIndexPair 0x64a440 (dead on PS4):
//    a game whose map has pack != 0 gets weight 0. Its PS3 counterpart 0x49bdb8 (0x49c1a8..0x49c300)
//    kept such a game only when every session member's DLC flags (exchanged in the member data)
//    included the pack; PS4 folded that to "never".
//    The custom game lists are built by 0x654c80 (callers 0x656550, tail calls from 0x655250,
//    0x655390, 0x655540): for every game type of *net-games* a row of 0x790 bytes, for every map of
//    that type a slot of 0x60 bytes (+0x18 map record found by id in 0x0dec97a4, +0x20 game count,
//    +0x24 game indices). There is no DLC check: a game whose map id is not in the map table gets no
//    slot, everything else is listed. So PS4 shows the DLC maps whether or not their data exists.
//    Matchmaking on PS4 is a stub (0x64e910); on PC session.cpp picks the game.
//
// What this file does:
//  * Map pack 1 / 2 count as present when the converted files exist (through the runtime's /app0
//    overlay resolution) and the level registry names them (see kPacks below).
//  * The map table lookup inside 0x654c80 (call at 0x654f98 to the dc lookup 0xa15e70) gets a copy
//    of the map table without the records of absent packs: their games get no slot, as for an
//    unknown map, so the custom game map list only offers present maps.
//  * dlc_net_game_available(index) for the C++ that picks net games itself (session.cpp matchmaking,
//    menus.cpp playlists).
// No entitlement is faked: nothing on PS4 gates the maps by entitlement.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include "u2mp_internal.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp {
namespace {

constexpr uint64_t fDcLookup = 0xa15e70;     // DC symbol by string id
constexpr uint32_t kNetGames = 0x3b9a067d;   // *net-games*: record stride 0x90, map id at +4
constexpr uint32_t kNetMaps = 0x0dec97a4;    // map table: record stride 0x38, map pack at +0x10
constexpr uint64_t NG_STRIDE = 0x90, NG_MAP = 0x04, MAP_STRIDE = 0x38, MAP_PACK = 0x10;
constexpr uint64_t kListMapLookup = 0x654f98;   // call 0xa15e70 in the custom game list builder 0x654c80

struct DcTable { int32_t count; int32_t pad; uint8_t *records; };
using LookupFn = void *(U2MP_GUEST *)(uint32_t);

void *dc_lookup(uint32_t id) { return reinterpret_cast<LookupFn>(image.base + fDcLookup)(id); }

// ---- which packs the import produced -----------------------------------------------------------
constexpr const char *kRoot = "/app0/u2data/build/main/";
struct Level { const char *name; bool ingame; };   // ingame: <name>-ingame10.pak must exist too
struct Pack { const char *label; Level levels[3]; int count; };
// Map pack 1: Facility + the map pack version of Ruins (its new wide level). Map pack 2: Highrise,
// Museum, Museum-EVA. Each level: pak22/ps4/<name>.pak (+ -ingame10.pak) and a registry entry.
const Pack kPacks[3] = {
    {"base", {}, 0},
    {"map pack 1", {{"facility-multi", true}, {"ruins-multi-wide", false}}, 2},
    {"map pack 2", {{"highrise-multi-2", true}, {"museum-multi", true}, {"museum-multi-eva", true}}, 3},
};
bool pack_present[3] = {true, false, false};
bool packs_checked = false;

bool guest_file_exists(const std::string &guest) {
    char host[1024];
    if (runtime_file_translate(guest.c_str(), host, sizeof host) != 0) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(host), ec);
}

std::string read_guest_file(const std::string &guest) {
    char host[1024];
    std::string out;
    if (runtime_file_translate(guest.c_str(), host, sizeof host) != 0) return out;
    std::FILE *f = std::fopen(host, "rb");
    if (!f) return out;
    char buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) out.append(buffer, n);
    std::fclose(f);
    return out;
}

// A registry line "<key> <name>" (retail writes "level-name X " with a trailing space). The main
// levels need their own level entry; the ruins wide level may also come in as a package line.
bool registry_names(const std::string &registry, const char *name, bool level_only) {
    const char *keys[] = {"level-name ", "package "};
    for (int k = 0; k < (level_only ? 1 : 2); ++k) {
        const std::string needle = std::string(keys[k]) + name;
        for (size_t at = registry.find(needle); at != std::string::npos; at = registry.find(needle, at + 1)) {
            const bool line_start = at == 0 || registry[at - 1] == '\n';
            const size_t end = at + needle.size();
            const bool word_end = end == registry.size() || registry[end] == ' ' || registry[end] == '\r' ||
                                  registry[end] == '\n' || registry[end] == '\t';
            if (line_start && word_end) return true;
        }
    }
    return false;
}

void check_packs() {
    if (packs_checked) return;
    packs_checked = true;
    const std::string registry = read_guest_file(std::string(kRoot) + "pak22_ps4.txt");
    for (int p = 1; p < 3; ++p) {
        bool ok = true;
        std::string missing;
        for (int l = 0; l < kPacks[p].count; ++l) {
            const Level &lv = kPacks[p].levels[l];
            const std::string pak = std::string(kRoot) + "pak22/ps4/" + lv.name;
            const bool have = guest_file_exists(pak + ".pak") && (!lv.ingame || guest_file_exists(pak + "-ingame10.pak")) &&
                              registry_names(registry, lv.name, lv.ingame);
            if (!have) {
                ok = false;
                missing += missing.empty() ? lv.name : std::string(", ") + lv.name;
            }
        }
        pack_present[p] = ok;
        if (ok) log("dlc: %s imported, its maps are offered", kPacks[p].label);
        else log("dlc: %s not imported (missing or unregistered: %s), its maps are hidden", kPacks[p].label,
                 missing.c_str());
    }
}

bool pack_available(int32_t pack) {
    check_packs();
    return pack >= 0 && pack < 3 && pack_present[pack];
}

// Ruins (map ids d9be0a6d, 37be4d69) is marked map pack 1 in net.bin, but the base game ships it: the PS4
// game and GOTY both have the base ruins-multi with its spawn pak. Without the map pack it is offered as
// the base map (assets audit 2026-11-09, GOTY-only import); with the map pack it is the flooded one.
bool map_available(const uint8_t *rec) {
    const uint32_t map = *reinterpret_cast<const uint32_t *>(rec);
    const int32_t pack = *reinterpret_cast<const int32_t *>(rec + MAP_PACK);
    if (pack_available(pack)) return true;
    if (map == 0xd9be0a6du || map == 0x37be4d69u) {
        static int base_ruins = -1;
        if (base_ruins < 0) {
            base_ruins = guest_file_exists(std::string(kRoot) + "pak22/ps4/ruins-multi.pak") ? 1 : 0;
            log("dlc: Ruins %s as the base map (map pack 1 not imported)", base_ruins ? "offered" : "hidden");
        }
        return base_ruins == 1;
    }
    return false;
}

// ---- the map table without the absent packs (for 0x654c80) -------------------------------------
// The builder stores pointers to the records it finds (slot +0x18) and the menu reads them later,
// so the copy lives in static memory and is rewritten in place on every lookup: same dc data, same
// contents at the same addresses.
constexpr int kMaxMaps = 64;
alignas(16) uint8_t filtered_records[kMaxMaps * MAP_STRIDE];
DcTable filtered_table{};

U2MP_GUEST void *list_map_lookup(uint32_t id) {
    void *found = dc_lookup(id);
    if (id != kNetMaps || !found) return found;
    auto *table = static_cast<DcTable *>(found);
    if (table->count <= 0 || !table->records || table->count > kMaxMaps) return found;
    int kept = 0;
    for (int32_t i = 0; i < table->count; ++i) {
        const uint8_t *rec = table->records + MAP_STRIDE * uint64_t(i);
        if (!map_available(rec)) continue;
        std::memcpy(filtered_records + MAP_STRIDE * uint64_t(kept), rec, MAP_STRIDE);
        ++kept;
    }
    if (kept == table->count) return found;
    filtered_table.count = kept;
    filtered_table.records = filtered_records;
    return &filtered_table;
}

}  // namespace

bool dlc_net_game_available(int32_t index) {
    auto *games = static_cast<DcTable *>(dc_lookup(kNetGames));
    if (!games || !games->records || index < 0 || index >= games->count) return true;
    const uint32_t map = *reinterpret_cast<const uint32_t *>(games->records + NG_STRIDE * uint64_t(index) + NG_MAP);
    auto *maps = static_cast<DcTable *>(dc_lookup(kNetMaps));
    if (!maps || !maps->records) return true;
    for (int32_t i = 0; i < maps->count; ++i) {
        const uint8_t *rec = maps->records + MAP_STRIDE * uint64_t(i);
        if (*reinterpret_cast<const uint32_t *>(rec) == map)
            return map_available(rec);
    }
    return true;   // unknown map: the game itself decides (0x654c80 skips it)
}

void install_dlc(int &installed) {
    check_packs();
    if (!image.executable(fDcLookup, 16)) {
        log("dlc: dc lookup 0x%llx not executable, map list filter not installed", (unsigned long long)fDcLookup);
        return;
    }
    // 0x654f8a: mov dword [rbx+r13+0x20], 0; mov edi, 0x0dec97a4; call 0xa15e70 (0x654f98);
    // then mov ecx, [rax]; test ecx, ecx; jle; lea rsi, [rbx+r13+0x20]; mov rax, ...
    static const unsigned char before[] = {0x42, 0xc7, 0x44, 0x2b, 0x20, 0x00, 0x00, 0x00, 0x00,
                                           0xbf, 0xa4, 0x97, 0xec, 0x0d};
    static const unsigned char after[] = {0x8b, 0x08, 0x85, 0xc9, 0x7e, 0x20, 0x4a, 0x8d, 0x74, 0x2b, 0x20, 0x48, 0x8b};
    if (!image.matches(kListMapLookup - sizeof before, before, sizeof before) ||
        !image.matches(kListMapLookup + 5, after, sizeof after)) {
        log("dlc: custom game list builder 0x654c80 differs at 0x%llx, map list filter not installed",
            (unsigned long long)kListMapLookup);
        return;
    }
    if (retarget_call(kListMapLookup, fDcLookup, reinterpret_cast<void *>(&list_map_lookup),
                      "dlc: custom game list map lookup"))
        ++installed;
}

}  // namespace u2mp
