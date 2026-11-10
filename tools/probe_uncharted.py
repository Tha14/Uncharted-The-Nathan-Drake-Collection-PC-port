"""Prepare and run Uncharted: The Nathan Drake Collection on the local runtime.

The launcher (launcher/uncharted_launcher.pyw) starts this with --boot --seconds 0. Its
environment carries the options: UNCHARTED_DIAGNOSTICS=0 leaves out the measurement logging and
live trigger files, GOW3_VBLANK_HZ=0 unlocks the frame rate (480 Hz vblank, menu limit).

The start sequence is the launcher's native code (launcher/native/start.cpp): the update laid over
the game, the preparation, the code patches, the render size, the runtime's settings, the runtime
itself with its log, freeze watchdog and memory tracker, and the Collection's game switches.
Options: --game, --boot, --executable, --guest-arg (repeatable), --collection-menu, --cpu-only,
--capture, --seconds (0: no time limit).
"""
import ctypes
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    dll = ctypes.CDLL(str(ROOT / "launcher" / "native" / "uncharted_native.dll"))
    dll.un_start_game.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p]
    dll.un_start_game.restype = ctypes.c_longlong
    return dll.un_start_game(str(ROOT), json.dumps(sys.argv[1:]).encode("utf-8"))


if __name__ == "__main__":
    sys.exit(main())
