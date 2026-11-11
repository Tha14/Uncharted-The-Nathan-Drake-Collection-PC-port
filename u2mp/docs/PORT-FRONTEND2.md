# PORT-FRONTEND2: the "partial" frontend functions

Code: `gow3_pc/src/u2mp/frontend2.cpp`, `void install_frontend2(int &installed)`. The lead adds the
declaration to `u2mp_internal.h` and the call to `install.cpp`.

The function map (`functions-frontend-net.tsv`) marks these PS4 functions "partial" because they are
smaller than their PS3 versions or keep fewer of the PS3 callees. Each one was compared with its PS3
1.10 counterpart. The PS3 address comes first in each pair. Most of the size difference has two
causes:

- **Inlined callees.** The PS4 compiler inlined small functions: IsHost `0x4d2394` (`+0x10 != 0 &&
  +0xb00 == +0xb04`), the tracker accessors `0x4919b0`/`0x4919d4`, the net-game, level and game-type
  record lookups `0x499988`/`0x491f38`/`0x492058`, the menu-entry lookup `0x4b7b3c` and the image
  lookup `0x8a015c`. PS3's sprintf-to-static `0x40bb24` became PS4 `0x5a8420` (buffer `0x14cd510`),
  and TextDb `0x9294c4` became PS4 `0xcddc00`.
- **Debug output.** PS3 code that only sets the print channel (`0x9238f8`) and prints (`0x923f14`)
  was dropped.

The other dropped calls are PSN, Matching2 or party-mailbox code. On PS4 nothing receives those
messages any more. On PC, `session.cpp` plays that role.

No PS4 address on this list was referenced in the other `u2mp/*.cpp` files before this port. Some
functions run inside flows that other files own; the table notes them.

| PS3 | PS4 | what | outcome |
|---|---|---|---|
| `438b3c` | `5ed8d0` | callback 705e651e | Same as PS3: ShowPage(`ed61e469`). Nothing to do. |
| `438b80` | `5eb790` | callback 2c2f1bb7 (BUTTON_MULTIPLAYER_LEAVE_GAME) | Same as PS3: ShowPage(`c89b86fc`). Nothing to do. |
| `439a88` | `5eba80` | callback bbb649ef | **Ported in frontend3.cpp** (0x115 as a session message). PS4 only pops the page. PS3 first did this when IsHost(party): `0x4c38f0(277)` (mailbox 0x115 to every party member) and `0x4d5338(party,0)` (Matching2 hide room). The 0x115 receiver is the PS3 page handler `0x439b80` (pops the member's page), which is absent on PS4. No PS4 code compares 0x115. Not ported (see open items). |
| `439b04` | `5eba30` | callback bf6ce4d5 | Same as bbb649ef (**frontend3.cpp**). |
| `43a3dc` | `5ebdb0` | callback 8e2767d9 | PS3 also set the print channel and printed "Replay". Otherwise the same: PlayTask(LookupTask(`9f2aeafb`,`ad76460e`)). Nothing to do. |
| `43d76c` | `5efd30` | TaskManagerOnline event helper (events 6ba8328a / bcc3b05b / 15f1cf05) | Complete. The tracker loop of `0x4919b0`/`0x4919d4` is inlined (ngm+0x10 stride 0x2c0, local = [ngm]). Nothing to do. |
| `43e99c` | `5eba40` | NET_SM -> START_MATCHMAKING | PS4 keeps the menu reset, state 19 and ShowPage(PAGE_MULTIPLAYER_MATCHMAKING `e35f0f1a`). Dropped: the party message 0x107 (`0x4c38f0(263)`), the debug print, and `0xe15591 = 1`. Only the matchmaking state machine `0x4b0a58` reads that byte, and on PS4 that machine is `ret` (`64e910`). **Ported with the machine (frontend3.cpp)**: kMmStart = 0x107. |
| `43ea8c` | `5eb7d0` | PAGE_MULTIPLAYER_LEAVING_GAME | Equivalent, but it waits for match+0xc8, which nobody set on PC: **session.cpp now sets it while the page is up** (PORT-FRONTEND3.md section 6). PS3 `0x4d26b0(match) != 1` is PS4's `match+0xc8 != 0`. The rest is the same: ClearNetPlayers, pop, then state 19 if netInfo+0x34 is set, else 18. Nothing to do. |
| `445020` | `5ed6a0` | PAGE_MULTIPLAYER_CONNECTING | session.cpp bypasses this page: `multiplayer_button` requests "net" and shows PAGE_MULTIPLAYER. PS4 kept the error box and the "net" package wait. Dropped (PSN, not ported): EULA (`sceNpEulaCheckEulaStatus`), NAT (`cellNetCtlGetNatInfo`, into netInfo+0x2c), NP comm id `0x4ec120`, and the Twitter init `0x4ef490` (Twitter is a message box on PC). Also dropped: a **package preload loop** over the name list at PS3 `0xe155d0` ("beretta-pistol", ...): each package not yet loaded is requested (`0xb3eec`) and gets flag `0x200<<32`. After that, ShowPage(`d6f98d11`), an id that does not occur in the PS4 image. The preload is now in session.cpp (`preload_ps3_packages`). |
| `4465c8` | `5eb1a0` | PAGE_MULTIPLAYER | Event 1: PS3 also sent mailbox 0x110 (mode 0) to the party. session.cpp `tick()` already broadcasts every mode change. Event 0: PS3 hid the room (host, Matching2) or left the party (`0x4c36c8`); PS4 sets party+0xb50. On PC, LEAVE_MULTIPLAYER (session.cpp) does the teardown. Event 6: PS3 also read the PSN friends' presence (sceNpBasic*), plus a debug auto-start timer (`0xe641f1`, `0xf12278` -> STARTMATCH). PSN and debug only, so not ported. PS3 also had **event 3**: when netInfo has an error, the teardown `0x4386d0`. PS4 lacks it; session.cpp now closes the transport when the host is lost (open item 1). |
| `4654d0` | `609640` | PAGE_MULTIPLAYER_JOIN | Equivalent. PS3 "search failed" (`0x4d2410 == 2`) is PS4 `match+0xa8 && +0xac < 0`: pop, state 18. Only `6095d0` reaches this page, and that control is on no PS4 page. Nothing to do. |
| `4655b0` | `6095d0` | BUTTON_MULTIPLAYER_JOIN (custom game list) | PS3 also started the Matching2 room search (`0x4d23d4`/`0x4d23dc`/`0x4d3c50`). On PC the join is by address and the list query (cond 3, `menus.cpp`) is empty. The control is on no PS4 page. Not ported. |
| `465808` | `609680` | BUTTON_MULTIPLAYER_SELECT_JOIN_GAME | **Replaced** (frontend2.cpp). PS3 took the chosen search result (`0x4d5940`), filled the join request (netInfo+0x60..0x80), computed the distance and activated signaling (`0x4e9060`), then state 7. PS4 reads the entry from an uninitialised stack array (`[rbp+row*0x50-0x608]`), dereferences it, and sets state 7, which JOINING can only fail. The list has no rows, so this should never run. The C++ version only logs. The patch first checks the prologue bytes and the array-read instruction. |
| `4661e0` | `609790` | PAGE_MULTIPLAYER_JOINING | session.cpp uses it: `kFollow` goes to state 12, mailbox 0x10e, then 14 and 17. States 12 and 14 are intact. The Matching2 join of states 7-10 (connect, slot reservation 0x101 with its timeout) is gone. It is not needed because the PC never sets 7. Not ported. |
| `4b0670` | `64e8e0` | LIST_MATCHMAKING_VOTE | PS4 records the vote locally (`0x113db50`: +4 choice, +0x10/+0x14 counts). Dropped: the broadcast of mailbox 0x10a {room = PS3 netInfo+0x58, vote} to the match members (`0x4af160`). The tally belonged to the absent matchmaking machine `0x4b0a58`. **Ported in frontend3.cpp** (kMmVote, the host relays). |
| `4b9a88` | `655cf0` | mode label lookup (table PS3 `0xb41210` / PS4 `0xdef390`) | Equivalent. Row 5 "%s (%i%%)": PS3 called `0x4c6fd0(NetPlayerData)`; PS4 inlines the same ratio over 23 `0x662090` entries. Nothing to do. |
| `4bc310` | `65ecc0` | mode label list (entry table `0x1041560`) | Equivalent. The menu-entry and image lookups are inlined. "%s%s" and "%s%s \|@CFFFF8040\|%i" use netInfo+0x18. Nothing to do. |
| `4bd2c4` | `656fe0` | "%i %s" (CUSTOMTIMELIMIT, cond 61) | Equivalent. The game-type lookup and IsHost are inlined. Nothing to do. |
| `4bf768` | `65ae50` | "%s %s" (matchmaking vote rows, block `0x113db50` = PS3 `0xf21dcc`) | Equivalent. The lookups are inlined. Its data is empty until something fills the vote block, which on PS3 was the matchmaking machine. |
| `4edea0` / `4f1208` | `675b30` / `676d30` | Twitter | Skipped. Twitter is a PC message box (remaining.cpp). |

## Open items for the lead (resolved 2026-11-08, PORT-FRONTEND3.md)

1. **Host lost or kicked, then re-entering Multiplayer.** Done in session.cpp: a lost host now closes
   the transport after the pump, as the kick path does (PORT-FRONTEND3.md section 5).
2. **Party leader backs out (bbb649ef / bf6ce4d5).** Done in frontend3.cpp: the party host sends the
   PC message kLeaderBack (PS3 0x115) before the pop; a member pops its message box page (the PS3
   popup handler `0x439b80`). The PS3 "Starting in" popup that offered these buttons has no PS4
   counterpart, so on PC they are not reached in normal play.
3. **Package preload (CONNECTING).** Done in session.cpp (`preload_ps3_packages`): the 21 names of PS3
   `0xe155d0`; PS4 has no copy of the list, all 21 packages exist in the PS4 data.
4. **Matchmaking.** Done in frontend3.cpp on session.cpp's transport: the page handler `64e910`
   (PS3 `0x4b0a58`), the vote callback `64e8e0` with its broadcast, START_MATCHMAKING `5eba40` with
   the party message (PS3 0x107). `0xe15591` itself is not needed (the port keeps its own state).


## Leaving multiplayer without Leave Multiplayer (2026-11-11)

PS3 always ran the teardown `0x4386d0` on the way out of multiplayer (score term `0x4ab66c`, cinema abort
`0x464148`, party reset `0x4c3ac0`, session manager term `0x4d1a00`, sockets, signaling `0x4e8afc`, voice end
`0x4e7038`, net player data freed `0x4a9918`, NET_SM 0). On PS4 closing PAGE_MULTIPLAYER by another way (Back,
or the main menu replacing it) left NET_SM at 18, and a campaign started next asserted in NetEventPlayVox ("no
me NetPlayerTracker"). session.cpp: `teardown(pop)` is the shared body of LEAVE_MULTIPLAYER; `page_event`
runs it when NET_SM is 18 (idle party) and PAGE_MULTIPLAYER (`53cbb5f2`) has not been on the menu page stack
(menu +0x9d0 pages, top index +0x1010, page id +0x10) for 30 frames. Log: `PAGE_MULTIPLAYER closed without
Leave Multiplayer: multiplayer teardown (PS3 0x4386d0)`.
