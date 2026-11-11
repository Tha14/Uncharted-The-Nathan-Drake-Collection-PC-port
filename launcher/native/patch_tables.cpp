// patch_tables.cpp (built into uncharted_native.dll): the code changes the game starter makes to each game's linked
// image, ported from tools/probe_uncharted.py. Which of them apply depends on the launcher's settings, which arrive as
// UNCHARTED_* environment variables; patcher.cpp does the actual writing.
//
// Every patch has a description and a list of variants. A variant is either (pattern, offset into the pattern, bytes
// expected there, bytes to write) or, when several spots must change together or not at all, (pattern, [(offset,
// expected bytes, new bytes), ...]). The patcher uses the first variant whose pattern matches at exactly one place in
// the image; "??" in a pattern stands for any byte. Variants exist for game versions 1.00 and 1.02, because update 1.02
// rebuilt all three executables. Image offsets quoted below are for 1.02 unless a note names another version.

#include "native.h"
#include "json.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

using json::Value;

// ------------------------------------------------------------------------------------------------ helpers

struct Edit {
    long long offset;
    std::string original, replacement;
};
struct Variant {
    std::string pattern;
    bool several = false;   // true: a pattern with a list of edits
    long long offset = 0;   // used when there is a single change
    std::string original, replacement;
    std::vector<Edit> edits;
};
struct Patch {
    std::string description;
    std::vector<Variant> variants;
};
using Patches = std::vector<Patch>;
struct Failure { std::string message; };

int hexdigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// hex text to bytes; spaces are skipped
std::string B(const std::string &hex) {
    std::string t, out;
    for (char c : hex) if (c != ' ') t += c;
    for (size_t i = 0; i + 1 < t.size(); i += 2) out += (char)(hexdigit(t[i]) * 16 + hexdigit(t[i + 1]));
    return out;
}
std::string i32(long long v) {
    uint32_t u = (uint32_t)(int32_t)v;
    std::string s(4, '\0');
    for (int i = 0; i < 4; ++i) s[i] = (char)(u >> (8 * i));
    return s;
}
std::string f32(double v) {
    float f = (float)v;
    std::string s(4, '\0');
    std::memcpy(&s[0], &f, 4);
    return s;
}
std::string rep(char c, size_t n) { return std::string(n, c); }
std::string cc(size_t n) { return rep('\xcc', n); }

Variant one(const std::string &pattern, long long offset, const std::string &original, const std::string &replacement) {
    Variant v;
    v.pattern = pattern;
    v.offset = offset;
    v.original = original;
    v.replacement = replacement;
    return v;
}
Variant several(const std::string &pattern, std::vector<Edit> edits) {
    Variant v;
    v.pattern = pattern;
    v.several = true;
    v.edits = std::move(edits);
    return v;
}
Patch patch(const std::string &description, std::vector<Variant> variants) { return Patch{description, std::move(variants)}; }

std::string env(const char *name, const char *fallback) {
    char buf[4096];
    SetLastError(0);
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    if (!n) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? fallback : "";
    return n < sizeof buf ? std::string(buf, n) : fallback;
}
std::string strip(const std::string &s) {
    const char *ws = " \t\r\n\v\f";
    size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// --------------------------------------------------------------------------------------------- patches for every game

// Each game waits for the GPU's end-of-pipe stamp of a frame, but stops waiting after 2.0 s (vucomiss against a 2.0f,
// then ja). A PS4 never stalls that long; a PC can, while it compiles shaders or reads data back. Once that timeout
// hits, 1.00 later fails FrameBegin's m_gfxEopTick assert, which checks the stamp of frame N-3 (fg-draw-mgr.cpp:234; U1
// 0x3fd816), and 1.02 stops on the spot (int 0x41; U1 0x4fdf96). The fix turns the timeout `ja` into NOPs. All three
// games share this loop; they differ only in the rip-relative displacement of the 2.0f constant.
Patch gpu_wait() {
    return patch("GPU frame wait: no 2 s timeout",
                 {one("c5f258c0 c5f82e05 ???????? 7739 f390 beffffffff", 12, B("7739"), B("9090")),
                  one("c5f258c0 c5f82e05 ???????? 7710 f390 498b00", 12, B("7710"), B("9090"))});
}

Patches base_patches(const std::string &exe) {
    if (exe == "eboot.bin")
        return {gpu_wait(),
                // Uncharted 1 plays no sound effects (steps, shots, ambience) while music and dialogue work. The loader
                // for the sound config (biglib audio acm 0x515a50) opens reverbs/mixers/environments/prototypes.txt via
                // the file callback 0x8ef80 with flag 1, and the `testb $5,%cl` at 0x8f1da maps that flag to O_CREAT.
                // On the read-only /app0 our Fios2 refuses a READ|CREAT open (SCE_FIOS_ERROR_READ_ONLY, which becomes
                // EROFS) before it ever reaches the kernel, so none of the sound prototypes load. Testing bit 2 alone
                // lets a plain read request open the file read-only. Both 1.00 and 1.02.
                patch("sound config: opened read-only, without O_CREAT on /app0",
                      {one("4a8d542b14f6c1050f95c00fb6c0c1e0068db000020000f6c106", 5, B("f6c105"), B("f6c104"))})};
    return {gpu_wait()};
}

// In Uncharted 2 the player's torch light follows the camera rather than the torch. The torch update (Flashlight Active
// 0x69a190) first points the light along the torch joint; after that, on every frame, the player light (entry 0 of the
// light table at 0x1157760) is pointed along the camera's forward vector again, both by function 0x698b50 and by an
// inlined copy beside UpdatePadLightBar (0x3fd78d). So when the camera looks at Nate, the light lands on the wall
// behind him. Both re-aims are skipped and the joint path is used for all camera modes, so the light goes where the
// torch, and the beam drawn from it, points. UNCHARTED_TORCH_FORWARD=0 leaves the game unchanged. 1.02 only: the code
// is arranged differently in 1.00.
Patches torch_forward() {
    return {patch("Uncharted 2 torch: keeps its joint aim (per-frame copy)",
                  {one("488d05cc9fd500 8b4850 84c9 7440 8a50", 12, B("7440"), B("eb40"))}),
            patch("Uncharted 2 torch: keeps its joint aim (flashlight.cpp)",
                  {one("488d0509ecab00 8b4850 84c9 7440 8a50", 12, B("7440"), B("eb40"))}),
            patch("Uncharted 2 torch: joint aim used in all camera modes",
                  {one("833df040de0009 0f8534010000 4885c00f842b010000", 7, B("0f8534010000"), B("90e934010000"))})};
}

// Sleeping idle job workers (1.02, all three games). The engine has 6 NdJobWorkerThreads, and they busy-wait on their
// queues using `pause` unless the sleep flag of the job manager (byte +0x59, set through its own SetSleepMode) is on.
// With the flag on, a worker that has nothing to do waits on a condition variable owned by the manager once it has spun
// 1000 times, and each submitted job wakes the workers again. A PS4 gives every worker a core of its own; a PC with 4
// cores saw those six busy workers eat 390% out of 400% CPU (19-27 FPS on the title screen with an RTX 5080). The patch
// makes the flag read as on in three places: the worker loop, the check just before waiting, and the submit path. A
// different retail 1.02 build (EP9000-CUSA02344, Supermedo/uncharted_pc #346) has the same sites with different
// registers, so each site lists both forms. UNCHARTED_JOB_SLEEP=0 leaves the game unchanged.
Patches job_sleep() {
    return {patch("job workers sleep when idle (worker loop)",
                  {one("4d8d7001 807f5900 74?? 4981f8e8030000 7c??", 8, B("74"), B("9090"))}),
            patch("job workers sleep when idle (check before the wait)",
                  {one("498b07 80785900 74?? 4889c7 4883ef80", 7, B("74"), B("9090")),
                   one("498b4500 80785900 74?? 4889c7 4883ef80", 8, B("74"), B("9090"))}),
            patch("job workers sleep when idle (submitting a job wakes them)",
                  {one("80785900 0f84????0000 4883c060", 4, B("0f84"), B("660f1f440000")),
                   one("807f5900 74?? 4883c760 beffffffff", 4, B("74"), B("9090"))})};
}

// The in-game setting Options > Controls > Aiming > Sensitivity (1.02) is a slider with 10 notches spanning 0 to 1.0,
// and the aiming code asserts that the value never exceeds 1. Because mouse aiming is fed through the stick input, that
// limit also caps how fast the mouse can turn. The patch removes the check and raises the slider's maximum.
// UNCHARTED_AIM_RANGE=0 leaves the sliders as they were.
Patches aim_range(const std::string &exe) {
    if (exe == "eboot.bin")   // range check at 0x276308; slider data (0x1453e4) set to middle 2.5, top 5.0
        return {patch("Uncharted 1 aim sensitivity can exceed 1 (range check on aim speed)",
                      {one("c5f82e05 ???????? 763d c5fa1145f4", 8, B("763d"), B("eb3d"))}),
                patch("Uncharted 1 aim sensitivity slider reaches 10",
                      {one("488d0dd5738000 48894818 48c740200a000000 48b9 0000003f0000803f 48894828", 21,
                           B("0000003f0000803f"), B("0000a04000002041"))})};
    if (exe == "big2-ps4_Shipping.elf")   // range check inside 0x719d80; the slider entry (0x5d25b1) now refers to a 5.0
        return {patch("Uncharted 2 aim sensitivity can exceed 1 (range check on aim speed)",
                      {one("c5f82e05 ???????? 7649 488b05", 8, B("7649"), B("eb49"))}),
                patch("Uncharted 2 aim sensitivity slider reaches 10",
                      {one("81f9d6c2ba1a 0f85???????? 4989ff 4c8d35???????? c5fa1005 75fa8800", 26, B("75fa8800"),
                           B("4de08800"))})};
    // UpdateAiming3Settings (0x927fe5); value <-> notch conversions (0x62bf60, 0x62c090) top out at 4.0
    if (exe == "big3-ps4_Shipping.elf")
        return {patch("Uncharted 3 aim sensitivity can exceed 1 (range check on aim speed)",
                      {one("c5fa1015???????? c5f82ed0 7351 488b05", 12, B("7351"), B("eb51"))}),
                patch("Uncharted 3 aim sensitivity slider reaches 10 (value -> notch)",
                      {one("81f9d6c2ba1a7547 488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b", 8,
                           B("488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b"),
                           B("6b05dd63190140488d0df6d71c014801c8f30f590d13c1b4ffeb2c90"))}),
                patch("Uncharted 3 aim sensitivity slider reaches 10 (notch -> value)",
                      {one("3dd6c2ba1a7520 488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36", 7,
                           B("488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36"),
                           B("6b0da662190140488d15bfd61c014801d1c5f828daf30f590dd8bfb4ffeb3790"))})};
    return {};
}

// FrameBegin (fg-draw-mgr.cpp; all three games, 1.02) releases the instances of frame N-3 and asserts that the GPU
// end-of-pipe timestamp of that frame has been written (m_gfxEopTick at +0xa28, at +0xa08 in Uncharted 1). Crash logs
// from players (GitHub #11/#12/#17) hit it after stalls of 40-55 ms, when the timestamp simply had not arrived yet; a
// PS4's GPU never falls that far behind. The 59-byte assert path is replaced by a wait: spin with pause until the
// timestamp is non-zero, give up after 50 million rounds, and continue either way. With UNCHARTED_FRAME_WAIT=0 the
// assert stays as it was.
std::string frame_begin_wait(long long offset) {
    std::string code = B("488dbc08") + i32(offset)   // lea rdi, [rax + rcx + offset]
                       + B("be80f0fa02")             // mov esi, 50000000
                       + B("48833f00")               // loop: cmp qword ptr [rdi], 0
                       + B("7506")                   // jne done
                       + B("f390")                   // pause
                       + B("ffce")                   // dec esi
                       + B("75f4")                   // jnz loop
                       + B("eb20");                  // done: jmp past the check to its normal continuation
    return code + cc(59 - code.size());
}
Patches frame_wait(const std::string &exe) {
    std::string code;
    long long offset = 0xa28;
    if (exe == "eboot.bin") {
        code = "4883bc08080a000000753b488b057035be004885c0742d488d3dd2053400488d35710434004889d3"
               "488d15fd053400488d0dd01c350041b8ea0000004531c9ffd04889dacd41";
        offset = 0xa08;
    } else if (exe == "big2-ps4_Shipping.elf") {
        code = "4883bc08280a000000753b488b05609b99004885c0742d488d3dad5f2e00488d354c5e2e00"
               "4889d3488d15d85f2e00488d0d38ff210041b8ea0000004531c9ffd04889dacd41";
    } else if (exe == "big3-ps4_Shipping.elf") {
        code = "4883bc08280a000000753b488b05f06edb004885c0742d488d3d7df56400488d351cf46400"
               "4889d3488d15a8f56400488d0d6409570041b8ea0000004531c9ffd04889dacd41";
    } else {
        return {};
    }
    return {patch("FrameBegin: waits for the GPU timestamp of frame N-3 instead of the m_gfxEopTick assert",
                  {one(code, 11, B(code).substr(11), frame_begin_wait(offset))})};
}

// Player::UpdateFocus in Uncharted 3 (player.cpp:1897, 0xbcee10) and PlayerBase::UpdateFocus in Uncharted 2
// (player-base.cpp:1101, 0x7272e0) move the point Nate looks at towards its target with a spring; an assert then fires
// when that point has become NaN (GitHub #13, #24, #148). A NaN point never recovers, and the reset the function
// already has is never taken for it, since every comparison with NaN is false. Now the assert path runs that reset
// whenever the target is a real number; only a NaN target still reaches the assert. Covers 1.02 and 1.00 (the full 1.00
// byte strings are found once, and only in that version's executable). UNCHARTED_FOCUS_RESET=0 leaves the game
// unchanged.
const char *U3_FOCUS_100 = "c4e37904cb00c5f0c2c900c5f850c183f80f7438e8c3c110004885c07427488d3dd698c100"
                           "488d350595c100488d15e598c100488d0d3b95c10041b8690700004531c9ffd0cd41";
const char *U2_FOCUS_100 = "c4e37904c900c5f0c2c900c5f850c183f80f7438e8a8d71a004885c07427488d3d4c9c8300"
                           "488d354b998300488d155b9c8300488d0d7e99830041b84d0400004531c9ffd0cd41";
Patches focus_nan(const std::string &exe) {
    if (exe == "big3-ps4_Shipping.elf")
        return {patch("Uncharted 3 player focus: a NaN point is reset, no assert",
                      {one("c4e37904cb00c5f0c2c900c5f850c183f80f743a488b05f73302014885c07427488d3d74ff8900"
                           "488d3546fd8900488d1583ff8900488d0d6ece7d0041b86a0700004531c9ffd0cd41",
                           20,
                           B("488b05f73302014885c07427488d3d74ff8900488d3546fd8900488d1583ff8900"
                             "488d0d6ece7d0041b86a0700004531c9ffd0cd41"),
                           B("c5d057edc5f8284580c5f8c2c800c5f850c183f80f0f84a5feffffcd41") + cc(24)),
                       // 1.00 (GitHub #148): here the function lives at 0x390330. The assert path (0x3908b8) spans 51
                       // bytes, the target is stored at rbp-0xa0, and the reset code at 0x390779 keeps the velocity in
                       // xmm4.
                       one(U3_FOCUS_100, 20, B(U3_FOCUS_100).substr(20),
                           B("c5d857e4c5f8288560ffffffc5f8c2c800c5f850c183f80f0f84a3feffffcd41") + cc(19))})};
    if (exe == "big2-ps4_Shipping.elf")
        return {patch("Uncharted 2 player focus: a NaN point is reset, no assert",
                      {one("c4e37904c900 c5f0c2c900 c5f850c1 83f80f 743a 488b05???????? 4885c0 7427 488d3d????????"
                           " 488d35???????? 488d15???????? 488d0d???????? 41b84d040000 4531c9 ffd0 cd41",
                           20,
                           B("488b05e267e4004885c07427488d3d7d4c7600488d352c4b7600488d158c4c7600"
                             "488d0dbdcb6c0041b84d0400004531c9ffd0cd41"),
                           B("c5f8284da0c5f0c2d100c5f850c283f80f7511c5e857d2c5f82993c0020000"
                             "e9e7feffffcd41") + cc(15)),
                       // 1.00 (taken from the first Uncharted 2 log in GitHub #24): function at 0x33b4f0, its assert
                       // path at 0x33ba13
                       one(U2_FOCUS_100, 20, B(U2_FOCUS_100).substr(20),
                           B("c5f8284da0c5f0c2d100c5f850c283f80f7511c5e857d2c5f82993c0020000"
                             "e9c2feffffcd41") + cc(13))})};
    return {};
}

// 1 Nov 2026, GitHub #293: "pAction != NULL" in Uncharted 1 and 2 at high frame rates. The player's move actions are
// allocated from a bitmap pool created with 64 slots, and a burst at 120-144 FPS uses them all up; the pool now gets
// 256. UNCHARTED_MOVE_POOL=0 turns this off.
Patch move_action_pool() {
    return patch("move action pool of the player: 256 entries (#293)",
                 {one("ba06000000 b940000000 41b820000000", 5, B("b940000000"), B("b900010000"))});
}
// Uncharted 2, chapter 21 (the convoy), 4 Nov 2026: after a bump, Elena's truck sinks into the snow. When an animation
// node is added, a bool that the convoy's play call never sets is shifted left by 2 (flag << 2) and stored into the
// flags of the node before it, and leftover stack bytes there also switched on "the reference frame follows the
// object". Now only bit 0 of that bool is used. UNCHARTED_CONVOY_SINK=0 turns this off.
Patch convoy_sink() {
    return patch("Uncharted 2 convoy: animation node flag uses only bit 0 of the bool",
                 {one("11 8a9080000000 c0e102 80e2fb 08ca 889080000000", 1,
                      B("8a9080000000c0e10280e2fb08ca889080000000"), B("80e101c0e10280a080000000fb08888000000090"))});
}

// gow3, 5 Nov 2026, Uncharted 2: the leg IK code asserts IsFinite(deltaTransform) (character-leg-ik.cpp:391) when the
// root delta is NaN, which happens at the train wreck in chapter 1. The assert path now sets the delta to zero for that
// frame and resumes at the same place the finite case does. 1.02 and 1.00. UNCHARTED_LEG_IK_RESET=0 skips it.
Patches leg_ik(const std::string &exe) {
    if (exe != "big2-ps4_Shipping.elf") return {};
    return {patch("Uncharted 2 leg IK: NaN root delta set to zero, no assert",
                  {one("488b05b8cb0f01 4885c0 7427 488d3df8a2a000 488d35c3a1a000 488d15d486a500 488d0d932f9800"
                       " 41b887010000 4531c9 ffd0 cd41 c4c1782807",
                       0,
                       B("488b05b8cb0f014885c07427488d3df8a2a000488d35c3a1a000488d15d486a500"
                         "488d0d932f980041b8870100004531c9ffd0cd41c4c1782807"),
                       B("c5f857c0c4c1782907eb2f") + cc(47)),
                   one("e8a3cbbfff 4885c0 7427 488d3d1bfe2500 488d3592fc2500 488d1506fe2500 488d0dd1fc2500"
                       " 41b887010000 4531c9 ffd0 cd41",
                       0,
                       B("e8a3cbbfff4885c07427488d3d1bfe2500488d3592fc2500488d1506fe2500"
                         "488d0dd1fc250041b8870100004531c9ffd0cd41"),
                       B("c5f857c0c4c1782907eb2d") + cc(40))})};
}

// GitHub #205 and #304 (Uncharted 1) plus #326 (Uncharted 3), 1 Nov 2026: a "read access to 0000000000000020" crash,
// always in one engine job that exists in all three games. The code that creates the job hands it {object,
// object->+0x78}; if the object's descriptor has not been filled in yet, that second value is null and the job
// dereferences address 0x20. The two loads are replaced by a jump to a stub: with a null pointer it leaves the job for
// this frame through the function's normal exit path, otherwise it carries on exactly as before. The stub lives in an
// unreachable assert path of that exit, and that path still begins with int 0x41. UNCHARTED_JOB_PARAMS_GUARD=0 turns
// this off.
Patches job_params_guard(const std::string &exe) {
    std::string pattern, cave_original;
    long long start, loads, landing, cave;
    if (exe == "eboot.bin") {
        pattern = "49c7424800000000488b05a4b6b80049894210498b4208488b482049894a18488b4818";
        start = 0x616aed; loads = 0x616b00; landing = 0x6175ab; cave = 0x617602;
        cave_original = "488b05efe6a9004885c07427488d3d2ce91e00488d35c4e7";
    } else if (exe == "big2-ps4_Shipping.elf") {
        pattern = "49c7424800000000488b0514e4940049894210498b4208488b482049894a18488b4818";
        start = 0xd24ffd; loads = 0xd25010; landing = 0xd25ab9; cave = 0xd25b10;
        cave_original = "488b05618484004885c07427488d3dfa731800488d35ef71";
    } else if (exe == "big3-ps4_Shipping.elf") {
        pattern = "49c7424800000000488b05c4c3d60049894210498b4208488b482049894a18488b4818";
        start = 0xf88f5d; loads = 0xf88f70; landing = 0xf89a19; cave = 0xf89a70;
        cave_original = "488b05418dc6004885c07427488d3d08414f00488d35fd3e";
    } else {
        return {};
    }
    long long stub_at = cave + 2;                                         // right behind the int 0x41 that stays
    std::string stub = B("cd41 498b4208 4885c0");                         // int 0x41; mov rax,[r10+8]; test rax,rax
    stub += B("0f84") + i32(landing - (cave + (long long)stub.size() + 6));   // jz to the exit path
    stub += B("488b4820");                                                // mov rcx, [rax+0x20]
    stub += B("e9") + i32((loads + 8) - (cave + (long long)stub.size() + 5));  // jmp back behind the replaced loads
    std::string jump = B("e9") + i32(stub_at - (loads + 5)) + B("909090");
    return {patch("Engine job: a job with no descriptor yet is skipped, not read through null (#205 #304 #326)",
                  {several(pattern, {{loads - start, B("498b4208488b4820"), jump},
                                     {cave - start, B(cave_original), stub}})})};
}

// GitHub #284, 28 Oct 2026 (Uncharted 3, "Stay in the Light", 38 minutes in): CreateOrthographicProjection's
// "zscale*n+zoffset" assert fires (render-camera.h:441; its int 0x41 is at 0xf77095). If a light camera's direction is
// zero or points exactly along +X or -X, the unguarded rsqrt yields NaN light matrices. The call at 0xf3cad8 now goes
// to a stub that tests both light matrices (8 rows at rbp-0x1b0) and the far value for NaN or infinity. When everything
// is finite it continues to the ortho builder at 0xf768d0 as before; otherwise that light camera is not updated this
// frame and keeps the matrices of the last good frame. The stub occupies the unreachable path of the far-plane assert
// next to it (0xf770b7, 171 bytes), which still begins with int 0x41. UNCHARTED_LIGHT_ORTHO_GUARD=0 skips it.
Patch u3_light_ortho_guard() {
    std::string pattern = "c5fa100524ed5000 c5fa108d0cfeffff e8f39d0300 4c89e7 e84b422800";   // 0xf3cac8
    long long path = 0xf770b7 - 0xf3cac8, builder = 0xf768d0 - 0xf3cac8;
    std::string original = B(
        "488b05fab6c7004885c00f8499000000488d3dcf4c5300488d352a2252004989d6488d15a14c53004989dc"
        "4889cb488d0d6451430041b8ba0100004531c9c57829adf0feffffc57829b5e0feffffc57829bdd0feffff"
        "c5f82995c0feffffc5f829a5b0feffffc5f829ada0feffffffd0c5f828ada0feffffc5f828a5b0feffff"
        "c5f82895c0feffffc57828bdd0feffffc57828b5e0feffffc57828adf0feffff4889d94c89e34c89f2cd41");
    std::string stub = B("cd41");                                      // int 0x41, still the first instruction of the old path
    stub += B("c5f8289d50feffff c5e05cdb");                            // xmm3 = row0 - row0: NaN if row0 holds a NaN/inf
    for (int row = 1; row < 8; ++row) {                                // rows 1 to 7: add (row - row)
        stub += B("c5f828a5") + i32(-0x1b0 + 0x10 * row);              // vmovaps xmm4, [rbp-..]
        stub += B("c5d85ce4 c5e058dc");                                // vsubps xmm4,xmm4,xmm4; vaddps xmm3,xmm3,xmm4
    }
    stub += B("c5f25ce1 c5e258dc");                                    // add (far - far), far being xmm1
    stub += B("c5e0c2db03 c5f850c3 85c0 7505");                        // a NaN in any lane -> not finite
    stub += B("e9") + i32(builder - (path + (long long)stub.size() + 5));   // finite: jmp to 0xf768d0
    stub += B("4883042408 c3");                                        // not finite: return to after the next call
    std::string call = B("e8") + i32((path + 2) - (0x10 + 5));
    return patch("Uncharted 3 light camera: NaN ortho projection skipped, no assert (#284)",
                 {several(pattern, {{0x10, B("e8f39d0300"), call}, {path, original, stub + cc(original.size() - stub.size())}})});
}
// Testing aid for the #284 guard (UNCHARTED_TEST_LIGHT_ORTHO_NAN=1; the launcher never sets it): each light camera
// that is active is given a zero direction, and the gate in the caller is forced open so the camera gets built on each
// frame.
Patch u3_test_light_ortho_nan() {
    return patch("Uncharted 3 TEST: light cameras get a zero direction (UNCHARTED_TEST_LIGHT_ORTHO_NAN)",
                 {one("c5e8591db41d4f00 c5e059c9 c5e85cc9 c5f859c1 c5f8298510feffff", 16, B("c5f859c1"), B("c5f857c0"))});
}
Patch u3_test_light_ortho_gate() {
    return patch("Uncharted 3 TEST: light cameras rebuilt on each frame (UNCHARTED_TEST_LIGHT_ORTHO_NAN)",
                 {several("488b8360010000 c5f82e8010050000 7436 c5fa1005e94f5600 488b8360010000 c5f82e80680a0000 731d",
                          {{15, B("7436"), B("9090")}, {40, B("731d"), B("9090")}})});
}

// Drake's Fortune's most frequent crash, "execute access to 0" (GitHub #27, #30, #42, #62; Uncharted 2 and 3 contain
// the same code). A leaf function that fills a shader's constant table stores its loop indices underneath rsp. These
// tables sit beside GPU data, so a store halfway through can land on a write-protected page; Windows then builds the
// exception frame on that same stack, an index gets overwritten with 0, and a later walk calls through a null member.
// The function's entry now branches to a stub, which writes once to every page of the table (lock or byte [p], 0)
// before anything is kept under rsp, so a protected page faults at that point, where it does no harm. The stub lives in
// an unreachable assert path close by, which still begins with int 0x41. UNCHARTED_CUE_PRETOUCH=0 turns this off.
const char *CUE_TABLE_FILL = "55 41 57 41 56 41 55 41 54 53 49 b9 00 24 00 0d 10 00 00 00 48 c7 07 00 00 00 00 45 31 f6";
Variant cue_pretouch_variant(long long cave, const std::string &original_hex) {
    std::string stub = B("cd41"            // int 0x41: once the end of the assert path, now its first instruction
                         "89d0"            // mov eax, edx                 entry count
                         "486bc030"        // imul rax, rax, 0x30
                         "488d440727"      // lea rax, [rdi + rax + 0x27]  last byte of the table
                         "4889f9"          // mov rcx, rdi
                         "f0800900"        // touch: lock or byte [rcx], 0
                         "6681c9ff0f"      // or cx, 0xfff
                         "48ffc1"          // inc rcx                      -> first byte of the following page
                         "4839c1"          // cmp rcx, rax
                         "76ef"            // jbe touch
                         "5541574156");    // the instructions the entry jump displaced
    stub += B("e9") + i32(5 - (cave + (long long)stub.size() + 5));   // jmp back to offset 5 of the function
    std::string original = B(original_hex);
    return several(CUE_TABLE_FILL, {{cave, original, stub + cc(original.size() - stub.size())},
                                    {0, B("5541574156"), B("e9") + i32(cave + 2 - 5)}});
}
Patches cue_pretouch(const std::string &exe) {
    const char *description = "constant table fill: its pages are touched first, so no fault inside the fill";
    if (exe == "eboot.bin")
        return {patch(description, {cue_pretouch_variant(0xfd7a, "488b056780a6004885c07427488d3de3671d00488d35f8671d00488d1524681d00"
                                                                  "488d0dca671d0041b8ad0100004531c9ffd0cd41")})};
    if (exe == "big2-ps4_Shipping.elf")
        return {patch(description, {cue_pretouch_variant(-0xa850, "488b05d14181004885c07427488d3dac8e1500488d35e9801500488d15b28e1500"
                                                                   "488d0daca5090041b8e40a00004531c9ffd0cd41")})};
    if (exe == "big3-ps4_Shipping.elf")
        return {patch(description, {cue_pretouch_variant(-0xb690, "488b05517192004885c07427488d3d4c841b00488d35b4761b00488d1552841b00"
                                                                   "488d0dc80b0e0041b8e40a00004531c9ffd0cd41")})};
    return {};
}

// GitHub #61 #113 #115 #164 #181 #187, Uncharted 2 crashing with "read access to 0 at guest offset 0xd09665". The run
// grouping in the particle setup, a leaf function present in all three games, keeps its loop limits below rsp. When a
// page fault happens inside it, Windows writes the exception frame over them and the look-ahead reads one entry beyond
// the end. Where the function reloaded those limits, it now reads the same values out of r10 (the array) and r11 (the
// count), registers it never modifies. The new code has the same length and is patched in place.
// UNCHARTED_RUN_GROUP_FIX=0 turns this off.
Patch run_group() {
    return patch("particle run grouping: loop limits read from registers instead of below rsp",
                 {// 1.02: mov rcx,[rsp-8] becomes lea rcx,[r10+0x10]; nop and mov r15,[rsp-0x10] becomes lea r15,[r11-1]; nop
                  several("4c8b27 488b4c24f8 4a8d1c31 4c8b7c24f0 4929f7",
                          {{3, B("488b4c24f8"), B("498d4a1090")}, {12, B("4c8b7c24f0"), B("4d8d7bff90")}}),
                  // 1.00: mov rcx,[rsp-8] gets that same lea; mov rcx,[rsp-0x10] plus lea r8,[rcx+rsi] are replaced by
                  // code computing r8 = i + 1 - count
                  several("4c8b27 488b4c24f8 4a8d1c31 488b4c24f0 4c8d0431 4531ed",
                          {{3, B("488b4c24f8"), B("498d4a1090")}, {12, B("488b4c24f04c8d0431"), B("4989f04d29d849ffc0")}})});
}

// Everything unlocked: the "Unlock all chapters" option on the launcher's Advanced page (UNCHARTED_UNLOCK=1). Two tweak
// flags from Bluepoint get switched on, "Unlock Chapter Select by default" and "Unlock Brutal difficulty by default".
// The chapter flag is set at the spot where the *bp-ucctweaks* table is read, the Brutal flag where that table gets
// cleared (1.02; Uncharted 1 contains that constructor twice).
Patches unlock(const std::string &exe) {
    if (exe == "eboot.bin") {
        Patches p;
        for (auto c : {std::make_pair("1", "fe250000"), std::make_pair("2", "f51d0000")})
            p.push_back(patch(std::string("Uncharted 1: chapter select available (") + c.first + ")",
                              {one(std::string("e8") + c.second + " 4885c0 7411 8a08 888b302c0000 8a4001 8883322c0000", 18,
                                   B("8a40018883322c0000"), B("c683322c0000016690"))}));
        for (auto c : {std::make_pair("1", "ea3b0000"), std::make_pair("2", "e1330000")})
            p.push_back(patch(std::string("Uncharted 1: Brutal difficulty available (") + c.first + ")",
                              {one(std::string("e8") + c.second + " c683302c000001 c683312c000000", 12, B("c683312c000000"),
                                   B("c683312c000001"))}));
        return p;
    }
    if (exe == "big2-ps4_Shipping.elf")
        return {patch("Uncharted 2: chapter select available",
                      {one("833800 0f9583a02d0000 83780400 0f9583a22d0000", 10, B("837804000f9583a22d0000"),
                           B("c683a22d0000010f1f4000"))}),
                patch("Uncharted 2: Brutal difficulty available",
                      {one("c683a02d000001 c683a12d000000 c683a22d000000", 7, B("c683a12d000000"), B("c683a12d000001"))})};
    if (exe == "big3-ps4_Shipping.elf")
        return {patch("Uncharted 3: chapter select available",
                      {one("833800 0f9583602c0000 83780400 0f9583622c0000", 10, B("837804000f9583622c0000"),
                           B("c683622c0000010f1f4000"))}),
                patch("Uncharted 3: Brutal difficulty available",
                      {one("c683602c000001 c683612c000000 c683622c000000", 7, B("c683612c000000"), B("c683612c000001"))})};
    return {};
}

// Every reward unlocked (UNCHARTED_UNLOCK_REWARDS=1). In Uncharted 2 and 3 some rewards can only be switched on once
// the game has been beaten on the current difficulty; that condition is removed. Drake's Fortune stores the state of
// every reward in a record of its own, and its debug switch writes those records too, so there the checks are patched
// instead: each reward counts as unlocked, usable and included in the total, and the Story Cutscenes list leaves no
// chapter out. Nothing is written to the save. 1.02 code.
Patches rewards(const std::string &exe) {
    if (exe == "big2-ps4_Shipping.elf")
        return {patch("Uncharted 2 rewards: usable without beating the game first",
                      {one("7441c6430e01833d401fa800", 5, B("01"), B("00"))})};
    if (exe == "big3-ps4_Shipping.elf")
        return {patch("Uncharted 3 rewards: usable without beating the game first",
                      {one("74054c39f0725c41c6450e01488d05", 11, B("01"), B("00"))})};
    if (exe == "eboot.bin")
        return {// IsUnlocked(item): `and al, 1`, which tests status bit 0, becomes `mov al, 1`
                patch("Uncharted 1 rewards: all unlocked (per-item check)",
                      {one("b001803f00750d4885f674098a86880000002401", 18, B("24"), B("b0"))}),
                // the same change in IsUnlocked(category, id)
                patch("Uncharted 1 rewards: all unlocked (per-id check)",
                      {one("75134d85e4740c418a8424880000002401eb02", 15, B("24"), B("b0"))}),
                // the counter behind "Unlocked N of 47": `je skip` becomes `je +0`
                patch("Uncharted 1 rewards: all unlocked (unlocked count)",
                      {one("4c89f275154885c07422f68088000000017419", 18, B("19"), B("00"))}),
                // the check whether an id is usable: `je not` becomes `je +0`
                patch("Uncharted 1 rewards: all unlocked (usable-by-id check)",
                      {one("41803e00750af684088800000001741e488d55d4", 15, B("1e"), B("00"))}),
                // CanEnable: `sete dl` (dl set means locked) becomes `setb dl`, which gives 0 because test clears CF
                patch("Uncharted 1 rewards: all unlocked (CanEnable check)",
                      {one("41f68688000000010f94c24584ff0f95c184c0", 9, B("94"), B("92"))}),
                // the reason code CanEnable returns for "beat the game on this difficulty first": `mov r13b, 1` becomes
                // `mov r13b, 0`
                patch("Uncharted 1 rewards: usable without beating the game first",
                      {one("448bb87848000041b5014183ff067338", 9, B("01"), B("00"))}),
                // Story Cutscenes list: the `js skip` for chapters not reached yet becomes `js +0`
                patch("Uncharted 1 rewards: every story cutscene listed",
                      {one("488b7840837f50000f88880000004c8b4210", 10, B("88"), B("00"))})};
    return {};
}

// Cheats from UNCHARTED_CHEATS that need code changes. Uncharted 1 and 2 hold "Infinite Ammo" in their settings system,
// which writes that byte again on every frame, so a memory poke does not last. Instead, each reader of the flag
// (a `cmp byte [flag], 0; jne`) is changed to always jump, as though the flag were on. Drake's Fortune's own Infinite Ammo only
// keeps the reserve full, and every shot still removes a round from the magazine; the cheat removes that decrement as
// well. All other cheats are memory pokes (runtime_env.cpp).
Variant ammo_reader(const std::string &pattern, long long jne_at) { return one(pattern, jne_at, B("75"), B("eb")); }
Patches cheat_patches(const std::string &exe, const std::string &cheat) {
    if (cheat != "ammo") return {};
    Patches p;
    if (exe == "eboot.bin") {
        const std::pair<const char *, int> readers[] = {{"488d15caca8200 b101 803a00 75", 12}, {"488d0d79673800 803900 75", 10},
                                                        {"488d0d3a5b3800 803900 75", 10}, {"488d0d0a023800 803900 75", 10}};
        int n = 1;
        for (auto &r : readers)
            p.push_back(patch("Uncharted 1 infinite ammo: flag read " + std::to_string(n++) + " always on",
                              {ammo_reader(r.first, r.second)}));
        p.push_back(patch("Uncharted 1 infinite ammo: magazine not decremented",
                          {one("89f3 4989fe e8???????? 4885c0 741b 83b82801000000 7512 89d8 488d0440 48c1e004 "
                               "41ff8c0640040000 5b415e5dc3",
                               34, B("41ff8c0640040000"), B("0f1f840000000000"))}));
    } else if (exe == "big2-ps4_Shipping.elf") {
        const std::pair<const char *, int> readers[] = {
            {"488d0dbc8d6f00 80793a00 75", 11}, {"488d0d63736f00 80793a00 75", 11}, {"488d0d09436f00 80793a00 75", 11},
            {"488d0d98635600 80793a00 75", 11}, {"488d0d3b635600 80793a00 75", 11}, {"4c8d2d285f5600 41807d3a00 75", 12},
            {"e8fd3e2600 41807d3a00 75", 10}};
        int n = 1;
        for (auto &r : readers)
            p.push_back(patch("Uncharted 2 infinite ammo: flag read " + std::to_string(n++) + " always on",
                              {ammo_reader(r.first, r.second)}));
    }
    return p;
}

// ------------------------------------------------------------------------------------------------ ultrawide

// Ultrawide support (GitHub #20). At any aspect ratio the games keep the horizontal field of view of a 16:9 camera and
// cut off the top and bottom of the image instead (Vert-). The render camera keeps its zoom, 1/tan(horizontal FOV / 2),
// at +0x254 (+0x294 in Drake's Fortune) and its aspect, height/width, at +0x250 (+0x290 in Drake's Fortune). Wherever
// the projection or the culling planes of the view frustum load a perspective camera's zoom, that value is multiplied
// by min(1, aspect * 16/9). A screen wider than 16:9 therefore keeps the vertical view of the 16:9 image and gains
// picture at the sides (Hor+), while 16:9, narrower screens and square cameras stay exactly as they were. Each zoom
// load jumps out to a short stub placed in an unreachable assert branch of the projection code (every such branch now
// begins with int 0x41) and jumps back. This is applied only when the render size is wider than 16:9, and never with
// UNCHARTED_HOR_PLUS=0.
int gpr(const std::string &name) {
    static const char *names[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    for (int i = 0; i < 16; ++i) if (name == names[i]) return i;
    throw Failure{"unknown register " + name};
}
std::string nops(size_t n) {
    if (n == 3) return B("0f1f00");
    if (n == 4) return B("0f1f4000");
    if (n == 5) return B("0f1f440000");
    throw Failure{"no nop of " + std::to_string(n) + " bytes"};
}
// Encodes op xmm<dst>, xmm<src>, [base + disp32] for a scalar single (src is 0 for vmovss): a 2-byte VEX prefix when
// the base is rax..rdi, a 3-byte one for r8..r15, plus the SIB byte a base of rsp or r12 requires
std::string vex_mem(int op, int dst, int src, const std::string &base, long long disp) {
    int reg = gpr(base);
    std::string modrm(1, (char)(0x80 | dst << 3 | (reg & 7)));
    if ((reg & 7) == 4) modrm += '\x24';
    std::string prefix = reg < 8 ? std::string{(char)0xc5, (char)(0x80 | (~src & 15) << 3 | 2)}
                                 : std::string{(char)0xc4, (char)0xc1, (char)((~src & 15) << 3 | 2)};
    return prefix + (char)op + modrm + i32(disp);
}
// Encodes op xmm<dst>, xmm<src>, [rip-relative target] as an 8-byte instruction located at `at`
std::string vex_rip(int op, int dst, int src, long long at, long long target) {
    return std::string{(char)0xc5, (char)(0x80 | (~src & 15) << 3 | 2), (char)op, (char)(dst << 3 | 5)} + i32(target - (at + 8));
}
struct Site { long long offset; std::string base; int dst, free; };
struct Cave { long long offset, size; std::string original_hex; };
// Builds one variant. Each site is a `vmovss xmm, [base + zoom]` load; each cave is an unreachable assert branch that
// receives stubs. All offsets count from where the pattern matched. A cave may come before the pattern or sit inside a
// neighbouring function, so its original bytes are verified as well.
Variant hor_plus_variant(const std::string &pattern, long long zoom, long long aspect, const std::vector<Site> &sites,
                         const std::vector<Cave> &caves) {
    struct CaveUse { long long offset, size; std::string original; long long used; };
    std::vector<CaveUse> cave_list;
    for (const Cave &c : caves) cave_list.push_back({c.offset, c.size, B(c.original_hex), 2});   // room starts behind the int 0x41
    struct Stub { long long at, site; std::string base; int dst, free; long long length; };
    std::vector<std::pair<long long, std::vector<Stub>>> stubs;   // grouped by cave, in the order the caves are first used
    std::vector<Edit> edits;
    for (const Site &s : sites) {
        long long length = (long long)vex_mem(0x10, s.dst, 0, s.base, zoom).size();
        long long size = 21 + 2 * length;
        CaveUse *cave = nullptr;
        for (CaveUse &c : cave_list) if (c.used + size + 8 <= c.size) { cave = &c; break; }
        if (!cave) throw Failure{"no cave room"};
        std::vector<Stub> *list = nullptr;
        for (auto &kv : stubs) if (kv.first == cave->offset) list = &kv.second;
        if (!list) { stubs.push_back({cave->offset, {}}); list = &stubs.back().second; }
        list->push_back({cave->offset + cave->used, s.offset, s.base, s.dst, s.free, length});
        cave->used += size;
    }
    for (const CaveUse &cave : cave_list) {
        std::vector<Stub> *list = nullptr;
        for (auto &kv : stubs) if (kv.first == cave.offset) list = &kv.second;
        if (!list) continue;
        long long k = cave.offset + cave.used, one_at = k + 4;
        std::string code = B("cd41");
        for (const Stub &st : *list) {
            long long mul_at = st.at + 8, min_at = mul_at + st.length, jmp_at = min_at + 8 + st.length;
            code += vex_rip(0x10, st.free, 0, st.at, k) +                   // vmovss free, [16/9]
                    vex_mem(0x59, st.free, st.free, st.base, aspect) +      // vmulss free, free, [aspect]
                    vex_rip(0x5d, st.free, st.free, min_at, one_at) +       // vminss free, free, [1.0]
                    vex_mem(0x59, st.dst, st.free, st.base, zoom) +         // vmulss dst, free, [zoom]
                    B("e9") + i32(st.site + st.length - (jmp_at + 5));      // jmp back to the site
            edits.push_back({st.site, vex_mem(0x10, st.dst, 0, st.base, zoom),
                             B("e9") + i32(st.at - (st.site + 5)) + nops((size_t)st.length - 5)});
        }
        edits.insert(edits.begin(), Edit{cave.offset, cave.original, code + f32(16.0 / 9.0) + f32(1.0)});
    }
    return several(pattern, edits);
}
Patches hor_plus_all(const std::string &exe) {
    if (exe == "eboot.bin")   // Drake's Fortune 1.02: render camera update at 0x2c9660, its frustum planes at 0x2ca4ca
        return {patch("Drake's Fortune ultrawide: Hor+ view, no cropping",
                      {hor_plus_variant("c5fa108b98020000 c5fa109b90020000 c5fa109394020000 c5f857c0 c5f82ec2 7251", 0x294, 0x290,
                                        {{0x10, "rbx", 2, 0}, {0x2a9, "rbx", 3, 0}, {0xcac, "rdi", 0, 1}},
                                        {{0x22, 0x51, "c5fa115dc0c5fa1155d0"}, {0x2c1, 0x63, "c5fa114db0c5fa115dc0"}})})};
    if (exe == "big2-ps4_Shipping.elf")
        return {// Uncharted 2 1.02: camera at 0x9dc750 (infinite far plane 0x9dc8fe, finite 0x9dcb55), frustum planes at
                // 0x9dd41d
                patch("Uncharted 2 ultrawide: Hor+ view, no cropping",
                      {hor_plus_variant("c5fa10ab58020000 c5fa109b50020000 c5fa10a354020000 c5f857c0 c5f82ec4 0f82b6000000",
                                        0x254, 0x250, {{0x10, "rbx", 4, 0}, {0x267, "rbx", 4, 0}, {0xb2f, "rdi", 0, 1}},
                                        {{0x26, 0xb6, "488b055d16b9004885c0"}})}),
                // Shadows outside the 16:9 area. The shadow culling frustums of the sun, of rain and of local lights
                // (flashlight, lamp) are derived from the render camera's own zoom, so with the 16:9 zoom any caster
                // beyond the 16:9 image was culled. Every stub goes into the numPlanes assert of its own function
                // (UNCHARTED_UW_SHADOWS=0 disables these).
                patch("Uncharted 2 ultrawide: sun shadows cover the full view width",
                      {hor_plus_variant("c5fa10b350020000 c5fa10ab54020000 c4637904ce00", 0x254, 0x250, {{0x8, "rbx", 5, 0}},
                                        {{-0x194, 0x35, "488b0573128a004885c0"}})}),
                patch("Uncharted 2 ultrawide: rain shadows cover the full view width",
                      {hor_plus_variant("c5fa10ab50020000 c5fa10a354020000 c4637904c500", 0x254, 0x250, {{0x8, "rbx", 4, 3}},
                                        {{-0x124, 0x35, "488b05a50e8a004885c0"}})}),
                patch("Uncharted 2 ultrawide: local light shadows cover the full view width",
                      {hor_plus_variant("c4c17a10942454020000 c4c17a10ac2450020000", 0x254, 0x250, {{0x0, "r12", 2, 5}},
                                        {{0x183, 0x35, "488b05c6018a004885c0"}})}),
                // GPU constants of the scene view (0xcfcb00): two further copies of the frustum code, one for the
                // render camera (r12) and one for a second view (r13). The stubs use the two SetConstantBuffers asserts
                // of that function and two of the ones in the next function, 0xd01020 (UNCHARTED_UW_VIEW_CONSTANTS=0
                // disables these).
                patch("Uncharted 2 ultrawide: view constants cover the full view width (1)",
                      {hor_plus_variant("c4c17a109c2454020000 c4417a10bc2450020000", 0x254, 0x250, {{0x0, "r12", 3, 5}},
                                        {{0x10e7, 0x38, "488b0549fa86004885c0"}})}),
                patch("Uncharted 2 ultrawide: view constants cover the full view width (2)",
                      {hor_plus_variant("c4c17a10842454020000 c57a100d???????? c5b25ec8", 0x254, 0x250, {{0x0, "r12", 0, 1}},
                                        {{0x2a24, 0x38, "488b0519da86004885c0"}})}),
                patch("Uncharted 2 ultrawide: view constants cover the full view width (3)",
                      {hor_plus_variant("c4c17a109d54020000 c4c17a10a550020000", 0x254, 0x250, {{0x0, "r13", 3, 4}},
                                        {{0x28d4, 0x35, "488b0504c486004885c0"}})}),
                patch("Uncharted 2 ultrawide: view constants cover the full view width (4)",
                      {hor_plus_variant("c4c17a108554020000 c5da5ec8", 0x254, 0x250, {{0x0, "r13", 0, 1}},
                                        {{0x2189, 0x35, "488b0568c186004885c0"}})})};
    // Uncharted 3: infinite far plane at 0x11c1107, finite at 0x11c11e1 and 0x11c1406, frustum at 0x11c1f21
    if (exe == "big3-ps4_Shipping.elf")
        return {patch("Uncharted 3 ultrawide: Hor+ view, no cropping",
                      {hor_plus_variant("80bb6902000000 c5fa10a358020000 c5fa108b50020000 c5fa109354020000 0f840a020000",
                                        0x254, 0x250,
                                        {{0x17, "rbx", 2, 3}, {0xf1, "rbx", 2, 0}, {0x316, "rbx", 1, 0}, {0xe31, "rdi", 0, 1}},
                                        {{0x10d, 0x9f, "488b05b415a3004885c0"}, {0x32e, 0x65, "488b059313a3004885c0"}})})};
    return {};
}
Patches hor_plus(const std::string &exe) {
    std::vector<std::string> skip;
    if (env("UNCHARTED_UW_SHADOWS", "1") == "0") skip.push_back("shadows cover the full view width");
    if (env("UNCHARTED_UW_VIEW_CONSTANTS", "1") == "0") skip.push_back("view constants cover the full view width");
    Patches out;
    for (Patch &p : hor_plus_all(exe)) {
        bool skipped = false;
        for (auto &s : skip) skipped = skipped || p.description.find(s) != std::string::npos;
        if (!skipped) out.push_back(p);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ launcher settings

// Tonemap constants for Uncharted 3 1.02 (pavlikdcx's patch from GitHub #199). The filmic exposure bias comes from
// [rbx+0x72c] in the render settings, loaded on two code paths (image 0xed9d30 and 0xed9d49); both now add <bias> EV
// before the tonemap. On the main path, the load is replaced by a jump into a stub that sits in the code segment's zero
// padding (image 0x1648800) and adds the bias. On the second path, the add of -0.5 (a shared constant at 0x144b3b8) is
// pointed at our own value of -0.5 + bias.
Patch u3_exposure_patch(double bias) {
    std::string pattern = "c5fa10932c070000 c5f057c9 c5fa100570165700 e9d1000000 c5fa10832c070000 c5fa58055f165700";
    long long cave = 0x1648800 - 0xed9d30;   // relative to the pattern's start
    std::string stub = B("c5fa10932c070000");                                         // vmovss xmm2, [rbx+0x72c]
    stub += B("c5ea5815") + i32(0x40 - ((long long)stub.size() + 8));                 // vaddss xmm2, xmm2, [cave+0x40]
    stub += B("c5f057c9");                                                            // vxorps xmm1, xmm1, xmm1
    stub += B("e9") + i32(12 - (cave + (long long)stub.size() + 5));                  // jmp back to pattern + 12
    stub += cc(0x40 - stub.size());
    stub += f32(bias) + f32(-0.5 + bias);
    std::string jump = B("e9") + i32(cave - 5) + rep('\x90', 7);
    std::string second = i32((cave + 0x44) - (0x21 + 8));                             // new displacement for the second path's add
    char description[96];
    std::snprintf(description, sizeof description, "Uncharted 3 tonemap exposure shifted by %+.2f EV (UNCHARTED_U3_EXPOSURE)",
                  bias);
    return patch(description, {several(pattern, {{0, B("c5fa10932c070000c5f057c9"), jump},
                                                 {cave, std::string(stub.size(), '\0'), stub},
                                                 {0x25, B("5f165700"), second}})});
}

// Worker count of the job manager (6) as passed in its init parameters. Uncharted 3 sets it in two places, identical
// apart from using r13 or r15.
Patches job_worker_patches(const std::string &exe, int count) {
    std::string tail = "48c7458800080000 48c7459000040000 48c74598";
    std::string replacement(1, (char)count);
    std::string description = "job workers: " + std::to_string(count) + " (default 6, UNCHARTED_JOB_WORKERS)";
    if (exe == "big3-ps4_Shipping.elf") {
        Patches p;
        int i = 0;
        for (const char *reg : {"ad", "bd"})
            p.push_back(patch(description + " (site " + std::to_string(i++) + ")",
                              {one(std::string("4c89") + reg + "68ffffff 48899d70ffffff " + tail + " 06000000 48c745a0", 34,
                                   B("06"), replacement)}));
        return p;
    }
    return {patch(description, {one(tail + " 06000000 48c745a0 80000000", 20, B("06"), replacement)})};
}

std::string game_key(const std::string &exe) {
    return exe == "eboot.bin" ? "u1" : exe == "big2-ps4_Shipping.elf" ? "u2" : exe == "big3-ps4_Shipping.elf" ? "u3" : "";
}
std::string upper(std::string s) {
    for (char &c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}
// Reads a whole decimal integer the way Python's int() does (optional sign, whitespace around it allowed); returns
// false when the text is not one
bool to_int(const std::string &text, long long &value) {
    std::string t = strip(text);
    size_t i = (t.size() && (t[0] == '+' || t[0] == '-')) ? 1 : 0;
    if (i >= t.size()) return false;
    for (size_t k = i; k < t.size(); ++k) if (!std::isdigit((unsigned char)t[k])) return false;
    value = std::strtoll(t.c_str(), nullptr, 10);
    return true;
}
std::string starter_setting(const char *what, const std::string &arg = "") {
    char buf[64];
    return un_starter(what, arg.c_str(), buf, sizeof buf) == 0 ? buf : "";
}

// Every code patch for this executable under the current settings, listed in the order they get applied.
Patches image_patches(const std::string &exe) {
    const bool u1 = exe == "eboot.bin", u2 = exe == "big2-ps4_Shipping.elf", u3 = exe == "big3-ps4_Shipping.elf";
    if (!u1 && !u2 && !u3) throw Failure{"unknown executable " + exe};
    Patches patches = base_patches(exe);
    auto add = [&](const Patches &more) { patches.insert(patches.end(), more.begin(), more.end()); };
    if (u2 && env("UNCHARTED_TORCH_FORWARD", "1") != "0") add(torch_forward());
    if (env("UNCHARTED_AIM_RANGE", "1") != "0") add(aim_range(exe));
    if (env("UNCHARTED_FRAME_WAIT", "1") != "0") add(frame_wait(exe));
    if (env("UNCHARTED_UNLOCK", "0") == "1") add(unlock(exe));
    if (env("UNCHARTED_UNLOCK_REWARDS", "0") == "1") add(rewards(exe));
    if (env("UNCHARTED_FOCUS_RESET", "1") != "0") add(focus_nan(exe));
    if (env("UNCHARTED_LEG_IK_RESET", "1") != "0") add(leg_ik(exe));
    if (env("UNCHARTED_JOB_PARAMS_GUARD", "1") != "0") add(job_params_guard(exe));
    if ((u1 || u2) && env("UNCHARTED_MOVE_POOL", "1") != "0") patches.push_back(move_action_pool());
    if (u2 && env("UNCHARTED_CONVOY_SINK", "1") != "0") patches.push_back(convoy_sink());
    // Testing aid for the job guard above (the launcher never sets it): in Uncharted 3 the code that creates the job
    // (0xf6da01) is made to write a null descriptor, so every such job sees the same null pointer as in the players'
    // crashes.
    if (u3 && env("UNCHARTED_TEST_JOB_PARAMS_NULL", "") == "1")
        patches.push_back(patch("Uncharted 3 TEST: job descriptor forced to null (UNCHARTED_TEST_JOB_PARAMS_NULL)",
                                {one("498b4500488b48784989042449894c2408813810", 4, B("488b4878"), B("31c99090"))}));
    if (u3) {
        if (env("UNCHARTED_LIGHT_ORTHO_GUARD", "1") != "0") patches.push_back(u3_light_ortho_guard());
        if (env("UNCHARTED_TEST_LIGHT_ORTHO_NAN", "") == "1") {
            patches.push_back(u3_test_light_ortho_nan());
            patches.push_back(u3_test_light_ortho_gate());
        }
    }
    if (env("UNCHARTED_CUE_PRETOUCH", "1") != "0") add(cue_pretouch(exe));
    if (env("UNCHARTED_RUN_GROUP_FIX", "1") != "0") patches.push_back(run_group());
    {   // cheats picked under launcher -> Cheats (UNCHARTED_CHEATS)
        std::string list = env("UNCHARTED_CHEATS", ""), t;
        list.erase(std::remove(list.begin(), list.end(), ' '), list.end());
        size_t start = 0;
        for (;;) {
            size_t cut = list.find(',', start);
            t = list.substr(start, cut == std::string::npos ? std::string::npos : cut - start);
            if (!t.empty()) add(cheat_patches(exe, t));
            if (cut == std::string::npos) break;
            start = cut + 1;
        }
    }
    // A render size wider than 16:9 (from the launcher's aspect ratio setting) gets the Hor+ view.
    std::string size = env(("UNCHARTED_RESOLUTION_" + upper(game_key(exe))).c_str(), "");
    if (size.empty()) size = env("UNCHARTED_RESOLUTION", "");
    for (char &c : size) c = (char)std::tolower((unsigned char)c);
    if (size.find('x') != std::string::npos && env("UNCHARTED_HOR_PLUS", "1") != "0") {
        size_t x = size.find('x');
        long long w = 0, h = 0;
        if (size.find('x', x + 1) != std::string::npos || !to_int(size.substr(0, x), w) || !to_int(size.substr(x + 1), h))
            throw Failure{"invalid resolution: " + size};
        if (h == 0) throw Failure{"invalid resolution: " + size};
        if ((double)w / (double)h > 16.0 / 9.0 + 0.01) add(hor_plus(exe));
    }
    if (u3) {
        double bias = std::strtod(starter_setting("u3_exposure").c_str(), nullptr);
        if (bias != 0.0) patches.push_back(u3_exposure_patch(bias));
    }
    int workers = std::atoi(starter_setting("job_workers").c_str());
    if (workers) add(job_worker_patches(exe, workers));
    if (starter_setting("job_sleep", exe) == "1") {
        add(job_sleep());
        // For diagnosis: UNCHARTED_JOB_SLEEP_SPINS=<n> sets how many idle spins a worker makes before it sleeps
        // (default 1000).
        std::string spins = strip(env("UNCHARTED_JOB_SLEEP_SPINS", ""));
        if (!spins.empty()) {
            long long n = 0;
            if (!to_int(spins, n) || n < 0 || n > 0xffffffffLL) throw Failure{"invalid UNCHARTED_JOB_SLEEP_SPINS: " + spins};
            std::string bytes(4, '\0');
            for (int i = 0; i < 4; ++i) bytes[i] = (char)(n >> (8 * i));
            patches.push_back(patch("job workers: idle spins before they sleep",
                                    {one("4d8d7001 807f5900 ???? 4981f8e8030000 7c??", 13, B("e8030000"), bytes)}));
        }
    }
    return patches;
}

std::string hex(const std::string &bytes) {
    static const char *d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) { out += d[c >> 4]; out += d[c & 15]; }
    return out;
}
// Converts the patches to the JSON layout that un_apply_patches expects
Value spec(const Patches &patches) {
    Value list;
    list.kind = Value::Array;
    for (const Patch &p : patches) {
        Value variants;
        variants.kind = Value::Array;
        for (const Variant &v : p.variants) {
            Value item;
            item.kind = Value::Array;
            item.items.push_back(Value::text(v.pattern));
            if (v.several) {
                Value edits;
                edits.kind = Value::Array;
                for (const Edit &e : v.edits) {
                    Value edit;
                    edit.kind = Value::Array;
                    edit.items = {Value::integer(e.offset), Value::text(hex(e.original)), Value::text(hex(e.replacement))};
                    edits.items.push_back(edit);
                }
                item.items.push_back(edits);
            } else {
                item.items.push_back(Value::integer(v.offset));
                item.items.push_back(Value::text(hex(v.original)));
                item.items.push_back(Value::text(hex(v.replacement)));
            }
            variants.items.push_back(item);
        }
        Value pair;
        pair.kind = Value::Array;
        pair.items = {Value::text(p.description), variants};
        list.items.push_back(pair);
    }
    return list;
}

int give(const std::string &s, char *out, int cap) {
    if ((int)s.size() + 1 > cap) return -(int)(s.size() + 1);
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

}  // namespace

extern "C" {

// Returns, as JSON in the format un_apply_patches reads, the code patches for this executable under the current
// settings. 0 on success; 1 when a setting is invalid (out then holds the reason).
UN_API int un_image_patches(const char *executable, char *out, int cap) {
    try {
        return give(json::dump(spec(image_patches(executable ? executable : ""))), out, cap);
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    }
}

// Writes this executable's patches, chosen by the current settings, into the linked image; out receives the log lines.
// Returns 0 when applied, 1 when not (the last line gives the reason), or a negative size when out is too small
// (nothing is written; call again with that size).
UN_API int un_apply_image_patches(const wchar_t *image, const char *executable, char *out, int cap) {
    std::string list;
    try {
        list = json::dump(spec(image_patches(executable ? executable : "")));
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    }
    return un_apply_patches(image, list.c_str(), out, cap);
}

}  // extern "C"
