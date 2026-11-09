# PORT-FRONTEND3: matchmaking, the vote screen, and the last frontend open items

Code: `gow3_pc/src/u2mp/frontend3.cpp` (`void install_frontend3(int &installed)`, header
`frontend3.h`), plus changes in `session.cpp` and `menus.cpp`. The lead adds the declaration
`void install_frontend3(int &installed);` to `u2mp_internal.h` and the call `install_frontend3(installed);`
to `install.cpp` (after `install_frontend2`).

Static analysis only (nothing was run). Addresses: PS4 1.02 image offsets, PS3 1.10 marked PS3. The PS3
reference is GOTY NPUA80698 1.10 only.

## 1. What was still open

From the function map (`functions-frontend-net.tsv`), every frontend row in state `stub ret` or
`partial` was checked against the C++ files:

| PS3 | PS4 | function | state before | now |
|---|---|---|---|---|
| `4b0a58` | `64e910` | PAGE_MULTIPLAYER_MATCHMAKING (matchmaking state machine) | ret | **frontend3.cpp** (section 2) |
| `4b0670` | `64e8e0` | LIST_MATCHMAKING_VOTE | partial (no 0x10a broadcast) | **frontend3.cpp**: same local count, plus the vote message |
| `439a88` | `5eba80` | bbb649ef, leader cancels the matchmaking countdown | partial (pop only) | **frontend3.cpp**: 0x115 to the members when party host, then pop |
| `439b04` | `5eba30` | bf6ce4d5, leader cancels the custom-game countdown | partial (pop only) | **frontend3.cpp**: same |
| `439b80` | absent | the members' popup handler for 0x115 | absent | member side of kLeaderBack (pops a message box page) |
| `43e99c` | `5eba40` | START_MATCHMAKING (cb 4055f45f) | partial (no 0x107, no `0xe15591`) | called as is; 0x107 = kMmStart sent by `matchmaking::leader_start` |
| `43a444` | absent | member side of 0x107 (Callback_Multiplayer) | absent | kMmStart: the member calls `5eba40` too |
| `4af160` | absent | vote broadcast helper | absent | `session::send_message` (the host relays) |
| `4b0700` / `4b0868` | absent | vote screen fill (member / host) | absent | `open_vote` / `start_vote` |
| `445020` | `5ed6a0` | PAGE_MULTIPLAYER_CONNECTING package preload `0xe155d0` | partial | **session.cpp** `preload_ps3_packages` (section 4) |
| `4386d0` | absent | MP teardown on a lost host (PAGE_MULTIPLAYER event 3) | absent | **session.cpp**: transport closed after the pump (section 5) |
| `43ea8c` | `5eb7d0` | PAGE_MULTIPLAYER_LEAVING_GAME | partial: waits for match+0xc8, which nothing set | **session.cpp** `finish_leaving_game` (section 6) |
| `4bc728` / `4bfac8` | `659bb0` / `659bc0` | party / player rows, image op | session.cpp had name only | **session.cpp**: rank badge (type 0x27) per member (section 7) |
| `43b698` | absent | net clock | session.cpp counted 60 Hz frames | **session.cpp**: 300 ticks per second (section 8) |

Already done elsewhere (no change): every other `stub ret` row of the map (`remaining.cpp`: party / player
/ friends / invites / clan tag / store / Twitter / cinema send-receive callbacks, the matchmaking box
24df6961 `64e900`; `session.cpp`: BUTTON_MULTIPLAYER, STARTMATCH, LEAVE_MULTIPLAYER, the roster queries;
`menus.cpp`: LIST_FRIENDS query, the join list; `cinema.cpp`: `5ecb80`), and the partial rows of
PORT-FRONTEND2.md. The three PS4 stubs `5eb7c0` / `5eba90` / `5ec470` are `blr` on PS3 too. The
leaderboard poll `64e060` (PS3 `4a9f78`, NP Score) and the NpConnectionManager wrappers `675200` /
`6752b0` stay unused: progression.cpp serves the boards, net::Transport the connections.

## 2. Matchmaking (PS3 `0x4b0a58`, net-matchmaking.cpp)

### 2.1 PS3

Start Match in matchmaking mode (`0x43aab0`): the leader sends party mailbox 0x114 and shows a 5 s
"Starting in [A]..." popup (`0x42a0b8`, text `73a73dd8`, done cb 4055f45f, cancel cb bbb649ef). Done ->
START_MATCHMAKING `0x43e99c`: mailbox 0x107 to the party, menu reset, NET_SM 19,
ShowPage(PAGE_MULTIPLAYER_MATCHMAKING `e35f0f1a`), `[0xe15591] = 1`. Members follow on 0x107
(`0x43a444`). The page handler `0x4b0a58` (0x34e8 bytes):

- event 0 (page closed): SetNetGame(-1), flags `0xf22638/9` and the vote block cleared, `0x4af240`
  (publish the player info; PS4 `64e810`, progression.cpp).
- events 2 / 3 (back): "Leave Matchmaking?" (`0x42a3e8`: cca090f2, yes 0260a263 -> cb 24df6961 =
  `0x4af4fc`, sets `0xf22638`; no 075bc82d -> 2a2d147e) and the back is held (`+0xc` = 1) until answered.
- event 6: mailboxes first (0x108, 0x103, 0x109 result, 0x112 vote options, 0x10a vote), then a switch on
  NET_SM 3..35 (jump table `0x4b0cb8`): Matching2 search (`0x4d3c50`) -> 5 GAME_LIST_WAIT -> 6 PICK ->
  JoinRoom -> 7 / 9 / 14 -> 17 CLIENT_LOBBY / 21 CLIENT_VOTE_SCREEN; none found -> CreateJoinRoom ->
  3 -> 16 SERVER_LOBBY ("GATHER": waits by player count, table `0xb40710`, "time out %i players"), then
  `0x4b0868` (two games from the playlist via `0x49bdb8` = PS4 `64a440`, mailbox 0x112, deadline
  clock + 4500 = 15 s, netInfo+0x34 = 2) -> 20 SERVER_VOTE_SCREEN; the host sends the winner (0x109:
  game, start time; `0x4a12e8` level preload) and netInfo+0x34 = 3; at 2 s left (`0x4b354c`) the host
  plays the game task (`0x43a398` / `0x7e7a74`).

### 2.2 What PS4 kept

All of the page's widgets: status (cond 89, `65a900`: phase netInfo+0x34, deadline netInfo+0x20 in
net-clock ticks, texts b3911b0d "Searching for players...", 747a57c3 "Voting ends in [A]...", 47bb9c98
"Game starting in [A]...", tips DC `b25ab071` indexed by `0x113ddac`, the playlist name and picture),
the vote rows (cond 90, `65ae50`: vote block `0x113db50`, map name / game type / map picture type 0x22 /
counts / "rows enabled while my vote is unset"), the player rows (cond 6..15), START_MATCHMAKING
(`5eba40`) and Leave::Enter's matchmaking return (`5f00d0`: push PAGE_MULTIPLAYER_MATCHMAKING,
netInfo+0x34 = 1, NET_SM 16 host / 17 members). The machine itself, the vote broadcast and the
mailboxes are gone.

### 2.3 PC port

The party is the match: there are no strangers to search for, so the Matching2 part (states 3..14) has
no PC counterpart. The rest is the PS3 flow over session messages (`session::send_message`; type numbers
from `session::kFrontendMessages` = 0x10, forwarded by session.cpp):

| PC message | PS3 mailbox | direction | content |
|---|---|---|---|
| kMmStart 0x10 | 0x107 | leader -> members | follow into matchmaking (`5eba40`) |
| kMmReady 0x11 | (room join) | member -> host | my page is up |
| kMmVoteOptions 0x12 | 0x112 | host -> members | s16 game a, s16 game b, u32 ticks left |
| kMmVote 0x13 | 0x10a | voter -> host, host relays to the others | u8 choice |
| kMmResult 0x14 | 0x109 | host -> members | s16 game, u32 ticks left |
| kMmLeave 0x15 | 0x103 / leave room | any | matchmaking ends for the party |
| kLeaderBack 0x16 | 0x115 | leader -> members | the leader cancelled |

Host (party leader): Start Match (mode 1) -> `matchmaking::leader_start` (session.cpp `start_match`
calls it when the port is installed) -> kMmStart + `5eba40` -> NET_SM 19 -> 16 (gather: phase 1,
netInfo+8 = 1 so the status reads "Searching for players..."; at least 3 s, until every member is
ready or 10 s) -> vote (NET_SM 20, phase 2, 15 s): two different `*net-games*` records of the chosen
playlist's game types (`menus.cpp playlist_game_types`; DLC maps only when imported) -> result: the
higher count wins, a tie is random; SetNetGame `63dc30`, vote closed, phase 3, 10 s -> at 2 s left
`session::create_match` (the custom-game create path: host page `6093f0`, kFollow for the members),
netInfo+0x34 set to 3 again after it (host_create clears it; Leave::Enter's ranked stats and the
return to this page read it).

Members: NET_SM 19 / 17 -> kMmReady; status shows the tips (cycled every 8 s as PS3 `0xf22630`); vote
options -> NET_SM 21 (CLIENT_VOTE_SCREEN); result -> SetNetGame, phase 3; the host's kFollow takes
them into the match as before.

Back: "Leave Matchmaking?" as PS3, but the yes button calls our own callback id
`sid("U2MP_MATCHMAKING_LEAVE")`, registered in the menu callback table (menu +0x18ec count, +0x18f0
entries of 0x18 `{id, fn, arg}`; dispatcher `5cb8d4`) when the page is shown: 24df6961 is
remaining.cpp's. Yes -> the party leaves matchmaking (kMmLeave): vote block and phase cleared, NET_SM
18, the page is popped (back to PAGE_LOBBY). PC difference: on PS3 a member could leave the room
alone; here the party is the match, so anyone leaving ends matchmaking for all.

Page events on PS4: the handler gets `{u32 event, u32 page, u64 arg, u8 handled (+0x10), u8 (+0x11)}`
(dispatch loops `5c7d03`, `5cd47c`); PS3 `+0xc` / `+0xd` are PS4 `+0x10` / `+0x11`. Event 0 is "page
closed" (PS4 PAGE_MULTIPLAYER `5eb323` sets party+0xb50 there, PS3 left the party there), event 1
"shown" (`5eb1be` resets the mode).

After a matchmaking match: Leave::Enter pushes the page with phase 1 and NET_SM 16 / 17; the port
sees phase 3 -> 1 and starts the next round (the PS3 room voted on the next game the same way).

Log lines: `matchmaking: started (host|member)`, `NET_SM n, phase p`, `vote between net games a and
b`, `voted for net game g`, `votes x / y, net game g wins`, `starting net game g`, `back from the
match, next round`, `leaving`.

## 3. LIST_MATCHMAKING_VOTE (`64e8e0`, PS3 `0x4b0670`)

PS4 kept `+4 = row != 0; count[+4]++`. The port does the same (only while the vote is open and not yet
voted, as the rows' enable element 17809498 shows them) and sends kMmVote. Checked before patching:
`cmp dword [rdi+4],0; setne al; movzx eax,al; lea rcx,[0x113db50]`.

## 4. Package preload (PS3 `0x445020` at `0x4454ac`)

PS3 CONNECTING requested every package of the list at `0xe155d0` that was not loaded yet (`0xaf220`),
with the same flag as the "net" request (PS3 package +0x10 |= 0x200 << 32; PS4 `5ed843`: +0x18 |= 0x40).
The list (21 names): beretta-pistol, beretta-pistol-magazine, colt-defender-pistol,
colt-defender-magazine, desert-eagle, desert-eagle-magazine, dragunov-magazine, dragunov-rifle,
fn-fal-clip, fn-fal-rifle, m4-rifle, m4-rifle-magazine, shotgun, spas12-shotgun, machine-gun,
machine-gun-clip, rpg, anim-proto-multiplayer, anim-proto-shield, anim-proto-carry-objects,
m32-grenade-launcher. The PS4 image has no copy (only "colt-defender-magazine" occurs as a string, and
nothing points at it); all 21 packages are in the PS4 data (`pak22_ps4.txt`, `actor27/ps4`).
`multiplayer_button` now requests them after the net package (RequestPackage `56ad90`, flag 0x40), once
per entry, skipping loaded ones. Log: `package preload (PS3 0xe155d0): N requested, N already present,
N not created`. `UNCHARTED_U2MP_PRELOAD=0` turns it off (memory test).

## 5. Host lost, then Multiplayer again (PORT-FRONTEND2 open item 1)

`on_peer` (host gone) now also sets `lost_host`; the next `tick` closes the transport, detaches the
peers, stops voice and sets `active = false`, like the kick path (not inside the pump). netInfo+0x1c
stays set, so the pages show the error box as before; the next BUTTON_MULTIPLAYER starts a new
transport and clears the error.

## 6. LEAVING_GAME (`5eb7d0`, PS3 `0x43ea8c`)

The page waits for match+0xc8 (PS3: `0x4d26b0(match) != 1`, the Matching2 room left by `0x4d8540`,
which "Leave Game" `0x44026c` and the error paths started). PS4 lost the LeaveRoom and nothing set the
byte: the page never closed (in-match Leave Game -> cb 2e1a9a7b `5eb080`, the client wait page `609d00`
on errors, Leave::Enter's error path). `session.cpp finish_leaving_game` (every frame): while the top
page is PAGE_MULTIPLAYER_LEAVING_GAME `b808d03c`, set match+0xc8 = 1 and netInfo+0x34 = 0 (the PC
returns to the lobby; the party stays together). The game then clears the players, pops, NET_SM 18.
The byte is cleared again when a match is created or joined (`create_match_now`, kFollow). It runs from the
per-page handler before the `active` check too, so the page also closes after a lost host closed
the transport.

## 7. Rank badge in the party / player rows

PS3 `0x4bc728` (party) and `0x4bfac8` (players), op 3 image 0 (+4 == 0): type 0x27 (rank badge), value =
the member's rank (room member data +8). Ported in `roster_query`: op 3, +4 == 0 -> type 0x27 at +8,
rank at +0x18 (PS3 +16), the rest as PS3 (+0x24 = +0x2c = -1). The rank travels in the session
messages: kInfo `{len, name, rank}`, kRoster entries `{slot, len, rank, name}` (format change; both
PCs need this build), refreshed every 2 s when it changes. Local rank: PS3 `0x0cfa58` over
NetPlayerData +0xd8 (cash) and the rank table `c85e199d` (as progression.cpp). Image 1 (the cellVoice
talking icon) and op 17 (team colour): ported, PORT-FRONTEND6.md 3.

## 8. Net clock rate

`[0x113a040]+0xc` counts 1/300 s: PS3 "Net Clock = %f" multiplies by 1/300 (`0xdb04c0`), PS4's readers
too (matchmaking status `65aab5`, constant `0xe62628`), remaining.cpp uses 300. session.cpp counted
game frames (60/s), so every countdown read from the clock ran 5x slow. Now 300 ticks per real second
(steady clock) from the clock's creation. `session::net_ticks()` returns it (also before the clock
exists).

## 9. Not ported (updated 2026-11-09, PORT-FRONTEND6.md)

- ~~The 5 s "Starting in [A]..." popup~~: **ported** (frontend6.cpp). PS4 kept the popup page and its
  update; only the opener `0x42a0b8` and its callers were gone. PORT-FRONTEND6.md 1.
- ~~The level preload during the vote countdown~~ (PS3 `0x4a12e8` = PS4 `641690`): **ported** at the four
  PS3 call points of the vote (frontend3.cpp `open_vote` / `apply_result`). PORT-FRONTEND6.md 2.
- PS3 member data shown in the vote / player rows that came from Matching2 (ping, bandwidth): not
  needed (no Matching2 room).
- party+0xb50 (PS3 `0x4c36c8`, "member leaves the party"): not needed on PC; reason in PORT-FRONTEND6.md 4.
- Image type 0x23: its only PS3 producer is the MOTD query (cond 96, PS3 `0x4ba660` at `0x4ba760`), whose
  page (server message of the day) is never shown on PS4 / PC and whose PS4 query (`65e2e0`) no longer
  produces it. Nothing to port.
