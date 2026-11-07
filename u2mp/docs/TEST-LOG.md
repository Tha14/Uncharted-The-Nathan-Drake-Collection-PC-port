# u2mp in-game test log

## 8 November 2026 (single PC, host alone, saves copied into a test profile)

| Run | Result | Fix |
|---|---|---|
| 1 | session hooks not installed: gamenet checked the connection records before their static constructor ran | the check moved to attach() |
| 2 | Multiplayer opened, stuck on "Connecting..." (the net package request never reached the loader); the call hooked at 0x3f8280 is not per frame | the button requests "net" itself (0x56ad90); per-frame work from a page handler for every page (event 6) and the match states' Update |
| 3 | frame hook 60/s, NET_SM 18, net.pak loaded, Multiplayer page shown; Start Match crashed in 0x5eb830 (room-attribute list 0x14ce9b8 is null on PS4) | host_create() in C++ without that pick |
| 4 | states 18 -> 3 -> 10; crash in the allocator: memory context 0x14 (network heap) has no heap on PS4 | context 0x14 shares the default heap |
| 5 | states 3 -> 10 -> 16 (PlayTask "deathmatch"); crash at 0x64100f (AssignTeams area) called from 0x9bddc3 | open: next bug-fixing round, after the menus and assets |
