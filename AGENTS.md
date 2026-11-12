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
  Only the current build folder is kept in the repository; older builds live in git history.
- Scripts that only glue tools together are shell (`bash`) for CI and packaging, or PowerShell on Windows.
  They stay short; anything with real logic goes into C++.
- If you think a task really needs Python, stop and ask the owner. Do not write it "for now".

## Where C++ goes

- Launcher: `launcher/native/` (the core, pages, updates, game setup). The window is `launcher-qt/` (Qt/QML).
- Game runtime and tests: `gow3_pc/` (build with ninja; see its README).
- New import or recipe steps: a C++ tool built with the launcher, run by the recipe like any other step.

## Other rules

- No game files, no server addresses, host names or credentials, anywhere in the repository or in commit messages.
- Keep the launcher's UI exactly as it is unless the owner asks for a change.
