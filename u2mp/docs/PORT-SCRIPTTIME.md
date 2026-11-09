# PORT-SCRIPTTIME: track-group delta time of net-timer scripts

Code: `gow3_pc/src/u2mp/scripttime.cpp` (`install_scripttime`). Addresses are PS4 1.02 image
offsets; PS3 = NPUA80698 1.10 EBOOT.

## Crash (test run 44)

Warzone, about 14 s into a match: `ASSERT(0)` in `GetDeltaSeconds`,
`game\state-script\ss-track-group.cpp` line 667 (0x29b), then `int 0x41` at `0xa4788f`.
Stack: `0xa47052` (track-group update `0xa46e..`) → `0xa477c0`.

## The flag

Process `+0x168` bit `0x8` is the state-script `is-net-timer` option. It is set at `0xa2e831`
when spawn info `+0x14` has bit `0x10`. The script natives `wait-until-net-time`,
`wait-until-frame` and `wait-until` check it (`0xa4091c`, `0xa40dec`, `0xa40f0c`) and print the
"You must (NOT) specify is-net-timer" warnings. On PS3 it is SsInstance `m_bUseNetClock`, process
`+264` bit 28 (`rldicl 0,0,36,63`).

## Clocks

`0x1116180` (PS3 `0xe60a00`) holds ten clocks at `+0x38 + 0x40*i` (PS3 `+0x38 + 0x38*i`).
`Clock::Update` is `0x476ee0` and is called for all ten at `0x4ca6df..0x4ca74c`. Each clock has:

| field | PS4 | PS3 |
|---|---|---|
| ticks (1/300 s) | `+0xc` | `+0xc` |
| delta ticks this frame | `+0x18` | ? |

- `clocks[0]` (`+0x38`) is the game clock: ticks at `0x1116180+0x44`, delta at `+0x50`.
- `clocks[7]` (`+0x1f8`, PS3 `0xe60bc0`) is the realtime clock, the one `matchend.cpp` uses as its
  "300 Hz frame counter". Its ticks are at `0x1116180+0x204` and its delta at `+0x210`. It keeps
  running during pause because of flag `+0` bit 1, set at `0x4c9735`.

## PS3 → PS4 mapping

| what | PS3 1.10 | PS4 1.02 | net-timer case |
|---|---|---|---|
| process → clock | `0x7c6ed8` SsInstance net clock time (asserts `m_bUseNetClock`) | `0xa2e3a0` GetClock | clocks[7], else clocks[0] |
| instance start time | – | `0xa2dfd9`, `0xa2f35a` | clocks[7] ticks |
| instance elapsed | – | `0xa44a7a` | clocks[7] ticks − start |
| track-group elapsed | `0x7da99c` (net: `0x7c6ed8` × 300; else `[0xe60a00+68]`) − tg start | `0xa46b90` | `[0x1116180+0x204]` − NGM start `[0x113b760+0x1d5c]` (PS3 `0x491b24`: ngm `+6560`) |
| track-group update | `0x7dab50` (calls elapsed only) | `0xa46e..` (elapsed `0xa47030`, then delta `0xa4704d`) | – |
| track-group delta | **none** | `0xa477c0` GetDeltaSeconds (also called from `0xa414d8`) | **ASSERT(0)** |

PS3 has no delta function. PS4 added `GetDeltaSeconds` and wrote only the game-clock case,
`[0x1116180+0x50] * 1/300` (`0xe6759c`). The net-timer case became an assert because nothing
single-player uses net timers. Every other net-timer reader on both platforms uses `clocks[7]`, so
the missing case is the frame delta of `clocks[7]`.

## Fix

The assert block at `0xa4785c..0xa47893` is reached only when the flag is set. It is overwritten
with this code, and the rest of the block is filled with `int3`:

```
0xa4785c  lea rax,[rip+0x6ce91d]          ; 0x1116180
0xa47863  vcvtsi2ss xmm0,xmm0,[rax+0x210] ; clocks[7] delta ticks
0xa4786b  jmp 0xa4789f                    ; vmulss 1/300; pop rbp; ret
```

All 86 bytes from `0xa47853` to `0xa478a9` (flag test, assert block, game-clock return) are checked
before writing. The owner lookup and the game-clock path are not changed. No host code runs, so
there is no guest→host call per track group.

The other net-timer sites (`0xa2dfd9`, `0xa2e3a0`, `0xa2f35a`, `0xa44a7a`, `0xa46b90`) already
handle the flag the way PS3 does. The three native warnings only print, so they need no change.
The NGM start time `+0x1d5c` is written by the network game code (`0x64858d`).

## Install

```
void install_scripttime(int &installed); // scripttime.cpp: net-timer track-group delta (PORT-SCRIPTTIME.md)
    install_scripttime(installed);
```

Not verified in game yet (not run, not built). Expected log line: `GetDeltaSeconds (0xa477c0):
net-timer scripts use the realtime clock delta`.
