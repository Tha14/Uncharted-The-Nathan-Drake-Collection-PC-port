"""Link the eboot with the game's bundled system modules into one probe image.

Successor of link_libc.py for several modules (default: libc.prx, then
libSceFios2.prx). Every module is placed after the previous one; imports of
all modules share one import table, so the C runtime can override any
function (host contracts first) and otherwise binds the native export.

Exports are matched by (NID, library identity, module identity). Imports of
libSceLibcInternal are served by libc.prx's same-NID exports: the internal
library is not shipped with the game and exposes the same functions.
Output: out/boot-linked.bin (format G3PROBE5) and out/link.json.

The work is done by the launcher's native code (launcher/native/link.cpp).
"""
from pathlib import Path

DEFAULT_MODULES = ('libc.prx', 'libSceFios2.prx')


def link(game, out, module_names=DEFAULT_MODULES, executable='eboot.bin'):
    """boot-linked.bin and link.json in `out` from its boot.bin (prepare.py) and the game folder's executable and
    sce_module files. Prints a summary; ValueError when they can't be linked."""
    import ctypes
    dll = ctypes.CDLL(str(Path(__file__).resolve().parent.parent.parent / 'launcher' / 'native' / 'uncharted_native.dll'))
    dll.un_link_modules.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p,
                                    ctypes.c_char_p, ctypes.c_int]
    dll.un_link_modules.restype = ctypes.c_int
    size = 1 << 16
    while True:
        text = ctypes.create_string_buffer(size)
        status = dll.un_link_modules(str(game), str(out), executable.encode(),
                                     '\n'.join(module_names).encode(), text, size)
        if status >= 0:
            break
        size = -status
    message = text.value.decode('utf-8', 'replace')
    if status != 0:
        raise ValueError(message)
    print(message, end='')


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--modules', nargs='*', default=list(DEFAULT_MODULES))
    p.add_argument('--executable', default='eboot.bin')
    a = p.parse_args()
    link(a.game, a.out, a.modules, a.executable)
