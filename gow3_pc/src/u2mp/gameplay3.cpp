// u2mp gameplay3: end of match -> results screen, ported from PS3 1.10 (notes: u2mp/docs/PORT-GAMEPLAY3.md).
//
// TaskManagerOnline state classes (vtables [dtor, Enter, Exit, Update]) pair one to one by vtable order:
//   PS3 0xd70438 Leave .. 0xd70578 EstablishConnection  <->  PS4 0x100a9c0 Leave .. 0x100ac40.
//   ShowResults: PS3 0xd70478 = Enter 0x43ec08, Exit 0x438478, Update 0x4421b8
//                PS4 0x100aa40 = Enter 0x5f0ea0 (ret stub), Exit 0x5f0e90, Update 0x5f0b30
// (CINEMA.md took PS3 0x443e40 for ShowResults::Enter; by the vtable it is Resetting::Enter = PS4 0x5f1240.)
//
// PS3 ShowResults::Enter 0x43ec08 does, in order:
//   1. 0x42a4bc(menu 0xf0e800): closes an open in-game menu (state +1460 not 0/5/6 -> 0x4298b8(menu, 5)).
//      PS4 has the same code inlined in Resetting::Enter 0x5f1264 and Leave::Enter 0x5f0130:
//      menu 0x1136640, state +0x1014, event 0xccf830(ev, 0, 0), 0x5cd5c0(menu, ev, 3), +0x1014 = 5,
//      +0x1018 = 0, +0x101c = 1.0f, 0x5cd5c0(menu, ev, 0).                                  -> ported
//   2. thread "Save Game Stats" (0x43bc30 -> 0x4e1590: sends the stat game to Naughty Dog's SQS
//      server) and NetPlayerData::Publish 0x4c6298 (TUS): server side, no PC counterpart; the local
//      progression store is written by progression.cpp at Leave.                              -> not ported
//   3. 0x4af240: matchmaking skill values into the NP room member data (sceNpMatching2): no PC room. -> not ported
//   4. results end time 0xf12504 = net clock frames / 300 + 10.0 s.                           -> ported
//   5. NET_SM 0xf11764 = 29 (NET_SM_RESULTS).                                                 -> ported
//   6. per remote tracker: cellVoiceSetVolume(port, 1.0) (voice.cpp owns PC voice).           -> not ported
//
// The folded global (item 4): PS4 ShowResults::Update 0x5f0b30 computes the time left as
// `end - now / 300` like PS3 0x4421b8 (fnmsubs f31 = [0xf12504] - now * k), but PS4 never writes the
// end time (its only PS3 writer is the stripped Enter), so the compiler folded it to 0.0:
// 0x5f0b5a..0x5f0b67 is `xmm1 = frames * (-1/300)`. The time left is negative from the first frame,
// so the host goes straight to Leave (no 10 s results screen) and clients only wait for message
// 0x105. 0x5f0b50 (the 0x17 bytes that load the clock and scale it) jumps to a stub that adds the
// end time our Enter stores: xmm1 = frames * (-1/300) + end; then back to 0x5f0b67. With no Enter
// (end = 0) it behaves exactly as before.
//
// Found by a scan over the PS3 image (PORT-GAMEPLAY3.md section 2): every data address whose PS3
// writers are all stripped on PS4 and whose readers survive. 0xf12504 is the only new one; the others
// (net clock 0xf116d4, abort flag 0xf119c4, menu/lobby globals) were fixed before or are only read
// by stubbed menu callbacks.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
void install_gameplay3(int &installed);

namespace {

// ---- PS4 1.02 image offsets ----------------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;      // NET_SM (PS3 0xf11764)
constexpr int32_t kNetSmResults = 29;          // NET_SM_RESULTS (PS3 0x43ed1c: li 0, 29)
constexpr uint64_t kNetClock = 0x113a040;      // pointer; +0xc frames at 300 Hz (PS3 [0xf116d4] + 12)
constexpr uint64_t kMenu = 0x1136640;          // menu manager (PS3 0xf0e800)
constexpr uint64_t M_STATE = 0x1014, M_STATE_ARG = 0x1018, M_FADE = 0x101c;   // PS3 +1460 ..
constexpr uint64_t fMenuEventInit = 0xccf830;  // (event, 0, 0)
constexpr uint64_t fMenuEvent = 0x5cd5c0;      // (menu, event, code)
constexpr uint64_t kScaleConst = 0xe62288;     // -1/300 (read by ShowResults::Update)
constexpr float kResultsSeconds = 10.0f;       // PS3 0xdb05d8

constexpr uint64_t vtShowResults = 0x100aa40;  // PS3 0xd70478
constexpr uint64_t hShowResultsEnter = 0x5f0ea0;   // slot 1; ret stub, or cinema.cpp's finish
constexpr uint64_t cCountdown = 0x5f0b50, cCountdownBack = 0x5f0b67;   // in ShowResults::Update 0x5f0b30

// {-1/300, end seconds}: read by the countdown stub (vmulss [rax], vaddss [rax+4]).
alignas(16) float g_countdown[4] = {-0.0033333334140479565f, 0.0f, 0.0f, 0.0f};

template <typename T> T G(uint64_t off) {
    T v;
    std::memcpy(&v, image.base + off, sizeof v);
    return v;
}
template <typename T> void P(uint64_t off, T v) { std::memcpy(image.base + off, &v, sizeof v); }

int32_t clock_frames() {
    const uint64_t p = G<uint64_t>(kNetClock);
    if (!p) return 0;
    int32_t f;
    std::memcpy(&f, reinterpret_cast<const void *>(uintptr_t(p + 0xc)), 4);
    return f;
}

// PS3 0x42a4bc (the PS4 copy inlined at 0x5f1264): close an open in-game menu.
void close_ingame_menu() {
    const uint32_t s = G<uint32_t>(kMenu + M_STATE);
    if (s <= 6 && ((0x61u >> s) & 1u)) return;   // 0, 5, 6: nothing open / already closing
    alignas(16) unsigned char ev[0x100] = {};
    using Init = void(U2MP_GUEST *)(void *, uint32_t, uint64_t);
    using Event = void(U2MP_GUEST *)(void *, void *, uint32_t);
    void *menu = image.base + kMenu;
    reinterpret_cast<Init>(image.base + fMenuEventInit)(ev, 0, 0);
    reinterpret_cast<Event>(image.base + fMenuEvent)(menu, ev, 3);
    P<uint32_t>(kMenu + M_STATE, 5);
    P<uint32_t>(kMenu + M_STATE_ARG, 0);
    P<float>(kMenu + M_FADE, 1.0f);
    reinterpret_cast<Event>(image.base + fMenuEvent)(menu, ev, 0);
}

// ShowResults::Enter (PS4 vtable 0x100aa40 slot 1), PS3 0x43ec08. Runs the slot's previous target
// afterwards: the PS4 ret stub, or cinema.cpp's recording finish installed there.
U2MP_GUEST void show_results_enter(void *state) {
    close_ingame_menu();
    const float now = float(clock_frames()) * 0.0033333334140479565f;
    g_countdown[1] = now + kResultsSeconds;
    P<int32_t>(kNetState, kNetSmResults);
    log("ShowResults::Enter: NET_SM_RESULTS, results end at %.2f s (now %.2f)", double(g_countdown[1]), double(now));
    using Enter = void(U2MP_GUEST *)(void *);
    reinterpret_cast<Enter>(image.base + hShowResultsEnter)(state);
}

bool install_countdown() {
    // 5f0b50: lea rax,[rip+..]  (-> 0x113a040); mov rax,[rax]; vcvtsi2ss xmm0,xmm0,[rax+0xc];
    //         vmulss xmm1,xmm0,[rip+..] (-> 0xe62288)
    static const unsigned char expect[] = {0x48, 0x8d, 0x05, 0xe9, 0x94, 0xb4, 0x00, 0x48, 0x8b, 0x00, 0xc5, 0xfa,
                                           0x2a, 0x40, 0x0c, 0xc5, 0xfa, 0x59, 0x0d, 0x21, 0x17, 0x87, 0x00};
    static_assert(sizeof expect == cCountdownBack - cCountdown, "countdown site length");
    if (!image.matches(cCountdown, expect, sizeof expect)) {
        log("results countdown (5f0b50): bytes differ, left alone");
        return false;
    }
    if (G<float>(kScaleConst) != g_countdown[0]) {
        log("results countdown (5f0b50): scale constant 0xe62288 differs, left alone");
        return false;
    }
    unsigned char *stub = near_stub(64);
    const int64_t delta = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + cCountdown + 5)) : 0;
    if (!stub || delta < INT32_MIN || delta > INT32_MAX) {
        log("results countdown (5f0b50): no stub memory in range");
        return false;
    }
    const uint64_t clock = uint64_t(uintptr_t(image.base + kNetClock));
    const uint64_t consts = uint64_t(uintptr_t(g_countdown));
    const uint64_t back = uint64_t(uintptr_t(image.base + cCountdownBack));
    unsigned char code[] = {
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,     // mov rax, &clock pointer
        0x48, 0x8b, 0x00,                       // mov rax,[rax]
        0xc5, 0xfa, 0x2a, 0x40, 0x0c,           // vcvtsi2ss xmm0,xmm0,dword [rax+0xc]
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,     // mov rax, &g_countdown
        0xc5, 0xfa, 0x59, 0x08,                 // vmulss xmm1,xmm0,[rax]      frames * -1/300
        0xc5, 0xf2, 0x58, 0x48, 0x04,           // vaddss xmm1,xmm1,[rax+4]    + end
        0xff, 0x25, 0, 0, 0, 0,                 // jmp [rip] -> 0x5f0b67
        0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code + 2, &clock, 8);
    std::memcpy(code + 20, &consts, 8);
    std::memcpy(code + 43, &back, 8);
    static_assert(sizeof code <= 64, "countdown stub size");
    std::memcpy(stub, code, sizeof code);
    unsigned char *c = image.base + cCountdown;
    const int32_t rel = int32_t(delta);
    c[0] = 0xe9;
    std::memcpy(c + 1, &rel, 4);
    std::memset(c + 5, 0x90, sizeof expect - 5);
    log("results countdown (5f0b50): end time restored (PS3 0xf12504)");
    return true;
}

bool layout_matches() {
    // ShowResults::Exit 0x5f0e90: mov rax,[rdi+8]; mov dword [rax+0x148],1.0; ret (PS3 0x438478)
    static const unsigned char exit_bytes[] = {0x48, 0x8b, 0x47, 0x08, 0xc7, 0x80, 0x48, 0x01, 0x00, 0x00,
                                               0x00, 0x00, 0x80, 0x3f, 0xc3};
    // Resetting::Enter 0x5f1264..: the inlined menu close this port repeats (calls 0xccf830 / 0x5cd5c0).
    static const unsigned char menu_bytes[] = {0x48, 0x8d, 0x1d, 0xd5, 0x53, 0xb4, 0x00, 0x8b, 0x83, 0x14, 0x10,
                                               0x00, 0x00, 0x83, 0xf8, 0x06, 0x77, 0x0a, 0xb9, 0x61, 0x00, 0x00, 0x00};
    // ShowRoundResults::Enter 0x5f0af0: mov dword [NET_SM],0x1e (the PS4 writer of the sibling state)
    static const unsigned char round_bytes[] = {0xc7, 0x05, 0x82, 0xde, 0xed, 0x00, 0x1e, 0x00, 0x00, 0x00};
    if (vtShowResults + 0x20 > image.size || kNetState + 4 > image.size) return false;
    if (!image.matches(0x5f0e90, exit_bytes, sizeof exit_bytes) ||
        !image.matches(0x5f1264, menu_bytes, sizeof menu_bytes) ||
        !image.matches(0x5f0af0, round_bytes, sizeof round_bytes)) {
        log("gameplay3: ShowResults / menu anchors differ, nothing installed");
        return false;
    }
    // call targets inside the anchor: 5f128e call ccf830, 5f129e call 5cd5c0
    const auto call_to = [](uint64_t at, uint64_t target) {
        int32_t rel;
        std::memcpy(&rel, image.base + at + 1, 4);
        return image.base[at] == 0xe8 && at + 5 + int64_t(rel) == int64_t(target);
    };
    if (!call_to(0x5f128e, fMenuEventInit) || !call_to(0x5f129e, fMenuEvent)) {
        log("gameplay3: menu event calls differ, nothing installed");
        return false;
    }
    return true;
}

}  // namespace

void install_gameplay3(int &installed) {
    if (!image.base || !layout_matches()) return;
    const uint64_t base = uint64_t(uintptr_t(image.base));
    if (install_countdown()) ++installed;
    uint64_t &slot = *image.at<uint64_t>(vtShowResults + 8 * 1);
    if (slot != base + hShowResultsEnter) {
        log("ShowResults::Enter: vtable 0x%llx slot 1 is not 0x%llx, left alone (countdown stays at 0)",
            (unsigned long long)vtShowResults, (unsigned long long)hShowResultsEnter);
        return;
    }
    slot = uint64_t(uintptr_t(&show_results_enter));
    log("ShowResults::Enter: vtable 0x%llx slot 1 -> C++ (PS3 0x43ec08)", (unsigned long long)vtShowResults);
    ++installed;
}

}  // namespace u2mp
