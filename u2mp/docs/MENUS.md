# Uncharted 2 multiplayer menus: PS3 1.10 vs PS4 1.02, and what the PC port restores

Static analysis only (nothing was run). Addresses are PS4 1.02 image offsets (`boot-linked.bin`
file offset - `0x183ce8`) unless marked PS3. No game bytes are stored here. No earlier third-party
Uncharted 2 multiplayer notes or code were used. Code: `gow3_pc/src/u2mp/menus.cpp`
(`install_menus`, called after `install_remaining`), texts in `remaining.cpp` (TextDb::Find hook).

## 1. Method and the two facts that decide the work

1. **menu.bin.** Every multiplayer page of the PS4 `dc1/ps4/menu.bin` was compared, control by
   control, with the PS3 `NPUA80698` `build/main/dc1.psarc:menu.bin` (big-endian, 32-bit pointers:
   page record 0x44 bytes, `+0x20` -> `{u32 count, u32 base}`, control record 0x48 bytes with
   `+0x08` kind, `+0x0c` flags, `+0x14` label, `+0x1c` action, `+0x20` condition, `+0x38` column;
   the PS4 layout is in `u2mp/tools/u2_menu_pages.py`, whose scan skips pages with label-less
   rows, so read page records directly from their name pointers). Action numbers are one higher on
   PS4 (PS3 21 = PS4 22, "query"). **All multiplayer pages have the same controls, kinds, flags,
   columns, labels and condition numbers on both**, and the page records keep the same panel query
   (`+0x58..+0x5c` on PS4: action 22, condition 97..101/130/137, help-bar id). The only menu.bin
   difference that matters is the missing Multiplayer button in the main menus, which
   `u2mp/patcher/u2mp_overlay.py` already adds. Nothing else is patched in menu.bin.
2. **Query table.** Query controls call `fn(query)` from the table at `0x105e370` (PS3
   `0xe15b50..0xe16168`, 12-byte entries `{cond, OPD, arg}`): 132 entries, the same conditions in the
   same order on both, ended by `fn == 0`, read on every call by the helpers `0x65f4e0..0x660660`
   (`call [entry+8]`, entry at `query+0x78`, arg at `query+0x48`; only while NET_SM > 1).
   Pairing PS3 and PS4 functions by condition and comparing size and callees (u2_func_match
   pairs) shows: 5 stubs (`ret`) besides the cond-64 one that is a `blr` on PS3 too; every other
   query keeps its code. The PS3 callees they lost are small accessors PS4 inlined (IsHost
   `0x4d2394`, GetVolatileData `0x4c4f74`, live-data getters `0x4a9c38..0x4a9d34`), the text
   formatter `0x40bb24` (now `0xcdde40`), or PSN/voice calls.

So the pages looked empty because of **data**, not layout: NetPlayerData never loaded (now
`progression.cpp`), the party unknown (now `session.cpp`), live data from Naughty Dog's server
(playlists, the status count) never downloaded, NetInit's online id never written, a few texts that
only PS3 title updates carried, and the 5 stubbed queries.

Query object: `+0` op (1 title text, 2 row text, 3 image, 4 get index, 5 last index, 7 enabled,
8 reset, 9 set index; 6 and others per query), `+4` value / row, `+0x08..+0x30` image descriptor
(op 3), `+0x40` text out, `+0x48` table arg, `+0x50` element id, `+0x54` column, `+0x80` the
page's help-bar id.

## 2. Query table audit (PS4 fn / PS3 fn, by condition)

State: **stub** = `ret`; **wrap** / **port** = changed by menus.cpp; otherwise intact.

| cond | control(s) | PS4 | PS3 | state / source of its data |
|---|---|---|---|---|
| 3 | PAGE_MULTIPLAYER_JOIN: BUTTON_MULTIPLAYER_SELECT_JOIN_GAME list | `659590` | `4b8ebc` | **stub -> port** (empty list; PC joins by address) |
| 5 | (no MP control) | `659560` | `4b7d10` | intact |
| 6..15 | PLAYER0..9 on MATCHMAKING, POSTGAME | `659bc0` | `4bfac8` | stub -> session.cpp roster |
| 16..35 | PARTY_PLAYER0..9 on PAGE_MULTIPLAYER / PAGE_LOBBY | `659bb0` | `4bc728` | stub -> session.cpp roster |
| 36 | PAGE_MULTIPLAYER_FRIENDS_LIST: LIST_FRIENDS | `65bbb0` | `4c1fe0` | **stub -> port** (party list) |
| 37 | PAGE_FRIENDACTIONS list | `65bbd0` | `4b8760` | intact (page unreachable on PC) |
| 38 | PAGE_FRIENDACTIONS header (k17) | `65bbc0` | `4c1a08` | stub, left (PSN friend card; page unreachable on PC) |
| 39 | LIST_PLAYERSMET | `65bcd0` | `4b8638` | **wrap** (rows from the progression store) |
| 40, 41 | PLAYER_OPTIONS: LIST_PREFERED_GOOD_GUY / BAD_GUY (character skins) | `6595a0` | `4c0ea8` | intact; NetPlayerData + DC skin tables |
| 44, 87, 88 | PAGE_MULTIPLAYER_SCOREBOARD header, k15 widget | `65b830`, `65cc80`, `65b490` | `4bf4a8`, `4bf218`, `4be434` | intact; NGM trackers, `*net-games*`; text 64370402 added |
| 45, 46 | (in-game) | `65b1c0`, `65b320` | `4bc600`, `4bacb4` | intact |
| 48 | BUTTON_MULTIPLAYER_INVITES | `659b50` | `4b8e08` | intact (callback: remaining.cpp) |
| 49 | status line under the party list (PAGE_MULTIPLAYER, PAGE_LOBBY) | `659a90` | `4bae1c` | **wrap**: count = party size (text 69cbee70 from the import) |
| 50 | BUTTON_MULTIPLAYER_STARTMATCH | `655c40` | `4b7d80` | intact (callback: session.cpp) |
| 51..53, 131, 136 | MATCHMAKING / CUSTOM / CINEMA / LEADERBOARD / NETTROPHIES labels | `655cf0` | `4b9a88` | intact; mode table `0xdef390` (texts 47a936dc, c21027d1, f1b9728c, 067e1f80 added) |
| 54 | (pause) | `655f90` | `4c1348` | intact |
| 55 | PAGE_LOBBY: PLAYLIST | `6562d0` | `4bd9c8` | **wrap**: playlists built, choice applied, picture as type 3 |
| 57..63, 65, 66 | PAGE_LOBBY custom game carousels and team | `656550`..`657900` | `4bb898`.. | intact; game type name `+0`, description `+4` (column 3), image `+8` of `f985c7b6` |
| 64 | (none) | `657ae0` | `4b712c` | stub on both |
| 67..76 | PAGE_CINEMA, cinema list, progress | `659410`.. | `4baef8`.. | intact (cinema.cpp) |
| 77, 78 | PLAYER_OPTIONS: LIST_BOOSTERS0/1 | `659bd0` | `4be788` | intact; NetPlayerData unlocks |
| 79..86, 105, 106 | results, round results, PLAYER_OPTIONS rank widget (k19), last game results | `65bd80`.. | `4baa4c`.. | intact; NetStatGame, NetPlayerData |
| 89, 90 | PAGE_MULTIPLAYER_MATCHMAKING status, vote list | `65a900`, `65ae50` | `4bcc68`, `4bf768` | intact; their data (phase, deadline, vote block `0x113db50`) now comes from the matchmaking port (frontend3.cpp, 3.6) |
| 92..94 | (in-game) | | | intact |
| 95, 96 | EULA, MOTD | `65e210`, `65e2e0` | `4b74f8`, `4ba660` | intact; pages never shown on PS4 |
| 97..101, 130, 137 | page panel queries (title, help bar from `0x1041560`) | `65ecc0` | `4bc310` | intact |
| 102..104 | controller layout, kick griefer, voice volume | | | intact (voice volume lost its cellVoice calls) |
| 107..110 | PLAYER_OPTIONS toggles | `65c610` | `4c0780` | intact |
| 111..127 | cinema render settings | `660760`.. | `4b7184`.. | intact |
| 128 | LEAVEGAME_CONFIRM | `65e3b0` | `4c15c8` | intact |
| 129 | BUTTON_POSTGAME_PARTYUP | `65e7f0` | `4b7c48` | intact |
| 133 | PAGE_MULTIPLAYER profile panel (k23) | `65e890` | `4bddd4` | **wrap**: online id written |
| 134 | PLAYER_OPTIONS: BUTTON_CLANTAG label | `6609e0` | `4bdc30` | intact; text 639e0fa8 added |
| 135, 138 | PLAYER_OPTIONS matchmaking pref, (138) | `65a790`, `65a660` | | intact |

## 3. Page by page

### 3.1 PAGE_MULTIPLAYER (handler `5eb1a0`, PS3 `0x4465c8`)

- **PS3:** mode buttons Matchmaking / Custom Game / Cinema / Rankings / Trophies with a description
  line; the profile panel (k23, cond 133): online id, rank badge from cash, character, kills, wins,
  money, co-op points; party leader and members (cond 16..25); a status line with a server count
  (cond 49); help bar (cond 98, table `0x1041560` row 1): Profile -> PAGE_MULTIPLAYER_PLAYER_OPTIONS,
  Party Up -> FRIENDS_LIST, Store -> PAGE_MULTIPLAYER_STORE, Results -> LASTGAMERESULTS.
- **PS4 as shipped:** same controls. Rankings / Trophies labels blank (texts 47a936dc and
  f1b9728c not in the PS4 tables, descriptions c21027d1 / 067e1f80 neither), panel zeros and no
  name (NetPlayerData state never 3, netInfo+0x298 never written), party rows `ret`, status line
  blank (text 69cbee70 absent, count from the dropped live data). The help bar row is intact (PS4
  calls the store "Rewards", text 90354b88).
- **Restored:** NetPlayerData (progression.cpp, on event 0 through `64e810`), party rows
  (session.cpp), panel name = `UNCHARTED_U2MP_NAME` (written to netInfo+0x298 before the panel
  query), status line "[A] daily players" (PS3 text, from the import) with the party size (netInfo+0x10 set around the
  call, then restored), the four mode texts.

### 3.2 PAGE_LOBBY (handler `5ebac0`, PS3 `0x443bc8`)

- **PS3:** Start Match, playlist (matchmaking) or the custom game carousels (game type, map,
  variation, score / time limit, friendly fire, rounds, co-op difficulty), each with its name and
  description; party column; team; invites; status line.
- **PS4 as shipped:** carousels intact (they read `*net-games*` and the game types `f985c7b6`: name
  `+0`, description `+4` shown in column 3, image `+8`). The playlist row was empty: the playlists
  `ngm+0x20a0[]/+0x21a0` came from PS3 live data and nothing on PS4 fills them. Start Match was a
  `ret` (session.cpp).
- **Restored:** one playlist per game type in `*net-games*` (record `{id = game type image id,
  name, description, +0x28 game list NULL, +0x30 0, +0x34 skill board -1}`, the fields read by
  `6562d0`, `6400d0`, `640f00`, `64a460`, `64aac0`). The lobby's event 0 picks the first one when
  none is saved (`5ebb92`). When the leader's choice (NetPlayerData+0x104) changes, the lobby's net
  game becomes a game of that type (SetNetGame `63dc30`); Start Match then picks a random game of
  that type (session.cpp). The status line as in 3.1.

### 3.3 PAGE_MULTIPLAYER_PLAYER_OPTIONS ("Profile"; page handler `5ed8b0` = remaining.cpp)

Rank widget (k19, cond 105: rank and progress to the next rank from cash), Boosters Slot 1/2
(cond 77/78), clan tag (cond 134), preferred hero / villain character (cond 40/41: the character
skin selection), co-op difficulty, taunt, matchmaking preference, toggles (108..110). All queries
intact; their data is NetPlayerData (now loaded and saved). Restored texts: NET_CLANTAG label
(d4143b18 "Clan Tag") and the empty-tag text (639e0fa8 "None"). The clan tag button explains it is
not available on PC (remaining.cpp).

### 3.4 Store ("Rewards"): PAGE_MULTIPLAYER_STORE, PAGE_MULTIPLAYER_STORE_ITEMS

LIST_MP_STORE_SECTIONS (action 25) and LIST_STORE_ITEMS (action 26) are list classes, not query
conditions. With NET_SM > 1 the store shows the multiplayer item table `0x92c6db02` of net.bin (Hero
Skins, Villain Skins, Boosters Slot 1 / 2, Co-op Weapon Upgrades, Taunts), else the single-player Rewards
`0x855a9c58` (store.bin); columns: name (`0x5aef40`), description (`0x5af1a0`), LOCKED / UNLOCKED from
the item state `0xa4c390`, picture `0x5b0070`. Every multiplayer item is reported unlocked (IsLocked
`0xa4bb40` and the co-op upgrade level `0xa4d280` ported, unlocks.cpp). Skin and booster rows were blank
because the PS4 net.bin skin / booster tables are 2-record stubs; the GOTY tables from net-patch9 are bound
over them (unlocks.cpp). Details: `UNLOCKS.md`. The PSN store button (sceNpCommerce2) explains it is not
available (remaining.cpp).

### 3.5 PAGE_MULTIPLAYER_FRIENDS_LIST ("Party Up") and PAGE_FRIENDACTIONS

- **PS3:** LIST_FRIENDS = PSN friends (label "Friends" from menu.bin; `ba8db526` "[A] players in party" is a friend's detail line), rows name + presence; selecting
  one opened PAGE_FRIENDACTIONS (friend card cond 38 + actions). LIST_PLAYERSMET = NP "players met".
- **PS4 as shipped:** LIST_FRIENDS query and callback `ret`; players met intact but its source
  (`0x113b668`, PS3 fetch `0x48f79c`) never filled.
- **Online (PORT-ONLINE.md 5a):** the RPCN friends with Join Party / Invite To Party.
- **Restored (LAN):** LIST_FRIENDS lists the party (leader marked "Party Leader"), title "N players in
  party"; selecting a row opens the player popup (remaining.cpp friends_row, same row order).
  LIST_PLAYERSMET lists the players of recent matches from the local progression store, most recent
  first. PAGE_FRIENDACTIONS (cond 37/38) stays as is: nothing on PC opens it.

### 3.6 PAGE_MULTIPLAYER_MATCHMAKING, JOIN / JOINING, LOBBY start

- **PS3:** Find Match -> "Searching for players..." (status cond 89, tips line), the players found
  (PLAYER0..9, cond 6..15), then the vote screen: two games (map name, map picture, game type, vote
  counts; LIST_MATCHMAKING_VOTE cond 90), "Voting ends in [A]...", the winner, "Game starting in
  [A]..." and the match. After it, back to this page and the next vote.
- **PS4 as shipped:** the widgets and their queries are intact; the page handler (the matchmaking state
  machine `64e910`, PS3 `0x4b0a58`) is a `ret`, the vote was never broadcast, nothing filled the vote
  block. session.cpp started matchmaking games straight from the lobby.
- **Restored (frontend3.cpp, PORT-FRONTEND3.md):** Start Match in matchmaking mode runs the PS3 flow on
  the party: gather (3..10 s, until every member's page is up), vote between two games of the chosen
  playlist (15 s), result, 10 s countdown, then the custom-game create path takes the party into the
  match. After the match Leave::Enter returns everyone to this page for the next vote. Back asks "Leave
  Matchmaking?" and returns the party to the lobby.

The join list (cond 3) is answered as an empty list; PC joins by address. PAGE_MULTIPLAYER_LEAVING_GAME
now closes (session.cpp sets match+0xc8 while it is up; PORT-FRONTEND3.md section 6).

Party and player rows (cond 16..35, 6..15): name and, as PS3 `0x4bc728` / `0x4bfac8` op 3, the
member's rank badge (type 0x27), the rank exchanged in the session messages.

### 3.7 Post-game: POSTGAME, RESULTS, ROUND_RESULTS, SCOREBOARD, LASTGAMERESULTS

Player lists are action-23 lists with a column set (`c441623e`, `dcae45f4`, `2eb65ce8`,
`519c67c`: icon, money-rank, player, co-op points, kills, deaths, ... in menu.bin), filled from the
NetGameManager trackers and NetStatGame, which the session layer fills. Header and widget queries
(81..88, 106) are intact. Restored: the scoreboard countdown text 64370402 ("Starting in [A]"),
absent from both text tables. POSTGAME's PLAYER0..9 rows: session.cpp.

### 3.8 Leaderboards (PAGE_MENU_LEADERBOARD) and Trophies (PAGE_NET_TROPHIES)

Leaderboard rows: progression.cpp (local boards). Net trophies (LIST_STATS) read NetPlayerData.
Restored: the page title MENU_TITLE_TROPHIES (85ed01cc "TROPHIES") and the button texts (3.1).

### 3.9 Cinema pages

Unchanged here; see `CINEMA.md`.

## 4. Texts (TextDb::Find hook, texts.cpp; import u2text.py; details in TEXTS.md)

The multiplayer code and menu.bin use 26 text ids that no PS4 text bank has (the PS3 game got them from
title updates and its server; they are in the BCUS dlc-patch7 networking bank, which is not a source):

| ids | used by | text (ours, English) |
|---|---|---|
| f67d9ad2, 70c81d2e, c0cd1b00 | profile panel k23 (`59ddb0`) stat labels | Competitive Wins / Competitive Kills / Cash |
| 47a936dc, c21027d1 | mode 4 label / mode 4 and 5 description (`0xdef390`) | Leaderboards (GOTY base bank: "Rankings") / View your stats |
| f1b9728c, 85ed01cc | mode 5 label, MENU_TITLE_TROPHIES | Trophies / TROPHIES |
| 780b62e0, e663a74e, b9602dbb, 966fa9ae, 4915c4b7, 7d24a9f9 | stats ticker k24 (`5b3240`) | Time Played, Global, Wins, Losses, Ratio, Rank |
| 320a6fc8, 8994a031, 5ba2fdfb, d82785e7 | help bar table `0x1041560` (leaderboard footer) | Go to Top, Next, Previous, Filter |
| 69cbee70 | status line, cond 49 | [A] players today |
| d4143b18, 639e0fa8 | NET_CLANTAG label, clan tag query `6609e0` | Clan Tag / Enter Clan Tag... |
| 64370402, 9b0dd3c2, e012d5e3 | scoreboard countdown `65cc80`, leave-game confirm `65e750` | Quit Penalty [A]... / If you leave now, you will get a [TEXT] second wait penalty ... |
| 9811415d, b7e89ee7, 6cff3670, c4c378d1, bde6ab5b | DLC notes and messages | DLC / Requires the ... Pack / You didn't finish your last game ... |

(`067e1f80`, which earlier versions served as a text, is the Trophies **icon** id of the mode table.)

47a936dc comes from the GOTY base bank (the import merges every GOTY archive). The other 25 are our own texts in
all 22 languages (texts.cpp `kOwn`), with the other ids no bank has: 041a85ca (DLC pack bought), 63801125
(Guardians), the three DLC map titles, the Siege description, net-patch9's game type variants, playlists and 48
skin names, the five server playlist texts of `kPlaylistDefs` (were `extra_text` in menus.cpp) and the PC message
boxes. The import writes `text1/ps4/<lang>.networking` and `.common` (u2text.complete: PS4 bank + GOTY of the same
language + sam-spanish <- spanish, brazilian <- portuguese for their `_` placeholders; English always). The game
reads its banks from `core.psarc` (loader `0xcdd960`), so it does not open those files: the TextDb::Find hook
answers a miss or a `_` from them, then from our texts, then English, and logs every id it can't answer once
per language ("texts: no text for ...", "texts: ... shown in English").

## 5. Gaps

- "[A] daily players" (69cbee70) was a count from Naughty Dog's server; the party size is shown with it.
- PAGE_FRIENDACTIONS, the MOTD/EULA pages and the PSN friend card (cond 38) are not restored (no PC
  counterpart). The matchmaking machine `64e910` is restored on the party (frontend3.cpp); the PS3
  5 s "Starting in [A]..." popup before it is ported too (frontend6.cpp, PORT-FRONTEND6.md 1).
- `dd2d35e7` (a net event's text, `62f250`) is absent from both tables; not a menu text, left.
- Not verified in the running game (static analysis only): the k23 panel and k19 rank widget
  drawing (`59ddb0`, `59bf50`) and the action-23 list classes are intact by size and callees, and
  get their data from NetPlayerData / NetStatGame / the trackers.
- Playlist descriptions are the game types' descriptions, not the PS3 server playlists (Deathmatch,
  Objective, Co-op Arena, ...): those were server data.

## 6. Page by page against the PS3 game (2026-11-08)

Reference: RPCS3 screenshots of PS3 1.10. "Before": the PC build tested on 7 November. Not run here
(static analysis); every "now" needs a check in game.

Three causes cover most of the gaps:

1. **Texts** (section 4): every "UNTRANSLATED STRING!!!" and blank label.
2. **The net image table** `0x1130008+0x70`. Every type 3 image of the MP menus (mode buttons
   `0x655f11`, game type `0x656864`, map card `0x656ac8`, boosters `0x65a0e0`) is looked up there
   (`0x5558c0`; selector 1 = net, 0 = hud `+0x68`, 2 = menu `+0x78`). `0x554090` (every frame, from
   Draw `0x3fc37a`) sets it from the package `"net"` (sid `4240ef2e`) only while that package is logged in
   (`+0x30 == 7`), else NULL. Without an entry the image gets size 0 (`0x5bcc1a` -> `0x5bbd90`), and the
   rich rows (kind 8) of PAGE_LOBBY take their height from it: Game Type / Map / Variation collapse onto
   one line. The game's own request (`0x5ed839`, PAGE_MULTIPLAYER_CONNECTING) sets package flag `0x40`
   right after RequestPackage (as the hud request `0x4b8c7f` does); session.cpp requested without it.
   Now: the flag is set, the package is requested again while the MP menus are up (states 18 / 3 / 12),
   and the log says `net package: present, state N, flags 0x..; net image table set|NULL, N images`
   whenever that changes. If the table stays NULL with state 7, the cause is elsewhere (next round).
3. **Image types PS4 cannot draw.** The menu image renderer (`0x5bd852`, types 4..0x29) has no case for
   `0x1e` (booster pair of the profile panel), `0x22..0x25` (0x25 = playlist picture, 0x22 = cinema list)
   and `0x27` (rank badge: profile panel, k19 widget, PS3 party rows): they go to its default
   (`0x5c1ab1`, draw nothing). The queries are intact; the drawing code went with the PS4 multiplayer.
   The PS4 net.pak has the materials (`general/net/rank-badges:default`, `general/net/boosters:*`,
   `map-photos:*`, `playlists:*`). Now ported (section 7); 0x23 is the MOTD picture, not needed.

| page (reference) | PS3 | before | now |
|---|---|---|---|
| PAGE_MULTIPLAYER (6) | mode icons, labels Leaderboards / Trophies (14%), panel: name, Competitive Wins / Kills / Money / Co-op, rank badge 28, booster icons; party leader with badge; "0 daily players"; ticker "Time Played: name: 10h 5m"; footer Profile / Party Up / R1 Store | no icons, stat labels UNTRANSLATED, no badge, "1 players in party", empty ticker | labels, Leaderboards, Trophies (n%), ticker and status texts from the PS3 bank; "N daily players" = party size; mode icons for Matchmaking / Custom Game / Cinema once the net table is set (the PS4 net.pak has no Leaderboards / Trophies button images: `mode-buttons:` cinema, customgame, matchmaking only). Rank badge and booster pair: cause 3 |
| PAGE_LOBBY, matchmaking (7) | Find Match, playlist "Deathmatch" with picture card | playlist "Machinima", empty picture box | Machinima (the cinema game type, text 5614c769) is no playlist; Deathmatch first; picture: type 0x25 replaced by the type 3 net image `general/net/playlists/<deathmatch, objective, coop, survival, default>` chosen by game type (the PS4 net.pak has these five) |
| PAGE_LOBBY, custom game (8, 14-18) | Game Type with mode icon, Map with card + name, Variation, Score / Time Limit, Friendly Fire; party leader with badge; Team | rows on one line ("Variation/Game Type"), party leader and members empty | party rows: cond 26..35 have table args 10..19 and the roster query read them as rows 10..19 (empty); now row = arg % 10 as PS3 `0x4bc780` (and the PARTY_PLAYERn callback likewise). Rows / icons / map cards: cause 2. DLC map cards are not in the PS4 net.pak (IMPORT.md). Leader badge: cause 3 |
| PAGE_MULTIPLAYER_PLAYER_OPTIONS, Profile (9-11) | rank ring and money, Boosters Slot 1/2 with names and icons, Clan Tag, Hero / Villain portraits, Co-op Difficulty, Taunt, Matchmaking, Golden Guns; booster carousel with descriptions | empty, footer UNTRANSLATED | Clan Tag label and "Enter Clan Tag..."; footer texts; booster and portrait images are type 3 (cause 2). Rank badge in the k19 widget (type 0x27): cause 3. Why the whole page was empty is not explained by the code (all its queries are intact): check with the new build |
| PAGE_MENU_LEADERBOARD (12) | Rank, Level badge, Players, Kills, Deaths, Ratio; footer Filter / Prev / Next / Top of List | footer UNTRANSLATED | footer texts; rows from progression.cpp; level badge = type 0x27 (cause 3) |
| PAGE_NET_TROPHIES (13) | list with icons, progress bars, counts, description | title / label only | texts; icons are net images (cause 2) |
| PAGE_CINEMA (19) | Local Files, map picture, Author / Players / Length / Size / Date | not reported | texts; the list picture uses type 0x22 (cause 3) |

Check in game (log `u2mp`): `texts: N strings from 2 imported bank(s) for language 2` (N about 2,700),
the `net package:` lines (state 7 and "set, N images" on the Multiplayer page), mode icons, custom game
rows on separate lines with icon and map card, party leader on the custom game page, the playlist card.

## 7. Why no net image drew, and the lost image types (menuimages.cpp, 2026-11-07)

Run 13 had the net table set (251 images) and still no menu image. Cause: **id naming**. The net
table is the `net` package's material table (package+0x940; entry 0x28 bytes, `+8` id, name via
`+0 -> +0x10 -> [0] -> [0]`). ConstructFromBoundMaterialTable (`0xbe81f0`) sets each id to the string
id (`[0x10666c8]` = `0xb26fc0`, the CRC sid) of the material name up to `,`. PS4 names them
`general/net/<set>:<name>`; the menu data and code ask for the PS3 ids `sid("general/net/<set>/<name>")`
(mode table `0xdef390+8`, map records `+0xc`, booster records `+8`, the playlists). No id matched, the
lookup returned NULL, the image size stayed 0 (`0x5bcc1a`), nothing drew and rich rows collapsed.
Fix: `0x5558c0` is ported whole; an id not in the table is looked up again among the names with `:`
read as `/` (cached per table). The queries that search inline (`0x655ec0`, `0x656a22`) get -1 and
then go through that lookup. Other checks of the type 3 path, all fine: selector 1 fades in over 300
frames from the package's login frame (`+0x924`, `0x5bca18`); size = the largest texture of
technique 0 (`0x5bbd90`).

Renderer `0x5bcd70`: jump table `0x5c2ec4` (types 4..0x29) entries for 0x1e, 0x22..0x25, 0x27 now
point at a stub (r11 descriptor, `[rbp-0x1fe0]` draw list, `[rbp-0x1ff0]` x, y, w, h, colour bytes in
ebx/r14d/r12d/r13d) that calls C++ and jumps to the exit `0x5c1ab1`. Quads are drawn with
`0xbdce90(ctx {list, 0...}, {material, 0, 0, 0}, 4 vertices of 0x40: x y z u v, colour at +0x20)`
as the type 3 case does. Descriptor value = `+0x10` (query `+0x18`). From PS3 `0x41a790`:

| type | PS3 case | drawn |
|---|---|---|
| 0x27 rank badge | `0x41aa48` | `general/net/rank-badges/default` (index > 63: `.../default2`, absent in both games), cell `index & 7`, `(index & 63) / 8` of 1/8 |
| 0x1e booster pair | `0x41c2d8` | low / high 16 bits = boosters (table `916e03b0`, rec `+8` image, ids 05454b23 / 01845694 none), each w x w/2, centred at 28 % / 72 % height |
| 0x22 map picture | `0x41eb50` | map table `0dec97a4` (0x38 bytes, `+0` id, `+0xc` image), u 0.0898..0.9102 |
| 0x24, 0x25 playlist | `0x41f418` | ngm playlist with that id, image `+0x10` (menus.cpp sets it), u 0.0703..0.9297 |
| 0x23 | `0x41cd34` | not needed: its only PS3 producer is the MOTD query (cond 96, `0x4ba660`, at `0x4ba760`), whose server page PS4 / PC never shows; the PS4 MOTD query `65e2e0` no longer produces it, and no other PS4 query sets 0x23 |

The 0x25 bottom-edge adjustment is ported (50 units, PS3 `0x41f4f4`). The PS3 overlay drawn when
descriptor `+0x20` is non-zero is not needed: no PS4 producer sets it (PORT-FRONTEND6.md 4).

Profile page empty: all its controls are k0 headers, k19 (cond 105, rank badge 0x27), k8 rich rows
(boosters, hero / villain portraits: type 3) and k13 settings; the queries are intact and the page
handler matches PS3 `0x4443f8` (clan tag keyboard, pop when not connected). Not explained statically:
if the page stays empty with the id fix, the log tells whether images are looked up / drawn on it.

Log (`u2mp: menu images:`): the first 6 entries of each net / menu table (id, PS3 id, name), the first
48 id lookups that miss the table (`found by PS3 name` or `missing`), the first 24 draws of the ported
types (type, value, image id, found, box). Check in game: mode icons, game type / map cards and
separate rows on the custom game page, playlist card, rank badge 28 and the booster pair on the
Multiplayer panel, Profile page contents, store skin pictures.

## 8. Menu images PS4 dropped: net-u2mp.pak (u2netimg.py, 2026-11-07)

Run 15 still missed two ids on the Multiplayer page: `e2c34a4a` = `general/net/mode-buttons/leaderboards`
and `067e1f80` = `.../trophies`. The PS4 net.pak has no such materials. Compared material by material
(PS3 id = sid of the `/` name, PS4 id = sid of the name with `:` read as `/`), the newest PS3 net.pak
(dlc-patch7) has 104 materials the PS4 one lacks: those two buttons, the DLC map photos (highrise,
museum, sts, sge, wtr), rank-badges/default2 (ranks above 63), the-lab playlist, medal icons, and the
player photos / icons of the PS3-only skins (store rows without a picture).

The import converts only those (the rule: what PS4 has stays the PS4 one) into
`pak22/ps4/net-u2mp.pak` with the Uncharted 3 net converter (`paktools/convert_net.py`), with three U2
differences in `u2conv/u2netimg.py`: the U2 PS3 MATERIAL_TABLE_1 (two tables, texture hashes in a shared
array on another page, no name array), a few textures that share one path under several hashes get a
unique name, and each material takes the effect of a PS4 net material of its set (the PS4 net.pak is
the effect reference). The registry gets a `net-u2mp` level / package entry.

In game: `keep_net_package` requests `net-u2mp` next to `net` when the file exists; the image lookup
(0x5558c0 port) answers a net id that neither the net table nor its PS3-name pass finds from the
`net-u2mp` package's material table (`+0x940`, once the package state is 7). Log:
`menu images: net-u2mp.pak imported`, `net-u2mp table ..., N materials`, `-> found in net-u2mp`.

## 9. Loading screen, playlist pictures, image sizes (2026-11-09)

Details: PORT-FRONTEND4.md.

- **Loading screen** (PS3 `0x4ae638`, absent on PS4): the fixed-layout version was rejected in run 43;
  loading.cpp now follows PS3 element by element (fill, edges, rosters page, band, map name, game
  description; not yet the fort.dds panels, coins and tip). On by default. PORT-FRONTEND5.md.
- **Playlist pictures**: no two built playlists share a picture; PS4 net.pak pictures are used where PS4
  has them, net-u2mp.pak only for elimination, grab-bag, plunder, the-lab. The image lookup now prefers
  the PS4 net table over net-u2mp.pak for every net id.
- **Image sizes**: PS4 net.pak textures are twice the PS3 resolution and a type 3 image takes its size
  from its texture, so on the multiplayer pages (PS3 layout) every PS4 net image was twice the PS3
  size. Those images are now measured at half their texture size.

## Audit of the PS3 net-menu helpers (2026-11-09)

The 75 PS3 net-menu functions the matcher left unpaired (0x4b7124..0x4c2bac) all have live PS4 slots in
the query table (PS3 0xe15b50 = PS4 0x105e370, paired by cond), except the known `ret` conds (3, 6..35, 36,
38, 64: ported or not needed). Two player-visible gaps were found and fixed:
- cond 100 (cinema list panel, 0x65ecc0) shows the L1 "Messages" entry (action 6b2970e2, cinema receive)
  only while netInfo+0x18 (PS3 NP message count) is set: online.cpp now counts received cinema files
  there and remaining.cpp's cinema_receive clears it.
- cond 104 (voice volume slider, 0x65f3f0 = PS3 0x4b77ac): menus.cpp answers op 4 (volume x 20), op 5
  (max 20, original) and op 9 (volume = value x 0.05) with voice.cpp's playback volume.
