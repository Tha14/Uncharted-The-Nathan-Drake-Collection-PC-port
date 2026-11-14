# PORT-NETPATCH: the PS3 server live-data module net-patch9.bin

Test run 33 reached Playing on Gompa, the spawners loaded, then the game asserted
`LookupSymbolValue: Symbol '0x8cf8e1cc' not found` (lookup 0xa15e70, assert 0xa15f25).

## The symbol

`0x8cf8e1cc` is a **weapon set** (type header `0x1755b0c7` = sid("weapon-set")). Its name is not in any
binary (brute force over likely names found none). The `*net-games*` record (stride 0x90) names its weapon set
at +0x30; the spawn code reads it (PS4 0x6633db..0x6634b7: default `0x42a5be7f`, and `0x8cf8e1cc` /
`0x469fefc8` are special-cased) and the loadout code looks the set up by id. The PS3 disc `dc1/net.bin` defines
it (82 symbols), the PS4 `dc1/ps4/net.bin` keeps 52 of those symbols and drops the weapon sets, and the PS3
server module `net-patch9.bin` (91 symbols, a superset of the PS4 net.bin's 52) defines it: an array of 5
entries `{string ptr ("pistol-semi-b", "assault-rifle-a", "hand-grenade-b", "none", "none"), sid, int ...}`.
39 symbols of net-patch9 are not in the PS4 net.bin (weapon sets `0x8cf8e1cc`, `0x42a5be7f`, `0x469fefc8`,
`0x556dd82a`, `0xe58a9398` and others). Other known ids: `0x6f1c3f87` = `*net-version*`, `0x916aa605` =
the module root type, `0x1ed2e0a2` = `array`, `0x16094dd2` = `map`, `0xc7cb275c` = `int32`.

## How PS3 1.10 GOTY loaded it (NPUA80698 EBOOT)

| PS3 | What |
|---|---|
| `0x447a5c` | NetInit thread (net-livedata, `u2livedata.final1.prod`, S3 bucket): `0x4e7e58` downloads `net-patch9.bin` into the data folder (`%sDATA2`), then `0x4e83c8("%s/%s")` |
| `0x4e83c8` | LoadNetPatch: `0x910c10` 256 KiB (heap 9, align 16), `0x4e7988` read, `0x156840` relocate -> root slot; `LookupSymbolValue(*net-version*)` + `0x14d088` find in module (result unused); every root entry except `0x87b03989` -> `0x151aa8` BindSymbol(sid, &ptr); an `0x87b03989` entry would run as a script (`0x151e18`) - net-patch9 has none |
| `0x151aa8` / `0x1518cc` | BindSymbol: hash map `0xeb98d0` (FNV-1a of the 4 sid bytes), replaces the value of an existing node |
| `0x151d4c` / `0x151bd8` | LookupSymbolValue: sorted native table `*0xeba398` first, then the hash map; DC modules bind their symbols there too (module loader `0x156bb8` -> `0x151aa8`) |

So net-patch9 *replaced* the disc net.bin's tables and *added* the missing ones.

## PS4 1.02 loader pieces

| PS4 | PS3 | What |
|---|---|---|
| `0xa19a00` | `0x156840` | RelocateModule(buf): asserts 16-byte alignment and '00CD', reloc u32 at +8, bitmap at buf+reloc, adds buf to each marked 8-byte slot, returns buf+0x10 |
| `0xa16d70` | `0x151aa8` | BindSymbol(u32 sid, const u64 *value): buckets `*0x15340d0`, count `0x153410c` (0x1000), free list `0x15340d8`, nodes {next, prev, sid +0x10, value +0x18}; asserts when the pool is empty |
| `0xa16f30` | - | UnbindSymbol(sid) |
| `0xa15300` | `0x151bd8` | DcFind(sid): native table `*0x1534200` ({sid, value} stride 16), then the hash map; NULL if absent |
| `0xa15e70` | `0x151d4c` | LookupSymbolValue (asserts) |
| `0xa17020` / `0xa17160` | - | find in / iterate one module (module list `0x1534140`, +0x98 name sid, +0xd0 root) |
| `0xa17600..` | - | script manager init: hash pool, native table, then the startup modules (name list `0x1044800`, `"%s/build/main/dc%u/ps4"` + `"%s/%s.bin"`) |

net.bin is a startup module, so its symbols are bound before Multiplayer is entered. The net-livedata code and
LoadNetPatch do not exist on PS4.

## Module format (U2; differs from U3)

- PS3 `DC00` BE: `+8` reloc, `+0x10` root ptr, `+0x14` header `0x916aa605`, `+0x18` root `{u32 1, u32 n, {sid, ptr}[n]}` (sorted);
  relocations at reloc: `{u32 count, u32 offset[count]}` + zero padding (a list, not a bitmap).
- PS4 `00CD` LE: `+8` u64 reloc, `+0x10` u64 root ptr (0x20), `+0x18` `[0][0x916aa605]`, `+0x20` root `{1, n, {u32 sid, u32 0, u64 ptr}[n]}`;
  relocations: `{u32 bytes, bitmap}`, bit k of byte i = 8-byte slot (8i+k)*8.
- Object type headers stay **4 bytes** on PS4 (U3: 8 bytes `[0][type]`); the body alignment decides any padding.

## Conversion (u2mp/patcher/importer/netpatch.cpp)

The U3 DC converter (dctools `dcconv`) runs with its Uncharted 2 differences (`u3::g_u2`; `u3import.exe` never sets it):
U2 header parser, 4-byte headers, pointers after a 4-byte header, `array` / `map` keyed by type
+ referring field (their layout depends on the element type), PS3 trailing zero padding trimmed for fixed-size
keys. Symbols are keyed by their own sid, so net-patch9 objects reuse the layouts of the same PS4 symbols.
Training: the 111 PS3 `dc1.psarc` / PS4 `dc1/ps4` module pairs (88 with the same graph learned whole, 23
learned partially by walking shared symbols) plus net-patch9 against the PS4 net.bin as a partial pair (655
objects with the same contents). Output: U3 layout shifted 0x10 into the U2 header.

Quality (2026-11-08):
- Whole-module test (PS3 disc module -> compare with the PS4 file): 37 of 88 same-graph pairs byte-identical
  (the other 23 pairs differ in content, PS4 was built from other PS3 versions). Remaining differences are mostly
  script lambdas / anim tables (ss-*, anim-*), not net data.
- net-patch9: 91 symbols, all 2864 pointers resolved (`check_module`: every slot inside the module, sorted root).
  2809 objects: 1999 with a learned layout, 810 with the generic layout (u32 words swapped, pointers 8-aligned
  after the header - the natural PS4 layout for u32/float/sid/pointer structs; wrong only for u16/u8/u64 fields).
  Held-out check before net-patch9 was used for training: of the net-patch9 objects whose contents equal a PS4
  net.bin object, ~62% came out byte-identical; most misses were the header of the next object kept as a data
  word (types only net uses), which the net-patch9~net.bin training now teaches.
- `0x8cf8e1cc` (and its 5 strings) use learned layouts: `weapon-set` learned from 25 instances, 0 failures.

Import: step net-patch (after the spawn paks): source `<title>DATA2/USRDIR/net-patch9.bin`
searched like `ps3_data_pak22` (`--ps3-data`, NPUA80698DATA2 first, then any DATA2), output
`u2data/build/main/dc1/ps4/net-patch9.bin`, schema cached in `work/dc`. First run learns the schema (~3 min).
Test import written to `scratch/u2mp/import-test/u2data/build/main/dc1/ps4/net-patch9.bin` (190918 bytes).

## Runtime (gow3_pc/src/u2mp/netpatch.cpp)

- `install_netpatch(installed)`: byte-checks `0xa19a00`, `0xa16d70`, `0xa15300`; patches nothing.
- `netpatch_load()` (idempotent): reads `/app0/u2data/build/main/dc1/ps4/net-patch9.bin`, validates header /
  root / bitmap / pointer range, copies it to a never-freed 16-aligned block, `0xa19a00` relocates, then each
  root entry is bound with `0xa16d70` **only if `DcFind` finds no symbol** (PS4 tables such as `*net-games*`
  stay the PS4 ones; `UNCHARTED_U2MP_NETPATCH=all` binds everything like PS3, `=off` disables). Keeps 48 hash
  nodes free for later modules. Logs bound / kept counts and whether `0x8cf8e1cc` is now defined. Missing or
  bad file: logs and returns.

**Lead:** add `install_netpatch(installed);` to install.cpp (+ declaration in u2mp_internal.h), and call
`netpatch_load();` in session.cpp `multiplayer_button` right after `netinit_prepare();` (before any match).

## Open

- Not run in game. The weapon-set entries' strings/ints come from a learned layout; other net-patch-only
  objects (39 symbols' subtrees) partly use the generic layout.
- If a later module unbinds a sid net-patch9 bound (only possible with `=all`), that symbol disappears again.
