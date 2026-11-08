// u2mp internals shared by the C++ files of the module.
#pragma once
#include <cstddef>
#include <cstdint>

// Guest code is x86-64 System V; the runtime is Windows x64. Every function the guest calls, and
// every guest function we call, uses this convention.
#define U2MP_GUEST __attribute__((sysv_abi))

extern "C" unsigned char *runtime_near_image_alloc(size_t bytes, int executable);

namespace u2mp {

// The loaded Uncharted 2 1.02 image (addresses below are image offsets of that build).
struct Image {
    unsigned char *base = nullptr;
    uint64_t size = 0;
    const uint64_t *segments = nullptr;  // {address, size, flags} triples
    uint64_t segment_count = 0;

    template <typename T> T *at(uint64_t offset) const { return reinterpret_cast<T *>(base + offset); }
    bool executable(uint64_t offset, uint64_t length) const;
    bool matches(uint64_t offset, const unsigned char *bytes, size_t length) const;
};

extern Image image;

// Executable bytes within rel32 range of the image (shared 64 KiB blocks); NULL when none is left.
unsigned char *near_stub(size_t bytes);
// Replaces the guest function at `offset` (its first `length` bytes, at least 5) with a jump to
// `host`, a U2MP_GUEST function. The original code is not kept: only for functions we port whole.
bool replace_function(uint64_t offset, unsigned length, void *host, const char *name);
// Points the call instruction at `offset` (which must call `expected_target`) at `host` instead.
bool retarget_call(uint64_t offset, uint64_t expected_target, void *host, const char *name);

void log(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Uncharted 2 1.02 addresses (image offsets), each checked in install.cpp before use.
namespace addr {
constexpr uint64_t menu_manager = 0x1136640;      // the frontend menu manager object
constexpr uint64_t show_page = 0x5cb990;          // ShowPage(menu, page id, flags)
constexpr uint64_t cb_multiplayer = 0x5eb160;     // BUTTON_MULTIPLAYER callback (stripped: ret)
constexpr uint64_t reg_multiplayer = 0x5ed9db;    // its registration (id 0xed9d8ae1)
}  // namespace addr

// String ids: CRC-32 of the name, MSB first, polynomial 0x04C11DB7, no init, no final xor.
constexpr uint32_t sid(const char *s, uint32_t crc = 0) {
    for (; *s; ++s) {
        crc ^= uint32_t(uint8_t(*s)) << 24;
        for (int i = 0; i < 8; ++i) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}
static_assert(sid("BUTTON_MULTIPLAYER") == 0xed9d8ae1u, "string id function");

void install_frontend(int &installed);
void install_natives(int &installed);   // natives.cpp: the co-op / net script natives
void install_remaining(int &installed); // remaining.cpp: the last stubbed callbacks and Applies
void install_menus(int &installed);     // menus.cpp: the multiplayer menu queries (u2mp/docs/MENUS.md)
void install_menuimages(int &installed); // menuimages.cpp: net image ids, lost menu image types (MENUS.md 7)
void install_dlc(int &installed);       // dlc.cpp: DLC maps offered only when imported (u2mp/docs/DLC.md)
void install_menudraw(int &installed);  // menudraw.cpp: 2D command arena 192 KiB -> 2 MiB (PORT-MENUDRAW.md)
void install_frontend2(int &installed); // frontend2.cpp: partial menu functions vs PS3 (PORT-FRONTEND2.md)
void install_spawn(int &installed);     // spawn.cpp: spawner lookup at match start (PORT-SPAWN.md)
void install_gameplay2(int &installed); // gameplay2.cpp: connection event Applies (PORT-GAMEPLAY2.md)
void install_nullsites(int &installed); // nullsites.cpp: replay stream fallback at WaitReady::Exit (PORT-NULLSITES.md)
void install_netpatch(int &installed);  // netpatch.cpp: the PS3 server live-data module net-patch9 (PORT-NETPATCH.md)
int netpatch_load();                    // netpatch.cpp: bind net-patch9's symbols (idempotent); when MP is entered
// False when the *net-games* record `index` plays on a DLC map whose map pack was not imported.
bool dlc_net_game_available(int32_t index);
// netinit.cpp: state the removed PS3 multiplayer init set up (u2mp/docs/NETINIT.md). Both idempotent.
void install_crashlog();   // crashlog.cpp: last-chance exception log (diagnostics)
void netheap_install();                     // netheap.cpp: memory context 0x14 gets a heap of its own
void *netheap_alloc(size_t size, size_t align);
void netinit_prepare();   // when Multiplayer is entered
void netinit_match();     // at match start
void netinit_dump(const char *when);   // diagnostics: the tables as the game finds them
// session.cpp: a loaded package by id (no request), NULL when absent.
const uint8_t *package_by_id(uint32_t id);
// menuimages.cpp: "net-u2mp" when the import wrote pak22/ps4/net-u2mp.pak (PS3 menu images), else NULL.
const char *extra_net_package();
// menus.cpp: texts only the PS3 server sent (playlist names / descriptions), NULL for other ids.
const char *extra_text(uint32_t id);
// menus.cpp: a random game type of the chosen matchmaking playlist, -1 when none.
int32_t playlist_pick_type();
// ShowPage(menu manager, page id, 0) in the guest.
void show_page(uint32_t page);
// True when the function at `offset` is a stripped stub: ret, then alignment padding.
inline bool is_ret_stub(uint64_t offset) {
    static const unsigned char stub[] = {0xc3, 0x66, 0x66, 0x66, 0x66};
    return image.matches(offset, stub, sizeof stub);
}

}  // namespace u2mp
