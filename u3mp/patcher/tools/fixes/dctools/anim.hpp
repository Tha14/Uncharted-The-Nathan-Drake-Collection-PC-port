// dctools/anim.hpp: PS3 -> PS4 Uncharted 3 animation packages and ANIM items (the actor toolset's anim.py in C++).
//
// Clips: PS3 ICE-anim command-list clips (magic 0x43440107, big endian) -> the OrbisAnim port (0x41430104, little
// endian). Header {magic, hash, size, u16 groups, u16, keyframe-bitmask offset, group table 0x30, 0, f32 fps, u16 flags,
// u16 frames, f32 x3}; group 16 B {block offset, u16 size, u8 joints, u8 floats, frame data offset, u16 stride, u16}.
// PS3 blocks start with their masks (16 B per non-zero joint/float count); PS4 collects the masks after the group table.
// Commands: u16 (argc << 8 | op) + argc u16 args; a location is offset | base (base 0x000c on PS3 -> 0x0003 on PS4).
// Each opcode's const-data region is re-emitted with the PS4 length rule and element swap (the PS3 block read as a
// buffer that is 0xff past its end); frame data is copied verbatim. A streamed clip keeps only its swapped header.
// ANIM item {6, u32, clip*, channels*, 0, skeleton, events*}; ANIM_RAWCHANNELS {n, sid*, channel*[n]}, channel {u16
// frames, u16 type, data*}; events {table sid, n, entries {sid, f32, n, key/value*}}.
// Streamed animations: the chunks 1..N come from the PS3 igc-stream-*.stm files (blocks of a 16-byte tag and a
// one-page PS3 pak with one ANIM '<anim>-chunk-N'); the STREAMING_BUFFER gets the PS4 chunk table.
#pragma once
#include "ndpak.hpp"
#include "ps4writer.hpp"

namespace u3::anim {

struct Anim {
    std::string name;
    uint32_t w0 = 0, w1 = 0, w4 = 0, skel = 0;
    std::optional<Bytes> clip;
    struct Chan {
        uint32_t sid, nfr, typ;
        std::optional<Bytes> data;
    };
    std::optional<std::vector<Chan>> chans;
    struct Event {
        uint32_t sid;
        uint32_t fbits;   // f32 bits
        uint32_t cnt;
        std::optional<std::vector<std::pair<uint32_t, uint32_t>>> kv;
    };
    struct Events {
        uint32_t tsid, cnt;
        std::vector<Event> ents;
    };
    std::optional<Events> events;
};

bool is_streamed(const Bytes &c3);
Bytes convert_clip(const Bytes &c3);
Anim read_ps3_anim(const Pak &p, const PakItem &it);
std::shared_ptr<Blob> build_ps4_anim(const Anim &a);
// a PS3 animation pak -> the PS4 pak; stm_dirs: folders with the PS3 .stm stream files
Bytes convert_pak(const fs::path &src, const std::vector<fs::path> &stm_dirs);

}  // namespace u3::anim
