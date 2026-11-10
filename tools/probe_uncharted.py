"""The game starter the launcher runs for PLAY: python -u tools/probe_uncharted.py --boot --seconds 0
--executable <eboot.bin | big2-ps4_Shipping.elf | big3-ps4_Shipping.elf> --game <game folder>.

All of it is launcher/native/start.cpp (un_start_game in uncharted_native.dll): the update laid over the game, the
preparation, the code patches, the render size, the runtime's settings from the launcher's environment
(UNCHARTED_*), the runtime with its log, freeze watchdog and memory tracker, and the Collection's switches between
games. Other options: --guest-arg (repeatable), --collection-menu, --cpu-only, --capture, and --seconds 0 for no time
limit (without --boot the runtime only checks the image's imports).
"""
import ctypes
import json
import sys
from pathlib import Path


def start(argv):
    root = Path(__file__).resolve().parent.parent
    library = ctypes.CDLL(str(root / "launcher" / "native" / "uncharted_native.dll"))
    library.un_start_game.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p]
    library.un_start_game.restype = ctypes.c_longlong
    return library.un_start_game(str(root), json.dumps(argv).encode("utf-8"))


if __name__ == "__main__":
    sys.exit(start(sys.argv[1:]))
