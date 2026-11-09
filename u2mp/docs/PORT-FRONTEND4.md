# PORT-FRONTEND4: loading screen, playlist pictures, image sizes (2026-11-09)

Static analysis only (nothing was run). PS4 1.02 image offsets; PS3 = GOTY NPUA80698 1.10. Files:
`gow3_pc/src/u2mp/loading.cpp` (new, `install_loading`), `menuimages.cpp`, `menus.cpp`, `frontend3.h`.
Install: `install_loading(installed);` after `install_menuimages(installed);` (declaration in
`frontend3.h`; the lead adds it to `u2mp_internal.h` or includes `frontend3.h` in install.cpp).

Asset rule (user, 2026-11-09): an asset the PS4 game has is the PS4 one, even under another name; the
PS3 / GOTY copy in net-u2mp.pak only where PS4 lacks it.

## 1. Loading screen

**Rejected in run 43** (invented layout, 1920x1080 coordinates where the 2D arena takes 1280x720
menu units, copied text style). Off unless `UNCHARTED_U2MP_LOADING=1`. The PS3 trace and the status of
each element: PORT-FRONTEND5.md. The text below describes the rejected version.

### PS3

- The per-frame net display `0x447248` (called from the frame draw `0x76c80`, PS4 pair Draw `0x3fc0b0`)
  runs the loading-screen object `0xf1ff04` while the byte `0xf119c5` is set: `0x4adaf8` (show / hide;
  on show `0x4ad600` starts the coin animation and `0x4233c4` marks PAGE_MULTIPLAYER_LOADING's lists
  for a refresh: page record +216 = 2) and `0x4ae638` (draw, 0xad0 bytes).
- `0x4ae638` draws: the band `general/net/loading-screen/title-back` (`270e3564`) with its edges
  `.../edge` (`6732636e`), the net game's map picture (level record `0x491f38`, +0xc), the map name and
  game type (TextDb `0x9294c4`), a tip, the two team rosters of PAGE_MULTIPLAYER_LOADING (`b9df67d6`,
  two action-23 lists, column set `dcae45f4`) drawn in place by `0x423410`, and the money / coins
  animation (`.../money` `9595f0a7`, `.../coins` `b4b036fa`, `0x4ad600`). Element placement moves with
  time (`0x4ad124`: sin / cos of the clock).
- `0xf119c5`: set by LoadLevels::Enter `0x4405c4` and WaitReady::Enter `0x43bd28`, cleared by
  LoadLevels::Exit `0x438448` and WaitReady::Exit `0x443f00`.
- PS4: none of these ids occurs in the code; PAGE_MULTIPLAYER_LOADING is in menu.bin but nothing shows
  it. The PS4 net.pak still has the four materials (`general/net/loading-screen:title-back, edge,
  money, coins`), so the screen uses the PS4 images (rule above).

### PC (loading.cpp)

- Show / hide on the same states: the Enter / Exit slots of the PS4 TaskManagerOnline state vtables
  (`[dtor, Enter, Exit, Update]`): LoadLevels `0x100ac00` Enter `5f2e50`, WaitReady `0x100abc0` Enter
  `5f2d90` / Exit `5f2830`; Playing::Enter `5f23e0` and Leave::Enter `5f00d0` also hide. session.cpp
  wraps only the Update slots (+0x18). Each slot is checked before it is replaced.
- Drawn each frame after the frame's 2D pass in Draw: the call `0x3fc52d -> 0x554340` is retargeted;
  the screen goes into the deferred 2D arena after the menus, so it is on top.
- Images: textured quads with a NULL draw list (menuimages.cpp `menu_draw_quad`, as the menu
  primitives of menudraw.cpp); materials by PS3 id through the image lookup (PS4 net.pak first).
- Text: the menu renderer's own text call `0xbda9c0(arena 0x124a000, style, text)`. The 0xa0-byte style
  (+0 x, +4 y, +0x10..+0x1c clip, +0x20 / +0x24 scale, +0x28 colour, +0x30 font of the language, +0x34
  flags, bit 2 = drawn, +0x50.. transform, +0x94 wrap width, +0x98 limit; built by the renderer's text
  case `0x5bdfe0..0x5be615`) is copied from the first menu text the renderer draws (call `0x5be615`
  retargeted) and positioned per line.
- Data: the net game (`ngm+0x1ca0`) -> `*net-games*` +4 level -> level record (`0dec97a4`, stride 0x38:
  +4 name text as the vote rows read it at `0x65b0c6`, +0xc map picture), +0xc game type -> name text
  (`f985c7b6`, stride 0x44); a random tip of DC `b25ab071` (stride 0x14, text +0xc, as the matchmaking
  status `0x65acd9`).
- Layout (screen 1920x1080, the renderer's box units): map picture 1280x700 at (320, 50) cropped as PS3
  type 0x22 (u 0.0898..0.9102); band 1920x340 at y 740; edge strip at y 730; coins 240x240 at
  (1560, 800); map name (scale 1.4), game type (1.0) and tip (0.8, wrap 1100) at x 360. Fade-in 0.3 s.
- Switches: `UNCHARTED_U2MP_LOADING=0` (off), `UNCHARTED_U2MP_LOADING_TEXT=0` (images only).
- Log: `loading screen: installed`, `text style captured`, `shown (LoadLevels::Enter)`, `hidden (...)`.

Not ported: the PS3 motion of the elements (`0x4ad124`) and the coin / money animation (`0x4ad600`):
the layout is fixed at the places above (approximating PS3, not traced instruction by instruction);
the team rosters (PS3 drew PAGE_MULTIPLAYER_LOADING in place with `0x423410`, which has no known PS4
pair; showing the page with ShowPage during level loading was not risked). Text placement depends on
the captured style's transform; if text appears offset in the test, `UNCHARTED_U2MP_LOADING_TEXT=0`.

## 2. Playlist pictures

PS3 pictures (`general/net/playlists/<name>`): the GOTY net.pak has ten: deathmatch, elimination, mlg,
plunder, coop, objective, grab-bag, default, survival, the-lab. PS4 has deathmatch, objective, coop,
survival, mlg, default (used as PS4's); net-u2mp.pak adds elimination, grab-bag, plunder, the-lab.
kPlaylistDefs (eleven playlists) had coop twice (Co-op Objective, Co-op Arena) and survival twice (Gold
Rush, Survival). Now each definition lists its pictures in order of preference and takes the first one
no earlier playlist took (`pick_picture`): Co-op Arena -> survival, Gold Rush -> default, Survival ->
survival when Co-op Arena did not take it, ... With ten pictures only an eleventh playable playlist can
repeat one. Log: `menus: playlist <key> -> picture <id>`.

The image lookup (`0x5558c0` port) now looks a net id up in the PS4 net table (by its PS3-form name)
before net-u2mp.pak, per the asset rule. Consequence: the mode buttons (matchmaking, custom game,
cinema) are PS4's again; u2netimg.py's PREFER_PS3 list no longer wins at run time (the patcher can drop
those three from net-u2mp.pak; patcher files are not this job's).

## 3. Oversized images (Profile page and others)

Cause: a type 3 image is as large as its largest texture times the descriptor scale (PS4 converter
`0x5bc290` type 3 -> `0x5bcc1a` -> `0x5bbd90`; PS3 `0x41fd18` case 3 -> `0x419d08`: the same rule), in
menu layout units, which menu.bin keeps from PS3 (same widths on both, e.g. 0x140; the converter's
fixed sizes equal PS3's: 0x24 440x256, 0x25 440x306, 0x1e 128x88; 0x22 / 0x26 / 0x27 fold PS3's 0.8
into the constant: 336 = 420 x 0.8, 400 = 500 x 0.8, 102.4 = 128 x 0.8). Every PS4 net.pak texture that
PS3 also has is exactly twice the PS3 one (mode buttons 256 / 128, player portraits 512 / 256, playlist
and map cards 1024x512 / 512x256, rank sheets 1024 / 512). So every PS4 net image drew at twice the
PS3 size: the Profile page's hero / villain portraits and booster icons, the mode buttons, cards.

Fix (menuimages.cpp): both calls of `0x5bbd90` (`0x5bcc1a`, `0x5bdb30`) go through `image_size`, which
halves the size of a material from the PS4 net table; net-u2mp images (PS3 textures), the hud and
menu tables are unchanged. `UNCHARTED_U2MP_NET_IMAGE_SCALE=<f>` sets the factor (1 = off). Log: `menu
images: PS4 net.pak images measured at 0.50 of their texture size`.

The ported types (0x1e, 0x22, 0x24, 0x25, 0x27) draw into the box the converter computed, as PS3 did
(`0x41aa48` uses the box), so they were already PS3-sized.

## 4. Other checks

- MP page labels against the text banks: every label of the multiplayer pages is in the PS4 bank
  except `d4143b18` NET_CLANTAG (PS3 dlc-patch7 bank, served by the import; strings agent).
- Session (not changed here; session.cpp is not this job's): the package preload log of run 41 says
  `0 requested, 1 already present, 20 not created`: RequestPackage `0x56ad90` creates nothing for the
  weapon and anim-proto packages (they are actor packages, not pak22 levels). The PS3 preload therefore
  has no effect yet; the request path for actor packages is open.
