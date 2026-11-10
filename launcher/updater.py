"""Launcher updates from the project's GitHub releases
(fidalgojoao/Uncharted-The-Nathan-Drake-Collection-PC-port: BilalReloaded's port with the Uncharted 3
multiplayer; the upstream port's own releases are bc160201983/...).

The package's VERSION file (written by tools/package_windows.sh) names the installed version; a
copy run from the source tree has none and is never updated. An update:
  1. latest_release(): the newest release with a newer version and an Uncharted-windows.zip
     asset whose SHA-256 GitHub publishes (the asset's "digest"),
  2. download(): the zip into update\\, checked against that size and SHA-256,
  3. stage(): unpacked to update\\staged and checked (the files the package needs, its VERSION),
  4. start_apply(): the new version's own Python runs its tools\\apply_update.py, which waits for
     this launcher to close, swaps the program folders (rolling back on any error) and starts
     the new launcher. Saves, settings, logs and the shader cache (analysis\\) are not touched.
UNCHARTED_UPDATE_API points the check at another releases list (tests; http only on 127.0.0.1).

The work is done by the launcher's native code (launcher/native/updater.cpp).
"""
import ctypes
import json
import sys
import threading
from pathlib import Path

REPOSITORY = "fidalgojoao/Uncharted-The-Nathan-Drake-Collection-PC-port"
RELEASES_PAGE = f"https://github.com/{REPOSITORY}/releases"
ASSET = "Uncharted-windows.zip"
NATIVE = Path(__file__).resolve().parent / "native" / "uncharted_native.dll"
_dll = None


class UpdateError(Exception):
    pass


def _native():
    global _dll
    if _dll is None:
        _dll = ctypes.CDLL(str(NATIVE))
        _dll.un_update_installed.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_update_latest.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        _dll.un_update_download.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_update_progress.argtypes = [ctypes.POINTER(ctypes.c_longlong), ctypes.POINTER(ctypes.c_longlong)]
        _dll.un_update_progress.restype = None
        _dll.un_update_stage.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_update_start_apply.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_update_cleanup.argtypes = [ctypes.c_wchar_p]
        _dll.un_update_cleanup.restype = None
        for name in ("un_update_installed", "un_update_latest", "un_update_download", "un_update_stage",
                     "un_update_start_apply"):
            getattr(_dll, name).restype = ctypes.c_int
    return _dll


def _call(function, *args, size=1 << 16, again=False):
    """(status, text) of a native call with room for its answer. Calls that download or write run once; the ones
    that only answer (again=True) are asked again with more room when they need it."""
    out = ctypes.create_string_buffer(size)
    status = function(*args, out, size)
    while status < 0 and again:
        size = -status
        out = ctypes.create_string_buffer(size)
        status = function(*args, out, size)
    return status, out.value.decode("utf-8", "replace")


def _version(text):
    return tuple(int(part) for part in text.split("."))


def version_text(version):
    return ".".join(str(part) for part in version)


def installed_version(root):
    """The package's version, or None for a copy run from the source tree."""
    status, text = _call(_native().un_update_installed, str(root), again=True)
    return _version(text) if status == 0 else None


def latest_release(current, timeout=15):
    """The newest release newer than `current` with an installable package, as a dict (version,
    tag, page, notes, url, size, sha256), or None. Pre-releases count: every release so far is a
    beta. UpdateError when the releases could not be read."""
    status, text = _call(_native().un_update_latest, version_text(current).encode(), int(timeout), size=1 << 22,
                         again=True)
    if status != 0:
        raise UpdateError(text)
    release = json.loads(text)
    if release:
        release["version"] = tuple(release["version"])
    return release


def update_dir(root):
    return Path(root) / "update"


def download(release, root, progress=None, cancelled=None):
    """Downloads the release's zip into update\\ and checks its size and SHA-256. A connection that
    stalls or breaks (slow or busy networks: "The read operation timed out", #149/#150) is opened
    again and the download goes on where it stopped (HTTP Range; GitHub's file host supports it).
    progress(done, total) is called while it runs. `cancelled` is not used (the launcher never cancels)."""
    request = dict(release, version=list(release["version"]))
    result = {}

    def work():
        result["answer"] = _call(_native().un_update_download, json.dumps(request).encode("utf-8"), str(root))
    worker = threading.Thread(target=work, daemon=True)
    worker.start()
    done, total = ctypes.c_longlong(0), ctypes.c_longlong(0)
    while worker.is_alive():
        worker.join(0.1)
        if progress:
            _native().un_update_progress(ctypes.byref(done), ctypes.byref(total))
            progress(done.value, total.value)
    status, text = result["answer"]
    if status != 0:
        raise UpdateError(text)
    return Path(text)


def stage(archive, root, version):
    """Unpacks the package to update\\staged and checks it; returns that folder."""
    status, text = _call(_native().un_update_stage, str(archive), str(root), version_text(version).encode())
    if status != 0:
        raise UpdateError(text)
    return Path(text)


def start_apply(root, staged):
    """Starts the new version's installer, which waits for this process to exit."""
    status, text = _call(_native().un_update_start_apply, str(root), str(staged))
    if status != 0:
        raise OSError(text)


def cleanup(root):
    """Removes what an applied or abandoned update left (best effort: files may still be in use)."""
    _native().un_update_cleanup(str(root))


if __name__ == "__main__":  # python launcher/updater.py: what the launcher would find
    here = Path(__file__).resolve().parent.parent
    current = installed_version(here) or (0, 0, 0)
    print("installed:", version_text(current) if installed_version(here) else "source tree")
    found = latest_release(current)
    print("newer release:", found and {k: v for k, v in found.items() if k != "notes"})
    sys.exit(0)
