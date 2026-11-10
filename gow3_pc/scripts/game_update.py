"""Read-only combined view of a dumped base game and its separate update folder.

The update (eboot.bin, sce_sys, ...) is laid over the base game in a temporary folder, so
neither dump is modified. Whatever the update does not touch is linked as a whole (files as
hard links first, then symlinks; folders as symlinks or junctions; files copied when nothing
else works), so a 47 GB game costs a handful of links. Names are matched without regard to case.

The work is done by the launcher's native code (launcher/native/starter_files.cpp: game_view,
remove_view); the game starter calls it directly.
"""
import json
from pathlib import Path


def _native(what, *args):
    import ctypes
    dll = ctypes.CDLL(str(Path(__file__).resolve().parent.parent.parent / 'launcher' / 'native' / 'uncharted_native.dll'))
    dll.un_starter_files.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    dll.un_starter_files.restype = ctypes.c_int
    out = ctypes.create_string_buffer(1 << 16)
    status = dll.un_starter_files(what.encode(), json.dumps([str(a) for a in args]).encode('utf-8'), out, len(out))
    text = out.value.decode('utf-8', 'replace')
    if status == 1:
        raise OSError(text)
    if status != 0:
        raise ValueError(text)
    return text


def create_view(base, update, out):
    """The folder holding base with update over it, made in out; the caller removes it (remove_view).
    ValueError when the folders are not a game and its update; OSError when the links can't be made."""
    return Path(_native('game_view', base, update, out))


def remove_view(path):
    """Deletes a view made by create_view (the dumps stay untouched)."""
    _native('remove_view', path)
