// u2mp scripttime: makes SsTrackGroup::GetDeltaSeconds (image 0xa477c0) usable for state scripts
// that run as net timers.
//
// On PS4 the net-timer branch of that function is only an assert followed by `int 0x41`, so such a
// script crashes the game (seen in Warzone). The net timer is meant to advance with the realtime
// clock, so the branch is rewritten in place to load this frame's realtime delta ticks and fall
// into the function's own "ticks * 1/300, return" tail. Nothing runs on the host at game time.
//
// Reimplemented from the behaviour description u2mp/docs/spec/gameplay.md.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
void install_scripttime(int &installed);

namespace {

constexpr uint64_t kGetDeltaSeconds = 0xa477c0;
constexpr uint64_t kCheckStart = 0xa47853;    // the flag test that picks the net-timer branch
constexpr uint64_t kBranchStart = 0xa4785c;   // first byte of the net-timer (assert) branch
constexpr uint64_t kBranchEnd = 0xa47893;     // the game-clock path begins here

// What the function must look like from the flag test to the end of its game-clock path. Anything
// else means a different build, and the rewrite below would land on the wrong instructions.
const unsigned char kExpected[86] = {
    0xF6, 0x80, 0x68, 0x01, 0x00, 0x00, 0x08, 0x74, 0x37,
    0x48, 0x8B, 0x05, 0x15, 0x67, 0xB2, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x27,
    0x48, 0x8D, 0x3D, 0x0A, 0x20, 0x48, 0x00, 0x48, 0x8D, 0x35, 0xA4, 0xDA, 0x45, 0x00,
    0x48, 0x8D, 0x15, 0xDA, 0xDA, 0x45, 0x00, 0x48, 0x8D, 0x0D, 0xF0, 0xCA, 0x3A, 0x00,
    0x41, 0xB8, 0x9B, 0x02, 0x00, 0x00, 0x45, 0x31, 0xC9, 0xFF, 0xD0, 0xCD, 0x41, 0x5D, 0xC3,
    0x48, 0x8D, 0x05, 0xE6, 0xE8, 0x6C, 0x00, 0xC5, 0xFA, 0x2A, 0x40, 0x50,
    0xC5, 0xFA, 0x59, 0x05, 0xF5, 0xFC, 0x41, 0x00, 0x5D, 0xC3,
};

// The new net-timer branch: point rax at the clock block, convert its clocks[7] delta (+0x210) to a
// float in xmm0, then jump to the shared multiply-by-1/300 and return at 0xa4789f.
const unsigned char kNetTimerBranch[17] = {
    0x48, 0x8D, 0x05, 0x1D, 0xE9, 0x6C, 0x00,         // lea rax, [clock block]
    0xC5, 0xFA, 0x2A, 0x80, 0x10, 0x02, 0x00, 0x00,   // vcvtsi2ss xmm0, xmm0, [rax+0x210]
    0xEB, 0x32,                                       // jmp 0xa4789f
};

}  // namespace

void install_scripttime(int &installed) {
    if (!image.executable(kCheckStart, sizeof kExpected) || !image.matches(kCheckStart, kExpected, sizeof kExpected)) {
        log("GetDeltaSeconds net timer (0x%llx): bytes differ, left alone", (unsigned long long)kGetDeltaSeconds);
        return;
    }
    unsigned char *code = image.at<unsigned char>(kBranchStart);
    std::memcpy(code, kNetTimerBranch, sizeof kNetTimerBranch);
    // The rest of the old branch can no longer be reached; trap it so a stray jump is obvious.
    const uint64_t rest = kBranchStart + sizeof kNetTimerBranch;
    std::memset(image.at<unsigned char>(rest), 0xCC, kBranchEnd - rest);
    log("GetDeltaSeconds (0x%llx): net-timer scripts use the realtime clock delta (clocks[7], PS3 net clock)",
        (unsigned long long)kGetDeltaSeconds);
    installed += 1;
}

}  // namespace u2mp
