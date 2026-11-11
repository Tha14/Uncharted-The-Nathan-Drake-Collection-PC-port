r"""Kept for launchers up to 0.10.73: before installing an update they check that this file is in the new package.
The launcher has no Python part any more; started anyway, this opens it (Uncharted Launcher.exe, two folders up)
with the same arguments."""
import pathlib
import subprocess as sp
import sys as _sys


def main(arguments):
    program = pathlib.Path(__file__).resolve().parents[1] / "Uncharted Launcher.exe"
    return sp.call([str(program)] + list(arguments))


raise SystemExit(main(_sys.argv[1:])) if __name__ == "__main__" else None
