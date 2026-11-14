// u2mp frontend6: the 5 second "Starting in [A]..." popup before a match, and the net game level
// preload used by the matchmaking vote.
//
// PS4 still has the timed popup page and its per-frame countdown, but not the code that opens it nor
// its callers. This file opens it on the party leader when Start Match is pressed, and on the members
// when the leader tells them. When the countdown runs out the game calls the popup's "done" id through
// its menu callback registry. Two of those registrations point at PS4 code that does not suit the PC,
// so their address loads are retargeted at our own callbacks. The level preload is a thin, checked
// wrapper around the game's own function.
//
// Reimplemented from the behaviour description u2mp/docs/spec/frontend.md.

#include <cstring>
#include <vector>

#include "frontend3.h"
#include "frontend6.h"
#include "session_api.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

// Menu manager fields.
constexpr uint64_t kMenuPageObjects = 0x370;
constexpr uint64_t kMenuPageObjectCount = 0x9b0;
constexpr uint32_t kPageObjectsLooked = 200;
constexpr uint64_t kMenuOverlayPage = 0x9b8;
constexpr uint64_t kMenuPopupDesc = 0x1880;
constexpr uint64_t kMenuDoneId = 0x18d4;
constexpr uint64_t kMenuCancelId = 0x18d8;
constexpr uint64_t kMenuPageListCount = 0x24f0;
constexpr uint64_t kMenuPageList = 0x24f8;
constexpr uint32_t kPageListMax = 32;

// Guest functions and data.
constexpr uint64_t kSetPopupTimer = 0x5ae8a0;
constexpr uint64_t kStartMatchmaking = 0x5eba40;
constexpr uint64_t kLevelPreload = 0x641690;
constexpr uint64_t kCreateGamePath = 0x5eb830;
constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNgmCurrentGame = 0x1ca0;

// Registration sites whose callback address load we retarget.
constexpr uint64_t kRegCustomDone = 0x5ef31a;        // e2707dbe -> 0x5eb830
constexpr uint64_t kRegMatchmakingDone = 0x5ef41e;   // 4055f45f -> 0x5eba40

// Ids.
constexpr uint32_t kPopupPage = 0x453470dd;
constexpr uint32_t kStartingText = 0x73a73dd8;   // "Starting in [A]..."
constexpr int32_t kPopupSeconds = 5;
constexpr uint32_t kMatchmakingDone = 0x4055f45f;
constexpr uint32_t kMatchmakingCancel = 0xbbb649ef;
constexpr uint32_t kCustomDone = 0xe2707dbe;
constexpr uint32_t kCustomCancel = 0xbf6ce4d5;
constexpr uint32_t kMemberDone = 0xee8f5f9b;   // the game does nothing for it

bool popup_ready = false;
bool preload_ready = false;

uintptr_t menu() { return uintptr_t(image.base + addr::menu_manager); }

template <typename T> T get_at(uintptr_t address) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void *>(address), sizeof v);
    return v;
}
template <typename T> void put_at(uintptr_t address, T v) {
    std::memcpy(reinterpret_cast<void *>(address), &v, sizeof v);
}

void set_popup_timer(void *desc, int32_t seconds) {
    using Fn = void(U2MP_GUEST *)(void *, int32_t);
    reinterpret_cast<Fn>(image.base + kSetPopupTimer)(desc, seconds);
}

// Opens the popup page with our text, timer and callback ids. False when the game has not made the
// popup's descriptor yet (then nothing is touched).
bool open_popup(uint32_t text, int32_t seconds, uint32_t done, uint32_t cancel, uint64_t fn, uint64_t arg) {
    const uintptr_t mm = menu();
    const uint64_t desc = get_at<uint64_t>(mm + kMenuPopupDesc);
    if (!desc) {
        log("starting popup: no popup descriptor yet (menu+0x1880), not shown");
        return false;
    }
    put_at<uint32_t>(mm + kMenuDoneId, done);
    put_at<uint32_t>(mm + kMenuCancelId, cancel);

    // Without a cancel id the back control must not close the popup.
    const uint32_t pages = get_at<uint32_t>(mm + kMenuPageObjectCount);
    for (uint32_t i = 0; i < pages && i < kPageObjectsLooked; ++i) {
        const uint64_t page = get_at<uint64_t>(mm + kMenuPageObjects + 8 * uint64_t(i));
        if (!page) break;
        if (get_at<uint32_t>(uintptr_t(page) + 0x10) == kPopupPage)
            put_at<uint8_t>(uintptr_t(page) + 0xf1, cancel == 0 ? 1 : 0);
    }

    put_at<uint32_t>(uintptr_t(desc) + 0x08, text);
    set_popup_timer(reinterpret_cast<void *>(uintptr_t(desc)), seconds);

    // The page list needs an entry for the popup page; reuse the last one that names it.
    const uint32_t count = get_at<uint32_t>(mm + kMenuPageListCount);
    const uint32_t seen = count < kPageListMax ? count : kPageListMax;
    int64_t chosen = -1;
    for (uint32_t i = 0; i < seen; ++i)
        if (get_at<uint32_t>(mm + kMenuPageList + 0x18 * uint64_t(i)) == kPopupPage) chosen = i;
    if (chosen < 0 && count < kPageListMax) {
        chosen = count;
        put_at<uint32_t>(mm + kMenuPageListCount, count + 1);
    }
    if (chosen >= 0) {
        const uintptr_t entry = mm + kMenuPageList + 0x18 * uint64_t(chosen);
        put_at<uint32_t>(entry + 0x00, kPopupPage);
        put_at<uint64_t>(entry + 0x08, fn);
        put_at<uint64_t>(entry + 0x10, arg);
    } else {
        log("starting popup: page list full (%u entries)", count);
    }

    show_page(kPopupPage);
    return true;
}

// ---- callbacks the registry calls when the popup runs out ----

U2MP_GUEST void custom_game_done(void *) {
    log("starting popup: done, creating the match");
    session::create_match();
}

U2MP_GUEST void matchmaking_done(void *call) {
    if (session::is_host() && matchmaking::installed()) {
        log("starting popup: done, starting matchmaking");
        matchmaking::leader_start();
        return;
    }
    // Members (or no matchmaking port): what the game registered originally.
    using Fn = void(U2MP_GUEST *)(void *, void *, void *);
    reinterpret_cast<Fn>(image.base + kStartMatchmaking)(call, nullptr, nullptr);
}

// ---- install ----

// Points the `lea rcx,[rip+d]` at `site` (which must load `expected`) at a new near stub jumping to
// `host`.
bool retarget_registration(uint64_t site, uint64_t expected, void *host, const char *name) {
    static const unsigned char lea_rcx[] = {0x48, 0x8d, 0x0d};
    bool ok = image.matches(site, lea_rcx, sizeof lea_rcx) && site + 7 <= image.size;
    if (ok) {
        int32_t d;
        std::memcpy(&d, image.base + site + 3, sizeof d);
        ok = site + 7 + uint64_t(int64_t(d)) == expected;
    }
    if (!ok) {
        log("%s at 0x%llx: not the registration this port expects, left alone", name,
            static_cast<unsigned long long>(site));
        return false;
    }
    unsigned char *stub = near_stub(16);
    if (!stub) return false;
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + site + 7));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    // mov rax, host; jmp rax
    const uint64_t target = uint64_t(uintptr_t(host));
    stub[0] = 0x48;
    stub[1] = 0xb8;
    std::memcpy(stub + 2, &target, sizeof target);
    stub[10] = 0xff;
    stub[11] = 0xe0;
    const int32_t disp = int32_t(delta);
    std::memcpy(image.base + site + 3, &disp, sizeof disp);
    log("%s: registration at 0x%llx -> C++", name, static_cast<unsigned long long>(site));
    return true;
}

struct ByteCheck {
    uint64_t offset;
    std::vector<unsigned char> bytes;
};

// The popup machinery we rely on, in the order it is checked.
bool popup_code_intact() {
    const ByteCheck checks[] = {
        {0x5c7981, {0xbe, 0xdd, 0x70, 0x34, 0x45}},
        {0x5cfb54, {0x49, 0x8b, 0x86, 0xb8, 0x09, 0x00, 0x00}},
        {0x5cfb64, {0x81, 0x78, 0x10, 0xdd, 0x70, 0x34, 0x45}},
        {0x5cfb71, {0x49, 0x8b, 0x86, 0x80, 0x18, 0x00, 0x00}},
        {0x5cfb85, {0x3b, 0x48, 0x44}},
        {0x5cfbed, {0x41, 0x8b, 0xb6, 0xd4, 0x18, 0x00, 0x00}},
        {0x5caf6c, {0x41, 0x8b, 0xb5, 0xd8, 0x18, 0x00, 0x00}},
        {0x5a3416, {0x80, 0xbb, 0xf1, 0x00, 0x00, 0x00, 0x00}},
        {0x5c9068, {0x41, 0x8b, 0x85, 0xf0, 0x24, 0x00, 0x00}},
        {0x5c90ed, {0x49, 0x8d, 0x94, 0xc5, 0xf8, 0x24, 0x00, 0x00}},
        {0x5cb8d4, {0x41, 0x8b, 0x85, 0xec, 0x18, 0x00, 0x00}},
        {0x5ae8a0, {0x48, 0x8d, 0x05, 0xd9, 0x78, 0xb6, 0x00, 0x8b, 0x80, 0x04, 0x01, 0x00, 0x00, 0x89, 0x47, 0x40}},
        {0x5eba40, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d}},
        {0x5ef30f, {0xc7, 0x84, 0xc3, 0xf0, 0x18, 0x00, 0x00, 0xbe, 0x7d, 0x70, 0xe2}},
        {0x5ef413, {0xc7, 0x84, 0xc3, 0xf0, 0x18, 0x00, 0x00, 0x5f, 0xf4, 0x55, 0x40}},
    };
    for (const ByteCheck &c : checks) {
        if (!image.matches(c.offset, c.bytes.data(), c.bytes.size())) {
            log("starting popup: bytes at 0x%llx differ, popup off (Start Match starts right away)",
                static_cast<unsigned long long>(c.offset));
            return false;
        }
    }
    return true;
}

bool preload_code_intact() {
    static const unsigned char prologue[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                                             0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
    static const unsigned char net_games_id[] = {0xbf, 0x7d, 0x06, 0x9a, 0x3b};
    static const unsigned char flag_store[] = {0x45, 0x88, 0xa7, 0x9d, 0x20, 0x00, 0x00};
    return image.matches(kLevelPreload, prologue, sizeof prologue) &&
           image.matches(0x6416ba, net_games_id, sizeof net_games_id) &&
           image.matches(0x6418bc, flag_store, sizeof flag_store);
}

}  // namespace

namespace frontend6 {

bool installed() { return popup_ready; }

bool leader_starting(bool matchmaking) {
    if (!popup_ready || !session::is_host()) return false;
    const uint32_t done = matchmaking ? kMatchmakingDone : kCustomDone;
    const uint32_t cancel = matchmaking ? kMatchmakingCancel : kCustomCancel;
    if (!open_popup(kStartingText, kPopupSeconds, done, cancel, 0, 0)) return false;
    session::send_message(-1, {kStartingMessage});
    log("starting popup: the leader starts a %s (%d s)", matchmaking ? "matchmaking search" : "custom game",
        kPopupSeconds);
    return true;
}

void member_starting() {
    if (!popup_ready || session::is_host()) return;
    if (open_popup(kStartingText, kPopupSeconds, kMemberDone, 0, 0, 0))
        log("starting popup: the leader starts the match");
}

bool popup_up() {
    const uint64_t overlay = get_at<uint64_t>(menu() + kMenuOverlayPage);
    return overlay && get_at<uint32_t>(uintptr_t(overlay) + 0x10) == kPopupPage;
}

void preload_net_game(bool request, bool keep_menu, bool flags_only) {
    if (!preload_ready) return;
    const int16_t game = get_at<int16_t>(uintptr_t(image.base + kNgm + kNgmCurrentGame));
    log("level preload (PS3 0x4a12e8): request %d, keep menu %d, flags only %d, net game %d", request ? 1 : 0,
        keep_menu ? 1 : 0, flags_only ? 1 : 0, game);
    using Fn = void(U2MP_GUEST *)(void *, int32_t, int32_t, int32_t);
    reinterpret_cast<Fn>(image.base + kLevelPreload)(image.base + kNgm, request ? 1 : 0, keep_menu ? 1 : 0,
                                                      flags_only ? 1 : 0);
}

}  // namespace frontend6

void install_frontend6(int &installed) {
    if (popup_code_intact() &&
        retarget_registration(kRegCustomDone, kCreateGamePath, reinterpret_cast<void *>(&custom_game_done),
                              "done callback e2707dbe") &&
        retarget_registration(kRegMatchmakingDone, kStartMatchmaking,
                              reinterpret_cast<void *>(&matchmaking_done), "done callback 4055f45f")) {
        popup_ready = true;
        installed += 2;
    }

    preload_ready = preload_code_intact();
    if (!preload_ready)
        log("level preload 0x641690: not the function this port expects, matchmaking does not preload");
}

}  // namespace u2mp
