// u2mp netpatch: loads the server DC module net-patch9 that PS3 1.10 downloaded when going online.
//
// That module defines symbols the disc data lacks (the weapon set 0x8cf8e1cc among them); PS4 1.02 has
// neither the download nor the loader, so a match asserts on the missing symbols. The importer writes a PS4
// conversion of the module; this file reads it when Multiplayer is entered, checks it, lets the game's own
// module relocator fix up its pointers and binds its root entries with the game's own BindSymbol. Symbols
// the PS4 game already defines are kept unless UNCHARTED_U2MP_NETPATCH=all. No game code is patched.
//
// Reimplemented from the behaviour description u2mp/docs/spec/progression.md.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "u2mp_internal.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp {

void install_netpatch(int &installed);
int netpatch_load();

namespace {

// ---------------------------------------------------------------------------------------------
// Game image offsets and ids (PS4 1.02).

constexpr uint64_t kRelocateModule = 0xa19a00;
constexpr uint64_t kBindSymbol = 0xa16d70;
constexpr uint64_t kDcFind = 0xa15300;

constexpr uint64_t kSymbolBuckets = 0x15340d0;     // pointer, NULL until the script manager is up
constexpr uint64_t kSymbolFreeList = 0x15340d8;    // pointer to the first free node
constexpr uint64_t kSymbolBucketCount = 0x153410c; // s32

constexpr uint32_t kRootType = 0x916aa605;
constexpr uint32_t kScriptEntry = 0x87b03989;
constexpr uint32_t kWeaponSet = 0x8cf8e1cc;
constexpr uint32_t kOnlineWeaponSettings = 0x837a26b6;
constexpr uint32_t kOfflineWeaponSettings = 0x974cf651;

constexpr const char *kModulePath = "/app0/u2data/build/main/dc1/ps4/net-patch9.bin";

// Binding stops this many free symbol nodes short of the end, so the game keeps some for itself.
constexpr int kNodeReserve = 48;

const unsigned char kRelocateBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                        0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0x89, 0x7d, 0xd0, 0x40, 0xf6, 0xc7};
const unsigned char kBindBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                    0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x0d, 0x78, 0x3e, 0x63, 0x00};
const unsigned char kDcFindBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec,
                                      0x18, 0x4c, 0x8b, 0x05, 0xf0, 0x58, 0x63, 0x00};

// ---------------------------------------------------------------------------------------------
// Helpers.

template <typename T> T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

uint8_t *relocate_module(void *buffer) {
    return static_cast<uint8_t *>(
        reinterpret_cast<void *(U2MP_GUEST *)(void *)>(image.at<uint8_t>(kRelocateModule))(buffer));
}
void bind_symbol(uint32_t sid, const uint64_t *value) {
    reinterpret_cast<void(U2MP_GUEST *)(uint32_t, const uint64_t *)>(image.at<uint8_t>(kBindSymbol))(sid, value);
}
const uint8_t *dc_find(uint32_t sid) {
    return static_cast<const uint8_t *>(reinterpret_cast<void *(U2MP_GUEST *)(uint32_t)>(image.at<uint8_t>(kDcFind))(sid));
}

bool prologue_ok(uint64_t offset, const unsigned char *bytes, size_t length) {
    return image.executable(offset, length) && image.matches(offset, bytes, length);
}

bool read_module(std::vector<uint8_t> &data) {
    char host[1024];
    if (runtime_file_translate(kModulePath, host, sizeof host) != 0) return false;
    FILE *f = std::fopen(host, "rb");
    if (!f) return false;
    uint8_t chunk[65536];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof chunk, f)) > 0) data.insert(data.end(), chunk, chunk + got);
    std::fclose(f);
    return true;
}

// Checks that the file is a PS4 DC module whose pointers all stay inside the module data, so that the
// game's relocator cannot be led outside the block. Returns the reason for rejecting it, or NULL.
const char *validate(const std::vector<uint8_t> &file) {
    const uint64_t size = file.size();
    const uint8_t *d = file.data();
    if (size < 0x40 || std::memcmp(d, "00CD", 4) != 0) return "not a PS4 DC module ('00CD')";
    const uint64_t reloc = get<uint64_t>(d + 8);
    const uint64_t root = get<uint64_t>(d + 0x10);
    if (reloc < 0x28 || reloc + 4 > size || reloc % 8 != 0) return "bad relocation offset";
    const uint64_t bitmap_len = get<uint32_t>(d + reloc);
    if (reloc + 4 + bitmap_len > size || bitmap_len * 64 < reloc) return "bad relocation bitmap";
    if (root != 0x20 || get<uint32_t>(d + 0x1c) != kRootType) return "unexpected root table";
    const uint64_t count = get<uint32_t>(d + 0x24);
    if (0x28 + count * 16 > reloc) return "root table past the data";
    const uint8_t *bitmap = d + reloc + 4;
    for (uint64_t s = 0; s < bitmap_len * 8; ++s) {
        if (!(bitmap[s / 8] & (1u << (s % 8)))) continue;
        if (s * 8 + 8 > reloc) return "pointer slot past the data";
        if (get<uint64_t>(d + s * 8) >= reloc) return "pointer outside the module";
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Module state.

bool g_ready = false;
bool g_load_done = false;
int g_bound = 0;

}  // namespace

void install_netpatch(int &installed) {
    if (!prologue_ok(kRelocateModule, kRelocateBytes, sizeof kRelocateBytes) ||
        !prologue_ok(kBindSymbol, kBindBytes, sizeof kBindBytes) ||
        !prologue_ok(kDcFind, kDcFindBytes, sizeof kDcFindBytes)) {
        log("netpatch: RelocateModule 0x%llx / BindSymbol 0x%llx / DcFind 0x%llx differ, net-patch9 loader not installed",
            (unsigned long long)kRelocateModule, (unsigned long long)kBindSymbol, (unsigned long long)kDcFind);
        return;
    }
    g_ready = true;
    ++installed;
    log("netpatch: net-patch9 loader ready (RelocateModule 0x%llx, BindSymbol 0x%llx); loads when Multiplayer is entered",
        (unsigned long long)kRelocateModule, (unsigned long long)kBindSymbol);
}

int netpatch_load() {
    if (g_load_done) return g_bound;
    g_load_done = true;
    if (!g_ready) {
        log("netpatch: not installed (guest functions differ), net-patch9 not loaded");
        return 0;
    }

    const char *mode = std::getenv("UNCHARTED_U2MP_NETPATCH");
    const bool all = mode && std::strcmp(mode, "all") == 0;
    if (mode && std::strcmp(mode, "off") == 0) {
        log("netpatch: disabled (UNCHARTED_U2MP_NETPATCH=off)");
        return 0;
    }

    // Binding needs the game's symbol table; until the script manager made it, try again next time.
    if (get<uint64_t>(image.at<uint8_t>(kSymbolBuckets)) == 0 || get<int32_t>(image.at<uint8_t>(kSymbolBucketCount)) <= 0) {
        log("netpatch: script symbol table not set up yet, net-patch9 not loaded");
        g_load_done = false;
        return 0;
    }

    std::vector<uint8_t> file;
    if (!read_module(file)) {
        log("netpatch: %s not found (run the u2mp import with the PS3 GOTY data folder NPUA80698DATA2): weapon set "
            "0x%08x and the other server symbols stay undefined",
            kModulePath, kWeaponSet);
        return 0;
    }
    if (const char *reason = validate(file)) {
        log("netpatch: %s rejected: %s", kModulePath, reason);
        return 0;
    }

    // The game keeps pointers into the module for as long as it runs, so the block is never freed.
    const size_t size = file.size();
    uint8_t *block = static_cast<uint8_t *>(::operator new(size, std::align_val_t(16), std::nothrow));
    if (!block) {
        log("netpatch: out of memory (%zu bytes)", size);
        return 0;
    }
    std::memcpy(block, file.data(), size);

    uint8_t *root_slot = relocate_module(block);
    if (root_slot != block + 0x10 ||
        get<uint64_t>(root_slot) != uint64_t(reinterpret_cast<uintptr_t>(block + 0x20))) {
        log("netpatch: relocation failed (root slot %p)", static_cast<void *>(root_slot));
        return 0;
    }

    const uint32_t count = get<uint32_t>(block + 0x24);
    // Each new symbol takes a node from the game's free list; count how many are left (enough to decide).
    const uint64_t limit = uint64_t(count) + 64;
    int room = 0;
    for (const uint8_t *node = reinterpret_cast<const uint8_t *>(uintptr_t(get<uint64_t>(image.at<uint8_t>(kSymbolFreeList))));
         node && uint64_t(room) < limit; node = reinterpret_cast<const uint8_t *>(uintptr_t(get<uint64_t>(node))))
        ++room;

    int kept = 0, not_bound = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t sid = get<uint32_t>(block + 0x28 + 16 * uint64_t(i));
        const uint64_t value = get<uint64_t>(block + 0x30 + 16 * uint64_t(i));
        if (sid == kScriptEntry) {
            log("netpatch: entry 0x%08x is a script to run (PS3 0x151e18), not supported; skipped", sid);
            continue;
        }
        const uint8_t *have = dc_find(sid);
        if (sid == kOnlineWeaponSettings && !have) {
            // PS4 renamed the player weapon settings; the online name points at the offline map.
            if (const uint8_t *offline = dc_find(kOfflineWeaponSettings)) {
                const uint64_t copy = get<uint64_t>(offline);
                bind_symbol(kOnlineWeaponSettings, &copy);
                ++g_bound;
                log("netpatch: online weapon settings 0x%08x -> the PS4 offline map 0x%08x", kOnlineWeaponSettings,
                    kOfflineWeaponSettings);
                continue;
            }
        }
        if (have && !all) {
            ++kept;
            continue;
        }
        if (!have && g_bound >= room - kNodeReserve) {
            ++not_bound;
            continue;
        }
        const uint64_t bound_value = value;
        bind_symbol(sid, &bound_value);
        ++g_bound;
    }

    const bool weapon_set = dc_find(kWeaponSet) != nullptr;
    log("netpatch: net-patch9 loaded at %p (%zu bytes, %u symbols): %d bound, %d kept from the PS4 game%s, %d not bound "
        "(symbol pool full, %d free nodes); weapon set 0x%08x %s",
        static_cast<void *>(block), size, count, g_bound, kept, all ? "" : " (PS4 data wins)", not_bound, room,
        kWeaponSet, weapon_set ? "defined" : "STILL MISSING");
    return g_bound;
}

}  // namespace u2mp
