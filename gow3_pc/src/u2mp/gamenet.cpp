// u2mp game transport bridge (see gamenet.h). Addresses: Uncharted 2 PS4 1.02 image offsets; PS3
// 1.10 originals in the comments. Field map: u2mp/docs/TRANSPORT-INTERFACE.md sections 1.4 and 2.
#include "gamenet.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bitstream.h"
#include "u2mp_internal.h"

namespace u2mp::gamenet {
namespace {

// ---- guest addresses ------------------------------------------------------------------------
constexpr uint64_t kRecords = 0x1152a18;     // 32 connection records, 0x220 bytes each
constexpr uint64_t kRecordSize = 0x220;
constexpr int kRecordCount = 32;
constexpr uint64_t kMessageVtable = 0x103fc80;
constexpr uint64_t kFrameClock = 0x1116180 + 0x204;  // game frame counter (300 per second)
constexpr uint64_t kNoopHook = 0x5e9d80;             // M+0x150 receive hook the game installs

// Message layer (M = record + 0x40) fields
constexpr uint64_t M_NEXT_SEQ = 0x08, M_RECV_SEQ = 0x0c, M_RECV_BITS = 0x14, M_ACKED_SEQ = 0x18;
constexpr uint64_t M_LAST_SEND = 0xa4, M_LAST_RECV = 0xa8;
constexpr uint64_t M_INFLIGHT_HEAD = 0x100, M_INFLIGHT_TAIL = 0x108;
constexpr uint64_t M_RELIABLE_HEAD = 0x110, M_UNRELIABLE_HEAD = 0x120, M_HOOK = 0x150;
// Packet record (ours, 0x18 bytes as the game's AllocPacketRecord): +0 seq, +8 next, +0x10 messages
constexpr uint64_t R_SEQ = 0x00, R_NEXT = 0x08;

constexpr int kWindow = 30;               // PS3: "%s window full" at 30 unacknowledged packets
constexpr uint32_t kMaxBits = 1200 * 8;   // the message layer's packet budget
constexpr double kSendInterval = 0.05, kIdleInterval = 2.0;

using ReadMessagesFn = void(U2MP_GUEST *)(void *M, void *bitstream);
using WriteMessagesFn = void(U2MP_GUEST *)(void *M, void *bitstream, void *record);
using PacketFn = void(U2MP_GUEST *)(void *M, void *record);
using ResetFn = void(U2MP_GUEST *)(void *M);

struct Fns {
    ReadMessagesFn read;
    WriteMessagesFn write;
    PacketFn delivered, lost;
    ResetFn reset;
} fn{};

template <typename T> T &field(uint64_t object, uint64_t offset) { return *reinterpret_cast<T *>(object + offset); }

// The game's BitStream and its bit functions: bitstream.h.

// ---- peers ----------------------------------------------------------------------------------
struct Peer {
    bool used = false;
    int record = -1;
    bool direct = true;
    double next_send = 0, last_send = 0;
};
Peer peers[net::kMaxPeers + 1];
net::Transport *transport = nullptr;

uint64_t record_address(int index) { return uint64_t(uintptr_t(image.base)) + kRecords + uint64_t(index) * kRecordSize; }
uint64_t layer_of(int index) { return record_address(index) + 0x40; }

double game_time() { return double(field<uint32_t>(uint64_t(uintptr_t(image.base)), kFrameClock)) / 300.0; }

// The PS3 header writer (0x4652cc / 0x465138): kind byte 0, hasMessages 1, seq 11, ack 10, bits 32.
void send_peer(uint8_t slot, Peer &peer, double now) {
    const uint64_t M = layer_of(peer.record);
    const bool queued = field<uint64_t>(M, M_RELIABLE_HEAD) || field<uint64_t>(M, M_UNRELIABLE_HEAD);
    const bool inflight = field<uint64_t>(M, M_INFLIGHT_HEAD) != 0;
    if (now < peer.next_send || (!queued && !inflight && now - peer.last_send < kIdleInterval)) return;
    std::vector<uint32_t> words(kMaxBits / 32 + 64, 0);   // zeroed: the game's writes OR into words
    BitStream bs{words.data(), 0, kMaxBits + 32 * 32, 0};
    write_bits(bs, 0, 8);
    const uint32_t next = field<uint32_t>(M, M_NEXT_SEQ), acked = field<uint32_t>(M, M_ACKED_SEQ);
    if (int32_t(next - acked) >= kWindow) {   // window full: an ack-only packet
        write_bits(bs, 0, 1);
        write_bits(bs, next & 0x7ff, 11);
        write_bits(bs, field<uint32_t>(M, M_RECV_SEQ) & 0x3ff, 10);
        write_bits(bs, field<uint32_t>(M, M_RECV_BITS), 32);
    } else {
        const uint32_t seq = next + 1;
        field<uint32_t>(M, M_NEXT_SEQ) = seq;
        auto *record = static_cast<uint8_t *>(std::calloc(1, 0x18));
        *reinterpret_cast<uint32_t *>(record + R_SEQ) = seq;
        const uint64_t r = uint64_t(uintptr_t(record));
        if (uint64_t tail = field<uint64_t>(M, M_INFLIGHT_TAIL)) field<uint64_t>(tail, R_NEXT) = r;
        else field<uint64_t>(M, M_INFLIGHT_HEAD) = r;
        field<uint64_t>(M, M_INFLIGHT_TAIL) = r;
        write_bits(bs, 1, 1);
        write_bits(bs, seq & 0x7ff, 11);
        write_bits(bs, field<uint32_t>(M, M_RECV_SEQ) & 0x3ff, 10);
        write_bits(bs, field<uint32_t>(M, M_RECV_BITS), 32);
        // The pack's own soft limit is kMaxBits; the message it starts just under it still has to fit,
        // or the game's inline writes assert: room for 1024 more bits (still within kMaxPacket).
        bs.max_bits = kMaxBits + 1024;
        fn.write(reinterpret_cast<void *>(M), &bs, record);
    }
    field<float>(M, M_LAST_SEND) = float(now);
    const size_t bytes = (size_t(bs.bits) + 31) / 32 * 4;
    std::vector<uint8_t> message(1 + bytes);
    message[0] = kGamePacket;   // the session layer's channel tag
    std::memcpy(message.data() + 1, words.data(), bytes);
    transport->send(slot, false, message.data(), message.size());
    peer.last_send = now;
    peer.next_send = now + kSendInterval;
}

// The PS3 header reader and ack processing (0x465218 / 0x464cb8), then unpack (M vtable[1]).
void receive_peer(Peer &peer, const uint8_t *data, size_t size, double now) {
    if (size < 8 || size > (kMaxBits + 1024) / 8 + 64 || size % 4) return;
    std::vector<uint32_t> words(size / 4 + 2, 0);   // one spare word: the game reads ahead
    std::memcpy(words.data(), data, size);
    BitStream bs{words.data(), 0, uint32_t(size * 8), 0};
    uint32_t kind, has_messages, seq, ack, ack_bits;
    if (!read_bits(bs, kind, 8) || kind != 0 || !read_bits(bs, has_messages, 1) || !read_bits(bs, seq, 11) ||
        !read_bits(bs, ack, 10) || !read_bits(bs, ack_bits, 32))
        return;
    const uint64_t M = layer_of(peer.record);
    uint32_t &recv_seq = field<uint32_t>(M, M_RECV_SEQ);
    uint32_t &recv_bits = field<uint32_t>(M, M_RECV_BITS);
    uint32_t &acked_seq = field<uint32_t>(M, M_ACKED_SEQ);
    const uint32_t next_seq = field<uint32_t>(M, M_NEXT_SEQ);
    // Extend the 11-bit sequence and the 10-bit ack with the high bits we know.
    uint32_t full_seq = (recv_seq & ~0x7ffu) | seq;
    if (int32_t(full_seq - recv_seq) < -1024) full_seq += 0x800;
    else if (int32_t(full_seq - recv_seq) > 1024) full_seq -= 0x800;
    uint32_t full_ack = (next_seq & ~0x3ffu) | ack;
    if (int32_t(full_ack - next_seq) > 0) full_ack -= 0x400;
    if (int32_t(full_seq - recv_seq) > 31 || int32_t(full_ack - next_seq) > 0) return;   // "Ignoring Packet"
    const bool fresh = int32_t(full_seq - recv_seq) > 0;
    if (fresh) {
        const uint32_t shift = full_seq - recv_seq;
        recv_bits = (shift >= 32 ? 0 : recv_bits << shift) | has_messages;
        recv_seq = full_seq;
    }
    // Every in-flight record up to the ack: delivered if its bit is set, else lost.
    if (int32_t(full_ack - acked_seq) > 0) {
        while (uint64_t r = field<uint64_t>(M, M_INFLIGHT_HEAD)) {
            const uint32_t rseq = field<uint32_t>(r, R_SEQ);
            if (int32_t(rseq - full_ack) > 0) break;
            const uint32_t back = full_ack - rseq;
            field<uint64_t>(M, M_INFLIGHT_HEAD) = field<uint64_t>(r, R_NEXT);
            if (!field<uint64_t>(M, M_INFLIGHT_HEAD)) field<uint64_t>(M, M_INFLIGHT_TAIL) = 0;
            field<uint64_t>(r, R_NEXT) = 0;
            if (back < 32 && (ack_bits >> back & 1)) fn.delivered(reinterpret_cast<void *>(M), reinterpret_cast<void *>(r));
            else fn.lost(reinterpret_cast<void *>(M), reinterpret_cast<void *>(r));
            std::free(reinterpret_cast<void *>(r));
        }
        acked_seq = full_ack;
    }
    field<float>(M, M_LAST_RECV) = float(now);
    if (fresh && has_messages) fn.read(reinterpret_cast<void *>(M), &bs);
}

}  // namespace

void set_transport(net::Transport *t) { transport = t; }

void frame(double now) {
    if (!transport || !fn.write) return;
    for (uint8_t slot = 0; slot <= net::kMaxPeers; ++slot)
        if (peers[slot].used && peers[slot].direct) send_peer(slot, peers[slot], now);
}

double clock() { return fn.write ? game_time() : 0; }

int attach(uint8_t slot, uint32_t key, bool direct) {
    if (slot > net::kMaxPeers || !key) return -1;
    detach(slot);
    for (int i = 0; i < kRecordCount; ++i) {
        const uint64_t rec = record_address(i);
        if (field<uint8_t>(rec, 0x04)) continue;
        if (field<uint64_t>(layer_of(i), 0) != uint64_t(uintptr_t(image.base)) + kMessageVtable) {
            log("connection record %d has no message layer yet, slot %u not attached", i, slot);
            return -1;
        }
        field<uint32_t>(rec, 0x00) = key;
        field<uint8_t>(rec, 0x04) = 1;
        field<uint8_t>(rec, 0x06) = direct ? 1 : 0;
        field<uint32_t>(rec, 0x2c) = 0x0100007f;   // any non-zero address and port: the lag check needs them
        field<uint16_t>(rec, 0x30) = 1;
        field<int32_t>(rec, 0x34) = int32_t(key);
        field<int32_t>(rec, 0x38) = 1;
        fn.reset(reinterpret_cast<void *>(layer_of(i)));
        field<uint64_t>(layer_of(i), M_HOOK) = uint64_t(uintptr_t(image.base)) + kNoopHook;
        peers[slot] = Peer{true, i, direct, 0, 0};
        return i;
    }
    log("no free connection record for slot %u", slot);
    return -1;
}

void detach(uint8_t slot) {
    if (slot > net::kMaxPeers || !peers[slot].used) return;
    const int i = peers[slot].record;
    const uint64_t M = layer_of(i);
    // The in-flight packet records are ours (calloc): hand their messages back as lost and free
    // them here, so the game's reset (which frees records with its own allocator) finds none.
    while (uint64_t r = field<uint64_t>(M, M_INFLIGHT_HEAD)) {
        field<uint64_t>(M, M_INFLIGHT_HEAD) = field<uint64_t>(r, R_NEXT);
        field<uint64_t>(r, R_NEXT) = 0;
        fn.lost(reinterpret_cast<void *>(M), reinterpret_cast<void *>(r));
        std::free(reinterpret_cast<void *>(r));
    }
    field<uint64_t>(M, M_INFLIGHT_TAIL) = 0;
    fn.reset(reinterpret_cast<void *>(M));   // frees the queues, resets the sequence counters
    const uint64_t rec = record_address(i);
    field<uint8_t>(rec, 0x04) = 0;
    field<int32_t>(rec, 0x38) = 0;
    field<uint32_t>(rec, 0x00) = 0;
    peers[slot] = Peer{};
}

uint64_t message_layer(uint8_t slot) {
    return slot <= net::kMaxPeers && peers[slot].used ? layer_of(peers[slot].record) : 0;
}

void receive(uint8_t slot, const uint8_t *data, size_t size) {
    if (slot > net::kMaxPeers || !peers[slot].used) return;
    receive_peer(peers[slot], data, size, game_time());
}

bool install() {
    const uint64_t base = uint64_t(uintptr_t(image.base));
    // (The records' message layers get their vtable from a static constructor (0x675570), which runs
    // after this install: attach() checks it when a peer connects.)
    const uint64_t vt = base + kMessageVtable;
    fn.read = reinterpret_cast<ReadMessagesFn>(field<uint64_t>(vt, 0x08));
    fn.write = reinterpret_cast<WriteMessagesFn>(field<uint64_t>(vt, 0x10));
    fn.delivered = reinterpret_cast<PacketFn>(field<uint64_t>(vt, 0x18));
    fn.lost = reinterpret_cast<PacketFn>(field<uint64_t>(vt, 0x20));
    fn.reset = reinterpret_cast<ResetFn>(base + 0x60ba10);
    if (uint64_t(uintptr_t(fn.read)) != base + 0x60a110 || uint64_t(uintptr_t(fn.write)) != base + 0x60ac70 ||
        uint64_t(uintptr_t(fn.delivered)) != base + 0x60b780 || uint64_t(uintptr_t(fn.lost)) != base + 0x60b8f0) {
        log("game transport: message layer vtable differs, not installed");
        return false;
    }
    log("game transport: packet layer ready");
    return true;
}

}  // namespace u2mp::gamenet
