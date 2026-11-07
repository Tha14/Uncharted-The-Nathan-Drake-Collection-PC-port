# Uncharted 2 multiplayer init state missing on PS4

Static analysis only (nothing was run). PS3 = 1.10 `EBOOT.elf`, PS4 = Collection 1.02 flat image
(`boot-linked.bin`, image address = file offset - `0x183ce8`). No earlier third-party Uncharted 2
multiplayer notes or code were used. Code: `gow3_pc/src/u2mp/netinit.cpp`
(`netinit_prepare()` on entering Multiplayer, `netinit_match()` at match start, both idempotent).

## Where the PS3 init ran

| PS3 | role | PS4 |
|---|---|---|
| `0x438bc8` bootstrap -> thread `0x447a5c` NetInit | NP, sockets, live data, session layer, voice, boosters, score/commerce | gone |
| `0x445020` PAGE_MULTIPLAYER_CONNECTING update (after NetInit) | NetPlayerData, text reload, **NetGameManager::Init `0x49c740`**, Twitter, NAT type, weapon preloads | `0x5ed6a0` only waits for the "net" package |
| `0x49c740` NetGameManager::Init | ClearNetPlayers, **two hash tables**, SetNetGame(0), recent-game history, playlist list | no PS4 code; nothing writes the tables |

## The crash: NetGameManager hash tables

`0x640fc0` (fault at `0x64100f`) is the game reset of PS3 `0x4a1be8`; the map pairing is right
(same `m_numTeams > 0` assert, line 1127 = `0x467`, same call to `0x641690` = PS3 `0x4a12e8`). The
name "AssignTeams (small)" comes only from its assert text. It clears two embedded HashTables:

| PS3 field | PS4 field | PS3 init | entries | buckets | node |
|---|---|---|---|---|---|
| ngm+6260 | **ngm+0x1ba0** | `0x4950f0(t, 16)` | 16 | 16 | PS3 76, PS4 0x60 (key +0x10, value +0x18, 9 qwords) |
| ngm+6304 | **ngm+0x1c10** | `0x494ee0(t, 256)` | 256 | 256 | PS3 48, PS4 0x38 |

PS3 reset clears A with `0x499418`, B with `0x499590`; PS4 inlines both into `0x640fc0`.
Users on PS4: insert `0x64d910`, lookup/remove `0x645065`, B at `0x645475`, plus ~20 inlined lookups
(`0x60ee6c`, `0x61133c`, `0x6281b5`, ...). Only the constructor `0x63d9e0` (zeroing) writes them.

HashTable 64-bit layout, from the inlined instance `0x5d9030` (256 x 0x58) and the readers:
`+0` buckets, `+8` free list, `+0x10` u32 node size, `+0x14` u32 pool count, `+0x18` pool,
`+0x20` sentinel node (= pool end), `+0x28` allocation, `+0x30` &buckets[nb], `+0x3c` s32 nb,
`+0x40` u8 ready. One allocation: `nb*8 + 8` (last bucket slot holds the sentinel), 16-aligned pool of
N nodes, then the sentinel node (next = prev = itself). Free list: node.next/prev linked, first prev
and last next null; buckets zero. nb = next power of two >= N (PS3 bit-smear of N-1, +1).
There is no callable PS4 HashTable::Init (every instance is inlined), so netinit.cpp builds it and
allocates through `0xb332a0(size, const int *ctx, align)` (rsi = context pointer, edx = align) with
context `0x14` when its heap slot is set, else `0x46`.

## Full list of missing state

| # | state | PS3 source | PS4 readers | init in netinit.cpp |
|---|---|---|---|---|
| 1 | ngm+0x1ba0 / +0x1c10 hash tables | `0x49c740` | `0x640fc0`, `0x645065`, `0x64d910` | built (above) |
| 2 | booster list `0x113a788` `{u32 count; rec* [64] at +8}` | NetInit -> `0x4554c0(0xf1b2c0)`: `*boosters*` (sid `0x916e03b0`, stride 0x20), kept if mask +0x18 passes the DLC check `0x4e8798`, max 63 | GetAmount `0x5f6410` (scan at `0x5f6750`), counts at `0x5fd8ef`, `0x659e14`, `0x5af04e`, `0x55b3e0` | filled from the dc table (non-asserting lookup `0xa15300`); every pack counts as owned; retried at match start if "net" was not loaded yet |
| 3 | session +0xc8 leave status = 1 (match `0x1065b08`, party `0x105efb8`) | Session::Init `0x4d1e2c` (PS4 copy `0x669e60` is dead) | LEAVING_GAME `0x5eb7d0`, `0x5ebeb0`, `0x5ec599`, `0x5f1b0d` | set to 1 |
| 4 | ngm+0x1ca2..0x1ca9 recent games = -1 | `0x49c740` memset(ngm+6382, 0xff, 8) | only shifted at `0x641619` | set, for parity |

Already handled elsewhere (not redone): network heap context 0x14 (session.cpp); playlists
ngm+0x20a0/+0x21a0 (menus.cpp); NpId netInfo+0x298 (menus.cpp); 128 KB buffer / bitstream
`0xf119b0` (cinema.cpp); session ctx/ids/members; trackers; connection records; "net" package request.

Checked and not needed:
- NGM::Init's ClearNetPlayers (`0x63db90`, alive) and SetNetGame(0) (`0x63dc30`, called by PAGE_LOBBY
  open); its zeroed fields are BSS/constructor zero.
- Session::Init other flags (+0xa8 read only on an error branch; +0xd0..+0xf0, +0xafc no reader);
  `0x4c3b00`, `0x4d5338`, `0x4d01a8`, `0x4d1ec8`, session manager `0x4d0fc0` (Matching2 only).
- netInfo +0/+4, +0x24/+0x28 (only copied into Matching2 search params), +0x2c NAT type (no reader),
  +0x38/+0x3c/+0x48 (zero skips the checks), +0x2c0/+0x2cc (UI counters).
- NetFriends `0x490178`, Twitter `0x4ef490`, invites `0x43b200`, stats `0x4e0564`/`0x44b030`,
  NetPlayerData `0x4c7820`/`0x4c5cf8`, DLC mask `0xf226c4`, text reload `0x929614`, weapon preload
  list (only `anim-proto-multiplayer` exists on PS4 and `0x641690` loads it), voice `0x4e71d8`.

## Byte checks

netinit.cpp does nothing unless these match: `0x640fe4`, `0x641012`, `0x6410e0` (node 0x60),
`0x64119e`, `0x6411d2`, `0x641290` (node 0x38), `0x5d90bb` (layout), `0x5ec590` (session +0xc8),
`0x5f6750`, `0x5fd8ef` (booster list), prologues of `0xb332a0` and `0xa15300`.

## Not verified

- Nothing was run. The booster mask offset (+0x18) is taken from PS3 (stride is the same 0x20 on PS4);
  if no record has a non-zero mask all records are listed.
- Whether any other removed-init state is read later in a match (after the reset) is not covered.
