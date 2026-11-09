// u2mp frontend6: the "Starting in [A]..." popup and the matchmaking level preload
// (notes: u2mp/docs/PORT-FRONTEND6.md).
//
// 1. "Starting in [A]..." (PS3 0x42a0b8, text 73a73dd8, 5 s)
//
// PS3 Start Match 0x43aab0: the party leader sends party mailbox 0x114 and opens the timed popup
// 0x42a0b8(menu, 73a73dd8, 5, done, cancel, fn 0, arg 0): matchmaking done 4055f45f
// (START_MATCHMAKING 0x43e99c) / cancel bbb649ef (0x439a88), custom game done e2707dbe
// (CREATE_GAME_WAIT 0x444bb0) / cancel bf6ce4d5 (0x439b04). A member on 0x114 (0x43a444 -> 0x43a8ec)
// opens the same popup with done ee8f5f9b (blr) and no cancel. 0x42a0b8:
//   menu+2704 = done, menu+2708 = cancel; page record 0x453470dd (menu+720[i], id +12): +209 = (cancel == 0)
//   (back disabled); desc [menu+2624]: +4 = text, 0x40e348(desc, seconds) (+44 now, +48 deadline);
//   list menu+4256 / +4260 (12-byte {page, fn, arg}, at most 31): the page's entry; ShowPage(0x453470dd).
// The menu update 0x428c88 closes it at the deadline (0x40e158), pops it and calls
// 0x426dbc(menu, done); the page's back control calls 0x426dbc(menu, cancel) (0x427c6c).
//
// PS4 kept all of it except 0x42a0b8 and its callers (no code writes menu+0x18d4, nothing refers to the
// text): page 0x453470dd is built at 0x5c7981 (desc [menu+0x1880], back control 0x128aa55b at
// page+0xe4), the menu update 0x5cfb54 closes it at the deadline ([0x1116180+0x104] > desc+0x44) and
// calls DoCallback 0x5c8840(menu, [menu+0x18d4], 0) at 0x5cfbed; back = 0x128aa55b -> DoCallback(menu,
// [menu+0x18d8]) at 0x5caf6c; CanGoBack 0x5a33c0 reads page+0xf1 (PS3 +209). PS3 -> PS4:
//   menu+2704 / +2708 -> +0x18d4 / +0x18d8; [menu+2624] -> [menu+0x1880]; desc +4 -> +8;
//   0x40e348 -> 0x5ae8a0 (desc+0x40 now, +0x44 deadline); pages menu+720 (id +12) -> menu+0x370 (count
//   +0x9b0, id +0x10); +209 -> +0xf1; list menu+4256 / +4260 {page, fn, arg} 12 bytes -> +0x24f0 /
//   +0x24f8, 0x18 bytes {u32 page, fn +8, arg +0x10}, at most 32 (0x5c909c); ShowPage 0x429a08 -> 0x5cb990.
//
// The done callbacks go through the menu callback registry (menu+0x18ec / +0x18f0, 0x18-byte {sid, fn,
// arg}, dispatched at 0x5cb8d4 with a pointer to {sid, x, arg}). Its registration 0x5ef2xx points
// e2707dbe at 0x5eb830 (the PS4 create path that reads the NULL room-attribute list, test run 3) and
// 4055f45f at START_MATCHMAKING 0x5eba40 (which lost PS3's mailbox 0x107). The two `lea rcx,[rip+..]`
// of that registration (0x5ef31a, 0x5ef41e) now load our callbacks: e2707dbe -> session::create_match
// (session.cpp's version of 0x444bb0), 4055f45f -> matchmaking::leader_start (0x107 + 0x5eba40) on the
// party host, the original 0x5eba40 otherwise. The cancel callbacks bbb649ef / bf6ce4d5 are
// frontend3.cpp's leader_back (mailbox 0x115 + PopPage, which also closes the overlay).
//
// 2. Level preload (PS3 0x4a12e8 = PS4 0x641690(ngm, request, keep_menu, flags_only))
//
// request: queue the net game's levels (level manager 0x1130390, 0x56c0e0) and its DC modules
// (0xa16bb0); !request: release them. keep_menu: also keeps "menu" / "menu-temple" queued and sets
// ngm+0x209d (written only). flags_only: skips the net game part. PS4 kept two of PS3's eight callers
// (0x5e9a9b = PS3 0x436014, LoadLevels 0x64148c). Lost with the matchmaking machine: the vote fill
// (0x4b0840 member, 0x4b0a28 host: 1, 1, 1) and the result (0x4affe0 host after mailbox 0x109,
// 0x4b1fd4 member on 0x109: 1, 1, 0); frontend3.cpp calls preload_net_game() at those points.
#include <cstdint>
#include <cstring>
#include <vector>

#include "frontend3.h"
#include "frontend6.h"
#include "session_api.h"
#include "u2mp_internal.h"

namespace u2mp {
namespace {

template <typename T> T &G(uint64_t offset) { return *image.at<T>(offset); }
template <typename T> T &M(uint8_t *base, uint64_t offset) { return *reinterpret_cast<T *>(base + offset); }
template <typename F> F guest(uint64_t offset) { return reinterpret_cast<F>(image.base + offset); }

// menu manager fields
constexpr uint64_t MENU_DONE_CB = 0x18d4, MENU_CANCEL_CB = 0x18d8;   // PS3 menu+2704 / +2708
constexpr uint64_t MENU_TIMED_DESC = 0x1880;                         // PS3 menu+2624
constexpr uint64_t MENU_PAGE_OBJECTS = 0x370, MENU_PAGE_OBJECT_COUNT = 0x9b0;   // PS3 menu+720
constexpr uint64_t MENU_OVERLAY = 0x9b8;
constexpr uint64_t MENU_PAGE_LIST_COUNT = 0x24f0, MENU_PAGE_LIST = 0x24f8, PAGE_LIST_SIZE = 0x18;   // PS3 +4256 / +4260
constexpr uint32_t kMaxPageObjects = 200, kMaxPageList = 32;
constexpr uint64_t PAGE_ID = 0x10, PAGE_NO_BACK = 0xf1;   // PS3 +12, +209
constexpr uint64_t DESC_TEXT = 0x08;                      // PS3 +4
// functions
constexpr uint64_t fSetTimer = 0x5ae8a0;                  // (desc, seconds) = PS3 0x40e348
constexpr uint64_t fStartMatchmaking = 0x5eba40;          // cb 4055f45f, PS3 0x43e99c (intact part)
constexpr uint64_t fPreload = 0x641690;                   // PS3 0x4a12e8
// the callback registration that names the two done callbacks
constexpr uint64_t kRegCreateGame = 0x5ef31a, kCreateGame = 0x5eb830;     // e2707dbe
constexpr uint64_t kRegStartMm = 0x5ef41e;                                // 4055f45f -> 0x5eba40
constexpr uint64_t kNgm = 0x113b760;
// ids
constexpr uint32_t kPopupPage = 0x453470dd;   // the timed popup page
constexpr uint32_t kTextStarting = 0x73a73dd8;   // "Starting in [A]..."
constexpr int32_t kSeconds = 5;
constexpr uint32_t kDoneMatchmaking = 0x4055f45f, kCancelMatchmaking = 0xbbb649ef;
constexpr uint32_t kDoneCreateGame = 0xe2707dbe, kCancelCreateGame = 0xbf6ce4d5;
constexpr uint32_t kDoneMember = 0xee8f5f9b;   // PS4 0x5eba90 = PS3 0x4384ec: blr

bool popup_ok = false, preload_ok = false;

// PS3 0x42a0b8(menu, text, seconds, done, cancel, fn, arg).
bool timed_popup(uint32_t text, int32_t seconds, uint32_t done, uint32_t cancel, uint64_t fn, uint64_t arg) {
    uint8_t *menu = image.at<uint8_t>(addr::menu_manager);
    uint8_t *desc = M<uint8_t *>(menu, MENU_TIMED_DESC);
    if (!desc) {
        log("starting popup: no popup descriptor yet (menu+0x1880), not shown");
        return false;
    }
    M<uint32_t>(menu, MENU_DONE_CB) = done;
    M<uint32_t>(menu, MENU_CANCEL_CB) = cancel;
    const uint32_t pages = M<uint32_t>(menu, MENU_PAGE_OBJECT_COUNT);
    for (uint32_t i = 0; i < pages && i < kMaxPageObjects; ++i) {
        uint8_t *page = M<uint8_t *>(menu, MENU_PAGE_OBJECTS + 8 * uint64_t(i));
        if (!page) break;
        if (M<uint32_t>(page, PAGE_ID) == kPopupPage) M<uint8_t>(page, PAGE_NO_BACK) = cancel == 0 ? 1 : 0;
    }
    M<uint32_t>(desc, DESC_TEXT) = text;
    guest<void(U2MP_GUEST *)(void *, int32_t)>(fSetTimer)(desc, seconds);
    uint32_t &count = M<uint32_t>(menu, MENU_PAGE_LIST_COUNT);
    uint8_t *entry = nullptr;
    for (uint32_t i = 0; i < count && i < kMaxPageList; ++i)
        if (M<uint32_t>(menu, MENU_PAGE_LIST + PAGE_LIST_SIZE * i) == kPopupPage) entry = menu + MENU_PAGE_LIST + PAGE_LIST_SIZE * i;
    if (!entry && count < kMaxPageList) entry = menu + MENU_PAGE_LIST + PAGE_LIST_SIZE * count++;
    if (entry) {
        M<uint32_t>(entry, 0) = kPopupPage;
        M<uint64_t>(entry, 8) = fn;
        M<uint64_t>(entry, 0x10) = arg;
    } else {
        log("starting popup: page list full (%u entries)", count);   // PS3 asserts here
    }
    show_page(kPopupPage);
    return true;
}

// Registry callbacks: called with a pointer to {u32 sid, u32 x, u64 arg}.
U2MP_GUEST void cb_create_game(void *) {   // e2707dbe, PS3 CREATE_GAME_WAIT 0x444bb0
    log("starting popup: done, creating the match");
    session::create_match();   // host only (session.cpp checks)
}

U2MP_GUEST void cb_start_matchmaking(void *call) {   // 4055f45f, PS3 START_MATCHMAKING 0x43e99c
    if (session::is_host() && matchmaking::installed()) {
        log("starting popup: done, starting matchmaking");
        matchmaking::leader_start();   // mailbox 0x107 to the party, then 0x5eba40
        return;
    }
    guest<void(U2MP_GUEST *)(void *, void *, void *)>(fStartMatchmaking)(call, nullptr, nullptr);
}

// Points the registration's `lea rcx,[rip+disp32]` at `site` (which loads `expected`) at `host`.
bool retarget_lea(uint64_t site, uint64_t expected, void *host, const char *name) {
    static const unsigned char lea_rcx[] = {0x48, 0x8d, 0x0d};
    int32_t disp = 0;
    if (image.matches(site, lea_rcx, sizeof lea_rcx)) std::memcpy(&disp, image.base + site + 3, 4);
    if (!image.matches(site, lea_rcx, sizeof lea_rcx) || site + 7 + int64_t(disp) != int64_t(expected)) {
        log("%s at 0x%llx: not the registration this port expects, left alone", name, (unsigned long long)site);
        return false;
    }
    unsigned char *stub = near_stub(16);
    if (!stub) return false;
    const uint64_t target = uint64_t(uintptr_t(host));
    stub[0] = 0x48;
    stub[1] = 0xb8;
    std::memcpy(stub + 2, &target, 8);
    stub[10] = 0xff;
    stub[11] = 0xe0;
    const int64_t delta = int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + site + 7));
    if (delta < INT32_MIN || delta > INT32_MAX) return false;
    const int32_t rel = int32_t(delta);
    std::memcpy(image.base + site + 3, &rel, 4);
    log("%s: registration at 0x%llx -> C++", name, (unsigned long long)site);
    return true;
}

}  // namespace

namespace frontend6 {

bool installed() { return popup_ok; }

bool leader_starting(bool matchmaking) {
    if (!popup_ok || !session::is_host()) return false;
    if (!timed_popup(kTextStarting, kSeconds, matchmaking ? kDoneMatchmaking : kDoneCreateGame,
                     matchmaking ? kCancelMatchmaking : kCancelCreateGame, 0, 0))
        return false;
    session::send_message(-1, {kStartingMessage});   // PS3 0x4c38f0(0x114)
    log("starting popup: the leader starts a %s (%d s)", matchmaking ? "matchmaking search" : "custom game", kSeconds);
    return true;
}

void member_starting() {
    if (!popup_ok || session::is_host()) return;
    if (timed_popup(kTextStarting, kSeconds, kDoneMember, 0, 0, 0)) log("starting popup: the leader starts the match");
}

bool popup_up() {
    const uint8_t *overlay = G<uint8_t *>(addr::menu_manager + MENU_OVERLAY);
    return overlay && *reinterpret_cast<const uint32_t *>(overlay + PAGE_ID) == kPopupPage;
}

void preload_net_game(bool request, bool keep_menu, bool flags_only) {
    if (!preload_ok) return;
    log("level preload (PS3 0x4a12e8): request %d, keep menu %d, flags only %d, net game %d", request, keep_menu,
        flags_only, int(G<int16_t>(kNgm + 0x1ca0)));
    guest<void(U2MP_GUEST *)(void *, int, int, int)>(fPreload)(image.at<void>(kNgm), request, keep_menu, flags_only);
}

}  // namespace frontend6

void install_frontend6(int &installed) {
    // The popup machinery PS4 kept (see the header).
    struct Check { uint64_t at; std::vector<unsigned char> bytes; };
    const Check checks[] = {
        {0x5c7981, {0xbe, 0xdd, 0x70, 0x34, 0x45}},                     // page 0x453470dd built
        {0x5cfb54, {0x49, 0x8b, 0x86, 0xb8, 0x09, 0x00, 0x00}},         // overlay page
        {0x5cfb64, {0x81, 0x78, 0x10, 0xdd, 0x70, 0x34, 0x45}},         // is the timed popup
        {0x5cfb71, {0x49, 0x8b, 0x86, 0x80, 0x18, 0x00, 0x00}},         // desc [menu+0x1880]
        {0x5cfb85, {0x3b, 0x48, 0x44}},                                 // deadline desc+0x44
        {0x5cfbed, {0x41, 0x8b, 0xb6, 0xd4, 0x18, 0x00, 0x00}},         // done cb menu+0x18d4
        {0x5caf6c, {0x41, 0x8b, 0xb5, 0xd8, 0x18, 0x00, 0x00}},         // cancel cb menu+0x18d8
        {0x5a3416, {0x80, 0xbb, 0xf1, 0x00, 0x00, 0x00, 0x00}},         // page+0xf1 back disabled
        {0x5c9068, {0x41, 0x8b, 0x85, 0xf0, 0x24, 0x00, 0x00}},         // page list count
        {0x5c90ed, {0x49, 0x8d, 0x94, 0xc5, 0xf8, 0x24, 0x00, 0x00}},   // page list entries
        {0x5cb8d4, {0x41, 0x8b, 0x85, 0xec, 0x18, 0x00, 0x00}},         // callback registry dispatch
        {fSetTimer, {0x48, 0x8d, 0x05, 0xd9, 0x78, 0xb6, 0x00, 0x8b, 0x80, 0x04, 0x01, 0x00, 0x00, 0x89, 0x47, 0x40}},
        {fStartMatchmaking, {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8d, 0x1d}},
        {0x5ef30f, {0xc7, 0x84, 0xc3, 0xf0, 0x18, 0x00, 0x00, 0xbe, 0x7d, 0x70, 0xe2}},   // e2707dbe registered
        {0x5ef413, {0xc7, 0x84, 0xc3, 0xf0, 0x18, 0x00, 0x00, 0x5f, 0xf4, 0x55, 0x40}},   // 4055f45f registered
    };
    bool ok = true;
    for (const Check &c : checks)
        if (!image.matches(c.at, c.bytes.data(), c.bytes.size())) {
            log("starting popup: bytes at 0x%llx differ, popup off (Start Match starts right away)", (unsigned long long)c.at);
            ok = false;
            break;
        }
    if (ok && retarget_lea(kRegCreateGame, kCreateGame, reinterpret_cast<void *>(&cb_create_game), "done callback e2707dbe") &&
        retarget_lea(kRegStartMm, fStartMatchmaking, reinterpret_cast<void *>(&cb_start_matchmaking), "done callback 4055f45f")) {
        popup_ok = true;
        installed += 2;
    }
    // The level preload: prologue, the *net-games* lookup and the ngm+0x209d store.
    static const unsigned char pre[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53};
    static const unsigned char games[] = {0xbf, 0x7d, 0x06, 0x9a, 0x3b};
    static const unsigned char store[] = {0x45, 0x88, 0xa7, 0x9d, 0x20, 0x00, 0x00};
    preload_ok = image.matches(fPreload, pre, sizeof pre) && image.matches(0x6416ba, games, sizeof games) &&
                 image.matches(0x6418bc, store, sizeof store);
    if (!preload_ok) log("level preload 0x641690: not the function this port expects, matchmaking does not preload");
}

}  // namespace u2mp
