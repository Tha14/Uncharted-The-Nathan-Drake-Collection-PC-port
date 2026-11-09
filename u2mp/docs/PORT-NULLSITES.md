# PORT-NULLSITES: NULL sites left by removed PS3 code

Code: `gow3_pc/src/u2mp/nullsites.cpp`, `install_nullsites(int &installed)` (not yet wired in install.cpp).

This is a systematic search of the PS4 1.02 executable for the class of bug behind the last crashes:
a PS3 accessor or init that PS4 removed, so the compiler folded its result to NULL/0, or a global
that nothing on PS4 writes and that code still dereferences. Addresses are PS4 image offsets. PS3 1.10
addresses are given as the reference.

## Method (scripts over the full objdump, image = file - 0x183ce8)

Scope: the multiplayer code at 0x5e0000-0x680000, plus every function reachable through direct calls
(up to 6 levels deep, 501 functions) from the match-path state methods. Those are the state vtables
0x100a9c0-0x100ac60: EstablishConnection, LoadLevels, WaitReady, Playing, the round/game over and
results states, and Leave. Also included: 0x5ec010, NetGameManager::Update 0x63e6a0 and spawn 0x662fa0.

1. **libc calls with a zero pointer argument.** The PLT stubs were named from the ELF JUMP_SLOTs.
   Every direct call to the str*/mem*/printf family was checked for an argument register set to 0
   (by xor or mov 0) in the same block. There are 22 hits in the whole executable. Only 4 are in
   multiplayer code, and only the 2 already fixed are on a reachable path.
2. **Zero, then cmov, then dereference without a check.** 3 hits in the MP range. 0x640598 is already
   fixed. 0x61d7c3 and 0x67e6d3 are false positives, because their control flow checks the pointer.
   The whole executable has many more hits, but those are the compiler's normal handle-resolve form
   `x = (slot && id match) ? p : 0`.
3. **Zero register used directly as a base.** Every hit is a handle resolve, with the same unchecked
   form as single-player code (`xor eax,eax; mov ..,[rax+N]` after an id mismatch). That is original
   game behaviour, not something the port introduced.
4. **Global pointer slots** (rip-relative, rw segment 0x104c000-0x173b898) that are loaded as a
   pointer and then dereferenced in scope. The script excludes slots with any non-zero store, with
   a non-zero initial image value, or written through `this` in a function that receives the object's
   address (interprocedural, 2 levels deep). Fields of the NetGameManager, the match session, the
   party, netInfo, the cinema manager and the menu manager were also followed through `this` in 2619
   functions.
5. **Results of stubbed functions.** There are no direct calls from the MP range to `ret` /
   `xor eax,eax; ret` stubs at all. The stubs are reached only through vtables (NetEvent Read/Write/
   Apply, menu callbacks), and their results are unused (void) or were already ported. Also checked:
   divisors folded to 0 (none), zero `this`/argument passed to a callee that dereferences it right away
   (none), and indirect calls through function-pointer globals that are never written (none in scope).

## Findings

| site | kind | PS3 ref | fix | confidence |
|---|---|---|---|---|
| 0x5f2aa3 WaitReady::Exit 0x5f2830 writes `[stream.data] = 5`. Stream 0x113a258 data is NULL. Pump 0x608c40 (from Playing::Update 0x5f177b) reads the same pointer | (b) global never set: no store of a non-zero data pointer anywhere, and the static initializer 0x5ef970 zeroes it | stream 0xf119b4; NetInit 0x448d68 attached a 128 KB buffer; WaitReady::Exit 0x443f00 | **nullsites.cpp**: a stub at 0x5f2aa3 attaches a 128 KB buffer (max_bits 0x100000) when data is NULL, then runs the original two instructions. This is a fallback: cinema.cpp's WaitReady::Exit hook does the same when cinema installs, and its `ensure_stream` swaps in its own buffer | high (byte-checked; only acts when data is NULL) |
| 0x66c75f / 0x66c76d strlen/memcpy(NULL) host name | (a) folded accessor | NetPlayerData name 0x4c4f74 (inlined) | already fixed: remaining.cpp | - |
| 0x64059d `mov edi,0; cmovne rdi,rsi` | (a) folded room accessor | - | already fixed: remaining.cpp | - |
| 0x642427 / 0x642438 strncpy(dst, NULL, n) | (a) folded name | AddOrUpdateNetPlayer 0x49ada8 | none: 0x642370 has no callers and no pointer slots (dead). session.cpp ports it | high |
| 0x113a040 net clock | (b) never written | [0xf116d4] | already fixed: session.cpp | - |
| 0x14ce9b8 list read in 0x5eb830 | (b) never written | PS3 net init room attributes | already fixed: session.cpp replaces 0x5eb830 | - |
| 0x113ab40 (cinema +0x1b0 entries) read in 0x5e3fa8 | (b) never written | cinema manager 0xf1b590 | already handled: cinema.cpp replaces PAGE_PROGRESS 0x5e3f60 and sets the lists | - |
| 0x113a258 readers in 0x5e56a0 / 0x5e5c80 / 0x5e9630 | (b) | replay states 0x437fb0 / 0x436220 / 0x435be8 | none: replay-only states, cinema.cpp. The fallback buffer above also covers them | medium |
| 0x1130328 (0x1130008+0x320) in 0x646ad0 / 0x649700 / 0x668980 | candidate (b) | 0x1026a88+0x120 via 0x8a0438 | none: false positive. The HUD init 0x552c70 writes +0x310/+0x320/+0x330 through `this` (0x5534a3). 0x646ad0 is dead anyway | high |
| 0x1136ff8 (menu +0x9b8) in 0x5eb0cf / 0x5ed763 | candidate (b) | - | none: null-checked just before (`cmp [rbx+0x9b8],0; je`) | high |
| 0x10608e0, 0x1241450, 0x1116168, 0x1130068, 0x13d2840 | candidate (b) | - | none: engine globals with 9-107 readers each, mostly in single-player code, so they are set (through ctors the scan does not follow). The MP readers are the same as the single-player ones | medium |
| 0x113d300 / 0x113d370 / 0x113d4e0 / 0x113d908 (NetGameManager fields) | candidate (b) | - | none: written by NetGameManager init 0x63d940 through `this` | high |
| 0x61d7c3, 0x67e6d3 | (2) cmov | - | none: checked by control flow / list head | high |
| 0xbdf10a strncpy(NULL), 0xbe83de / 0xbe83f9 strchr(NULL), memset(NULL, ..) at 0x3528a3 / 0xa3b596 / 0xb282a9 / 0xc4b60e and others, 0x175635 vsnprintf(NULL, 0) | (a) folded debug names | - | none: debug-only render / print paths outside multiplayer and not reachable from the match path. vsnprintf(NULL, 0) is a legal length query | medium |

## Current crash (boot31) is not in this class

The fault at 0x6630dc is the `int 0x41` assert in GetSpawnLocator ("Missing spawner"). It fires when the
spawner id is not in the level package's +0x9a0 table. That is data, not a NULL site: it is the
level-package choice in remaining.cpp `level_record_package` (0x644130).

## What is left

- The typed vtable pass is done (2026-11-09, last section). Nothing needed porting.
- The scan scripts are in the session scratchpad (`ns/`: conv.py, scan1-5.py, gl.py, objf.py, check.py).
  They are not in the repo (job tools).

Note (2026-11-08): the shader-binding guards that were nullsites.cpp section 2 (runs 35-37) moved to render.cpp;
see PORT-RENDER.md for the root cause (cbuffer list capacity in converted actor paks).

## Typed pass over the MP vtable stubs (2026-11-09)

Static analysis only. Input: every vtable `vt.py` lists (at least a third of its slots in
0x5e0000-0x680000) with a `ret` or `ret 0` slot. For each one the class was identified from its
constructor (the vptr store) and its other slots. The PS3 vtable was then found by content: slot
functions with the same query op, callee or state role, and anchors where both games share a base
default.

Rule for "shared": a stub that 20 or more PS4 vtables point at is a base-class default. Examples:
`5df3a0` (49 vtables), `579670`/`579660`/`5794d0` (menu widget base, 36-49), `d78130` (107), `2030b0`
(20). A Bluepoint strip leaves a `ret` at the function's own address, referenced by one vtable only.

| PS4 vtable | class | stub slots (PS4) | PS3 pair | verdict |
|---|---|---|---|---|
| `100a520` | base of several frontend classes (vptr stored by 4 constructors) | 3 `5df3a0` (shared) | base default | nothing lost |
| `100a6d0` | TaskManagerReplay Leave state (CINEMA.md) | 2 Exit `5e5510` | `d70288` Exit `0x432840` = `blr` | empty on PS3 too |
| `100aa40` | ShowResults state | 1 Enter `5f0ea0` | `0x43ec08` | already ported (gameplay3.cpp / cinema.cpp) |
| `100adf0` | NetCharacter state (Update `5f9610` calls NetCharacter update `0x605070`) | 2 Exit `d78130` (shared) | `d70788` (Update `0x457d08` calls `0x45ed60`), Exit `0x10774` = `blr` | empty on PS3 too. The other two NetCharacter states match as well: `100b370` = `d70b48` (Exit `blr`), and `100b2f0` = `d70b08`, which has a real Exit on both |
| `100b430` | menu query widget (helpers `65f4e0..660660`, MENUS.md) | 3, 4 (base `579670`/`579660`), 20 `660da0` (`xor eax,eax`), 25 (base) | `d71c48` (22 slots, `0x4ba400` ...). Aligned by query op: PS4 0/1/2/5/6/7 = ops 4/9/8/1/6/5 = PS3 0..5, PS4 12, 16, 18, 19, 21, 23, 24 = ops 10, 14, 15, 16, 17, 18, 7 = PS3 10, 14, 16, 17, 19, 20, 21. PS4 inserted base slots 3 or 4, 9 and 22 | slot 20 = PS3 `0x4b7124` `li r3,0; blr`: the same. Its caller (`0x58e471`) tests the bool. Base slots = PS3 base `0x40a410` `blr` / `ret -1`. Nothing lost |
| `100b510` | PS4 party session event listener (`this` = the party session object: slot 4 `661280` sets `+0xb50`, slot 2 `661230` releases a connection of `0x1152a10` through an NP import) | 3 `661270`, 6 `661430`, 7 `661290`, 8 `661420` | none: the interface belongs to the PS4 NP session layer, which PS3 did not have in this form (Matching2 callbacks) | not needed: only the PS4 NP library calls it, and PC has no NP session (session.cpp replaces it; party+0xb50: PORT-FRONTEND3.md) |
| `103fa50` | match session event listener, the same 9-slot interface (slot 2 `5e9dd0` releases the member's connection, 4 sets abort `0x113a270`, 5 sets `+8`/`+0xb55`) | 3 `5e9f00`, 7 `5ea040`, 8 `5ea050` (the same slots as `100b510`) | none (as above) | not needed (PSN only; SESSION-INTERFACE.md) |
| `100b570` | base snapshot visitor (slot 0 = base reset `66b270` = PS3 `0x4daf88`) | 1-6 | `d71e88`: slot 0 `0x4daf88`, 1-6 all `blr` | empty on PS3 too. PORT-GAMEPLAY3's pairing with `d70638` came from a single vote and was wrong: `d70638` is the CreateAnimCmds visitor `100ac80` |
| `103fb10` | locator visitor | 4 `5f5430`, 5 `5f53e0` (base defaults of `100b570`) | `d70608`: 4 `0x451b38`, 5 `0x451b3c` = base `blr` | empty on PS3 too |
| `100bc30`, `1041a50` | photo mode (`CameraControlPhotoMode`, PS4 only) | `2030b0`, `d78130` (shared); `67bf30` | none | not MP |
| `1041a00` | dev menu widget (`dev-widgets.h`, built at `0xb251ee`) | 3 `677920`, 4 (shared), 5 `677910` | none | not MP (debug widget; its code happens to sit at 0x677xxx) |
| net event vtables (`103fcc0` ... `1041520`) | NetEvent classes | slot 0, and slots 2/3/4 where stubbed | PS3 slot 0 bodies are 0x18 bytes: `stw vptr, 0(r3)` (destructor) | slot 0: no effect. Every stubbed Read/Write/Apply is ported: `events.cpp`, `gameplay2.cpp` (`63bda0`, `63c0e0`), `remaining.cpp` (`61b900`). Checked by address against the C++ |
| `100af70`, `100ad20`, `100adb0`, `100b370`, `100b3b0`, `100ac80` | paired by votes (PORT-GAMEPLAY3.md section 2) | | PS3 slots `blr` | empty on PS3 too |
| `103fab0` | NetAnimCmdGenerator | 0, 4 | | slot 4: netanim.cpp (PORT-NETANIM.md). Slot 0 is the destructor (PS3 `0x4520f0`: vptr reset, then the base destructor), no effect |

Side note: PS3 `d70668` (slots `0x4525e8` ... `0x452880`) is the snapshot debug-print visitor
("AnimCmdBreakLayer:", "AnimCmdBlend: ..."). Its PS4 pair is intact (`5f5970`, `5f5bf0`, `5f5cd0`).

Result: no vtable stub on PS4 lost a PS3 body that matters on PC. No new code. Limit: the selection
only covers vtables with MP-range code. A stripped MP method in a class whose code lies elsewhere
would not show up here. The NULL-site scans (sections above) cover the data side of those.
