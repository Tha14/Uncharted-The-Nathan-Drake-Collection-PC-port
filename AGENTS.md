# Rules for coding agents (Claude, Codex, Copilot, Cursor and others)

These rules apply to every change in this repository. They are the owner's decision and are not up for discussion.

## Language: C++ only. No Python.

- **Write all new code in C++** (C++20, built with clang through the existing `build.sh` / CMake setups).
  Plain C is fine only where the surrounding code is already C (the game runtime, small native helpers).
- **Never add a Python file** (`.py`, `.pyw`, `.pyi`, notebooks). Not for a feature, not for a fix, not for a
  "quick" tool, a test, a build step, a one-off script or a CI step.
- **Never extend existing Python.** Python that is still here is being replaced. When a change touches a Python tool,
  port that tool (or the part you need) to C++ first, switch the callers to the C++ version, and then remove the
  Python file in its own commit.
- **Never copy Python files** into new folders (for example a new helper build folder under `u3mp/patcher/build/`).
- Scripts that only glue tools together are shell (`bash`) for CI and packaging, or PowerShell on Windows.
  They stay short; anything with real logic goes into C++.
- If you think a task really needs Python, stop and ask the owner. Do not write it "for now".

## Keep it simple, but legal and exact

- **Prefer the simplest implementation that does the job**, with fewer moving parts, fewer special cases and no
  layers, options or abstractions nobody needs yet. Before adding code, check whether existing code already does it.
- **Always legal:** code that comes from the game (PS3 or PS4) is reimplemented from a description of what it does,
  never rewritten 1:1 from the original code, so that no copyrighted code is reproduced. Do not copy constant tables
  or values from the game into the source.
- **Simple only when it is exact:** a simpler version is used only if it replicates 100% of what the original does,
  in every case. If the simpler way would change or break any behaviour, keep the full behaviour and do not simplify.
  Check this with a test that runs the old and the new code on the same inputs.

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

## Other rules

- No game files, no server addresses, host names or credentials, anywhere in the repository or in commit messages.
- Keep the launcher's UI exactly as it is unless the owner asks for a change.
