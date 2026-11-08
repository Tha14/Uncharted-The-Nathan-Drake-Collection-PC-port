# Uncharted 2 multiplayer frontend, session and network: function map (PS3 1.10 vs PS4 1.02)

Purpose: list every multiplayer frontend, session and network function of the PS3 1.10 build
(`NPUA80698` EBOOT, see `PS3-110-MAP.md` for the binary and its layout). For each one, the
table gives the PS4 1.02 counterpart and its state, so the stripped parts can be ported to
the PC build in C++. Match, gameplay and progression code is mapped separately and is not
covered here.

All of this is static analysis of the two executables only. Nothing was executed and no
game file was changed. No game bytes are stored here: only addresses, ids, names and short
strings. Server host names, URLs, keys and credentials in the strings are left out. Roles
that come from such a string read `str "<omitted>"`. No earlier third-party Uncharted 2
multiplayer notes or code were used.

Machine-readable form: `functions-frontend-net.tsv` (same rows, more columns: area, module,
sizes, MP callers, MP callees, PS3 net imports, PS3 data slots holding the OPD).

## 1. How it was made

| Tool (`u2mp/tools/`) | Used for |
|---|---|
| `ps3_elf.py`, `ps3_map.py`, `ps3_dis.py` | PS3 call graph, strings, imports, OPDs, constants (cache `work/ps3-110`) |
| `ps4_imports.py` (new) | Names the PS4 executable's imports from its own dynamic tables (`DT_SCE_JMPREL/SYMTAB/STRTAB/IMPORT_LIB`) and the NID table `gow3_pc/src/import_names.inc`. Also finds the PLT stubs and call sites. |
| `ps4_cg.py`, `u2_func_match.py` | PS4 call graph, stub detection, liveness, and the PS3 to PS4 function matcher (shared with the match/gameplay work, results in `work/match`) |
| `u2_menu_callbacks.py` | Pairs menu control ids to callbacks on both sides (`work/ps3-110/menu-callbacks.txt`) |
| `u2_netmap.py` (new) | This map. It selects the in-scope PS3 ranges, adds the curated roles and gets PS4 function lengths from `.eh_frame_hdr`. It applies verified overrides, then classifies each function and writes the TSV and the generated tables below. |

```
set P="D:/2. PS Games/PS3/NPUA80698/USRDIR/EBOOT.elf"
set B="D:/1. PC Games/Uncharted-windows/analysis/big2-ps4_Shipping/boot-linked.bin"
set L="D:/1. PC Games/Uncharted-windows/analysis/big2-ps4_Shipping/eboot.elf"   (decrypted PS4 ELF)
python u2mp/tools/u2_func_match.py %P% %B% %L% build        (once)
python u2mp/tools/u2_netmap.py %P% %B% %L% tsv u2mp/docs/functions-frontend-net.tsv
python u2mp/tools/u2_netmap.py %P% %B% %L% md  work/ps3-110/netmap-frag.md
python u2mp/tools/u2_netmap.py %P% %B% %L% netsm
python u2mp/tools/ps4_imports.py %L% %B% callers "sceNp|sceNet|Voice"
```

PS4 state values:
- **stub ret**: the PS4 function is a bare `ret`, `ret 0` or `ret constant`. Only the slot or registration is left.
- **intact**: the PS4 function has code. Its size and the share of matched PS3 callees it still calls are comparable to PS3.
- **partial**: the PS4 function is much smaller, has lost most of its matched callees, or the PS3 function uses an NP/net API that the PS4 executable does not import. `(dead)` means nothing on PS4 references it.
- **inlined**: there is no separate PS4 function. The features of the PS3 function were found inside the PS4 counterpart of a caller. This needs checking by hand: for tiny helpers it is a weak signal.
- **absent**: no PS4 counterpart. The evidence column gives the reason. `api not linked on PS4` means the function calls a PS3 NP/net API whose PS4 equivalent is not imported at all, so it cannot exist on PS4 in that form. `its strings are not in the PS4 image` means the PS4 build kept its assert and log strings elsewhere but not these. `no PS4 match` means none of the matching passes found a counterpart.

## 2. Key evidence: what the PS4 executable can still do on the network

The PS4 1.02 ELF lists 52 needed modules, including libSceNpMatching2, libSceNpParty,
libSceVoice, libSceNet, libSceNpSignaling, libSceHttp and libSceSsl. However, its 422 import
slots name only these NP and net functions:

| Library | Imported functions | PS4 functions that call them |
|---|---|---|
| libSceNet | `sceNetInetPton` | `121dc0` (not MP) |
| libSceNetCtl | `sceNetCtlGetState` | `5a0620` (menu system-message poller, see the bootstrap row) |
| libSceNpSignaling | `ActivateConnection`, `TerminateConnection`, `GetConnectionInfo` | `675200` `6752b0` `675370` `675400` (connection manager wrappers, dead); inlined copies in `609790` (PAGE_MULTIPLAYER_JOINING), `609680` (SELECT_JOIN_GAME), `6093f0` (called from `5eb830` = CREATE_GAME_WAIT callback), `5e9dd0` and `661230` (vtable slots) |
| libSceNpScore | `PollAsync`, `DeleteRequest` | `64e060` |
| libSceNpTus | Create/Delete NpTitleCtx, Create/Delete Request, Get/Set DataAsync, GetFriendsDataStatusAsync, PollAsync | `d5baf0`, `d5bfd0` (Collection-level stats, not U2 MP) |
| libSceNpManager | `sceNpGetNpId`, `sceNpCheckCallback`, `sceNpSetContentRestriction` | `d5bfd0`, `b892c0`, `ce1e20` (system level) |
| libSceNpTrophy, ProfileDialog, CommerceDialog, InvitationDialog | trophies and dialogs | system level |

The PS4 executable imports **no** sceNpMatching2 function, no socket, bind, sendto, recvfrom
or setsockopt (`sceNet*`), no sceVoice function, no sceNpParty function and no HTTP/SSL
function. The linker dropped every import that no surviving code calls. So the whole
Matching2 room layer, the UDP transport, voice, party and the HTTP/stat upload paths are gone
from the PS4 build. They are not just disconnected. The signaling wrappers are the only
networking code that is still compiled, and nothing references them.

## 3. Call graph: bootstrap to lobby to match start (PS3 1.10)

```
BUTTON_MULTIPLAYER cb 0x43b150                         PS4 5eb160 = ret
 +- 0x93904  game data / content permission check (cellGameDataCheck, msg dialog)
 +- 0x438bc8 MP bootstrap                              PS4 absent
     +- 0x4ebf7c cellNetCtlInit + NetStartDialogLoadAsync; cellNetCtlGetState must be 3
     +- 0x4ec068 sceNpManagerGetStatus check; sceNpManagerGetContentRatingFlag
     +- on failure 0x42a3e8(menu, msg 0x88a082af) and 0x4386d0 cleanup
     +- 0x429a08(menu 0xf0e800, PAGE_MULTIPLAYER_CONNECTING 0x9e1863b1)
     +- sys_ppu_thread_create "NetInit" -> 0x447a5c       PS4 absent
     |   +- NP: GetNpId -> 0xf1fa48+0x288, tickets, DRM, sceNpCommerce2Init,
     |   |       sceNpBasicRegisterHandler(0x449ae8)
     |   +- 0x4e7e58 / 0x4e83c8 live-data files, map-dlc checks
     |   +- NAT probe: 0x4db630 world socket, 6 x (0x4db1d0 sendto, 0x4ec068, 0x4dba48 recvfrom), 0x4db600 close
     |   +- 0x4e71d8 voice init (cellVoice ports)
     |   +- 0x4ec120 NP communication id select
     |   +- 0x4d0fc0 g_sessionManager.Init(0xf11978)      Matching2 ctx, callbacks 0x4d0c88/0x4d0d90/0x4d1c80/0x4d02c8/0x4d0e78/0x4d0040/0x4d0130
     |   +- 0x4cfe38, 0x4d1e2c(g_matchSession 0xf20c48), 0x4c3b00(party session 0xf2c8a0), 0x4d01a8 x2, 0x4d5338(0xf2c8a0,1), 0x4cfe88, 0x4d1ec8
     |   +- 0x4db7d4 g_udpSocket.Init(0xf10e18) P2P, SO_USECRYPTO
     |   +- 0x4db630 g_udpSocketWorld.Init(0xf11270)
     |   +- 0x4e9d28 g_npConnectionManager.Init(0xf3a060) signaling ctx + handlers 0x4e9618/0x4ea438
     |   +- 0x42f868/0x42f880/0x430048 bitstream setup, 0x44b030 http, 0x43b200 invite count, 0x4aba88 live data
     |   +- 0x4e0564 stats, 0x4c7820 NetPlayerData connect thread, 0x4ab854 leaderboard init
     |   +- initialized flag byte 0xf10e14; SetNetState(18 = NET_SM_CHOOSE_HOST_JOIN) at 0x4494a8
     +- 0x775370; log "AutoMatchmaking"

Per frame: FrameBegin 0x21db8 (PS4 3f5c20, 96 callees on PS3, 5 left on PS4)
 +- 0x449e88 NdiNetUpdate                                PS4 absent
     +- 0x43bdd0 NpNetUpdate (timeouts, kick, ResetConnection)   PS4 absent
     +- 0x43cdb8 NetConnectionUpdate: recvfrom loop -> 0x465218 read packet -> 0x464cb8 ack window;
     |                             send -> 0x4652cc / 0x465138 -> 0x4db1d0 sendto      PS4 absent
     +- heartbeat / location sends, voice 0x4e7560 / 0x4e6cd0
TaskManagerOnline (vtable, update 0x442558 absent on PS4; state objects 0x43d6b0..0x43f528 intact on PS4 5ea250..5ea750)

Menu side (page callbacks run every frame while the page is open):
 PAGE_MULTIPLAYER_CONNECTING 0x445020 (PS4 5ed6a0 partial): waits for NetInit, cellNetCtlGetNatInfo, cleanup on error
 PAGE_MULTIPLAYER 0x4465c8 (PS4 5eb1a0 partial) "Callback_Multiplayer": mode buttons, party roster, cleanup on back
   BUTTON_MULTIPLAYER_STARTMATCH 0x43aab0 (PS4 ret)  -> 0x43a444: NET_SM_START_MATCHMAKING (19)
   matchmaking / custom choice: buttons 0x4055f45f -> 0x43e99c (START_MATCHMAKING), 0xe2707dbe -> 0x444bb0 (CREATE_GAME_WAIT, CreateJoinRoom 0x4d421c)
 PAGE_MULTIPLAYER_MATCHMAKING 0x4b0a58 (PS4 ret): matchmaking state machine (order below inferred from the setter sites)
   19 START_MATCHMAKING -> 0x4d3c50 SearchRoom -> 5 CLIENT_GAME_LIST_WAIT -> 6 CLIENT_GAME_LIST_PICK
   -> 0x4d4578 JoinRoom -> 7 CLIENT_CONNECT_TO_HOST -> 9 CLIENT_RESERVE_SLOTS_WAIT -> 14 CLIENT_JOIN_GAME_WAIT
   -> 17 CLIENT_LOBBY / 21 CLIENT_VOTE_SCREEN -> 4 CLIENT_START
   no room found: 0x4d421c CreateJoinRoom -> 3 CREATE_GAME_WAIT -> 16 SERVER_LOBBY -> 20 SERVER_VOTE_SCREEN
   party: 12 CLIENT_WAIT_FOR_PARTY_HOST_JOIN, 10 CLIENT_WAIT_FOR_PARTY_CONNECT_GAME_HOST; errors -> 35 LEAVE_GAME
 PAGE_MULTIPLAYER_JOIN 0x4654d0 / BUTTON_MULTIPLAYER_JOIN 0x4655b0 / SELECT_JOIN_GAME 0x465808 / JOINING 0x4661e0
   (custom games, same states 5/7/9/10/12/14/17/18; PS4 keeps partial bodies 609640/6095d0/609680/609790)
 PAGE_LOBBY 0x443bc8 (PS4 5ebac0 intact): lobby page; roster lists 0x4b7898..0x4c1a08 (PS4 65xxxx intact)
 Match start (inferred): lobby -> 0x444660 NET_SM_UPDATE (26) -> 0x4a85a0 start net game -> TaskManagerOnline
   LoadLevels -> EstablishConnection -> WaitReady -> Playing; round end 0x4430d8 (30), results 0x43ec08 (29),
   reset 0x43edc8 (27), leave 0x445608 (35) -> SERVER_LOBBY/CLIENT_LOBBY by g_matchSession.IsHost (0x4d2394)
Teardown: 0x4386d0 (called from shutdown 0x23980, CONNECTING, PAGE_MULTIPLAYER): sceNpBasicUnregisterHandler,
 0x4d1a00 Matching2 term, socketclose x2, 0x4e8afc signaling term, 0x4e7038 voice end, sceNpCommerce2Term,
 0x4ab66c score term, 0x4c41f8 party error reset, SetNetState(0). sceNp/sceNp2 init/term at game start/exit (0x4ec184 from 0x26bbc, 0x4ec0f0 from 0x23ad0).
```

Globals used along this path (PS3 1.10): NET_SM state `0xf11764` (SetNetState `0x4383cc`,
GetNetState `0x4383e4`, IsOnline = state > 1 `0x4384b0`, 379 callers); flags `0xf11758`,
`0xf1175c`; initialized flag `0xf10e14`; g_sessionManager `0xf11978`; g_matchSession
`0xf20c48`; party session `0xf2c8a0`; g_udpSocket `0xf10e18`; g_udpSocketWorld `0xf11270`;
g_npConnectionManager `0xf3a060`; NP id `0xf1fa48+0x288`; g_netGameManager `0xf1dc80`
(IsHost `0x494ea8`); NetPlayerData `0xf2da60`; menu `0xf0e800`.

## 4. PS4 survivors without a firm PS3 pair

| PS4 1.02 | What it is | Evidence |
|---|---|---|
| `5a0620` | menu system-message poller: `sceNetCtlGetState`, many message ids including 0x88a082af | The matcher paired it with the bootstrap `0x438bc8` by that one id. Rejected: the PS4 function has about 15 unrelated message ids and is reached from a data slot. |
| `6093f0` | inlined signaling terminate, called from `5eb830` (CREATE_GAME_WAIT callback) | candidate for custom-games `0x465da0` or `0x465b70`; no shared id |
| `5e9dd0` | vtable method: signaling activate and terminate | in the flow.cpp vtable block near `5e9ca0` (= PS3 `0x4382f0`) |
| `661230` | vtable method: signaling activate | lobby/menu vtable block |
| `674e90`, `674f20` | title-id compare ("NPUA70051", "BCET70015") | PS3 has these strings inside NetInit `0x447a5c`; on PS4 they are separate, dead functions |
| `6750c0` | file open/close helper next to the connection-manager wrappers, dead | |
| `64e060` | sceNpScorePollAsync + DeleteRequest | paired by hand with leaderboard poll PS3 `0x4a9f78` (in the tables) |

## 4a. Port status of the frontend stub / partial rows (2026-11-08)

The generated tables below describe the PS4 image and do not change when code is ported (the TSV is
rebuilt by `u2_netmap.py`). Where each frontend `stub ret` / `partial` row is handled:

| PS3 | PS4 | role | PC |
|---|---|---|---|
| `4384e8` `4384ec` `4385d8` | `5eb7c0` `5eba90` `5ec470` | menu callbacks | `blr` on PS3 too: nothing to port |
| `438b3c` `438b80` `43a3dc` `43d76c` | `5ed8d0` `5eb790` `5ebdb0` `5efd30` | callbacks / event helper | equivalent (PORT-FRONTEND2.md) |
| `438638` `438a10` `4399a0` `439bf0` `439c7c` `439cf8` `439d60` `43f830` `43ff60` `440620` `440948` `441440` `4417f0` `4443f8` `4af4fc` `4f1950` `4f1c38` | `5ecb70` `5ebdd0` `5ed8c0` `5ec4b0` `5ec4c0` `5ec4a0` `5ec480` `5ebde0` `5ec000` `5ec460` `5ebea0` `5ed8a0` `5ec490` `5ed8b0` `64e900` `6769d0` `6769e0` | party / player / friends / invites / clan tag / store / Twitter callbacks, PLAYER_OPTIONS page, matchmaking box | remaining.cpp |
| `43aab0` `43b150` | `5eb780` `5eb160` | STARTMATCH, BUTTON_MULTIPLAYER | session.cpp (STARTMATCH in matchmaking mode -> frontend3.cpp) |
| `439a88` `439b04` | `5eba80` `5eba30` | leader back-out | frontend3.cpp (PORT-FRONTEND3.md) |
| `43e99c` | `5eba40` | START_MATCHMAKING | called by frontend3.cpp; party message 0x107 added |
| `43ea8c` | `5eb7d0` | LEAVING_GAME | session.cpp sets match+0xc8 while it is up |
| `43f5e8` | `5ebdf0` | LIST_PLAYERSMET | menus.cpp (rows), callback as PS4 |
| `440f90` | `5f2ea0` | "Connections done" | session.cpp (state update wrap) |
| `443248` | `5ecb80` | mode dispatcher | cinema.cpp |
| `445020` | `5ed6a0` | CONNECTING | bypassed by session.cpp; package preload `0xe155d0` ported there |
| `4465c8` | `5eb1a0` | PAGE_MULTIPLAYER | event 3 teardown: session.cpp (lost host) |
| `4654d0` `4655b0` `4661e0` | `609640` `6095d0` `609790` | custom game join pages | not needed (join by address); JOINING used by session.cpp |
| `465808` | `609680` | SELECT_JOIN_GAME | frontend2.cpp |
| `4a9f78` | `64e060` | leaderboard poll (dead) | progression.cpp serves local boards |
| `4b0670` | `64e8e0` | LIST_MATCHMAKING_VOTE | frontend3.cpp |
| `4b0a58` | `64e910` | PAGE_MULTIPLAYER_MATCHMAKING | frontend3.cpp |
| `4b9a88` `4bc310` `4bd2c4` `4bf768` | `655cf0` `65ecc0` `656fe0` `65ae50` | menu text queries | equivalent; the vote rows now have data |
| `4e8c8c` `4e9060` | `675200` `6752b0` | NpConnectionManager (dead) | net::Transport |
| `4edea0` `4f1208` | `675b30` `676d30` | Twitter | message box (remaining.cpp) |

## 5. Generated tables

Generated by `u2mp/tools/u2_netmap.py` (664 PS3 functions in scope): absent 527, inlined 23, intact 64, partial 27, stub ret 23.

### Menu controls and pages (registration table, PS3 `0x43918c` / PS4 `0x5ed8f0`)

| control id | name | PS3 site | PS3 callback | PS4 callback | PS4 state |
|---|---|---|---|---|---|
| `fb637366` | (name unknown) | `0043297c` | `432c00` | `5e45d0` | code |
| `6c3c0690` | LIST_CINEMA | `00432998` | `435248` | `5e4080` | code |
| `f35f2353` | BUTTON_CINEMA_PROTECT | `004329b4` | `433644` | `5e41d0` | stub ret |
| `9ddc5658` | BUTTON_CINEMA_UNPROTECT | `004329d0` | `43360c` | `5e41e0` | stub ret |
| `dd990755` | BUTTON_CINEMA_DELETE | `004329ec` | `433300` | `5e4270` | code |
| `7bb3c875` | (name unknown) | `00432a08` | `433588` | `5e41f0` | code |
| `140271ff` | BUTTON_CINEMA_UPLOAD | `00432a24` | `433240` | `5e42d0` | code |
| `4c28d6ff` | (name unknown) | `00432a40` | `43317c` | `5e4350` | code |
| `fe0e7de7` | (name unknown) | `00432a5c` | `43402c` | `5e4510` | stub ret |
| `6b2970e2` | (name unknown) | `00432a78` | `433554` | `5e4520` | stub ret |
| `fda94ac8` | (name unknown) | `00432a94` | `433118` | `5e44b0` | code |
| `ff26a432` | (name unknown) | `00432ab4` | `4341bc` | `5e43d0` | code |
| `1d84d11e` | PAGE_PROGRESS | `00432ad4` | `4336e4` | - | - |
| `354224e7` | PAGE_PROGRESS_CANCEL | `00432af0` | shared (register) | - | - |
| `ca9509a0` | PAGE_CINEMA | `00432b10` | `43367c` | - | - |
| `735c4dbe` | PAGE_MULTIPLAYER_CINEMA_LIST | `00432b2c` | shared (register) | - | - |
| `bf6960e8` | (name unknown) | `00432b4c` | `432c8c` | `5e4490` | code |
| `c9f003a8` | (name unknown) | `00432b68` | shared (register) | `5e4490` | code |
| `dba0d3e6` | BUTTON_MULTIPLAYER_STARTCINEMA | `00432b88` | `432ea4` | `5e45a0` | code |
| `390ffbfb` | BUTTON_RENDER_SETTINGS_DEFAULT | `00432ba4` | `432fe4` | `5e4530` | code |
| `1409e168` | BUTTON_PROGRESS_CANCEL | `00432bc0` | `432844` | `5e3f50` | stub ret |
| `7926436e` | (name unknown) | `00432bdc` | `432d8c` | `5e4040` | code |
| `?` | (name unknown) | `004391bc` | `443248` | - | - |
| `ed9d8ae1` | BUTTON_MULTIPLAYER | `004391dc` | `43b150` | `5eb160` | stub ret |
| `afb7834c` | BUTTON_MULTIPLAYER_LEAVE_MULTIPLAYER | `004391fc` | `43911c` | `5eb180` | code |
| `70e80076` | BUTTON_MULTIPLAYER_STARTMATCH | `00439218` | `43aab0` | `5eb780` | stub ret |
| `3ff2b9cf` | BUTTON_MULTIPLAYER_JOIN | `00439234` | `4655b0` | `6095d0` | code |
| `5cc5c2b2` | BUTTON_MULTIPLAYER_SELECT_JOIN_GAME | `00439250` | `465808` | `609680` | code |
| `2c2f1bb7` | BUTTON_MULTIPLAYER_LEAVE_GAME | `0043926c` | `438b80` | `5eb790` | code |
| `41103666` | BUTTON_MULTIPLAYER_START | `00439288` | `4384e8` | `5eb7c0` | stub ret |
| `a7613ffe` | BUTTON_MULTIPLAYER_LEAVEGAME | `004392a4` | `43a160` | `5ec450` | code |
| `8e2767d9` | (name unknown) | `004392c0` | `43a3dc` | `5ebdb0` | code |
| `37c9cb76` | (name unknown) | `004392dc` | `438a10` | `5ebdd0` | stub ret |
| `8a7e1d58` | LIST_FRIENDS | `004392f8` | `43f830` | `5ebde0` | stub ret |
| `369314b2` | LIST_PLAYERSMET | `00439314` | `43f5e8` | `5ebdf0` | code |
| `2e1a9a7b` | (name unknown) | `00439330` | `44026c` | `5eb080` | code |
| `ec9950f8` | (name unknown) | `0043934c` | `439158` | `5eb150` | code |
| `0e8bb2d7` | BUTTON_MULTIPLAYER_INVITES | `00439368` | `440948` | `5ebea0` | stub ret |
| `45a9c65b` | (name unknown) | `00439384` | `440050` | `5ebee0` | code |
| `0ace8463` | (name unknown) | `004393a0` | `43ff60` | `5ec000` | stub ret |
| `dc6e89d8` | LIST_MATCHMAKING_VOTE | `004393c0` | `4b0670` | `64e8e0` | code |
| `?` | (name unknown) | `004393d8` | `4448d0` | - | - |
| `ed61e469` | PAGE_MULTIPLAYER_PLAYER_OPTIONS | `004393f8` | `4443f8` | `5ed8b0` | stub ret |
| `53cbb5f2` | PAGE_MULTIPLAYER | `00439414` | `4465c8` | `5eb1a0` | code |
| `e8d59d98` | PAGE_MULTIPLAYER_JOIN | `00439434` | `4654d0` | `609640` | code |
| `1af4bdd2` | PAGE_MULTIPLAYER_JOINING | `00439450` | `4661e0` | `609790` | code |
| `b808d03c` | PAGE_MULTIPLAYER_LEAVING_GAME | `00439470` | `43ea8c` | `5eb7d0` | code |
| `e35f0f1a` | PAGE_MULTIPLAYER_MATCHMAKING | `0043948c` | `4b0a58` | `64e910` | stub ret |
| `b99a3235` | PAGE_LOBBY | `004394a8` | `443bc8` | `5ebac0` | code |
| `9e1863b1` | PAGE_MULTIPLAYER_CONNECTING | `004394c4` | `445020` | `5ed6a0` | code |
| `c3815c33` | PAGE_MULTIPLAYER_FRIENDS_LIST | `004394e4` | `43aa30` | - | - |
| `2b2e3367` | PAGE_MULTIPLAYER_STORE | `00439500` | shared (register) | - | - |
| `aa51efcd` | PAGE_MULTIPLAYER_STORE_ITEMS | `00439520` | shared (register) | - | - |
| `6b7378ea` | PAGE_MULTIPLAYER_POSTGAME | `0043953c` | `43fb38` | `5ec4f0` | code |
| `f7c82577` | BUTTON_MULTIPLAYER_PLAYER0 | `0043955c` | `440620` | `5ec460` | stub ret |
| `f30938c0` | BUTTON_MULTIPLAYER_PLAYER1 | `00439578` | shared (register) | `5ec460` | stub ret |
| `fe4a1e19` | BUTTON_MULTIPLAYER_PLAYER2 | `00439594` | shared (register) | `5ec460` | stub ret |
| `fa8b03ae` | BUTTON_MULTIPLAYER_PLAYER3 | `004395b0` | shared (register) | `5ec460` | stub ret |
| `e4cc53ab` | BUTTON_MULTIPLAYER_PLAYER4 | `004395cc` | shared (register) | `5ec460` | stub ret |
| `e00d4e1c` | BUTTON_MULTIPLAYER_PLAYER5 | `004395e8` | shared (register) | `5ec460` | stub ret |
| `ed4e68c5` | BUTTON_MULTIPLAYER_PLAYER6 | `00439604` | shared (register) | `5ec460` | stub ret |
| `e98f7572` | BUTTON_MULTIPLAYER_PLAYER7 | `00439620` | shared (register) | `5ec460` | stub ret |
| `d1c0c8cf` | BUTTON_MULTIPLAYER_PLAYER8 | `00439640` | shared (register) | `5ec460` | stub ret |
| `d501d578` | BUTTON_MULTIPLAYER_PLAYER9 | `00439660` | shared (register) | `5ec460` | stub ret |
| `053e5690` | (name unknown) | `0043967c` | `4385d8` | `5ec470` | stub ret |
| `7273924c` | (name unknown) | `00439698` | `439d60` | `5ec480` | stub ret |
| `3ef1f497` | BUTTON_MULTIPLAYER_PARTY_PLAYER0 | `004396b8` | `4417f0` | `5ec490` | stub ret |
| `3a30e920` | BUTTON_MULTIPLAYER_PARTY_PLAYER1 | `004396d4` | shared (register) | `5ec490` | stub ret |
| `3773cff9` | BUTTON_MULTIPLAYER_PARTY_PLAYER2 | `004396f0` | shared (register) | `5ec490` | stub ret |
| `33b2d24e` | BUTTON_MULTIPLAYER_PARTY_PLAYER3 | `0043970c` | shared (register) | `5ec490` | stub ret |
| `2df5824b` | BUTTON_MULTIPLAYER_PARTY_PLAYER4 | `00439728` | shared (register) | `5ec490` | stub ret |
| `29349ffc` | BUTTON_MULTIPLAYER_PARTY_PLAYER5 | `00439744` | shared (register) | `5ec490` | stub ret |
| `2477b925` | BUTTON_MULTIPLAYER_PARTY_PLAYER6 | `00439760` | shared (register) | `5ec490` | stub ret |
| `20b6a492` | BUTTON_MULTIPLAYER_PARTY_PLAYER7 | `0043977c` | shared (register) | `5ec490` | stub ret |
| `18f9192f` | BUTTON_MULTIPLAYER_PARTY_PLAYER8 | `00439798` | shared (register) | `5ec490` | stub ret |
| `1c380498` | BUTTON_MULTIPLAYER_PARTY_PLAYER9 | `004397b4` | shared (register) | `5ec490` | stub ret |
| `7cc46b94` | (name unknown) | `004397d0` | `439cf8` | `5ec4a0` | stub ret |
| `6547d63c` | (name unknown) | `004397ec` | `439bf0` | `5ec4b0` | stub ret |
| `2d32c8b0` | (name unknown) | `00439808` | `439c7c` | `5ec4c0` | stub ret |
| `e2707dbe` | (name unknown) | `00439824` | `444bb0` | `5eb830` | code |
| `bf6ce4d5` | (name unknown) | `00439844` | `439b04` | `5eba30` | code |
| `4055f45f` | (name unknown) | `00439860` | `43e99c` | `5eba40` | code |
| `bbb649ef` | (name unknown) | `0043987c` | `439a88` | `5eba80` | code |
| `ee8f5f9b` | (name unknown) | `00439898` | `4384ec` | `5eba90` | stub ret |
| `24df6961` | (name unknown) | `004398b8` | `4af4fc` | `64e900` | stub ret |
| `2a2d147e` | (name unknown) | `004398d4` | shared (register) | `64e900` | stub ret |
| `b64cfeca` | BUTTON_POSTGAME_PARTYUP | `004398f4` | `4399e0` | `5eca50` | code |
| `56126d00` | (name unknown) | `00439910` | `438638` | `5ecb70` | stub ret |
| `b69ea58b` | BUTTON_CLANTAG | `00439930` | `441440` | `5ed8a0` | stub ret |
| `a857e46b` | (name unknown) | `00439950` | `4399a0` | `5ed8c0` | stub ret |
| `705e651e` | (name unknown) | `0043996c` | `438b3c` | `5ed8d0` | code |
| `d1683525` | (name unknown) | `004a9f00` | `4ab9f0` | `64e0f0` | code |
| `035e68ef` | (name unknown) | `004a9f1c` | `4ab9b0` | `64e120` | code |
| `cacfc66d` | (name unknown) | `004a9f3c` | `4a9e94` | `64e150` | code |
| `61f5718d` | (name unknown) | `004a9f58` | `4a9eb4` | `64e160` | code |
| `2ad3b7ab` | PAGE_TWITTER | `004f0a64` | `4f1950` | `6769d0` | stub ret |
| `2c738b98` | BUTTON_TWITTER_USERNAME | `004f0a88` | `4f1c38` | `6769e0` | stub ret |
| `b1668731` | BUTTON_TWITTER_PASSWORD | `004f0aac` | shared (register) | `6769e0` | stub ret |
| `a989fea1` | BUTTON_TWITTER_SIGN_IN | `004f0acc` | shared (register) | `6769e0` | stub ret |
| `ddaab65a` | (name unknown) | `004f0aec` | shared (register) | `6769e0` | stub ret |

### NET_SM states (values written by SetNetState `0x4383cc` or stored to `0xf11764`)

| value | name (GOTO log) | written in (PS3 1.10) |
|---|---|---|
| 3 | NET_SM_CREATE_GAME_WAIT | `444bb0@444e10` `4b0a58@4b2a9c` |
| 4 | NET_SM_CLIENT_START | `4b0a58@4b2bcc` |
| 5 | NET_SM_CLIENT_GAME_LIST_WAIT | `4655b0@465728` `4b0a58@4b11b8` |
| 6 | NET_SM_CLIENT_GAME_LIST_PICK | `4b0a58@4b13a0` |
| 7 | NET_SM_CLIENT_CONNECT_TO_HOST | `43a444@43a6b0` `465808@465a74` `4b0a58@4b1698` |
| 9 | NET_SM_CLIENT_RESERVE_SLOTS_WAIT | `4661e0@46639c` `4b0a58@4b3970` |
| 10 | NET_SM_CLIENT_WAIT_FOR_PARTY_CONNECT_GAME_HOST | `465da0@4661d4` `4661e0@466cb0` |
| 12 | NET_SM_CLIENT_WAIT_FOR_PARTY_HOST_JOIN | `4661e0@466b60` `4b0a58@4b3b58` |
| 14 | NET_SM_CLIENT_JOIN_GAME_WAIT | `4661e0@46661c` `4b0a58@4b181c` |
| 16 | NET_SM_SERVER_LOBBY | `465da0@465f8c` `4b0a58@4b329c` |
| 17 | NET_SM_CLIENT_LOBBY | `4661e0@466ad0` `4b0a58@4b3388` |
| 18 | NET_SM_CHOOSE_HOST_JOIN | `43ea8c@43ebfc` `4654d0@4655a4` `4655b0@4657e0` `465808@465b40` `465da0@465eb0` `4661e0@466528` `4b0a58@4b0f3c` `4b0a58@4b30d8` |
| 18 | - | `444bb0@444ea4` `447a5c@4494a8` |
| 19 | NET_SM_START_MATCHMAKING | `43a444@43a9d8` `43e99c@43ea5c` `43ea8c@43eb80` `43fb38@43ff50` `443248@443798` `4b0a58@4b176c` |
| 20 | NET_SM_SERVER_VOTE_SCREEN | `4b0a58@4b3690` |
| 21 | NET_SM_CLIENT_VOTE_SCREEN | `4b0a58@4b1f28` |
| 26 | NET_SM_UPDATE | `444660@44472c` |
| 27 | NET_SM_RESET | `43edc8@43ee5c` |
| 28 | NET_SM_REPLAY | `4348f0@434994` `435be8@435cac` |
| 29 | NET_SM_RESULTS | `43ec08@43ed24` |
| 30 | NET_SM_ROUND_RESULTS | `4430d8@44317c` |
| 35 | NET_SM_LEAVE_GAME | `445608@44579c` `4b0a58@4b19dc` `4b0a58@4b1ac4` `4b0a58@4b2198` `4b0a58@4b269c` |
| (computed) | - | `4383cc@4383dc` `4386d0@438800` |
| (computed) | g_matchSession.IsHost() ? NET_SM_CREATE_GAME_WAIT : NET_SM_CLIENT_LOBBY | `445608@445f64` |
| (computed) | g_matchSession.IsHost() ? NET_SM_SERVER_LOBBY : NET_SM_CLIENT_LOBBY | `445608@446068` |

### Frontend: menu callbacks, page handlers, NET_SM transitions, lobby UI

**flow.cpp: menu callbacks, bootstrap, MP control registration**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| helper | `4382f0` | `5e9ca0` | intact | matched by vtable; size 60/72; callees kept 0/1 | reuse PS4 code |
| helper | `438350` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| clear net flags (f11758/f1175c) | `4383a4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| SetNetState(state) -> f11764 | `4383cc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| GetNetState() | `4383e4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4655b0 (custom game list (BUTTON_MULTIPLAYER_JOI) | `4383fc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `438424` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `438440` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `438444` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `438448` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `438478` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `438488` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `438498` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| IsOnline(): NetState > 1 (379 callers) | `4384b0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback id 41103666 | `4384e8` | `5eb7c0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| menu callback id ee8f5f9b | `4384ec` | `5eba90` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper of 43a164 (send invite (sceNpBasicSendMessageGui)) | `4384f0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback id 053e5690 | `4385d8` | `5ec470` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `4385dc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4385e0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4385e8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4385ec` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback id 56126d00 | `438638` | `5ecb70` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `43866c` | `5f30e0` | intact | matched by vtable; size 64/72; callees kept 0/1 | reuse PS4 code |
| MP cleanup / abort (Matching2 term, sockets, signaling, voice, commerce, score) | `4386d0` | absent | absent | api not linked on PS4: sceNpBasicUnregisterHandler sceNpCommerce2Term | reimplement on PC transport (replaces sceNpBasicUnregisterHandler, sceNpCommerce2Term) |
| menu callback id 37c9cb76 | `438a10` | `5ebdd0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| sceNpBasic handler "basicCb" | `438a50` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| menu callback id 705e651e | `438b3c` | `5ed8d0` | partial | matched by menucb; size 44/13; callees kept 1/1 | compare with PS3; restore stripped calls |
| menu callback id 2c2f1bb7 | `438b80` | `5eb790` | partial | matched by sid:c89b86fc; size 48/13; callees kept 1/1 | compare with PS3; restore stripped calls |
| MP bootstrap (NetCtl checks, PAGE_MULTIPLAYER_CONNECTING, starts NetInit thread) | `438bc8` | absent | absent | api not linked on PS4: cellNetCtlInit sceNpManagerGetContentRatingFlag | reimplement on PC transport (replaces cellNetCtlGetState, cellNetCtlInit, sceNpManagerGetContentRatingFlag) |
| npManagerCallback (ticket) | `438fa0` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `439044` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback BUTTON_MULTIPLAYER_LEAVE_MULTIPLAYER | `43911c` | `5eb180` | intact | matched by menucb; size 3c/c; callees kept 1/1 | reuse PS4 code |
| menu callback id ec9950f8 | `439158` | `5eb150` | intact | matched by menucb; size 34/c; callees kept 1/1 | reuse PS4 code |
| register MP menu controls and pages | `43918c` | `5ed8f0` | intact | matched by sid:053e5690; size 814/1feb; callees kept 2/2 | reuse PS4 code |
| store browse (sceNpCommerce2ExecuteStoreBrowse) | `4399a0` | `5ed8c0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| menu callback BUTTON_POSTGAME_PARTYUP | `4399e0` | `5eca50` | intact | matched by menucb; size a8/11f | reuse PS4 code |
| menu callback id bbb649ef | `439a88` | `5eba80` | partial | matched by menucb; size 7c/c; callees kept 1/1 | compare with PS3; restore stripped calls |
| menu callback id bf6ce4d5 | `439b04` | `5eba30` | partial | matched by menucb; size 7c/c; callees kept 1/1 | compare with PS3; restore stripped calls |
| helper | `439b80` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback id 6547d63c | `439bf0` | `5ec4b0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| menu callback id 2d32c8b0 | `439c7c` | `5ec4c0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| menu callback id 7cc46b94 | `439cf8` | `5ec4a0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| menu callback id 7273924c | `439d60` | `5ec480` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper of 442558 (TaskManagerOnlineUpdate) | `439df8` | `5ec010` | inlined | features inside PS4 5ec010 (inlined(1.00)) | logic lives in PS4 5ec010 |
| menu callback id a7613ffe | `43a160` | `5ec450` | intact | matched by menucb; size 4/5 | reuse PS4 code |
| send invite (sceNpBasicSendMessageGui) | `43a164` | absent | absent | api not linked on PS4: sceNpBasicSendMessageGui | reimplement on PC transport (replaces sceNpBasicSendMessageGui) |
| helper of 43f830 (LIST_FRIENDS) | `43a340` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 465b70 (host asked to leave the room) | `43a398` | `5f1000` | inlined | features inside PS4 5f1000 (inlined(1.00)) | logic lives in PS4 5f1000 |
| menu callback id 8e2767d9 | `43a3dc` | `5ebdb0` | partial | matched by menucb; size 68/1c; callees kept 2/3 | compare with PS3; restore stripped calls |
| NET_SM: lobby status change -> START_MATCHMAKING / CLIENT_CONNECT_TO_HOST | `43a444` | absent | absent | api not linked on PS4: sceNpBasicAbortGui | reimplement on PC transport (replaces sceNpBasicAbortGui) |
| menu callback PAGE_MULTIPLAYER_FRIENDS_LIST | `43aa30` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| BUTTON_MULTIPLAYER_STARTMATCH | `43aab0` | `5eb780` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| BUTTON_MULTIPLAYER callback | `43b150` | `5eb160` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `43b1dc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| invite / message count | `43b200` | absent | absent | api not linked on PS4: sceNpBasicGetCustomInvitationEntryCount sceNpBasicGetMessageEntryCount | reimplement on PC transport (replaces sceNpBasicGetCustomInvitationEntryCount, sceNpBasicGetMessageEntryCount) |
| helper | `43b290` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str ": /@CFFF0C828/" | `43b30c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**flow.cpp: TaskManagerOnline, NET_SM transitions, page handlers**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| TaskManagerOnline LoadLevels | `43d6b0` | `5ea4d0` | intact | matched by str:LoadLevels; size bc/c7 | reuse PS4 code |
| helper | `43d76c` | `5efd30` | partial | matched by vtable; size 1e0/c8; callees kept 1/1 | compare with PS3; restore stripped calls |
| str "************** Asked to start Connection" | `43d94c` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "%s left match" | `43d9e0` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "%s joined match" | `43dad8` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `43dcd8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43de50` | `5f2350` | intact | matched by sid:97e94999; size b0/88; callees kept 1/1 | reuse PS4 code |
| str "timeout2 = %2.2f" | `43df00` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `43e680` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NET_SM -> START_MATCHMAKING | `43e99c` | `5eba40` | partial | matched by menucb; size f0/34; callees kept 2/3 | compare with PS3; restore stripped calls |
| PAGE_MULTIPLAYER_LEAVING_GAME | `43ea8c` | `5eb7d0` | partial | matched by menucb; size 17c/57; callees kept 2/3 | compare with PS3; restore stripped calls |
| NET_SM -> RESULTS ("Save Game Stats" thread) | `43ec08` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NET_SM -> RESET | `43edc8` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| TaskManagerOnline Leave | `43ee8c` | `5eaed0` | intact | matched by strset; size bc/c7 | reuse PS4 code |
| TaskManagerOnline EstablishConnection | `43ef48` | `5ea390` | intact | matched by str:EstablishConnection; size bc/c7 | reuse PS4 code |
| str "Active" | `43f004` | `5ea250` | intact | matched by vtable; size bc/c7 | reuse PS4 code |
| TaskManagerOnline ShowRoundResults | `43f0c0` | `5ead90` | intact | matched by str:ShowRoundResults; size bc/c7 | reuse PS4 code |
| TaskManagerOnline ShowResults | `43f17c` | `5eac50` | intact | matched by str:ShowResults; size bc/c7 | reuse PS4 code |
| TaskManagerOnline ResettingReady | `43f238` | `5eab10` | intact | matched by str:ResettingReady; size bc/c7 | reuse PS4 code |
| TaskManagerOnline Resetting | `43f2f4` | `5ea9d0` | intact | matched by strset; size bc/c7 | reuse PS4 code |
| TaskManagerOnline PreResetting | `43f3b0` | `5ea890` | intact | matched by str:PreResetting; size bc/c7 | reuse PS4 code |
| TaskManagerOnline Playing | `43f46c` | `5ea750` | intact | matched by str:Playing; size bc/c7 | reuse PS4 code |
| TaskManagerOnline WaitReady | `43f528` | `5ea610` | intact | matched by str:WaitReady; size c0/c7 | reuse PS4 code |
| LIST_PLAYERSMET | `43f5e8` | `5ebdf0` | partial | matched by sid:20d51153; size 248/a9 | compare with PS3; restore stripped calls |
| LIST_FRIENDS | `43f830` | `5ebde0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| PAGE_MULTIPLAYER_POSTGAME | `43fb38` | `5ec4f0` | intact | matched by sid:eba80bd9; size 428/552; callees kept 2/3 | reuse PS4 code |
| menu callback id 0ace8463 | `43ff60` | `5ec000` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| menu callback id 45a9c65b | `440050` | `5ebee0` | intact | matched by sid:03ef26e6; size 6c/117 | reuse PS4 code |
| helper | `4400bc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `440130` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback id 2e1a9a7b | `44026c` | `5eb080` | intact | matched by menucb; size c8/c5; callees kept 2/3 | reuse PS4 code |
| str "New host : %s" | `440334` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "********* HOST %s LEFT ***********" | `440478` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `4405c4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| BUTTON_MULTIPLAYER_PLAYER0..9 | `440620` | `5ec460` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| BUTTON_MULTIPLAYER_INVITES | `440948` | `5ebea0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| str "********* HOST %s LEFT ***********" | `4409c4` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `440c38` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `440d50` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `440f48` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "Connections done" | `440f90` | `5f2ea0` | partial | matched by caller; size 410/1b0; callees kept 2/3 | compare with PS3; restore stripped calls |
| helper | `4413a0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| BUTTON_CLANTAG | `441440` | `5ed8a0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `4416e0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| BUTTON_MULTIPLAYER_PARTY_PLAYER0..9 | `4417f0` | `5ec490` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `441b88` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `441ca0` | `5efc80` | intact | matched by vtable; size d8/a8; callees kept 2/2 | reuse PS4 code |
| helper | `441d78` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `441dd8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 449e88 (NdiNetUpdate per frame (from FrameBegin ) | `441e30` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "Eula status = %i version = %i" | `442100` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "time left %i" | `4421b8` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| TaskManagerOnlineUpdate | `442558` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NET_SM -> ROUND_RESULTS | `4430d8` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `4431a0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback id ? | `443248` | `5ecb80` | partial | matched by sid:041a85ca; size 980/88a; callees kept 4/6; PS3 APIs not linked on PS4: sceNpBasicAddFriend sceNpEulaShowCurrentEula sceNpManagerGetChatRestrictionFlag | compare with PS3; restore stripped calls |
| PAGE_LOBBY | `443bc8` | `5ebac0` | intact | matched by menucb; size 278/2e7; callees kept 1/1 | reuse PS4 code |
| helper | `443e40` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "Description" | `443f00` | `5f2830` | intact | matched by str:%s - %s %s; size 31c/55c; callees kept 1/1 | reuse PS4 code |
| str "TaskManagerOnline" | `44421c` | `5ea890` | inlined | features inside PS4 5ea890 (inlined(1.00)) | logic lives in PS4 5ea890 |
| helper | `4443d4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4443e4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| PAGE_MULTIPLAYER_PLAYER_OPTIONS | `4443f8` | `5ed8b0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| NET_SM -> UPDATE (starts the net game) | `444660` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| menu callback id ? | `4448d0` | `5ed410` | intact | matched by sid:20a76f2a; size 2e0/286 | reuse PS4 code |
| NET_SM -> CREATE_GAME_WAIT (CreateJoinRoom) | `444bb0` | `5eb830` | intact | matched by sid:e5350103; size 30c/200; callees kept 3/4 | reuse PS4 code |
| helper | `444ebc` | `5efe00` | intact | matched by vtable; size 164/12b | reuse PS4 code |
| PAGE_MULTIPLAYER_CONNECTING | `445020` | `5ed6a0` | partial | matched by menucb; size 5e8/1ff; callees kept 3/5; PS3 APIs not linked on PS4: cellNetCtlGetNatInfo sceNpEulaCheckEulaStatus | compare with PS3; restore stripped calls |
| NET_SM -> LEAVE_GAME, lobby choice SERVER/CLIENT | `445608` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| PAGE_MULTIPLAYER (Callback_Multiplayer) | `4465c8` | `5eb1a0` | partial | matched by menucb; size 698/5d3; callees kept 2/3; PS3 APIs not linked on PS4: sceNpBasicGetFriendListEntry sceNpBasicGetFriendListEntryCount sceNpBasicGetFriendPresenceByNpId | compare with PS3; restore stripped calls |
| str "<omitted>" | `446c60` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| menu-temple / MP level select helper | `447248` | absent | absent | api not linked on PS4: inet_ntoa | reimplement on PC transport (replaces inet_ntoa) |

**net-custom-games.cpp: join / joining / custom game list**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| PAGE_MULTIPLAYER_JOIN | `4654d0` | `609640` | partial | matched by menucb; size e0/3b; callees kept 1/2 | compare with PS3; restore stripped calls |
| custom game list (BUTTON_MULTIPLAYER_JOIN) | `4655b0` | `6095d0` | partial | matched by menucb; size 258/62; callees kept 2/3 | compare with PS3; restore stripped calls |
| BUTTON_MULTIPLAYER_SELECT_JOIN_GAME | `465808` | `609680` | partial | matched by menucb; size 368/10a; callees kept 2/3 | compare with PS3; restore stripped calls |
| host asked to leave the room | `465b70` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| custom game: host/join decision | `465da0` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| PAGE_MULTIPLAYER_JOINING (client join state machine) | `4661e0` | `609790` | partial | matched by menucb; size ce4/568; callees kept 3/4 | compare with PS3; restore stripped calls |
| helper | `466ec4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `466ee0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**net-leaderboard: score service + pages**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| helper | `4a9e8c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback id cacfc66d | `4a9e94` | `64e150` | intact | matched by menucb; size 20/f | reuse PS4 code |
| menu callback id 61f5718d | `4a9eb4` | `64e160` | intact | matched by menucb; size 1c/f | reuse PS4 code |
| helper of 43918c (register MP menu controls and pages) | `4a9ed0` | `64e170` | intact | matched by sid:035e68ef; size a8/21c | reuse PS4 code |
| leaderboard poll | `4a9f78` | `64e060` | partial (dead) | matched by only PS4 caller of sceNpScorePollAsync (DeleteRequest where PS3 destroys the ctx); size 88/61; PS3 APIs not linked on PS4: sceNpScoreDestroyTransactionCtx; PS4 dead (no reference) | compare with PS3; restore stripped calls |
| helper | `4aa000` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4aa084` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4aa0c4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| calls sceNpScoreCensorCommentAsync sceNpScoreCreateTransactionCtx | `4aa0d4` | absent | absent | api not linked on PS4: sceNpScoreCensorCommentAsync sceNpScoreCreateTransactionCtx | reimplement on PC transport (replaces sceNpScoreCensorCommentAsync, sceNpScoreCreateTransactionCtx) |
| leaderboard rankings | `4aa170` | absent | absent | api not linked on PS4: sceNpScoreCreateTransactionCtx sceNpScoreDestroyTransactionCtx sceNpScoreGetRankingByNpIdAsync sceNpScoreGetRankingByRangeAsync | reimplement on PC transport (replaces sceNpScoreCreateTransactionCtx, sceNpScoreDestroyTransactionCtx, sceNpScoreGetRankingByNpIdAsync) |
| str "Page" | `4ab3e8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| leaderboard "Score" thread start | `4ab45c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| calls sys_net_free_thread_context | `4ab620` | absent | absent | api not linked on PS4: sys_net_free_thread_context | reimplement on PC transport (replaces sys_net_free_thread_context) |
| leaderboard term | `4ab66c` | absent | absent | api not linked on PS4: sceNpScoreDestroyTitleCtx sceNpScoreDestroyTransactionCtx sceNpScoreTerm | reimplement on PC transport (replaces sceNpScoreDestroyTitleCtx, sceNpScoreDestroyTransactionCtx, sceNpScoreTerm) |
| leaderboard record score | `4ab6d0` | absent | absent | api not linked on PS4: sceNpScoreCreateTransactionCtx sceNpScoreDestroyTransactionCtx sceNpScoreRecordScore sys_net_free_thread_context | reimplement on PC transport (replaces sceNpScoreCreateTransactionCtx, sceNpScoreDestroyTransactionCtx, sceNpScoreRecordScore) |
| leaderboard init (NetInit) | `4ab854` | absent | absent | api not linked on PS4: sceNpScoreCreateTitleCtx sceNpScoreInit | reimplement on PC transport (replaces sceNpManagerGetNpId, sceNpScoreCreateTitleCtx, sceNpScoreInit) |
| menu callback id 035e68ef | `4ab9b0` | `64e120` | intact | matched by menucb; size 40/2d | reuse PS4 code |
| menu callback id d1683525 | `4ab9f0` | `64e0f0` | intact | matched by menucb; size 3c/2c | reuse PS4 code |
| helper | `4aba2c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4aba74` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "ingame" | `4aba88` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |

**net-matchmaking.cpp: matchmaking state machine and vote**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| helper | `4af108` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4af150` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4b0670 (LIST_MATCHMAKING_VOTE) | `4af160` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43ec08 (NET_SM -> RESULTS ("Save Game Stats" thr) | `4af240` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| menu callback id 24df6961 | `4af4fc` | `64e900` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `4af524` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4af568` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4af5bc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4af604` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| calls sceNpUtilCmpNpId | `4af614` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| str "jstack <= (I32)bufferSize" | `4af6d0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4b0a58 (PAGE_MULTIPLAYER_MATCHMAKING (matchmakin) | `4afe48` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4b0a58 (PAGE_MULTIPLAYER_MATCHMAKING (matchmakin) | `4afee8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| calls sceNpUtilCmpNpId | `4b0008` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| LIST_MATCHMAKING_VOTE | `4b0670` | `64e8e0` | partial | matched by menucb; size 90/19 | compare with PS3; restore stripped calls |
| helper of 4b0a58 (PAGE_MULTIPLAYER_MATCHMAKING (matchmakin) | `4b0700` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4b0a58 (PAGE_MULTIPLAYER_MATCHMAKING (matchmakin) | `4b0868` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| PAGE_MULTIPLAYER_MATCHMAKING (matchmaking state machine) | `4b0a58` | `64e910` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `4b3f40` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b405c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b40c8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b40d8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b414c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b41c0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**net-menu / lobby lists, roster query table, player cards**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| helper | `4b7124` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b712c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b7130` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b7178` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b7184` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b7294` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b732c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b73c4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b745c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b74f8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b7650` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b7714` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b77ac` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "<omitted>" | `4b7898` | `65f220` | intact | matched by str:<omitted>; size 1f0/1c4; callees kept 2/3 | reuse PS4 code |
| helper | `4b7a88` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4bc310 (mode label list) | `4b7b3c` | `65ecc0` | inlined | features inside PS4 65ecc0 (inlined(1.00)) | logic lives in PS4 65ecc0 |
| helper | `4b7c48` | `65e7f0` | intact | matched by sid:51257d8b; size c8/93; callees kept 1/1 | reuse PS4 code |
| helper | `4b7d10` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b7d80` | `655c40` | intact | matched by sid:63091cd0; size 100/a6; callees kept 1/1 | reuse PS4 code |
| str "%i  %s" | `4b7e80` | `658070` | inlined | features inside PS4 658070 (inlined(0.79)) | logic lives in PS4 658070 |
| str "%s %s" | `4b7fa8` | `65d280` | intact | matched by sid:6327a51a; size 220/221; callees kept 4/4 | reuse PS4 code |
| helper | `4b81c8` | `65bf00` | intact | matched by sid:3fb98809; size 470/70c | reuse PS4 code |
| helper | `4b8638` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b8760` | `65bbd0` | intact | matched by sid:b6bd34c3; size 180/fc; callees kept 1/1 | reuse PS4 code |
| helper | `4b88e0` | `6595a0` | intact | matched by sid:4252add5; size 258/4e8; callees kept 1/1 | reuse PS4 code |
| helper | `4b8b38` | `65a380` | intact | matched by sid:b5bebbce; size 188/2e0; callees kept 1/1 | reuse PS4 code |
| helper | `4b8cc0` | `659bd0` | inlined | features inside PS4 659bd0 (inlined(1.00)) | logic lives in PS4 659bd0 |
| helper | `4b8e08` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b8ebc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "jstack <= (I32)bufferSize" | `4b9000` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%I:%M%p" | `4b9540` | `660bf0` | intact | matched by str:%I:%M%p; size 234/1a2 | reuse PS4 code |
| helper | `4b9774` | `658070` | inlined | features inside PS4 658070 (inlined(0.83)) | logic lives in PS4 658070 |
| helper | `4b992c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b99ac` | `655f90` | inlined | features inside PS4 655f90 (inlined(1.00)) | logic lives in PS4 655f90 |
| mode label lookup (table 0xb41210) | `4b9a88` | `655cf0` | partial | matched by str:%s (%i%%); size 230/294; callees kept 1/2 | compare with PS3; restore stripped calls |
| helper | `4b9cb8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b9df4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b9e4c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b9ea0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b9ef4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4b9f54` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba014` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba0c4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba174` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba1d4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba244` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba2f4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba348` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba3a4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba400` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba454` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba514` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba5a8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba614` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ba660` | `65e2e0` | intact | matched by sid:76e08710; size 140/d0; callees kept 2/2 | reuse PS4 code |
| str "%s%s" | `4ba7a0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4baa4c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bac10` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bacb4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bad48` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bae1c` | `659a90` | intact | matched by sid:69cbee70; size dc/b8; callees kept 2/2 | reuse PS4 code |
| helper | `4baef8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bafa0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bb170` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bb4a0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bb684` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bb898` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%i:%02i" | `4bbad0` | `658480` | intact | matched by sid:212b0371; size 6b8/f90; callees kept 4/5 | reuse PS4 code |
| helper | `4bc188` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| mode label list | `4bc310` | `65ecc0` | partial | matched by str:%s%s /@CFFFF8040/%i; size 2f0/4c4; callees kept 1/2 | compare with PS3; restore stripped calls |
| helper | `4bc600` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| roster/party query callback (table 0xe15d30) | `4bc728` | absent | absent | api not linked on PS4: cellVoiceGetPortAttr | reimplement on PC transport (replaces cellVoiceGetPortAttr) |
| str "%s %s" | `4bcc68` | `65a900` | intact | matched by sid:249f5194; size 304/412; callees kept 4/4 | reuse PS4 code |
| helper | `4bcf6c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bd130` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%i %s" | `4bd2c4` | `656fe0` | partial | matched by str:%i %s; size 1e4/28c; callees kept 1/2 | compare with PS3; restore stripped calls |
| str "%s %s" | `4bd4a8` | `65bf00` | inlined | features inside PS4 65bf00 (inlined(0.81)) | logic lives in PS4 65bf00 |
| str "%s /@S//@CFFF0C828/%s" | `4bd790` | `65c860` | intact | matched by str:%s /@S//@CFFF0C828/%s; size 238/230; callees kept 3/3 | reuse PS4 code |
| helper | `4bd9c8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4bdc30` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s%s" | `4bddd4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s%s" | `4be174` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4be254` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4be434` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4be6e4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4be774` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4be788` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4beba8` | `65ca90` | intact | matched by sid:6df770ee; size 23c/1e8; callees kept 3/4 | reuse PS4 code |
| str "%s %s" | `4bede4` | `65cfb0` | intact | matched by sid:222d0d79; size 2d4/2cc; callees kept 2/3 | reuse PS4 code |
| helper | `4bf0b8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s %s" | `4bf218` | `65cc80` | intact | matched by sid:2aff2f55; size 290/32c; callees kept 4/4 | reuse PS4 code |
| helper | `4bf4a8` | `65b830` | intact | matched by sid:0a7e5b3a; size 2c0/375; callees kept 3/3 | reuse PS4 code |
| str "%s %s" | `4bf768` | `65ae50` | partial | matched by sid:81cc5e93; size 360/368; callees kept 1/2 | compare with PS3; restore stripped calls |
| calls cellVoiceGetPortAttr | `4bfac8` | absent | absent | api not linked on PS4: cellVoiceGetPortAttr | reimplement on PC transport (replaces cellVoiceGetPortAttr) |
| helper | `4c0018` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s%s" | `4c0100` | `65d4b0` | intact | matched by str:+ %s%s; size 680/7f4; callees kept 4/5 | reuse PS4 code |
| helper | `4c0780` | `65c610` | intact | matched by sid:2951e0f7; size 228/248; callees kept 1/1 | reuse PS4 code |
| helper | `4c09a8` | `65a790` | intact | matched by sid:ea2119f2; size 198/16c; callees kept 1/1 | reuse PS4 code |
| helper | `4c0b40` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c0c78` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c0ea8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c1190` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s/banners/%s" | `4c1348` | `655f90` | intact | matched by str:%s/banners/%s; size 280/340; callees kept 0/1 | reuse PS4 code |
| str "%s\\%s" | `4c15c8` | `65e3b0` | intact | matched by str:%s\\%s; size 440/440; callees kept 4/5 | reuse PS4 code |
| str "- %s / %s / %s - %s" | `4c1a08` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "/@H/%s /@N/ %s%s%s" | `4c1fe0` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper of 438bc8 (MP bootstrap (NetCtl checks, PAGE_MULTIP) | `4c29f8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43a444 (NET_SM: lobby status change -> START_MAT) | `4c2a18` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c2b70` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c2bac` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**twitter page and thread**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| helper | `4ed4e0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ed684` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ed748` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ed8b0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ed8ec` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ed8fc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s/twitter.msg" | `4eda98` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `4edd24` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4edda8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "[GAME]" | `4edea0` | `675b30` | partial | matched by strfuzzy; size 470/13b | compare with PS3; restore stripped calls |
| str "[#EARNED]" | `4ee310` | `675d90` | intact | matched by sid:776754aa; size 280/51b; callees kept 3/3 | reuse PS4 code |
| str "[GAME]" | `4ee590` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "[#LEVEL]" | `4ee8e8` | `6762b0` | intact | matched by str:[#LEVEL]; size 158/14f; callees kept 1/1 | reuse PS4 code |
| str "[#COUNT]" | `4eea40` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "[#EARNED]" | `4eeee0` | `675d90` | inlined | features inside PS4 675d90 (inlined(0.76)) | logic lives in PS4 675d90 |
| str "[#CHAPTER]" | `4ef218` | `675c70` | intact | matched by str:[#CHAPTER]; size fc/11f; callees kept 1/1 | reuse PS4 code |
| str "[PROFILE]" | `4ef314` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 445020 (PAGE_MULTIPLAYER_CONNECTING) | `4ef490` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ef584` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ef600` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ef6f8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ef6fc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4ef74c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| twitter thread start | `4ef8d0` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `4ef9ac` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4efb64` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s:%s@" | `4efbdc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4efc2c` | `5b0410` | inlined | features inside PS4 5b0410 (inlined(1.00)) | logic lives in PS4 5b0410 |
| helper | `4efc6c` | `5b0410` | inlined | features inside PS4 5b0410 (inlined(1.00)) | logic lives in PS4 5b0410 |
| str "%s:%s@" | `4efd00` | `9ef610` | inlined | features inside PS4 9ef610 (inlined(1.00)) | logic lives in PS4 9ef610 |
| helper | `4efda4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4efe54` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4eff24` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f0058` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f00f0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f0178` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f01e0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f0374` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f03f0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "<omitted>" | `4f0428` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "<omitted>" | `4f0510` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "<omitted>" | `4f05dc` | absent | absent | api not linked on PS4: cellHttpUtilEscapeUri | reimplement on PC transport (replaces cellHttpUtilEscapeUri) |
| str "TwitterPostThread::Unkown event" | `4f0828` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `4f0a10` | `6766d0` | intact | matched by sid:2c738b98; size 1e8/300 | reuse PS4 code |
| helper | `4f0bf8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s/twitter.msg" | `4f0c34` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "%s:%s@" | `4f0cdc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s:%s@" | `4f0de8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f10d8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f10e8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s:%s@" | `4f10f8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s %s" | `4f1208` | `676d30` | partial | matched by sid:51994978; size 178/1fd; callees kept 1/2 | compare with PS3; restore stripped calls |
| helper of 4f14d0 (twitter NetCtl check) | `4f1380` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f13f0` | `677000` | intact | matched by sid:24089a11; size e0/c8; callees kept 1/1 | reuse PS4 code |
| twitter NetCtl check | `4f14d0` | absent | absent | api not linked on PS4: cellNetCtlInit | reimplement on PC transport (replaces cellNetCtlGetState, cellNetCtlInit) |
| helper | `4f15cc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "twitter" | `4f1744` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| PAGE_TWITTER | `4f1950` | `6769d0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `4f195c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| BUTTON_TWITTER_* | `4f1c38` | `6769e0` | stub ret | stub(ret), matched by menucb | reimplement from PS3; PS4 keeps only the slot |
| helper | `4f1c44` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f1ca4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4f1d34` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |


### Bootstrap, NetInit, NP init/term, voice, teardown

**flow.cpp: NetInit thread**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| NetInit thread (NP, tickets, NAT probes, sockets, session, signaling, voice, score) | `447a5c` | absent | absent | api not linked on PS4: cellNetCtlGetNatInfo cellNetCtlInit gethostbyname sceNpBasicRegisterHandler | reimplement on PC transport (replaces cellNetCtlGetNatInfo, cellNetCtlInit, gethostbyname) |
| str "playercount" | `4496ac` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| sceNpBasic event handler | `449ae8` | absent | absent | api not linked on PS4: sceNpBasicGetEvent sceNpBasicRecvMessageAttachmentLoad | reimplement on PC transport (replaces sceNpBasicGetEvent, sceNpBasicRecvMessageAttachmentLoad) |

**net-voice: cellVoice**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| voice write | `4e6cd0` | absent | absent | api not linked on PS4: cellVoiceWriteToIPort | reimplement on PC transport (replaces cellVoiceWriteToIPort) |
| calls cellVoiceGetVolume | `4e6e24` | absent | absent | api not linked on PS4: cellVoiceGetVolume | reimplement on PC transport (replaces cellVoiceGetVolume) |
| calls cellVoiceSetVolume | `4e6e64` | absent | absent | api not linked on PS4: cellVoiceSetVolume | reimplement on PC transport (replaces cellVoiceSetVolume) |
| calls cellVoiceSetVolume | `4e6e94` | absent | absent | api not linked on PS4: cellVoiceSetVolume | reimplement on PC transport (replaces cellVoiceSetVolume) |
| calls cellVoiceGetMuteFlag | `4e6ecc` | absent | absent | api not linked on PS4: cellVoiceGetMuteFlag | reimplement on PC transport (replaces cellVoiceGetMuteFlag) |
| calls cellVoiceSetMuteFlag | `4e6f04` | absent | absent | api not linked on PS4: cellVoiceSetMuteFlag | reimplement on PC transport (replaces cellVoiceSetMuteFlag) |
| calls cellVoiceSetMuteFlag | `4e6f50` | absent | absent | api not linked on PS4: cellVoiceSetMuteFlag | reimplement on PC transport (replaces cellVoiceSetMuteFlag) |
| calls cellVoiceGetPortAttr cellVoiceSetPortAttr | `4e6f78` | absent | absent | api not linked on PS4: cellVoiceGetPortAttr cellVoiceSetPortAttr | reimplement on PC transport (replaces cellVoiceGetPortAttr, cellVoiceSetPortAttr) |
| voice end | `4e7038` | absent | absent | api not linked on PS4: cellVoiceDeletePort cellVoiceEnd cellVoiceStop | reimplement on PC transport (replaces cellVoiceDeletePort, cellVoiceEnd, cellVoiceStop) |
| voice init | `4e71d8` | absent | absent | api not linked on PS4: cellVoiceConnectIPortToOPort cellVoiceCreatePort cellVoiceInit cellVoiceStart | reimplement on PC transport (replaces cellVoiceConnectIPortToOPort, cellVoiceCreatePort, cellVoiceInit) |
| helper of 43cdb8 (NetConnectionUpdate (recv loop, sendto)) | `4e7464` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| voice read | `4e7560` | absent | absent | api not linked on PS4: cellVoiceGetPortInfo cellVoiceReadFromOPort | reimplement on PC transport (replaces cellVoiceGetPortInfo, cellVoiceReadFromOPort) |
| voice loopback | `4e77e0` | absent | absent | api not linked on PS4: cellVoiceConnectIPortToOPort cellVoiceCreatePort cellVoiceSetMuteFlag | reimplement on PC transport (replaces cellVoiceConnectIPortToOPort, cellVoiceCreatePort, cellVoiceSetMuteFlag) |
| helper | `4e7988` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**np-util: error text, NetCtl dialog, sceNp init/term**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| NP error code to text | `4eae30` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NetCtl init + start dialog | `4ebf7c` | absent | absent | api not linked on PS4: cellNetCtlInit cellNetCtlNetStartDialogLoadAsync | reimplement on PC transport (replaces cellNetCtlInit, cellNetCtlNetStartDialogLoadAsync) |
| sceNpManagerGetStatus check | `4ec068` | absent | absent | api not linked on PS4: sceNpManagerGetStatus | reimplement on PC transport (replaces sceNpManagerGetStatus) |
| sceNp/sceNp2 term | `4ec0f0` | absent | absent | api not linked on PS4: sceNp2Term sceNpTerm | reimplement on PC transport (replaces sceNp2Term, sceNpTerm) |
| NP communication id select | `4ec120` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| sceNp/sceNp2 init | `4ec184` | absent | absent | api not linked on PS4: sceNp2Init sceNpInit | reimplement on PC transport (replaces sceNp2Init, sceNpInit) |


### Session, room, party, friends, signaling

**net-friends: presence, friends, players met**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| NetFriends set presence | `48f16c` | absent | absent | api not linked on PS4: sceNpBasicSetPresenceDetails | reimplement on PC transport (replaces sceNpBasicSetPresenceDetails) |
| helper of 43f830 (LIST_FRIENDS) | `48f268` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "NetFriend::UpdatePresence" | `48f2b8` | absent | absent | api not linked on PS4: sceNpBasicGetFriendPresenceByNpId | reimplement on PC transport (replaces sceNpBasicGetFriendPresenceByNpId) |
| NetFriends::RoomMemberCallback | `48f350` | absent | absent | api not linked on PS4: sceNpMatching2ClearEventData sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2ClearEventData, sceNpMatching2GetEventData) |
| helper | `48f558` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| calls sceNpUtilCmpNpId | `48f5a8` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| calls sceNpUtilCmpNpId | `48f654` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| str "Load Players Met" | `48f79c` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "jstack <= (I32)bufferSize" | `48f858` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `48ff00` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 449ae8 (sceNpBasic event handler) | `48ffe0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetFriends friend list thread | `490040` | absent | absent | api not linked on PS4: sceNpBasicGetFriendListEntryCount sceNpBasicGetFriendPresenceByIndex sys_net_free_thread_context | reimplement on PC transport (replaces sceNpBasicGetFriendListEntryCount, sceNpBasicGetFriendPresenceByIndex, sys_net_free_thread_context) |
| helper | `490170` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "Load Friends and Players Met" | `490178` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NetFriends presence update | `490338` | absent | absent | api not linked on PS4: sceNpBasicSetPresenceDetails | reimplement on PC transport (replaces sceNpBasicSetPresenceDetails) |
| helper of 4386d0 (MP cleanup / abort (Matching2 term, sock) | `490878` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4909a8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetFriends add players history thread | `490a88` | absent | absent | api not linked on PS4: sceNpBasicAddPlayersHistory sceNpUtilCmpNpId sys_net_free_thread_context | reimplement on PC transport (replaces sceNpBasicAddPlayersHistory, sceNpUtilCmpNpId, sys_net_free_thread_context) |
| helper | `490c20` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| calls sceNpMatching2GetRoomMemberDataExternalList sceNpUtilCmpNpId | `490c28` | absent | absent | api not linked on PS4: sceNpMatching2GetRoomMemberDataExternalList sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpMatching2GetRoomMemberDataExternalList, sceNpUtilCmpNpId) |
| calls sceNpUtilCmpNpId | `490f60` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| str "Add Players Met" | `491028` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| NetFriends players met thread | `491580` | absent | absent | api not linked on PS4: sceNpBasicGetPlayersHistoryEntry sceNpBasicGetPlayersHistoryEntryCount sys_net_free_thread_context | reimplement on PC transport (replaces sceNpBasicGetPlayersHistoryEntry, sceNpBasicGetPlayersHistoryEntryCount, sys_net_free_thread_context) |
| helper | `491690` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491698` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4916f0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491720` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491768` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `49177c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `49181c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491878` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4918bc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43bdd0 (NpNetUpdate (connection management, time) | `4918e8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43bdd0 (NpNetUpdate (connection management, time) | `4919b0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43bdd0 (NpNetUpdate (connection management, time) | `4919d4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 445608 (NET_SM -> LEAVE_GAME, lobby choice SERVE) | `4919dc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4919e4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 490338 (NetFriends presence update) | `4919fc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4b0a58 (PAGE_MULTIPLAYER_MATCHMAKING (matchmakin) | `491a04` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491a60` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491ab0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491af4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 490338 (NetFriends presence update) | `491b24` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491b34` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491b60` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491be8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491ca0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491d08` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `491d70` | `5f6880` | inlined | features inside PS4 5f6880 (inlined(1.00)) | logic lives in PS4 5f6880 |
| helper | `491e00` | `5f1000` | inlined | features inside PS4 5f1000 (inlined(1.00)) | logic lives in PS4 5f1000 |
| helper | `491e30` | `5b0070` | inlined | features inside PS4 5b0070 (inlined(1.00)) | logic lives in PS4 5b0070 |
| helper | `491ea8` | `64ab70` | inlined | features inside PS4 64ab70 (inlined(1.00)) | logic lives in PS4 64ab70 |
| helper | `491f38` | `5f2830` | inlined | features inside PS4 5f2830 (inlined(1.00)) | logic lives in PS4 5f2830 |
| helper | `491fc4` | `559850` | inlined | features inside PS4 559850 (inlined(1.00)) | logic lives in PS4 559850 |
| helper | `492058` | `655810` | inlined | features inside PS4 655810 (inlined(1.00)) | logic lives in PS4 655810 |
| helper | `492104` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `492148` | `64ade0` | intact | matched by callee; size 150/d0; callees kept 1/1 | reuse PS4 code |
| helper | `492298` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `492340` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4925e0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**net-party: party messages, party error**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| party: send message | `4c2bc0` | absent | absent | api not linked on PS4: sceNpBasicSendMessage | reimplement on PC transport (replaces sceNpBasicSendMessage) |
| str "%i messages :" | `4c2d18` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| party: get message | `4c2e08` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "Post Message %x, size %i" | `4c3018` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| party: post message | `4c3258` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "Get Message %x, size %i" | `4c3380` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `4c36a8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c36ac` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4465c8 (PAGE_MULTIPLAYER (Callback_Multiplayer)) | `4c36b0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4386d0 (MP cleanup / abort (Matching2 term, sock) | `4c36c8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c36e8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c3708` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4661e0 (PAGE_MULTIPLAYER_JOINING (client join st) | `4c370c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4b0a58 (PAGE_MULTIPLAYER_MATCHMAKING (matchmakin) | `4c3764` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4661e0 (PAGE_MULTIPLAYER_JOINING (client join st) | `4c37c8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43aab0 (BUTTON_MULTIPLAYER_STARTMATCH) | `4c38f0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c39d0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c39f4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| party: member left | `4c3a18` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper of 4386d0 (MP cleanup / abort (Matching2 term, sock) | `4c3ac0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 447a5c (NetInit thread (NP, tickets, NAT probes,) | `4c3b00` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 465da0 (custom game: host/join decision) | `4c3b58` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 465da0 (custom game: host/join decision) | `4c3d00` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4b0a58 (PAGE_MULTIPLAYER_MATCHMAKING (matchmakin) | `4c3dc8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 465da0 (custom game: host/join decision) | `4c3e38` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%s joined party" | `4c40f0` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NET_SM_PARTY_ERROR handler | `4c41f8` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `4c4a30` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c4a78` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c4a88` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4c4aa0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**net-session-manager.cpp: Matching2 context, callbacks**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| Matching2 DeleteServerContext callback | `4cff50` | absent | absent | api not linked on PS4: sceNpMatching2ClearEventData | reimplement on PC transport (replaces sceNpMatching2ClearEventData) |
| Matching2 CreateServerContext callback | `4d0040` | absent | absent | api not linked on PS4: sceNpMatching2ClearEventData | reimplement on PC transport (replaces sceNpMatching2ClearEventData) |
| Matching2 request callback (default opt) | `4d0130` | absent | absent | api not linked on PS4: sceNpMatching2ClearEventData | reimplement on PC transport (replaces sceNpMatching2ClearEventData) |
| helper of 447a5c (NetInit thread (NP, tickets, NAT probes,) | `4d01a8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| Matching2 GetServerInfo callback | `4d02c8` | absent | absent | api not linked on PS4: sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2GetEventData) |
| room event processing (session event queue drain) | `4d040c` | absent | absent | api not linked on PS4: sceNpMatching2ClearEventData sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2ClearEventData, sceNpMatching2GetEventData) |
| helper of 449e88 (NdiNetUpdate per frame (from FrameBegin ) | `4d0bd0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| room event callback (queues SESSION events) | `4d0c88` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| Matching2 context callback | `4d0d90` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| Matching2 GetWorldInfoList callback | `4d0e78` | absent | absent | api not linked on PS4: sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2GetEventData) |
| g_sessionManager.Init (Matching2 context, callbacks, server/world) | `4d0fc0` | absent | absent | api not linked on PS4: sceNpMatching2AbortContextStart sceNpMatching2ContextStartAsync sceNpMatching2CreateContext sceNpMatching2CreateServerContext | reimplement on PC transport (replaces sceNpManagerGetNpId, sceNpMatching2AbortContextStart, sceNpMatching2ContextStartAsync) |
| g_sessionManager.Term (Matching2) | `4d1a00` | absent | absent | api not linked on PS4: sceNpMatching2ClearEventData sceNpMatching2ContextStop sceNpMatching2DeleteServerContext sceNpMatching2DestroyContext | reimplement on PC transport (replaces sceNpMatching2ClearEventData, sceNpMatching2ContextStop, sceNpMatching2DeleteServerContext) |
| room message callback | `4d1c80` | absent | absent | api not linked on PS4: sceNpMatching2ClearEventData sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2ClearEventData, sceNpMatching2GetEventData) |
| helper of 447a5c (NetInit thread (NP, tickets, NAT probes,) | `4d1e2c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d1ebc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43aab0 (BUTTON_MULTIPLAYER_STARTMATCH) | `4d1ec8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43aab0 (BUTTON_MULTIPLAYER_STARTMATCH) | `4d1ed4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43bdd0 (NpNetUpdate (connection management, time) | `4d1f08` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 465da0 (custom game: host/join decision) | `4d2080` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43bdd0 (NpNetUpdate (connection management, time) | `4d21d4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d234c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43a444 (NET_SM: lobby status change -> START_MAT) | `4d2370` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetSession::IsHost | `4d2394` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4c41f8 (NET_SM_PARTY_ERROR handler) | `4d23cc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 444bb0 (NET_SM -> CREATE_GAME_WAIT (CreateJoinRo) | `4d23d4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 444bb0 (NET_SM -> CREATE_GAME_WAIT (CreateJoinRo) | `4d23dc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 465da0 (custom game: host/join decision) | `4d23e4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4654d0 (PAGE_MULTIPLAYER_JOIN) | `4d2410` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4661e0 (PAGE_MULTIPLAYER_JOINING (client join st) | `4d245c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43aab0 (BUTTON_MULTIPLAYER_STARTMATCH) | `4d24a8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d2668` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4386d0 (MP cleanup / abort (Matching2 term, sock) | `4d26b0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d26dc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d26e8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d26f4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**net-session.cpp: rooms, members, room data**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| str "[%s] %s" | `4d2700` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "jstack <= (I32)bufferSize" | `4d2790` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 465808 (BUTTON_MULTIPLAYER_SELECT_JOIN_GAME) | `4d2e68` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetSession set member data | `4d2ed4` | absent | absent | api not linked on PS4: sceNpMatching2SetRoomMemberDataInternal | reimplement on PC transport (replaces sceNpMatching2SetRoomMemberDataInternal) |
| helper | `4d3060` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d3748` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d3754` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetSession ping info | `4d3848` | absent | absent | api not linked on PS4: sceNpMatching2SignalingGetPingInfo | reimplement on PC transport (replaces sceNpMatching2SignalingGetPingInfo) |
| str "********************** ROOM NAME '%s' ********************" | `4d3920` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NetSession grant owner | `4d3a10` | absent | absent | api not linked on PS4: sceNpMatching2GrantRoomOwner | reimplement on PC transport (replaces sceNpMatching2GrantRoomOwner) |
| NetSession set room data external | `4d3af8` | absent | absent | api not linked on PS4: sceNpMatching2SetRoomDataExternal | reimplement on PC transport (replaces sceNpMatching2SetRoomDataExternal) |
| NetSession SearchRoom | `4d3c50` | absent | absent | api not linked on PS4: sceNpManagerGetAccountRegion sceNpManagerGetMyLanguages sceNpMatching2SearchRoom | reimplement on PC transport (replaces sceNpManagerGetAccountRegion, sceNpManagerGetMyLanguages, sceNpMatching2SearchRoom) |
| calls sceNpUtilCmpNpId | `4d4020` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| NetSession CreateJoinRoom | `4d421c` | absent | absent | api not linked on PS4: sceNpManagerGetAccountRegion sceNpManagerGetMyLanguages sceNpMatching2CreateJoinRoom | reimplement on PC transport (replaces sceNpManagerGetAccountRegion, sceNpManagerGetMyLanguages, sceNpMatching2CreateJoinRoom) |
| NetSession JoinRoom | `4d4578` | absent | absent | api not linked on PS4: sceNpMatching2JoinRoom | reimplement on PC transport (replaces sceNpMatching2JoinRoom) |
| str "SetRoomDataInternal %lld 0x%x" | `4d479c` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NetSession set host rank | `4d4868` | absent | absent | api not linked on PS4: sceNpMatching2SetRoomDataInternal | reimplement on PC transport (replaces sceNpMatching2SetRoomDataInternal) |
| str "SetRoomDataInternal %lld 0x%x" | `4d4a0c` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NetSession set host migrate | `4d4b28` | absent | absent | api not linked on PS4: sceNpMatching2SetRoomDataInternal | reimplement on PC transport (replaces sceNpMatching2SetRoomDataInternal) |
| str "SetRoomDataInternal %lld 0x%x" | `4d4cf0` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "RTT Room Id %lld : %i ms" | `4d4e0c` | absent | absent | api not linked on PS4: sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2GetEventData) |
| str "GrantRoomOwnder %lld 0x%x" | `4d4f68` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "KickoutRoomMember %lld 0x%x" | `4d5034` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "SetRoomDataExternal %lld 0x%x" | `4d5100` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "SetRoomDataInternal %lld 0x%x" | `4d521c` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NetSession hide/unhide room | `4d5338` | absent | absent | api not linked on PS4: sceNpMatching2SetRoomDataInternal | reimplement on PC transport (replaces sceNpMatching2SetRoomDataInternal) |
| str "SetRoomDataInternal %lld 0x%x" | `4d550c` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "Session SearchRoom Failed 0x%x" | `4d5628` | absent | absent | api not linked on PS4: sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2GetEventData) |
| NetSession set room data internal | `4d57b0` | absent | absent | api not linked on PS4: sceNpMatching2SetRoomDataInternal | reimplement on PC transport (replaces sceNpMatching2SetRoomDataInternal) |
| helper of 465808 (BUTTON_MULTIPLAYER_SELECT_JOIN_GAME) | `4d5940` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetSession find member by NpId | `4d5a18` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| NetSession member info (avatar url, NpId) | `4d5e38` | absent | absent | api not linked on PS4: sceNpManagerGetAvatarUrl | reimplement on PC transport (replaces sceNpManagerGetAvatarUrl, sceNpManagerGetNpId) |
| helper of 43aab0 (BUTTON_MULTIPLAYER_STARTMATCH) | `4d6a08` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d6ab0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 440620 (BUTTON_MULTIPLAYER_PLAYER0..9) | `4d6d10` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 440620 (BUTTON_MULTIPLAYER_PLAYER0..9) | `4d6fa0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetSession remove member | `4d7200` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| helper of 4d040c (room event processing (session event que) | `4d74b8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetSession add member | `4d76e8` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| JoinRoom response | `4d7e48` | absent | absent | api not linked on PS4: sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2GetEventData) |
| CreateJoinRoom response | `4d8220` | absent | absent | api not linked on PS4: sceNpMatching2GetEventData | reimplement on PC transport (replaces sceNpMatching2GetEventData) |
| NetSession LeaveRoom | `4d8540` | absent | absent | api not linked on PS4: sceNpMatching2LeaveRoom | reimplement on PC transport (replaces sceNpMatching2LeaveRoom) |
| NetSession KickoutRoomMember | `4d8a98` | absent | absent | api not linked on PS4: sceNpMatching2KickoutRoomMember | reimplement on PC transport (replaces sceNpMatching2KickoutRoomMember) |
| LeaveRoom response | `4d8bf4` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper of 4386d0 (MP cleanup / abort (Matching2 term, sock) | `4d8e88` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d90e0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d911c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d912c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d9178` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d9398` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**slot manager**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| slot manager remove user | `4d987c` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| helper | `4d9a78` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4d9af0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| slot manager add/update user | `4d9be8` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper of 4da278 (slot manager room id) | `4da168` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| slot manager room id | `4da278` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper of 465da0 (custom game: host/join decision) | `4da3f8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4da434` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4da438` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4da474` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4da484` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4da504` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**np-connection-manager: sceNpSignaling**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| calls sceNpUtilCmpNpId | `4e8978` | absent | absent | api not linked on PS4: sceNpUtilCmpNpId | reimplement on PC transport (replaces sceNpUtilCmpNpId) |
| helper of 4e9618 (signaling callback) | `4e8a50` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NpConnectionManager term (signaling ctx) | `4e8afc` | absent | absent | api not linked on PS4: sceNpSignalingDestroyCtx | reimplement on PC transport (replaces sceNpSignalingDestroyCtx, sceNpSignalingTerminateConnection) |
| NpConnectionManager terminate connection | `4e8c8c` | `675200` | partial (dead) | matched by only out-of-line PS4 caller of sceNpSignalingTerminateConnection besides inlined copies; neighbours 6752b0/675370/675400 hold the other wrappers; size 204/78; callees kept 0/1; PS4 dead (no reference) | compare with PS3; restore stripped calls |
| helper | `4e8e90` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NpConnectionManager connection status | `4e8f1c` | absent | absent | api not linked on PS4: sceNpSignalingGetConnectionStatus | reimplement on PC transport (replaces sceNpSignalingGetConnectionStatus) |
| NpConnectionManager activate connection | `4e9060` | `6752b0` | partial (dead) | matched by only out-of-line PS4 wrapper of sceNpSignalingActivateConnection; size 264/55; callees kept 0/1; PS4 dead (no reference) | compare with PS3; restore stripped calls |
| helper of 447248 (menu-temple / MP level select helper) | `4e92c4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43cdb8 (NetConnectionUpdate (recv loop, sendto)) | `4e92d0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NpConnectionManager connection info A | `4e9338` | `675370` | intact (dead) | matched by PS4 wrapper of sceNpSignalingGetConnectionInfo (first of two); size c8/8f; PS4 dead (no reference) | reuse PS4 code |
| NpConnectionManager connection info B | `4e9400` | `675400` | intact (dead) | matched by PS4 wrapper of sceNpSignalingGetConnectionInfo (second of two); size c8/8f; PS4 dead (no reference) | reuse PS4 code |
| helper of 43bdd0 (NpNetUpdate (connection management, time) | `4e94c8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 442558 (TaskManagerOnlineUpdate) | `4e954c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| signaling callback | `4e9618` | absent | absent | api not linked on PS4: inet_ntoa sceNpSignalingGetConnectionStatus sceNpSignalingGetPeerNetInfoResult | reimplement on PC transport (replaces inet_ntoa, sceNpSignalingGetConnectionStatus, sceNpSignalingGetPeerNetInfoResult) |
| g_npConnectionManager.Init (signaling ctx) | `4e9d28` | absent | absent | api not linked on PS4: sceNpSignalingAddExtendedHandler sceNpSignalingCreateCtx | reimplement on PC transport (replaces sceNpManagerGetNpId, sceNpSignalingAddExtendedHandler, sceNpSignalingCreateCtx) |
| NpConnectionManager connect by NpId | `4e9e98` | absent | absent | api not linked on PS4: inet_ntoa sceNpSignalingGetConnectionFromNpId sceNpSignalingGetConnectionStatus sceNpUtilCmpNpId | reimplement on PC transport (replaces inet_ntoa, sceNpSignalingActivateConnection, sceNpSignalingGetConnectionFromNpId) |
| signaling extended handler / connection update | `4ea438` | absent | absent | api not linked on PS4: inet_ntoa sceNpSignalingGetConnectionStatus sceNpUtilCmpNpId | reimplement on PC transport (replaces inet_ntoa, sceNpSignalingActivateConnection, sceNpSignalingGetConnectionInfo) |


### Transport: sockets, connection, reliability, per-frame update

**flow.cpp: net clock, NpNetUpdate, NetConnectionUpdate**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| net clock "Net Clock = %f" | `43b698` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `43b738` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43b794` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43b7f0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43b844` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43b898` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43b8ec` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43b940` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43b99c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43b9f0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43ba44` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43ba98` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43baec` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bb10` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bb34` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bb58` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bb7c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bba0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bbc4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bbe8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bc0c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| calls sys_net_free_thread_context | `43bc30` | absent | absent | api not linked on PS4: sys_net_free_thread_context | reimplement on PC transport (replaces sys_net_free_thread_context) |
| helper | `43bc80` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 444660 (NET_SM -> UPDATE (starts the net game)) | `43bcbc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bd28` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43bd68` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NpNetUpdate (connection management, timeouts, kick) | `43bdd0` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NetConnectionUpdate (recv loop, sendto) | `43cdb8` | absent | absent | api not linked on PS4: inet_ntoa | reimplement on PC transport (replaces inet_ntoa) |
| helper | `43d5d8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43d610` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `43d660` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**flow.cpp: NdiNetUpdate (per-frame net update)**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| NdiNetUpdate per frame (from FrameBegin 0x21db8) | `449e88` | absent | absent | api not linked on PS4: cellNetCtlGetInfo cellVoiceGetPortAttr | reimplement on PC transport (replaces cellNetCtlGetInfo, cellVoiceGetPortAttr) |

**net-connection.cpp: reliability window, ack**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| NetConnection ack processing / packet header | `464cb8` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| NetConnection send window helper | `465138` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 43bdd0 (NpNetUpdate (connection management, time) | `4651b4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetConnection read packet | `465218` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| NetConnection write packet / window full | `4652cc` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `465484` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4654c0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**net-connection.cpp/net-message: message layer**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| helper of 447a5c (NetInit thread (NP, tickets, NAT probes,) | `466fe0` | `b88ec0` | inlined | features inside PS4 b88ec0 (inlined(1.00)) | logic lives in PS4 b88ec0 |
| helper of 449e88 (NdiNetUpdate per frame (from FrameBegin ) | `467160` | `b88ec0` | inlined | features inside PS4 b88ec0 (inlined(1.00)) | logic lives in PS4 b88ec0 |
| helper | `467378` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "<omitted>" | `4673c0` | absent | absent | matcher pair rejected: matcher pair rests on a fuzzy string match only | port from PS3 code |
| helper of 43cdb8 (NetConnectionUpdate (recv loop, sendto)) | `467438` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4674e0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4674ec` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `467528` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| message layer: wait seq | `467538` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| message layer: pack (1200 byte packet) | `467600` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| helper | `467ae8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `467b5c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `467ba0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| message layer: expire | `467d20` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |
| str "%i %x" | `467e88` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 4e8afc (NpConnectionManager term (signaling ctx)) | `468658` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `4688a8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468a18` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468c10` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468c18` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468c1c` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468c20` | `634970` | inlined | features inside PS4 634970 (inlined(1.00)) | logic lives in PS4 634970 |
| helper | `468d70` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468dc0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468dc4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468dc8` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468dcc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468dd0` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper of 467600 (message layer: pack (1200 byte packet)) | `468dd4` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468e34` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468e70` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| helper | `468e88` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| str "%40s %8i %8i   %3.2f %c    %4i %4i   %4i" | `468ed8` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |

**net-socket: UDP sockets**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| helper of 447a5c (NetInit thread (NP, tickets, NAT probes,) | `4db1cc` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |
| UDP sendto | `4db1d0` | absent | absent | api not linked on PS4: sendto | reimplement on PC transport (replaces sendto) |
| socket close | `4db600` | absent | absent | api not linked on PS4: socketclose | reimplement on PC transport (replaces socketclose) |
| g_udpSocketWorld.Init (bind localhost) | `4db630` | absent | absent | api not linked on PS4: _sys_net_errno_loc bind setsockopt socket | reimplement on PC transport (replaces _sys_net_errno_loc, bind, setsockopt) |
| g_udpSocket.Init (P2P, SO_USECRYPTO) | `4db7d4` | absent | absent | api not linked on PS4: _sys_net_errno_loc bind setsockopt socket | reimplement on PC transport (replaces _sys_net_errno_loc, bind, setsockopt) |
| UDP recvfrom | `4dba48` | absent | absent | api not linked on PS4: recvfrom | reimplement on PC transport (replaces recvfrom) |
| helper of 43cdb8 (NetConnectionUpdate (recv loop, sendto)) | `4dbc98` | absent | absent | no PS4 match (str/sid/callee/vtable/score passes) | port from PS3 code |

**net-stats: bandwidth**

| role | PS3 1.10 | PS4 1.02 | PS4 state | evidence | porting note |
|---|---|---|---|---|---|
| bandwidth stats | `4dbe70` | absent | absent | no PS4 match; its strings are not in the PS4 image | port from PS3 code |

