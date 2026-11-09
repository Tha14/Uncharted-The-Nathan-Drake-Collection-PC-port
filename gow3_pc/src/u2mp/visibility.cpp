// u2mp visibility: NetGameManager::KickVisibilityJob (PS3 1.10 0x494c38), stripped on PS4 1.02
// (notes: u2mp/docs/PORT-GAMEPLAY3.md section 7).
//
// PS3: PostProcessUpdate 0x21db8 calls KickVisibilityJob(g_netGameManager) once per frame in
// multiplayer. For every remote player tracker (used, not excluded, not the local net id) it takes the
// player's position (0x4c7df8), skips players the view test rejects (0x8a2388, result +44 != 0), then
// re-initialises the tracker's collision probe (tracker+408; Init 0x39d120(probe, 1, 32, 0, 13, 0, 0))
// and kicks one sphere cast from the camera position (0xee0170) towards the player
// (0x6c2d58: SetRayDir slot 7 with length 1.0, radius slot 32 = 0.01, filter slot 9 = {26, 0, default},
// slot 11 = 0.0, Kick slot 2 with the call-site globals 0xf71140..48). The next frame
// UpdatePlayerTrackerVisibility 0x494608 reads the probe (0x4c9748).
//
// PS4: the reader is intact - NetGameManager::Update 0x63e6a0 calls 0x666160(tracker) at 0x63e813 for
// each remote tracker. 0x666160 needs the probe at tracker+0x1b0 to be initialised (+0xf bit 0) and kicked
// in this or the previous frame (+0x18 frame = [0x1116180+0x28] or one less, +0x24 job handle), then
// reads the hit fraction (0x97e530; -1 = no hit = visible). PostProcessUpdate 0x3f5620 lost its
// multiplayer branch, so nothing ever initialised or kicked the probe and every remote player stayed
// "not visible" (name tags / spotting, state at tracker+0x218..0x220, event sender 0x611fa0).
//
// PS4 pieces used (PS3 -> PS4):
//   probe class: tracker ctor 0x662660 builds it at tracker+0x1b0 with vptr 0x1004180 (sphere probe;
//     the slot numbers are the PS3 ones): 1 Init 0x4f1dc0 (= PS3 0x39d120), 2 Kick 0x4f2130 (writes
//     +0x18 frame and +0x24 handle), 5 Release 0x4f2490, 7 SetRayDir 0x4f25b0, 9 SetFilter 0x4f26c0,
//     11 = ret on PS4, 12 IsActive 0x4f2850, 32 SetRadius 0x4f2600. The same sequence inlined: PS4
//     0x97ec30 (= PS3 0x6c3990).
//   0x4c7df8 player position = 0x665f70(out, tracker); 0x8a2388 view test = 0x5566b0(out, 0.0f, &pos),
//     flag at out+0x2c; camera position 0xee0170 = 0x104e820; call-site globals = 0x151f058/60/68;
//     default filter object [0xf70c58] = [0x151f048]. 0x666160 uses the same calls and globals.
//   0x6b2ba4 (local player) -> the local tracker ngm+0 must be set, as 0x63e782 checks.
//
// Where it runs: once per frame after Playing::Update (matchend.cpp calls visibility_kick() after the
// original update), so NetGameManager::Update reads the probe one frame after the kick, as PS3 did
// (game update, then PostProcessUpdate). It does nothing outside multiplayer (NET_SM < 2), without a
// collision world ([0x1125370+0x2a60]), without a local tracker or with no remote player. Playing::Exit
// (vtable 0x100ab80 slot 2, 0x5f2350) releases the probes before the original Exit, so no job is in
// flight when LoadLevels resets the trackers (0x662660 zeroes the probe without releasing it).
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
void install_visibility(int &installed);
void visibility_kick();   // matchend.cpp: after Playing::Update

namespace {

constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kCollisionWorld = 0x1125370 + 0x2a60;   // non-NULL when the collision world exists
constexpr uint64_t kCamera = 0x104e820;                     // camera position (vec4), PS3 0xee0170
constexpr uint64_t kSiteA = 0x151f058, kSiteB = 0x151f060, kSiteC = 0x151f068;   // PS3 0xf71140..48
constexpr uint64_t kDefaultFilterObj = 0x151f048;           // PS3 [0xf70c58]
constexpr uint64_t fPlayerPos = 0x665f70;                   // (vec4 *out, tracker) (PS3 0x4c7df8)
constexpr uint64_t fViewTest = 0x5566b0;                    // (out, float, const vec4 *) (PS3 0x8a2388)
constexpr uint64_t vtSphereProbe = 0x1004180;
constexpr uint64_t NGM_TRACKERS = 0x10, NGM_TRACKER_SIZE = 0x2c0;
constexpr int kTrackers = 10;
constexpr uint64_t T_USED = 0x20, T_NETID = 0x120, T_EXCLUDED = 0x26c, T_PROBE = 0x1b0;
constexpr uint64_t VIEW_REJECTED = 0x2c;
// PS3 constants: Init(count 1, hits 32, flags 0, type 13), ray length 1.0 (0xdc0f04), radius 0.01
// (0xdb1e14), filter {26, 0}.
constexpr uint32_t kCount = 1, kHits = 32, kFlags = 0, kType = 13, kFilterA = 26, kFilterB = 0;
constexpr float kRayLength = 1.0f, kRadius = 0.01f;
constexpr uint64_t sPlayingExit = 0x100ab80 + 0x10, hPlayingExit = 0x5f2350;

struct alignas(16) Vec4 {
    float v[4];
};

using InitFn = void(U2MP_GUEST *)(void *, uint32_t, uint32_t, uint32_t, uint32_t, void *, uint32_t);
using KickFn = void(U2MP_GUEST *)(void *, uint64_t, uint32_t, uint64_t, uint32_t, uint32_t);
using ReleaseFn = void(U2MP_GUEST *)(void *);
using IsActiveFn = bool(U2MP_GUEST *)(void *);
using SetRayDirFn = void(U2MP_GUEST *)(void *, uint32_t, const Vec4 *, const Vec4 *, float);
using SetFilterFn = void(U2MP_GUEST *)(void *, uint32_t, uint32_t, uint32_t, uint64_t *);
using SetRadiusFn = void(U2MP_GUEST *)(void *, uint32_t, float);
using PosFn = Vec4 *(U2MP_GUEST *)(Vec4 *, const void *);
using ViewFn = void(U2MP_GUEST *)(void *, float, const Vec4 *);
using ExitFn = uint64_t(U2MP_GUEST *)(void *);

bool g_ready = false;
ExitFn previous_exit = nullptr;
uint64_t g_kicks = 0;
bool kicked[kTrackers] = {};
constexpr uint64_t kProbeFrame = 0x1116180 + 0x28;   // the frame count the probes compare with
int64_t g_exit_frame = -1;   // frame of the last Playing::Exit: no kick after it in the same frame

template <typename T> T G(uint64_t off) {
    T v;
    std::memcpy(&v, image.base + off, sizeof v);
    return v;
}
uint8_t *P(uint64_t off) { return image.base + off; }

template <typename F> F slot_fn(const uint8_t *probe, unsigned slot) {
    uint64_t vt;
    std::memcpy(&vt, probe, 8);
    uint64_t fn;
    std::memcpy(&fn, reinterpret_cast<const void *>(uintptr_t(vt + 8 * slot)), 8);
    return reinterpret_cast<F>(uintptr_t(fn));
}

bool probe_ok(const uint8_t *probe) {
    uint64_t vt;
    std::memcpy(&vt, probe, 8);
    return vt == uint64_t(uintptr_t(image.base + vtSphereProbe));
}

void release(uint8_t *probe) {
    if (!probe_ok(probe)) return;
    if (slot_fn<IsActiveFn>(probe, 12)(probe)) slot_fn<ReleaseFn>(probe, 5)(probe);
}

void release_all(const char *why) {
    int n = 0;
    for (int i = 0; i < kTrackers; ++i) {
        if (!kicked[i]) continue;
        kicked[i] = false;
        if (!G<uint64_t>(kCollisionWorld)) continue;
        release(P(kNgm + NGM_TRACKERS + NGM_TRACKER_SIZE * i + T_PROBE));
        ++n;
    }
    if (n) log("visibility: %d probe(s) released (%s)", n, why);
}

// PS3 0x494c38 body for one tracker.
void kick_one(int i, const uint8_t *local) {
    uint8_t *t = P(kNgm + NGM_TRACKERS + NGM_TRACKER_SIZE * i);
    uint8_t *probe = t + T_PROBE;
    uint32_t id, local_id;
    std::memcpy(&id, t + T_NETID, 4);
    std::memcpy(&local_id, local + T_NETID, 4);
    if (!t[T_USED] || t[T_EXCLUDED] || id == local_id) {
        if (kicked[i]) {   // the slot was freed or excluded since the last kick
            kicked[i] = false;
            release(probe);
        }
        return;
    }
    if (!probe_ok(probe)) return;   // tracker not constructed (0x662660) or reset meanwhile
    alignas(16) Vec4 target{};
    reinterpret_cast<PosFn>(image.base + fPlayerPos)(&target, t);
    alignas(16) uint8_t view[0x80] = {};
    reinterpret_cast<ViewFn>(image.base + fViewTest)(view, 0.0f, &target);
    int32_t rejected;
    std::memcpy(&rejected, view + VIEW_REJECTED, 4);
    if (rejected) return;   // PS3: only kicked when the view test passes; the reader checks the same
    alignas(16) Vec4 start;
    std::memcpy(&start, P(kCamera), sizeof start);
    alignas(16) Vec4 delta;
    for (int k = 0; k < 4; ++k) delta.v[k] = target.v[k] - start.v[k];
    slot_fn<InitFn>(probe, 1)(probe, kCount, kHits, kFlags, kType, nullptr, 0);
    slot_fn<SetRayDirFn>(probe, 7)(probe, 0, &start, &delta, kRayLength);
    slot_fn<SetRadiusFn>(probe, 32)(probe, 0, kRadius);
    uint64_t filter_obj = G<uint64_t>(kDefaultFilterObj);
    slot_fn<SetFilterFn>(probe, 9)(probe, 0, kFilterA, kFilterB, &filter_obj);
    slot_fn<KickFn>(probe, 2)(probe, G<uint64_t>(kSiteA), G<uint32_t>(kSiteB), G<uint64_t>(kSiteC), 0, 0);
    kicked[i] = true;
    if (++g_kicks == 1) log("visibility: first probe kicked (tracker %d, net id %u)", i, id);
}

U2MP_GUEST uint64_t playing_exit(void *state) {
    g_exit_frame = G<int32_t>(kProbeFrame);
    release_all("Playing::Exit");
    return previous_exit(state);
}

bool layout_matches() {
    struct Slot {
        unsigned slot;
        uint64_t fn;
    };
    static const Slot slots[] = {{1, 0x4f1dc0}, {2, 0x4f2130}, {5, 0x4f2490}, {7, 0x4f25b0},
                                 {9, 0x4f26c0}, {12, 0x4f2850}, {32, 0x4f2600}};
    const uint64_t base = uint64_t(uintptr_t(image.base));
    for (const Slot &s : slots) {
        if (vtSphereProbe + 8 * s.slot + 8 > image.size || G<uint64_t>(vtSphereProbe + 8 * s.slot) != base + s.fn) {
            log("visibility: probe vtable 0x%llx slot %u is not 0x%llx, not installed",
                (unsigned long long)vtSphereProbe, s.slot, (unsigned long long)s.fn);
            return false;
        }
    }
    // 0x665f70: push rbp; mov rbp,rsp; push r15; push r14; push r12; push rbx; sub rsp,0x80; mov r12,rdi
    static const unsigned char pos_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
                                              0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00, 0x49, 0x89, 0xfc};
    // 0x5566b0: push rbp; mov rbp,rsp; push r14; push rbx; sub rsp,0x30; mov rbx,rdi
    static const unsigned char view_bytes[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x89, 0xfb};
    // 0x66618c (the reader 0x666160): lea r14,[rax+0x1b0]; movsxd rdx,[rax+0x1d4]
    static const unsigned char reader_bytes[] = {0x4c, 0x8d, 0xb0, 0xb0, 0x01, 0x00, 0x00, 0x48, 0x63, 0x90, 0xd4, 0x01, 0x00, 0x00};
    // 0x662797 (tracker ctor): lea rax,[rip+..] (-> 0x1004180); mov [rbx+0x1b0],rax
    static const unsigned char ctor_bytes[] = {0x48, 0x8d, 0x05, 0xe2, 0x19, 0x9a, 0x00, 0x48, 0x89, 0x83, 0xb0, 0x01, 0x00, 0x00};
    // 0x63e813: call 0x666160 (NetGameManager::Update, the visibility read)
    static const unsigned char call_bytes[] = {0xe8, 0x48, 0x79, 0x02, 0x00};
    if (!image.matches(fPlayerPos, pos_bytes, sizeof pos_bytes) || !image.matches(fViewTest, view_bytes, sizeof view_bytes) ||
        !image.matches(0x66618c, reader_bytes, sizeof reader_bytes) || !image.matches(0x662797, ctor_bytes, sizeof ctor_bytes) ||
        !image.matches(0x63e813, call_bytes, sizeof call_bytes)) {
        log("visibility: player position / view test / reader / tracker ctor bytes differ, not installed");
        return false;
    }
    return kNetState + 4 <= image.size && kCamera + 16 <= image.size && kSiteC + 8 <= image.size;
}

}  // namespace

void visibility_kick() {
    if (!g_ready || G<int32_t>(kNetState) < 2 || !G<uint64_t>(kCollisionWorld)) return;
    // Playing::Update may leave the state (GoShowResults / GoLeave) and run Exit inside the update; then
    // no new probe is kicked this frame, so none is in flight when LoadLevels resets the trackers.
    if (g_exit_frame == G<int32_t>(kProbeFrame)) return;
    const uint64_t local = G<uint64_t>(kNgm);
    if (!local) return;
    for (int i = 0; i < kTrackers; ++i) kick_one(i, reinterpret_cast<const uint8_t *>(uintptr_t(local)));
}

void install_visibility(int &installed) {
    if (!image.base || !layout_matches()) return;
    uint64_t &slot = *image.at<uint64_t>(sPlayingExit);
    if (slot != uint64_t(uintptr_t(image.base + hPlayingExit))) {
        log("visibility: Playing::Exit slot 0x%llx is not 0x%llx, not installed", (unsigned long long)sPlayingExit,
            (unsigned long long)hPlayingExit);
        return;
    }
    previous_exit = reinterpret_cast<ExitFn>(uintptr_t(slot));
    slot = uint64_t(uintptr_t(&playing_exit));
    g_ready = true;
    log("visibility: KickVisibilityJob (PS3 0x494c38) ready; Playing::Exit 0x%llx releases the probes",
        (unsigned long long)hPlayingExit);
    ++installed;
}

}  // namespace u2mp
