"""Hands a launcher update to tools\\apply_update.exe (launcher/native/apply.cpp), the installer that sits next to this
file in the new version's package.

Launchers up to 0.10.63 install an update by starting the new package's
    python\\pythonw.exe -I tools\\apply_update.py --root <install folder> --wait-pid <launcher process id>
so this script stays in every package for them; newer launchers start apply_update.exe themselves. It passes its
arguments on unchanged and ends with the installer's exit code.
"""
import subprocess
import sys
from pathlib import Path

if __name__ == "__main__":
    installer = Path(__file__).resolve().with_name("apply_update.exe")
    sys.exit(subprocess.call([str(installer), *sys.argv[1:]], close_fds=True))
