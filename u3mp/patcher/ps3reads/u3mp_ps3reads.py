"""Records which PS3 files the import opens (for the Multiplayer tab's "keep only the PS3 files updates need" option).

Loaded by sitecustomize.py of this folder in every Python process of the import (u3mp_import.py puts this folder on
PYTHONPATH) and by u3mp_import.py itself. U3MP_PS3_ROOTS = the PS3 folders, joined with os.pathsep;
U3MP_PS3_READS = the file the paths are appended to (one per line). A file counts when it is under one of the folders
or directly in a folder's parent (PS3_GAME/PARAM.SFO next to USRDIR). Nothing is recorded without both variables."""
import io
import os
import sys

_open = io.open


def install(roots=None, log_path=None):
    roots = roots if roots is not None else [r for r in os.environ.get('U3MP_PS3_ROOTS', '').split(os.pathsep) if r]
    log_path = log_path or os.environ.get('U3MP_PS3_READS', '')
    roots = [os.path.normcase(os.path.abspath(r)).rstrip('\\/') for r in roots if r]
    if not roots or not log_path:
        return False
    prefixes = tuple(r + os.sep for r in roots)
    parents = {os.path.dirname(r) for r in roots}
    seen = set()

    def hook(event, args):
        if event != 'open' or not args:
            return
        p = args[0]
        if isinstance(p, int) or p is None:
            return
        try:
            p = os.path.abspath(os.fsdecode(p))
        except (TypeError, ValueError):
            return
        q = os.path.normcase(p)
        if q in seen:
            return
        if not q.startswith(prefixes) and os.path.dirname(q) not in parents:
            return
        seen.add(q)
        try:
            with _open(log_path, 'a', encoding='utf-8') as f:
                f.write(p + '\n')
        except OSError:
            pass

    sys.addaudithook(hook)
    return True
