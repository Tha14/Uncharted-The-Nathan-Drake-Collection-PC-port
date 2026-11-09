# Port job "hud": the multiplayer HUD in matches

Static analysis only (nothing built or run). PS3 = 1.10 EBOOT (NPUA80698), PS4 = Collection 1.02 image.
Code: `gow3_pc/src/u2mp/hud.cpp`, `install_hud(int &installed)`.

## 1. Why nothing showed

The multiplayer HUD is C++ in `game/hud/hud.cpp`, not a script. The HUD manager (`0x1130008`, PS3
`0x1026a88`; init PS3 `0x8a13e8` = PS4 `0x552c70`) creates one multiplayer element: PS3 ctor
`0x8ac330` = PS4 `0x55aa80`, stored at hud+0x320, element bits 0x2000, vtable PS3 `0xd87db8` = PS4
`0x103f550`:

| slot | PS3 | PS4 | state |
|---|---|---|---|
| 0 Update | `0x8ac1e8` (calls the sub-update `0x8abf00(+64)`, names `0x8aa448(+2080)`, ORs the HUD flags 0xe0000 while ngm state != 0 and not cinema, element fade +6368) | `0x55abe0` (all of it inlined: +0x40, +0x860, `or $0xe0000` at `0x55c51f`, fade +0x1920) | intact |
| 1 Draw | `0x8aef68` (0x1158 bytes) | **`0x55c5b0` = `ret`** | stripped |
| 3 Reset | `0x8a9b90` | `0x55abb0` | intact |

The HUD loop `0x554710` (inside the per-frame HUD update `0x554340` = PS3 `0x8a2b48`) calls slot 1
of every element whose bits are in the flags, so the element is drawn every frame, but slot 1 does
nothing. All the helpers only the Draw called are also missing from the PS4 image: `0x8ac3f0`,
`0x8ad018`, `0x8adb60`, `0x8a8ed8`, `0x8a9e48`, `0x8abc68`, `0x8ab600`, `0x8ab4d0`, `0x8a9bbc`.
Their text formats ("%i:", "%02i", "x%2.1f") and DC ids (`d38a7d18`, `e1167dfa`, `12ffe6ae`) are
not in the PS4 image. The HUD package, the DC widget data and the script natives are all present:
PS4 `dc1/ps4/hud-settings.bin` record `9353ed16` has the PS3 multiplayer widget set at the same index.

## 2. Ported (hud.cpp): the score / time panel of 0x8aef68

| PS3 | PS4 |
|---|---|
| IsMultiplayer `0x4384b0` | NET_SM `0x14ce97c` >= 2 |
| HUD flags `[[0x1026a88+56]]` | `[[0x1130008+0x60]]` (the PS4 flag code `0x554386..` / `0x55c506` uses it) |
| settings `0x151d4c(9353ed16)`, multiplayer set rec+0x58 | `0xa15e70(9353ed16)`, rec+0x90 (16 pointers at +0x20 on both: PS3 +0x20+4k = PS4 +0x20+8k; the SP timer `0x559770` reads +0x88 = PS3 +0x54) |
| set fields +4 / +8 / +24 / +28 / +32 / +36 / +44 | +0x08 / +0x10 / +0x30 / +0x38 / +0x40 / +0x48 / +0x58 (layout checked in both hud-settings.bin files) |
| text `0x8a0f04(widget, pos, scale, fmt, ...)` | `0x5555a0(widget, pos, fmt, float scale, ...)` (variadic, xmm0 = fixed scale; as `0x559770` calls it) |
| image `0x8a10fc(widget, pos, scale)` | `0x5559f0(draw ctx, widget, pos, scale)`; the ctx is slot 1's second argument |
| text `0x9294c4(0x109d3f0, id)` | TextDb::Find `0xcddc00(0x1066660, id)` |
| local tracker `0x4919d4` | ngm+0 |
| net game record `0x499908` | `*net-games*[ngm+0x1ca0]` |
| co-op test `0x4a0bcc` (type 4/8/9/12) | same |
| team tracker+300 | tracker+0x134 (the CheckGameOver team loop `0x63f4ae`) |
| team scores `0xf2efc4+15840+14+52*t` (u16) | `[0x104ae70]+0x3dee+0x34*t` (CheckGameOver `0x63f3f7`) |
| score limit +7220, time limit +7224, round start +6560, match start +6564, round +6580 | +0x21b4, +0x21b8, +0x1d5c, +0x1d60, +0x1d70 |

Logic (PS3 `0x8af000..0x8af960`): only competitive games (not co-op, not type 11), only with flag
0x40000:
- **type 10, elimination** (`0x8af648`): frame image, own and other team score, label `e1167dfa`,
  round ngm+0x1d70 + 1, time left from the match start;
- **other types** (`0x8af220`, e.g. type 0 deathmatch): frame image, own and other team score,
  label `d38a7d18`, score limit, time left from the round start;
- **time left** (`0x49e0e8`, `0x8af878`): limit = time limit x 300 frames. When it is > 0,
  secs = (limit - elapsed) / 300, shown as minutes % 60 "%i:" and secs % 60 "%02i".

Install checks: the prologues of `0x5555a0` / `0x5559f0`, the ctor's vptr load `0x55aab2` and bits
store `0x55ab94`, the ret stub at `0x55c5b0` and the vtable slot value. Test switch:
`UNCHARTED_U2MP_HUD=0` leaves the stub. The log prints `hud: multiplayer draw #1 ...` at the first
call, then every 600th call (type, flags, team, scores, limits). That line proves the element is drawn.

## 3. Not ported yet (the rest of 0x8aef68; PS3 addresses, PS4 element offsets)

| Part | PS3 | Gate | PS4 data |
|---|---|---|---|
| type 7 panel: per-player list with score bars (`0x8a015c`, `0x8a1f88`, `0x8a9e48`, `0x12c070`) | `0x8af960..0x8afe6c` | type 7 | set +0x60 (PS3 +48) |
| objective carrier icon (ngm+6412/6416 handle, per-tracker loop `310f465e`) | `0x8af360..0x8af41c`, `0x8afe70..` | competitive | set +0x28 (PS3 +20) |
| co-op type 9 money / wave (`49cca8` "x%i", `49cae8`, `0x8a9e48`) | `0x8af4e0..0x8af5c0` | type 9, flag 0x8000 | set +0xd0.. (PS3 +104/+112/+116) |
| booster icons (`0x45536c`/`0x45543c`/`0x4552b8` or tracker +608/+612, `0x4551b0`, draw `0x8a9bbc`) | `0x8af440`, `0x8af5c4` | flag 0x10 clear / 0x40000 | booster manager `0x113a788` |
| recording / multiplier indicator (`0x8a8ed8`, `0x8ac3f0`, "x%2.1f") | `0x8af620` | flag 0x100000 | element +0x1928 |
| kill feed / messages (`0x8abc68` -> `0x8ab600`; 16 entries of 124 bytes, PS4 0x800 bytes at +0x48) | `0x8af424` | flag 0x80000, not type 11 | element +0x40 |
| player names over heads, draw half (`0x8aeeb0` -> `0x8adb60`, 0x1350 bytes; the update half is intact in `0x55abe0`) | `0x8af4c4` | flag 0x20000 | element +0x860 |
| world markers (`0x8ad018`, 0xb48; projects with `0x8a2388` = PS4 `0x5566b0`) | `0x8af4d8` | flag 0x20000 | element +0x1560 |
| list `0x8ab4d0` | `0x8af1e8` | flag 0x20000 | element +0x1a90 |
| DrawText flush `0x73bf94(0)` | tail | - | not needed: PS4 HUD texts are queued by `0x5555a0` like the SP elements |

These need the same kind of mapping per helper. Ammo, weapon, grenade count and reticle belong to
the shared (single-player) HUD elements (vtables `0x103f360..0x103f510`). No stub was found among their
slots, but that was not checked against PS3 slot by slot. The medal and XP pop-ups were not located.

Unverified: nothing was run. Whether `0x5555a0` places the texts where PS3 did depends on the widget
data (positions come from the dc widget, the pos argument is {0, 0} as on PS3). Team scores for a
free-for-all game are the scores of team 0 / 1 as on PS3.
