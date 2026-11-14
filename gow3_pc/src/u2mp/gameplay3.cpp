// u2mp gameplay3: gives the match results screen its time on screen again.
//
// When a match ends the game switches to the ShowResults state. On PS4 that state's Enter is an
// empty function, so the in-game menu stays open, the network state never becomes NET_SM_RESULTS
// and the results end time is never set; ShowResults::Update then sees no time left and the host
// leaves the results at once. We put a host Enter in the state's vtable (close the menu, end time =
// now + 10 s, NET_SM_RESULTS) and patch Update's time-left computation so it adds that end time.
//
// Reimplemented from the behaviour description u2mp/docs/spec/gameplay.md.
#include <bit>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "u2mp_internal.h"

namespace u2mp {
void install_gameplay3(int &installed);

namespace {

constexpr uint64_t kNetState = 0x14ce97c;
constexpr int32_t kNetStateResults = 29;
constexpr uint64_t kNetClock = 0x113a040;          // u64 pointer; clock+0xc = s32 300 Hz frames
constexpr uint64_t kMenuState = addr::menu_manager + 0x1014;
constexpr uint64_t kMenuStateArg = addr::menu_manager + 0x1018;
constexpr uint64_t kMenuFade = addr::menu_manager + 0x101c;
constexpr uint64_t kMenuEventInit = 0xccf830;
constexpr uint64_t kMenuEvent = 0x5cd5c0;
constexpr uint64_t kUpdateScale = 0xe62288;        // the -1/300 ShowResults::Update multiplies with

constexpr uint64_t kShowResultsVtable = 0x100aa40;
constexpr uint64_t kEnterSlot = kShowResultsVtable + 8;
constexpr uint64_t kOriginalEnter = 0x5f0ea0;

constexpr uint64_t kPatchAt = 0x5f0b50;            // Update: load the clock and scale it
constexpr uint64_t kPatchResume = 0x5f0b67;
constexpr size_t kPatchLength = kPatchResume - kPatchAt;

constexpr float kTick = 1.0f / 300.0f;
static_assert(std::bit_cast<uint32_t>(kTick) == 0x3B5A740Eu, "1/300 as a float");
static_assert(std::bit_cast<uint32_t>(-kTick) == 0xBB5A740Eu, "-1/300 as a float");
constexpr float kResultsSeconds = 10.0f;

// Read by the countdown stub: [0] the per-frame scale, [1] the results end time in seconds.
alignas(16) float countdown[4] = {-kTick, 0.0f, 0.0f, 0.0f};

template <typename T> T peek_image(uint64_t off) {
    T v;
    std::memcpy(&v, image.base + off, sizeof v);
    return v;
}
template <typename T> void poke_image(uint64_t off, T v) { std::memcpy(image.base + off, &v, sizeof v); }

// True when the instruction at `offset` is `call image+target`.
bool calls(uint64_t offset, uint64_t target) {
    if (image.base[offset] != 0xE8) return false;
    return int64_t(offset + 5) + peek_image<int32_t>(offset + 1) == int64_t(target);
}

// Same computation as the replaced code (time left = frames * -1/300) plus the end time, so the
// result lands in xmm1 where Update expects it. Only rax, xmm0 and xmm1 change, as in the original;
// rax is left pointing at our block.
size_t write_countdown_stub(unsigned char *stub) {
    size_t n = 0;
    auto bytes = [&](std::initializer_list<unsigned char> list) {
        for (unsigned char b : list) stub[n++] = b;
    };
    auto quad = [&](uint64_t v) {
        std::memcpy(stub + n, &v, 8);
        n += 8;
    };
    bytes({0x48, 0xB8});                                   // mov rax, &clock pointer
    quad(uint64_t(uintptr_t(image.base + kNetClock)));
    bytes({0x48, 0x8B, 0x00});                             // mov rax, [rax]
    bytes({0xC5, 0xFA, 0x2A, 0x40, 0x0C});                 // vcvtsi2ss xmm0, xmm0, [rax+0xc]
    bytes({0x48, 0xB8});                                   // mov rax, &countdown
    quad(uint64_t(uintptr_t(countdown)));
    bytes({0xC5, 0xFA, 0x59, 0x08});                       // vmulss xmm1, xmm0, [rax]
    bytes({0xC5, 0xF2, 0x58, 0x48, 0x04});                 // vaddss xmm1, xmm1, [rax+4]
    bytes({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00});           // jmp [rip]
    quad(uint64_t(uintptr_t(image.base + kPatchResume)));
    return n;
}

void install_countdown(int &installed) {
    static const unsigned char expected[] = {0x48, 0x8D, 0x05, 0xE9, 0x94, 0xB4, 0x00, 0x48, 0x8B, 0x00, 0xC5, 0xFA,
                                             0x2A, 0x40, 0x0C, 0xC5, 0xFA, 0x59, 0x0D, 0x21, 0x17, 0x87, 0x00};
    static_assert(sizeof expected == kPatchLength, "countdown patch length");
    if (!image.matches(kPatchAt, expected, sizeof expected)) {
        log("results countdown (5f0b50): bytes differ, left alone");
        return;
    }
    if (peek_image<float>(kUpdateScale) != -kTick) {
        log("results countdown (5f0b50): scale constant 0xe62288 differs, left alone");
        return;
    }
    unsigned char *stub = near_stub(64);
    const int64_t distance = stub ? int64_t(uintptr_t(stub)) - int64_t(uintptr_t(image.base + kPatchAt + 5)) : 0;
    if (!stub || distance < INT32_MIN || distance > INT32_MAX) {
        log("results countdown (5f0b50): no stub memory in range");
        return;
    }
    write_countdown_stub(stub);
    unsigned char *code = image.at<unsigned char>(kPatchAt);
    code[0] = 0xE9;
    const int32_t rel = int32_t(distance);
    std::memcpy(code + 1, &rel, 4);
    std::memset(code + 5, 0x90, kPatchLength - 5);
    log("results countdown (5f0b50): end time restored (PS3 0xf12504)");
    installed += 1;
}

using EventInitFn = void(U2MP_GUEST *)(void *, uint32_t, uint64_t);
using EventFn = void(U2MP_GUEST *)(void *, void *, uint32_t);
using EnterFn = void(U2MP_GUEST *)(void *);

U2MP_GUEST void show_results_enter(void *state) {
    // Close the in-game menu if one is open (0 = none, 5 / 6 = already closing).
    const uint32_t menu_state = peek_image<uint32_t>(kMenuState);
    if (menu_state != 0 && menu_state != 5 && menu_state != 6) {
        alignas(16) unsigned char event[0x100] = {};
        void *menu = image.base + addr::menu_manager;
        reinterpret_cast<EventInitFn>(image.base + kMenuEventInit)(event, 0, 0);
        reinterpret_cast<EventFn>(image.base + kMenuEvent)(menu, event, 3);
        poke_image<uint32_t>(kMenuState, 5);
        poke_image<uint32_t>(kMenuStateArg, 0);
        poke_image<float>(kMenuFade, 1.0f);
        reinterpret_cast<EventFn>(image.base + kMenuEvent)(menu, event, 0);
    }

    const uint64_t clock = peek_image<uint64_t>(kNetClock);
    int32_t frames = 0;
    if (clock) std::memcpy(&frames, reinterpret_cast<const unsigned char *>(clock) + 0xc, 4);
    const float now = float(frames) * kTick;
    const float end = now + kResultsSeconds;
    countdown[1] = end;

    poke_image<int32_t>(kNetState, kNetStateResults);
    log("ShowResults::Enter: NET_SM_RESULTS, results end at %.2f s (now %.2f)", double(end), double(now));
    // Whatever the Enter body is by now (the empty one, or cinema.cpp's replacement) still runs.
    reinterpret_cast<EnterFn>(image.base + kOriginalEnter)(state);
}

}  // namespace

void install_gameplay3(int &installed) {
    if (!image.base) return;
    if (kShowResultsVtable + 0x20 > image.size || kNetState + 4 > image.size) return;

    static const unsigned char results_exit[] = {0x48, 0x8B, 0x47, 0x08, 0xC7, 0x80, 0x48, 0x01,
                                                 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0xC3};
    static const unsigned char menu_close[] = {0x48, 0x8D, 0x1D, 0xD5, 0x53, 0xB4, 0x00, 0x8B, 0x83, 0x14, 0x10, 0x00,
                                               0x00, 0x83, 0xF8, 0x06, 0x77, 0x0A, 0xB9, 0x61, 0x00, 0x00, 0x00};
    static const unsigned char round_results_enter[] = {0xC7, 0x05, 0x82, 0xDE, 0xED, 0x00, 0x1E, 0x00, 0x00, 0x00};
    if (!image.matches(0x5f0e90, results_exit, sizeof results_exit) ||
        !image.matches(0x5f1264, menu_close, sizeof menu_close) ||
        !image.matches(0x5f0af0, round_results_enter, sizeof round_results_enter)) {
        log("gameplay3: ShowResults / menu anchors differ, nothing installed");
        return;
    }
    if (!calls(0x5f128e, kMenuEventInit) || !calls(0x5f129e, kMenuEvent)) {
        log("gameplay3: menu event calls differ, nothing installed");
        return;
    }

    install_countdown(installed);   // the Enter below is useful on its own, so go on either way

    uint64_t *slot = image.at<uint64_t>(kEnterSlot);
    if (*slot != uint64_t(uintptr_t(image.base + kOriginalEnter))) {
        log("ShowResults::Enter: vtable 0x%llx slot 1 is not 0x%llx, left alone (countdown stays at 0)",
            (unsigned long long)kShowResultsVtable, (unsigned long long)kOriginalEnter);
        return;
    }
    *slot = uint64_t(uintptr_t(&show_results_enter));
    log("ShowResults::Enter: vtable 0x%llx slot 1 -> C++ (PS3 0x43ec08)", (unsigned long long)kShowResultsVtable);
    installed += 1;
}

}  // namespace u2mp
