r"""Kept for launchers up to 0.10.73: before installing an update they check that this file is in the new package.
The game starter is launcher\native\game_starter.exe (launcher/native/start.cpp); run with Python, this runs it with
the same arguments, e.g. --boot --seconds 0 --executable eboot.bin --game <game folder>."""
import pathlib
import subprocess as sp
import sys as _sys


def main(arguments):
    starter = pathlib.Path(__file__).resolve().parents[1] / "launcher" / "native" / "game_starter.exe"
    return sp.call([str(starter)] + list(arguments))


raise SystemExit(main(_sys.argv[1:])) if __name__ == "__main__" else None
