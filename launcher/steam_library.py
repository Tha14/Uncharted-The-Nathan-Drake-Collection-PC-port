"""Add to Steam (launcher: Advanced -> STEAM): the Python side of uncharted_native.dll's Steam functions
(launcher/native/steam.cpp, where the work is done: Steam's folders and accounts, the shortcut file, the library
pictures from the player's own game files).

The launcher calls steam_running(), launch_targets() and add_to_steam() and shows the questions and messages.
"""
import ctypes
from pathlib import Path

COLLECTION_NAME = "Uncharted: The Nathan Drake Collection"
NATIVE = Path(__file__).resolve().parent / "native" / "uncharted_native.dll"
_ERRORS = {1: "no_steam", 2: "bad_file", 3: "os"}


class SteamError(Exception):
    """kind: 'no_steam' (no Steam account found) or 'bad_file' (a shortcuts.vdf it cannot read)."""

    def __init__(self, kind, detail=""):
        super().__init__(f"{kind}: {detail}" if detail else kind)
        self.kind, self.detail = kind, detail


_dll = None


def _native():
    global _dll
    if _dll is None:
        _dll = ctypes.CDLL(str(NATIVE))
        for name in ("un_steam_targets", "un_steam_add"):
            getattr(_dll, name).argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
            getattr(_dll, name).restype = ctypes.c_int
        _dll.un_steam_running.restype = ctypes.c_int
    return _dll


def _call(function, lines):
    request = "".join("\t".join(str(part) for part in line) + "\n" for line in lines).encode("utf-8")
    size = 1 << 16
    while True:
        out = ctypes.create_string_buffer(size)
        status = function(request, out, size)
        if status >= 0:
            return status, out.value.decode("utf-8", "replace")
        size = -status


def _records(text):
    return [line.split("\t") for line in text.splitlines() if line]


def steam_running():
    """Whether Steam is running (its list of games would overwrite a change on exit)."""
    try:
        return bool(_native().un_steam_running())
    except OSError:
        return False


def launch_targets(root, starter, python_exe, script, games):
    """[(name, exe, start folder, options)] for each Steam entry: games is None for one Collection entry, or a list of
    (key, name) for one entry per game."""
    lines = [("root", root), ("starter", starter), ("python", python_exe), ("script", script)]
    if games is not None:
        lines.append(("per_game",))
        lines += [("game", key, name) for key, name in games]
    _, text = _call(_native().un_steam_targets, lines)
    return [tuple(record) for record in _records(text)]


def add_to_steam(targets, icon, game_dir, accounts=None, all_names=()):
    """Adds or updates the entries in every Steam account (accounts: [(folder, name)] to use instead).
    Returns [(folder, name, result)]; raises SteamError."""
    lines = [("target",) + tuple(target) for target in targets]
    lines += [("name", name) for name in all_names]
    lines.append(("icon", icon))
    if game_dir:
        lines.append(("game_dir", game_dir))
    for folder, name in accounts or ():
        lines.append(("account", folder, name))
    status, text = _call(_native().un_steam_add, lines)
    if status:
        kind = _ERRORS.get(status, "os")
        if kind == "os":
            raise OSError(text)
        raise SteamError(kind, text)
    results = []
    for _, folder, name, file, backup, added, updated, removed, pictures, ids in _records(text):
        results.append((Path(folder), name, {
            "file": Path(file), "backup": Path(backup) if backup else None,
            "ids": [int(i) for i in ids.split(",") if i], "added": int(added), "updated": int(updated),
            "removed": int(removed), "pictures": int(pictures)}))
    return results
