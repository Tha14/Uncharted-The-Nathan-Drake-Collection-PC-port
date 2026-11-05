"""Copies a Windows Python installation into the package (python/): the interpreter, the standard
library without tests and tools, Tkinter (DLLs, tcl), Pillow and, when installed, numpy (the Uncharted 3
multiplayer import's map and character steps). Players need no Python installed.

usage: copy_python.py <python home> <destination>
"""
import shutil
import sys
from pathlib import Path

SKIP_LIB = {"site-packages", "test", "tests", "idlelib", "ensurepip", "turtledemo", "__pycache__",
            "lib2to3", "venv", "pydoc_data"}
SITE_PACKAGES = ("PIL",)
OPTIONAL_PACKAGES = ("numpy", "numpy.libs")  # pip install numpy (for the Python that is packaged)


def main():
    home, dest = Path(sys.argv[1]), Path(sys.argv[2])
    dest.mkdir(parents=True, exist_ok=True)
    for pattern in ("python.exe", "pythonw.exe", "python3.dll", "python3*.dll", "vcruntime140*.dll",
                    "LICENSE.txt"):
        for path in home.glob(pattern):
            shutil.copy2(path, dest / path.name)
    for folder in ("DLLs", "tcl"):
        if (home / folder).exists():
            shutil.copytree(home / folder, dest / folder, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("__pycache__"))

    def ignore_lib(directory, names):
        return {name for name in names if name in SKIP_LIB or name.endswith(".pyc")}

    shutil.copytree(home / "Lib", dest / "Lib", dirs_exist_ok=True, ignore=ignore_lib)
    site = home / "Lib" / "site-packages"
    for package in SITE_PACKAGES:
        if not (site / package).exists():
            sys.exit(f"{package} is not installed in {home} (pip install pillow)")
        shutil.copytree(site / package, dest / "Lib" / "site-packages" / package, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("__pycache__"))
    for package in OPTIONAL_PACKAGES:
        if (site / package).exists():
            shutil.copytree(site / package, dest / "Lib" / "site-packages" / package, dirs_exist_ok=True,
                            ignore=shutil.ignore_patterns("__pycache__", "tests"))
        elif package == "numpy":
            print("Note: numpy is not installed in this Python: the Uncharted 3 multiplayer import will skip "
                  "the steps that need it (pip install numpy)")
    print(f"Python copied from {home}")


if __name__ == "__main__":
    main()
