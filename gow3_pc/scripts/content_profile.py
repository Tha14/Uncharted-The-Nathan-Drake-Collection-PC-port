"""Prepare explicit offline AppContent metadata; SKU is a probe setting, not a license check.

Output: out/content.bin and out/content-profile.json, from the game's sce_sys/param.sfo (its four user-defined
values). The work is done by the launcher's native code (launcher/native/prepare.cpp).
"""
import argparse
from pathlib import Path


def prepare(game, out, sku='full'):
    """content.bin and content-profile.json in `out`. Prints a summary; ValueError when they can't be made."""
    import ctypes
    dll = ctypes.CDLL(str(Path(__file__).resolve().parent.parent.parent / 'launcher' / 'native' / 'uncharted_native.dll'))
    dll.un_content_profile.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p,
                                       ctypes.c_int]
    dll.un_content_profile.restype = ctypes.c_int
    size = 1 << 12
    while True:
        text = ctypes.create_string_buffer(size)
        status = dll.un_content_profile(str(game), str(out), sku.encode(), text, size)
        if status >= 0:
            break
        size = -status
    message = text.value.decode('utf-8', 'replace')
    if status != 0:
        raise ValueError(message)
    print(message, end='')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('game', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    p.add_argument('--sku', choices=['full', 'trial'], default='full')
    a = p.parse_args()
    prepare(a.game, a.out, a.sku)
