# Spec: cinema (theater) recording and playback

Clean-room description of `gow3_pc/src/u2mp/cinema.cpp` and `gow3_pc/src/u2mp/cinema_store.cpp`.
The headers `cinema.h`, `cinema_store.h`, `u2mp_internal.h`, `bitstream.h` and `u2mp.h` stay unchanged.
Background and evidence: `u2mp/docs/CINEMA.md`. All addresses are image offsets of the Uncharted 2 PS4 1.02
build. "Guest call" means a call into the game image with the x86-64 System V convention (`U2MP_GUEST`).
All multi-byte values in game memory and in files are little-endian.

## 1. Purpose and where it hooks in

The PS4 build kept the game's replay task (states RePlay, Resetting, RePlaying, Leave), the code that reads
the replay stream, the code that writes the stream start (WaitReady::Exit) and the cinema list query. It lost
the file store, the recorder, the stream buffer, the PAGE_PROGRESS logic and most cinema menu actions.

This module restores them:

- Every online match is recorded into a file: the net events this console sends and receives are written into
  the game's replay stream, and the stream is streamed to disk.
- The cinema menus list, select, protect, unprotect, delete and play those files.
- Playback feeds the file back into the replay stream, which the game's replay task reads.
- Two functions let other modules share a recording with a friend (`read_selected`, `add_received`).

Hook points (details in section 2.3): one replaced pump function, two detoured send functions, one replaced
receive-hook stub, five swapped state vtable slots, ten replaced menu functions, and one patched branch inside
the cinema list query.

Other modules depend on what this module leaves in the image:

- `loading.cpp` (installed later) looks at vtable slot `0x100abd0`. When it holds a non-zero value outside the
  image, it chains that value. So the WaitReady::Exit slot must hold the plain host address of the hook
  function (no stub inside the image).
- `gameplay3.cpp` calls whatever is at `0x5f0ea0` (the replaced ShowResults::Enter).
- `nullsites.cpp` (installed later) adds a fallback stream buffer inside WaitReady::Exit. It relies on this
  module replacing any stream buffer that is not its own (section 4.1).
- `gamenet.cpp` stores the address `image + 0x5e9d80` into each message layer's receive-hook field (M+0x150)
  at run time. Replacing the function at `0x5e9d80` makes that hook the recorder.
- `online.cpp` calls `cinema::read_selected` and `cinema::add_received`; `remaining.cpp` provides
  `cinema_pick_friend`.
- `install.cpp` calls `install_cinema` once, after `install_progression` and before `install_remaining`.

## 2. Interface

### 2.1 Declarations (unchanged headers)

From `cinema.h` (namespace `u2mp`):

```
void install_cinema(int &installed);
namespace cinema {
void record_event(void *event);
bool recording();
bool read_selected(std::vector<uint8_t> &bytes, std::string &name);
int add_received(const uint8_t *data, size_t size, const std::string &name);
}
void cinema_pick_friend();   // defined in remaining.cpp, called by this module
```

From `cinema_store.h` (namespace `u2mp::cinema`): all constants (`kMagic`, `kHeaderSize`, `kMaxFiles`,
`kEntrySize`, `kModeWritable`, `kModeProtected`, `kBufferBytes`, `kWriteChunk`, `kReadChunk`,
`kDiscardChunk`, `kStartTag`, `kExtension`), the `hdr::` and `entry::` offsets, and:

```
uint32_t get32(const uint8_t *p);
void put32(uint8_t *p, uint32_t v);
uint64_t get64(const uint8_t *p);
void put64(uint8_t *p, uint64_t v);
bool valid_header(const uint8_t *header);
struct File { std::string name; uint64_t time = 0; uint32_t size = 0; bool protect = false; std::vector<uint8_t> header; };
std::vector<File> list(const std::string &dir);
void fill_entry(const File &f, uint8_t *out);
bool make_room(const std::string &dir);
bool remove(const std::string &dir, const std::string &name);
bool set_protected(const std::string &dir, const std::string &name, bool protect);
std::string new_name(const std::string &dir, std::time_t t);
bool ensure_dir(const std::string &dir);
struct Stream { uint32_t *data; uint32_t bits; uint32_t max_bits; uint32_t bytes; };
class Writer { ~Writer(); bool open(const std::string &path); bool is_open() const; void flush(Stream &s);
               bool finish(Stream &s, const uint8_t *header); void abandon(); /* private: f_, path_, failed_ */ };
class Reader { ~Reader(); bool open(const std::string &path, uint8_t *header_out); bool is_open() const;
               void fill(Stream &s); bool eof() const; void close(); /* private: f_, eof_ */ };
```

`cinema_store.cpp` must stay free of guest addresses: the unit test `u2mp-cinema-test` links it alone with
`gow3_pc/tests/test_u2mp_cinema.cpp`.

Host services used (from `u2mp_internal.h`, `install.cpp`, `frontend.cpp`): `image`, `log`, `near_stub`,
`replace_function`, `is_ret_stub`, `show_page`, `sid`, `addr::menu_manager`. The BitStream helpers
`write_bits` from `bitstream.h`. The C function `runtime_file_user_dir()` (declared `extern "C" const char *
runtime_file_user_dir(void);` by this module itself).

### 2.2 The store directory

The store directory string is: the value of `runtime_file_user_dir()`, or `"."` when that returns NULL or an
empty string, followed by `"/u2mp/cinema"`. A file path in this module's log lines and file opens is that
directory, then `"/"`, then the file name. (Inside `cinema_store` functions, paths are joined with
`std::filesystem::path` `/`.)

### 2.3 Entry points the game reaches

Every host function here uses the System V convention. Argument registers are as the game passes them.

| # | Where | How it is reached | Arguments | Return |
|---|---|---|---|---|
| E1 | `0x608c40` stream pump | function replaced | (cinema manager pointer, ignored) | u64 0 |
| E2 | `0x644ab0` NetGameManager::SendNetEvent | detour (original kept in a trampoline) | (ngm, event, u64 mask) | the original's u64 result |
| E3 | `0x66a3d0` NetSession::SendNetEvent | detour, same bytes as E2 | (session, event, u64 mask) | the original's u64 result |
| E4 | `0x5e9d80` receive-hook ret stub | function replaced | (event, float dt in xmm0, ignored) | none |
| E5 | slot `0x100abd0` WaitReady::Exit (`0x5f2830`) | vtable slot swapped | (state) | none |
| E6 | `0x5f0ea0` ShowResults::Enter ret stub | function replaced | (state, ignored) | none |
| E7 | slot `0x100a9c8` online Leave::Enter (`0x5f00d0`) | vtable slot swapped | (state) | none |
| E8 | slot `0x100a7a0` RePlaying::Exit (`0x5e8740`) | vtable slot swapped | (state) | none |
| E9 | slot `0x100a6d8` replay Leave::Enter (`0x5e5520`) | vtable slot swapped | (state) | none |
| E10 | `0x5e3f60` PAGE_PROGRESS handler | function replaced | (s32 *event) | none |
| E11 | `0x5e4060` PAGE_CINEMA handler | function replaced | (s32 *event, ignored) | none |
| E12 | `0x5e4080` LIST_CINEMA | function replaced | (s32 *control) | none |
| E13 | `0x5e4490` cinema refresh (`bf6960e8` / `c9f003a8`) | function replaced | (ignored) | none |
| E14 | `0x5e45d0` local/server list toggle (`fb637366`) | function replaced | (ignored) | none |
| E15 | `0x5e41d0` BUTTON_CINEMA_PROTECT ret stub | function replaced | (ignored) | none |
| E16 | `0x5e41e0` BUTTON_CINEMA_UNPROTECT ret stub | function replaced | (ignored) | none |
| E17 | `0x5e41f0` delete confirmed (`7bb3c875`) | function replaced | (ignored) | none |
| E18 | `0x5e42d0` BUTTON_CINEMA_UPLOAD | function replaced | (ignored) | none |
| E19 | `0x5e45a0` BUTTON_MULTIPLAYER_STARTCINEMA | function replaced | (ignored) | none |
| E20 | `0x657e7a` cinema list query (cond 69), case "op 9 set" | branch patched to a call | edi = s32 value (was r8d), esi = u32 local flag (zero-extended cl) | none; then the game continues at `0x657bea` |

Expected original bytes (checked at install, section 5):

| target | bytes (hex) | length = bytes patched |
|---|---|---|
| `0x608c40` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 81 e4` | 16 |
| `0x644ab0`, `0x66a3d0` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 58` | 17 |
| `0x5e3f60` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 50 8b 07` | 16 |
| `0x5e4060` | `83 3f 06 75 0c 48 8d 3d d4 25 b5 00 e9 7f 86 fe ff c3` | 18 |
| `0x5e4080` | `55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec` | 16 |
| `0x5e4490` | `48 8d 3d a9 21 b5 00 be 1e d1 84 1d 31 d2 e9 ed` | 16 |
| `0x5e45d0` | `48 8d 05 e2 ab a6 00 80 30 01 48 8d 3d 5f 20 b5` | 16 |
| `0x5e41f0` | `55 48 89 e5 53 50 48 8d 1d 43 24 b5 00 48 89 df` | 16 |
| `0x5e42d0` | `48 8d 05 b9 66 b5 00 8b 80 b8 01 00 00 48 8d 0d` | 16 |
| `0x5e45a0` | `55 48 89 e5 48 8d 3d 95 20 b5 00 e8 30 8c fe ff` | 16 |
| `0x657e7a` | `84 c9 0f 85 68 fd ff ff 44 89 05 27 80 e7 00 e9 5c fd ff ff` | 20 |
| ret stubs `0x5e9d80`, `0x5f0ea0`, `0x5e41d0`, `0x5e41e0` | `is_ret_stub` (`c3 66 66 66 66`) | 16 |

Vtable slots: the 8-byte value at the slot must equal `image.base + expected function`.

## 3. Game facts

### 3.1 Guest data

| address / offset | type | meaning |
|---|---|---|
| `0x113a990` | object | cinema manager ("C" below) |
| C+0x00 | s32 | file handle / "file open" marker (this module writes 1 or 0) |
| C+0x94 | u8 | reading (1 while a file plays) |
| C+0x95 | u8 | end of file reached (playback) |
| C+0x9c..C+0x9f | u8 x4 | control enable flags: +0x9c protect, +0x9d delete, +0x9e unprotect, +0x9f upload. Cleared as 8 bytes (C+0x9c..C+0xa3) |
| C+0x1a4 | s32 | selected index in the local list |
| C+0x1a8 | s32 | local list count |
| C+0x1b0 | u64 | pointer to the local list entries |
| C+0x1b8 | s32 | server list count |
| C+0x1c0 | u64 | pointer to the server list entries |
| C+0x1c8 | 0x4b0 bytes | current recording header |
| `0x113a258` | Stream (0x18 bytes) | replay stream: +0 u64 data pointer (u32 words), +8 u32 bit cursor, +0xc u32 max bits, +0x10 u32 bytes loaded |
| `0x113a040` | u64 | pointer to the net clock object; its u32 at +0xc is the frame counter |
| `0x113b760` | object | net game manager ("NGM") |
| NGM+0x1ca0 | s16 | current net game index |
| NGM+0x21b4 | 0x14 bytes | SetNetGame overrides |
| `0x104f1c8` | object | net info |
| `0x104f1c8`+0x2c4 | s32 | net mode; 3 = cinema |
| `0x104f1b9` | u8 | 1 = the cinema list shows local files |
| `0x14cfeb0` | s32 | remote list row (written by the list query for the server list) |
| `0x14ce098` | s32 | row LIST_CINEMA was pressed on |
| `addr::menu_manager` = `0x1136640` | object | frontend menu manager |
| menu_manager+0x17d0 | u64 | pointer to the message box data (may be NULL) |
| message box +0x08 | u32 | message id |
| message box +0x40..+0x5b | u32 x7 | message parameters |

Pointers read from game memory (clock object, message box, event vtable) are absolute host addresses, not
image offsets. Pointers this module writes into game memory (stream data, list entries) are absolute host
addresses of its own buffers.

### 3.2 Guest functions called

| address | role | arguments |
|---|---|---|
| `0x5cc6f0` | pop page | (menu_manager) |
| `0x5cb990` via `show_page(page)` | ShowPage | (menu_manager, page id, 0) |
| `0x63dc30` | SetNetGame | (NGM, s32 game index, NULL, s32 10) |
| `0x5cd1e0` | menu reset | (menu_manager) |
| `0xa572d0` | find task | (u32 task set `0x9f2aeafb`, u32 task id `0xad76460e`) returns a task pointer |
| `0xa5be10` | play task | (task pointer as returned, even NULL) |
| event vtable slot 3 (vtable+0x18) | NetEvent::Write | (event, pointer to the guest stream `0x113a258`) |

The event object: +0x00 u64 vtable pointer, +0x08 u8 event type.

### 3.3 Ids

| id | value |
|---|---|
| PAGE_PROGRESS | `0x1d84d11e` |
| PAGE_MULTIPLAYER_CINEMA_LIST | `0x735c4dbe` |
| PAGE_CINEMA | `0xca9509a0` |
| message page (no files box) | `0x719e9fec` |
| task set | `0x9f2aeafb` |
| task "replay" | `0xad76460e` (`sid("replay")`) |
| no-files message id | `0x2760b1fd` |
| no-files parameters at box+0x40.. | `0, 0xc6467f2d, 0x11d8e78a, 0, 0x1519fa3d, 0, 0` |

### 3.4 Store constants (cinema_store.h)

| name | value | meaning |
|---|---|---|
| kMagic | `0x04b00005` | header +0 |
| kHeaderSize | `0x4b0` | header bytes |
| kMaxFiles | 20 | list / store limit |
| kEntrySize | `0x6c8` | one game list entry |
| kModeWritable / kModeProtected | `0x180` / `0x100` | entry mode |
| kBufferBytes | `0x20000` | stream buffer |
| kWriteChunk | `0x8000` | write chunk |
| kReadChunk | `0x10000` | read chunk |
| kDiscardChunk | `0x4000` | discard chunk |
| kExtension | `".u2cinema"` | file extension |
| recorder size limit | `0x1fc00` bytes | no record is added past this |

Header fields used: +0x00 magic, +0x04 game index (u32/s32), +0x10 duration seconds (u32), +0x18 time_t (u64),
+0x20 overrides (0x14 bytes), +0x114 player count (s32). Entry fields: +0x000 date text (0x100), +0x100 name
(0x100), +0x200 stat byte, +0x208 u64 time, +0x210 u32 size, +0x214 u32 mode, +0x218 header copy (0x4b0).

## 4. Behaviour

### 4.1 Module state and shared steps

Host state:

- A stream buffer of `0x20000 + 64` bytes (0x20040), 64-byte aligned. The 64 spare bytes past 0x20000 exist
  because the game's readers look one word ahead. "Clear the buffer" always means zeroing all 0x20040 bytes.
- Local list entries: 20 x 0x6c8 bytes, 16-byte aligned. Server list entries: another 20 x 0x6c8 bytes, never
  filled (always zero).
- One Writer, one Reader, flags `recording` (false), `record_enabled` (true), `can_record` (false), a steady
  clock start time, and a one-shot page request {pending, load first file, next page id}, initially empty.

Shared steps (named here only for reference):

- **Attach stream.** If the guest stream's data pointer is already this module's buffer and its max bits is
  `0x100000`, nothing happens. Otherwise clear the buffer and set the guest stream to data = buffer, bits 0,
  max bits `0x100000`, bytes 0.
- **Attach lists.** If C+0x1b0 does not hold the local entries' address, write that address there and set
  C+0x1a8 = 0 and C+0x1a4 = 0. Then, every time, write the server entries' address to C+0x1c0 and 0 to C+0x1b8.
- "count" and "selected" mean the s32 values at C+0x1a8 and C+0x1a4, read from game memory at the time of use.
  "In range" means 0 <= selected < count. "Entry i" means the host local entry i; its name is the C string at
  entry+0x100.
- **Update flags.** Zero 8 bytes at C+0x9c. If selected is in range: if (entry mode & 0x180) == 0x180, set
  C+0x9c = 1 and C+0x9d = 1; else set C+0x9e = 1. Then set C+0x9f = 1. (Out of range: all stay 0.)
- **Refresh.** Attach lists. Ensure the store directory exists (result ignored). Get `list(dir)`. Zero all
  20 local entries. Fill entry i from file i with `fill_entry`. Set count = number of files. If selected >= n
  or selected < 0, set selected = n - 1 when n > 0, else 0 (note: a negative selected becomes n - 1, not 0).
  Write u8 1 to `0x104f1b9`. Update flags.
- **Load(index).** Attach lists. If index is out of [0, count), stop (result false). Set selected = index.
  Copy the 0x4b0-byte header from entry index (+0x218) to C+0x1c8. Guest call SetNetGame(NGM, g, NULL, 10),
  where g is the low 16 bits of the header's u32 at +0x04, sign-extended. Update flags. Result true.
- **Pop page.** Guest call `0x5cc6f0(menu_manager)`.
- **No-files message.** Read the u64 at menu_manager+0x17d0. If 0, stop. Else write u32 `0x2760b1fd` at
  box+0x08, then the 7 u32 values `0, 0xc6467f2d, 0x11d8e78a, 0, 0x1519fa3d, 0, 0` at box+0x40..+0x5b, then
  `show_page(0x719e9fec)`.
- **Frame clock.** Read the u64 at `0x113a040`. If 0, the clock is 0. Else it is the u32 at that address+0xc.
- **Close playback.** If the Reader is not open, nothing happens. Else close it, write s32 0 to C+0x00 and u8 0
  to C+0x94.

### 4.2 Recording an event (E2, E3, E4, `record_event`)

Nothing happens when not recording, when the event pointer is NULL, when the guest stream's data pointer is not
this module's buffer, or when (bits + 7) / 8 > 0x1fc00 (u32 arithmetic on the stream's bit cursor).

Otherwise, at the stream's bit cursor, using `write_bits` with the stream's data, cursor and max bits:

1. 32 bits: the frame clock, or 1 when the clock is 0 (0 is the end marker).
2. 8 bits: the event's type byte (event+8).
3. Store the new cursor into the guest stream's bits field.
4. Guest call the event's Write (its vtable slot at +0x18) with (event, guest stream address `0x113a258`).

E4 (receive hook) does only this. E2 and E3 first record, then call the original function through the
trampoline with the same first three integer arguments, and return its result. Only those three arguments are
forwarded.

### 4.3 Starting a recording (E5)

E5 does, in order: attach stream; call the original WaitReady::Exit with the state; then the start step:

1. Stop silently if `record_enabled` or `can_record` is false.
2. Finish any running recording (4.4).
3. Close the Reader (plain close; C+0x00 and C+0x94 are not touched here).
4. Ensure the store directory. On failure log `"cinema: cannot create %s, match not recorded"` (directory) and
   stop.
5. `make_room(dir)`. On false log `"cinema: all %d recordings are protected, match not recorded"` with 20 and
   stop.
6. Write the header at C+0x1c8: +0x00 = `0x04b00005`; +0x04 = NGM+0x1ca0 as s16 sign-extended to 32 bits;
   +0x10 = 0; +0x18 = current `time(nullptr)` as u64; +0x20..+0x33 = copy of NGM+0x21b4..+0x21c7. Other header
   bytes stay as they are.
7. File path = directory + "/" + `new_name(dir, that same time)`.
8. Open the Writer on it. On failure log `"cinema: cannot write %s, match not recorded"` (path) and stop (the
   header writes of step 6 remain).
9. Write u8 0 to C+0x94, u8 0 to C+0x95, s32 1 to C+0x00. Set recording on, remember the steady clock time.
   Log `"cinema: recording to %s"` (path).

### 4.4 Finishing a recording (E6, E7, E19, and step 2 of 4.3)

Nothing happens when not recording. Otherwise:

1. Recording off.
2. Write 32 zero bits at the stream cursor (`write_bits`), store the cursor.
3. seconds = whole seconds (truncated) of steady clock time since the start. Write u32(seconds) at
   C+0x1c8+0x10.
4. `Writer::finish(stream, C+0x1c8)` gives ok.
5. Guest stream bits = 0 and bytes = 0. Clear the buffer. s32 0 to C+0x00.
6. Log `"cinema: recording %s (%lld s)"` with `"saved"` when ok, else `"failed, removed"`, and seconds.

E6 does only this. E7 does this, then calls the original online Leave::Enter with the state.

### 4.5 Pump (E1)

If the guest stream's data pointer is not this module's buffer, return 0. If recording, `Writer::flush`.
Else, if the Reader is open, `Reader::fill`, then u8 C+0x95 = 1 when the Reader is at eof, else 0. Return 0.

### 4.6 Playback

**Open for reading** (used by E19): attach stream; attach lists; close playback; clear the buffer; guest stream
bits = 0, bytes = 0. If selected is in range, path = directory + "/" + entry name; `Reader::open(path,
C+0x1c8)`:

- Success: u8 1 to C+0x94, u8 0 to C+0x95, s32 1 to C+0x00; `Reader::fill`; log `"cinema: playing %s"`
  (path); result true.
- Failure: log `"cinema: cannot read %s"` (path); continue below. Note that `Reader::open` may already have
  overwritten C+0x1c8 with the bytes it read.

When not in range or the open failed: guest stream bytes = 4, u8 1 to C+0x95, result false.

E8: call the original RePlaying::Exit with the state, then close playback.
E9: close playback, then call the original replay Leave::Enter with the state.

### 4.7 Menus

**E10 PAGE_PROGRESS.** Attach lists. Read the s32 event.

- Event 2, 3 or 4: update flags; done.
- Any event other than 6: done.
- Event 6: take the page request and reset it to empty. If it was not pending: if the net mode (s32 at
  `0x104f1c8`+0x2c4) is not 3, done; else use the request {load first file, next page
  `0x735c4dbe`}. Then: refresh; if "load first file", load(0); pop page; if the next page id is not 0,
  `show_page(next)`; if count is 0, the no-files message.

**E11 PAGE_CINEMA.** Does nothing.

**E12 LIST_CINEMA.** Attach lists. row = the s32 at control+4. Write row to `0x14ce098`. If row is out of
[0, count), done. If row != selected, load(row). `show_page(0xca9509a0)`.

**E13 refresh.** Set the request to {pending, no load, next `0x735c4dbe`}; `show_page(0x1d84d11e)`.

**E14 list toggle.** Set the request to {pending, no load, next 0}; `show_page(0x1d84d11e)`.

**E15 / E16 protect / unprotect** (on = true for E15, false for E16). Attach lists. If selected is out of
range, done. `set_protected(dir, entry name, on)`. On false log `"cinema: cannot change protection of %s"`
(entry name) and stop. Else write entry mode u32 = `0x100` (on) or `0x180` (off), then update flags.

**E17 delete confirmed.** Attach lists. If selected (i) is in range: `remove(dir, entry name)`; on false log
`"cinema: cannot delete %s"` (entry name); on true, if i > 0, selected = i - 1. Then (always): refresh; if
count > 0, load(selected); pop page; if count is 0, the no-files message.

**E18 upload.** Calls `cinema_pick_friend()`.

**E19 start cinema.** Finish recording (4.4). Open for reading (4.6); if false, done (stay on the page).
Else guest calls in order: menu reset `0x5cd1e0(menu_manager)`; task = `0xa572d0(0x9f2aeafb, 0xad76460e)`;
`0xa5be10(task)`.

**E20 list query set.** If the local flag is 0: write the value to s32 `0x14cfeb0`; done. Else if value !=
selected (read from C+0x1a4 without attaching lists first), load(value) (which attaches lists, then ignores an
out-of-range value).

### 4.8 Public functions

**`record_event(event)`**: 4.2 without forwarding. **`recording()`**: the recording flag.

**`read_selected(bytes, name)`**: attach lists. If selected is out of range, return false (name and bytes
untouched). name = entry name. Open directory + "/" + name for binary reading; failure returns false. Find
the size by seeking to the end. The size is acceptable when >= 0x4b0 and < 64 MiB (`0x4000000`). If
acceptable, resize `bytes` to the size and read the whole file; the result is the read being complete. Close.
Return true only when acceptable, read complete, and `valid_header(bytes)`. (When the size is not acceptable,
`bytes` is left unchanged.)

**`add_received(data, size, wanted)`**:

1. Return -1 if data is NULL, size < 0x4b0, or the data's header is not valid.
2. Ensure the store directory; failure returns -1.
3. `make_room(dir)`; false returns 0.
4. The wanted name is "safe" when non-empty, shorter than 64 characters, contains none of `/`, `\`, `:`, and
   does not start with `.`. A safe name whose file already opens for binary reading in the directory is not
   safe.
5. If not safe, or shorter than 9 characters, or not ending in `.u2cinema`, use `new_name(dir,
   time(nullptr))` instead.
6. Create the file (binary, truncate). Failure returns -1. Write all `size` bytes. On a short write, delete
   the file and return -1.
7. Refresh (4.1, writes game memory). Log `"cinema: received file saved as %s (%zu bytes)"` (final name,
   size). Return 1.

### 4.9 File store (cinema_store)

**`get32/put32/get64/put64`**: unaligned little-endian (native) load / store.

**`valid_header(h)`**: u32 at +0 == `0x04b00005`, s32 at +4 in [-1, 0x7fff], s32 at +0x114 in [0, 10].

**`ensure_dir(dir)`**: create the directory and its parents, ignoring errors; return whether `dir` is a
directory.

**Listing (shared by `list` and `make_room`)**: walk `dir` (non-recursive). A walk error ends the walk with
what was found so far; a missing directory gives an empty list. A directory entry counts when it is a regular
file (following links), its extension (`std::filesystem` rules, case-sensitive) is exactly `.u2cinema`, its
size is in [0x4b0, 0xffffffff], its first 0x4b0 bytes can be read, and they pass `valid_header`. Per-entry
errors skip that entry. For each: name = file name, size, header (0x4b0 bytes), time = header u64 at +0x18,
protect = the file's status can be read and has no owner-write permission (a read-only file on Windows).
Files that do not count are never deleted. Sort: larger time first; equal time: larger name first (byte-wise
string comparison).

**`list(dir)`**: the sorted listing, cut to the first 20.

**`fill_entry(f, out)`**: zero 0x6c8 bytes. If the time converts to local time, write it at +0 with
`strftime` format `"%a, %d %b %Y %H:%M:%S"`, at most 0x100 bytes. Write the name at +0x100 with at most 0x100
bytes including the terminator (truncated). +0x200 = 0. +0x208 = time (u64). +0x210 = size (u32). +0x214 =
`0x100` if protect, else `0x180`. If the header has exactly 0x4b0 bytes, copy it to +0x218.

**`make_room(dir)`**: take the full sorted listing (not cut to 20). While it has 20 or more files: pick the
last unprotected file in sort order (the oldest); if there is none, return false; delete it; if the delete
does not report a removed file, return false; drop it from the listing. Return true. So after success at most
19 valid files remain.

**`remove(dir, name)`**: false if name is empty or the file is protected (as defined above). Else delete it
and return whether a file was removed.

**`set_protected(dir, name, on)`**: remove (on) or add (off) the owner, group and others write permissions
of the file. Return true when that raised no error (a missing file gives false).

**`new_name(dir, t)`**: stamp = local time of t as `"%Y%m%d-%H%M%S"`, or `"recording"` when the local time
conversion fails. Candidate = stamp + `.u2cinema`. While the candidate exists in dir, try stamp + `-2`,
`-3`, ... `-99` + `.u2cinema` in that order. The search stops at `-99`, which is returned even if it exists.
An error while checking existence counts as "does not exist".

**Writer::open(path)**: abandon any open file first. Create path (binary, truncate); failure returns false.
Remember the path, clear the failed flag, write 0x4b0 zero bytes. A short write sets failed and returns false,
but the file stays open (it is deleted by the next open/abandon or by the destructor).

**Writer::flush(stream)**: while bits + 7 >= 0x40008 (u32): if a file is open, write the first 0x8000 bytes
of data (a short write sets failed); move data bytes [0x8000, 0x20000) to the start; zero bytes [0x18000,
0x20000); bits -= 0x40000. (Shifting happens even without an open file.)

**Writer::finish(stream, header)**: false if no file is open. Flush. Write ceil(bits / 32) u32 words from data
(computed without 32-bit overflow). Seek to offset 0 and write the 0x4b0-byte header. Close. Any short write,
failed seek or failed close sets failed. If failed, delete the file. Return not failed.

**Writer::abandon()**: if a file is open, close it and delete it. The destructor abandons.

**Reader::open(path, header_out)**: close first. Open for binary reading; failure returns false and leaves
header_out untouched. Clear eof. Read 0x4b0 bytes directly into header_out. If fewer came or the header is not
valid, close and return false. Else true.

**Reader::fill(stream)**: first, while bits + 7 >= 0x20008 (u32) and bytes >= 0x4000: move data bytes
[0x4000, bytes) to the start, bytes -= 0x4000, bits -= 0x20000, zero data from offset `bytes` up to 0x20000.
Then, while a file is open, not at eof, and bytes <= 0x10000: read up to 0x10000 bytes at data+bytes, add the
count read to bytes; a read shorter than 0x10000 sets eof. (A file whose stream length is a multiple of
0x10000 reaches eof only after a zero-length read.)

**Reader::close()**: close the file if open, clear eof. The destructor closes.

### 4.10 File format

`<store dir>/<name>.u2cinema`: 0x4b0-byte header (the bytes of C+0x1c8 at finish time), then the stream as
u32 words in host (little-endian) order, whole words: the game's own stream start (written by WaitReady::Exit),
then records {u32 time, u8 type, event payload} packed MSB-first in each word, then u32 0.

## 5. Install (`install_cinema(installed)`)

`record_enabled` = false when the environment variable `UNCHARTED_U2MP_CINEMA` is set and its first
character is `0`; true otherwise (also when set to an empty string).

Patching mechanisms (all logs below are format strings passed to `log`):

- **Checked replace** (pump and menu functions): if the bytes at the address differ from the table in 2.3, log
  `"cinema: %s at 0x%llx differs, not replaced"` (name, address) and fail. Else `replace_function(address,
  length, host function, name)` (it checks the executable range, takes one 16-byte `near_stub`, writes
  `e9 rel32` then 0xcc up to the length, and logs itself).
- **Ret-stub replace**: if `is_ret_stub(address)` is false, log `"cinema: %s at 0x%llx is not a ret stub, not
  replaced"` and fail. Else `replace_function(address, 16, host, name)`.
- **Detour** (17 bytes): fail with log `"cinema: %s at 0x%llx differs, not hooked"` when the 17 bytes are not
  all in an executable segment or differ. Else take `near_stub(31)` for the trampoline, then `near_stub(12)`
  for the jump stub (in that order); if either is NULL fail silently. Trampoline = the 17 original bytes, then
  `ff 25 00 00 00 00` and the 8-byte absolute address of image+address+17. Jump stub = `48 b8`, the 8-byte host
  function address, `ff e0`. If the jump stub is out of rel32 range from image+address+5, fail silently (the
  two allocations are spent). Else remember the trampoline as the original, write `e9` + rel32 to the jump stub
  at the address, fill bytes 5..16 with 0xcc, log `"cinema: %s at 0x%llx hooked"`.
- **Slot swap**: fail with log `"cinema: %s slot 0x%llx differs, not hooked"` (name, slot) when slot + 8 >
  image size or the u64 at the slot != image.base + expected. Else remember the old value as the original,
  write the plain host address of the hook function into the slot, log `"cinema: %s (slot 0x%llx) -> C++"`.
- **Query patch**: fail with log `"cinema: list query at 0x%llx differs, not patched"` (`0x657e7a`) when the 20
  bytes are not executable or differ. Take `near_stub(32)`; NULL fails silently. Stub = `44 89 c7` (mov
  edi, r8d), `0f b6 f1` (movzx esi, cl), `48 b8` + 8-byte address of the E20 host function, `ff d0` (call
  rax), `e9` + rel32 to image+`0x657bea` (relative to stub+23). Out of rel32 range fails silently (at either
  jump). Else write `e9` + rel32 to the stub at `0x657e7a`, fill bytes 5..19 with 0xcc, log
  `"cinema: list query set -> C++"`.

Each mechanism that succeeds adds 1 to `installed`. Order and names:

1. Checked replace `0x608c40`, name `"cinema pump"`. On failure log `"cinema: not installed (the stream pump
   differs)"` and return (nothing else is installed, final log not printed).
2. Detour `0x644ab0`, name `"SendNetEvent"` (send_ok).
3. Detour `0x66a3d0`, name `"NetSession::SendNetEvent"`.
4. Ret-stub replace `0x5e9d80`, name `"cinema receive hook"` (recv_ok).
5. Slot swap `0x100abd0` expecting `0x5f2830`, name `"WaitReady::Exit"` (start_ok).
6. Ret-stub replace `0x5f0ea0`, name `"ShowResults::Enter"`.
7. Slot swap `0x100a9c8` expecting `0x5f00d0`, name `"online Leave::Enter"`.
8. `can_record` = send_ok and recv_ok and start_ok. If false log `"cinema: recording off (a recorder hook is
   missing)"`. If start_ok is false (also) log `"cinema: WARNING the replay stream has no buffer;
   WaitReady::Exit writes to NULL"`.
9. Slot swap `0x100a7a0` expecting `0x5e8740`, name `"RePlaying::Exit"`.
10. Slot swap `0x100a6d8` expecting `0x5e5520`, name `"replay Leave::Enter"`.
11. Checked replace `0x5e3f60`, `"PAGE_PROGRESS"`.
12. Checked replace `0x5e4060` (18 bytes), `"PAGE_CINEMA"`.
13. Checked replace `0x5e4080`, `"LIST_CINEMA"`.
14. Checked replace `0x5e4490`, `"cinema refresh"`.
15. Checked replace `0x5e45d0`, `"cinema list toggle"`.
16. Ret-stub replace `0x5e41d0`, `"BUTTON_CINEMA_PROTECT"`.
17. Ret-stub replace `0x5e41e0`, `"BUTTON_CINEMA_UNPROTECT"`.
18. Checked replace `0x5e41f0`, `"cinema delete"`.
19. Checked replace `0x5e42d0`, `"BUTTON_CINEMA_UPLOAD"`.
20. Checked replace `0x5e45a0`, `"BUTTON_MULTIPLAYER_STARTCINEMA"`.
21. Query patch.
22. Log `"cinema: recording %s; files in <user dir>/u2mp/cinema"` with `"on"` when can_record and
    record_enabled, else `"off"`.

A failed step never stops the later ones (except step 1). The `near_stub` allocation sequence on a full
install is: 16 (pump, inside replace_function), 31 and 12 (detour 1), 31 and 12 (detour 2), 16, 16, then 16
for each of steps 11-20, then 32 (query).

## 6. Test scenarios

Install:

1. Fake image with every expected byte, ret stub and slot value: all 20 mechanisms succeed, `installed` +20,
   final log "on". Repeat with `UNCHARTED_U2MP_CINEMA=0` (final log "off") and `=""` (on).
2. Pump bytes differ: only the pump "differs" line and "not installed", `installed` unchanged.
3. Each other target broken on its own (wrong bytes, not a ret stub, wrong slot value, slot past image end,
   query bytes differ): its failure line, the rest installed. For send / receive / WaitReady also the
   "recording off" line; for WaitReady also the WARNING line. Session send broken alone: no "recording off".
4. Detour where the bytes match but the range is not executable: "differs, not hooked".

Recording (after install, with stubs for the event Write and the original functions):

5. E5 with the stream data NULL: buffer attached, original called, file created, header fields written,
   "recording to" line, C+0x00 = 1.
6. E5 with record disabled or can_record false: original called, no file, no log.
7. E5 with the directory not creatable / 20 protected files / file not creatable: the matching log line.
8. E5 while already recording: the old recording is finished ("saved") before the new one starts.
9. E2/E3/E4 while recording: 32-bit clock (clock pointer NULL -> 1; counter 0 -> 1; counter n -> n), 8-bit
   type, Write called with (event, stream), then for E2/E3 the original with the same three arguments and its
   return value passed back. Not recording, NULL event, foreign data pointer, or cursor past 0x1fc00 bytes: only
   the original is called.
10. E1 while recording with the cursor below, at and past 0x40001 bits (one and several 32 KB flushes); with a
    foreign data pointer (nothing).
11. E6, E7, E19 finishing: end marker, duration, file content (header at 0, whole words), "saved" line; a
    failing write gives "failed, removed" and no file. E7 calls the original after finishing.

Playback and menus:

12. E19 with a valid selected file: finish, open, fill, "playing" line, then menu reset, find task, play task
    in that order. Selected out of range: no calls, bytes = 4, eof = 1. File missing or invalid header:
    "cannot read" line, C+0x1c8 holds what was read.
13. E1 during playback: discard at bits >= 0x20001 with bytes >= 0x4000, reads of 64 KB, eof flag at C+0x95;
    files of length 0, below 64 KB, exactly 64 KB and over 128 KB of stream.
14. E8 and E9 with and without an open Reader (order of the original call vs the close).
15. E10 with events 2, 3, 4 (flags only), 5 / 7 (nothing), 6 with no request and mode != 3 (nothing), 6 with no
    request and mode 3 (refresh, load(0) SetNetGame if files exist, pop, ShowPage list, message if empty, with
    the message box pointer NULL and non-NULL), 6 after E13 (no load, list page), 6 after E14 (no ShowPage).
16. Refresh with 0, 1, 20 and 25 files, junk files, wrong extension, a protected file; selected beforehand
    -1, in range, past the end.
17. E12 with row -1, row >= count, row == selected (no load), row != selected (load).
18. E15 / E16 on a writable and a protected file, out of range, and when changing permissions fails.
19. E17 at index 0, at index > 0, on a protected file (log, selection kept), with the last file deleted (no
    load, message).
20. E20 with local 0 (row write), local 1 with value == selected (nothing), != selected in range (load), out of
    range (lists attached only).
21. E11, E18 (`cinema_pick_friend` called once).

Public functions and store:

22. `read_selected`: out of range, missing file, file shorter than 0x4b0, invalid header, valid file.
23. `add_received`: NULL data, short data, bad header, all 20 protected (0), wanted names: valid and free,
    taken, empty, 64+ characters, with `/`, `\`, `:`, leading `.`, wrong extension, exactly `.u2cinema`;
    resulting file name, content, refresh effects, log line.
24. `new_name` with 0, 1 and 98+ existing collisions; `make_room` with 19, 20, 21 files and with protected
    oldest files; `list` sort order with equal times.
25. The existing unit test `u2mp-cinema-test` passes.
