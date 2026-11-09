// u2mp: the session layer's party, as other parts of the module see it (session.cpp implements it).
#pragma once
#include <cstdint>
#include <string>

namespace u2mp::session {

// The party member shown in roster row `row` (the order of the PARTY_PLAYERn / PLAYERn rows).
bool member_at(int row, uint32_t &net_id, std::string &name);
// The member with this net id (host 1, a client in transport slot s has s + 1).
bool member_name(uint32_t net_id, std::string &name);
// This PC's net id; 0 when it is not in a party.
uint32_t local_id();
// True when this PC leads the party (it hosts the transport).
bool is_host();
// Host only: tells the member with this net id that it was kicked. It leaves the party, and the
// host drops it (NetEventPlayerLeft, roster) when its goodbye arrives. False when not hosting,
// for the host itself, or for an unknown member.
bool kick(uint32_t net_id);
// This PC's player name (UNCHARTED_U2MP_NAME, else "Drake").
std::string local_name();
int rank();   // this PC's rank badge index (profile panel)
// Players in the party, this PC included; 0 when not in one.
int member_count();

}  // namespace u2mp::session
