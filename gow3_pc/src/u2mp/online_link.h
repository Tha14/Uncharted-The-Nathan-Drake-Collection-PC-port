// u2mp online link: the client of u2_online.exe, the separate program that holds Uncharted 2's online link
// (rooms, signaling, peer datagrams). online.cpp calls these functions; they have the signatures of
// u2_online_abi.h and carry each call over the program's pipes (u2_online_rpc.h). The program is not part of
// this runtime: it runs in its own process and only this protocol connects the two.
//
// Datagrams and events are pushed by the program as they arrive and queued here, so recv and poll never wait;
// send does not wait either. Room, friend and score calls wait for the program's answer (a pipe round trip).
// If the program ends, every call fails (0), state reports U2O_FAILED and poll reports U2O_EV_LINK_LOST once.
#pragma once
#include <cstdint>
#include <string>

#include "u2_online_abi.h"

namespace u2mp::online_link {

// Starts the program at `exe` (once per process) and checks its ABI. False: `why` says what failed.
bool start(const wchar_t *exe, std::string &why);
// The program runs and answers (false after it ended).
bool alive();

int abi();
int open(const char *relay, const char *com_id);
int state(char *text, int cap);
int poll(U2oEvent *e);
void close();
uint64_t find_world();
uint64_t create_room(uint32_t world, const U2oCreate *c);
uint64_t join_room(uint64_t room_id, const uint8_t *member_bin, uint32_t member_bin_len, uint8_t team);
uint64_t leave_room(uint64_t room_id);
uint64_t search_rooms(uint32_t world, const U2oSearch *s);
uint64_t set_room_external(uint64_t room_id, const U2oAttr *attrs, uint32_t count);
uint64_t set_room_internal(uint64_t room_id, uint32_t flag_filter, uint32_t flag_attr, const U2oAttr *attrs,
                           uint32_t count);
uint64_t set_member_internal(uint64_t room_id, uint16_t member_id, uint8_t team, const uint8_t *bin, uint32_t len);
uint64_t members_external(uint64_t room_id);
uint64_t peer_address(const char *npid);
int peer_connect(const char *npid, const uint8_t ip[4], uint16_t port);
void peer_disconnect(const char *npid);
int peer_state(const char *npid, uint32_t *rtt_us);
int send(const char *npid, uint16_t dst_vport, uint16_t src_vport, const uint8_t *data, uint32_t len);
int recv(uint16_t vport, uint8_t *data, uint32_t cap, char npid[20]);
int identity(char npid[20], char name[52]);
int friends(U2oFriend *out, int cap);
uint64_t set_presence(const char *title, const char *status, const char *comment, const uint8_t *data, uint32_t len);
uint64_t send_message(const char *const *npids, uint32_t count, const U2oMessage *m);
uint64_t set_owner_rank(uint64_t room_id, const uint16_t *member_ids, uint32_t count);
int friend_requests(int in, char *out, int cap);
uint64_t add_friend(const char *npid);
uint64_t remove_friend(const char *npid);
uint64_t record_score(uint32_t board, int64_t score, const char *comment, const uint8_t *info, uint32_t info_len);
uint64_t get_scores(uint32_t board, int mode, uint32_t start, uint32_t count);

}  // namespace u2mp::online_link
