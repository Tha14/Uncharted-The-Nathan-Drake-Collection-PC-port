# Rules for coding agents (Claude, Codex, Copilot, Cursor and others)

These rules apply to every change in this repository. They are the owner's decision and are not up for discussion.

## Language: C++ above everything else

**C++ is the language of this project. Everything is written in C++.** Another language is used only where C++ is
really impossible or clearly not practical, and that case is rare. "It is quicker", "the file is already C" or "it is
just a small script" are not reasons.

- **Write all new code in C++** (C++20, built with clang through the existing `build.sh` / CMake setups). This includes
  tools, tests, build and import steps, one-off helpers and fixes.
- **No new C.** Existing C (for example the Uncharted 3 helper's `u3_lan_profiles.c` and its `.inc` files) is being
  ported to C++: when you change a C function, move it to C++ instead of growing the C. Do not add C files or C
  functions.
- **No Python, ever.** Never add a Python file (`.py`, `.pyw`, `.pyi`, notebooks): not for a feature, a fix, a "quick"
  tool, a test, a build step, a one-off script or a CI step. Never extend existing Python: port the tool (or the part
  you need) to C++, switch the callers, and remove the Python file in its own commit. Never copy Python files into new
  folders (for example a new helper build folder under `u3mp/patcher/build/`).
- **The only accepted exceptions,** because C++ cannot do the job or it would not be practical:
  - third-party libraries kept as they are (for example BearSSL, `u3_bearssl.inc`);
  - a few lines of assembly inside C++ where the game's calling convention or an instruction needs it (calls that
    pass floats in x87 / xmm registers, `int 0x41`);
  - QML for the launcher's Qt window layout (`launcher-qt/`), with all logic in C++;
  - short shell (`bash`) for CI and packaging, PowerShell on Windows, CMake for builds: glue only, anything with real
    logic goes into C++.
- **If you think something really needs another language, stop and ask the owner** before writing it. Do not write it
  "for now".

## Keep it simple, but legal and exact

- **Prefer the simplest implementation that does the job**, with fewer moving parts, fewer special cases and no
  layers, options or abstractions nobody needs yet. Before adding code, check whether existing code already does it.
- **Always legal:** code that comes from the game (PS3 or PS4) is reimplemented from a description of what it does,
  never rewritten 1:1 from the original code, so that no copyrighted code is reproduced. Do not copy constant tables
  or values from the game into the source.
- **Simple only when it is exact:** a simpler version is used only if it replicates 100% of what the original does,
  in every case. If the simpler way would change or break any behaviour, keep the full behaviour and do not simplify.
  Check this with a test that runs the old and the new code on the same inputs.

## Assets: PS4 first, PS3 only when PS4 has none

- **Port an asset from the PS3 version only when the PS4 version has no such asset.** If PS4 already has it
  anywhere (multiplayer or campaign), the import uses the PS4 asset: it usually has better textures, and not
  bringing over a second copy keeps the import smaller.
- Example: baseball-shirt Drake (red shirt Drake) is in PS4 as a campaign skin, so it is not ported from PS3; the
  import uses the PS4 asset.
- Before adding a PS3 asset to an import step, check PS4's data (campaign and multiplayer) for the same asset.

## Versions: keep ONLY the latest

- **The repository holds only the latest version of anything versioned.** Never keep an older version next to a new
  one: no `v7.109/` beside `v7.110/`, no `tool_v2.cpp` beside `tool.cpp`, no `old/`, `backup/` or `-copy` folders.
  Older versions live in git history only.
- **A new helper build replaces the previous one in the same commit:** `git mv u3mp/patcher/build/v<old>
  u3mp/patcher/build/v<new>`, then update its files. `u3mp/patcher/build/` always contains exactly one folder.
  `tools/package_windows.sh` stops when more than one is committed.
- **Never copy files to make a new version.** Change the existing files. Shared code lives in one place and is used
  from there, not duplicated per version.

## Where C++ goes

- Launcher: `launcher/native/` (the core, pages, updates, game setup). The window is `launcher-qt/` (Qt/QML).
- Game runtime and tests: `gow3_pc/` (build with ninja; see its README).
- New import or recipe steps: a C++ tool built with the launcher, run by the recipe like any other step.

## Helper source: several people at once

The Uncharted 3 helper's source is `u3mp/helper-source/` (see CONTRIBUTING.md, section 7).

In short: the contributor syncs their fork, edits only `u3mp/helper-source/`, and opens a pull request; the
maintainer merges it, builds and ships the next helper version. Nobody else changes a version number or builds. Each
person writes only in their own marked spots and ports only the rows assigned to them, so changes never collide.
When you work for a contributor:

- **Never make a helper version.** No new or renamed `u3mp/patcher/build/v*` folder, no `build.json` change, no
  version number. Only the maintainer ships helper versions, once per batch, after merging the helper PRs.
- **Never edit or commit the generated block** at the end of `u3_lan_profiles.c` (from the line
  `u3x (C++): generated from u3x_fixes.cpp` to the end), `tests/u3x_fixes_for_test.inc` or any built file.
- **Write only between the contributor markers:** C++ in `u3x_fixes.cpp` above `===== end of contributor section
  =====`; `u3x_` prototypes and `U3H` exports in their contributor blocks in `u3_lan_profiles.c`. Outside them, only
  turn the C function you ported into a one-line wrapper that calls your `u3x_` function. Never change the three
  lines after an end marker. The maintainer's agent appends only at the very end of `u3x_fixes.cpp`.
- **Port only the rows assigned to your contributor**, and add an old-versus-new test as a new
  `tests/rewrite_test_bNN.inc` for each one.

## Other rules

- No game files, no server addresses, host names or credentials, anywhere in the repository or in commit messages.
- Keep the launcher's UI exactly as it is unless the owner asks for a change.
