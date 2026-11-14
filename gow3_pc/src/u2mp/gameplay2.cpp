// u2mp: Apply bodies for the two connection-handshake net events that the PS4 1.02 build left as
// a bare `ret` (type 0 NetEventStartConnection, type 1 NetEventConnectionDone).
//
// PS4 also lost the globals the PS3 handshake kept, so these Applies work from what survives:
// a client asked to start the connection answers the host at once through the PS4 sender, and
// the host, on ConnectionDone, finds the sending player via the connection record the event
// arrived on and marks that player connected. Nothing on PS4 sends either event today; the
// Applies make them useful if something does. Hooked in by overwriting vtable slot 4.
//
// Reimplemented from the behaviour description u2mp/docs/spec/events.md.
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
namespace {

// Image facts (offsets into the PS4 1.02 image).
constexpr uint64_t kNetState = 0x14ce97c;          // s32; above 1 means a multiplayer session
constexpr uint64_t kMatchSession = 0x1065b08;      // +0x10 context, +0xb00 my net id, +0xb04 host net id
constexpr uint64_t kNetGameManager = 0x113b760;
constexpr uint64_t kTrackers = kNetGameManager + 0x10;
constexpr int kTrackerCount = 10;
constexpr uint64_t kTrackerStride = 0x2c0;
constexpr uint64_t kConnRecords = 0x1152a18;
constexpr uint64_t kConnRecordCount = 32;
constexpr uint64_t kConnRecordStride = 0x220;
constexpr uint64_t kConnArgOffset = 0x40;          // the Apply receives record + 0x40
constexpr uint64_t kSendConnectionDone = 0x60c0f0;

constexpr uint64_t kStartVtable = 0x1041520, kStartStub = 0x63c0e0;
constexpr uint64_t kDoneVtable = 0x10414d0, kDoneStub = 0x63bda0;

template <typename T> T load(const void *base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(base) + off, sizeof v);
    return v;
}
template <typename T> void store(void *base, size_t off, T v) {
    std::memcpy(static_cast<uint8_t *>(base) + off, &v, sizeof v);
}

// The PS3 kept an "asked to start connection" flag; nothing reads it on PS4, it is kept for parity.
[[maybe_unused]] volatile bool asked_to_start = false;

U2MP_GUEST void apply_start_connection(void * /*event*/, void *connection) {
    asked_to_start = true;
    log("************** Asked to start Connection");
    const uint8_t *match = image.at<const uint8_t>(kMatchSession);
    const bool host = load<uint64_t>(match, 0x10) == 0 || load<uint32_t>(match, 0xb00) == load<uint32_t>(match, 0xb04);
    // Only a client in a multiplayer session that heard this over a real connection answers.
    if (!(*image.at<const int32_t>(kNetState) > 1 && !host && connection)) return;
    const uint32_t host_id = load<uint32_t>(match, 0xb04);
    reinterpret_cast<void(U2MP_GUEST *)(uint32_t, uint8_t, int32_t, int32_t)>(image.base + kSendConnectionDone)(
        host_id, 1, 0, 0);
    log("************** Connections done sending to host %u", host_id);
    asked_to_start = false;
}

// The connection key of the record `connection` points into; 0 when it is not one of the records
// or the record is not in use.
uint32_t connection_key(const void *connection) {
    if (!connection) return 0;
    const uintptr_t first = uintptr_t(image.base + kConnRecords + kConnArgOffset);
    const uintptr_t at = uintptr_t(connection);
    if (at < first) return 0;
    const uintptr_t distance = at - first;
    if (distance % kConnRecordStride != 0 || distance / kConnRecordStride >= kConnRecordCount) return 0;
    const uint8_t *record = static_cast<const uint8_t *>(connection) - kConnArgOffset;
    return record[4] ? load<uint32_t>(record, 0) : 0;
}

U2MP_GUEST void apply_connection_done(void *event, void *connection) {
    const uint8_t can_talk = load<uint8_t>(event, 0x0c);
    const int32_t a = load<int32_t>(event, 0x10);
    const int32_t b = load<int32_t>(event, 0x14);
    const uint32_t key = connection_key(connection);
    if (!key) return;
    for (int i = 0; i < kTrackerCount; ++i) {
        uint8_t *tracker = image.at<uint8_t>(kTrackers + uint64_t(i) * kTrackerStride);
        const char *name = reinterpret_cast<const char *>(tracker + 0x20);
        if (name[0] == 0 || load<int32_t>(tracker, 0x23c) != int32_t(key)) continue;
        store<uint8_t>(tracker, 0x27c, can_talk);
        store<uint8_t>(tracker, 0x275, 1);
        log("************** Connection established for %s (net id %u)  canTalkToAll = %u (%d, %d)", name,
            load<uint32_t>(tracker, 0x120), unsigned(can_talk), a, b);
        return;
    }
    log("NetEventConnectionDone: no player tracker for connection key %u", key);
}

struct Anchor {
    uint64_t offset;
    unsigned char bytes[9];
    size_t length;
};
// Code the handshake depends on, checked before anything is hooked.
const Anchor kAnchors[] = {
    {0x63bfe9, {0x41, 0x0F, 0x95, 0x46, 0x0C}, 5},
    {0x63c059, {0x41, 0x89, 0x5E, 0x10}, 4},
    {0x63c0c9, {0x41, 0x89, 0x5E, 0x14}, 4},
    {0x60c0f0, {0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56}, 8},
    {0x60c174, {0x45, 0x88, 0x67, 0x0C, 0x41, 0x89, 0x5F, 0x10}, 8},
    {0x60c17f, {0x41, 0x89, 0x47, 0x14}, 4},
    {0x5f2f13, {0x41, 0x80, 0xBC, 0x15, 0x85, 0x02, 0x00, 0x00, 0x00}, 9},
    {0x5f2ef8, {0x41, 0x80, 0xBC, 0x15, 0x7C, 0x02, 0x00, 0x00, 0x00}, 9},
};

struct RipRef {
    uint64_t offset;
    uint64_t length;
    uint64_t disp_at;
    uint64_t target;
};
// RIP-relative instructions that must still point at the vtables and the NetGameManager we use.
const RipRef kRipRefs[] = {
    {0x60c165, 7, 3, kDoneVtable},
    {0x60bf8c, 7, 3, kStartVtable},
    {0x5f2ec2, 7, 3, kNetGameManager},
};

struct Hook {
    const char *name;
    uint64_t vtable;
    uint64_t stub;
    void *fn;
};

}  // namespace

void install_gameplay2(int &installed) {
    if (!image.base) return;
    for (const Anchor &a : kAnchors) {
        if (!image.matches(a.offset, a.bytes, a.length)) {
            log("gameplay2: code at 0x%llx differs, connection events left stubbed",
                static_cast<unsigned long long>(a.offset));
            return;
        }
    }
    for (const RipRef &r : kRipRefs) {
        if (r.offset + r.length > image.size) return;
        const int32_t disp = load<int32_t>(image.base, r.offset + r.disp_at);
        if (int64_t(r.offset + r.length) + int64_t(disp) != int64_t(r.target)) {
            log("gameplay2: 0x%llx does not reference 0x%llx, connection events left stubbed",
                static_cast<unsigned long long>(r.offset), static_cast<unsigned long long>(r.target));
            return;
        }
    }
    if (kConnRecords + kConnRecordCount * kConnRecordStride > image.size || kNetGameManager + 0x1d00 > image.size)
        return;

    const Hook hooks[] = {
        {"NetEventStartConnection::Apply", kStartVtable, kStartStub, reinterpret_cast<void *>(apply_start_connection)},
        {"NetEventConnectionDone::Apply", kDoneVtable, kDoneStub, reinterpret_cast<void *>(apply_connection_done)},
    };
    for (const Hook &h : hooks) {
        if (h.vtable + 0x28 > image.size) continue;
        uint64_t *slot = image.at<uint64_t>(h.vtable + 0x20);
        if (*slot != uint64_t(uintptr_t(image.base + h.stub)) || !is_ret_stub(h.stub)) {
            log("%s: vtable 0x%llx slot 4 is not the stub 0x%llx, left alone", h.name,
                static_cast<unsigned long long>(h.vtable), static_cast<unsigned long long>(h.stub));
            continue;
        }
        *slot = uint64_t(uintptr_t(h.fn));
        log("%s: vtable 0x%llx slot 4 -> C++", h.name, static_cast<unsigned long long>(h.vtable));
        ++installed;
    }
}

}  // namespace u2mp
