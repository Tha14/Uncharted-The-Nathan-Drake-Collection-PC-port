// u2_online_abi.h - the C interface of u2_online.dll, Uncharted 2's online link (rooms, signaling, peer
// datagrams). The DLL ships with the launcher's online extension; this runtime loads it with LoadLibrary
// (online.cpp) and uses only what is declared here. Without the DLL, Uncharted 2 has LAN play only.
//
// Everything is asynchronous: a u2o_* request returns a request id (0 = refused: invalid parameters or no
// link) and its result arrives later as an event from u2o_poll with the same id. Events also report the
// room service's own news (members joining and leaving, room and member data changes, peer addresses).
// Strings are UTF-8, NUL-ended. Event data stays valid until the next u2o_poll.
//
// Keep this file identical to the copy next to the DLL's source.
#pragma once
#include <cstdint>

extern "C" {

enum : int32_t {
    U2O_ABI = 2,              // 2: friends, presence, messages (invites), identity
    U2O_MAX_MEMBERS = 16,
    U2O_MAX_ROOMS = 20,
};

// Link state (u2o_state)
enum : int32_t { U2O_OFF = 0, U2O_CONNECTING = 1, U2O_READY = 2, U2O_FAILED = -1 };

// Event kinds (U2oEvent.kind)
enum : int32_t {
    U2O_EV_LINK_LOST = 1,       // text: the reason; every room is gone
    U2O_EV_WORLD = 2,           // u2o_find_world: error 0 -> server, world
    U2O_EV_ROOM_CREATED = 3,    // u2o_create_room: room
    U2O_EV_ROOM_JOINED = 4,     // u2o_join_room: room (+ peer addresses in its members)
    U2O_EV_ROOM_LEFT = 5,       // u2o_leave_room: room_id
    U2O_EV_SEARCH = 6,          // u2o_search_rooms: rooms[count], total
    U2O_EV_DONE = 7,            // u2o_set_*: error only
    U2O_EV_MEMBER_JOINED = 8,   // member (with the peer address when the room uses signaling)
    U2O_EV_MEMBER_LEFT = 9,     // member, cause (1 left, 5 disappeared); owner = the room's owner after it
    U2O_EV_ROOM_UPDATED = 10,   // room (the new internal data)
    U2O_EV_MEMBER_UPDATED = 11, // member
    U2O_EV_PEER_ADDRESS = 12,   // u2o_peer_address: npid, ip, port (error 26 = not online)
    U2O_EV_PEER_STATE = 13,     // npid, state (U2O_PEER_*), rtt_us: signaling with that player changed
    U2O_EV_MEMBERS_EXT = 14,    // u2o_members_external: members[count] (npid, name, owner flag only)
    U2O_EV_PUBLIC_ADDRESS = 15, // ip, port: how the room service sees our datagram socket (first time / changes)
    // ABI 2
    U2O_EV_FRIEND = 16,         // npid, state (U2O_FRIEND_*): the friend list changed; u2o_friends has the new list
    U2O_EV_MESSAGE = 17,        // message: a player sent us a message (an invite: main_type U2O_MSG_INVITE)
};

// U2O_EV_FRIEND states
enum : int32_t { U2O_FRIEND_LIST = 0, U2O_FRIEND_ADDED = 1, U2O_FRIEND_REMOVED = 2, U2O_FRIEND_ONLINE = 3,
                 U2O_FRIEND_OFFLINE = 4, U2O_FRIEND_PRESENCE = 5, U2O_FRIEND_REQUEST = 6 };
// message main types (sceNpBasic)
enum : int32_t { U2O_MSG_DATA = 0, U2O_MSG_GENERAL = 1, U2O_MSG_ADD_FRIEND = 2, U2O_MSG_INVITE = 3 };

enum : int32_t { U2O_PEER_INACTIVE = 0, U2O_PEER_PENDING = 1, U2O_PEER_ACTIVE = 2, U2O_PEER_DEAD = 3 };

struct U2oMember {
    uint16_t id;          // member id (slot << 4 | counter)
    uint8_t team;
    uint8_t is_owner;
    uint32_t flags;       // 0x80000000 owner
    uint64_t join_date;
    char npid[20];
    char name[52];
    uint8_t bin[128];     // member internal bin attr 0x59
    uint32_t bin_len;
    uint8_t ip[4];        // peer address for signaling (0.0.0.0 when the room service gave none)
    uint16_t port;        // host order
    uint16_t pad;
};

struct U2oRoom {
    uint64_t room_id;
    uint32_t world;
    uint16_t server;
    uint16_t owner_id;
    uint16_t my_id;       // our member id in this room (0 = not a member)
    uint16_t max_slot;
    uint32_t flag_attr;
    uint32_t member_count;
    U2oMember members[U2O_MAX_MEMBERS];
    uint8_t bin_internal[2][256];   // room internal bin attrs 0x57, 0x58
    uint32_t bin_internal_len[2];
};

struct U2oRoomExt {        // a search result
    uint64_t room_id;
    uint32_t world;
    uint16_t server;
    uint16_t max_slot;
    uint16_t cur_members;
    uint16_t open_public_slots;
    uint32_t flag_attr;
    uint32_t int_attr[8];           // searchable int attrs 0x4C..0x53
    uint8_t search_bin[64];         // searchable bin attr 0x54
    uint32_t search_bin_len;
    uint8_t bin_external[2][256];   // bin attrs 0x55, 0x56
    uint32_t bin_external_len[2];
    char owner_npid[20];
    char owner_name[52];
};

struct U2oAttr {           // one room attribute in a request
    uint16_t id;           // 0x4C..0x53 int (num), 0x54..0x58 bin (data, len), 0x59 member bin
    uint16_t len;
    uint32_t num;
    const uint8_t *data;
};

struct U2oCreate {
    uint32_t max_slot;     // 1..16
    uint32_t flag_attr;
    const U2oAttr *attrs;  // any of 0x4C..0x58
    uint32_t attr_count;
    const uint8_t *member_bin;   // our member bin attr 0x59 (optional, <= 128 bytes)
    uint32_t member_bin_len;
    uint8_t team;
    uint8_t signaling;     // 0 none, 1 mesh, 2 star (the room service then hands out peer addresses)
    uint16_t pad;
};

struct U2oIntFilter { uint8_t op; uint8_t pad; uint16_t id; uint32_t num; };   // op 1 EQ 2 NE 3 LT 4 LE 5 GT 6 GE

struct U2oSearch {
    uint32_t flag_filter;
    uint32_t flag_attr;
    U2oIntFilter ints[8];
    uint32_t int_count;
    uint8_t bin_op;        // 0 none, 1 EQ, 2 NE (bin attr 0x54)
    uint8_t pad[3];
    const uint8_t *bin_data;
    uint32_t bin_len;
    uint32_t start;        // 1-based
    uint32_t max;          // 1..20
};

struct U2oFriend {
    char npid[20];
    uint8_t online;
    uint8_t same_game;     // its presence is this game's communication id
    uint8_t pad[2];
    char title[128];       // presence: game title
    char status[192];      //           status line
    char comment[64];
    uint8_t data[128];     //           game data (this game: what it is doing, which room)
    uint32_t data_len;
};

struct U2oMessage {
    char npid[20];         // the sender (received) - ignored when sending
    uint16_t main_type;    // U2O_MSG_*
    uint16_t sub_type;
    uint32_t features;
    uint64_t msg_id;
    char subject[128];
    char body[512];
    uint8_t data[512];
    uint32_t data_len;
};

struct U2oEvent {
    int32_t kind;
    int32_t error;         // 0 = success, else the room service's error number (14 no such room, 16 full, ...)
    uint64_t request;      // the request id, 0 for news
    uint64_t room_id;
    const U2oRoom *room;
    const U2oRoomExt *rooms;
    const U2oMember *member;   // or members[count]
    uint32_t count;
    uint32_t total;
    uint32_t cause;
    uint32_t state;
    uint32_t rtt_us;
    uint16_t server;
    uint16_t port;
    uint32_t world;
    uint8_t ip[4];
    char npid[20];
    const char *text;
    const U2oMessage *message;   // ABI 2: U2O_EV_MESSAGE
};

// ---- calls ----
typedef int(__cdecl *u2o_abi_fn)(void);
typedef int(__cdecl *u2o_open_fn)(const char *relay, const char *com_id);   // "port:key", "NPWRxxxxx_yy"
typedef int(__cdecl *u2o_state_fn)(char *text, int cap);    // name when ready, reason when failed
typedef int(__cdecl *u2o_poll_fn)(U2oEvent *e);
typedef void(__cdecl *u2o_close_fn)(void);
typedef uint64_t(__cdecl *u2o_find_world_fn)(void);
typedef uint64_t(__cdecl *u2o_create_room_fn)(uint32_t world, const U2oCreate *c);
typedef uint64_t(__cdecl *u2o_join_room_fn)(uint64_t room_id, const uint8_t *member_bin, uint32_t member_bin_len, uint8_t team);
typedef uint64_t(__cdecl *u2o_leave_room_fn)(uint64_t room_id);
typedef uint64_t(__cdecl *u2o_search_rooms_fn)(uint32_t world, const U2oSearch *s);
typedef uint64_t(__cdecl *u2o_set_room_external_fn)(uint64_t room_id, const U2oAttr *attrs, uint32_t count);
typedef uint64_t(__cdecl *u2o_set_room_internal_fn)(uint64_t room_id, uint32_t flag_filter, uint32_t flag_attr,
                                                    const U2oAttr *attrs, uint32_t count);
typedef uint64_t(__cdecl *u2o_set_member_internal_fn)(uint64_t room_id, uint16_t member_id, uint8_t team,
                                                      const uint8_t *bin, uint32_t len);
typedef uint64_t(__cdecl *u2o_members_external_fn)(uint64_t room_id);
typedef uint64_t(__cdecl *u2o_peer_address_fn)(const char *npid);
// Peer datagrams through the shared UDP socket (RPCS3-compatible framing): connect starts signaling with a
// player (address from a room event or u2o_peer_address), send/recv carry the game's packets.
typedef int(__cdecl *u2o_peer_connect_fn)(const char *npid, const uint8_t ip[4], uint16_t port);
typedef void(__cdecl *u2o_peer_disconnect_fn)(const char *npid);
typedef int(__cdecl *u2o_peer_state_fn)(const char *npid, uint32_t *rtt_us);
typedef int(__cdecl *u2o_send_fn)(const char *npid, uint16_t dst_vport, uint16_t src_vport, const uint8_t *data, uint32_t len);
// recv: one datagram for src... returns its length (0 none, -1 buffer too small); npid of the sender
typedef int(__cdecl *u2o_recv_fn)(uint16_t vport, uint8_t *data, uint32_t cap, char npid[20]);

// ---- ABI 2 ----
// Our NpId and online name (from the launcher's session); 1 when known.
typedef int(__cdecl *u2o_identity_fn)(char npid[20], char name[52]);
// The friend list: fills up to cap entries (online ones first), returns how many friends there are.
typedef int(__cdecl *u2o_friends_fn)(U2oFriend *out, int cap);
// Our presence for friends (this game's communication id): title, status line, comment, game data (<= 128).
typedef uint64_t(__cdecl *u2o_set_presence_fn)(const char *title, const char *status, const char *comment,
                                               const uint8_t *data, uint32_t len);
// A message to friends (npids[count], 1..16); an invite needs main_type U2O_MSG_INVITE. Result: U2O_EV_DONE.
typedef uint64_t(__cdecl *u2o_send_message_fn)(const char *const *npids, uint32_t count, const U2oMessage *m);
// The owner succession of a room we own: the first member id still present becomes the owner when we leave.
typedef uint64_t(__cdecl *u2o_set_owner_rank_fn)(uint64_t room_id, const uint16_t *member_ids, uint32_t count);
// Friend requests: in = 1 the ones to us, 0 ours; names NUL-separated into out; returns how many there are.
typedef int(__cdecl *u2o_friend_requests_fn)(int in, char *out, int cap);
// Accept a request / send one (add), decline / cancel / unfriend (remove). Result: U2O_EV_DONE.
typedef uint64_t(__cdecl *u2o_add_friend_fn)(const char *npid);
typedef uint64_t(__cdecl *u2o_remove_friend_fn)(const char *npid);

}  // extern "C"
