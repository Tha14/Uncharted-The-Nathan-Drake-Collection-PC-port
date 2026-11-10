"""The launcher's Add to Steam (Advanced page), as the Python launcher reaches it: the work (Steam's install and
accounts, shortcuts.vdf, the library pictures made from the player's game) is in uncharted_native.dll
(launcher/native/steam.cpp); this module turns the launcher's values into the library's tab-separated requests and
its answers back.

  steam_running()                    True while Steam runs (it rewrites shortcuts.vdf when it exits)
  launch_targets(root, starter, python_exe, script, games)
                                     the entries to add: (name, program, start folder, options) each; games None
                                     for one Collection entry, else [(key, name)] for one entry per game
  add_to_steam(targets, icon, game_dir, accounts, all_names)
                                     writes them into every Steam account (or `accounts`: [(folder, name)]);
                                     [(folder, account name, result dict)], SteamError or OSError
"""
import ctypes
from pathlib import Path

COLLECTION_NAME = "Uncharted: The Nathan Drake Collection"
_LIBRARY = Path(__file__).resolve().parent / "native" / "uncharted_native.dll"
_FAILURES = {1: "no_steam", 2: "bad_file"}   # anything else: an OSError
_lib = None


class SteamError(Exception):
    """A Steam problem the launcher explains: kind "no_steam" (no account to add to) or "bad_file" (a shortcuts.vdf
    it can't read); detail is the library's text."""

    def __init__(self, kind, detail=""):
        self.kind, self.detail = kind, detail
        super().__init__(kind + (f": {detail}" if detail else ""))


def _library():
    global _lib
    if _lib is None:
        lib = ctypes.CDLL(str(_LIBRARY))
        lib.un_steam_running.restype = ctypes.c_int
        for name in ("un_steam_targets", "un_steam_add"):
            function = getattr(lib, name)
            function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
            function.restype = ctypes.c_int
        _lib = lib
    return _lib


def _request(name, rows):
    """Sends rows (tuples) as tab-separated lines; (status, answer text, answer rows)."""
    text = "".join("\t".join(map(str, row)) + "\n" for row in rows).encode("utf-8")
    room = 1 << 16
    while True:
        buffer = ctypes.create_string_buffer(room)
        status = getattr(_library(), name)(text, buffer, room)
        if status >= 0:
            answer = buffer.value.decode("utf-8", "replace")
            return status, answer, [line.split("\t") for line in answer.splitlines() if line]
        room = -status


def steam_running():
    try:
        return bool(_library().un_steam_running())
    except OSError:
        return False


def launch_targets(root, starter, python_exe, script, games):
    rows = [("root", root), ("starter", starter), ("python", python_exe), ("script", script)]
    if games is not None:
        rows.append(("per_game",))
        rows.extend(("game", key, name) for key, name in games)
    return [tuple(row) for row in _request("un_steam_targets", rows)[2]]


def add_to_steam(targets, icon, game_dir, accounts=None, all_names=()):
    rows = [("target", *target) for target in targets]
    rows += [("name", name) for name in all_names]
    rows.append(("icon", icon))
    if game_dir:
        rows.append(("game_dir", game_dir))
    rows += [("account", folder, name) for folder, name in accounts or ()]
    status, answer, records = _request("un_steam_add", rows)
    if status:
        if status in _FAILURES:
            raise SteamError(_FAILURES[status], answer)
        raise OSError(answer)
    added = []
    for _, folder, name, file, backup, new, changed, removed, pictures, ids in records:
        added.append((Path(folder), name, {
            "file": Path(file), "backup": Path(backup) if backup else None,
            "ids": [int(n) for n in ids.split(",") if n], "added": int(new), "updated": int(changed),
            "removed": int(removed), "pictures": int(pictures)}))
    return added
