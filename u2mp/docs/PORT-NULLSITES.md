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

- Vtable stubs whose return values are used could only be checked where the class is known (NetEvent
  vtables and menu callbacks, all void). A typed pass over the remaining MP vtables (0x100af70,
  0x100b430, 0x100b510, match session 0x103fa50) has not been done. Their stub slots are listed by
  `vt.py`.
- The scan scripts are in the session scratchpad (`ns/`: conv.py, scan1-5.py, gl.py, objf.py, check.py).
  They are not in the repo (job tools).
