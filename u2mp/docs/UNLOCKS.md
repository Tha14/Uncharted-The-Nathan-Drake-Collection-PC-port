# Uncharted 2 multiplayer unlockables on PC: everything unlocked, every store row named and pictured

Static analysis only (nothing was run). Addresses are PS4 1.02 image offsets (`boot-linked.bin` file
offset - `0x183ce8`) unless marked PS3 (GOTY NPUA80698 1.10). No game bytes or texts are stored here; no
earlier third-party Uncharted 2 multiplayer notes or code were used. Code: `gow3_pc/src/u2mp/unlocks.cpp`
(`install_unlocks`, `unlocks_refresh`, `unlocks_text`), called from `progression.cpp` (0x64e810 hook) and
`remaining.cpp` (TextDb miss path).

## 1. The items

dc symbol `0x92c6db02` in `dc1/ps4/net.bin` (PS3: the same symbol in net-patch9, same 6 sections and the
same items except one hero, see 5). `{s32 sections; +8 section*}`, section 0x18 bytes `{+0 id, +4 title
text, +8 s32 count, +0x10 item*}`, item 0x50 bytes:

| field | meaning |
|---|---|
| `+0` | bit index (NetPlayerData `+0x4c + 4 * (0x25 + bit / 32)`, the "owned" bit) |
| `+4` | type: 10 skin, 11 booster, 12 co-op weapon upgrade, 13 taunt |
| `+8` | id (skin id = skin table `+0x10`, booster id = booster `+0xc`) |
| `+0xc`, `+0x10` | taunt name / description texts |
| `+0x14` | cash value shown with the rank condition |
| `+0x18` | condition list `{s32 count; +8 cond* (0x18: +0 type, +4/+8 values, +0x10 string); +0x10 any}` |
| `+0x2c`, `+0x34` | co-op upgrade: weapon id, tier |

| section (title text) | items | conditions |
|---|---|---|
| Hero Skins | 29 | rank (type 7) for 9 of them, the rest none |
| Villain Skins | 29 | rank for 11 |
| Boosters Slot 1 | 15 | rank 2..57 |
| Boosters Slot 2 | 15 | rank 4..58 (one none) |
| Co-op Weapon Upgrades | 32 | tier 1: rank; tiers 2, 3: the previous tier (type 1) |
| Taunts | 7 | rank 1..53 |

## 2. Every check, PS3 -> PS4, and what PC does

| check | PS3 | PS4 | PC |
|---|---|---|---|
| IsLocked(item): the item's condition list (rank from cash, previous tier, trophies, entitlement ...) via ConditionMet `0xa4bcb0` | store module `0x163000..0x164400` (`0x1642c0`, called by `0x4b88e0`) | `0xa4bb40` (`0xa4bb30` = !IsLocked) | **ported**; multiplayer items (records of the `0x92c6db02` arrays) are never locked. Other items (single-player Rewards `0x855a9c58`) as before, debug switch `0x1114178` honoured |
| hero / villain lists (PLAYER_OPTIONS cond 40/41) | `0x4c0ea8` -> `0x4b88e0` | `0x6595a0`, reset op at `0x6597b7` | via IsLocked: every skin with a skin record |
| booster lists (cond 77/78) | `0x4be788` | `0x659bd0` (`0x659fd4`) | via IsLocked |
| taunt list | | `0x65a4eb` (section `b5bebbce`, `0x65a573`) | via IsLocked |
| random skin | | `0x64b4c0` (`0x64b51b`) | via IsLocked |
| store rows: LOCKED / UNLOCKED, `? ? ?` name of hidden locked items | `0x40b60c` area | item state `0xa4c390` (`+0` = !IsLocked), `0x5aef40`, `0x5b0260` | via IsLocked |
| owned (purchase bit) | | read only by the co-op upgrade level `0xa4d280` (`0xa4d382`); written by the purchase `0xa4d900` (`0xa4db63`); helpers `0x661480` / `0x661540` have no callers | `0xa4d280` **ported**: every tier counts as owned (cache `+0x20` of the `829f99c6` weapon record = highest tier) |
| debug "unlock all" | | byte `0x1114178` (menu callback `0x3fed90`) | not set: it would unlock the single-player Rewards too |

No other code gates an item: the PS4 store has no price column (`0x5b0260`: name, description, status), the
rank and cash stay the player's own (progression.cpp), medals and net trophies are awards, not unlocks.
Off switch: `UNCHARTED_U2MP_UNLOCKS=off` (tables of section 3 still bound).

## 3. Why store rows had no name or picture

The PS4 `net.bin` defines two of the tables the store reads as stubs; GOTY took them from the server module
net-patch9 (imported as `dc1/ps4/net-patch9.bin`, loaded by `netpatch.cpp`, which binds only symbols the PS4
game lacks, so the stubs stayed):

| symbol | PS4 net.bin | net-patch9 (GOTY) | read by |
|---|---|---|---|
| `5a4731d2` skins, PS4 record 0x30: `+0` index, `+4` side, `+8` package name*, `+0x10` id (8 bytes), `+0x18` name, `+0x1c` photo, `+0x20` icon, `+0x24`, `+0x28` | 2 (Drake, Eddy Raja) | 107 | store name `0x5aefe8` (`+0x18`), store picture `0x5b00bf` (`+0x1c`), skin lists `0x6595a0`, spawn `0x6632e5` (`+8`), party icons `0x65b749` (`+0x20`) |
| `916e03b0` boosters, 0x20: `+0` name, `+4` description, `+8` picture, `+0xc` id, `+0x10` slot, `+0x14` amount, `+0x18`, `+0x1c` | 2 ("None" x2) | 32 | booster list `0x113a788` (netinit.cpp; store name `0x5af04e`, picture `0x5b0117`, amounts `0x5f6410`), player booster index -> id `0x5e99a9`, `0x662df5` |

Other PS4 stubs next to them are bound too (section 3.1).

`unlocks_refresh()` (first call while NET_SM > 1: 0x64e810 or the first IsLocked) loads its own copy of
net-patch9, relocates it with `0xa19a00` and binds with `0xa16d70` (byte-checked as in netpatch.cpp):

- boosters: net-patch9's table as converted (4-byte fields only; checked: 32 records, slot 0/1, both "None"
  ids, identical to the PS3 records), then the list `0x113a788` is filled again (records with `+0x18 != 0`,
  at most 63, as netinit.cpp).
- skins: the import's converter writes 0x28-byte records (id as 4 bytes); the PS4 record has an 8-byte id.
  The layout is detected (every record's `+8` is an `mp-...` string inside the module; PS4 layout also
  `+0x14 == 0`) and re-laid out into a new table. The PS4 Drake and Eddy records replace the first GOTY
  record of their id (GOTY's first Drake record is the server's holiday model `mp-hero-frosty`).
- Re-checked on every 0x64e810 call; rebound if something replaced them. A missing / bad file: logged, PS4
  tables kept, no crash.

### 3.1 The other net.bin stubs (2026-11-09)

Five more symbols are stubs in the PS4 `net.bin` and full in net-patch9. Every reader is intact PS4 code;
`unlocks_refresh()` builds a table of its own for each from its net-patch9 copy (the converted layout is
checked record by record against the PS4 readers) and binds it with the skins and boosters. Off switch:
`UNCHARTED_U2MP_GOTY_TABLES=off`. Log: `unlocks: net-npc-info: ...`, `character attributes: ...`,
`speakers: ...`, `material parameter set: ...`, `default game settings: player health 90 (PS4 80)`,
`... 0x<sid> bound`.

| symbol | what (PS4 readers) | PS4 stub / GOTY | effect of the stub | PC |
|---|---|---|---|---|
| `a977dad3` net-npc-info | 12-byte `{archetype sid, class sid, co-op points}`; GetCoopPointsForKill `0x5decf0` (moneymanager.cpp), `0x645d5e`, `0x645e9b`, `0x64ab78` | 1 / 56 (= GOTY base net.bin) | an archetype not in the list hits the assert `int 0x41` (`0x5dedac` / `0x5dedd8`): every co-op kill of another NPC type | GOTY records as converted (same layout), PS4 records appended if GOTY lacks their archetype |
| `42d1ec02` character attributes | 0x20 `{actor name*, +8 key = sid(actor name), +0x10, +0x14 rig variant 2 / 3, +0x18 class 1..5 (tables 0xdef540 / 0xdef560), +0x1c base sid of "<x>-phys-fx"}`, searched after `74bfa7d0`: `0x5fdbc0` (phys-fx, else default `0x474e00`), `0x725480`, `0x725820`, `0x94f61f`, `0x94fbc0`, `0x9579e1`, `0x964fc1`, `0x965b05`, `0x97863f`, `0x9786ff` | 2 (mp-hero, mp-hero-eddie) / 98 | every other skin gets default rig variant, class and no phys-fx | re-laid out (the converter packed `{ptr, id, a, b, c, sid, pad}`); the two PS4 records replace GOTY's of the same key |
| `bdf0cfd6` speakers | sorted 0x10 `{key, pad, value*}`, value `{name*, line prefix*, *, *}` ("Multiplayer Hero (sullivan...)", "mlb-"); binary search after `1b405df2` at `0x474c08`, `0x725564` (multiplayer only) | 1 / 52 | other skins get no speaker entry | GOTY records (checked sorted, values inside our copy; PS3's fifth u32 stays after the four pointers, unread) |
| `ef68c757` material parameter set | 0x30 `{material path*, parameter*, type 1, from*, to*, cache*}`, applied by `0x9f3b70` (from `0x64b926`, value by the map record `+0x34`) through `0xbe78d0` (follows +0x28 only when set) | 1 (Drake's outfit cloth) / 98 | only Drake's outfit gets `g_fTileMaskStrength` | GOTY records (converted layout = PS4 layout) |
| `1ad223b4` default game settings | type `2209c8ff` used when a `*net-games*` record names none (`+0x64`): `+0` player health (`0x5fd853`, scaled by the health booster `9a925faf`), floats `+4..+0x30` | 80 (= GOTY base) / 90 | 80 health instead of GOTY's 90 | PS4 object with net-patch9's `+0..+0x34`. The per-game settings `63e35446`, `ac1dedb5` are equal in both |

Every picture id of the store skins and boosters exists in the PS4 `net.pak` or in the imported
`net-u2mp.pak` (u2netimg.py: player photos / icons of Platzor, Glowzor, Rika, Pinkerton, Lazarevic, Cole,
Sev, Helghast, Hale, Chimera, Zeke, the Uncharted 1 mercenaries, Heist / Borneo Chloe, the Sullivans,
Wetsuit Elena); the image lookup (menuimages.cpp) resolves them. The co-op upgrades draw the weapon icon
(type 0x1f), taunts have no picture (as PS3).

## 4. Texts

Taunt, booster, weapon upgrade and section texts are in the PS4 banks. Skin names: the store lists 58 skin
ids; 57 have a skin record after the merge and 42 of those have their name in the PS4 networking bank. 15 have a name id that no GOTY bank has (base
`text1.psarc` common / networking, `all-patches.psarc` common): their names came with the PS3 DLC skin packs.
For those `unlocks_text()` answers on the TextDb miss path only (after the PS4 bank and the imported banks)
with a label made from the actor package name (`mp-hero-killzone-sev` -> "Killzone Sev"):

| package | name id |
|---|---|
| mp-hero-rika | 91436a43 |
| mp-hero-pinkerton | 11f7b7ec |
| mp-hero-lazarevic-fat | 80f54e0f |
| mp-hero-killzone-sev | 6d76782e |
| mp-hero-killzone-infantry | 8acd02bf |
| mp-hero-resistance-hale | 4f19c9be |
| mp-hero-resistance-hybrid | 868de84b |
| mp-hero-infamous-zeke | fd14e3ae |
| mp-hero-u1-merc-brsr-2a | be99f3dd |
| mp-hero-u1-merc-grdr-1a | b3dad504 |
| mp-hero-u1-shotgun-1a | b71bc8b3 |
| mp-hero-u1-merc-brsr-1a | a95c98b6 |
| mp-hero-chloe-heist | 006d7d58 |
| mp-hero-sullivan-blue | b461f41e |
| mp-hero-year-1 | 25416211 |

Skin descriptions: the PS4 store shows none for skins (the description column of `0x5af1a0` reads the item
`+0x10`, 0 for skins), as PS3.

## 5. Still missing

- Hero item index 5 (`76474d48` = sid("mp-hero-red")): no record in the PS4 stub or net-patch9 (GOTY's patch put
  `mp-hero-nd` at index 1, where the GOTY base `net.bin` of `dc1.psarc` has it). **Added** from the GOTY base
  record: `{index 1, hero, "mp-hero-red", 76474d48, name 9a88ecdc "Baseball Shirt Drake", photo dafcd51b, icon
  c283da48}`. All of it is PS4's: `mp-hero-red.pak`, `player-photos:hero-red` / `player-icons:hero-red` in the PS4
  net.pak, the name in the PS4 networking bank. It is appended at the end of the merged table, so every other
  index is unchanged. Its character attributes record is GOTY's (3.1).
- Skin index vs id between PCs: the player data message sends a skin as its position in `5a4731d2` (byte,
  `0x6145a7` / `0x614632`), and the receiver reads the same position (`0x641aa0`, bounds-checked). The merged table's
  order is fixed (net-patch9's order, PS4 records in place, Baseball Shirt Drake appended), so it is the same on
  every PC with the same import. The log line `unlocks: skin table order fingerprint XXXXXXXX (N records ...)` must
  be equal on the PCs of a match. A PC without the import (2 + 1 records) maps the indices differently: the
  remote skin is then out of range (no model) or another record. Sending ids instead would change the game's own
  message (0x6144b0); not done. Instead a client's join info (session kInfo) carries its fingerprint and the
  host compares it: on a mismatch it logs `party: <name>'s skin table differs` and shows a notice to run the
  importer again.
- The 15 names above are labels, not the PS3 DLC texts (not in any GOTY file).
- Not verified in the game. Log (`u2mp: unlocks:`): `skin table: 107 GOTY records (0x28-byte converted
  layout) + PS4 2 (2 replaced, 0 appended) = 107`, `booster table: 32 GOTY records`, `... bound`,
  `booster list 0x113a788: 32 boosters`, `multiplayer items reported unlocked`.

## 6. Test run 41: no player model (open; diagnostics added)

How the PS4 build picks and loads the player's model (all present, no stub):

1. `0x6145a7` builds the member data: hero / villain skin = the index in `5a4731d2` of NetPlayerData
   `+0x10c` / `+0x110` (byte; not found -> random skin `0x64b430`).
2. AssignTeams (`0x640a08`, `0x640e14`) copies tracker `+0x145` (hero) / `+0x146` (villain) to `+0x138`.
3. LoadLevels' request list (`0x641aa0..0x641b1e`): for every tracker, record `+8` (package name) is
   requested (`0x56d170`, flag = record `+0x24 != 0`).
4. The spawn (`0x663280`, `0x6636b0`) resolves the name with `0x56a980` (sid looked up in the table
   `0x14705f0`, then the package) into tracker `+0x290` (record index invalid, or byte
   `0x1059640+0x4975` set: "proto-mesh").

Run 41: the local player was the default Drake (`b095d270`): the merged table maps it to the PS4 Drake
record (index 63, package `mp-hero`) and LoadLevels requested and loaded `mp-hero`. So the rebinding did not
change what the selection resolves to: before it the PS4 stub (index 0) named the same package. But PS4
ships no `mp-hero.pak`: the file is the import's (u2twin from `costume-hero-nd`, 9/14 LOD0 submeshes and
9/15 materials from PS4, the rest converted). Every run so far used that one skin. No PS4 asset is the same
model (PS3 texture names: `costume-hero-nd` 80/85, `hero-default` 76/85; the mesh check matched 37 %), so
under the PS4-asset rule it stays the import's build. The 15 PS4 `mp-hero-*.pak` (Eddy Raja, Navarro,
Roman, UDF Drake, Wetsuit Drake / Elena, Borneo Chloe / Sullivans, Prakoso, Javier, Glowzor, Platzor ...)
are named by the GOTY records under their own names and come from the PS4 game (the import writes none of
them).

Diagnostics (unlocks.cpp): the two `0x56a980` calls (`0x663339`, `0x663782`) are retargeted to log
`skins: spawn ...: model '<package>' -> <result>; package <id> loaded / not loaded (state), actor file from
PS4 game / import overlay; name table N entries, name listed / NOT listed` (first 32), and the table build
logs `skin actors: N records from the PS4 game, M from the import overlay, missing: ...`. A NULL result with
"name NOT listed" = the name table `0x14705f0` lacks the actor; non-NULL = the model object exists and the
problem is in the actor file or its drawing.

## 7. Campaign skins in the Profile lists (2026-11-09)

The hero / villain lists (cond 40/41, `0x6595a0`, reset op `0x6597b7`) walk only the MP store sections `681022f5`
(heroes) and `4252add5` (villains) and show an item when a skin record has its id; the record's side is not read.
unlocks.cpp now binds a copy of `92c6db02` whose two skin sections carry an item (a sibling as template, no
condition) for each skin that had none, and appends their records after Baseball Shirt Drake:
Zoran Lazarević (`mp-hero-lazarevic`) and Prakoso (`mp-hero-pirate`) (records without items: net-patch9 swapped
their ids with the ND / Doughnut twins), the campaign costumes Lazarević - Healed (`costume-hero-lazarevic-healed`,
villain), Drake - Dirty and Beat-up (`hero-dirty-beatup`, hero), Sir Francis Drake
(`costume-hero-skeleton-sir-francis`, hero: a guess, as Marco Polo), and the GOTY MP skins ND Drake and Santa
Drake. Every other campaign costume (store.bin `855a9c58`) already has an MP twin in the lists. The campaign packages
share the MP skeleton (LOD0 only); not yet spawned in a match. The table order changed: every PC needs this build.
