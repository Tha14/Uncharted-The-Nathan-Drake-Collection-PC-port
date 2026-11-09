# PORT-FRONTEND6: "Starting in" popup, vote level preload, voice icon and team colour in the party rows

Static analysis only (nothing was run). PS4 1.02 image offsets; PS3 = GOTY NPUA80698 1.10. Code:
`gow3_pc/src/u2mp/frontend6.cpp` / `frontend6.h` (new, `install_frontend6`), changes in `session.cpp`,
`frontend3.cpp`, `voice.cpp` / `voice.h`, `menuimages.cpp`.

Install: `install_frontend6(installed);` after `install_frontend3(installed);`, declaration
`void install_frontend6(int &installed);` in `u2mp_internal.h` (or include `frontend6.h` in install.cpp).

## 1. "Starting in [A]..." (PS3 0x42a0b8, PORT-FRONTEND3.md 9 / MENUS.md 5)

PS3 Start Match `0x43aab0` sends party mailbox 0x114 and opens the timed popup `0x42a0b8(menu, 73a73dd8, 5,
done, cancel, fn, arg)`: matchmaking done `4055f45f` / cancel `bbb649ef`, custom game done `e2707dbe` /
cancel `bf6ce4d5`. Members on 0x114 (`0x43a444` -> `0x43a8ec`) open it with done `ee8f5f9b` (blr on both)
and no cancel. PS4 kept the whole popup page and its update; only the opener and its callers are gone:

| PS3 | PS4 | |
|---|---|---|
| menu+2704 / +2708 done / cancel | menu+0x18d4 / +0x18d8 | read at `0x5cfbed` (deadline) and `0x5caf6c` (back control `128aa55b`) |
| desc [menu+2624], +4 text | [menu+0x1880], +8 | page built at `0x5c7981` (widget `0x57a600` gets the desc) |
| `0x40e348`(desc, s): +44 now, +48 deadline | `0x5ae8a0`: +0x40, +0x44 | `0x5cfb85`: frame `[0x1116180+0x104]` > deadline |
| page records menu+720, id +12, +209 back off | menu+0x370 (count +0x9b0), id +0x10, +0xf1 | CanGoBack `0x5a33c0` reads +0xf1 |
| list menu+4256 / +4260, 12-byte {page, fn, arg} | +0x24f0 / +0x24f8, 0x18 bytes, 32 | fn NULL is skipped (`0x5c7d20`, `0x5cd4a3`) |
| close: 0x40e158, pop, 0x426dbc(done) | `0x5cfb64..0x5cfbf9`, DoCallback `0x5c8840` | registry dispatch `0x5cb8d4` |

`frontend6.cpp` ports `0x42a0b8` and the two callers: session.cpp `start_match` calls
`frontend6::leader_starting(matchmaking)` (popup + session message `kStartingMessage` 0x17 = mailbox 0x114);
members open it from frontend3.cpp's message handler. The done ids reach the callback registry
(menu+0x18ec / +0x18f0). Its registration names `0x5eb830` for `e2707dbe` (the PS4 create path that reads
the NULL room-attribute list, test run 3) and `0x5eba40` for `4055f45f` (START_MATCHMAKING without PS3's
mailbox 0x107), so the two `lea rcx` of the registration (`0x5ef31a`, `0x5ef41e`) now load ours:
`e2707dbe` -> `session::create_match`, `4055f45f` -> `matchmaking::leader_start` on the party host (else the
original `0x5eba40`). Cancel stays frontend3's `leader_back` (0x115 + PopPage, which also closes the
overlay); a member pops the popup on 0x115 (`frontend6::popup_up`, overlay menu+0x9b8).

Without the install checks (15 byte checks, both registration leas), Start Match starts at once as before.
Log: `starting popup: ...`. Text `73a73dd8` is in the PS4 common bank.

## 2. Level preload of the matchmaking vote (PS3 0x4a12e8 = PS4 0x641690)

`0x641690(ngm, request, keep_menu, flags_only)`: request queues the net game's levels (level manager
`0x1130390`, `0x56c0e0`, list reset at `0x6418a1`) and DC modules (`0xa16bb0`), else releases them;
keep_menu also queues "menu" / "menu-temple" and writes ngm+0x209d (never read); flags_only skips the net
game part. PS4 kept two of the eight PS3 callers (`0x5e9a9b` = PS3 `0x436014`, LoadLevels `0x64148c`). The
lost ones belong to the matchmaking machine: vote fill `0x4b0840` / `0x4b0a28` (1, 1, 1) and the winner
`0x4affe0` (host, after 0x109) / `0x4b1fd4` (member, on 0x109) (1, 1, 0). frontend3.cpp now calls them in
`open_vote` and `apply_result`. Not added: PS3 `0x43895c` (0, 1, 0) is the multiplayer frontend start, not
the matchmaking leave; leaving after the result leaves the levels queued until the next LoadLevels resets
the list, as on PS3.

## 3. Party / player rows: voice icon (op 3 image 1) and team colour (op 17)

PS3 roster query `0x4bc728` (jump table `0x4bc830`, resolved from TOC `0xe183b8`):

- **image 1** (`0x4bca08` / `0x4bcb38`): a member with a voice port shows `general/net/mic/default`
  (`ffa98ec5`) while cellVoice reports energy (attribute 1001); a muted member `general/net/mic/mute`
  (`7a5f5bf1`); else nothing. Type 3 image. Now: `voice::talking(net_id)` (voice arrived in the last 300 ms;
  the local member: a frame was sent) and `voice::muted(net_id)`, written as PS4 writes a type 3 net image
  (`0x655f11`: +0x1c {id, selector 1}, +0x24 -1).
- **op 17** (`0x4bc934`): only PAGE_LOBBY's rows (table arg 10..19) of a custom game (netInfo mode 2) whose
  game type is not free for all (`0x4a0bcc`: types 4, 8, 9, 12 = the types the PS4 team carousel hides,
  `0x657945`), from the member's team (room member data +9: 1 / 2) through GetTeamColor `0x495cf0` = PS4
  `0x64aeb0(out, ngm, team - 1, 0)` (colour at out+0xc): (colour & 0xffffff) | 0x20000000 into the query
  colour (PS3 +44, PS4 +0x3c, default set by the dispatcher `0x660707`). The team is each PC's
  NetPlayerData +0x114 (the PAGE_LOBBY team carousel writes it at `0x657a6b`): client -> host as one more
  byte of kInfo, host -> clients as session message kTeams (6) `{slot, team}...` (the roster format is
  unchanged; an older build ignores both).

## 4. Checked, not ported

- **party+0xb50** (PS3 `0x4c36c8`: party+4468 = 1, "leave the party", read only by the NP party layer):
  set by PAGE_MULTIPLAYER event 0 and by the in-match Leave Game `0x5eb080` (PS3 `0x44026c`) of a member.
  PAGE_MULTIPLAYER closing is LEAVE_MULTIPLAYER on PC (session.cpp tears the party down). For the in-match
  Leave Game the PC keeps the member in the party on purpose (PORT-FRONTEND3.md 6): the PC party is the
  transport to the leader, there is no NP layer that would give the member a party of its own, so leaving
  it would strand the member on an error box. Not needed.
- **Actor package preload** (PORT-FRONTEND4.md 4, `preload_ps3_packages`): RequestPackage `0x56ad90`
  creates nothing for weapons / anim-protos because on PS4 they are actor packages that the level loads
  (`0x56d170` from LoadLevels and the registry's actor lines; every actor line of the 84 MP / co-op levels
  resolves, IMPORT.md). The PS3 loop only kept them resident between matches. Not needed; the log line stays.
- **Matching2 member data** (ping, bandwidth in the vote / player rows): no Matching2 room. Not needed.
- **Menu image overlay** (MENUS.md 7): PS3 drew a second element after the 0x22 / 0x24 / 0x25 picture when
  the descriptor float +0x20 (PS3 +24) is non-zero (`0x41ecec` -> `0x8b0920`, `0x41f5e0` -> `0x8b1540`).
  Every PS4 producer writes 0 there (`0x656397`, `0x657fe1`, `0x658338`, `0x65ac49`, `0x65afa9`): never
  reached. The **0x25 bottom edge** is ported (menuimages.cpp: the picture ends 50 units above the box,
  PS3 `0x41f4f4`, constant `[0xdb7690-32120]` = 50.0).

## 5. To check in game

- Start Match (leader): "Starting in 5..." counts down, then matchmaking / the match; Back cancels on both
  PCs. Member: the same popup, no Back.
- Matchmaking: `level preload (PS3 0x4a12e8)` lines at the vote and at the result; the match then loads.
- Party rows: mic icon while someone talks, mute icon for a muted member; team colours in a custom team game
  after choosing a team on PAGE_LOBBY (both PCs need this build for the team byte).
