// dctools/skel.hpp: JOINT_HIERARCHY, PS3 (ICE "JH\x01\x0a" skeleton, 32-bit BE item) -> PS4 (OrbisAnim "AH\x01\x06",
// 64-bit LE); the actor toolset's skel.py in C++.
//
// PS3 item {u32 8, u32 nJoints, blob*, B*, names*, u32 0, D*, u32 skelSid} (0x20) -> PS4 {u32 8, u32 nJoints, blob*,
// B*, names*, u64 0, D*, u32 skelSid, aa x3} (0x40). names {u32 sid, char*} -> {u32 sid, aa, ptr}, strings 16-aligned;
// D {3, zeros} -> a fixed PS4 block (0xb0); B -> zeros + S* at +0x50, S {u16* per group, float**, u8**}; the blob:
// header, pose, inverse bind, group tables, command list (context location 0xc -> 3), joint sids, parents.
// PS4 item order: header, names, strings, D, B, the S tree, blob.
#pragma once
#include "ndpak.hpp"
#include "ps4writer.hpp"

namespace u3::skel {

struct JH {
    uint32_t ver = 0, nj = 0, sid = 0;
    Bytes blob;
    std::vector<std::pair<uint32_t, std::string>> names;
    bool d = false, b = false;
    bool has_s = false;
    Bytes s_cnt;
    std::vector<std::pair<Bytes, Bytes>> s_groups;   // (floats, bytes) per group
};
JH read_jh(const Pak &p, const PakItem &it);
std::shared_ptr<Blob> build_jh(const JH &m);

}  // namespace u3::skel
