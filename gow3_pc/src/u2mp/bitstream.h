// u2mp: the game's BitStream (u2mp/docs/TRANSPORT-INTERFACE.md section 4), shared by the packet
// bridge (gamenet.cpp) and the ported net-event serializers (events.cpp).
//
// Layout used by every inlined PS4 copy and by the message layer (0x60a110 / 0x60ac70):
//   { u32 *data; u32 numBits (cursor) @8; u32 maxBits @0xc; u32 pad }
// Bit n lives in data[n >> 5] under mask 0x80000000 >> (n & 31) (MSB first in native u32 words).
#pragma once
#include <cstddef>
#include <cstdint>

namespace u2mp {

struct BitStream {
    uint32_t *data;
    uint32_t bits;
    uint32_t max_bits;
    uint32_t pad;
};
static_assert(offsetof(BitStream, bits) == 0x08 && offsetof(BitStream, max_bits) == 0x0c, "game BitStream layout");

// Writes the low `count` (0..32) bits of `value` at the cursor, MSB first. Like the game's inlined
// WriteBits, the bits after the field in its last word are cleared. Never touches memory past
// max_bits: a field that does not fit is not written, the cursor still advances (where the game
// would assert) and false is returned.
inline bool write_bits(BitStream &bs, uint32_t value, unsigned count) {
    const uint32_t start = bs.bits;
    bs.bits = start + count;
    if (count == 0) return true;
    if (start > bs.max_bits || count > bs.max_bits - start) return false;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t n = start + i, mask = 0x80000000u >> (n & 31);
        if ((value >> (count - 1 - i)) & 1) bs.data[n >> 5] |= mask;
        else bs.data[n >> 5] &= ~mask;
    }
    const uint32_t end = start + count;
    if (end & 31) bs.data[end >> 5] &= ~(0xffffffffu >> (end & 31));
    return true;
}

// Reads `count` (0..32) bits at the cursor. False (cursor and value unchanged) when fewer than
// `count` bits remain before max_bits.
inline bool read_bits(BitStream &bs, uint32_t &value, unsigned count) {
    if (bs.bits > bs.max_bits || count > bs.max_bits - bs.bits) return false;
    uint32_t v = 0;
    for (unsigned i = 0; i < count; ++i) {
        const uint32_t n = bs.bits++;
        v = (v << 1) | ((bs.data[n >> 5] >> (31 - (n & 31))) & 1);
    }
    value = v;
    return true;
}

}  // namespace u2mp
