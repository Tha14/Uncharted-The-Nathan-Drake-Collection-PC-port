// u2mp scripttime: the state-script track-group delta time of net-timer scripts (u2mp/docs/PORT-SCRIPTTIME.md).
//
// A state-script instance spawned with `is-net-timer` (spawn info +0x14 bit 0x10, set at 0xa2e831
// as process +0x168 bit 0x8; PS3 SsInstance m_bUseNetClock, process +264 bit 28) runs on the realtime
// clock clocks[7] (0x1116180+0x1f8) instead of the game clock clocks[0] (0x1116180+0x38). PS4 kept
// that choice everywhere (0xa2e3a0 GetClock, 0xa2dfd9, 0xa2f35a, 0xa44a7a, SsTrackGroup elapsed time
// 0xa46b90) except in the PS4-only SsTrackGroup::GetDeltaSeconds (0xa477c0), where the net-timer case
// is `ASSERT(0)` (ss-track-group.cpp line 667) and int 0x41: Warzone's net-timer scripts crashed the
// game 14 s into a match (test run 44). PS3 1.10 has no delta function (its track-group update
// 0x7dab50 only reads the elapsed time 0x7da99c, which for net timers goes through SsInstance's net
// clock time 0x7c6ed8 = clocks[7] ticks minus the NetGameManager start, both 1/300 s), so the net-timer
// delta is the frame delta of that same clock: clocks[7].delta_ticks (+0x18, written each frame by
// Clock::Update 0x476ee0 for all ten clocks at 0x4ca6df..0x4ca74c) times the 1/300 the function already
// uses (0xe6759c).
//
// The patch rewrites the assert block (0xa4785c, reached only when bit 0x8 is set) in place:
//   lea rax,[rip+..] (0x1116180); vcvtsi2ss xmm0,xmm0,[rax+0x210]; jmp 0xa4789f (vmulss 1/300; pop; ret)
// The game-clock path (0xa47893: [0x1116180+0x50] * 1/300) and the owner lookup are not touched.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

constexpr uint64_t fGetDeltaSeconds = 0xa477c0;
constexpr uint64_t cFlagTest = 0xa47853;    // test byte [rax+0x168],8; je 0xa47893
constexpr uint64_t cAssert = 0xa4785c;      // the net-timer case: assert handler call, int 0x41
constexpr uint64_t cAssertEnd = 0xa47893;   // the game-clock path
constexpr uint64_t cMulRet = 0xa4789f;      // vmulss xmm0,[0xe6759c]; pop rbp; ret
constexpr uint64_t kClocks = 0x1116180;
constexpr uint32_t kNetClockDelta = 0x1f8 + 0x18;   // clocks[7] (0x1f8) delta ticks (+0x18) = 0x210

bool install_net_timer_delta() {
    // 0xa47853..0xa478a9 as shipped: the flag test, the assert block, the game-clock return
    static const unsigned char expect[] = {
        0xf6, 0x80, 0x68, 0x01, 0x00, 0x00, 0x08,             // test byte [rax+0x168],8
        0x74, 0x37,                                           // je 0xa47893
        0x48, 0x8b, 0x05, 0x15, 0x67, 0xb2, 0x00,             // mov rax,[0x156df78] (assert handler)
        0x48, 0x85, 0xc0, 0x74, 0x27,                         // test rax,rax; je int 0x41
        0x48, 0x8d, 0x3d, 0x0a, 0x20, 0x48, 0x00,             // lea rdi,"0"
        0x48, 0x8d, 0x35, 0xa4, 0xda, 0x45, 0x00,             // lea rsi,"...ss-track-group.cpp"
        0x48, 0x8d, 0x15, 0xda, 0xda, 0x45, 0x00,             // lea rdx,"GetDeltaSeconds"
        0x48, 0x8d, 0x0d, 0xf0, 0xca, 0x3a, 0x00,             // lea rcx,""
        0x41, 0xb8, 0x9b, 0x02, 0x00, 0x00,                   // mov r8d,0x29b (line 667)
        0x45, 0x31, 0xc9, 0xff, 0xd0,                         // xor r9d,r9d; call rax
        0xcd, 0x41, 0x5d, 0xc3,                               // int 0x41; pop rbp; ret
        0x48, 0x8d, 0x05, 0xe6, 0xe8, 0x6c, 0x00,             // 0xa47893: lea rax,[0x1116180]
        0xc5, 0xfa, 0x2a, 0x40, 0x50,                         // vcvtsi2ss xmm0,xmm0,[rax+0x50]
        0xc5, 0xfa, 0x59, 0x05, 0xf5, 0xfc, 0x41, 0x00,       // vmulss xmm0,xmm0,[0xe6759c] (1/300)
        0x5d, 0xc3};                                          // pop rbp; ret
    static_assert(sizeof expect == 0xa478a9 - cFlagTest, "expected bytes span 0xa47853..0xa478a9");
    if (!image.executable(cFlagTest, sizeof expect) || !image.matches(cFlagTest, expect, sizeof expect)) {
        log("GetDeltaSeconds net timer (0x%llx): bytes differ, left alone", (unsigned long long)fGetDeltaSeconds);
        return false;
    }
    unsigned char code[] = {
        0x48, 0x8d, 0x05, 0, 0, 0, 0,                         // lea rax,[rip+rel] -> 0x1116180
        0xc5, 0xfa, 0x2a, 0x80, 0, 0, 0, 0,                   // vcvtsi2ss xmm0,xmm0,dword [rax+0x210]
        0xeb, 0};                                             // jmp short 0xa4789f
    const int32_t lea = int32_t(int64_t(kClocks) - int64_t(cAssert + 7));
    std::memcpy(code + 3, &lea, 4);
    std::memcpy(code + 11, &kNetClockDelta, 4);
    const int64_t back = int64_t(cMulRet) - int64_t(cAssert + sizeof code);
    static_assert(sizeof code == 17, "patch layout");
    if (back < -128 || back > 127) return false;
    code[16] = uint8_t(int8_t(back));
    unsigned char *c = image.base + cAssert;
    std::memcpy(c, code, sizeof code);
    std::memset(c + sizeof code, 0xcc, cAssertEnd - cAssert - sizeof code);   // the rest of the old assert
    log("GetDeltaSeconds (0x%llx): net-timer scripts use the realtime clock delta (clocks[7], PS3 net clock)",
        (unsigned long long)fGetDeltaSeconds);
    return true;
}

}  // namespace

void install_scripttime(int &installed) {
    if (install_net_timer_delta()) ++installed;
}

}  // namespace u2mp
