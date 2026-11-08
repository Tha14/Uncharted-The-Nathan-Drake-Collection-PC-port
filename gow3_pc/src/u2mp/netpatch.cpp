// u2mp netpatch: the PS3 server "live data" DC module net-patch9.bin, loaded the way PS3 1.10 did.
//
// Why: test run 33 reached Playing on Gompa and asserted "LookupSymbolValue: Symbol '0x8cf8e1cc' not
// found" (DC lookup 0xa15e70, assert 0xa15f25). 0x8cf8e1cc is a weapon set (type header 0x1755b0c7 =
// sid("weapon-set")): the *net-games* record +0x30 names it and the spawn code asks for it (PS4 0x6634a2
// compares the record's set with 0x8cf8e1cc, then the loadout code looks it up). The PS4 dc1/ps4/net.bin
// only references the id; the definition is in net-patch9.bin, which PS3 1.10 downloaded from the server.
//
// PS3 1.10 GOTY (NPUA80698) map:
//   0x447a5c NetInit thread (net-livedata, "u2livedata.final1.prod"): downloads "net-patch9.bin" with
//            0x4e7e58 into the game data folder (<title>DATA2/USRDIR), then 0x4e83c8("%s/%s" path).
//   0x4e83c8 LoadNetPatch(path): 0x910c10 alloc 256 KiB (heap 9, align 16); 0x4e7988 read the file;
//            0x156840 relocate ("Invalid magic number", "Script is not 16 byte aligned") -> slot of the
//            root; root = {u32 1, u32 count, {u32 sid, u32 ptr}[count]} (sorted); LookupSymbolValue
//            (*net-version* 0x6f1c3f87) + 0x14d088 find it in the module (result unused); then every entry
//            except 0x87b03989 goes to 0x151aa8 BindSymbol(sid, &ptr) (hash map 0xeb98d0, FNV-1a of the
//            sid), and an 0x87b03989 entry (not present in net-patch9) would be run as a script (0x151e18).
//   0x151d4c LookupSymbolValue -> 0x151bd8: sorted native table first, then the bound-symbol hash map.
//            DC modules bind their symbols the same way (module loader 0x156bb8 -> 0x151aa8).
// PS4 1.02 equivalents (net-livedata and LoadNetPatch are gone):
//   0xa19a00 RelocateModule(buf): 16-byte aligned, '00CD', reloc u32 at +8, bitmap {u32 bytes, bits}
//            at buf+reloc, adds buf to every marked 8-byte slot, returns buf+0x10 (slot of the root).
//   0xa16d70 BindSymbol(u32 sid, const u64 *value): hash map buckets *0x15340d0, count 0x153410c,
//            free list 0x15340d8 (pool of 0x1000 nodes {next, prev, sid +0x10, value +0x18}); an existing
//            node of the same sid gets the new value. Asserts when the pool is empty.
//   0xa15300 DcFind(sid): sorted native table *0x1534200, then the hash map; slot or NULL (no assert).
//   0xa15e70 LookupSymbolValue = *DcFind or assert.
//   The startup modules (name list 0x1044800, "%s/build/main/dc%u/ps4" + "%s/%s.bin", loop 0xa17b20)
//   are loaded and bound at boot, so net.bin's symbols exist when Multiplayer is entered.
//
// What this file does: netpatch_load() reads /app0/u2data/build/main/dc1/ps4/net-patch9.bin (written by
// u2mp_import.py from the player's PS3 GOTY data folder, u2mp/patcher/u2conv/u2dc.py), checks it,
// relocates it with 0xa19a00 and binds each root entry with 0xa16d70 - only the symbols the PS4 game does
// not define (DcFind NULL), so the PS4 tables (*net-games* with its PS4 record layout, levels ...) stay the
// PS4 ones. UNCHARTED_U2MP_NETPATCH=all binds every entry (PS3 behaviour: the server data replaces the
// disc data), =off disables it. If the file is missing or bad it logs and the game continues as before.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include "u2mp_internal.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp {

void install_netpatch(int &installed);
// Load and bind net-patch9 (idempotent). Call when Multiplayer is entered, before any match starts
// (session.cpp multiplayer_button, next to netinit_prepare()). Returns the number of symbols bound.
int netpatch_load();

namespace {

constexpr uint64_t fRelocate = 0xa19a00;      // PS3 0x156840
constexpr uint64_t fBindSymbol = 0xa16d70;    // PS3 0x151aa8
constexpr uint64_t fDcFind = 0xa15300;        // PS3 0x151bd8
constexpr uint64_t gHashBuckets = 0x15340d0;  // u64 *buckets (NULL before the script manager init)
constexpr uint64_t gHashFree = 0x15340d8;     // free node list
constexpr uint64_t gHashCount = 0x153410c;    // s32 bucket count
constexpr uint32_t kRootType = 0x916aa605;    // type header of the module root table
constexpr uint32_t kRunScript = 0x87b03989;   // PS3: this entry is run, not bound
constexpr uint32_t kWeaponSet = 0x8cf8e1cc;   // the symbol test run 33 missed
constexpr uint32_t kOnlineWeaponSettings = 0x837a26b6;   // read online by 0xada5e0 (net-patch9 only)
constexpr uint32_t kOfflineWeaponSettings = 0x974cf651;  // PS4 player-weapon-settings.bin
constexpr const char *kPath = "/app0/u2data/build/main/dc1/ps4/net-patch9.bin";

using RelocateFn = void **(U2MP_GUEST *)(void *);
using BindFn = void(U2MP_GUEST *)(uint32_t, const uint64_t *);
using FindFn = void **(U2MP_GUEST *)(uint32_t);

const unsigned char kRelocateBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                        0x53, 0x48, 0x83, 0xec, 0x18, 0x48, 0x89, 0x7d, 0xd0, 0x40, 0xf6, 0xc7};
const unsigned char kBindBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                    0x53, 0x48, 0x83, 0xec, 0x18, 0x4c, 0x8b, 0x0d, 0x78, 0x3e, 0x63, 0x00};
const unsigned char kFindBytes[] = {0x55, 0x48, 0x89, 0xe5, 0x53, 0x48, 0x83, 0xec,
                                    0x18, 0x4c, 0x8b, 0x05, 0xf0, 0x58, 0x63, 0x00};

bool g_ready = false;     // install_netpatch verified the three guest functions
bool g_done = false;      // netpatch_load ran (success or not)
int g_bound = 0;

template <typename T> T guest(uint64_t off) { return reinterpret_cast<T>(image.base + off); }

uint32_t rd32(const unsigned char *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint64_t rd64(const unsigned char *p) { uint64_t v; std::memcpy(&v, p, 8); return v; }

bool read_file(std::string &out) {
    char host[1024];
    if (runtime_file_translate(kPath, host, sizeof host) != 0) return false;
    std::FILE *f = std::fopen(host, "rb");
    if (!f) return false;
    char buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) out.append(buffer, n);
    std::fclose(f);
    return true;
}

// The checks 0xa19a00 asserts on, plus the ones it does not make (sizes, root table, pointer range).
const char *check(const unsigned char *d, size_t size) {
    if (size < 0x40 || std::memcmp(d, "00CD", 4) != 0) return "not a PS4 DC module ('00CD')";
    const uint64_t reloc = rd64(d + 8), root = rd64(d + 0x10);
    if (reloc < 0x28 || reloc + 4 > size || reloc % 8) return "bad relocation offset";
    const uint32_t bytes = rd32(d + reloc);
    if (reloc + 4 + bytes > size || uint64_t(bytes) * 64 < reloc) return "bad relocation bitmap";
    if (root != 0x20 || rd32(d + 0x1c) != kRootType) return "unexpected root table";
    const uint32_t count = rd32(d + 0x24);
    if (0x28 + uint64_t(count) * 16 > reloc) return "root table past the data";
    const unsigned char *bm = d + reloc + 4;
    for (uint64_t slot = 0; slot < uint64_t(bytes) * 8; ++slot) {
        if (!(bm[slot / 8] >> (slot % 8) & 1)) continue;
        if (slot * 8 + 8 > reloc) return "pointer slot past the data";
        const uint64_t v = rd64(d + slot * 8);
        if (v >= reloc) return "pointer outside the module";
    }
    return nullptr;
}

// Nodes left in the symbol hash map's pool (BindSymbol asserts when it is empty).
int free_nodes(int limit) {
    int n = 0;
    for (auto *p = *image.at<uint64_t *>(gHashFree); p && n < limit; p = reinterpret_cast<uint64_t *>(*p)) ++n;
    return n;
}

}  // namespace

int netpatch_load() {
    if (g_done) return g_bound;
    g_done = true;
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
    if (!*image.at<void *>(gHashBuckets) || *image.at<int32_t>(gHashCount) <= 0) {
        log("netpatch: script symbol table not set up yet, net-patch9 not loaded");
        g_done = false;   // try again on the next call
        return 0;
    }
    std::string file;
    if (!read_file(file)) {
        log("netpatch: %s not found (run the u2mp import with the PS3 GOTY data folder NPUA80698DATA2): "
            "weapon set 0x%08x and the other server symbols stay undefined", kPath, kWeaponSet);
        return 0;
    }
    if (const char *why = check(reinterpret_cast<const unsigned char *>(file.data()), file.size())) {
        log("netpatch: %s rejected: %s", kPath, why);
        return 0;
    }
    // PS3 kept the module in a 16-aligned heap block for the whole game; so do we (never freed).
    auto *buf = static_cast<unsigned char *>(::operator new(file.size(), std::align_val_t(16), std::nothrow));
    if (!buf) {
        log("netpatch: out of memory (%zu bytes)", file.size());
        return 0;
    }
    std::memcpy(buf, file.data(), file.size());
    void **root_slot = guest<RelocateFn>(fRelocate)(buf);
    if (root_slot != reinterpret_cast<void **>(buf + 0x10) || *root_slot != buf + 0x20) {
        log("netpatch: relocation failed (root slot %p)", static_cast<void *>(root_slot));
        return 0;
    }
    const unsigned char *root = buf + 0x20;
    const uint32_t count = rd32(root + 4);
    const int room = free_nodes(int(count) + 64);
    auto find = guest<FindFn>(fDcFind);
    auto bind = guest<BindFn>(fBindSymbol);
    int kept = 0, skipped_room = 0;
    bool weapon_set = false;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *e = root + 8 + 16 * i;
        const uint32_t id = rd32(e);
        const uint64_t value = rd64(e + 8);   // already relocated: an address inside buf
        if (id == kRunScript) {
            log("netpatch: entry 0x%08x is a script to run (PS3 0x151e18), not supported; skipped", id);
            continue;
        }
        void **have = find(id);
        // Test run 34: the online player weapon settings map (0x837a26b6, read when NET_SM > 1 by 0xada5e0)
        // converts only partly (its large records fall to the generic layout: field +0x2c0 read as -1).
        // Its records have the layout of the offline map 0x974cf651 that the PS4 player-weapon-settings.bin
        // defines correctly, so the online id is bound to the PS4 offline map (single-player tuning).
        if (id == kOnlineWeaponSettings && !have) {
            if (void **offline = find(kOfflineWeaponSettings)) {
                const uint64_t v = *reinterpret_cast<uint64_t *>(offline);
                bind(id, &v);
                ++g_bound;
                log("netpatch: online weapon settings 0x%08x -> the PS4 offline map 0x%08x", id, kOfflineWeaponSettings);
                continue;
            }
        }
        if (have && !all) {
            ++kept;   // the PS4 game defines it: keep the PS4 data
            continue;
        }
        // keep a margin of the pool for the modules the game loads later
        if (!have && g_bound >= room - 48) {
            ++skipped_room;
            continue;
        }
        bind(id, &value);
        ++g_bound;
        if (id == kWeaponSet) weapon_set = true;
    }
    log("netpatch: net-patch9 loaded at %p (%zu bytes, %u symbols): %d bound, %d kept from the PS4 game%s, "
        "%d not bound (symbol pool full, %d free nodes); weapon set 0x%08x %s",
        static_cast<void *>(buf), file.size(), count, g_bound, kept, all ? "" : " (PS4 data wins)", skipped_room,
        room, kWeaponSet, find(kWeaponSet) ? "defined" : "STILL MISSING");
    (void)weapon_set;
    return g_bound;
}

void install_netpatch(int &installed) {
    const bool ok = image.executable(fRelocate, sizeof kRelocateBytes) &&
                    image.matches(fRelocate, kRelocateBytes, sizeof kRelocateBytes) &&
                    image.executable(fBindSymbol, sizeof kBindBytes) &&
                    image.matches(fBindSymbol, kBindBytes, sizeof kBindBytes) &&
                    image.executable(fDcFind, sizeof kFindBytes) && image.matches(fDcFind, kFindBytes, sizeof kFindBytes);
    if (!ok) {
        log("netpatch: RelocateModule 0x%llx / BindSymbol 0x%llx / DcFind 0x%llx differ, net-patch9 loader not installed",
            (unsigned long long)fRelocate, (unsigned long long)fBindSymbol, (unsigned long long)fDcFind);
        return;
    }
    g_ready = true;
    ++installed;
    log("netpatch: net-patch9 loader ready (RelocateModule 0x%llx, BindSymbol 0x%llx); loads when Multiplayer is entered",
        (unsigned long long)fRelocate, (unsigned long long)fBindSymbol);
}

}  // namespace u2mp
