# PORT-MATCHMAKING: Uncharted 2 PS3 online matchmaking, mapped for the PC port

What the PS3 GOTY build (NPUA80698 1.10) did with NP Matching2, NP Signaling and NP Basic. For each
piece, this document gives what the PS4 Collection 1.02 image kept, and the hooks a PC room service
needs to drive the state machine the PS4 still has.

Static analysis only. Nothing was run and no network service was contacted. No game bytes, server
names, URLs, keys or passphrase bytes are given here, only their locations. Addresses: PS3 = PS3 1.10
virtual address; PS4 = PS4 1.02 image offset (file offset - 0x183ce8).

Confidence marks: **P** proven (read in the disassembly on both sides, or a byte/semantic match), **L**
likely (one strong signal), **G** guess.

Related docs: `FUNCTIONS-FRONTEND-NET.md` (full function table), `SESSION-INTERFACE.md` (PS4 session
objects, TaskManagerOnline), `TRANSPORT-INTERFACE.md` (UDP transport), `PORT-FRONTEND3.md`
(matchmaking vote/gather, already ported), `NETINIT.md`.

Tools used: `ps3_map.py netcallers / callers / xref / strings`, `ps3_dis.py --annotate`, `ps4_dis.py`
(`--vt`), `ps4_xref.py`, the existing `functions-*.tsv`. Working dumps went to the session scratchpad
(`mm/*.dis`), outside the repo.

---

## 0. Summary

- **Communication id**: retail uses **NPWR00462**: SceNpCommunicationId at PS3 `0xe161b0`, passphrase
  (SceNpCommunicationPassphrase, 128 bytes) right after it at `0xe161bc`. A beta/demo build (title id
  BCET70015 / NPUA70051, test `0x94de8`) uses NPWR00795 at `0xe162dc`, passphrase at `0xe162e8`.
  Selector: `0x4ec120` (TOC slots `0xdb3e64` / `0xdb3e68`). The PS4 image has no NPWR string. (P)
- **Matching2 layer** = `game/net/net-session-manager.cpp` (`0x4cfe38..0x4d1ec8`) and
  `game/net/net-session.cpp` (`0x4d1e2c..0x4da600`). One context, every available server gets a
  server context, a world is picked from a DC list. Two NetSession objects: **g_matchSession
  `0xf20c48`** and the **party session `0xf2c8a0`**. Both are rooms. **None of it exists on PS4**: the
  PS4 executable imports no sceNpMatching2 function. (P)
- **Signaling** = `NpConnectionManager` `0xf3a060` (`0x4e8848..0x4eae30`): 32 records of 460 bytes. The
  game activates signaling itself, by NpId, for every room member (Matching2 signaling type NONE). On
  ESTABLISHED it reads the peer IP and port, and the UDP transport sends to them. PS4 keeps only dead
  wrappers. (P)
- **The state machine survived on PS4** as data: NET_SM global `0x14ce97c`, the session objects (same
  class, 64-bit layout, section 5.1), the listener interface (vtable `0x103fa50` = PS3 `0xd70408`, P),
  the custom-game pages (partial) and TaskManagerOnline. The PC LAN path (`session.cpp`) already
  writes those objects directly. A room service only has to replace what `session.cpp` fakes.
- **Coordinator check (a)**, the "match started / LoadLevel received" flag: PS3 `match+4472` = PS4
  `match+0xb54`. It is raised by NetEventLoadLevel Apply (PS3 `0x474bd8` = PS4 `0x63b6b0`, intact), and
  by the PS3 host itself when the match task starts (`0x4b1c24`, `0x465c34`). Unlike U3, it does
  **not** depend on an online-only backend slot. It travels as a net event, so a PC room service gets
  it for free once the transport carries events. (P)
- **Coordinator check (b)**, a net player per room member: PS3 EstablishConnection::Enter `0x4416e0`
  walks the room members (`0x4d5e38`) and creates a net player for each (`0x49b058` ->
  AddOrUpdateNetPlayer `0x49ada8`). It then sends each member mailbox 0x106 (16 bytes). PS4
  EstablishConnection::Enter `0x5f3050` is reduced to `ClearNetPlayers(0x113b760)`: **the member loop
  was dropped** (P). `session.cpp fill_net_players()` already does it for the LAN path. An online
  path must call it from the room member list, after the reset and before EstablishConnection::Update
  `0x5f2ea0` runs.

---

## 1. Every NP call site (PS3 1.10)

### 1.1 Matching2 (`sceNp2`, stubs `0xae2344..0xae2704`)

| Import | Wrapper (caller) | What it is |
|---|---|---|
| sceNpMatching2Init2 | `0x4d0fc0` | SessionManager::Init, `(0,0,0)`: default stack/priority |
| sceNpMatching2CreateContext | `0x4d0fc0` | `(npId, commId=0x4ec120(), passphrase=commId+12, &ctxId -> mgr+28, option=3)` (3 = USE_ONLINENAME \| USE_AVATARURL) |
| sceNpMatching2RegisterContextCallback | `0x4d0fc0` | cb `0x4d0d90`, arg mgr |
| sceNpMatching2ContextStartAsync | `0x4d0fc0` | timeout 10,000,000 us (10 s) |
| sceNpMatching2AbortContextStart | `0x4d0fc0` | after 5 x 1 s sleeps without the Start event |
| sceNpMatching2RegisterRoomEventCallback | `0x4d0fc0` | cb `0x4d0c88` (queues events) |
| sceNpMatching2RegisterRoomMessageCallback | `0x4d0fc0` | cb `0x4d1c80` |
| sceNpMatching2SetDefaultRequestOptParam | `0x4d0fc0` | `{cb 0x4d0130, arg mgr, timeout 20,000,000 us, appReqId 0}` |
| sceNpMatching2GetServerIdListLocal | `0x4d0fc0` | up to 8 servers -> mgr+0 count, mgr+4 ids |
| sceNpMatching2GetServerInfo | `0x4d0fc0` | per server, cb `0x4d02c8` (event 0x0001), busy-waits mgr+14 |
| sceNpMatching2GetWorldInfoList | `0x4d0fc0` | per available server, cb `0x4d0e78` (event 0x0002), waits mgr+20 |
| sceNpMatching2CreateServerContext | `0x4d0fc0` | per available server, cb `0x4d0040` (event 0x0009), waits mgr+12 |
| sceNpMatching2DeleteServerContext | `0x4d1a00` | Term, cb `0x4cff50` (event 0x000a) |
| sceNpMatching2ContextStop / DestroyContext / Term2 | `0x4d1a00` | Term (called by teardown `0x4386d0`) |
| sceNpMatching2GetEventData | `0x4d02c8 0x4d040c 0x4d0e78 0x4d1c80 0x4d4e0c 0x4d5628 0x4d7e48 0x4d8220 0x48f350` | |
| sceNpMatching2ClearEventData | `0x4cff50 0x4d0040 0x4d0130 0x4d040c 0x4d1a00 0x4d1c80 0x48f350` | |
| sceNpMatching2CreateJoinRoom | **`0x4d421c` NetSession::CreateJoinRoom** | section 1.3 |
| sceNpMatching2JoinRoom | **`0x4d4578` NetSession::JoinRoom** | section 1.4 |
| sceNpMatching2SearchRoom | **`0x4d3c50` NetSession::SearchRoom** | section 1.5 |
| sceNpMatching2LeaveRoom | **`0x4d8540` NetSession::LeaveRoom** | `{roomId, optData 0}`, cb `0x4d8bf4` (0x0103), timeout 10 s |
| sceNpMatching2SetRoomDataExternal | **`0x4d3af8`** | int attr 0x53 only (section 1.6) |
| sceNpMatching2SetRoomDataInternal | **`0x4d4868` HostRank, `0x4d4b28` HostMigrate, `0x4d5338` Hide, `0x4d57b0` RoomName** | section 1.7 |
| sceNpMatching2SetRoomMemberDataInternal | **`0x4d2ed4`** | member bin attr 0x59 (section 1.8) |
| sceNpMatching2GrantRoomOwner | **`0x4d3a10`** | `{roomId, newOwner memberId}`, cb `0x4d4f68` (0x0104), 10 s |
| sceNpMatching2KickoutRoomMember | **`0x4d8a98`** | `{roomId, target memberId u16, blockKickFlag u8}`, cb `0x4d5034` (0x0105), 10 s. Kicking yourself = LeaveRoom |
| sceNpMatching2SignalingGetPingInfo | **`0x4d3848`** | request all zero (roomId 0 as compiled), cb `0x4d4e0c` (0x0E01) logs "RTT Room Id %lld : %i ms". Only from SELECT_JOIN_GAME `0x465808` |
| sceNpMatching2GetRoomMemberDataExternalList | `0x490c28` | NetFriends "players met" (not matchmaking) |

Not imported at all: GetRoomDataExternalList, SendRoomMessage, SendRoomChatMessage,
GetRoomDataInternal, SetSignalingOptParam, lobby functions. The room message callback `0x4d1c80`
exists, but nothing in this build sends room messages. Lobbies are not used (lobbyId 0 everywhere).

Request event ids checked by the callbacks: GetServerInfo 1, GetWorldInfoList 2, SetRoomDataExternal
4, CreateServerContext 9, DeleteServerContext 10, CreateJoinRoom 0x101, JoinRoom 0x102, LeaveRoom
0x103, GrantRoomOwner 0x104, Kickout 0x105, SearchRoom 0x106, SetRoomDataInternal 0x109,
SignalingGetPingInfo 0xE01. (P)

### 1.2 Server and world selection (`0x4d0fc0`, `0x4cfe88`, `0x444bb0`)

1. ContextStartAsync. The context callback `0x4d0d90` sets flag `0xf2ef90` and stores the error
   code at `0xf2ef94` on event 0x6F02 (Start). If the error is 0x8002233c, it retries, up to 10
   starts.
2. GetServerIdListLocal (max 8), "%i Server(s) found".
3. For each server: GetServerInfo. A server whose status byte (+2) is not 1 (available) is removed
   ("Server %i NOT Available"). Otherwise GetWorldInfoList ("worldId = %d, numOfRoom = %d,
   numRoomMember = %d"), then CreateServerContext. The world lists are kept at mgr+24[i].
4. "%d Servers - %d Worlds" / "No Server Available".
5. Default world for a NetSession: `0x4d01a8` (register the session, max 4: mgr+32..+44) sets
   `session+6 = ctxId` and `session+8 = worldId`, from the first world of the first server.
6. **Custom game host** `0x444bb0`: DC table `e5350103` = {count, int[]}. Picks
   `entry[rand() % count]` = a world index n, then `0x4cfe88(mgr, n)` = the n-th world across all
   servers -> `session+8`. PS4 `0x5eb830` still does this pick (DC `e5350103`, `0xde72c0` rand), over a
   list at `0x14ce990` (count) / `0x14ce9b8` (array) that only PS3's init filled. (P)

### 1.3 CreateJoinRoom `0x4d421c(session, maxSlot, ownerAutoGrant, tier, partyFlag, partySize)`

Request (PS3 32-bit struct, sp+272, 112 bytes zeroed):

| Field | Value |
|---|---|
| worldId | `session+8` |
| lobbyId | 0 |
| maxSlot | arg (custom: `0x49e620(ngm)` = the custom game's player count; matchmaking: `0x49a958(ngm)` = the playlist's max players; party room: **10**) |
| flagAttr | `0x80000000` (OWNER_AUTO_GRANT) if `ownerAutoGrant`. Party room: yes. Match rooms: no (switched later by HostMigrate). Copied to `session+152` |
| roomBinAttrInternal (1) | id **0x58** = room name `"%s.%lld"` (own online id from netInfo+0x288, `0x4e7cf0(0)` network time), in `session+24` |
| roomSearchableIntAttrExternal (6 or 7) | see table below |
| roomSearchableBinAttrExternal | none |
| roomBinAttrExternal (0 or 1) | id **0x55** = `session+248`, size `session+244` (8 bytes: two floats, see 0x4E) |
| password, groups, allowed/blocked users, joinRoomGroupLabel | none |
| roomMemberBinAttrInternal (0 or 1) | id **0x59** = `session+4404`, size `session+4400` (section 1.8) |
| sigOptParam | all zero (signaling type NONE: the game runs its own sceNpSignaling) |
| opt | `{cb 0x4d8220, arg session, timeout 0 = default 20 s}` |

Searchable int attributes (ids are SCE_NP_MATCHING2_ROOM_SEARCHABLE_INT_ATTR_EXTERNAL_1..8 = 0x4C..0x53):

| Id | Value at create | Search filter (`0x4d3c50`) | Meaning |
|---|---|---|---|
| 0x4C | `session+236` \| (partySize << 12 if partyFlag) | EQ, same value | **Room kind/version key**. `session+236` is set before each create/search by `0x4d23d4`: custom games = `netInfo+0` (= DC `6f1c3f87`[0] \| 0x90000, a net/build version, P); matchmaking = `(playlistIndex << 20) | netInfo+0 | 0x10000` (playlistIndex = `0x491a04(ngm, NetPlayerData+192)`, P). A room only matches the same version and the same playlist |
| 0x4D | `partySize` (creator's party) | none (read when picking, section 4.3) | party size of the room's creator |
| 0x4E | int(netInfo+36) | GE/LE window `v -/+ tol` | **latitude**, int(degrees) (P: `0x4496ac` stores the "location" latitude/longitude from the ND heartbeat reply into netInfo+36/+40 via `0x4a9d44`) |
| 0x4F | int(netInfo+40) | GE/LE window | **longitude** |
| 0x50 | country code (2 chars) << 16 \| language1 | EQ, only if the criteria row asks for it | region / language |
| 0x51 | tier, if tier >= 0 | GE `tier - criteriaIndex`, LE `tier` | player tier `0x4c54b0` = NetPlayerData+224 clamped (<=1 -> 1, >3 -> 4) |
| 0x53 | `session+504` | none | room state, set by SetRoomDataExternal (1 = match running, 0 = back in lobby; G) |

Bin external 0x55 (8 bytes) = host latitude/longitude floats (`0x4d3920(session, 0, 0xf20c40, 8)`
before create). Searchers get it back and rank rooms by great-circle distance `0x4e86a0` ("%8.2f km").

Response `0x4d8220` (event 0x101): GetEventData (CreateJoinRoomResponse). Clears the member table,
adds every member (`0x4d76e8`), `session+16 = roomId`, `session+152 = flag`, calls listener slot 0,
logs "Created Room Id %llx", sets `session+153 = 1` (done) and `session+156 = error`. On failure:
"Session Create failed %x", roomId 0.

### 1.4 JoinRoom `0x4d4578(session, roomId)`

`{roomId, password 0, groupLabel 0, roomMemberBinAttrInternal {0x59, session+4404, session+4400}}`,
opt `{cb 0x4d7e48, arg session, timeout 5,000,000 us (5 s)}`. Response `0x4d7e48` (0x102): same as
create (members, roomId, flag, internal bin attrs 0x57/0x58 -> `0x4d3920`), listener slot 1,
`session+176 = 1`, `session+180 = error`, `session+184 = response buffer`. On error: "JoinRoom
Failed: %s (%x)" (text from `0x4eae30`). Callers: custom JOINING `0x466590` / `0x466694`, matchmaking
`0x4b1790` / `0x4b399c`, party join `0x4c4704`.

### 1.5 SearchRoom `0x4d3c50(session, latLonTol, regionFlag, tier, tierBelow, tierAbove, partyFlag, partySize)`

`{option 0x13 (WITH_NPID | WITH_ONLINENAME | RANDOM), worldId session+8, lobbyId 0, range
{startIndex 1, max 5}, flagFilter 0, flagAttr 0, intFilter[] (above), binFilter none, attrId [0x55]}`,
opt `{cb 0x4d5628, arg session, timeout default}`. Callback `0x4d5628` (0x106) copies the event data
to `session+172` (size known up front), `session+168 = error`, `session+164 = 1`. `0x4d5940` turns it
into the caller's list: entries of 80 bytes {+8 roomId u64, +16 owner name, +80 roomDataExternal*}.
Filter operators: 1 EQ, 4 LE, 6 GE.

Callers:
- Custom game list `0x4655b0` (BUTTON_MULTIPLAYER_JOIN): `(0, 0, -1, 0, 0, 0, 0)` -> only
  `0x4C == netInfo+0`. Up to 5 random custom rooms of the same build.
- Matchmaking `0x4b1134`: `(criteria.latLonTol, criteria.regionFlag, tier, criteriaIndex, 0, 0,
  partySize)`. The criteria row comes from DC `b25ab071` (20-byte rows, indexed by `0xf22630`; +0
  lat/lon tolerance, +4 same-region flag, +8 max ping in ms, G for +4/+8 roles). The index goes up
  on each failed round, so the search widens.

### 1.6 SetRoomDataExternal `0x4d3af8(session, value)`

Before a room exists, it only stores `session+504` (published by the next create). Otherwise
`{roomId, roomSearchableIntAttrExternal [{0x53, value}]}`, cb `0x4d5100` (event 4: "SetRoomDataExternal
%lld 0x%x", `session+232 = error`, `session+228 = 1`). Callers: custom host entering SERVER_LOBBY
`0x465fe8` (value 1), Leave::Enter `0x445e9c` (value 0).

### 1.7 SetRoomDataInternal (all cb event 0x109, timeout 10 s, error 0x80022335 is ignored)

| Wrapper | Request | Done flag / error | Callers |
|---|---|---|---|
| `0x4d4868` **HostRank(session, memberIds[], n)** | ownerPrivilegeRank list = the order of the next owners | +212 / +216 ("HostRank Error") | EstablishConnection::Update `0x441290` (members sorted by link quality once "Connections done") |
| `0x4d4b28` **HostMigrate(session, on)** | flagFilter 0x80000000, flagAttr on ? 0x80000000 : 0 | +204 / +208 | host-left handler `0x440c2c` (value `0x4a5548()`), Leave::Enter `0x446300` (0) |
| `0x4d5338` **Hide(session, on)** | flagFilter 0x50000000 (CLOSED \| HIDDEN), flagAttr on ? 0x50000000 : 0; `session+4396 = on` | +196 / +200 | party room hidden at NetInit `0x448ed4`; match room hidden when the match is about to start (`0x43ad84`, `0x4b35d8`, `0x4b3c18`, `0x443ad4`, `0x43b0fc`) and shown again in the lobby (`0x439ad8`, `0x439b54`, `0x43e608`, `0x443dc4`, `0x446310`, `0x446708`) |
| `0x4d57b0` **RoomName(session)** | bin internal 0x58 = "%s.%lld" | +220 / +224 | matchmaking host after the vote `0x4b35c8` |

### 1.8 Member data: SetRoomMemberDataInternal `0x4d2ed4(session, data, size <= 64)`

Copies to `session+4404` (size `session+4400`, assert "size <= sizeof(m_meBinData)"), then
`{roomId, memberId 0 (me), roomMemberBinAttrInternal [{0x59, data, size}]}`, cb `0x4d479c`, 10 s.
The only producer is **`0x4af240`** (PS4 `0x64e810`, progression.cpp). It publishes the same 48 bytes
to both sessions:

| Offset | Content |
|---|---|
| +0 u64 | **party room id** (party session +16) when the party has more than one member and is not left, else 0. This is how party members find each other's match (P) |
| +8 u8 | rank badge `0xd0578(0xe9c94c)` (session.cpp: "room member data +8 rank") |
| +9 u8 | team preference, NetPlayerData+211 (1 / 2) |
| +10 u32 | NetPlayerData+356 |
| +14 u32 | NetPlayerData+360 |
| +19 u8 | byte `0xe1d728+12` (G: headset/voice) |
| +20..+35 | NetPlayerData+228, +232, +236, +240 (u32 each; stats, G) |
| +36 8 bytes | 4 x u16 from `0xf1f56e` (G: local network address/port words) |

Received member data (event 0x1107) is copied into the member record (+248 size, +252 data).

### 1.9 Signaling (`sceNp` signaling imports, NpConnectionManager `0xf3a060`)

| Import | Wrapper | Role |
|---|---|---|
| CreateCtx, AddExtendedHandler | `0x4e9d28` Init (from NetInit) | ctx with handler `0x4e9618`, extended handler `0x4ea438`; magic 0x1234 |
| ActivateConnection | `0x4e9060` activate, `0x4e9e98` connect by NpId, `0x4ea438` | |
| GetConnectionFromNpId | `0x4e9e98` | reuse an existing connId ("---- cached") |
| GetConnectionStatus | `0x4e8f1c` status, `0x4e9618`, `0x4e9e98`, `0x4ea438` | status + **peer IP/port** |
| GetConnectionInfo | `0x4e9338` (RTT), `0x4e9400` (bandwidth), `0x4ea438` (peer address) | |
| GetPeerNetInfoResult | `0x4e9618` | NETINFO_RESULT (NAT type) |
| TerminateConnection | `0x4e8c8c`, `0x4e8afc`, `0x4e9618`, `0x4ea438` | |
| DestroyCtx | `0x4e8afc` Term | |

Details in section 2.3.

### 1.10 NP Basic

| Import | Wrapper | Role |
|---|---|---|
| RegisterHandler / UnregisterHandler | NetInit `0x447a5c` / teardown `0x4386d0` | handler `0x449ae8` |
| GetEvent, RecvMessageAttachmentLoad | `0x449ae8` | incoming messages -> mailbox queue (`0x4c3018` "Post Message %x, size %i") |
| **SendMessage** | **`0x4c2bc0` party/peer message** `(npId, code, data, size)` | body = {u32 code, data}, at most 511 bytes |
| SendMessageGui | `0x43a164` invite, `0x43402c` (cinema) | invite: mainType 3 (INVITE), 16-byte attachment {u32 netInfo+0 (version), ...} |
| RecvMessageCustom | `0x440948` BUTTON_MULTIPLAYER_INVITES, `0x433554` (cinema) | accept an invite |
| GetCustomInvitationEntryCount, GetMessageEntryCount | `0x43b200` | invite badge count |
| Friend list / presence / players history | `0x4465c8 0x490040 0x48f2b8 0x48f16c 0x490338 0x490a88 0x491580` | friends UI, presence, players met |

---

## 2. Callbacks and events

### 2.1 Room events (`0x4d0c88` queue -> `0x4d0bd0` per frame -> `0x4d040c`)

`0x4d0c88` only queues: 40-byte entries at `0xf2e588`, count `0xf2ef88`, max 64 (assert
"g_sessionEventQueueNum != SESSION_EVENT_QUEUE_MAX"). NdiNetUpdate `0x449e88` drains it every frame
through `0x4d0bd0`. `0x4d040c` finds the registered session whose roomId is the event's ("**** GOT
MESSAGE from old ROOM" otherwise) and dispatches:

| Event | Handler effect |
|---|---|
| 0x1101 MemberJoined | GetEventData -> "%s joined" -> **add member `0x4d76e8`** (record, signaling connect by NpId `0x4e9e98`, listener slot 2) |
| 0x1102 MemberLeft | "%s has left : SCE_NP_MATCHING2_ROOM_EVENT_MemberLeft" -> **remove member `0x4d7200`** (listener slot 3, TerminateConnection) |
| 0x1103 Kickedout | listener slot 4, slot 5; if still that room: roomId = 0, `session+188 = 1`; remove all members `0x4d74b8` |
| 0x1104 RoomDestroyed | listener slot 4; same cleanup |
| 0x1105 RoomOwnerChanged | `session+4392 = newOwner`; listener slot 6 (host migration) |
| 0x1106 UpdatedRoomDataInternal | `session+4396 =` new flagAttr HIDDEN bit; each bin attr -> `0x4d3920` (0x57 -> session+248, 0x58 -> room name) |
| 0x1107 UpdatedRoomMemberDataInternal | member by NpId (`0x4d4020`) gets the new 0x59 data (+248 / +252) |
| 0x1108 UpdatedSignalingOptParam | ignored |

Room message callback `0x4d1c80`: GetEventData, then listener slot 7 (never fires: nothing sends).

### 2.2 The NetSession listener (vtable), PS3 and PS4

PS3 match session listener `0xd70408` (9 slots) vs PS4 `0x103fa50`. Paired by body (P):

| Slot | PS3 | PS3 effect | PS4 | PS4 |
|---|---|---|---|---|
| 0 created | `0x43d660` | `0x4d26dc`; abort byte `0xf119c4 = 0`; +4472 = +4473 = 0 | `0x5e9d90` | +8 = 0, `0x113a270 = 0`, +0xb54 = 0 (u16) — kept |
| 1 joined | `0x43d610` | same as slot 0 | `0x5e9db0` | kept |
| 2 member added | `0x43dad8` "%s joined match": activate connection `0x4e9060`, message channel `0x4674e0`, "Clearing Initial Host Connection" | `0x5e9dd0` | connection record ref/activate (record table `0x1152a10`) — kept, partly |
| 3 member removed | `0x43d9e0` "%s left match": sends NetEventPlayerLeft (type 79) | `0x5e9f00` | **ret (stripped)**. session.cpp `player_left()` replaces it |
| 4 room gone | `0x438424` abort byte = 1 | `0x5e9f10` | `0x113a270 = 1` — kept |
| 5 kicked | `0x43d5d8` `0x4d26f4`, +4473 = 1 | `0x5e9f20` | +8 = 1, +0xb55 = 1 — kept |
| 6 owner changed | `0x440334` "New host : %s" -> new host tracker `0x494428` | `0x5e9f30` | partly kept (looks up the tracker of `match+0xb04`, event `15f1cf05`) |
| 7 room message | `0x438440` blr | `0x5ea040` | ret |
| 8 | `0x438444` blr | `0x5ea050` | ret |

So `PORT-NULLSITES.md`'s "no PS3 pair" for `0x103fa50` is superseded: it is the same interface.
Party session listener PS3 `0xd71e58` (`0x4c39f4, 0x4c39d0, 0x4c40f0 "%s joined party", 0x4c3a18 member
left, 0x4c4a88, 0x4d26f4, 0x4c36a8, 0x4c3708, 0x4c36ac`) vs PS4 `0x100b510` (`6612a0, 6612b0, 661230,
661270 ret, 661280, 66a810, 661430 ret, 661290 ret, 661420 ret`): same pattern, member-left stripped (L).

PS3 abort byte `0xf119c4` = PS4 `0x113a270` (P). PS3 `session+4472/4473` = PS4 `+0xb54/+0xb55` (P).

### 2.3 Signaling usage

- One NpConnectionManager, 32 records (`0xf3a060 + 8 + i*460`). Record: +4 used, +5/+6 dead flags,
  +44 peer IPv4, +48 peer port, +52 connId, +60 message channel (reset by `0x468658`). NpIds of recent
  dead peers go to a 32-entry ring at mgr+14720 (count +14728). The matchmaking pick skips them
  (`0x4e8978`).
- Connect: `0x4e9e98(mgr, npId)` = GetConnectionFromNpId, else ActivateConnection(ctx, npId); a record
  per NpId. Called for every room member (`0x4d76e8`), the chosen room owner before joining
  (`0x4b1604`), party/lobby paths (`0x43a444`, `0x465808`).
- Main callback `0x4e9618(ctx, subj, event, error)`: event 0 DEAD -> mark dead (+6; +5 = error is
  0x8002a80f), voice port off `0x4e8a50`, channel reset, TerminateConnection; event 1 ESTABLISHED ->
  GetConnectionStatus -> **record+44 = peer addr, +48 = peer port** ("%24s %1d %s:%d"); event 2
  NETINFO_ERROR; 3 NETINFO_RESULT -> GetPeerNetInfoResult.
- Extended handler `0x4ea438`: 10 EXT_PEER_ACTIVATED (activate back, "Connection %s connId %i"), 11
  EXT_PEER_DEACTIVATED ("%s has left"), terminate), 12 EXT_MUTUAL_ACTIVATED (`0x4e9e98`, log address).
- **Peer address -> UDP transport**: NetConnectionUpdate `0x43cdb8` / `0x4652cc` sends through
  `0x4db1d0` (sendto on g_udpSocket `0xf10e18`, the P2P socket with SO_USECRYPTO) to record+44/+48.
  Receive: `0x4dba48` recvfrom, matched by address. `TRANSPORT-INTERFACE.md` 1.4 maps the PS4 record
  table `0x1152a10` (stride 0x220).
- Thresholds: matchmaking CONNECT_TO_HOST reads RTT (`0x4e9338`, us) and bandwidth (`0x4e9400`)
  ("retRtt = %x ... Bandwidth = %i KBps"). If RTT/1000 >= the criteria row's max ping (+8, non-zero)
  the room is dropped. The custom game browser only logs ping (`0x4d3848`).

---

## 3. NET_SM state machine (PS3 `0xf11764` = PS4 `0x14ce97c`)

States written (from `FUNCTIONS-FRONTEND-NET.md`): 0 off, 3 CREATE_GAME_WAIT, 4 CLIENT_START, 5
CLIENT_GAME_LIST_WAIT, 6 CLIENT_GAME_LIST_PICK, 7 CLIENT_CONNECT_TO_HOST, 9 CLIENT_RESERVE_SLOTS_WAIT,
10 CLIENT_WAIT_FOR_PARTY_CONNECT_GAME_HOST, 12 CLIENT_WAIT_FOR_PARTY_HOST_JOIN, 14
CLIENT_JOIN_GAME_WAIT, 16 SERVER_LOBBY, 17 CLIENT_LOBBY, 18 CHOOSE_HOST_JOIN, 19 START_MATCHMAKING, 20
SERVER_VOTE_SCREEN, 21 CLIENT_VOTE_SCREEN, 26 UPDATE, 27 RESET, 28 REPLAY, 29 RESULTS, 30
ROUND_RESULTS, 35 LEAVE_GAME. NetInit ends with 18.

### 3.1 Custom game, host (`0x444bb0` cb `e2707dbe` = PS4 `0x5eb830`; page cb `0x465da0` = PS4 `0x6093f0`, P by page id `69deab50`)

1. `0x444bb0`: netInfo mode, NGM reset `0x4942ac`, `session+236 = netInfo+0`, `+240 = netInfo+4`
   (DLC mask), lat/lon -> bin ext, random world (1.2), CreateJoinRoom(match, customMax, 0, -1, 0, 0)
   -> page `69deab50` with cb `0x465da0` -> **3 CREATE_GAME_WAIT**. Fails -> 18.
2. `0x465da0` per frame: waits for create done (`0x4d23e4`). Error -> 18. OK -> `0x4c3e38` (party: wait
   for members whose member data +0 is our party room, slot manager `0x4da168`) -> **10
   WAIT_FOR_PARTY_CONNECT_GAME_HOST**. When every party member has joined and is connected
   (`0x4d2080`, `0x4c3d00`, `0x4da3f8`; "A party member couldn't talk to the game host" on timeout)
   -> `0x4c38f0`, `0x4895b8` -> **16 SERVER_LOBBY**, SetRoomDataExternal(0x53 = 1), start the lobby
   game task (`0x43a398` -> `0x7e7a74`; PS4 PlayTask "deathmatch").

PS4 `0x5eb830` kept steps 1 except CreateJoinRoom (no Matching2 import), and `0x6093f0` runs 3 -> 10 ->
16 off `match+0x99` (created). session.cpp sets that byte (`create_match_now`). (P)

### 3.2 Custom game, client (JOIN `0x4655b0` = PS4 `0x6095d0` partial; SELECT `0x465808` = `0x609680` partial; JOINING `0x4661e0` = `0x609790` partial)

1. BUTTON_MULTIPLAYER_JOIN: `0x4383fc`, `session+236/240` as host, SearchRoom (custom filter) -> **5**.
   The page lists the results (`0x4d2410` status, `0x4d5940` list).
2. SELECT_JOIN_GAME `0x465808`: ping info `0x4d3848` (log only), `0x4d5940`, connect to the room owner
   by NpId (`0x4e9e98` + `0x4e9060`) -> **7 CONNECT_TO_HOST**.
3. JOINING `0x4661e0`: 7: wait for signaling ESTABLISHED (`0x4e8f1c`). Then **9 RESERVE_SLOTS_WAIT**:
   mailbox 0x100 to the host over the P2P link (the host's slot manager `0x4da278` answers 0x101,
   48 bytes, via `0x4893dc`). "Joining Request timed out" -> 18. Granted -> `0x4c3b58` (party
   bookkeeping) -> JoinRoom -> **14 JOIN_GAME_WAIT**. Joined -> **17 CLIENT_LOBBY** (`0x4c370c`).
   With a party: leader -> 12 WAIT_FOR_PARTY_HOST_JOIN / 10 (members follow, section 3.4).
4. CLIENT_LOBBY: PAGE_LOBBY `0x443bc8` (PS4 `0x5ebac0`, intact). The host's NetEventLoadLevel moves it
   on (PS4 client wait `0x609d00`).

### 3.3 Matchmaking (PAGE_MULTIPLAYER_MATCHMAKING `0x4b0a58` = PS4 `0x64e910` ret; ported for parties by frontend3.cpp)

- **19 START_MATCHMAKING** (`0x4b0f48`): `session+236 = (playlistIdx << 20) | netVer | 0x10000`,
  `+240 = netInfo+4`. Criteria row `b25ab071[f22630]` ("Searching Game with Criteria %i"). If the
  party leader can't search (`0x49a738`), or the party flag at NetPlayerData+367 says so, it goes
  to create. Otherwise SearchRoom (1.5) -> **5 GAME_LIST_WAIT**.
- **5**: on completion, `0x4d5940` list; for each room log "%s, %i players %8.2f km" (distance from
  bin ext 0x55 to our lat/lon) -> **6 GAME_LIST_PICK**.
- **6** (`0x4b13ac`): **room scoring** = geo distance (km); a room whose 0x4D (creator party size)
  shares bits with our party size gets a penalty; skip full rooms (member count == `0x49a958` max)
  and rooms whose owner is in the dead-peer ring; keep the minimum. None -> next criteria /
  create. Chosen: owner NpId -> netInfo+96, roomId -> netInfo+88, connect to the owner, `match+4468
  = connId` -> **7**.
- **7 CONNECT_TO_HOST**: status 2 (active) -> RTT/bandwidth check (2.3). Too slow / failed ->
  terminate, back to 19 with the next criteria. OK -> reserve slots (**9**, as 3.2) -> JoinRoom
  `0x4b1790` -> **14** -> **17 CLIENT_LOBBY** / **21 CLIENT_VOTE_SCREEN** -> 4 CLIENT_START.
- No room: "GATHER": CreateJoinRoom(match, playlistMax, 0, tier, 0, partySize) `0x4b2a18` -> **3** ->
  **16 SERVER_LOBBY** (waits by player count, table `0xb40710`, "time out %i players" -> LeaveRoom
  and search again) -> vote (**20**) -> result -> RoomName `0x4d57b0`, Hide(1) -> host sets
  `match+4472 = 1` and starts the game task (`0x4b1c24`). Detail of gather/vote: `PORT-FRONTEND3.md`
  2.1.
- "Host Asked to leave the room" (`0x4b20f4`, mailbox 0x103): terminate + LeaveRoom -> 35.

### 3.4 Party (party session `0xf2c8a0`)

- NetInit: party room created on demand: CreateJoinRoom(party, **10**, ownerAutoGrant **1**, -1, 0, 0)
  `0x4c48d0`; Hide(party, 1) `0x448ed4` (closed and hidden: joined by roomId only).
- Join a party: NP Basic message **0x10c** {u64 party roomId} (`0x4c2bc0` from `0x4c4820..0x4c49e8`) ->
  `0x4c41f8` -> JoinRoom(party) `0x4c4704`. "Couldn't talk to the party host %s" / NET_SM_PARTY_ERROR
  handling in `0x4c41f8`.
- **Follow the leader**: every member publishes member data +0 = party roomId (1.8). The leader's
  match create/join is followed through the mailbox: the leader sends 0x107 (start matchmaking), 0x114
  (countdown), 0x10e (party host joined the game). Members join the same match room (12 -> 14 -> 17).
  The leader waits in 10 until every member is in its match room and connected (`0x4c3d00` /
  `0x4c3e38` compare members' room ids via the slot manager `0x4da168`).
- Party data flows over the same P2P connections (party room members are signaling peers too).

### 3.5 Mailbox codes

Queue `0xf2bba0` (52-byte entries, count `0xf2bb98`). Peek `0x4c2a18(code, ..., consume)`, read
`0x4c2e08`, post `0x4c3258`. Filled from NP Basic messages (`0x449ae8`) and from NetEventMessage
(type 46, PS4 Apply `0x62dc50`), sent with `0x4893dc(conn, code, data, len)` (in game) or `0x4c2bc0`
(NP Basic, to an NpId).

| Code | Seen at (PS3) | Role |
|---|---|---|
| 0x100 / 0x101 | `0x4da2e0`, `0x4da358` (48 bytes), `0x4663d0` | slot reservation request / grant (P sites, L role) |
| 0x102 | `0x43a568` | lobby status (PAGE_MULTIPLAYER) |
| 0x103 | `0x4b0c18`, `0x465bc4` | leave room / matchmaking (PORT-FRONTEND3) |
| 0x104, 0x106 | `0x44a248`, `0x44a270`; 0x106 also EstablishConnection::Enter per member (16 bytes) | connection hello / ready (G) |
| 0x105 | `0x4422c0` | results to clients (SESSION-INTERFACE 3.2) |
| 0x107, 0x108, 0x109, 0x10a, 0x112, 0x114, 0x115 | matchmaking / countdown | PORT-FRONTEND3 table |
| 0x10b (NP Basic, 8 bytes) | `0x43e15c`, `0x43ffd8`, `0x449e04` | "join my room" {u64 roomId} (friend join, auto reply) (L) |
| 0x10c (NP Basic, 8 bytes) | `0x4c4810`, `0x4c4968`, `0x4c49d8` | party invite {u64 party roomId} (L) |
| 0x10d | `0x4c3ba4` | party (G) |
| 0x10e | `0x466628` | party host joined (session.cpp `kMsgPartyHostJoined`) |
| 0x110 | `0x43a500`, `0x443ce0` | multiplayer mode (session.cpp `kMsgMode`) |
| 0x113 | `0x4b2dcc` | matchmaking (G) |
| 0x116 (NP Basic + mailbox) | `0x43e95c`, `0x4c4220` | party (G) |
| 0x117 | `0x442794` | TaskManagerOnlineUpdate (G) |
| 0x118, 0x119 (NP Basic 48 bytes), 0x11a | `0x43e0ac`, `0x43e46c`, `0x43fd94` | party member data (0x119 carries the 48-byte member blob, L) |

### 3.6 Invites

Send: `0x43a164` (sceNpBasicSendMessageGui, mainType INVITE, 16-byte attachment, "Error %x sending
invite to %s"). Receive: `0x449ae8` (events, attachment load), count `0x43b200`, accept from
BUTTON_MULTIPLAYER_INVITES `0x440948` (RecvMessageCustom) -> party join (3.4). PS4 kept the
InvitationDialog import only at system level; the U2 paths are absent / ret (`0x5ebea0`).

### 3.7 Host migration

- Match rooms are created without OWNER_AUTO_GRANT. At "Connections done" the host publishes the
  owner order (HostRank `0x4d4868`, members sorted by link quality in `0x440f90`).
- When the host leaves during a match, the state's host-left handler (`0x4409c4` / `0x440478`, "*****
  HOST %s LEFT") either leaves (LeaveRoom, abort) or, if the mode allows it (`0x4a5548`), takes over
  (`0x48a13c`) and sets HostMigrate (`0x4d4b28`). The server's RoomOwnerChanged event -> listener
  slot 6 "New host : %s" -> `0x494428` (NGM host). Leave::Enter switches it off again (`0x446300`).
- PS4: slot 6 `0x5e9f30` partly kept; `0x440f90`'s HostRank call is gone from PS4 `0x5f2ea0`. (L)

### 3.8 Late join

There is no separate late-join path: JoinRoom is accepted while the room is not CLOSED. The host
closes (Hide) the room when the game is about to start and reopens it in the lobby. Late join in a
running match is therefore not possible on PS3 (P for the Hide sites, L for the conclusion).

### 3.9 Hand-over to the match (PS3 -> PS4 -> session.cpp)

| Step | PS3 | PS4 | session.cpp |
|---|---|---|---|
| lobby -> start | `0x444660` = Playing::Enter (NET_SM 26) | `0x5f23e0` | intact, used |
| TaskManagerOnline Start | IsHost(match) ? EstablishConnection : LoadLevels | `0x5efe00` | uses match +0xb00/+0xb04 |
| EstablishConnection::Enter | `0x4416e0`: NGM reset, **net player per room member** `0x49b058`, mailbox 0x106 to each | `0x5f3050`: reset only | `fill_net_players()` |
| EstablishConnection::Update | `0x440f90`: all trackers connected -> "Connections done", `0x4895b8`, AssignTeams `0x4a2490`, `0x48e5b8`, LoadLevel event `0x489d4c`, **HostRank**; next state when `match+4472` | `0x5f2ea0` (no HostRank) | state updates wrapped; T_CONNECTED = 1 |
| LoadLevel flag | NetEventLoadLevel Apply `0x474bd8` sets `match+4472` | `0x63b6b0` sets `+0xb54` | intact |
| results, back to lobby | Leave::Enter `0x445608` (SetRoomDataExternal 0, HostMigrate 0, Hide 0, NET_SM 16/17 by IsHost) | `0x5f00d0` | intact; LEAVING_GAME `finish_leaving_game` |

---

## 4. Heartbeat, stats and the "no connection" path

- **Heartbeat** is plain UDP to a Naughty Dog server, not NP Score or TUS. NdiNetUpdate `0x449e88`
  formats "%s heartbeat %s headset %d party %d match_mode %d gametype %d nat %d playlist %d" (`0x44a428`:
  online id, page/state string, headset, party size `0x4d24a8`, netInfo+44 match mode, game type, NAT
  `0xf3a030`, playlist). It wraps it with `0x467160` and sends it with `0x4db1d0` on the "world"
  socket `0xf11270` to the address at `[0xf10cac]` (+6 port). The reply is parsed by `0x4496ac`:
  "playercount", "dailycount", "location" (latitude/longitude -> netInfo+36/+40 via `0x4a9d44`,
  "Distance from %s : %8.2f km"), "heartbeat" (interval), "next", "Invalid Data from Server". The same
  socket did the NAT probe in NetInit (6 x sendto/recvfrom). **Port note**: latitude/longitude feed
  the matchmaking attributes 0x4E/0x4F and the room choice. A PC room service should provide them, or
  zero them on both sides.
- **Stats and profile** go over HTTPS (cellHttp/cellSsl) to cloud storage: `0x44b030` "Saving Stats to
  SQS", `0x44b580` / `0x44ba68` / `0x44c018` signed object GET/PUT ("Saving Stats to S3"), `0x44cc48`
  / `0x44d898` / `0x44ebb0` cached file downloads (cellFs), `0x44d450` header check, init `0x44cb54` /
  `0x450170`. Signing material is in the image (not reproduced). Results thread: NET_SM_RESULTS
  `0x43ec08` "Save Game Stats". PC: progression.cpp / netpatch.cpp replace this data locally.
- **"No connection" dialog**: PAGE_MULTIPLAYER_CONNECTING `0x445020` (PS4 `0x5ed6a0` partial) waits for
  NetInit `0x447a5c` (NP status `0x4ec068`, tickets, DRM, HTTP downloads, NAT probe, Matching2 init).
  Any failure -> message `0x88a082af` (`0x42a3e8`) and teardown `0x4386d0`. session.cpp bypasses it.

---

## 5. PS4 counterparts

### 5.1 Session object layout (same class; PS4 = 64-bit pointers)

| Field | PS3 | PS4 | Source |
|---|---|---|---|
| ctxId / worldId | +6 / +8 | - | not used on PS4 |
| roomId | +16 | +0x10 | session.cpp S_CTX (P) |
| created / create error | +153 / +156 | +0x99 / +0x9c | (P) |
| join done / error / response | +176 / +180 / +184 | +0xb8 / +0xbc / +0xc0 | (P) |
| left | +188 | +0xc8 | (P) |
| attr 0x4C base / DLC mask | +236 / +240 | +0x100 is set to 8 by `0x5eb830` (G) | |
| members (10) | +512, stride 352: +224 valid, +232 memberId, +240 connId, +248/+252 bin data | +0x228, stride 0xd0: +9 valid, +0x10 id, +0x18 key | (P) |
| my memberId / owner | +4388 / +4392 | +0xb00 / +0xb04 | (P) |
| hidden | +4396 | - | |
| member blob | +4400 / +4404 | - | |
| LoadLevel / kicked | +4472 / +4473 | +0xb54 / +0xb55 | (P) |
| host conn (matchmaking) | +4468 | +0xb50 (party: "member leaves", PORT-FRONTEND6) | (L) |

### 5.2 Function table

States: **kept** = code present; **partial**; **stub** = ret/ret 0; **dropped** = no PS4 code.
All of the Matching2, NP Basic and signaling-context functions are dropped (no import), P.

| PS3 | Role | PS4 | State | Mark |
|---|---|---|---|---|
| `0x4ec120` / `0x4ec184` / `0x4ec0f0` | commId select / sceNp+sceNp2 init / term | - | dropped | P |
| `0x4d0fc0` / `0x4d1a00` | SessionManager Init / Term | - | dropped | P |
| `0x4d0c88 0x4d0bd0 0x4d040c` | room event queue / drain / dispatch | - | dropped | P |
| `0x4d0d90 0x4d02c8 0x4d0e78 0x4d0040 0x4cff50 0x4d0130 0x4d1c80` | context/request/message callbacks | - | dropped | P |
| `0x4d01a8 0x4d1e2c 0x4d1ebc 0x4d1ec8 0x4cfe88` | session register / ctor / set ctx / set world / world by index | - | dropped (`0x5eb830` reads its own list) | P |
| `0x4d421c` / `0x4d8220` | CreateJoinRoom / response | - | dropped (call removed from `0x5eb830`) | P |
| `0x4d4578` / `0x4d7e48` | JoinRoom / response | - | dropped | P |
| `0x4d3c50` / `0x4d5628` / `0x4d5940` | SearchRoom / callback / result list | - | dropped (`0x6095d0` partial) | P |
| `0x4d8540` / `0x4d8bf4` | LeaveRoom / response | - | dropped (`match+0xc8` never set; session.cpp sets it) | P |
| `0x4d8a98` / `0x4d5034` | Kickout | - | dropped (session.cpp kKick) | P |
| `0x4d3a10` / `0x4d4f68` | GrantRoomOwner | - | dropped | P |
| `0x4d3af8` / `0x4d5100` | SetRoomDataExternal 0x53 | - | dropped | P |
| `0x4d4868 0x4d4b28 0x4d5338 0x4d57b0` (+ cbs `0x4d4a0c 0x4d4cf0 0x4d550c 0x4d521c`) | HostRank / HostMigrate / Hide / RoomName | - | dropped | P |
| `0x4d2ed4` / `0x4d479c` | SetRoomMemberDataInternal | - | dropped; producer `0x4af240` = `0x64e810` | P |
| `0x4d3848` / `0x4d4e0c` | SignalingGetPingInfo | - | dropped | P |
| `0x4d76e8` / `0x4d7200` / `0x4d74b8` | add / remove member / remove all | - | dropped (session.cpp `session_members`) | P |
| `0x4d2394` IsHost, `0x4d2370` owner, `0x4d234c` me, `0x4d24a8` count, `0x4d5e38` member list, `0x4d5a18` find by NpId | accessors | inlined field reads | inlined | L |
| `0x4d2410 0x4d245c 0x4d23e4` | search / join / create status | inlined (`0x6093f0` reads +0x99) | inlined | L |
| `0x4da278 0x4da168 0x4d9be8 0x4d987c` | slot manager | - | dropped | P |
| `0xd70408` listener | | `0x103fa50` | kept except slot 3 (ret) | P |
| `0xd71e58` party listener | | `0x100b510` | kept except member-left etc. (ret) | L |
| `0x4e9d28` / `0x4e8afc` | NpConnectionManager Init / Term | - | dropped | P |
| `0x4e8c8c` / `0x4e9060` / `0x4e9338` / `0x4e9400` | terminate / activate / RTT / bandwidth | `0x675200` / `0x6752b0` / `0x675370` / `0x675400` | kept, dead (no caller) | P (existing map) |
| `0x4e9e98 0x4e8f1c 0x4e9618 0x4ea438 0x4e8978` | connect by NpId / status / callbacks / dead-peer check | - (inlined copies in `0x609790`, `0x609680`, `0x6093f0`, `0x5e9dd0`, `0x661230`) | dropped / inlined pieces | P |
| `0x4c2bc0 0x4c2a18 0x4c2e08 0x4c3258` | mailbox send (NP Basic) / peek / get / post | - | dropped; NetEventMessage Apply `0x62dc50` kept | P |
| `0x449ae8` | NP Basic event handler | - | dropped | P |
| `0x43a164` / `0x440948` / `0x43b200` | invite send / accept / count | - / `0x5ebea0` ret / - | dropped / stub | P |
| `0x4c41f8` | NET_SM_PARTY_ERROR, party join/create | - | dropped | P |
| `0x4c3b58 0x4c3d00 0x4c3e38 0x4c38f0 0x4c370c 0x4c3764 0x4c37c8` | party follow helpers | - | dropped | P |
| `0x444bb0` | custom host -> CREATE_GAME_WAIT | `0x5eb830` | kept minus CreateJoinRoom | P |
| `0x465da0` | custom host page | `0x6093f0` | kept (3 -> 10 -> 16) | P |
| `0x4655b0 0x465808 0x4661e0` | custom join list / select / joining | `0x6095d0 0x609680 0x609790` | partial | P (existing map) |
| `0x4b0a58` | matchmaking page | `0x64e910` | stub (frontend3.cpp) | P |
| `0x43a444` | member side of lobby messages | - | dropped | P |
| `0x4416e0` | EstablishConnection::Enter | `0x5f3050` | partial (loop dropped) | P |
| `0x440f90` | EstablishConnection::Update | `0x5f2ea0` | partial (HostRank dropped) | P |
| `0x474bd8` | NetEventLoadLevel Apply | `0x63b6b0` | kept | P |
| `0x440334` / `0x4409c4` / `0x440478` | new host / host left (2 states) | `0x5e9f30` / - / - | partial / dropped | L |
| `0x449e88` / `0x4496ac` | NdiNetUpdate + heartbeat / heartbeat reply | - | dropped | P |
| `0x447a5c` / `0x445020` | NetInit / CONNECTING | - / `0x5ed6a0` | dropped / partial | P |

---

## 6. Port plan: the minimal hooks for an external room service

Order = dependency. Items 1-4 already exist for the LAN path in `session.cpp`; the online version
feeds them from the room service instead of the direct transport.

1. **Room service client (C++, no guest hook)**: create / search / join / leave / kick / set member
   data / room events. It must carry, per room, the PS3 attribute set (1.3): 0x4C key (build
   version + playlist index), 0x4D creator party size, 0x4E/0x4F lat/lon (or 0), 0x50 region, 0x51
   tier, 0x53 state, bin 0x55 (8 bytes), room name 0x58, member blob 0x59 (48 bytes, 1.8), flags
   HIDDEN/CLOSED, maxSlot (custom: `0x49e620`, matchmaking: `0x49a958`, party: 10).
2. **Peer address -> transport** (replaces sceNpSignaling): on member joined, the service gives the
   peer's IP:port (or a relay). Call the transport's connect for that peer. On link up: what
   `gamenet::attach` does now (record table `0x1152a10`, TRANSPORT-INTERFACE 7.2). On link down:
   detach + `player_left()` (PS3 slot 3 is a PS4 ret).
3. **Session objects** (match `0x1065b08`, party `0x105efb8`): on create/join results, write roomId
   (+0x10), me/owner (+0xb00/+0xb04), member table (+0x228, stride 0xd0), and the done/result bytes
   (+0x99/+0x9c, +0xb8/+0xbc/+0xc0); +0xc8 on leave. Then call listener slots of `0x103fa50` /
   `0x100b510`: 0 created, 1 joined, 2 member added, 4 room gone (abort), 5 kicked, 6 owner changed.
   Existing: `session_set`, `session_members`.
4. **Net players at match start**: in EstablishConnection::Enter (`0x5f3050`, after its
   ClearNetPlayers) create one tracker per room member: `fill_net_players()`, i.e. PS3 `0x4416e0`.
   Without it, EstablishConnection::Update `0x5f2ea0` sees no trackers and the NGM local player is
   NULL (the U3 cause B).
5. **Host create**: in the custom host path (`session.cpp` host_create = PS4 `0x5eb830` without its
   world pick), call the service's CreateRoom, wait for the result, then set `match+0x99 = 1`, `+0x9c =
   0`. Page `0x6093f0` takes 3 -> 10 -> 16 by itself. Party members: send the follow (kFollow =
   mailbox 0x10e path) once the room exists.
6. **Join list**: BUTTON_MULTIPLAYER_JOIN `0x6095d0` -> service search (filter 0x4C == version);
   fill the list the join page reads (menus.cpp join list); SELECT `0x609680` -> connect to the owner,
   then the reserve step (mailbox 0x100/0x101: the host checks free slots for the whole party), then
   JoinRoom; JOINING `0x609790` states 7 -> 9 -> 14 -> 17.
7. **Member data**: hook PS4 `0x64e810` (= PS3 `0x4af240`, already in progression.cpp) to also publish
   the 48-byte blob to the service. The roster queries `0x659bb0/0x659bc0` read rank (+8) and team
   (+9) from it.
8. **Matchmaking** (optional, strangers): frontend3.cpp already runs gather/vote for a party. Add
   states 19 -> 5 -> 6 -> 7 -> 9 -> 14 with service search (0x4C key = playlist; widen lat/lon/tier per
   criteria row `b25ab071`), pick by distance and party-size penalty, RTT check against the criteria
   max ping. No room -> create (state 3) and gather (16).
9. **Room state**: Hide(1) when the countdown ends (close the room), Hide(0) and 0x53 = 0 back in the
   lobby (Leave::Enter `0x5f00d0`), 0x53 = 1 when the lobby task starts.
10. **Host migration** (last): owner order (HostRank) at "Connections done", server-side owner
    handover, listener slot 6. Until then: the host leaving ends the match (slot 4 abort), as LAN does
    now.
11. **Not needed for matches**: heartbeat (only lat/lon and player counts), HTTPS stats (local on PC),
    NP Basic invites (the service can carry the 0x10b/0x10c "join my room" payload {u64 roomId}
    instead).

U3 lessons, checked for U2:
- (a) **Connection-ready flag**: U2 has no online-only backend slot for it. The flag (+0xb54) is set
  by the LoadLevel event, which runs on every path that carries net events. Nothing to add beyond a
  working event transport. The host sets its own copy when it applies its own LoadLevel (`0x5f2ea0`
  sends it to itself).
- (b) **Member loop**: dropped on PS4 (`0x5f3050`); must be done in C++ (item 4).
- **Party follow**: PS3 used the member blob (+0 = party roomId) plus mailboxes (0x107 matchmaking,
  0x10e host joined). The leader's step 10 waits for all members in its room, with a timeout and "A
  party member couldn't talk to the game host". session.cpp's kFollow covers the LAN case. Online:
  the leader publishes the match roomId (member blob or service field); members JoinRoom it and
  acknowledge (0x10e).

---

## 7. Open questions

1. DC rows: `b25ab071` (criteria: +0 lat/lon tolerance, +4 region flag, +8 max ping are inferred
   from use; +12/+16 unknown), `e5350103` (world list), `6f1c3f87` (net version), `0xb40710` (gather
   counts). Their values are in the PS3 DC data, not read here.
2. Exact meaning of member blob +10..+35 and +36 (address words?), and of the mailbox codes marked G.
3. Invite attachment: the 16 bytes are {u32 version, ...}; where the party room id goes at accept time
   (`0x440948`) was not traced.
4. NetInfo+4 (DLC/map-pack mask, `session+240`): it is stored, but no create or search use was found.
   Possibly checked client-side via the result's bin data (`0x4d2e68`). Not confirmed.
5. Host migration end to end (`0x48a13c`, `0x4a5548` mode test, PS4 `0x5e9f30` remainder) is L only.
6. PS4 `match+0x100 = 8` in `0x5eb830`: which PS3 field it replaced (attr base +236?) is G.
7. The PS4 signaling callers listed in the brief (`0x5e9328 0x609328 0x609928 0x6751f8 0x660ce8`) are
   import-thunk call sites inside the functions above (`0x5e9dd0`, `0x6093f0`, `0x609680/0x609790`,
   `0x675200`, `0x661230`). They were not re-traced one by one.
