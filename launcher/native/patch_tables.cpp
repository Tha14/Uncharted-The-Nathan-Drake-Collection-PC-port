// patch_tables.cpp - the game starter's code patches (tools/probe_uncharted.py), part of uncharted_native.dll: what is
// patched into each game's linked image, chosen by the launcher's settings (UNCHARTED_* in the environment), applied by
// patcher.cpp.
//
// A patch lists variants (description, [(pattern, offset in pattern, original bytes, replacement bytes), ...]): the
// first variant whose pattern occurs exactly once is applied ("??" = any byte). The variants cover the game versions
// 1.00 and 1.02 (the update recompiled all three executables). A variant can also make several changes, all or none:
// (pattern, [(offset, original bytes, replacement bytes), ...]). Image offsets in the notes are 1.02's unless said.

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

// ------------------------------------------------------------------------------------------------ building blocks

struct Edit {
    long long offset;
    std::string original, replacement;
};
struct Variant {
    std::string pattern;
    bool several = false;   // the (pattern, [edits]) form
    long long offset = 0;   // the single-change form
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
// bytes from hex text (spaces ignored)
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

// ------------------------------------------------------------------------------------------------ all three games

// The wait for a frame's GPU end-of-pipe stamp gives up after 2.0 s (vucomiss [2.0f]; ja). On PC the command processor
// can pause that long (shader compilation, readbacks), never on a PS4. 1.00 then asserts m_gfxEopTick of frame N-3 in
// FrameBegin (fg-draw-mgr.cpp:234; U1 0x3fd816); 1.02 halts right there (int 0x41; U1 0x4fdf96). NOP the `ja` that
// leaves the loop on timeout. The three games have the same loop; only the rip-relative offset of the 2.0f differs.
Patch gpu_wait() {
    return patch("wait for GPU frame completion without the 2 s timeout",
                 {one("c5f258c0 c5f82e05 ???????? 7739 f390 beffffffff", 12, B("7739"), B("9090")),
                  one("c5f258c0 c5f82e05 ???????? 7710 f390 498b00", 12, B("7710"), B("9090"))});
}

Patches base_patches(const std::string &exe) {
    if (exe == "eboot.bin")
        return {gpu_wait(),
                // Silent sound effects (footsteps, gunfire, ambience; music and speech fine): the sound config loader
                // (biglib audio acm 0x515a50) opens reverbs/mixers/environments/prototypes.txt through the sound file
                // callback 0x8ef80 with flag 1, which `testb $5,%cl` at 0x8f1da turns into O_CREAT. Our Fios2 rejects
                // READ|CREAT on read-only /app0 (SCE_FIOS_ERROR_READ_ONLY, mapped to EROFS) before any kernel call, so
                // no sound prototypes load. Test bit 2 only: a read request opens read-only. (1.00 and 1.02)
                patch("sound config files open read-only (no O_CREAT on /app0)",
                      {one("4a8d542b14f6c1050f95c00fb6c0c1e0068db000020000f6c106", 5, B("f6c105"), B("f6c104"))})};
    return {gpu_wait()};
}

// Uncharted 2's player torch aims with the camera. The torch update (Flashlight Active 0x69a190) aims the light along
// the torch joint, and then, every frame, the player light (record 0 of the light table 0x1157760) is re-aimed along
// the camera's forward vector (function 0x698b50 and a copy inlined next to UpdatePadLightBar, 0x3fd78d): turn the
// camera to face Nate and the light falls on the wall behind him. Skip both re-aims and take the joint path in every
// camera mode: the light follows the torch, like the beam drawn from it. UNCHARTED_TORCH_FORWARD=0 keeps the original;
// 1.02 only (1.00 lays this code out differently).
Patches torch_forward() {
    return {patch("Uncharted 2 torch: no re-aim along the camera (frame update copy)",
                  {one("488d05cc9fd500 8b4850 84c9 7440 8a50", 12, B("7440"), B("eb40"))}),
            patch("Uncharted 2 torch: no re-aim along the camera (flashlight.cpp)",
                  {one("488d0509ecab00 8b4850 84c9 7440 8a50", 12, B("7440"), B("eb40"))}),
            patch("Uncharted 2 torch: aimed by its joint in every camera mode",
                  {one("833df040de0009 0f8534010000 4885c00f842b010000", 7, B("0f8534010000"), B("90e934010000"))})};
}

// Idle job workers sleep (all three games, 1.02). The engine's 6 NdJobWorkerThreads spin on their job queues with
// `pause` unless the job manager's sleep flag (byte +0x59, set by its own SetSleepMode) is on; with it on, an idle
// worker waits on the manager's condition variable after 1000 spins and every job submission wakes them. On the PS4
// each worker owns a core; on a 4-core PC the six spinning workers took 390% of 400% CPU (title screen 19-27 FPS on an
// RTX 5080). Treat the flag as on in the worker loop, its re-check before waiting, and the submit path. Another retail
// 1.02 build (EP9000-CUSA02344, Supermedo/uncharted_pc #346) has the same sites with other registers: each site takes
// whichever form matches. UNCHARTED_JOB_SLEEP=0 keeps the original.
Patches job_sleep() {
    return {patch("idle job workers sleep (worker loop)",
                  {one("4d8d7001 807f5900 74?? 4981f8e8030000 7c??", 8, B("74"), B("9090"))}),
            patch("idle job workers sleep (re-check before waiting)",
                  {one("498b07 80785900 74?? 4889c7 4883ef80", 7, B("74"), B("9090")),
                   one("498b4500 80785900 74?? 4889c7 4883ef80", 8, B("74"), B("9090"))}),
            patch("idle job workers sleep (job submission wakes them)",
                  {one("80785900 0f84????0000 4883c060", 4, B("0f84"), B("660f1f440000")),
                   one("807f5900 74?? 4883c760 beffffffff", 4, B("74"), B("9090"))})};
}

// The games' own Options > Controls > Aiming > Sensitivity (1.02): a 10-notch slider from 0 to 1.0, and the aiming
// code asserts it is at most 1. Mouse aiming goes through the stick there, so the game's aim speed caps it: lift the
// check and raise the slider's top. UNCHARTED_AIM_RANGE=0 keeps the original sliders.
Patches aim_range(const std::string &exe) {
    if (exe == "eboot.bin")   // the check at 0x276308; the slider (0x1453e4) gets mid 2.5, max 5.0
        return {patch("Uncharted 1 aim sensitivity above 1 (aim speed range check)",
                      {one("c5f82e05 ???????? 763d c5fa1145f4", 8, B("763d"), B("eb3d"))}),
                patch("Uncharted 1 aim sensitivity slider up to 10",
                      {one("488d0dd5738000 48894818 48c740200a000000 48b9 0000003f0000803f 48894828", 21,
                           B("0000003f0000803f"), B("0000a04000002041"))})};
    if (exe == "big2-ps4_Shipping.elf")   // the check in 0x719d80; the slider item (0x5d25b1) points at a 5.0
        return {patch("Uncharted 2 aim sensitivity above 1 (aim speed range check)",
                      {one("c5f82e05 ???????? 7649 488b05", 8, B("7649"), B("eb49"))}),
                patch("Uncharted 2 aim sensitivity slider up to 10",
                      {one("81f9d6c2ba1a 0f85???????? 4989ff 4c8d35???????? c5fa1005 75fa8800", 26, B("75fa8800"),
                           B("4de08800"))})};
    if (exe == "big3-ps4_Shipping.elf")   // UpdateAiming3Settings (0x927fe5); value <-> notch (0x62bf60, 0x62c090): max 4.0
        return {patch("Uncharted 3 aim sensitivity above 1 (aim speed range check)",
                      {one("c5fa1015???????? c5f82ed0 7351 488b05", 12, B("7351"), B("eb51"))}),
                patch("Uncharted 3 aim sensitivity slider up to 10 (value -> notch)",
                      {one("81f9d6c2ba1a7547 488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b", 8,
                           B("488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b"),
                           B("6b05dd63190140488d0df6d71c014801c8f30f590d13c1b4ffeb2c90"))}),
                patch("Uncharted 3 aim sensitivity slider up to 10 (notch -> value)",
                      {one("3dd6c2ba1a7520 488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36", 7,
                           B("488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36"),
                           B("6b0da662190140488d15bfd61c014801d1c5f828daf30f590dd8bfb4ffeb3790"))})};
    return {};
}

// FrameBegin (fg-draw-mgr.cpp, all three games, 1.02) frees frame N-3's instances and asserts that frame's end-of-pipe
// GPU timestamp is set (m_gfxEopTick, +0xa28, Uncharted 1 +0xa08). Players' crash logs (GitHub #11/#12/#17) show it
// after 40-55 ms stalls: the timestamp was not there yet; on the PS4 the GPU never lags that far. The assert's path
// (59 bytes) becomes a wait for the timestamp: pause in a loop until it is set, at most 50 million times, then carry
// on. UNCHARTED_FRAME_WAIT=0 keeps the assert.
std::string frame_begin_wait(long long offset) {
    std::string code = B("488dbc08") + i32(offset)   // lea rdi, [rax + rcx + offset]
                       + B("be80f0fa02")             // mov esi, 50000000
                       + B("48833f00")               // loop: cmp qword ptr [rdi], 0
                       + B("7506")                   // jne done
                       + B("f390")                   // pause
                       + B("ffce")                   // dec esi
                       + B("75f4")                   // jnz loop
                       + B("eb20");                  // done: jmp to where the check continues
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
    return {patch("FrameBegin waits for frame N-3's GPU timestamp (no m_gfxEopTick assert)",
                  {one(code, 11, B(code).substr(11), frame_begin_wait(offset))})};
}

// Uncharted 3's Player::UpdateFocus (player.cpp:1897, 0xbcee10) and Uncharted 2's PlayerBase::UpdateFocus
// (player-base.cpp:1101, 0x7272e0) spring the point Nate looks at towards the target and assert the result is not NaN
// (GitHub #13, #24, #148). Once NaN it stays NaN, and the function's own reset never runs for it: NaN compares false.
// The assert's path now takes that reset when the target itself is a number; a NaN target still asserts. Both 1.02
// and 1.00 (the exact 1.00 bytes occur once in its executable only). UNCHARTED_FOCUS_RESET=0 keeps the original.
const char *U3_FOCUS_100 = "c4e37904cb00c5f0c2c900c5f850c183f80f7438e8c3c110004885c07427488d3dd698c100"
                           "488d350595c100488d15e598c100488d0d3b95c10041b8690700004531c9ffd0cd41";
const char *U2_FOCUS_100 = "c4e37904c900c5f0c2c900c5f850c183f80f7438e8a8d71a004885c07427488d3d4c9c8300"
                           "488d354b998300488d155b9c8300488d0d7e99830041b84d0400004531c9ffd0cd41";
Patches focus_nan(const std::string &exe) {
    if (exe == "big3-ps4_Shipping.elf")
        return {patch("Uncharted 3 player focus: reset instead of asserting on NaN",
                      {one("c4e37904cb00c5f0c2c900c5f850c183f80f743a488b05f73302014885c07427488d3d74ff8900"
                           "488d3546fd8900488d1583ff8900488d0d6ece7d0041b86a0700004531c9ffd0cd41",
                           20,
                           B("488b05f73302014885c07427488d3d74ff8900488d3546fd8900488d1583ff8900"
                             "488d0d6ece7d0041b86a0700004531c9ffd0cd41"),
                           B("c5d057edc5f8284580c5f8c2c800c5f850c183f80f0f84a5feffffcd41") + cc(24)),
                       // 1.00 (GitHub #148): the same function at 0x390330; the assert path at 0x3908b8 is 51 bytes,
                       // the target sits at rbp-0xa0, the reset is 0x390779 and its velocity register is xmm4.
                       one(U3_FOCUS_100, 20, B(U3_FOCUS_100).substr(20),
                           B("c5d857e4c5f8288560ffffffc5f8c2c800c5f850c183f80f0f84a3feffffcd41") + cc(19))})};
    if (exe == "big2-ps4_Shipping.elf")
        return {patch("Uncharted 2 player focus: reset instead of asserting on NaN",
                      {one("c4e37904c900 c5f0c2c900 c5f850c1 83f80f 743a 488b05???????? 4885c0 7427 488d3d????????"
                           " 488d35???????? 488d15???????? 488d0d???????? 41b84d040000 4531c9 ffd0 cd41",
                           20,
                           B("488b05e267e4004885c07427488d3d7d4c7600488d352c4b7600488d158c4c7600"
                             "488d0dbdcb6c0041b84d0400004531c9ffd0cd41"),
                           B("c5f8284da0c5f0c2d100c5f850c283f80f7511c5e857d2c5f82993c0020000"
                             "e9e7feffffcd41") + cc(15)),
                       // 1.00 (GitHub #24's first U2 log): the same function at 0x33b4f0, assert path at 0x33ba13
                       one(U2_FOCUS_100, 20, B(U2_FOCUS_100).substr(20),
                           B("c5f8284da0c5f0c2d100c5f850c283f80f7511c5e857d2c5f82993c0020000"
                             "e9c2feffffcd41") + cc(13))})};
    return {};
}

// 1 Nov 2026, GitHub #293 (Uncharted 1 and 2 "pAction != NULL" at high frame rates): the player's move actions come
// from a bitmap pool made with 64 slots; at 120-144 FPS a burst fills it. 256 slots. UNCHARTED_MOVE_POOL=0: off.
Patch move_action_pool() {
    return patch("player move action pool: 256 slots (#293)",
                 {one("ba06000000 b940000000 41b820000000", 5, B("b940000000"), B("b900010000"))});
}
// 4 Nov 2026, Uncharted 2 chapter 21 (the convoy): Elena's truck sinks into the snow after a bump. Adding an animation
// node copies a bool the convoy's play call never writes into the previous node's flags as (flag << 2); stack garbage
// also set "the reference frame follows the object". Only bit 0 of the bool is used now. UNCHARTED_CONVOY_SINK=0: off.
Patch convoy_sink() {
    return patch("Uncharted 2 convoy: animation node flag from bit 0 of its bool",
                 {one("11 8a9080000000 c0e102 80e2fb 08ca 889080000000", 1,
                      B("8a9080000000c0e10280e2fb08ca889080000000"), B("80e101c0e10280a080000000fb08888000000090"))});
}

// gow3 5 Nov 2026: Uncharted 2's leg IK (character-leg-ik.cpp:391, IsFinite(deltaTransform)) asserts on a NaN root delta
// (the chapter 1 train wreck). The assert path now zeroes the delta for that frame and goes on where the finite path
// goes on. 1.02 and 1.00. UNCHARTED_LEG_IK_RESET=0: not applied.
Patches leg_ik(const std::string &exe) {
    if (exe != "big2-ps4_Shipping.elf") return {};
    return {patch("Uncharted 2 leg IK: a NaN delta is zeroed instead of asserting",
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

// 1 Nov 2026, GitHub #205, #304 (Uncharted 1), #326 (Uncharted 3): "read access to 0000000000000020" in the same engine
// job in all three games. Its creator queues the job with {object, object->+0x78}; when the object's descriptor is not
// set yet that pointer is null and the job reads address 0x20. The two loads now jump to a stub that ends the job for
// this frame when the pointer is null (at the function's own exit path), and otherwise carries on as before. The stub
// sits in a dead assert path of that exit, which still starts with int 0x41. UNCHARTED_JOB_PARAMS_GUARD=0: off.
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
    long long stub_at = cave + 2;                                         // after the kept int 0x41
    std::string stub = B("cd41 498b4208 4885c0");                         // int 0x41; mov rax,[r10+8]; test rax,rax
    stub += B("0f84") + i32(landing - (cave + (long long)stub.size() + 6));   // jz exit landing
    stub += B("488b4820");                                                // mov rcx, [rax+0x20]
    stub += B("e9") + i32((loads + 8) - (cave + (long long)stub.size() + 5));  // jmp back
    std::string jump = B("e9") + i32(stub_at - (loads + 5)) + B("909090");
    return {patch("Engine job: skip a job whose descriptor is not set yet instead of reading null (#205 #304 #326)",
                  {several(pattern, {{loads - start, B("498b4208488b4820"), jump},
                                     {cave - start, B(cave_original), stub}})})};
}

// 28 Oct 2026, GitHub #284 (Uncharted 3, "Stay in the Light", after 38 min): the game asserts "zscale*n+zoffset" in
// CreateOrthographicProjection (render-camera.h:441, int 0x41 at 0xf77095). A light camera's direction can be zero or
// exactly along +-X, which gives NaN light matrices (rsqrt without a guard). The call site (0xf3cad8) now calls a stub
// that checks the two light matrices (rbp-0x1b0, 8 rows) and the far value for NaN/inf: good -> on to the ortho builder
// 0xf768d0 as before; bad -> this frame's update of that light camera is skipped, so it keeps the last good frame's
// matrices. The stub sits in the dead path of the neighbouring far-plane assert (0xf770b7, 171 bytes), which still
// starts with int 0x41. UNCHARTED_LIGHT_ORTHO_GUARD=0: not applied.
Patch u3_light_ortho_guard() {
    std::string pattern = "c5fa100524ed5000 c5fa108d0cfeffff e8f39d0300 4c89e7 e84b422800";   // 0xf3cac8
    long long path = 0xf770b7 - 0xf3cac8, builder = 0xf768d0 - 0xf3cac8;
    std::string original = B(
        "488b05fab6c7004885c00f8499000000488d3dcf4c5300488d352a2252004989d6488d15a14c53004989dc"
        "4889cb488d0d6451430041b8ba0100004531c9c57829adf0feffffc57829b5e0feffffc57829bdd0feffff"
        "c5f82995c0feffffc5f829a5b0feffffc5f829ada0feffffffd0c5f828ada0feffffc5f828a5b0feffff"
        "c5f82895c0feffffc57828bdd0feffffc57828b5e0feffffc57828adf0feffff4889d94c89e34c89f2cd41");
    std::string stub = B("cd41");                                      // int 0x41 (the old path's start)
    stub += B("c5f8289d50feffff c5e05cdb");                            // xmm3 = row0 - row0 (NaN if not finite)
    for (int row = 1; row < 8; ++row) {                                // + (row - row) for rows 1..7
        stub += B("c5f828a5") + i32(-0x1b0 + 0x10 * row);              // vmovaps xmm4, [rbp-..]
        stub += B("c5d85ce4 c5e058dc");                                // vsubps xmm4,xmm4,xmm4; vaddps xmm3,xmm3,xmm4
    }
    stub += B("c5f25ce1 c5e258dc");                                    // + (far - far) (xmm1)
    stub += B("c5e0c2db03 c5f850c3 85c0 7505");                        // any lane NaN? -> bad
    stub += B("e9") + i32(builder - (path + (long long)stub.size() + 5));   // good: jmp 0xf768d0
    stub += B("4883042408 c3");                                        // bad: return past the follow-up call
    std::string call = B("e8") + i32((path + 2) - (0x10 + 5));
    return patch("Uncharted 3 light camera: skip a NaN light ortho instead of asserting (#284)",
                 {several(pattern, {{0x10, B("e8f39d0300"), call}, {path, original, stub + cc(original.size() - stub.size())}})});
}
// Test knob for the #284 guard (UNCHARTED_TEST_LIGHT_ORTHO_NAN=1, never in the launcher): every active light camera
// gets a zero direction, and the caller's gate is opened so the camera is built every frame.
Patch u3_test_light_ortho_nan() {
    return patch("Uncharted 3 TEST: zero light-camera direction (UNCHARTED_TEST_LIGHT_ORTHO_NAN)",
                 {one("c5e8591db41d4f00 c5e059c9 c5e85cc9 c5f859c1 c5f8298510feffff", 16, B("c5f859c1"), B("c5f857c0"))});
}
Patch u3_test_light_ortho_gate() {
    return patch("Uncharted 3 TEST: light camera built every frame (UNCHARTED_TEST_LIGHT_ORTHO_NAN)",
                 {several("488b8360010000 c5f82e8010050000 7436 c5fa1005e94f5600 488b8360010000 c5f82e80680a0000 731d",
                          {{15, B("7436"), B("9090")}, {40, B("731d"), B("9090")}})});
}

// The most common Drake's Fortune crash (GitHub #27, #30, #42, #62: "execute access to 0"; Uncharted 2 and 3 have the
// same code): a leaf function that fills a shader's constant table keeps its running indices below rsp. The tables lie
// next to GPU data, so a store can fault on a write-protected page in the middle of the function, and Windows builds
// the exception frame on that same stack: an index then reads 0 and a later walk calls a null member. The function's
// start now jumps to a stub that writes each page of the table once (lock or byte [p], 0) before anything lives below
// rsp: a protected page faults there, harmlessly. The stub sits in a dead assert path nearby, which still starts with
// int 0x41. UNCHARTED_CUE_PRETOUCH=0: off.
const char *CUE_TABLE_FILL = "55 41 57 41 56 41 55 41 54 53 49 b9 00 24 00 0d 10 00 00 00 48 c7 07 00 00 00 00 45 31 f6";
Variant cue_pretouch_variant(long long cave, const std::string &original_hex) {
    std::string stub = B("cd41"            // int 0x41 (the assert path's end, now its start)
                         "89d0"            // mov eax, edx                 entries
                         "486bc030"        // imul rax, rax, 0x30
                         "488d440727"      // lea rax, [rdi + rax + 0x27]  the table's last byte
                         "4889f9"          // mov rcx, rdi
                         "f0800900"        // touch: lock or byte [rcx], 0
                         "6681c9ff0f"      // or cx, 0xfff
                         "48ffc1"          // inc rcx                      the next page
                         "4839c1"          // cmp rcx, rax
                         "76ef"            // jbe touch
                         "5541574156");    // the moved instructions
    stub += B("e9") + i32(5 - (cave + (long long)stub.size() + 5));   // jmp to the function's 6th byte
    std::string original = B(original_hex);
    return several(CUE_TABLE_FILL, {{cave, original, stub + cc(original.size() - stub.size())},
                                    {0, B("5541574156"), B("e9") + i32(cave + 2 - 5)}});
}
Patches cue_pretouch(const std::string &exe) {
    const char *description = "constant tables: pages written before the fill (no fault inside it)";
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

// Uncharted 2 "read access to 0 at guest offset 0xd09665" (GitHub #61 #113 #115 #164 #181 #187): the particle setup's run
// grouping (the same leaf in all three games) keeps its loop bounds below rsp; a page fault inside it lets Windows'
// exception frame overwrite them and the look-ahead reads one entry past the end. The reloads now take the same values
// from r10 (array) and r11 (count), which the function never changes. Same length, in place. UNCHARTED_RUN_GROUP_FIX=0.
Patch run_group() {
    return patch("particle run grouping: loop bounds from registers, not below rsp",
                 {// 1.02: mov rcx,[rsp-8] -> lea rcx,[r10+0x10]; nop | mov r15,[rsp-0x10] -> lea r15,[r11-1]; nop
                  several("4c8b27 488b4c24f8 4a8d1c31 4c8b7c24f0 4929f7",
                          {{3, B("488b4c24f8"), B("498d4a1090")}, {12, B("4c8b7c24f0"), B("4d8d7bff90")}}),
                  // 1.00: mov rcx,[rsp-8] -> the same lea | mov rcx,[rsp-0x10]; lea r8,[rcx+rsi] -> r8 = i + 1 - count
                  several("4c8b27 488b4c24f8 4a8d1c31 488b4c24f0 4c8d0431 4531ed",
                          {{3, B("488b4c24f8"), B("498d4a1090")}, {12, B("488b4c24f04c8d0431"), B("4989f04d29d849ffc0")}})});
}

// Everything unlocked (launcher -> Advanced -> Unlock all chapters, UNCHARTED_UNLOCK=1): Bluepoint's tweak flags
// "Unlock Chapter Select by default" and "Unlock Brutal difficulty by default": the chapter flag is set where the
// *bp-ucctweaks* table is read, Brutal where it is cleared (1.02; U1 has two copies of that constructor).
Patches unlock(const std::string &exe) {
    if (exe == "eboot.bin") {
        Patches p;
        for (auto c : {std::make_pair("1", "fe250000"), std::make_pair("2", "f51d0000")})
            p.push_back(patch(std::string("Uncharted 1 chapter select unlocked (") + c.first + ")",
                              {one(std::string("e8") + c.second + " 4885c0 7411 8a08 888b302c0000 8a4001 8883322c0000", 18,
                                   B("8a40018883322c0000"), B("c683322c0000016690"))}));
        for (auto c : {std::make_pair("1", "ea3b0000"), std::make_pair("2", "e1330000")})
            p.push_back(patch(std::string("Uncharted 1 Brutal difficulty unlocked (") + c.first + ")",
                              {one(std::string("e8") + c.second + " c683302c000001 c683312c000000", 12, B("c683312c000000"),
                                   B("c683312c000001"))}));
        return p;
    }
    if (exe == "big2-ps4_Shipping.elf")
        return {patch("Uncharted 2 chapter select unlocked",
                      {one("833800 0f9583a02d0000 83780400 0f9583a22d0000", 10, B("837804000f9583a22d0000"),
                           B("c683a22d0000010f1f4000"))}),
                patch("Uncharted 2 Brutal difficulty unlocked",
                      {one("c683a02d000001 c683a12d000000 c683a22d000000", 7, B("c683a12d000000"), B("c683a12d000001"))})};
    if (exe == "big3-ps4_Shipping.elf")
        return {patch("Uncharted 3 chapter select unlocked",
                      {one("833800 0f9583602c0000 83780400 0f9583622c0000", 10, B("837804000f9583622c0000"),
                           B("c683622c0000010f1f4000"))}),
                patch("Uncharted 3 Brutal difficulty unlocked",
                      {one("c683602c000001 c683612c000000 c683622c000000", 7, B("c683612c000000"), B("c683612c000001"))})};
    return {};
}

// Unlock all rewards (UNCHARTED_UNLOCK_REWARDS=1). Uncharted 2 and 3 let some rewards be enabled only once the game was
// finished on the current difficulty: that requirement is cleared. Drake's Fortune keeps each reward's status in its
// own record and its debug switch also writes them, so its checks are patched instead: every reward reads as unlocked,
// usable and counted, and the Story Cutscenes list skips no chapter (nothing saved). 1.02 code.
Patches rewards(const std::string &exe) {
    if (exe == "big2-ps4_Shipping.elf")
        return {patch("Uncharted 2 rewards can be enabled before the game is finished",
                      {one("7441c6430e01833d401fa800", 5, B("01"), B("00"))})};
    if (exe == "big3-ps4_Shipping.elf")
        return {patch("Uncharted 3 rewards can be enabled before the game is finished",
                      {one("74054c39f0725c41c6450e01488d05", 11, B("01"), B("00"))})};
    if (exe == "eboot.bin")
        return {// IsUnlocked(item): `and al, 1` on status bit 0 -> `mov al, 1`
                patch("Uncharted 1 rewards unlocked (item check)",
                      {one("b001803f00750d4885f674098a86880000002401", 18, B("24"), B("b0"))}),
                // IsUnlocked(category, id)
                patch("Uncharted 1 rewards unlocked (id check)",
                      {one("75134d85e4740c418a8424880000002401eb02", 15, B("24"), B("b0"))}),
                // the "Unlocked N of 47" counter: `je skip` -> `je +0`
                patch("Uncharted 1 rewards unlocked (counter)",
                      {one("4c89f275154885c07422f68088000000017419", 18, B("19"), B("00"))}),
                // usable-by-id check: `je not` -> `je +0`
                patch("Uncharted 1 rewards unlocked (usable by id)",
                      {one("41803e00750af684088800000001741e488d55d4", 15, B("1e"), B("00"))}),
                // CanEnable: `sete dl` (dl = locked) -> `setb dl` (0: test clears CF)
                patch("Uncharted 1 rewards unlocked (enable check)",
                      {one("41f68688000000010f94c24584ff0f95c184c0", 9, B("94"), B("92"))}),
                // CanEnable's "finish the game on this difficulty first" reason: `mov r13b, 1` -> `mov r13b, 0`
                patch("Uncharted 1 rewards can be enabled before the game is finished",
                      {one("448bb87848000041b5014183ff067338", 9, B("01"), B("00"))}),
                // Story Cutscenes list: `js skip` (chapter not reached) -> `js +0`
                patch("Uncharted 1 story cutscenes unlocked",
                      {one("488b7840837f50000f88880000004c8b4210", 10, B("88"), B("00"))})};
    return {};
}

// Cheats (UNCHARTED_CHEATS) that need code: Uncharted 1 and 2 keep "Infinite Ammo" in their settings system, which
// writes the byte back every frame (a poke does not stick): each place that reads it (`cmp byte [flag], 0; jne`) takes
// the jump as if it were set. Drake's Fortune's own Infinite Ammo only spares the reserve: each shot still takes a round
// from the magazine; with the cheat that decrement is gone. The other cheats are pokes (runtime_env.cpp).
Variant ammo_reader(const std::string &pattern, long long jne_at) { return one(pattern, jne_at, B("75"), B("eb")); }
Patches cheat_patches(const std::string &exe, const std::string &cheat) {
    if (cheat != "ammo") return {};
    Patches p;
    if (exe == "eboot.bin") {
        const std::pair<const char *, int> readers[] = {{"488d15caca8200 b101 803a00 75", 12}, {"488d0d79673800 803900 75", 10},
                                                        {"488d0d3a5b3800 803900 75", 10}, {"488d0d0a023800 803900 75", 10}};
        int n = 1;
        for (auto &r : readers)
            p.push_back(patch("Uncharted 1 infinite ammo (reader " + std::to_string(n++) + ")", {ammo_reader(r.first, r.second)}));
        p.push_back(patch("Uncharted 1 infinite ammo (magazine)",
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
            p.push_back(patch("Uncharted 2 infinite ammo (reader " + std::to_string(n++) + ")", {ammo_reader(r.first, r.second)}));
    }
    return p;
}

// ------------------------------------------------------------------------------------------------ ultrawide

// Ultrawide (GitHub #20): the games keep a 16:9 camera's horizontal field of view at every aspect ratio and lose the
// top and bottom of the picture instead (Vert-). The render camera holds zoom = 1/tan(horizontal FOV / 2) and aspect =
// height/width (U2/U3 +0x254/+0x250, Drake's Fortune +0x294/+0x290). Where the projection and the view frustum's culling
// planes read the zoom of a perspective camera it becomes zoom * min(1, aspect * 16/9): wider than 16:9 keeps the 16:9
// picture's vertical view and shows more at the sides (Hor+), 16:9 and narrower views and square cameras are unchanged.
// The zoom loads jump to small stubs in dead assert branches of the projection code (each now starts with int 0x41)
// and back. Applied only to render sizes wider than 16:9 (UNCHARTED_HOR_PLUS=0: never).
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
// op xmm<dst>, xmm<src>, [base + disp32] (scalar single; vmovss: src 0): VEX 2-byte for rax..rdi, 3-byte for r8..r15,
// with the SIB byte that rsp/r12 as a base need
std::string vex_mem(int op, int dst, int src, const std::string &base, long long disp) {
    int reg = gpr(base);
    std::string modrm(1, (char)(0x80 | dst << 3 | (reg & 7)));
    if ((reg & 7) == 4) modrm += '\x24';
    std::string prefix = reg < 8 ? std::string{(char)0xc5, (char)(0x80 | (~src & 15) << 3 | 2)}
                                 : std::string{(char)0xc4, (char)0xc1, (char)((~src & 15) << 3 | 2)};
    return prefix + (char)op + modrm + i32(disp);
}
// op xmm<dst>, xmm<src>, [rip -> target], the 8-byte instruction placed at `at`
std::string vex_rip(int op, int dst, int src, long long at, long long target) {
    return std::string{(char)0xc5, (char)(0x80 | (~src & 15) << 3 | 2), (char)op, (char)(dst << 3 | 5)} + i32(target - (at + 8));
}
struct Site { long long offset; std::string base; int dst, free; };
struct Cave { long long offset, size; std::string original_hex; };
// One patch variant: sites are `vmovss xmm, [base + zoom]` loads; caves are the dead assert branches that take the
// stubs. Offsets are from the start of the pattern's match (a cave may lie before it or in a neighbouring function: its
// original bytes are checked too).
Variant hor_plus_variant(const std::string &pattern, long long zoom, long long aspect, const std::vector<Site> &sites,
                         const std::vector<Cave> &caves) {
    struct CaveUse { long long offset, size; std::string original; long long used; };
    std::vector<CaveUse> cave_list;
    for (const Cave &c : caves) cave_list.push_back({c.offset, c.size, B(c.original_hex), 2});   // after int 0x41
    struct Stub { long long at, site; std::string base; int dst, free; long long length; };
    std::vector<std::pair<long long, std::vector<Stub>>> stubs;   // per cave, in first-use order
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
                    B("e9") + i32(st.site + st.length - (jmp_at + 5));      // jmp back
            edits.push_back({st.site, vex_mem(0x10, st.dst, 0, st.base, zoom),
                             B("e9") + i32(st.at - (st.site + 5)) + nops((size_t)st.length - 5)});
        }
        edits.insert(edits.begin(), Edit{cave.offset, cave.original, code + f32(16.0 / 9.0) + f32(1.0)});
    }
    return several(pattern, edits);
}
Patches hor_plus_all(const std::string &exe) {
    if (exe == "eboot.bin")   // render camera update 0x2c9660 and its frustum planes 0x2ca4ca (Drake's Fortune 1.02)
        return {patch("Drake's Fortune ultrawide: wider view instead of a cropped one",
                      {hor_plus_variant("c5fa108b98020000 c5fa109b90020000 c5fa109394020000 c5f857c0 c5f82ec2 7251", 0x294, 0x290,
                                        {{0x10, "rbx", 2, 0}, {0x2a9, "rbx", 3, 0}, {0xcac, "rdi", 0, 1}},
                                        {{0x22, 0x51, "c5fa115dc0c5fa1155d0"}, {0x2c1, 0x63, "c5fa114db0c5fa115dc0"}})})};
    if (exe == "big2-ps4_Shipping.elf")
        return {// 0x9dc750 (infinite far 0x9dc8fe, finite 0x9dcb55), frustum planes 0x9dd41d (Uncharted 2 1.02)
                patch("Uncharted 2 ultrawide: wider view instead of a cropped one",
                      {hor_plus_variant("c5fa10ab58020000 c5fa109b50020000 c5fa10a354020000 c5f857c0 c5f82ec4 0f82b6000000",
                                        0x254, 0x250, {{0x10, "rbx", 4, 0}, {0x267, "rbx", 4, 0}, {0xb2f, "rdi", 0, 1}},
                                        {{0x26, 0xb6, "488b055d16b9004885c0"}})}),
                // Shadows past the 16:9 area: the culling frustums of the sun, rain and local (flashlight, lamp) shadows
                // are built from the render camera's own zoom; with the 16:9 zoom casters past the 16:9 picture were
                // culled. Each stub goes in that function's own numPlanes assert (UNCHARTED_UW_SHADOWS=0: off).
                patch("Uncharted 2 ultrawide: sun shadows as wide as the view",
                      {hor_plus_variant("c5fa10b350020000 c5fa10ab54020000 c4637904ce00", 0x254, 0x250, {{0x8, "rbx", 5, 0}},
                                        {{-0x194, 0x35, "488b0573128a004885c0"}})}),
                patch("Uncharted 2 ultrawide: rain shadows as wide as the view",
                      {hor_plus_variant("c5fa10ab50020000 c5fa10a354020000 c4637904c500", 0x254, 0x250, {{0x8, "rbx", 4, 3}},
                                        {{-0x124, 0x35, "488b05a50e8a004885c0"}})}),
                patch("Uncharted 2 ultrawide: local light shadows as wide as the view",
                      {hor_plus_variant("c4c17a10942454020000 c4c17a10ac2450020000", 0x254, 0x250, {{0x0, "r12", 2, 5}},
                                        {{0x183, 0x35, "488b05c6018a004885c0"}})}),
                // The scene view's GPU constants (0xcfcb00): two more copies of the frustum code for the render camera
                // (r12) and a second view (r13). Stubs in the function's two SetConstantBuffers asserts and two of the
                // neighbouring function 0xd01020's (UNCHARTED_UW_VIEW_CONSTANTS=0: off).
                patch("Uncharted 2 ultrawide: view constants as wide as the view (1)",
                      {hor_plus_variant("c4c17a109c2454020000 c4417a10bc2450020000", 0x254, 0x250, {{0x0, "r12", 3, 5}},
                                        {{0x10e7, 0x38, "488b0549fa86004885c0"}})}),
                patch("Uncharted 2 ultrawide: view constants as wide as the view (2)",
                      {hor_plus_variant("c4c17a10842454020000 c57a100d???????? c5b25ec8", 0x254, 0x250, {{0x0, "r12", 0, 1}},
                                        {{0x2a24, 0x38, "488b0519da86004885c0"}})}),
                patch("Uncharted 2 ultrawide: view constants as wide as the view (3)",
                      {hor_plus_variant("c4c17a109d54020000 c4c17a10a550020000", 0x254, 0x250, {{0x0, "r13", 3, 4}},
                                        {{0x28d4, 0x35, "488b0504c486004885c0"}})}),
                patch("Uncharted 2 ultrawide: view constants as wide as the view (4)",
                      {hor_plus_variant("c4c17a108554020000 c5da5ec8", 0x254, 0x250, {{0x0, "r13", 0, 1}},
                                        {{0x2189, 0x35, "488b0568c186004885c0"}})})};
    if (exe == "big3-ps4_Shipping.elf")   // infinite far 0x11c1107, finite 0x11c11e1 and 0x11c1406, frustum 0x11c1f21
        return {patch("Uncharted 3 ultrawide: wider view instead of a cropped one",
                      {hor_plus_variant("80bb6902000000 c5fa10a358020000 c5fa108b50020000 c5fa109354020000 0f840a020000",
                                        0x254, 0x250,
                                        {{0x17, "rbx", 2, 3}, {0xf1, "rbx", 2, 0}, {0x316, "rbx", 1, 0}, {0xe31, "rdi", 0, 1}},
                                        {{0x10d, 0x9f, "488b05b415a3004885c0"}, {0x32e, 0x65, "488b059313a3004885c0"}})})};
    return {};
}
Patches hor_plus(const std::string &exe) {
    std::vector<std::string> skip;
    if (env("UNCHARTED_UW_SHADOWS", "1") == "0") skip.push_back("shadows as wide as the view");
    if (env("UNCHARTED_UW_VIEW_CONSTANTS", "1") == "0") skip.push_back("view constants as wide as the view");
    Patches out;
    for (Patch &p : hor_plus_all(exe)) {
        bool skipped = false;
        for (auto &s : skip) skipped = skipped || p.description.find(s) != std::string::npos;
        if (!skipped) out.push_back(p);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ settings

// Uncharted 3 1.02's tonemap constants (pavlikdcx's patch in GitHub #199): the filmic exposure bias, read from the
// render settings at [rbx+0x72c] on two paths (image 0xed9d30 / 0xed9d49), gets <bias> EV more before the tonemap. Main
// path: the load jumps to a stub in the code segment's zero padding (image 0x1648800) that adds the bias; second path:
// its add of -0.5 (shared constant 0x144b3b8) reads our own (-0.5 + bias) instead.
Patch u3_exposure_patch(double bias) {
    std::string pattern = "c5fa10932c070000 c5f057c9 c5fa100570165700 e9d1000000 c5fa10832c070000 c5fa58055f165700";
    long long cave = 0x1648800 - 0xed9d30;   // from the pattern's start
    std::string stub = B("c5fa10932c070000");                                         // vmovss xmm2, [rbx+0x72c]
    stub += B("c5ea5815") + i32(0x40 - ((long long)stub.size() + 8));                 // vaddss xmm2, xmm2, [cave+0x40]
    stub += B("c5f057c9");                                                            // vxorps xmm1, xmm1, xmm1
    stub += B("e9") + i32(12 - (cave + (long long)stub.size() + 5));                  // jmp back (pattern + 12)
    stub += cc(0x40 - stub.size());
    stub += f32(bias) + f32(-0.5 + bias);
    std::string jump = B("e9") + i32(cave - 5) + rep('\x90', 7);
    std::string second = i32((cave + 0x44) - (0x21 + 8));                             // disp of the 2nd path's add
    char description[96];
    std::snprintf(description, sizeof description, "Uncharted 3 exposure %+.2f EV (UNCHARTED_U3_EXPOSURE)", bias);
    return patch(description, {several(pattern, {{0, B("c5fa10932c070000c5f057c9"), jump},
                                                 {cave, std::string(stub.size(), '\0'), stub},
                                                 {0x25, B("5f165700"), second}})});
}

// The job manager's worker count (6) in its init parameters; Uncharted 3 has two init sites (the same code with r13/r15).
Patches job_worker_patches(const std::string &exe, int count) {
    std::string tail = "48c7458800080000 48c7459000040000 48c74598";
    std::string replacement(1, (char)count);
    std::string description = "job workers: " + std::to_string(count) + " instead of 6 (UNCHARTED_JOB_WORKERS)";
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
// Python's int() of an already split-off part (spaces around allowed); false when it isn't one
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

// The launcher's code patches for this executable and the current settings, in the order they are applied.
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
    // Test knob for that guard (never in the launcher): Uncharted 3's creator at 0xf6da01 stores a null descriptor,
    // so every such job gets the null the players' crash had.
    if (u3 && env("UNCHARTED_TEST_JOB_PARAMS_NULL", "") == "1")
        patches.push_back(patch("Uncharted 3 TEST: null job descriptor (UNCHARTED_TEST_JOB_PARAMS_NULL)",
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
    {   // the cheats asked for (UNCHARTED_CHEATS, launcher -> Cheats)
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
    // Ultrawide render sizes (the launcher's aspect ratio): the wider view.
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
        // Diagnostics: UNCHARTED_JOB_SLEEP_SPINS=<n> idle spins before a worker sleeps (1000).
        std::string spins = strip(env("UNCHARTED_JOB_SLEEP_SPINS", ""));
        if (!spins.empty()) {
            long long n = 0;
            if (!to_int(spins, n) || n < 0 || n > 0xffffffffLL) throw Failure{"invalid UNCHARTED_JOB_SLEEP_SPINS: " + spins};
            std::string bytes(4, '\0');
            for (int i = 0; i < 4; ++i) bytes[i] = (char)(n >> (8 * i));
            patches.push_back(patch("idle job workers: spins before sleeping",
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
// the patch list in the form un_apply_patches takes it
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

// The code patches for this executable and the current settings, as JSON (the list un_apply_patches takes). 0 ok,
// 1 a setting is not valid (out: why).
UN_API int un_image_patches(const char *executable, char *out, int cap) {
    try {
        return give(json::dump(spec(image_patches(executable ? executable : ""))), out, cap);
    } catch (const Failure &f) {
        int status = give(f.message, out, cap);
        return status < 0 ? status : 1;
    }
}

// Applies this executable's patches (the current settings) to the linked image. out: the log lines. 0 applied;
// 1 not applied (the last line says why); negative: out too small (nothing written; call again with that size).
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
