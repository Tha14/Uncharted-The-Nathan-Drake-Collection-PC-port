# PORT-ONLINE: Uncharted 2 online play (rooms + peer-to-peer)

Online play for the PC port, built on the PS3 map in `PORT-MATCHMAKING.md` (section 6 is the plan this
follows). The player signs in to RPCN in the launcher; the game reuses that session through a local
relay. Rooms (party, custom game, join list) go through the room service of `u2_online.exe`, a separate
program the runtime starts and calls over pipes (`online_link.cpp`, protocol `u2_online_rpc.h`); match
traffic goes peer to peer through the program's NAT-punched UDP socket (each datagram crosses the pipe
once each way; the program pushes received ones at once, about 25 us there and back on one PC).

Code: `gow3_pc/src/u2mp/online.cpp` (+ `online.h`, `online_link.cpp`), with small hooks in `session.cpp`, `net.h/.cpp`,
`menus.cpp`, `remaining.cpp`, `frontend2.cpp`, `progression.cpp`. The only interface to the room service
is `u2_online_abi.h` (carried by `u2_online_rpc.h`). Nothing about the service's wire protocol is in this
repository, and no code of the service runs in the runtime's process: if the program is missing, fails to start
or ends, Uncharted 2 plays over LAN (or reports the link lost).

Status: **built, not run on two PCs yet**. Friends / presence / invites / matchmaking / migration: section 5a. See "Untested / open" at the end.

---

## 1. Switches (environment)

| Variable | Set by | Meaning |
|---|---|---|
| `UNCHARTED_U2MP_ONLINE_EXE` | launcher, when the online extension is installed | full path of `u2_online.exe` |
| `UNCHARTED_U2MP_RELAY` | launcher, when signed in | the local relay handed to `u2o_open` (never logged) |
| `UNCHARTED_U2MP_NAME` | launcher | the online name (player name, room name, roster) |
| `UNCHARTED_U2MP_NPID` | launcher | this player's NpId (skips ourselves in member lists, logs) |
| `UNCHARTED_U2MP_JOIN` | manual only | LAN join by address; when set, online stays off |

Online is on only when the DLL path and the relay are set, the DLL loads, exports every `u2o_*` call
and reports `U2O_ABI`. Otherwise every `online::` function is a no-op and the LAN path runs exactly as
before (one `online: off (...)` line in boot.log says why).

## 2. Model

The LAN session layer (`session.cpp`) stays the core: a star transport (`net::Transport`) with the
party leader / match host in the middle; roster, follow, matchmaking vote (`frontend3.cpp`), game
events (`gamenet.cpp`) and voice ride on it. Online changes only two things:

1. **The transport runs over the online link.** `net::Link` (new, `net.h`) replaces the transport's own
   UDP socket: `online.cpp`'s link sends with `u2o_send(npid, vport 1, vport 1, ...)` and receives with
   `u2o_recv(1, ...)`. Peers get synthetic addresses (index of the NpId in a table, port 1), so the
   transport code (hello / welcome / acks / reliable channel / timeouts) is unchanged.
2. **Rooms decide who hosts.** The owner of the room you are in hosts the transport; the other
   members are its clients. Signaling (`u2o_peer_connect`) is started with every member of every room
   we are in (mesh, as PS3 did for every room member, `0x4d76e8` -> `0x4e9e98`).

### Rooms

| Room | Made when | maxSlot / flags | Attributes (PS3 1.3) |
|---|---|---|---|
| party | Multiplayer entered (this PC leads its own party; PS3 `0x4c48d0`) | 10, OWNER_AUTO_GRANT | 0x4C key, 0x4D party size, 0x4E/0x4F 0, 0x50 0, **0x52 = 1**, 0x53 0, bin 0x55 (8 zero bytes), bin 0x58 `"%s.%lld"`, member bin 0x59 |
| custom game (match) | Start Match in a custom game (PS3 `0x444bb0` -> `0x4d421c`) | 10 (`0x49e620` is inlined on PS4), no flags | same, **0x52 = 2** |

- 0x4C key = netInfo+0 when set (PS3: DC `6f1c3f87`[0] \| 0x90000), else the PC value `0x90102`
  (PS4 never fills netInfo+0). Only PCs with the same key see each other.
- 0x52 (searchable int 7, unused by PS3) tells the join list what a room is. PS3 hid its party rooms
  and used NP Basic invites, which the room service does not carry; here party rooms are listed.
- Lat/lon (0x4E/0x4F, bin 0x55) and region (0x50) are 0 on every PC (no heartbeat server); they only
  mattered for stranger matchmaking, which is not ported.
- Signaling type 1 (mesh): the service hands out every member's peer address.

## 3. Hooks

| PS4 address | PS3 | What | Where |
|---|---|---|---|
| `0x5eb160` BUTTON_MULTIPLAYER (session.cpp `multiplayer_button` -> `start_transport`) | NetInit `0x447a5c`, Matching2 init `0x4d0fc0` | `online::start()`: load DLL, `u2o_open(relay, "NPWR00462_00")`, transport hosted on the link | session.cpp |
| per frame (`tick`: menu page handler event 6 + TaskManagerOnline state updates) | NdiNetUpdate `0x449e88` / event queue `0x4d0bd0` | `online::frame()`: link state, `u2o_find_world` once READY, `u2o_poll` events, party room create, timers, room hide / 0x53, member data, RTT log | session.cpp |
| `0x5eb780` STARTMATCH -> `create_match_now` | `0x444bb0` + CreateJoinRoom `0x4d421c`, response `0x4d8220` | custom game: create the match room, wait (15 s max) for `U2O_EV_ROOM_CREATED`, then `match+0x99 = 1` (`session::online_match_ready`); `0x6093f0` runs 3 -> 10 -> 16 | session.cpp / online.cpp |
| kFollow session message | party follow, mailbox 0x10e, member data +0 | carries the match room id (`{kFollow, u64}`); members `u2o_join_room` it as members (transport unchanged) | session.cpp / online.cpp |
| host `on_peer` join while NET_SM 16 and `match+0x99` | JOINING 12 -> 14 -> 17 (`0x4661e0`) | a player who joined from the list gets the follow at once and enters CLIENT_LOBBY | session.cpp |
| `0x5ebea0` BUTTON_MULTIPLAYER_INVITES (remaining.cpp) | `0x440948` (invite inbox) | online: calls the intact BUTTON_MULTIPLAYER_JOIN `0x6095d0` (PAGE_MULTIPLAYER_JOIN, NET_SM 5) + `u2o_search_rooms` (SearchRoom `0x4d3c50`: 0x4C EQ key, flag filter HIDDEN\|CLOSED = 0, max 20) | remaining.cpp |
| `0x659590` join list query, cond 3 (menus.cpp) | `0x4b8ebc` + result list `0x4d5940` | rows "owner - Custom Game / Party (n/max)", title "Online Games"; empty: "Searching..." / "No open games found. Select to search again." | menus.cpp |
| `match+0xa8` / `+0xac` | `0x4d2410` search status | written on search done (1 / 0) and on failure (1 / -1: PAGE_MULTIPLAYER_JOIN `0x609640` pops, NET_SM 18) | online.cpp |
| `0x609680` SELECT_JOIN_GAME (frontend2.cpp) | `0x465808` + reserve 0x100/0x101 + JoinRoom `0x4d4578` | free-slot check from the search data (whole party must fit), `u2o_join_room` with our member blob | frontend2.cpp |
| `0x64e810` (progression.cpp) | `0x4af240` -> SetRoomMemberDataInternal `0x4d2ed4` | member bin 0x59 (48 bytes: +0 party room id when the party has 2+, +8 rank, +9 team; rest 0) via `u2o_set_member_internal`, on change (also checked every second) | progression.cpp / online.cpp |
| NET_SM watch (online.cpp `room_state`) | Hide `0x4d5338`, SetRoomDataExternal `0x4d3af8` | owned rooms hidden + closed while NET_SM is 26..30 (match), shown again after; match room 0x53 = 1 at the first SERVER_LOBBY (`0x465fe8`), 0 when the match ends (Leave::Enter `0x445e9c`) | online.cpp |
| transport `on_peer` | listener slots 2 / 3 (`0x103fa50`), EstablishConnection::Enter `0x4416e0` | unchanged LAN code: `gamenet::attach/detach`, `player_left`, `fill_net_players` at match setup | session.cpp |
| LEAVE_MULTIPLAYER `0x5eb180`, kick, host lost | LeaveRoom `0x4d8540`, Kickout `0x4d8a98`, teardown `0x4386d0` | `online::on_transport_closed()`: leave every room, disconnect peers (the link stays up) | session.cpp |

## 4. Flows

**Host a custom game.** Multiplayer -> link READY -> world -> party room created. Custom Game -> Start
Match: `host_create()` shows the wait page (NET_SM 3), the match room is created, then `match+0x99`; the
page goes 3 -> 10 -> 16. The match room's 0x53 becomes 1. Party members get `{kFollow, room}` and join
the match room as members.

**Join from the list.** Invites -> PAGE_MULTIPLAYER_JOIN, search. Select a row -> free-slot check ->
`u2o_join_room`. On `U2O_EV_ROOM_JOINED`: our old rooms are left, every member gets `u2o_peer_connect`,
and the transport reopens as a client of the room owner (`session::online_reopen`). When the transport
reaches the owner (hello / welcome over the link): the owner in its custom game lobby sends the follow
(JOINING -> CLIENT_LOBBY); a party room / an owner not in a lobby: back to PAGE_MULTIPLAYER (NET_SM 18) as
a party member. 20 s without reaching the owner, or the owner's peer DEAD: leave the room, own party again.

**Party follows its leader.** A leader with members who joins another room sends `{kOnlineMessage 7, op 1,
u64 room}` to its party, waits 0.3 s, then switches. Members join the same room and switch to its owner;
the old host closing is expected (`host_change_expected`), not a "host lost" error.

**Match.** Unchanged from LAN: `fill_net_players` at EstablishConnection / LoadLevels / WaitReady,
NetEventLoadLevel sets `+0xb54`, game events through `gamenet` over the link. The owned rooms are
hidden while NET_SM is 26..30, so nobody joins a running match (PS3 3.8).

**Leaving / kick / host gone.** Like LAN; rooms are left in `on_transport_closed`. RPCN sends no
owner-change or kick events: the new owner arrives with `U2O_EV_ROOM_UPDATED` after a member left
(logged); GrantRoomOwner (Promote) still shows the "not available" box; a kick is the session message
`kKick`, the victim leaves its rooms itself.

## 5. boot.log lines to look for

All online lines start with `u2mp: online:`. In order of a test:

| Step | Line(s) |
|---|---|
| program | `online: u2_online.exe started (ABI 2)` (or `online: off (...)` with the reason) |
| link | `online: opening the link (communication id NPWR00462_00): u2o_open N`, `online: link connecting -> ready (<name>)` |
| world | `online: looking for a world (request N)`, `online: world W on server S` |
| entry | `online: Multiplayer entered online as <name> (NpId ...)`, `online: hosting a party on the online link as <name>` |
| party room | `online: creating the party room in world W: key 0x90102, ...`, `online: created party room <id>: 1 member(s) of 10 ...` |
| public address | `online: the room service sees our peer socket at a.b.x.x:port` |
| custom host | `online: creating the custom game room ...`, `online: created custom game room <id> ...`, `online: Start Match goes on (match room <id>)`, `net state 3 -> 10`, `-> 16`, `online: custom game room <id>: state 0x53 = 1` |
| join list | `online: Invites -> join list ...`, `online: join list: searching world W for key 0x90102`, one `online: join list: room ...` per room, `online: join list: N room(s) listed` |
| join | `online: joining custom game room <id> of <owner> (...)`, `online: joined custom game room <id>: ...` + `online:   member ...` lines, `online: peer <npid> (...) in room <id>: connecting to a.b.x.x:p`, `online: the room owner is ...`, `online: transport switch: joining the room owner over the online link` |
| peers | `online: peer <npid>: pending / active, rtt N ms` (event), then every 10 s `online: peer <npid>: active, rtt N ms` |
| transport | client: `party: joined the host as net id N`, `online: transport switch done: reached the owner of room <id>, our net id N`; host: `party: peer in slot S joined`, `online: transport: net id N connected ...`, `online: slot S joined while the lobby is up: sending the follow` |
| lobby | client: `following the host into the match`, `online: the leader's match room <id>: already a member` or `online: joining the leader's match room ...` |
| member data | `online: member data 0x59 to ... room <id>: party room ..., rank R, team T (request N)`, then `online: member data 0x59 on room <id> done`; on the other PC `online: member data of <name> in room <id>: ...` |
| match | `online: NET_SM 26: match running`, `online: custom game room <id> hidden and closed (match running)`; after: `online: NET_SM 16: back from the match`, `... shown and open again (lobby)`, `... state 0x53 = 0` |
| leave | `online: the transport closed (Multiplayer left): leaving every room`, `online: leaving ... room <id> (...)`, `online: left room <id>: error 0 (ok)`; other PC: `online: <name> (<npid>) left room <id> (cause 1: left)`, `online: room <id> owner changed ...` |
| failures | `online: joining room <id> failed: error E (...)`, `online: join refused: room <id> has F free slot(s), the party needs P`, `online: the room owner was not reached in 20 s ...`, `online: link lost (...)`, `online: creating the ... room failed: error E` |

## 5a. Friends, presence, invites, matchmaking, migration (ABI 2)

ABI 2 of `u2_online.exe` (then `u2_online.dll`) adds the player's identity, the RPCN friend list (from the launcher's Login reply plus the
friend news it forwards), presence and messages. The launcher relay also replays friend presence changes to a
game that links later.

| Piece | PS3 | PC |
|---|---|---|
| Player name | NP online id (netInfo+0x288) | the RPCN online name (`u2o_identity`) replaces UNCHARTED_U2MP_NAME once the link is ready: profile panel, rosters, room names |
| LIST_FRIENDS (cond 36, `0x65bbb0`) | PSN friends, label "Friends", detail line per friend (PS3 `0x4c1fe0`) | online: the RPCN friends, online ones first, "name - status" (offline / another game / what they do in Uncharted 2); LAN: the party as before |
| Friend row (`0x5ebde0`) | PAGE_FRIENDACTIONS popup (PS3 `0x43f830`): Join Party `0ace8463`, Invite To Party `45a9c65b`, View Profile | a popup with Join Party (friend in this game, in a party / lobby / matchmaking room with room for us) and Invite To Party (friend online); no profile viewer |
| Presence | `sceNpBasicSetPresenceDetails` (NetFriends `0x48f16c` / `0x490338`), 88 bytes with the party room id | `u2o_set_presence` on every change (2 s apart at most): title "Uncharted 2: Among Thieves", status line, 32 bytes `U2MP` {state, members, max, key, kind, room id}; empty when the game closes |
| Invite To Party (`0x5ebee0`, PS4 kept only the "Creating Party..." box) | `0x440050` -> `0x43a164` sceNpBasicSendMessageGui, 16-byte attachment {version, -, u64 party room} | `u2o_send_message` main type INVITE, the same 32 bytes as the presence (the room we are in) |
| Invites inbox (BUTTON_MULTIPLAYER_INVITES, cond 48 `0x659b50`) | system inbox; count in netInfo+0x14 | the invites (10 minutes, one per sender) are the first rows of the join list ("Invite from X - Party (n/max)"); the button shows their number; a message box says when one arrives (menus only, never in a match) |
| Join Party (`0x5ec000`) | `0x43ff60`: NP message 0x10b, the friend answers 0x10c with its party room | join the room from the friend's presence directly (free slots checked; a leader takes its party along) |
| Find Match (strangers) | `0x4b0a58` states 19 -> 5/6/7 or 3 -> 16 | NET_SM 19: search kind-3 rooms of the same key and playlist (0x51), join the fullest with room for the party (the party follows); none: create one and gather (16): vote when 10 players, or after 20 s with everyone ready / 40 s with at least 2 players (UNCHARTED_U2MP_MM_MIN); alone after 45..60 s: leave the room and search again (PS3 "time out %i players"). Players who arrive get the page and the open vote. A member backing out leaves only itself |
| Host leaves (lobby, party, matchmaking) | party room: the server's next owner leads (auto grant); custom lobby: everyone left (host-migration.md) | every room has OWNER_AUTO_GRANT; a client that loses its host outside a match waits (15 s) for the room's new owner: that player hosts the transport, the others join it. Matchmaking goes on in the same room; party and custom lobby go back to PAGE_MULTIPLAYER as a party |
| Promote To Party Leader (`2d32c8b0`) | GrantRoomOwner `0x4d3a10` (no RPCN command) | online: owner succession = the chosen member (`u2o_set_owner_rank`), the leader leaves and joins the room again; the members follow through the migration path |

Friend requests (ABI 2, `u2o_friend_requests` / `u2o_add_friend` / `u2o_remove_friend` = RPCN AddFriend 8 /
RemoveFriend 9 through the launcher's session): Party Up lists the requests to us ("Friend request": Add Friend /
Decline) and ours ("Request sent": Cancel) after the friends; every friend row also has View Profile (name, state,
presence; PS3 sceNpProfileCallGui) and Remove Friend. Our own popup callbacks are registered in the menu callback
table (remaining.cpp).

## 5b. Host migration in a running match

PS3 (host-migration research): WaitReady turns OWNER_AUTO_GRANT on when the match starts (not in co-op: game types 4,
8, 9, 12, `0x4a5548`); when the host leaves, the room's next owner gets RoomOwnerChanged -> listener slot 6
(`0x440334` = PS4 `0x5e9f30`, intact): NGM host = that player's tracker, event `0x15f1cf05`, and the intact reset
states restart the round on the same map under the new host.

PC: at the match start the host sets the room's owner succession (the other players, `u2o_set_owner_rank`), and with
every roster it sends `kIds` (net id -> NpId). A client that loses the host while NET_SM is 26 and the game type is
not co-op waits for the room's new owner (15 s). Every player keeps its net id: the new host hosts the transport with
its own net id (its old slot reserved), the others join it asking for their old slot (`Transport::join_link`
`want_slot`, carried in the hello); everyone applies NetEventPlayerLeft for the old host, sets match session +0xb04,
and calls `0x5e9f30` (the new host at once, the others when they reach it). `UNCHARTED_U2MP_HOST_MIGRATION=0` turns it
off. Untested; if it fails, the match ends as before.

## 5c. Rankings and cinema files

**Rankings** (PS3 net-leaderboard 0x4a9e8c..0x4aba88, NP Score): boards 300 kills, 301 cash, 302 wins, 303 co-op
points (table PS4 0xdef0a0), career totals with a 64-byte big-endian game info {rank badge, seconds, deaths, losses}.
Leave::Enter (progression.cpp `leave_publish`) records them as PS3 did: co-op game types 303 + 301; competitive
300 / 301 / 302 only after a matchmaking match (netInfo+0x34). The leaderboard list (5b3000 port) fetches online when
the link is up: Global "my page" = GetScoreNpid(self) then GetScoreRange(page * 11 + 1, 11); a page = the range;
Friends = GetScoreFriends(self included), 11 a page. The leaderboard object's busy word holds the list while the
answer comes; a failed request shows the local board as before. Boards are made by RPCN on the first score
(descending, normal update), shared with RPCS3 players of NPWR00462.

**Cinema files** (PS3 fe0e7de7 0x43402c / 6b2970e2 0x433554): PS3 sent friends a 100-byte pointer to the copy on
Naughty Dog's server. PC sends the file itself: PAGE_CINEMA's upload button (and fe0e7de7) opens a popup of the
online RPCN friends; the message (main type 4 custom data, subject "Cinema File") carries {"U2CN", key, name, size}
and the file (up to 6 MB, `U2oMessage.blob`). A received file is saved at once (PS3 0x43302c: make room, the
sender's file name, the list refreshed; "no free slots" when all 20 are protected) and a message box says so; the
receive button (6b2970e2) tells how many came.

## 6. Untested / open

- Nothing here has run yet: first test = two PCs, both signed in (see TEST-LOG.md).
- BUTTON_MULTIPLAYER_INVITES is on the PS4 pages (test 50 showed its label); Party Up's first row
  "Browse Online Games" opens the join list as well.
- Exact semantics of `u2o_open`'s return value and of `U2O_EV_MEMBER_LEFT`'s owner (read from
  `e.room` when present; the owner change is taken from `U2O_EV_ROOM_UPDATED`).
- Search flag filter HIDDEN|CLOSED = 0: whether the service applies it as the PS3 server did.
- Party follow into a stranger's room (leader with members joining from the list) and the follower's
  error path.
- Member blob +10..+47 are 0 (PS3 NetPlayerData +356/+360/+228..+240 and the address words have no
  known PS4 counterpart); only +0, +8, +9 are filled.
- Done since (sections 5a-5c): stranger matchmaking (kind-3 rooms by playlist), lobby and in-match
  host migration, Promote (owner succession), invites by NpId (messages). Tier: rooms carry the owner's rank in 0x50
  and the search prefers rooms within 20 ranks, then the fullest. Still not done: distance scoring and
  the RTT check of the PS3 search (no location service), Kickout as a service call (no such call), player
  counts / lat-lon (no heartbeat server).
- The transport is a star over the owner: clients do not exchange game packets directly (as LAN).


## 7. Review against RPCN master (2026-11-11)

The DLL was checked line by line against RPCN 1.12.1 (7a3c3c4) and RPCS3 master: command / notification ids,
framing, every proto3 field, attribute ranges, signaling (SIGN v3, UDP 3657 ping) match. Fixed after the review:
- DLL: a member leaving one room keeps its P2P link while still in another of our rooms (it broke the party link
  when a member followed the leader into a match room); CONNECT_ACK resends echo the peer's timestamp; create / join
  wait up to 2 s for the first UDP pong (RPCN copies our address into the room then); lowest server / world id;
  IPv6 addresses ignored; room data only for our rooms; friend requests rolled back on error; exit presence under
  the empty communication id; timed-out peers tried again every 5 s.
- online.cpp: a joined room that is CLOSED (match running; RPCN does not enforce it) is left again ("That game has
  already started."); the join list shows PC rooms only (0x52 >= 1); party rooms get an owner succession (lowest
  member id first, re-sent when the members change; the match room uses the same order); host migration falls
  back to that order (or the promoted player) when RPCN has not moved the room in 15 s; a lost link is reopened
  every 15 s while in multiplayer; a refused party-room create waits 5 s.
- Still in the launcher's relay (another change in progress there): only the command ids the game uses should be
  forwarded (any server-side error closes the launcher's RPCN session), RemoveFriend's NotFound is 26, and the
  handoff should pass the RPCN host as UNCHARTED_U2MP_RPCN_HOST (the DLL's UDP pings default to np.rpcs3.net).
