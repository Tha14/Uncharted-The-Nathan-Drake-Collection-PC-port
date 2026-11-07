# Uncharted 2 multiplayer: where a new network transport plugs into PS4 1.02

Read-only static analysis of the PS4 Collection 1.02 image (`boot-linked.bin`, image address =
file offset - 0x183ce8) and PS3 1.10 `EBOOT.elf`. All PS4 addresses are image addresses, all
PS3 addresses are vaddrs. No game bytes are reproduced; short instruction excerpts only.

Tools: `u2mp/tools/ps4_dis.py` (new: PS4 range disassembly with image addresses, `--vt` dumps
a pointer table from the ELF relocations), `ps4_cg.py` (callers/refs), `ps3_dis.py --annotate`.

## 0. Result in one paragraph

The PS4 build kept far more of the net stack than the earlier maps suggest. Everything above the
packet level survives and is reachable through the message-layer vtable `0x103fc80` of the 32
per-peer connection records at `0x1152a10`: event queuing (`0x644ab0` -> `0x609fc0` and inline
queue code), message packing into a BitStream (`0x60ac70`), unpacking + factory + Read + relay +
Apply (`0x60a110`), ack/loss handling (`0x60b780`, `0x60b8f0`) and reset (`0x60ba10`). What is
missing is only the PS3 `NetConnection` packet layer (header, sequence/ack bitfield, 30-packet
window: `0x465138`, `0x4652cc`, `0x465218`, `0x464cb8`), the UDP update loop (`0x43cdb8`), the
connection manager that creates the records, and the code that registers players in the
NetGameManager tracker table (`AddOrUpdateNetPlayer 0x642370` is dead and has its name source
stripped). The transport therefore has to: (1) fill connection records and player trackers,
(2) per frame, build packets by calling `vtable[2]` and parse them by calling `vtable[1]`, with
a port of the PS3 packet header/ack code around them, and (3) report delivered/lost packets
through `vtable[3]` / `vtable[4]`.

**Correction to FUNCTIONS-MATCH-GAMEPLAY.md section 7 and the TSV:** event vtable slot 2 is
**Read** and slot 3 is **Write** (the column headings are swapped). Evidence: PS4 ConnectionDone
slot 2 `0x63bf70` uses the `ReadBits` assert string `0xe87dca`, slot 3 `0x63bdb0` the `WriteBits`
string `0xe87dc0`; PS3 pack `0x467600` serializes with vtable `+12` (slot 3); PS4 pack `0x60ac70`
calls `[vtbl+0x18]`, PS4 unpack `0x60a110` calls `[vtbl+0x10]`. Likewise PS3 `0x42f8d8` is
**ReadBits** and `0x42fe78` is **WriteBits** (section 4).

## 1. Sending: what gameplay calls and what it does on PS4

### 1.1 Event object (PS4)

| Offset | Meaning | PS3 |
|---|---|---|
| +0x00 | vtable (7 used slots, below) | +0 |
| +0x08 u8 | type id 0..79 | +4 |
| +0x09 u8 | delivery mode: 0 reliable ordered, 1 unreliable but re-queued on loss, 2 unreliable fire-and-forget (snapshots) | +5 |
| +0x0a u8 | reference count (sender holds 1, each queue node +1) | +6 |
| +0x0b u8 | receiver applies only while `g_netGameManager+0x1e80 == 2` (in game) | +7 |
| +0x0c.. | payload | +8.. |

Vtable slots (8 bytes each): 0 dtor, 1 deleting dtor (called when refcount hits 0),
**2 Read(BitStream*)**, **3 Write(BitStream*)**, 4 Apply(NetConnection* from; NULL = local),
5 Relay(from, count, u16* netIds) = shared `0x60bce0`, 6 IsExpired() (shared `0x619da0`,
returns 0; pack drops an unreliable message when it returns non-zero).
Factory: `CreateNetEventByType 0x6180f0(type)` allocates (`0xb332a0`), sets vtable, type,
mode, refcount 1 and the +0xb flag.

### 1.2 Send entry points (PS4)

| PS4 | PS3 | Signature / behaviour |
|---|---|---|
| `0x644ab0` | `0x493728` | `NetGameManager::SendNetEvent(ngm=0x113b760, NetEvent* ev, u32 slotMask)`. 83 direct call sites in 0x5e0000..0x680000 (+2 in `0x243e40`). `-1` = everyone; SendToAll PS3 `0x493d20` is inlined as mask -1. |
| `0x644e30` (tail inside 644ab0) | `0x493a48` | `SendNetEventToPlayer(ngm, ev, netId)`: finds the tracker with that net id, skips the local one, mask = 1<<slot, jumps to 644ab0. |
| `0x644e80` (tail inside 644ab0) | `0x493ca8` | `SendNetEventToHost(ngm, ev)`: host tracker = `[ngm+8]`. |
| `0x66a3d0` | `0x4d3754` | `NetSession::SendNetEvent(session=0x1065b08, ev, memberMask)` - same algorithm over the session member list (10 x 0xd0 at session+0x20c; member +0x25 used, +0x2c net id, +0x34 connection key; session+0xb00 local net id, +0xb04 host net id). Used by PlayerInfo `0x6144b0`, NpcDealDamage `0x6142a0`, and the dead StartConnection `0x60bf20` / ConnectionDone `0x60c0f0` senders. |
| `0x66a350` | - | `NetSession::SendNetEventToMember(session, ev, netId)`. |
| `0x609fc0` | `0x467ba0` | `NetMessageLayer::QueueEvent(M, ev, u32 relayCount, u16* relayIds, bool relayOnly)`. |

Example sender, EndGame `0x60c7f0(reason, int*)` (PS3 `0x489ff8`): alloc 0x18 bytes, vtable
`0x1040d00`, type 6, mode 0, refcount 1, payload +0xc reason, +0x10 value;
`0x644ab0(0x113b760, ev, -1)`; then `ev->Apply(ev, NULL)` (slot 4) locally; refcount-- and
delete. StartGame `0x60c660`, AssignTeam `0x610840`, LoadLevel `0x60c2e0`, PlayerDeath
`0x616580`, CharacterMove `0x60ded0`/`0x60e7f0` follow the same pattern. Gameplay never touches a
socket; the host applies its own events locally and sends them to the others.

### 1.3 What 0x644ab0 does (verified in the disassembly)

Pass 1 over the 10 trackers (`tracker i = ngm + 0x10 + i*0x2c0`): skip unused
(`tracker+0x20` name[0] == 0), excluded (`tracker+0x26c` != 0) and local (`net id == [ngm]+0x120`)
trackers and slots not in the mask. The host tracker, and every peer whose connection record
has `rec+6 != 0` (directly connected), get a bit in the "direct" mask; for every other
target its 16-bit net id is put in a relay list (max 10).

Pass 2 over the trackers: look up the connection record whose `rec+0 == tracker+0x23c`; for the
host's record with a non-empty relay list call `0x609fc0(M, ev, n, list, relayOnly = !direct)`;
otherwise, if direct, allocate a 0x28-byte queue node and append it.

Queue node (0x28, PS3 20 bytes): +0x00 NetEvent* (refcount++), +0x08 u32 reliable sequence
(mode 0: `M+0x140`++; modes 1/2: 0xffffffff), +0x10 u16* relay ids, +0x18 u16 relay count,
+0x1a u16 relayOnly, +0x20 next.
Reliable nodes go to `M+0x110` (head) / `M+0x118` (tail), unreliable (mode 1/2) to
`M+0x120` / `M+0x128`.

So clients without a direct link reach each other through the host: the host receives the
message with a relay list and re-queues it (`0x60bce0`, only when session local id == host id).

### 1.4 Connection record table (PS4 `0x1152a10`, PS3 NpConnectionManager `0xf3a060`)

Header: +0x04 signaling context id. 32 records at `0x1152a18 + j*0x220` (`[0x104ae30]` holds
`0x1152a10`). Static constructor `0x675570` sets each `M` vtable to `0x103fc80`.

| rec+ | Meaning |
|---|---|
| 0x00 u32 | connection key (= `tracker+0x23c`, PS3 room member id) |
| 0x04 u8 | record in use |
| 0x06 u8 | direct link up (644ab0 tests byte 6 of the first qword) |
| 0x07 | SceNpId (36 bytes) given to sceNpSignalingActivateConnection |
| 0x2c u32 / 0x30 u16 | peer IPv4 / port (dead getter `0x675320`); port must be non-zero for the lag check |
| 0x34 s32 | signaling connection id (non-zero needed by the lag check, -1 none) |
| 0x38 s32 | reference count (release `0x675200`/`0x5e9dd0`: at <=1 -> `0x60ba10`, clear +4, TerminateConnection) |
| 0x40 | `M`, the NetConnection/NetMessageLayer object (to rec+0x220) |

`M` layout (PS3 `NetConnection` + message layer, PS3 offset +4 for the 8-byte vtable pointer):

| M+ | Meaning | PS3 C+ |
|---|---|---|
| 0x00 | vtable `0x103fc80` | 0 |
| 0x08 u32 | next outgoing packet sequence | 4 |
| 0x0c u32 | highest received packet sequence | 8 |
| 0x14 u32 | received-packet bitfield (bit k = packet `M+0xc - k` had payload) | 16 |
| 0x18 u32 | highest acked outgoing packet sequence | 20 |
| 0x1c..0x9b | per-sequence ring, 32 x u32 (inferred from PS3 layout) | 24 |
| 0xa4 / 0xa8 f32 | last send / last receive time (reset sets 0xa8 = now) | 160/164 |
| 0xbc | name string | 184 |
| 0x100 / 0x108 | in-flight packet records head/tail (linked through record+8) | 248/252 |
| 0x110 / 0x118 | reliable send queue | 256/260 |
| 0x120 / 0x128 | unreliable send queue | 264/268 |
| 0x130 | reliable receive reorder list (sorted by seq) | 272 |
| 0x138 | acked-out-of-order reliable list | - |
| 0x140 u32 | next reliable send seq | 280 |
| 0x144 s32 | highest contiguously acked reliable seq (init -1) | 284 |
| 0x148 u32 | next expected reliable receive seq | 288 |
| 0x150 / 0x158 | receive hook fn(ev, float) + context (PS4 installs no-op `0x5e9d80`) | 292 |

`M+0x08` and `M+0x18` are read by the TaskManagerOnline update `0x5f14d0` (`@5f17b0`): with
`rec+4`, `rec+0x34`, `rec+0x30` set, `M+8 - M+0x18` (packets in flight) is compared with a
config limit clamped to 30 (lag/pause logic). The transport must keep these two counters
truthful.

Message-layer vtable `0x103fc80` (PS3 `0xd70bd8`):

| Slot | PS4 | PS3 | Function |
|---|---|---|---|
| 0 | `0x609e60` | `0x467b5c` | AllocPacketRecord() -> 0x18 bytes {+0 u32 packet seq, +8 next, +0x10 message list} |
| 1 | `0x60a110` | `0x467e88` | ReadMessages(M, BitStream*) - receive path, section 3 |
| 2 | `0x60ac70` | `0x467600` | WriteMessages(M, BitStream*, record) - pack, section 2 |
| 3 | `0x60b780` | `0x468a18` | OnPacketDelivered(M, record) - releases messages, advances `M+0x144` |
| 4 | `0x60b8f0` | `0x4688a8` | OnPacketLost(M, record) - mode 0: back into `M+0x110` sorted by seq; mode 1: front of `M+0x120`; mode 2: released |
| 5 | `0x60bcc0` | `0x467538` | tail of `M+0x130` (helper) |
| - | `0x60ba10` | - | Reset(M): lost-handles all in-flight records, frees all lists, `M+0x140=0`, `M+0x144=-1`, `M+0x148=0` |

## 2. Consumption on PS3 and the wire format (to reimplement)

Call chain PS3: `NetConnectionUpdate 0x43cdb8` (per frame from `0x449e88`) -> per connection:
`0x467d20` drop expired unreliable messages (slot 6), every 10 s a ping, at most every 0.05 s
and only if `0x467438` (queues or unacked non-mode-2 messages pending) or 2 s passed:
`WriteU8(0)` + `NetConnection::WritePacket 0x4652cc` -> header `0x465138` + `vtable[2]`
(pack `0x467600`) -> `sendto 0x4db1d0`. Times are frame counter `[0xe60a00+0xec]` x 1/300.

UDP datagram (bit stream, MSB first, 8191-byte receive buffer):

```
u8  kind           0 data | 2 clock request {u32 t} -> reply 3 {u32 t, u32 netclock}
                   4 ping {f32 now} -> reply 5 {f32 echo} (latency -> conn+368)
kind 0:
  1   hasMessages  (0 = ack-only, sent when 30 packets are unacked: "%s window full")
  11  packetSeq    (M+8 after increment; receiver extends with its high bits, wrap 2048)
  10  ackSeq       (sender's highest received seq M+0xc; wrap 1024)
  32  ackBits      (sender's M+0x14; bit k -> packet ackSeq-k arrived with messages)
  if hasMessages:  message body (below)
```

Receive header (`0x464cb8`): reject if `seq - M+0xc > 31` or `ackSeq > M+8` ("Ignoring Packet");
if seq is new: `M+0x14 = (M+0x14 << (seq-M+0xc)) | hasMessages`; for every outgoing record
between `M+0x18+1` and `ackSeq` (records are kept in send order on the in-flight list and must
satisfy `record.seq == M+0x18 + i + 1`): bit set -> `vtable[3]` (delivered), else `vtable[4]`
(lost); free the record; then `M+0xc = seq`, `M+0x18 = ackSeq`. Only a new packet with
messages is handed to `vtable[1]`.

Message body (pack `0x467600` = PS4 `0x60ac70`; soft limit 9600 bits = "Exceeded Packet size
1200 bytes"):

```
unreliable section (queue M+0x120):
  repeat { 1; u8 type; <event slot 3 Write>; 1 hasRelay;
           [if hasRelay: 1 relayOnly; 4 count; count x u16 netId] }
  0
reliable section (queue M+0x110, only seq < M+0x144 + 64):
  repeat { 1; 1 seqJump; [if seqJump: 7 seq]   (else seq = previous + 1; first is explicit)
           u8 type; <Write>; relay block as above }
  0
```

All written messages move to the packet record's list (`record+0x10`); the record stays on
`M+0x100` until acked or lost. Per-type bit statistics are added in-line (PS3 `0x468dd4`).

## 3. Receive side

PS3: `recvfrom 0x4dba48` -> peer lookup by address (`0x4e92d0`) -> `ReadU8 kind` -> kind 0:
`NetConnection::ReadPacket 0x465218` (sets last receive time, header/acks `0x464cb8`, then
`vtable[1]`).

`vtable[1]` = PS4 `0x60a110` (PS3 `0x467e88`), fully present:
for each message: `CreateNetEventByType 0x6180f0(type)` -> `ev->Read(bs)` (slot 2) -> read relay
block -> receive hook `M+0x150` -> if relay list: `ev->Relay(M, n, ids)` (slot 5 `0x60bce0`,
host only: re-queues to the listed net ids, skipping the sender's record) -> unless relayOnly,
and unless `ev+0xb` is set while `[0x113b760+0x1e80] != 2`: `ev->Apply(ev, M)` (slot 4) ->
refcount-- / delete. Reliable messages are delivered in order (`M+0x148`, 7-bit seq extended
by +128, out-of-order ones parked on `M+0x130`).

Apply functions identify players by net ids in the payload; the sampled ones (`0x630f20`,
`0x6382d0`, `0x62c9f0`, `0x632110`, `0x61cb50`, `0x63b500`) do not use the `M` argument, so a
transport-built `M` (or NULL) is acceptable for injected events.

**What a new receive path must call on PS4:** after its own header/ack processing, call
`(*(fn*)(*(u64*)M + 0x08))(M, bs)` i.e. `0x60a110(M, bs)` with `bs` positioned at the message
body. To inject one event outside a packet: `ev = 0x6180f0(type); ev->Read(bs)` or fill the
payload, then `ev->Apply(ev, M)` and release.

## 4. BitStream API

PS4 has **no out-of-line BitStream functions**: `WriteBits` and `ReadBits` are inlined in 81 and
83 functions (assert strings `0xe87dc0` / `0xe87dca`, file `0xe87d91`). Object layout used by
all inlined copies and by `0x60a110`/`0x60ac70`:

```
struct BitStream { uint32_t* data; /*+0x00*/  uint32_t numBits; /*+0x08 cursor*/
                   uint32_t maxBits; /*+0x0c*/  uint32_t pad; /* PS3 has a 4th word */ };
```

Bit order: bit n lives in `data[n>>5]`, mask `0x80000000 >> (n&31)` (MSB first inside native
u32 words). PS3 used the same rule on big-endian words, so a PS3-compatible byte stream is the
PS4 word array byte-swapped per u32. Write: `w[i] = (w[i] & (~0u << (32-c))) | (v << (32-n)) >> c`,
spill into `w[i+1]`. Read mirrors it and reads `w[i+1]` whenever the field crosses a word.

Caveats for the transport:
- For `c == 0` the x86 shift by 32 is a no-op, so the write keeps the old word: **zero the send
  buffer before packing**.
- After each access `numBits` is compared with `maxBits`; on overflow the game calls the assert
  hook `[0x156df78]` then executes `int 0x41` (crash). A truncated or forged packet crashes the
  receiver inside an event Read: validate datagrams (length, checksum/MAC, known peer) before
  calling `0x60a110`, and allocate one spare word after the data.

PS3 reference (`0x42f868..0x430180`): Init `0x42f868`, Attach(buf, bytes) `0x42f880`
(maxBits = bytes*8), GetNumBits `0x42f8bc`, GetNumBytes `0x42f8c8`, **ReadBits(bs,&v,n)
`0x42f8d8`**, ReadFloat `0x42fc80`, ReadS32 `0x42fcbc`, ReadU32 `0x42fcf8`, ReadU16 `0x42fd70`,
ReadU8 `0x42fde8`, ReadBool `0x42fe24`, **WriteBits(bs,v,n) `0x42fe78`**, WriteFloat `0x430120`,
WriteS32 `0x430134`, WriteU32 `0x430140`, WriteU16 `0x430154`, WriteU8 `0x430168`,
WriteBool `0x430170` (all thin wrappers of the two bit functions). The stubbed PS4 serializers
(types 0, 37, 38, 44 Read only, 47, 49, 51, 52, 53, 54, 55, 57, 60, 61, 65, 67, 76) are ported from
the PS3 slot 2 (Read) / slot 3 (Write) bodies using these widths.

## 5. Snapshots (player state replication)

- Player state is `NetEventCharacterMove` (type 12): PS3 constructor `0x4694e0` sets mode 2
  and +7 (in-game only), filled by `0x4824b0` / `0x484cc8` (anim stream via
  `FillAnimCmdBitStream 0x480698`), sent with `SendNetEvent 0x493728(mask)` from `0x48a908`
  (caller `0x498f58`) and `0x48aa30` (NetPlayerTracker update `0x4cac50`).
  PS4: senders `0x60ded0` (called by tracker update `0x664bc0`) and `0x60e7f0` (called by
  `0x63fbf0`), both call `0x634970` and `0x644ab0`; Read `0x6398a0`, Write `0x6387f0`,
  Apply `0x6382d0` are intact. Nothing to port; the transport only needs to carry
  unreliable messages (mode 2 is dropped on loss, never resent).
- `NetEventSimpleSnapshot` (54, objects; interpolation `0x66ab90`): sender `0x615960` and
  Apply `0x61e1c0` alive, Read stub `0x61e2a0` (slot 2) and Write stub `0x61e290` (slot 3) ->
  port PS3 Read `0x46dda8` / Write `0x46d218`.
- `NetEventTurretSnapshot` (67): Read stub `0x61bee0`, Write stub `0x61bed0`, Apply `0x61be10` alive (PS3 Read `0x46dc98`,
  Write `0x46d124`).
- There is no separate snapshot channel on PS3: snapshots travel as unreliable events in the
  same packets.

## 6. Connection and player identity

NetGameManager `0x113b760`: `+0x00` local tracker*, `+0x08` host tracker*, 10 trackers at
`+0x10 + i*0x2c0` (PS3 `0xf1dc80`, +0/+4, 10 x 624 at +16). Tracker fields (cleared by
`0x662a30`):

| Tracker+ | Meaning | PS3 |
|---|---|---|
| 0x20 char[64] | name; first byte != 0 = slot used | 24 |
| 0x60 char[64] / 0xa0 char[64] | display string / second name | |
| 0x120 u32 | net id (12-bit values on the wire, u16 in relay lists) | 280 |
| 0x148 u8 | PlayerInfo received (set by PlayerInfo Apply `0x62e090`) | 320 |
| 0x238 u32 | member field +0x28 | |
| 0x23c s32 | connection key -> `rec+0` (-1 none) | 516 |
| 0x26c u8 | excluded (left); skipped by sends and by "Connections done" | 564 |
| 0x275 u8 | connection established (ConnectionDone) | 573 |
| 0x27c u8 | can talk to all peers (ConnectionDone; host candidate ranking) | 580 |

Registration: `AddOrUpdateNetPlayer 0x642370(ngm, session, member)` (PS3 `0x49ada8`) is
**dead** and its name copy is `strncpy(tracker+0x20, NULL, 64)` - do not call it. Port it: pick
the slot with the same net id or the first free one, `0x662a30(tracker)`, write name (+0x20),
net id (+0x120), key (+0x23c), +0x238, and set `[ngm]`/`[ngm+8]` when the net id equals the
local/host id. `RemoveNetPlayer` = `0x6422c0(ngm, slot)` (-> `0x662a30`), `ClearNetPlayers
0x63db90`.

Connection handshake on PS3 (absent on PS4): `NpNetUpdate 0x43bdd0` sends StartConnection
(`0x489c50`, via NetSession send `0x4d3754`; Apply `0x474e50` sets flag `0xf12620`), clients
answer ConnectionDone(canTalkToAll, a, b) (`0x489ec8`; Apply `0x474d90` -> `0x43dcd8`: tracker
of the pending id gets +573 = 1 and +580 = canTalkToAll). PS4 senders `0x60bf20`/`0x60c0f0` are
dead and Applies `0x63c0e0`/`0x63bda0` are `ret`. The host task "Connections done" `0x5f2ea0`
(PS3 `0x440f90`) waits until every used, non-excluded remote tracker has `+0x275 != 0` and
`+0x148 != 0`, then sends PlayerInfo `0x6144b0`, runs AssignTeams `0x6400d0`, sends AssignTeam
`0x610840` and LoadLevel `0x60c2e0`.

Leave: PS3 member-left handler `0x43d9e0` (host migration `0x4d9a78` if the host left) sends
PlayerLeft (`0x489968`, to all). On PS4 PlayerLeft is only built by the factory (vtable
`0x103fcc0`, Apply `0x619db0`: finds tracker by net id, `0x665cc0`, key = -1). Join: PS3
`0x43dad8` ("%s joined match") binds the member's connection (`0x4e9060`, `0x4674e0`).

## 7. Interface spec for the C++ transport

### 7.1 Data the transport owns per peer

```
struct Peer { uint32_t key;          // unique, != 0, = rec+0 = tracker+0x23c (e.g. index+1)
              uint32_t netId;        // game net id, 12-bit, = tracker+0x120
              int      rec;          // 0..31 in 0x1152a10
              Address  addr; bool canTalkToAll; double lastRecv; };
```

### 7.2 Peer connected (replaces NpConnectionManager + ConnectionDone)

1. Pick a record with `rec+4 == 0`: write `rec+0 = key`, `rec+4 = 1`, `rec+6 = 1` (direct;
   leave 0 for peers reachable only through the host), `rec+0x2c/0x30` = any non-zero
   address/port, `rec+0x34 = key` (non-zero), `rec+0x38 = 1`; call `0x60ba10(rec+0x40)`; set
   `M+0x150 = 0x5e9d80` (no-op hook) or 0.
2. Register the player (port of `0x642370`): tracker slot, name, net id, key; `[ngm]` / `[ngm+8]`;
   session `0x1065b08`: member entry (+0x25 used, +0x2c net id, +0x34 key), `+0xb00` local net id,
   `+0xb04` host net id, `+0x10` non-null (relay and session sends test it).
3. On the host, when the peer reports its links are up: `tracker+0x275 = 1`,
   `tracker+0x27c = canTalkToAll` (what ConnectionDone Apply did). PlayerInfo then sets `+0x148`.

### 7.3 Peer disconnected

Build PlayerLeft: `ev = 0x6180f0(79)`, `*(u32*)(ev+0xc) = netId`, host:
`0x644ab0(0x113b760, ev, -1)` then `ev->Apply(ev, NULL)`, release. Then
`0x60ba10(M)`, `rec+4 = 0`, `rec+0x38 = 0`, `rec+0 = 0`; if it was the host, run host
migration (PS3 `0x4d9a78`; not present on PS4).

### 7.4 Per frame

Call `Receive()` before and `Send()` after the TaskManagerOnline update `0x5f14d0` (vtable slot
`0x100ab98`; it runs `NetGameManager::Update 0x63e6a0`, which produces most events).

`Receive()` - for each datagram (validated, from a known peer, copied into a zeroed u32
buffer with one spare word, words byte-swapped from network order):
read kind; kind 0 -> port of `0x464cb8` on `M` (fields in 1.4) calling
`0x60b780(M, rec)` / `0x60b8f0(M, rec)` per acked/lost record and freeing it, then
`M+0xa8 = now` and, if new and hasMessages, `0x60a110(M, &bs)`. Kinds 2..5: clock sync and
ping as in `0x43cdb8` (or the transport's own equivalents).

`Send()` - for each peer record in use, if `now >= next` (0.05 s) and (`M+0x110` or `M+0x120`
non-empty or unacked reliable data or 2 s passed): zero buffer, `bs = {buf, 0, 1200*8}`,
WriteU8(0); if `M+8 - M+0x18 >= 30`: header with hasMessages 0 and the current `M+8`; else
`M+8++`, `r = 0x609e60(M)` (or own zeroed 0x18 block), `r+0 = M+8`, append `r` to
`M+0x100/0x108` via `r+8`, header with hasMessages 1, then `0x60ac70(M, &bs, r)`; byte-swap the
`(numBits+7)/8` bytes per word and send. Keep `M+8`, `M+0xc`, `M+0x14`, `M+0x18`, `M+0xa4`
current (the game reads `M+8`/`M+0x18`).

### 7.5 Functions the transport calls

| Address | Use |
|---|---|
| `0x60a110` | M vtable[1]: unpack + factory + Read + relay + Apply |
| `0x60ac70` | M vtable[2]: pack queued events into a BitStream |
| `0x609e60` | M vtable[0]: allocate packet record |
| `0x60b780` / `0x60b8f0` | M vtable[3]/[4]: packet delivered / lost |
| `0x60ba10` | reset a connection's message layer |
| `0x6180f0` | CreateNetEventByType(type) |
| `0x644ab0` | SendNetEvent(ngm, ev, mask) for transport-made events |
| `0x662a30` / `0x6422c0` / `0x63db90` | tracker clear / remove / clear all |
| `0xb332a0` / `0xb356d0` | game allocator alloc(size, align, tag*) / free(ptr, file, 0, file) as used for queue nodes |

Hooks needed: none on the send side (gameplay already queues into the records); the
stubbed Applies of types 0 and 1 and the 17 stubbed Read/Write slots are replaced in their
vtables only if those events have to cross the wire.

## 8. Limits

- Static reading only; nothing was executed. Offsets marked "inferred" come from the PS3 layout
  plus the PS4 constructor `0x675570` and reset `0x60ba10`.
- Tracker +0x238, +0x60/+0xa0 meanings and the exact lag threshold source (`0x104f1c8+0x58`) were
  not traced further.
- `0x609e60` allocates from the thread's current allocator; freeing records with `0xb356d0`
  matches what `0x60ba10` does, but a transport-owned record block avoids the question.
