// u2mp visibility: kicks the line-of-sight probes that make remote players "visible".
//
// Name tags and spotting only show a remote player when a collision probe from the camera to that
// player was kicked in this frame or the one before. PS4 no longer kicks these probes, so nobody is
// ever visible. Once per frame (from matchend's Playing::Update wrapper, via visibility_kick) we
// kick one thin sphere-cast probe per remote player that passes the view test, using the probe
// object every tracker already owns. When the Playing state exits we release the probes we kicked
// before the original Exit runs, so none outlive the match.
//
// Reimplemented from the behaviour description u2mp/docs/spec/gameplay.md.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
void install_visibility(int &installed);
void visibility_kick();

namespace {

constexpr uint64_t kNgm = 0x113b760;
constexpr uint64_t kTrackers = 0x113b770;
constexpr uint64_t kTrackerStride = 0x2c0;
constexpr int kTrackerCount = 10;
constexpr uint64_t kUsed = 0x20;
constexpr uint64_t kNetId = 0x120;
constexpr uint64_t kProbe = 0x1b0;
constexpr uint64_t kExcluded = 0x26c;

constexpr uint64_t kNetState = 0x14ce97c;
constexpr uint64_t kCollisionWorld = 0x1127dd0;
constexpr uint64_t kCamera = 0x104e820;
constexpr uint64_t kDefaultFilter = 0x151f048;
constexpr uint64_t kKickA = 0x151f058;
constexpr uint64_t kKickB = 0x151f060;
constexpr uint64_t kKickC = 0x151f068;
constexpr uint64_t kProbeFrame = 0x11161a8;

constexpr uint64_t kPlayerPosition = 0x665f70;
constexpr uint64_t kViewTest = 0x5566b0;
constexpr uint64_t kProbeVtable = 0x1004180;

constexpr uint64_t kExitSlot = 0x100ab90;
constexpr uint64_t kPlayingExit = 0x5f2350;

// Probe vtable slots we use, with the function each must hold in this build.
enum Slot : unsigned { Init = 1, Kick = 2, Release = 5, SetRayDir = 7, SetFilter = 9, IsActive = 12, SetRadius = 32 };
struct SlotCheck {
    unsigned slot;
    uint64_t function;
};
const SlotCheck kSlotChecks[] = {
    {Init, 0x4f1dc0},     {Kick, 0x4f2130},      {Release, 0x4f2490}, {SetRayDir, 0x4f25b0},
    {SetFilter, 0x4f26c0}, {IsActive, 0x4f2850}, {SetRadius, 0x4f2600},
};

template <typename T> T peek(const unsigned char *p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <typename T> T peek_image(uint64_t off) { return peek<T>(image.base + off); }

bool kicked[kTrackerCount] = {};
int kick_count = 0;
int64_t exit_frame = -1;   // probe frame of the last Playing::Exit; no kicks in that frame
bool ready = false;
uint64_t original_exit = 0;

unsigned char *tracker_at(int i) { return image.at<unsigned char>(kTrackers + uint64_t(i) * kTrackerStride); }

bool is_sphere_probe(const unsigned char *probe) {
    return peek<uint64_t>(probe) == uint64_t(uintptr_t(image.base + kProbeVtable));
}

// The probe's virtual function `slot`, looked up at the moment of the call.
template <typename Fn> Fn probe_slot(unsigned char *probe, unsigned slot) {
    const unsigned char *vptr = reinterpret_cast<const unsigned char *>(peek<uint64_t>(probe));
    return reinterpret_cast<Fn>(peek<uint64_t>(vptr + 8ull * slot));
}

using ProbeFn = void(U2MP_GUEST *)(unsigned char *);
using IsActiveFn = uint8_t(U2MP_GUEST *)(unsigned char *);
using InitFn = void(U2MP_GUEST *)(unsigned char *, uint32_t, uint32_t, uint32_t, uint32_t, void *, uint32_t);
using RayDirFn = void(U2MP_GUEST *)(unsigned char *, uint32_t, const float *, const float *, float);
using RadiusFn = void(U2MP_GUEST *)(unsigned char *, uint32_t, float);
using FilterFn = void(U2MP_GUEST *)(unsigned char *, uint32_t, uint32_t, uint32_t, const uint64_t *);
using KickFn = void(U2MP_GUEST *)(unsigned char *, uint64_t, uint32_t, uint64_t, uint32_t, uint32_t);
using PositionFn = uint64_t(U2MP_GUEST *)(float *, unsigned char *);
using ViewTestFn = uint64_t(U2MP_GUEST *)(unsigned char *, float, const float *);
using ExitFn = uint64_t(U2MP_GUEST *)(void *);

// Only a probe that is still a sphere probe and still running is released.
void release_probe(unsigned char *probe) {
    if (!is_sphere_probe(probe)) return;
    if (probe_slot<IsActiveFn>(probe, IsActive)(probe) != 0) probe_slot<ProbeFn>(probe, Release)(probe);
}

U2MP_GUEST uint64_t exit_hook(void *state) {
    exit_frame = int64_t(peek_image<int32_t>(kProbeFrame));
    int released = 0;
    for (int i = 0; i < kTrackerCount; ++i) {
        if (!kicked[i]) continue;
        kicked[i] = false;
        if (peek_image<uint64_t>(kCollisionWorld) == 0) continue;   // nothing left to release into
        release_probe(tracker_at(i) + kProbe);
        ++released;
    }
    if (released != 0) log("visibility: %d probe(s) released (%s)", released, "Playing::Exit");
    return reinterpret_cast<ExitFn>(original_exit)(state);
}

}  // namespace

void visibility_kick() {
    if (!ready || peek_image<int32_t>(kNetState) < 2 || peek_image<uint64_t>(kCollisionWorld) == 0) return;
    // Playing::Exit already ran in this frame: kicking now would leave a probe nobody releases.
    if (exit_frame == int64_t(peek_image<int32_t>(kProbeFrame))) return;
    unsigned char *local = reinterpret_cast<unsigned char *>(peek_image<uint64_t>(kNgm));
    if (!local) return;

    for (int i = 0; i < kTrackerCount; ++i) {
        unsigned char *tracker = tracker_at(i);
        unsigned char *probe = tracker + kProbe;
        const uint32_t net_id = peek<uint32_t>(tracker + kNetId);
        const uint32_t local_id = peek<uint32_t>(local + kNetId);

        // Not a remote player (any more): drop the probe we kicked for this slot earlier.
        if (peek<uint8_t>(tracker + kUsed) == 0 || peek<uint8_t>(tracker + kExcluded) != 0 || net_id == local_id) {
            if (kicked[i]) {
                kicked[i] = false;
                release_probe(probe);
            }
            continue;
        }
        if (!is_sphere_probe(probe)) continue;

        alignas(16) float target[4] = {};
        reinterpret_cast<PositionFn>(image.base + kPlayerPosition)(target, tracker);
        alignas(16) unsigned char view[0x80] = {};
        reinterpret_cast<ViewTestFn>(image.base + kViewTest)(view, 0.0f, target);
        if (peek<int32_t>(view + 0x2c) != 0) continue;   // off screen: no line of sight to test

        alignas(16) float start[4];
        std::memcpy(start, image.base + kCamera, sizeof start);
        alignas(16) float delta[4];
        for (int k = 0; k < 4; ++k) delta[k] = target[k] - start[k];

        // One hit-test sphere cast from the camera to the player (ray length 1 = the full delta).
        probe_slot<InitFn>(probe, Init)(probe, 1, 32, 0, 13, nullptr, 0);
        probe_slot<RayDirFn>(probe, SetRayDir)(probe, 0, start, delta, 1.0f);
        probe_slot<RadiusFn>(probe, SetRadius)(probe, 0, 0.01f);
        const uint64_t filter_object = peek_image<uint64_t>(kDefaultFilter);
        probe_slot<FilterFn>(probe, SetFilter)(probe, 0, 26, 0, &filter_object);
        probe_slot<KickFn>(probe, Kick)(probe, peek_image<uint64_t>(kKickA), peek_image<uint32_t>(kKickB),
                                        peek_image<uint64_t>(kKickC), 0, 1);

        kicked[i] = true;
        if (++kick_count == 1) log("visibility: first probe kicked (tracker %d, net id %u)", i, net_id);
    }
}

void install_visibility(int &installed) {
    if (!image.base) return;
    for (const SlotCheck &check : kSlotChecks) {
        const uint64_t slot_offset = kProbeVtable + 8ull * check.slot;
        if (slot_offset + 8 > image.size ||
            peek_image<uint64_t>(slot_offset) != uint64_t(uintptr_t(image.base + check.function))) {
            log("visibility: probe vtable 0x%llx slot %u is not 0x%llx, not installed",
                (unsigned long long)kProbeVtable, check.slot, (unsigned long long)check.function);
            return;
        }
    }

    // Prologues of the two guest functions we call, code that reads the probe at tracker +0x1b0,
    // the tracker constructor storing the sphere probe vtable there, and one more anchor call.
    static const unsigned char position[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53,
                                             0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x49, 0x89, 0xFC};
    static const unsigned char view_test[] = {0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53,
                                              0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0xFB};
    static const unsigned char reader[] = {0x4C, 0x8D, 0xB0, 0xB0, 0x01, 0x00, 0x00,
                                           0x48, 0x63, 0x90, 0xD4, 0x01, 0x00, 0x00};
    static const unsigned char ctor[] = {0x48, 0x8D, 0x05, 0xE2, 0x19, 0x9A, 0x00,
                                         0x48, 0x89, 0x83, 0xB0, 0x01, 0x00, 0x00};
    static const unsigned char call[] = {0xE8, 0x48, 0x79, 0x02, 0x00};
    if (!image.matches(kPlayerPosition, position, sizeof position) ||
        !image.matches(kViewTest, view_test, sizeof view_test) || !image.matches(0x66618c, reader, sizeof reader) ||
        !image.matches(0x662797, ctor, sizeof ctor) || !image.matches(0x63e813, call, sizeof call)) {
        log("visibility: player position / view test / reader / tracker ctor bytes differ, not installed");
        return;
    }
    if (kNetState + 4 > image.size || kCamera + 16 > image.size || kKickC + 8 > image.size) return;

    uint64_t *exit_slot = image.at<uint64_t>(kExitSlot);
    if (*exit_slot != uint64_t(uintptr_t(image.base + kPlayingExit))) {
        log("visibility: Playing::Exit slot 0x%llx is not 0x%llx, not installed", (unsigned long long)kExitSlot,
            (unsigned long long)kPlayingExit);
        return;
    }
    original_exit = *exit_slot;
    *exit_slot = uint64_t(uintptr_t(&exit_hook));
    ready = true;
    log("visibility: KickVisibilityJob (PS3 0x494c38) ready; Playing::Exit 0x%llx releases the probes",
        (unsigned long long)kPlayingExit);
    installed += 1;
}

}  // namespace u2mp
