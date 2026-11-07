// u2mp: net-event serializers the PS4 1.02 build left as bare `ret`, ported from PS3 1.10.
//
// Every net event (u2mp/docs/TRANSPORT-INTERFACE.md section 1.1) has a vtable with slot 2
// Read(BitStream*) and slot 3 Write(BitStream*); for 14 types one or both are `ret` stubs on PS4,
// so those events cross the wire empty. events.cpp ports the PS3 bodies to the PS4 payload layout
// and writes the ports into the vtable slots (the stub stays in place for nothing else).
#pragma once
#include <cstddef>
#include <cstdint>

#include "bitstream.h"
#include "u2mp_internal.h"

namespace u2mp::events {

using SerializeFn = void(U2MP_GUEST *)(void *event, BitStream *bs);
using ApplyFn = void(U2MP_GUEST *)(void *event, void *connection);

struct Port {
    int type;
    const char *name;
    uint64_t vtable;        // PS4 image offset of the event's vtable
    uint32_t size;          // allocation size in CreateNetEventByType 0x6180f0
    uint64_t read_stub;     // PS4 slot 2 stub replaced by `read` (0: slot left alone)
    uint64_t write_stub;    // PS4 slot 3 stub replaced by `write`
    uint64_t apply_stub;    // PS4 slot 4 stub replaced by `apply`
    SerializeFn read;
    SerializeFn write;
    ApplyFn apply;
};
extern const Port kPorts[];
extern const size_t kPortCount;

// What the serializers read from the running game. install_events points these at the game;
// the host test points them at fakes.
struct Game {
    const int32_t *frame_clock = nullptr;   // 300 Hz frame counter, image 0x1116180 + 0x204
    int (*game_mode)() = nullptr;           // the current game mode (12 = TerritoryOwn sends a float)
    void *(*lookup_process)(uint32_t id) = nullptr;   // LookupProcessByUniqueId
};
extern Game game;

void install_events(int &installed);

}  // namespace u2mp::events

namespace u2mp {
using events::install_events;
}
