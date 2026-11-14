# u2mp behaviour descriptions

The Uncharted 2 multiplayer runtime (`gow3_pc/src/u2mp`) first carried function bodies ported from the PS3 game. On
2026-11-14 every such file was reimplemented clean-room, in three steps per group of files:

1. A describer wrote what the old code does (these files): purpose, interface, game facts (addresses, offsets,
   ids, layouts), observable behaviour, install checks, exact log texts and test scenarios. No code.
2. An implementer who never saw the old code wrote the new C++ from the description alone.
3. A tester ran the old code (git 62fc26b) and the new on the same fake game image
   (`gow3_pc/tests/u2mp_rewrite`): same guest calls with the same arguments in the same order, same logs, same
   memory. Differences were sent back to the implementer as behaviour reports, never as old code, and the
   description was corrected (each file's last section).

| Description | Source files | Differential test |
|---|---|---|
| events.md | events.cpp, gameplay2.cpp | 108 groups, 182,185 cases |
| frontend.md | frontend.cpp, frontend2.cpp, frontend3.cpp, frontend6.cpp | 255 scenarios |
| cinema.md | cinema.cpp, cinema_store.cpp | 128 scenarios |
| remaining.md | remaining.cpp, clantag.cpp | 255 scenarios |
| loading.md | loading.cpp | 209 steps |
| gameplay.md | scoreboard, rosternames, visibility, gameplay3, matchend, spawn, scripttime | 110 scenarios |
| netanim.md | netanim.cpp, natives.cpp | 174 scenarios, 59,660 cases |
| menus.md | menus.cpp, menuimages.cpp | 121 scenarios |
| progression.md | progression.cpp, netinit.cpp, netpatch.cpp | 227 scenarios |
| unlocks.md | unlocks.cpp | 53 worlds, 588 steps |
| hud.md | hud.cpp | 454 scenarios |
| session.md | session.cpp | 140 scenarios |

The other files of the module (net, voice, online, gamenet, install, crashlog, netheap, menudraw, render, nullsites,
dlc, texts, progression_store) were our own design from the start, not ports of game functions.
