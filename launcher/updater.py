"""The launcher's self-update, as the Python launcher reaches it: every step runs in uncharted_native.dll
(launcher/native/updater.cpp), this module only hands values across.

Steps, in the order the launcher takes them:
  latest_release(current)      the newest GitHub release above `current` that carries Uncharted-windows.zip with a
                               published SHA-256 (a dict: version, tag, page, notes, url, size, sha256), or None
  download(release, root, cb)  that zip into <root>\\update, resumed after a broken connection, size and SHA-256
                               checked; cb(done, total) reports progress while it runs
  stage(zip, root, version)    unpacked into <root>\\update\\staged and checked; returns that folder
  start_apply(root, staged)    starts the new version's installer (tools\\apply_update.exe), which waits for the
                               launcher to exit, swaps the program folders and restarts the launcher
  cleanup(root)                removes what an update left in <root>\\update
installed_version(root) reads <root>\\VERSION (None in a source checkout, which never updates itself).
"""
import ctypes
import json
import threading
from pathlib import Path

_LIBRARY = Path(__file__).resolve().parent / "native" / "uncharted_native.dll"
_lib = None


class UpdateError(Exception):
    """Why an update step failed, in words for the player."""


def _library():
    global _lib
    if _lib is None:
        lib = ctypes.CDLL(str(_LIBRARY))
        text_out = [ctypes.c_char_p, ctypes.c_int]
        lib.un_update_installed.argtypes = [ctypes.c_wchar_p] + text_out
        lib.un_update_latest.argtypes = [ctypes.c_char_p, ctypes.c_int] + text_out
        lib.un_update_download.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p] + text_out
        lib.un_update_stage.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_char_p] + text_out
        lib.un_update_start_apply.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p] + text_out
        lib.un_update_progress.argtypes = [ctypes.POINTER(ctypes.c_longlong)] * 2
        lib.un_update_progress.restype = None
        lib.un_update_cleanup.argtypes = [ctypes.c_wchar_p]
        lib.un_update_cleanup.restype = None
        _lib = lib
    return _lib


def _ask(name, *args, room=1 << 16, repeatable=False):
    """Runs a library function that writes text; (status, text). Only functions without side effects
    (repeatable) are run again when the text needs more room."""
    function = getattr(_library(), name)
    while True:
        buffer = ctypes.create_string_buffer(room)
        status = function(*args, buffer, room)
        if status >= 0 or not repeatable:
            return status, buffer.value.decode("utf-8", "replace")
        room = -status


def version_text(version):
    return ".".join(map(str, version))


def installed_version(root):
    status, text = _ask("un_update_installed", str(root), repeatable=True)
    return tuple(int(n) for n in text.split(".")) if status == 0 else None


def latest_release(current, timeout=15):
    status, text = _ask("un_update_latest", version_text(current).encode(), int(timeout), room=1 << 22,
                        repeatable=True)
    if status:
        raise UpdateError(text)
    found = json.loads(text)
    if found:
        found["version"] = tuple(found["version"])
    return found


def download(release, root, progress=None):
    answer = []
    request = json.dumps(dict(release, version=list(release["version"]))).encode("utf-8")
    worker = threading.Thread(target=lambda: answer.append(_ask("un_update_download", request, str(root))),
                              daemon=True)
    worker.start()
    done, total = ctypes.c_longlong(), ctypes.c_longlong()
    while worker.is_alive():
        worker.join(0.1)
        if progress:
            _library().un_update_progress(ctypes.byref(done), ctypes.byref(total))
            progress(done.value, total.value)
    status, text = answer[0]
    if status:
        raise UpdateError(text)
    return Path(text)


def stage(archive, root, version):
    status, text = _ask("un_update_stage", str(archive), str(root), version_text(version).encode())
    if status:
        raise UpdateError(text)
    return Path(text)


def start_apply(root, staged):
    status, text = _ask("un_update_start_apply", str(root), str(staged))
    if status:
        raise OSError(text)


def cleanup(root):
    _library().un_update_cleanup(str(root))


if __name__ == "__main__":   # what this checkout would be offered
    here = Path(__file__).resolve().parent.parent
    mine = installed_version(here)
    print("installed:", version_text(mine) if mine else "source tree")
    offer = latest_release(mine or (0, 0, 0))
    print("newer release:", offer and {key: value for key, value in offer.items() if key != "notes"})
