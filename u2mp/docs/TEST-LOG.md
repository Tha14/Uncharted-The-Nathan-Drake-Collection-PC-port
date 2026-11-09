# u2mp in-game test log

## 8 November 2026 (single PC, host alone, saves copied into a test profile)

| Run | Result | Fix |
|---|---|---|
| 1 | session hooks not installed: gamenet checked the connection records before their static constructor ran | the check moved to attach() |
| 2 | Multiplayer opened, stuck on "Connecting..." (the net package request never reached the loader); the call hooked at 0x3f8280 is not per frame | the button requests "net" itself (0x56ad90); per-frame work from a page handler for every page (event 6) and the match states' Update |
| 3 | frame hook 60/s, NET_SM 18, net.pak loaded, Multiplayer page shown; Start Match crashed in 0x5eb830 (room-attribute list 0x14ce9b8 is null on PS4) | host_create() in C++ without that pick |
| 4 | states 18 -> 3 -> 10; crash in the allocator: memory context 0x14 (network heap) has no heap on PS4 | context 0x14 shares the default heap |
| 5 | states 3 -> 10 -> 16 (PlayTask "deathmatch"); crash at 0x64100f (AssignTeams area) called from 0x9bddc3 | open: next bug-fixing round, after the menus and assets |
| 6-7 | (with the PS3 import) Multiplayer, 11 playlists, both map packs offered; states 3 -> 10 -> 16; crash in 0x640fc0 (NetGameManager match reset): its two hash tables were never built on PS4 | netinit.cpp builds them (PS3 NetGameManager::Init 0x49c740) |
| 8 | crash again in 0x640fc0: the tables' memory was reused (context 0x14 had been pointed at the thread's scratch heap) | netheap.cpp: context 0x14 gets a heap of its own |
| 9 | no crash: tables clean up to state 16 (the match task); the game then went quiet until closed | open: what the screen showed |
| 10-15 | menu rounds: icons, playlists, DLC cards, texts | MENUS.md |
| 16 | match start: strlen/memcpy of a NULL host name in the NetStatGame init (0x66c75f) | remaining.cpp: the host name |
| 17 | map loads, black screen | online-state log (session.cpp) |
| 18 | EstablishConnection -> LoadLevels, crash on an empty tag table (0x4bf48b) | empty table = not found (0x4bf44c) |
| 19-21 | LoadLevels waits forever: the level `<map>-pak` (server spawn data) is missing | skip flag on that entry; later the imported spawn paks |
| 22 | Treasure loaded fully, then Leave | leave-reason log |
| 23 | Multiplayer button dead: near-image stub memory ran out, 63 functions not installed | one shared near_stub pool (install.cpp) |
| 24 | Leave after LoadLevels: level loading clears the player list (host tracker NULL) | players registered again during setup |
| 25 | NULL+8 at 0x6405af (removed room-attribute read) | read the session block (0x640598) |
| 26 | Profile page draws only up to the Level widget | 2D command arena 192 KiB -> 2 MiB (menudraw.cpp) |
| 27 | net clock 0x113a040 NULL at 0x5f276c | session.cpp creates it |
| 28-31 | NULL+0x9a0 at 0x662fd5, then "Missing spawner" | spawn paks imported from the server data, spawn.cpp lookup |
| 32 | dynamic vertex stream assert on the Profile page | streams enlarged, quad guard |
| 33 | DC symbol 0x8cf8e1cc (online weapon set) missing | netpatch.cpp loads net-patch9 |
| 34 | Playing on Gompa; rdx=-1 at 0xad34f3 (online weapon settings record) | online map 0x837a26b6 -> PS4 offline map 0x974cf651 |
| 35 | Playing on Shambhala; first frame: unresolved shader resource at 0xc12173 | bind guard (nullsites.cpp) |
| 36 | same entry read again at 0xc25ddb; the entry is raw ids (mesh-instance parameter reference never linked) | skip entry |
| 37 | another render path (0xc04403) reads the same entry | repair entry with a zero block |
| 38 | repair ran (1 entry); draw path 0xc30ec0/0xc04340 needs a real resource (NULL at 0xc04403) | root cause handed to the render port (PORT-RENDER.md) |

From run 38 on, fixes are batched (render root cause, remaining gameplay/menu functions, assets) before the next test.

## 9 November 2026 (batch test)

| Run | Result | Fix |
|---|---|---|
| 39 | first run without a crash: Gompa reached Playing (no render repairs needed), Leave about 5 s later, back to the menu cleanly; the Fort then waited in LoadLevels for `sky-fort-multi` and `the-fort-multi-wide` | Fort pieces imported again (group B); the Gompa leave reason is open |
| 40 | Gompa: the match-over byte 0x104f1c0 is 1 in the image data (cleared only by Leave::Enter), so the first match left at once; the Fort (second match, all pieces) reached Playing, then GetAllocator(0x45) asserted in the net player sort (0x66d413) | byte cleared at match setup (session.cpp); scratch arena for the sort (nullsites.cpp section 3) |
| 41 | no crash, 7.5 min: Gompa, Fort, Treasure played 1-2.5 min each and left by Leave Game; unlocks, 107 skins and 32 boosters loaded; the player character was not visible | spawn log, then the converted-skin draw (run 42) |
| 42 | no crash: the Fort with a 60 s test limit ran to game over, ShowResults with its 10 s countdown, back to the lobby; player skin `mp-hero-year-1` resolved and loaded (import build) but not drawn; loading screen not installed (cinema already wraps WaitReady::Exit); text sid("none") looked up | loading screen chains earlier hooks; "none" is empty text; converted-skin draw handed to the render port |
| 43 | Fort/Shambhala match to game over; hero mesh instances created, 0 submits; skin year-1 | render diag, skin default |
| 44 | Warzone: assert GetDeltaSeconds (ss-track-group.cpp 667, net-timer scripts) 14 s into the match; hero 0 submits, probe validated by references | scripttime.cpp (clocks[7] delta) |
| 45 | two full Warzone matches with the 60 s limit, results, back to the lobby, no crash; MP HUD draw ran; loading screen (cards, tip) shown; hero render object collected every frame but never submitted, draw data flags 0x6a00 vs 0x2000 on drawn actors | hero flag bits to the render port |

## Open after run 45 (sweep 2026-11-09)

Run 39's Gompa leave reason was the match-over byte (fixed in run 40). Built since run 45 and not yet run;
each needs a check in the next runs:

| Area | What to look at | Doc |
|---|---|---|
| remote animations (two PCs) | remote players play clips, footsteps / anim sounds fire; both PCs on this build (snapshot format changed) | PORT-NETANIM.md |
| "Starting in [A]..." popup | leader and member see the 5 s countdown; Back cancels on both | PORT-FRONTEND6.md 1 |
| vote level preload | `level preload (PS3 0x4a12e8)` at vote and result, then the match loads | PORT-FRONTEND6.md 2 |
| party rows | mic / mute icon, team colours in a custom team game | PORT-FRONTEND6.md 3 |
| GOTY stub tables | co-op kills of every NPC type (no assert at `0x5dedac`), health 90, Baseball Shirt Drake in the store | UNLOCKS.md 3.1, 5 |
| visibility probe (two PCs) | enemy name tags in line of sight and behind walls (filter {26, 0}) | PORT-GAMEPLAY3.md 7 |
| player character drawn, HUD, loading rosters | other ports in progress | PORT-RENDER.md, PORT-HUD.md, PORT-FRONTEND5.md |
