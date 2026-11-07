// u2mp game transport bridge: carries the game's own net-event packets over net::Transport.
//
// Uncharted 2 PS4 1.02 keeps everything above the packet level (u2mp/docs/TRANSPORT-INTERFACE.md):
// gameplay queues events into per-peer message layers ("M", vtable 0x103fc80) of 32 connection
// records at 0x1152a18; the PS3 packet code (header, acks: 0x4652cc / 0x465138 / 0x465218 /
// 0x464cb8) and the per-frame update (0x43cdb8) are gone. This file is that code, in C++:
//   per frame (frame()):  for each peer, pack (M vtable[2]) behind a PS3-style header and send it
//                         as one unreliable transport message (tag kGamePacket);
//   on receive (receive()): header/acks (delivered / lost through M vtable[3]/[4]), then unpack and
//                         apply (M vtable[1]).
// Peers are attached by the session layer (session.cpp), which owns players and slots.
#pragma once
#include <cstdint>
#include <string>

#include "net.h"

namespace u2mp::gamenet {

// The transport the bridge uses (owned by the session layer); null = idle.
void set_transport(net::Transport *transport);

// A remote peer reached through transport slot `slot`: takes a free connection record, keyed
// `key` (non-zero, unique: the value the player tracker's +0x23c holds), resets its message layer.
// `direct`: we exchange packets with it (false: reached through the host). Returns the record
// index (0..31) or -1.
int attach(uint8_t slot, uint32_t key, bool direct);
void detach(uint8_t slot);
// The guest address of a record's message layer (for the session layer), or 0.
uint64_t message_layer(uint8_t slot);

// Checks this build's message layer and caches its functions. Returns false if it differs.
bool install();
// Once per frame (session layer, after receiving): pack and send each direct peer's queued events.
void frame(double now);
// The game clock in seconds (frame counter / 300), the time base of the message layer.
double clock();
// First byte of every transport message: which layer it belongs to.
constexpr uint8_t kGamePacket = 0, kSessionMessage = 1;

// One packet arrived from `slot` (called by the session layer's transport dispatch).
void receive(uint8_t slot, const uint8_t *data, size_t size);

}  // namespace u2mp::gamenet
