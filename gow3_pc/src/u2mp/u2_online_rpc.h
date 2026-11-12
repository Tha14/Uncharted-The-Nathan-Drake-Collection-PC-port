// u2_online_rpc.h - the pipe protocol between this runtime and u2_online.exe, the separate program that holds
// Uncharted 2's online link. The runtime starts the program with two anonymous pipes as its stdin / stdout
// (online_link.cpp) and calls the functions of u2_online_abi.h through them; the program runs them and sends
// the results back. Nothing of the online service itself is in this protocol: it only carries the ABI calls.
//
// Frames, both ways: u32 size (bytes after it), u8 op, payload; little endian, structs of u2_online_abi.h as
// their raw bytes (both ends are x64 builds of the same header). Pointers inside structs are sent as zero and
// their data follows the struct.
//   runtime -> program: one frame per call (op = Op::*). Calls with a result get exactly one R_REPLY, in order;
//                       send and peer_disconnect have none (no wait for game packets).
//   program -> runtime: R_REPLY {u64 result, out data}, P_HELLO {i32 abi} once at the start, and pushes as soon
//                       as the link has them: P_DATAGRAM {u16 vport, char npid[20], data...} and P_EVENT
//                       (an encoded U2oEvent, see put_event). So u2o_recv and u2o_poll never wait for the pipe.
// The runtime closing its end (or exiting) ends the program.
//
// Keep this file identical to the copy next to the program's source.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "u2_online_abi.h"

namespace u2o_rpc {

enum : uint32_t { kMaxFrame = U2O_MAX_BLOB + (1u << 20) };   // a message with the largest attachment fits

enum Op : uint8_t {
    ABI = 1, OPEN, STATE, CLOSE, FIND_WORLD, CREATE_ROOM, JOIN_ROOM, LEAVE_ROOM, SEARCH_ROOMS, SET_ROOM_EXTERNAL,
    SET_ROOM_INTERNAL, SET_MEMBER_INTERNAL, MEMBERS_EXTERNAL, PEER_ADDRESS, PEER_CONNECT, PEER_DISCONNECT, PEER_STATE,
    SEND, IDENTITY, FRIENDS, SET_PRESENCE, SEND_MESSAGE, SET_OWNER_RANK, FRIEND_REQUESTS, ADD_FRIEND, REMOVE_FRIEND,
    RECORD_SCORE, GET_SCORES,
    R_REPLY = 0x80, P_HELLO = 0x81, P_DATAGRAM = 0x82, P_EVENT = 0x83,
};

constexpr uint32_t kNull = 0xffffffffu;   // length of a NULL pointer (string or bytes)

// ---- writing ----
struct Out {
    std::vector<uint8_t> b;
    explicit Out(uint8_t op) { b.resize(5); b[4] = op; }
    template <typename T> Out &put(const T &v) {
        const size_t at = b.size();
        b.resize(at + sizeof v);
        std::memcpy(b.data() + at, &v, sizeof v);
        return *this;
    }
    Out &raw(const void *p, size_t n) {
        if (n) b.insert(b.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
        return *this;
    }
    Out &bytes(const void *p, uint32_t n) {   // NULL -> kNull
        if (!p) return put(kNull);
        put(n);
        return raw(p, n);
    }
    Out &str(const char *s) { return s ? bytes(s, uint32_t(std::strlen(s))) : put(kNull); }
    // the finished frame (fills in the size)
    const std::vector<uint8_t> &frame() {
        const uint32_t n = uint32_t(b.size() - 4);
        std::memcpy(b.data(), &n, 4);
        return b;
    }
};

// ---- reading (payload of one frame; every read is bounds-checked, ok() says whether all fit) ----
struct In {
    const uint8_t *p, *end;
    bool good = true;
    In(const uint8_t *data, size_t n) : p(data), end(data + n) {}
    bool ok() const { return good; }
    template <typename T> T get() {
        T v{};
        if (size_t(end - p) < sizeof v) { good = false; p = end; return v; }
        std::memcpy(&v, p, sizeof v);
        p += sizeof v;
        return v;
    }
    const uint8_t *raw(size_t n) {
        if (size_t(end - p) < n) { good = false; p = end; return nullptr; }
        const uint8_t *r = p;
        p += n;
        return r;
    }
    // bytes: false = NULL; data / n are set otherwise
    bool bytes(const uint8_t *&data, uint32_t &n) {
        n = get<uint32_t>();
        data = nullptr;
        if (!good || n == kNull) { n = 0; return false; }
        data = raw(n);
        if (!data) { n = 0; return false; }
        return true;
    }
    // a string kept in `keep`; returns NULL for a NULL string
    const char *str(std::string &keep) {
        const uint8_t *d;
        uint32_t n;
        if (!bytes(d, n)) return nullptr;
        keep.assign(reinterpret_cast<const char *>(d), n);
        return keep.c_str();
    }
    size_t left() const { return size_t(end - p); }
};

// attribute lists (U2oAttr[count] with their data)
inline void put_attrs(Out &o, const U2oAttr *a, uint32_t count) {
    if (!a) count = 0;
    o.put(count);
    for (uint32_t i = 0; i < count; ++i) {
        U2oAttr c = a[i];
        c.data = nullptr;
        o.put(c).bytes(a[i].data, a[i].len);
    }
}
// into attrs; their data points into the frame
inline bool get_attrs(In &in, std::vector<U2oAttr> &attrs) {
    const uint32_t n = in.get<uint32_t>();
    if (!in.ok() || n > 4096) return false;
    attrs.resize(n);
    for (U2oAttr &a : attrs) {
        a = in.get<U2oAttr>();
        uint32_t len;
        in.bytes(a.data, len);
        if (!in.ok() || (a.data && len != a.len)) return false;
    }
    return in.ok();
}

// ---- events ----
// counts of the arrays an event points to: room (0/1), rooms, members, scores
struct EventCounts {
    uint32_t room, rooms, members, scores;
};

// U2oEvent (pointers zero) + EventCounts + room, rooms[], members[], message? (+ its blob), scores[], text
inline void put_event(Out &o, const U2oEvent &e, const EventCounts &c) {
    U2oEvent h = e;
    h.room = nullptr; h.rooms = nullptr; h.member = nullptr; h.text = nullptr; h.message = nullptr; h.scores = nullptr;
    EventCounts n = c;
    if (!e.room) n.room = 0;
    if (!e.rooms) n.rooms = 0;
    if (!e.member) n.members = 0;
    if (!e.scores) n.scores = 0;
    o.put(h).put(n);
    if (n.room) o.raw(e.room, sizeof(U2oRoom) * n.room);
    if (n.rooms) o.raw(e.rooms, sizeof(U2oRoomExt) * n.rooms);
    if (n.members) o.raw(e.member, sizeof(U2oMember) * n.members);
    o.put(uint8_t(e.message ? 1 : 0));
    if (e.message) {
        U2oMessage m = *e.message;
        m.blob = nullptr;
        o.put(m).bytes(e.message->blob, e.message->blob ? e.message->blob_len : 0);
    }
    if (n.scores) o.raw(e.scores, sizeof(U2oScore) * n.scores);
    o.str(e.text);
}

// An event received, with its own storage (valid until the next decode into the same object).
struct Event {
    U2oEvent e{};
    std::vector<U2oRoom> room;
    std::vector<U2oRoomExt> rooms;
    std::vector<U2oMember> members;
    std::vector<U2oScore> scores;
    std::vector<U2oMessage> message;
    std::vector<uint8_t> blob;
    std::string text;
    bool has_text = false;

    template <typename T> static bool take(In &in, std::vector<T> &v, uint32_t n, uint32_t max) {
        if (n > max) return false;
        v.resize(n);
        if (!n) return true;
        const uint8_t *d = in.raw(sizeof(T) * n);
        if (!d) return false;
        std::memcpy(static_cast<void *>(v.data()), d, sizeof(T) * n);
        return true;
    }
    bool decode(In &in) {
        e = in.get<U2oEvent>();
        const EventCounts n = in.get<EventCounts>();
        if (!in.ok()) return false;
        if (!take(in, room, n.room, 1) || !take(in, rooms, n.rooms, 1024) || !take(in, members, n.members, 1024))
            return false;
        const uint8_t has_msg = in.get<uint8_t>();
        message.clear();
        blob.clear();
        if (has_msg) {
            message.push_back(in.get<U2oMessage>());
            const uint8_t *d;
            uint32_t len;
            if (in.bytes(d, len)) blob.assign(d, d + len);
            message[0].blob = nullptr;
            if (!in.ok()) return false;
        }
        if (!take(in, scores, n.scores, 4096)) return false;
        has_text = in.str(text) != nullptr;
        if (!in.ok()) return false;
        fix();
        return true;
    }
    void fix() {   // point the C view at the storage
        e.room = room.empty() ? nullptr : room.data();
        e.rooms = rooms.empty() ? nullptr : rooms.data();
        e.member = members.empty() ? nullptr : members.data();
        e.scores = scores.empty() ? nullptr : scores.data();
        if (!message.empty()) {
            message[0].blob = blob.empty() ? nullptr : blob.data();
            if (!blob.empty()) message[0].blob_len = uint32_t(blob.size());
        }
        e.message = message.empty() ? nullptr : message.data();
        e.text = has_text ? text.c_str() : nullptr;
    }
};

}  // namespace u2o_rpc
