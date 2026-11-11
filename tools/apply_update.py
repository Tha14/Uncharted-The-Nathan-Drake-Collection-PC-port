r"""Kept for launchers up to 0.10.63, which install an update by running the new package's
    python\pythonw.exe -I tools\apply_update.py --root <install folder> --wait-pid <launcher process id>.
The installer is tools\apply_update.exe (launcher/native/apply.cpp), beside this file: it gets the same arguments,
and its exit code is this script's."""
import pathlib
import subprocess as sp
import sys as _sys


def main(arguments):
    installer = pathlib.Path(__file__).resolve().parent / "apply_update.exe"
    return sp.call([str(installer)] + list(arguments), close_fds=True)


raise SystemExit(main(_sys.argv[1:])) if __name__ == "__main__" else None
