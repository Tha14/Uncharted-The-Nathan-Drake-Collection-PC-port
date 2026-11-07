# Uncharted 2 PS3 1.10 multiplayer address map

Target: `NPUA80698/USRDIR/EBOOT.elf` (PSN, APP_VER 1.10), decrypted PPU ELF,
SHA-256 `fdac662847ab13252e8c423a902dc8e2a98513f584da70134c7abba00c19fd1a`.
PowerPC64 big-endian, 32-bit pointers. Text `0x10230..0xadfbc0`, import stubs
`0xadfbe4..`, OPD `0xdd8928..0xe103b8` (28,496 descriptors), TOC `r2 = 0xe183b8`.

PS4 side: the prepared flat image `analysis/big2-ps4_Shipping/boot-linked.bin` of the
Collection 1.02 executable (image address = file offset - 0x183ce8). PS4 entries below
were checked by the scripts in this directory. Nothing else was carried over.

The map was built from scratch from these two executables. It is static evidence only.
Nothing was executed. No game file was modified. No game bytes are stored in the repo.

## 1. Method and how to reproduce

Tools (all read-only, in `u2mp/tools/`):

| Script | What it does |
|---|---|
| `ps3_elf.py` | ELF loader. Parses `.lib.stub`, names NIDs by hashing candidate names (SHA-1 of name + NID suffix). Finds OPD and TOC. Decodes the whole text once with per-function constant propagation (lis/addis/addi/ori/oris/lwz/mr; TOC pool loads; epilogue restores ignored). Records bl call sites, indirect bctrl calls through OPDs, computed addresses, memory accesses and 32-bit constants. Cache: `work/ps3-110/` (git-ignored). |
| `ps3_map.py` | Queries: `imports`, `strings <re>` (each string with its code sites, also through pointer pools), `xref`, `callers`, `callees`, `const <hex>`, `sid <NAME>`, `funcs <a-b>` (one line per function: size, callers, OPD, data slots holding the OPD, imports called, first strings), `netcallers`. |
| `ps3_dis.py` | `clang --target=powerpc64-linux` + `.incbin`, then `llvm-objdump -d`. Use `--annotate CACHE` to add import names, strings, OPD targets, constants and TOC values. Takes a range or a function. |
| `ps4_xref.py` | PS4 flat image: strings with RIP-relative lea/mov refs, 32-bit immediates, and x86 disassembly. Function starts are a padding heuristic, so treat them as approximate. |
| `u2_menu_callbacks.py` | Pairs PS3 menu control/page callbacks with PS4 ones by control string id. PS3 side: the registration calls reached from `0xed9d8ae1`. PS4 side: the `mov [rbx+8*rax+disp], id ; lea rcx, cb` pattern. It flags PS4 callbacks that are a bare `ret`. |
| `u2_string_pairs.py` | Pairs PS3 and PS4 functions that reference the same unique string. |

LLVM tools come from MSYS2 clang64 (`C:/msys64/clang64/bin`), or set `PS3_LLVM_BIN`.
NID names come from `rpcs3.exe` identifiers plus a built-in list of sys_net BSD names.
All 42 libraries resolved and no NID was left unnamed. Rizin and REA were not used.

```
set E="D:/2. PS Games/PS3/NPUA80698/USRDIR/EBOOT.elf"
set B="D:/1. PC Games/Uncharted-windows/analysis/big2-ps4_Shipping/boot-linked.bin"
python u2mp/tools/ps3_map.py %E% imports            (first run builds the cache, ~5 s)
python u2mp/tools/ps3_map.py %E% netcallers
python u2mp/tools/ps3_map.py %E% strings "game/net/"
python u2mp/tools/ps3_map.py %E% sid BUTTON_MULTIPLAYER
python u2mp/tools/ps3_map.py %E% funcs 4d0c00-4d2000
python u2mp/tools/ps3_dis.py %E% 438bc8 --annotate work/ps3-110
python u2mp/tools/u2_menu_callbacks.py %E% %B% --names work/ps3-110/names.txt
python u2mp/tools/u2_string_pairs.py %E% %B% --range 42f000-4f2000
```

`names.txt` lists the upper-case identifiers in the PS3 `build/main/dc1.psarc` members
`menu.bin`, `part-menu.bin` and `net.bin`, read with `psarc_lib.py`. Their CRC-32 ids
(MSB-first, polynomial 0x04C11DB7) label the callback table.

Ways addresses were found:
- **str**: a unique assert, log or source-file string referenced by the function.
  Most strings are reached through the per-module TOC pool, for example
  `lwz r30,-0x78dc(r2)` and then `lwz r3,k(r30)`.
- **imp**: the function calls the named import stub.
- **sid**: a 32-bit string-id immediate, or a registration with an id and an OPD.
- **call**: call graph from an already identified function.
- **glob**: a global loaded from the TOC pool and passed as `this`.

## 2. Map

PS4 column (Collection 1.02, checked here):
- `RET`: the callback is a bare `ret`.
- `code`: the callback has a body.
- `~`: the PS4 function was matched by a shared string or id; its start is heuristic.
- `-`: not found or not checked.

### Entry, init, teardown

| Role | Evidence | PS3 1.10 | PS4 1.02 |
|---|---|---|---|
| Control registration routine (id, OPD, ctx) | sid: called right after `ori r4,..,0x8ae1` building `0xed9d8ae1` | `0x422174` (page/state variant `0x421b44`) | registration stores at `5ed9db` |
| Registration of the MP controls | sid, call | function `0x43918c`; BUTTON_MULTIPLAYER site `0x4391dc` | `5ed9db` |
| BUTTON_MULTIPLAYER callback | OPD `0xdeedc0`; logs "Multiplayer"; calls `0x93904`, then bootstrap | `0x43b150` | `5eb160` **RET** |
| MP bootstrap | call from `0x43b1b4`. Logs "multi-player" and "Game Data Version = %x". Calls cellNetCtlInit and cellNetCtlGetState (needs state 3), sceNpManagerGetContentRatingFlag. Transition `0x429a08(menu 0xf0e800, PAGE_MULTIPLAYER_CONNECTING 0x9e1863b1)`. Starts thread "NetInit" via sys_ppu_thread_create, OPD `0xdef120`. Then calls `0x775370`. Logs "AutoMatchmaking". | `0x438bc8` | - |
| Bootstrap failure message | `0x42a3e8(menu, 0x88a082af, 0xc6467f2d)` | `0x438c7c` | id `0x88a082af` at `5a0d6b` |
| Cleanup / abort | the callback tail-calls it when bootstrap fails. Calls sceNpBasicUnregisterHandler, `0x4d1a00` (Matching2 term), socketclose ×2, `0x4e8afc` (signaling term), `0x4e7038` (voice end), sceNpCommerce2Term, `0x4ab66c` (score term), `0x4c41f8` | `0x4386d0` | - |
| NetInit thread | str "NetInit Started" / "NetInit Done" / "ERROR NET INIT %x" | `0x447a5c` | - |
| NetInit: NP side | imp: bandwidth test, GetNpId → `0xf1fa48+0x288`, RegisterCallback, RequestTicket2/GetTicket, DrmIsAvailable, sceNpCommerce2Init, sceNpBasicRegisterHandler (`0x448e08`) | inside `0x447a5c` | - |
| g_sessionManager.Init | str "g_sessionManager.Init()() failed"; this = `0xf11978` | `0x4d0fc0` (call `0x448e24`) | - |
| g_udpSocket.Init (P2P port, SO_USECRYPTO) | str; this = `0xf10e18` | `0x4db7d4` (call `0x448f50`) | - |
| g_udpSocketWorld.Init | str "bind to localhost"; this = `0xf11270` | `0x4db630` (call `0x448f84`) | - |
| g_npConnectionManager.Init (signaling ctx) | str; imp sceNpSignalingCreateCtx/AddExtendedHandler; this = `0xf3a060` | `0x4e9d28` (call `0x448fb0`) | - |
| Other NetInit object setup | call order after session Init: `0x4cfe38`, `0x4d1e2c(0xf20c48)`, `0x4c3b00(0xf2c8a0)`, `0x4d01a8(mgr, obj)` ×2, `0x4d5338(0xf2c8a0,1)`, `0x4cfe88`, `0x4d1ec8` | `0x448e3c..0x448f44` | - |
| Voice init | imp cellVoiceInit/CreatePort/ConnectIPortToOPort/Start; called from NetInit `0x448dec` | `0x4e71d8` | - |
| Per-frame net update "NdiNetUpdate" | str. Called from frame function `0x21db8` ("FrameBegin"). Calls NpNetUpdate and NetConnectionUpdate. Sends heartbeat and get-location. | `0x449e88` | - |
| NP basic event handler | str "sceNpBasicGetEvent() failed"; OPD passed to RegisterHandler | `0x449ae8` (also `0x438a50` "basicCb") | - |
| Initialized-flag | written by NetInit and cleanup | byte `0xf10e14` | - |

### Session manager / Matching2 room layer

| Role | Evidence | PS3 1.10 | PS4 1.02 |
|---|---|---|---|
| Session manager object | glob, Init `this` | `0xf11978` | - |
| Room event callback | OPD `0xdf1fa0` passed to RegisterRoomEventCallback. Asserts "g_sessionEventQueueNum != SESSION_EVENT_QUEUE_MAX" in net-session-manager.cpp. | `0x4d0c88` | - |
| Context callback | OPD `0xdf1fa8` to RegisterContextCallback | `0x4d0d90` | - |
| Room message callback | OPD `0xdf1fc8` | `0x4d1c80` | - |
| Request callbacks (server info, world list, server ctx, default opt) | OPDs in Init | `0x4d02c8`, `0x4d0e78`, `0x4d0040`, `0x4d0130` | - |
| Matching2 term | DestroyContext/Term2/ContextStop/DeleteServerContext | `0x4d1a00` | - |
| g_matchSession object | glob: `IsHost` on it picks SERVER_LOBBY / CLIENT_LOBBY (`0x445e90..0x446068`) | `0xf20c48` | - |
| Session IsHost | call | `0x4d2394` | - |
| Second session object (party) | glob: used by roster callback `0x4bc728`, NetInit `0x4d5338(obj,1)` | `0xf2c8a0` | - |
| CreateJoinRoom / JoinRoom / SearchRoom / LeaveRoom / Kick / GrantOwner | imp | `0x4d421c` / `0x4d4578` / `0x4d3c50` / `0x4d8540` / `0x4d8a98` / `0x4d3a10` | - |
| Room data internal/external, member data | imp | `0x4d4868`, `0x4d4b28`, `0x4d5338`, `0x4d57b0`; `0x4d3af8`; net-session.cpp `0x4d2ed4` | - |
| Ping info | imp SignalingGetPingInfo | `0x4d3848` | - |

### Transport

| Role | Evidence | PS3 1.10 | PS4 1.02 |
|---|---|---|---|
| UDP send (sendto) | imp; 15 callers (NetConnectionUpdate, NetInit NAT probes, NdiNetUpdate) | `0x4db1d0` | - |
| UDP receive (recvfrom) | imp | `0x4dba48` | - |
| Socket close | imp | `0x4db600` | - |
| NetConnectionUpdate (recv loop) | str "NetConnectionUpdate", "[udpp2p] : recv from %s:%d", "Error: sendto %x" | `0x43cdb8` | - |
| Packet read (incoming connection packet) | call from `0x43cf98` → ack processing | `0x465218` | - |
| Ack loop / packet header | str assert "notify->packetSequenceNumber == m_highestAckedSequence + i + 1", "%s Ignoring Packet %i > %i" (net-connection.cpp) | `0x464cb8` | - |
| Packet write / send window | str "%s window full"; call from `0x43d0d4` | `0x4652cc` (helper `0x465138`) | - |
| Message layer (vtable methods) | str "Exceeded Packet size 1200 bytes", "%s Wait Seq (%i):", "Message type %i has expired" | `0x467600`, `0x467538`, `0x467d20` | - |
| NpNetUpdate (connection management, timeouts, kick) | str "NpNetUpdate", "ResetConnection", "has left : timed out" | `0x43bdd0` | - |
| NP connection manager (signaling) | imp ActivateConnection/GetConnectionStatus/GetConnectionInfo/Terminate | `0x4e9060`, `0x4e9e98`, `0x4ea438`, `0x4e9618`, `0x4e8afc`; object `0xf3a060` | - |
| Bit stream | str "game/net/bitstream.cpp" | `0x42f8d8`, `0x42fe78` | ~ `5e13b8`.. (assert shared by many) |

### Flow (NET_SM), matchmaking, lobby

| Role | Evidence | PS3 1.10 | PS4 1.02 |
|---|---|---|---|
| NET_SM state variable | glob: every "%s:%i : GOTO %s" site in flow.cpp stores into it (NET_SM_RESULTS = 29) | `0xf11764` | - |
| NET_SM transitions (flow.cpp) | str NET_SM_* plus GOTO | `0x43a444` (START_MATCHMAKING / CLIENT_CONNECT_TO_HOST, "Callback_Multiplayer, Update: Lobby status changed"), `0x43e99c`, `0x43ea8c` (LEAVING_GAME page), `0x43ec08` (RESULTS, "Save Game Stats" thread), `0x43edc8` (RESET), `0x43fb38` (postgame), `0x4430d8` (ROUND_RESULTS), `0x444660` (UPDATE), `0x444bb0` (CREATE_GAME_WAIT, CreateJoinRoom), `0x445608` (LEAVE_GAME, lobby choice) | - |
| TaskManagerOnline states | str; task names | LoadLevels `0x43d6b0`, Leave `0x43ee8c`, EstablishConnection `0x43ef48`, ShowRoundResults `0x43f0c0`, ShowResults `0x43f17c`, ResettingReady `0x43f238`, Resetting `0x43f2f4`, PreResetting `0x43f3b0`, Playing `0x43f46c`, WaitReady `0x43f528` | ~ `5e9fa8` / `5ef8a8` (same task strings) |
| TaskManagerOnlineUpdate | str | `0x442558` | - |
| Matchmaking state machine (net-matchmaking.cpp) | str NET_SM_*, "GATHER", "SEARCH"; PAGE_MULTIPLAYER_MATCHMAKING callback | `0x4b0a58` | `64e910` **RET** |
| Matchmaking vote list | sid LIST_MATCHMAKING_VOTE | `0x4b0670` | `64e8e0` code |
| Custom games (net-custom-games.cpp) | sid / str | PAGE_MULTIPLAYER_JOIN `0x4654d0`, JOINING `0x4661e0`, SELECT_JOIN_GAME `0x465808`, list `0x4655b0`, "Host Asked to leave the room" `0x465b70`, `0x465da0` | `609640`, `609790`, `609680`, `6095d0` code |
| PAGE_MULTIPLAYER callback | sid `0x53cbb5f2` | `0x4465c8` | `5eb1a0` code |
| PAGE_LOBBY callback | sid `0xb99a3235` | `0x443bc8` | `5ebac0` code |
| PAGE_MULTIPLAYER_CONNECTING callback | sid `0x9e1863b1` | `0x445020` | `5ed6a0` code |
| BUTTON_MULTIPLAYER_STARTMATCH | sid `0x70e80076` | `0x43aab0` | `5eb780` **RET** |
| BUTTON_MULTIPLAYER_INVITES | sid `0x0e8bb2d7` | `0x440948` | `5ebea0` **RET** |
| BUTTON_MULTIPLAYER_LEAVE_MULTIPLAYER | sid `0xafb7834c` | `0x43911c` | `5eb180` code |
| Party member buttons PARTY_PLAYER0..9 | sid | `0x4417f0` (player 0) | all `5ec490` **RET** |
| Player buttons PLAYER0..9 | sid | `0x440620` (player 0) | all `5ec460` **RET** |
| Roster/party query table (commands 16..35, records of 12 bytes) | data scan `{16,OPD,0},{17,OPD,1}` | table `0xe15d30`, callback OPD `0xdf18a8` → `0x4bc728` | table `105e730` (24-byte records; callback pointer not filled in the flat image) |
| Party error | str "NET_SM_PARTY_ERROR" | `0x4c41f8` | - |
| Friends / players met | str NetFriends::*, imp sceNpBasic*, Matching2GetEventData | `0x48f350`, `0x490040`, `0x491580`, `0x490a88` | - |
| Mode table (labels "_", matchmaking, custom game, cinema, ...) | data: ids `bd1d2c53`, `66d63005`, `cb379354`, stride 0x14 | `0xb41210`, read by `0x4b9a88` and `0x4bc310` | `def390` (refs `655d41`..) |
| Label lookup → TextLookup | call: `idx [r3+0x34]`, table `0xb41210`, db `0x109d3f0` | `0x4b9a88` → `0x9294c4` | - |

### Game (net game manager, players, scoring)

| Role | Evidence | PS3 1.10 | PS4 1.02 |
|---|---|---|---|
| g_netGameManager | glob: assert "g_netGameManager.IsHost()" loads it; 515 functions use it | `0xf1dc80` (IsHost `0x494ea8`) | - |
| NetGameManager update | str "net-task: %s"; called from TaskManagerOnlineUpdate and replay | `0x4a9410` | - |
| Client / host update | str "ClientUpdate", "HostUpdate" | `0x494930` / `0x4a8af8` | - |
| Start net game (StartNetGame role) | str "NetGameManager::m_startTime = %f", "Play Task"; called from NET_SM_UPDATE `0x444660` and cinema `0x4348f0` | `0x4a85a0` | ~ `647908` ("Play Task") |
| Match end / score limit / time limit / rounds | str "GAME OVER - time expired", "team %i has %i score, limit is %i", "next round %i, round limit %i"; called from HostUpdate | `0x4a49e8` | - |
| Round end | str "ROUND OVER - time expired / only team alive" | `0x49f114` | - |
| Timer display | str "time left %i" | `0x4421b8` | - |
| Net events start/end game, end round | str NetEventStartGame, NetEventEndGame, NetEventEndRound | `0x4759b0`, `0x47a680`, `0x47b168` | - |
| Team assignment | str "NetGameManager::AssignTeams - num teams is zero" | `0x4a1be8`, `0x4a2490` | ~ `63f7f8` / `641098` (refs `64019a`, `6415dc`) |
| NetEventAssignTeam (team, model, spawn, boosters) | str | `0x46ef58`, `0x4747b0` | - |
| Team bound check | str "team < kMaxNetTeams" | `0x495cf0` | ~ `64a8e8` |
| Net player tracker (spawn) | str "Missing spawner '%s' trying to spawn player" (net-player-tracker.cpp) | `0x4c8568` | ~ `662a28` |
| Spawn remote NetCharacter | sid `0xf2c86c22` at `0x4c9940`; "hero-default"; calls `0x4c8568` and factory `0x7069c8` | `0x4c9880` | id `0xf2c86c22` at `60f0d5`; ~ `663238` |
| Snapshot | str net-snapshot.cpp | `0x4da608`, `0x4db060` | ~ `66ad98` |
| Simple snapshot | str "index < m_snapshotCount - 1" | `0x4d93e8` | ~ `66a2d8` |
| Net event factory / serializers | str net-event.cpp asserts | `0x46d328`.., `0x480698` (FillAnimCmdBitStream) | ~ `61fdf8`, `633ca8`, `6352e8` |
| Debug "Net" menu | str "Disable Min Player Limit", "Force Rank", ... | `0x4b4244` | ~ `64eae8`..`654188` (split) |

### Online services, profile, economy

| Role | Evidence | PS3 1.10 | PS4 1.02 |
|---|---|---|---|
| Leaderboards | imp sceNpScore*: init `0x4ab854` (called from NetInit), rankings `0x4aa170`, record score `0x4ab6d0`, term `0x4ab66c`, poll `0x4a9f78`, comment `0x4aa0d4` | as listed | - |
| Trophies | imp sceNpTrophy*: TrophyManager thread `0x1735b8` (init/register/unlock), `0x173290`, unlock state `0x1731a0`, term `0x1739f0` | as listed | - |
| Trophy/stat bookkeeping (net-player-data.cpp) | str "index < trophies->m_numTrophies" | `0x4c4aa8`..`0x4c75e8` | ~ `661dc8` |
| NetPlayerData object (save stats, TUS) | glob: "Save Game Stats" thread calls `0x4c6298(0xf2da60)`; imp sceNpTus* `0x4c5f48` (set), `0x4c65f8` (get), `0x4c6e18` (init) | `0xf2da60` | - |
| Cinema / theater | str cinema.cpp, NET_SM_REPLAY | `0x4348f0`, `0x435be8`; TaskManagerReplay `0x433b98`..`0x435468`; LIST_CINEMA `0x435248`; STARTCINEMA `0x432ea4`; upload `0x433240` | ~ `5e8978`; `5e13b8`/`5e4ff8`; `5e4080`, `5e45a0`, `5e42d0` code |
| Loadout / weapon set | str "player-save-weapon-set", "weapon-set-reset" (script natives) | `0x666728` | - |
| Boosters | str "NetBoosterManager::GetAmount()", "Invalid booster id", "Net Boosters" | `0x455900`, `0x4552b8`, `0x4555d0`; script natives DcNetGetBoosterAmount in `0x4ce820` | ~ `5f6ac8` |
| Money / stats tally | str "AddMoneyToTally", "NetStatGame::Stop() Total money", moneymanager.cpp | `0x4dcf88`, `0x4dfc08`, `0xcfd94` | moneymanager ~ `5deaa8` |
| Rank / skill | str "Player '%s' skill %f ->" (rating update); "Force Rank" debug | `0x4df898` | - |
| Store | imp sceNpCommerce2ExecuteStoreBrowse | `0x4399a0` | - |
| Live data / patches | str net-livedata.cpp | `0x4aba88` | - |
| HTTP / SSL | imp cellHttp*/cellSsl* | `0x44aab0`..`0x450170` | - |

## 3. Network imports and their callers (PS3 1.10)

The callers below are the functions that contain the `bl` to each stub. Full list:
`python ps3_map.py %E% netcallers`.

**sceNpMatching2 (sceNp2)**: Init2, CreateContext, RegisterContextCallback,
ContextStartAsync, AbortContextStart, RegisterRoomEventCallback,
RegisterRoomMessageCallback, SetDefaultRequestOptParam, GetServerIdListLocal,
GetServerInfo, CreateServerContext and GetWorldInfoList are called from `4d0fc0`. The rest:

| Import | Callers |
|---|---|
| DestroyContext / Term2 / ContextStop / DeleteServerContext | `4d1a00` |
| GetEventData | `48f350 4d02c8 4d040c 4d0e78 4d1c80 4d4e0c 4d5628 4d7e48 4d8220` |
| ClearEventData | `48f350 4cff50 4d0040 4d0130 4d040c 4d1a00 4d1c80` |
| CreateJoinRoom | `4d421c` |
| JoinRoom | `4d4578` |
| SearchRoom | `4d3c50` |
| LeaveRoom | `4d8540` |
| KickoutRoomMember | `4d8a98` |
| GrantRoomOwner | `4d3a10` |
| SetRoomDataInternal | `4d4868 4d4b28 4d5338 4d57b0` |
| SetRoomDataExternal | `4d3af8` |
| SetRoomMemberDataInternal | `4d2ed4` |
| GetRoomMemberDataExternalList | `490c28` |
| SignalingGetPingInfo | `4d3848` |
| sceNp2Init / sceNp2Term | `4ec184` / `4ec0f0` |

**sys_net**:

| Import | Callers |
|---|---|
| sendto | `4db1d0` |
| recvfrom | `4dba48` |
| socket | `4db630 4db7d4 914b6c a4dec0 a4e028` |
| bind | `4db630 4db7d4 9149d8 a4dec0` |
| setsockopt | `4db630 4db7d4 8fd3b0 9149d8 914b6c 9154cc a4dec0 a4e1d8` |
| socketclose | `4db600 914a68 914b6c a4db48 a4dd48 a4ddf4` |
| send / recv | `9149a8 a4d9f0` / `914ab4 a4da9c` |
| connect / listen / accept / socketselect | `9154cc a4e028` / `9155d8 a4dec0` / `c06c8 a4e1d8` / `914608 a4e1d8` |
| gethostbyname | `447a5c 913348` |
| inet_ntoa | `43cdb8 447248 4e9618 4e9e98 4ea438` |
| sys_net_initialize_network_ex / finalize | `bfc44 a4dc38` / `bfc44 1683d4 a4db90` |

The game's UDP traffic is `4db1d0`/`4dba48`. The `914xxx` and `a4dxxx` groups are TCP
users (listen/accept/connect). They look like development or tool connections and are
not tied to the net code here.

**cellVoice**:

| Import | Callers |
|---|---|
| Init, CreatePort, ConnectIPortToOPort, Start | `4e71d8` (CreatePort/Connect also `4e77e0`) |
| End, Stop, DeletePort | `4e7038` |
| ReadFromOPort, GetPortInfo | `4e7560` |
| WriteToIPort | `4e6cd0` |
| SetMuteFlag | `4e6f04 4e6f50 4e77e0` |
| GetMuteFlag | `4e6ecc` |
| Get/SetVolume | `4e6e24` / `4e6e64 4e6e94` |
| GetPortAttr | `449e88 4bc728 4bfac8 4e6f78` |
| SetMuteFlagAll | (no caller) |

**cellNetCtl**: Init `3af410 438bc8 447a5c 4ebf7c 4f14d0 a4dc38`; GetState
`3af410 438bc8 44cc48 44d450 44d898 44ebb0 45148c 4f14d0 a4dc38`; GetNatInfo `445020 447a5c`;
GetInfo `449e88`; NetStartDialogLoadAsync `4ebf7c`.

**sceNp (signaling and basic)**:

| Import | Callers |
|---|---|
| SignalingCreateCtx, AddExtendedHandler | `4e9d28` |
| ActivateConnection | `4e9060 4e9e98 4ea438` |
| GetConnectionStatus | `4e8f1c 4e9618 4e9e98 4ea438` |
| GetConnectionInfo | `4e9338 4e9400 4ea438` |
| TerminateConnection | `4e8afc 4e8c8c 4e9618 4ea438` |
| DestroyCtx | `4e8afc` |
| GetPeerNetInfoResult | `4e9618` |
| BasicRegisterHandler / Unregister | `447a5c` / `4386d0` |
| BasicGetEvent | `449ae8` |
| ManagerGetNpId | `447a5c 4d0fc0 4e9d28 4ab854 4c5e08 ...` |
| sceNpInit / sceNpTerm | `4ec184` / `4ec0f0` |

Unused: sceNpLookup* and SignalingGetPeerNetInfo have no callers.

## 4. Remarks on this build

- The scope changed during this work: earlier third-party notes were withdrawn and are not used. This document was built from the 1.10 EBOOT and the PS4 1.02 image only. A comparison with another PS3 build was not done.
- 1.10 is a late build. It has PAGE_TWITTER and BUTTON_TWITTER_* (`0x4f1950`, `0x4f1c38`), `map-dlc%i.edat` promo checks in NetInit, live-data patch files, and an S3/HTTP stats path. Server host names and keys appear in the strings and are deliberately not reproduced here.
- The bootstrap calls cellNetCtlInit before cellNetCtlGetState and checks sceNpManagerGetContentRatingFlag.
- On PS4 1.02 most MP menu callbacks of the entry, matchmaking, invites and party flow are bare `ret`. Examples: BUTTON_MULTIPLAYER `5eb160`, STARTMATCH `5eb780`, INVITES `5ebea0`, PAGE_MULTIPLAYER_MATCHMAKING `64e910`, party buttons `5ec490`, player buttons `5ec460`.
- These PS4 callbacks keep code: PAGE_MULTIPLAYER, PAGE_LOBBY, CONNECTING, JOIN/JOINING, LEAVING_GAME, POSTGAME, cinema and the matchmaking vote list.
- The PS4 1.02 image has none of these strings: `game/net/*.cpp`, NET_SM_*, sceNpMatching2 or the connection/session texts. Net-event, snapshot, NetGameManager (pickups, coop, AssignTeams), boosters and the debug Net menu strings survive.
- Limits: function bounds come from OPD entries and bl targets. Register tracking is linear within a function, so a few sites are missed; the `?` rows in `menu-callbacks.txt` are callbacks reused from a register. PS4 function starts are heuristic. Semantics are inferred from strings, imports and call order, not from execution.

Generated outputs (git-ignored) are in `work/ps3-110/`: `imports.txt`, `netcallers.txt`,
`netstrings.txt`, `cpp.txt`, `menu-callbacks.txt`, `string-pairs.txt`, `names.txt`.
