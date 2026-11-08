// u2mp gameplay2: the connection-handshake event Applies PS4 1.02 left as `ret`, ported from
// PS3 1.10 (notes: u2mp/docs/PORT-GAMEPLAY2.md).
//
//   type 0 NetEventStartConnection::Apply  PS3 0x474e50 -> 0x43d94c   PS4 0x63c0e0 (ret), vtable 0x1041520 slot 4
//   type 1 NetEventConnectionDone::Apply   PS3 0x474d90 -> 0x43dcd8   PS4 0x63bda0 (ret), vtable 0x10414d0 slot 4
//
// PS3 handshake (TRANSPORT-INTERFACE.md section 6): NpNetUpdate 0x43bdd0 on the host remembers the
// peer it is connecting (pending net id 0xe15594) and sends it StartConnection (0x489c50). The peer's
// Apply sets "asked to start connection" (0xf12620 = 1); its NpNetUpdate then connects to every room
// member, answers ConnectionDone(canTalkToAll, avg a, avg b) to the host (0x489ec8, from 0x43c94c)
// and clears the flag. The host's Apply (0x43dcd8) takes the tracker of the pending net id and sets
// tracker+580 = canTalkToAll, tracker+573 = 1 (PS4 +0x27c / +0x275), then pending = -1.
//
// PS4 has neither NpNetUpdate nor the two globals: the senders 0x60bf20 / 0x60c0f0 survive but are
// never called, and session.cpp marks every registered tracker connected itself. These ports make
// the events work if anything sends them (our session layer, or a peer running other code):
//   - StartConnection on a client: our transport is already connected (the event arrived through
//     it) and every client reaches the others through the host, so it answers at once with
//     ConnectionDone(host net id, canTalkToAll = 1, 0, 0) through the intact PS4 sender 0x60c0f0.
//     The two PS3 averages are only printed by the host, so 0 is sent.
//   - ConnectionDone on the host: PS4 has no pending-id global, so the sender is found from the
//     connection the event came in on: Apply's second argument is M = record + 0x40 of the 32
//     connection records at 0x1152a18 (gamenet.cpp), record+0 = connection key = tracker+0x23c.
//
// Checked and not ported (nothing missing on PS4; details in PORT-GAMEPLAY2.md):
//   StartConnection Read/Write 0x63c0f0/0x63c100, EarnMedalClient Read/Write 0x61d260/0x61d270,
//   CompleteTask Read/Write 0x62d8f0/0x62d900: PS3 bodies are `blr` (0x468c18/0x468c1c,
//   0x468dc0/0x468dc4, 0x468dcc/0x468dd0) and no Apply reads a payload (EarnMedalClient Apply
//   0x61d250 = PS3 0x468dc8 blr; CompleteTask Apply 0x62d820 = PS3 0x4711c0 never reads the event).
//   net-get-net-id-from-player 0x668fe0: complete; PS3 0x4ce030 is the same loop unrolled 10 times.
//   "(no pat found)" PS3 0x45e728: inlined into the PS4 NetCharacter update 0x5fe4b0 (0x5ff30c..).
//   HashTable::Init PS3 0x494ee0 / 0x4950f0: inlined on PS4; netinit.cpp builds both tables.
//   NetAnimList::AddAnim PS3 0x45212c: only caller PS3 0x453c10, whose PS4 slot is a ret stub
//   (0x5f48d0) - left open, see the notes.
#include <cstdint>
#include <cstring>

#include "u2mp_internal.h"

namespace u2mp {
void install_gameplay2(int &installed);

namespace {

// ---- PS4 1.02 image offsets ----------------------------------------------------------------
constexpr uint64_t kNetState = 0x14ce97c;   // NET_SM; multiplayer when > 1 (PS3 IsMultiplayer 0x4384b0)
constexpr uint64_t kMatch = 0x1065b08;      // g_matchSession: +0x10 context, +0xb00 me, +0xb04 host
constexpr uint64_t kNgm = 0x113b760;        // g_netGameManager (PS3 0xf1dc80)
constexpr uint64_t kRecords = 0x1152a18;    // 32 connection records of 0x220 bytes; M = record + 0x40
constexpr uint64_t kRecordSize = 0x220, kLayer = 0x40;
constexpr int kRecordCount = 32;

constexpr uint64_t NGM_TRACKERS = 0x10, NGM_TRACKER_SIZE = 0x2c0;
constexpr int kTrackers = 10;
// tracker fields (PS3 +24 name, +280 net id, +516 key, +564 excluded, +573 connected, +580 canTalkToAll)
constexpr uint64_t T_NAME = 0x20, T_NETID = 0x120, T_KEY = 0x23c, T_CONNECTED = 0x275, T_TALKS_TO_ALL = 0x27c;
// NetEventConnectionDone payload (PS4 Read 0x63bf70 / Write 0x63bdb0; PS3 +8 / +12 / +16)
constexpr uint64_t CD_TALKS_TO_ALL = 0x0c, CD_A = 0x10, CD_B = 0x14;

constexpr uint64_t vtStartConnection = 0x1041520, vtConnectionDone = 0x10414d0;
constexpr uint64_t sStartConnectionApply = 0x63c0e0, sConnectionDoneApply = 0x63bda0;
constexpr uint64_t fSendConnectionDone = 0x60c0f0;   // (target net id, canTalkToAll, a, b) (PS3 0x489ec8)

using SendConnectionDoneFn = void(U2MP_GUEST *)(uint32_t target, uint8_t can_talk_to_all, int32_t a, int32_t b);

template <typename T> T rd(const void *p, uint64_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}
template <typename T> void wr(void *p, uint64_t off, T v) { std::memcpy(static_cast<uint8_t *>(p) + off, &v, sizeof v); }

bool is_multiplayer() { return rd<int32_t>(image.base, kNetState) > 1; }

// PS3 IsHost 0x494ea8 (inlined on PS4, e.g. net-is-host 0x669c80).
bool is_host() {
    const uint8_t *match = image.at<uint8_t>(kMatch);
    return rd<uint64_t>(match, 0x10) == 0 || rd<uint32_t>(match, 0xb00) == rd<uint32_t>(match, 0xb04);
}

uint8_t *tracker(int i) { return image.at<uint8_t>(kNgm + NGM_TRACKERS + uint64_t(i) * NGM_TRACKER_SIZE); }

// The connection key of the record whose message layer is `connection`, 0 when it is none of them
// (NULL for a local Apply, or a stand-in connection such as session.cpp's mailbox_post).
uint32_t connection_key(const void *connection) {
    const uintptr_t first = uintptr_t(image.base + kRecords + kLayer);
    const uintptr_t m = uintptr_t(connection);
    if (!connection || m < first) return 0;
    const uintptr_t delta = m - first;
    if (delta % kRecordSize || delta / kRecordSize >= uintptr_t(kRecordCount)) return 0;
    const uint8_t *record = static_cast<const uint8_t *>(connection) - kLayer;
    return rd<uint8_t>(record, 0x04) ? rd<uint32_t>(record, 0x00) : 0;
}

// ---- 0 NetEventStartConnection::Apply (PS3 0x474e50: debug print, then 0x43d94c) ---------------
// 0x43d94c: g_askedToStartConnection (0xf12620) = 1, "Asked to start Connection". The answer that
// PS3 NpNetUpdate sent once its peer links were up (0x43c8b4..0x43c958) is sent here.
bool asked_to_start_connection = false;

U2MP_GUEST void start_connection_apply(void *, void *connection) {
    asked_to_start_connection = true;
    log("************** Asked to start Connection");
    if (!is_multiplayer() || is_host() || !connection) return;   // PS3: only a client answers (!IsHost)
    const uint32_t host = rd<uint32_t>(image.at<uint8_t>(kMatch), 0xb04);
    reinterpret_cast<SendConnectionDoneFn>(image.base + fSendConnectionDone)(host, 1, 0, 0);
    log("************** Connections done sending to host %u", host);
    asked_to_start_connection = false;   // PS3 0x43c954
}

// ---- 1 NetEventConnectionDone::Apply (PS3 0x474d90: debug print, then 0x43dcd8(+8, +12, +16)) --
U2MP_GUEST void connection_done_apply(void *ev, void *connection) {
    const uint8_t can_talk_to_all = rd<uint8_t>(ev, CD_TALKS_TO_ALL);
    const int32_t a = rd<int32_t>(ev, CD_A), b = rd<int32_t>(ev, CD_B);
    const uint32_t key = connection_key(connection);
    if (!key) return;   // PS3: pending id == -1, nothing to do
    for (int i = 0; i < kTrackers; ++i) {
        uint8_t *t = tracker(i);
        if (!rd<char>(t, T_NAME) || rd<int32_t>(t, T_KEY) != int32_t(key)) continue;
        wr<uint8_t>(t, T_TALKS_TO_ALL, can_talk_to_all);   // PS3 tracker+580
        wr<uint8_t>(t, T_CONNECTED, 1);                    // PS3 tracker+573
        log("************** Connection established for %s (net id %u)  canTalkToAll = %u (%d, %d)",
            reinterpret_cast<const char *>(t + T_NAME), rd<uint32_t>(t, T_NETID), unsigned(can_talk_to_all), a, b);
        return;
    }
    log("NetEventConnectionDone: no player tracker for connection key %u", key);
}

struct Anchor {
    uint64_t at;
    unsigned char bytes[9];
    uint8_t length;
};

// Surviving PS4 code that pins every layout fact used above.
const Anchor kAnchors[] = {
    {0x63bfe9, {0x41, 0x0f, 0x95, 0x46, 0x0c}, 5},                          // Read: setne [ev+0xc]
    {0x63c059, {0x41, 0x89, 0x5e, 0x10}, 4},                                // Read: [ev+0x10] = 32 bits
    {0x63c0c9, {0x41, 0x89, 0x5e, 0x14}, 4},                                // Read: [ev+0x14] = 32 bits
    {0x60c0f0, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56}, 8},        // ConnectionDone sender prologue
    {0x60c174, {0x45, 0x88, 0x67, 0x0c, 0x41, 0x89, 0x5f, 0x10}, 8},        // sender: +0xc = arg 2, +0x10 = arg 3
    {0x60c17f, {0x41, 0x89, 0x47, 0x14}, 4},                                // sender: +0x14 = arg 4
    {0x5f2f13, {0x41, 0x80, 0xbc, 0x15, 0x85, 0x02, 0x00, 0x00, 0x00}, 9},  // "Connections done": tracker+0x275
    {0x5f2ef8, {0x41, 0x80, 0xbc, 0x15, 0x7c, 0x02, 0x00, 0x00, 0x00}, 9},  // ... tracker+0x26c excluded
};

// RIP-relative references: {instruction, length, displacement offset, target}.
struct RipRef {
    uint64_t at;
    uint8_t length, disp;
    uint64_t target;
};
const RipRef kRipRefs[] = {
    {0x60c165, 7, 3, vtConnectionDone},    // sender stores the ConnectionDone vtable
    {0x60bf8c, 7, 3, vtStartConnection},   // StartConnection sender 0x60bf20 stores its vtable
    {0x5f2ec2, 7, 3, kNgm},                // "Connections done" walks g_netGameManager's trackers
};

bool layout_matches() {
    for (const Anchor &a : kAnchors)
        if (!image.matches(a.at, a.bytes, a.length)) {
            log("gameplay2: code at 0x%llx differs, connection events left stubbed", (unsigned long long)a.at);
            return false;
        }
    for (const RipRef &r : kRipRefs) {
        if (r.at + r.length > image.size) return false;
        int32_t disp;
        std::memcpy(&disp, image.base + r.at + r.disp, 4);
        if (int64_t(r.at + r.length) + disp != int64_t(r.target)) {
            log("gameplay2: 0x%llx does not reference 0x%llx, connection events left stubbed",
                (unsigned long long)r.at, (unsigned long long)r.target);
            return false;
        }
    }
    return kRecords + uint64_t(kRecordCount) * kRecordSize <= image.size && kNgm + 0x1d00 <= image.size;
}

}  // namespace

void install_gameplay2(int &installed) {
    if (!image.base || !layout_matches()) return;
    const uint64_t base = uint64_t(uintptr_t(image.base));
    const struct {
        uint64_t vtable, stub;
        void *host;
        const char *name;
    } slots[] = {
        {vtStartConnection, sStartConnectionApply, reinterpret_cast<void *>(&start_connection_apply),
         "NetEventStartConnection::Apply"},
        {vtConnectionDone, sConnectionDoneApply, reinterpret_cast<void *>(&connection_done_apply),
         "NetEventConnectionDone::Apply"},
    };
    for (const auto &s : slots) {
        if (s.vtable + 0x28 > image.size) continue;
        uint64_t &slot = *image.at<uint64_t>(s.vtable + 8 * 4);
        if (slot != base + s.stub || !is_ret_stub(s.stub)) {
            log("%s: vtable 0x%llx slot 4 is not the stub 0x%llx, left alone", s.name,
                (unsigned long long)s.vtable, (unsigned long long)s.stub);
            continue;
        }
        slot = uint64_t(uintptr_t(s.host));
        log("%s: vtable 0x%llx slot 4 -> C++", s.name, (unsigned long long)s.vtable);
        ++installed;
    }
}

}  // namespace u2mp
