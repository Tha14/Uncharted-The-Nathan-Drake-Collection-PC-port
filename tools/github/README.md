# Uncharted: The Nathan Drake Collection — PC port

A port of the PS4 collection (CUSA02344: Uncharted 1, 2 and 3) to Windows, built on the gow3_pc
runtime (shadPS4 video core). **No game files are in this repository**: players use their own copy.

## Layout

- `gow3_pc/` the runtime (C runtime, GPU library, scripts). Submodules under `gow3_pc/gpu/third_party/`
  and `gow3_pc/third_party/`.
- `launcher/` the Uncharted launcher (Tkinter): game cards, presets, per-game settings.
- `tools/probe_uncharted.py` prepares a game and starts the runtime; `tools/package_windows.sh` builds
  the Windows package; `tools/export_github.sh` syncs the working tree into this repository.
- `analysis/STATUS.md` compatibility notes.

## Building

GitHub Actions (`.github/workflows/build-windows.yml`) builds every push to `main`. The artifact
`Uncharted-windows` is the ready-to-run folder: extract it and start `Uncharted Launcher.exe`.

Locally, from an MSYS2 CLANG64 shell (packages: `gow3_pc/packaging/windows/README.md`):

```bash
git clone --recursive <this repository>
gow3_pc/build.sh
tools/package_windows.sh   # needs a Windows Python 3.10+ with Pillow (PYTHON_HOME)
```
