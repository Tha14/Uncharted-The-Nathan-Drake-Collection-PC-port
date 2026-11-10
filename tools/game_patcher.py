"""The game starter's code patches (tools/probe_uncharted.py): the Python side of uncharted_native.dll's patcher
(launcher/native/patcher.cpp), which finds the byte patterns in the linked game image and applies the patches.

apply_image_patches(image, executable): the game's code patches for the current settings (launcher/native/
patch_tables.cpp); prints what was patched, raises SystemExit when a patch's place holds other bytes (the image is then
left as it was).
apply_patches(image, patches): a patch list of your own (the same form; tests and tools).
pattern_find(file, pattern, limit): the start offsets of a pattern's matches in a file, without overlaps.
system(what, arg): the native game detection (launcher/native/system.cpp) the game starter uses: sfo, starter_version,
find_update; None when there is no answer.
resolution_patch(data, width, height, move, too_large): the render resolution in the linked image.
render_resolution(image, executable, width, height, multiplayer): the render size that fits, patched in.
starter(what, arg): the game starter's settings from its environment (launcher/native/starter.cpp).
runtime_env(...): the runtime's environment (launcher/native/runtime_env.cpp).
starter_files(what, *args): the game starter's files (launcher/native/starter_files.cpp); OSError when it failed."""
import ctypes
import json
from pathlib import Path

NATIVE = Path(__file__).resolve().parent.parent / "launcher" / "native" / "uncharted_native.dll"
_dll = None


def _native():
    global _dll
    if _dll is None:
        _dll = ctypes.CDLL(str(NATIVE))
        _dll.un_apply_patches.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_apply_patches.restype = ctypes.c_int
        _dll.un_pattern_find.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        _dll.un_pattern_find.restype = ctypes.c_int
        _dll.un_system.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_system.restype = ctypes.c_int
        _dll.un_starter.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_starter.restype = ctypes.c_int
        _dll.un_runtime_env.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_runtime_env.restype = ctypes.c_int
        _dll.un_starter_files.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_starter_files.restype = ctypes.c_int
        _dll.un_apply_image_patches.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_apply_image_patches.restype = ctypes.c_int
        _dll.un_render_resolution.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                              ctypes.c_char_p, ctypes.c_int]
        _dll.un_render_resolution.restype = ctypes.c_int
        _dll.un_resolution_patch.argtypes = [ctypes.POINTER(ctypes.c_ubyte), ctypes.c_ulonglong, ctypes.c_int, ctypes.c_int,
                                             ctypes.c_int, ctypes.POINTER(ctypes.c_longlong), ctypes.c_char_p, ctypes.c_int]
        _dll.un_resolution_patch.restype = ctypes.c_int
    return _dll


def _spec(patches):
    out = []
    for description, variants in patches:
        listed = []
        for variant in variants:
            if len(variant) == 2:
                listed.append([variant[0], [[offset, bytes(original).hex(), bytes(replacement).hex()]
                                            for offset, original, replacement in variant[1]]])
            else:
                listed.append([variant[0], variant[1], bytes(variant[2]).hex(), bytes(variant[3]).hex()])
        out.append([description, listed])
    return json.dumps(out).encode("utf-8")


def apply_patches(image, patches):
    spec = _spec(patches)
    size = 1 << 16
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_apply_patches(str(image), spec, out, size)
        if status >= 0:
            break
        size = -status
    lines = out.value.decode("utf-8", "replace").splitlines()
    if status == 1:
        for line in lines[:-1]:
            print(line)
        raise SystemExit(lines[-1] if lines else "Patches not applied")
    for line in lines:
        print(line)


def pattern_find(file, pattern, limit=0):
    size = 1 << 12
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_pattern_find(str(file), pattern.encode(), limit, out, size)
        if status > 0:
            raise OSError(f"cannot search {file}")
        if status == 0:
            return [int(x) for x in out.value.split()]
        size = -status


def system(what, arg=""):
    size = 1 << 12
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_system(what.encode(), str(arg), out, size)
        if status > 0:
            return None
        if status == 0:
            return out.value.decode("utf-8", "replace")
        size = -status


def resolution_patch(data, width, height, move=True, too_large=Exception):
    """The render resolution patched into the linked image `data` (a bytearray, changed in place only when it
    worked); prints what was patched and returns how many MiB the memory areas grew. Raises `too_large` when the
    size doesn't fit the game's memory, SystemExit when the game's code is not as expected."""
    buffer = (ctypes.c_ubyte * len(data)).from_buffer(data)
    extra = ctypes.c_longlong(0)
    size = 1 << 16
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_resolution_patch(buffer, len(data), int(width), int(height), 1 if move else 0,
                                               ctypes.byref(extra), out, size)
        if status >= 0:
            break
        size = -status
    del buffer
    lines = out.value.decode("utf-8", "replace").splitlines()
    if status == 0:
        for line in lines:
            print(line)
        return extra.value
    for line in lines[:-1]:
        print(line)
    message = lines[-1] if lines else "Resolution patch failed"
    if status == 2:
        raise too_large(message)
    raise SystemExit(message)


def starter(what, arg=""):
    """The game starter's settings from its environment (launcher/native/starter.cpp): hardware_threads, job_sleep,
    job_workers, u3_exposure, instance_suffix, u3mp_image."""
    size = 1 << 12
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_starter(what.encode(), str(arg).encode("utf-8"), out, size)
        if status > 0:
            raise ValueError(f"unknown starter setting {what}")
        if status == 0:
            return out.value.decode("utf-8", "replace")
        size = -status


def runtime_env(executable, game, game_key, user, out, render_size, size, extra_mib, loadexec, capture=False):
    """The runtime's environment (launcher/native/runtime_env.cpp) from this process's own, the game and its render
    size; prints what the game starter tells the log. Returns (env, capture folder or None, capture trigger or None).
    It writes the player's files as the game starter did (probe-pad.txt, upscaler=off, capture folders)."""
    import os
    inputs = {"env": [[k, v] for k, v in os.environ.items()], "executable": executable, "game": str(game),
              "game_key": game_key, "user": str(user), "out": str(out), "root": str(Path(__file__).resolve().parent.parent),
              "loadexec": str(loadexec), "render": list(render_size), "size": list(size), "extra_mib": int(extra_mib),
              "capture": bool(capture)}
    size_out = 1 << 23
    out_buffer = ctypes.create_string_buffer(size_out)
    status = _native().un_runtime_env(json.dumps(inputs).encode("utf-8"), out_buffer, size_out)
    text = out_buffer.value.decode("utf-8", "replace")
    if status != 0:
        raise SystemExit(f"Runtime settings: {text if status > 0 else 'too large'}")
    result = json.loads(text)
    for line in result["print"]:
        print(line)
    return ({k: v for k, v in result["env"]}, Path(result["capture"]) if result["capture"] else None,
            Path(result["trigger"]) if result["trigger"] else None)


def starter_files(what, *args):
    """game_executable(root), user_dir(root, suffix), boot_view(root, game, update): their text (see
    launcher/native/starter_files.cpp). They copy files, so they run once, with room for the answer."""
    out = ctypes.create_string_buffer(1 << 16)
    status = _native().un_starter_files(what.encode(), json.dumps([str(a) for a in args]).encode("utf-8"), out, len(out))
    text = out.value.decode("utf-8", "replace")
    if status == 1:
        raise OSError(text)
    if status != 0:
        raise ValueError(f"starter_files {what}: status {status}")
    return text


def apply_image_patches(image, executable):
    size = 1 << 16
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_apply_image_patches(str(image), executable.encode(), out, size)
        if status >= 0:
            break
        size = -status
    lines = out.value.decode("utf-8", "replace").splitlines()
    if status == 1:
        for line in lines[:-1]:
            print(line)
        raise SystemExit(lines[-1] if lines else "Patches not applied")
    for line in lines:
        print(line)


def render_resolution(image, executable, width, height, multiplayer=False):
    """The render size of the linked image for the size chosen: the size itself or the largest smaller one that fits
    the game's memory, patched in (launcher/native/patcher.cpp). Prints what it did; returns ((width, height), MiB the
    memory areas grew by); SystemExit when the game's code is not as expected (the image is left as it was)."""
    size = 1 << 16
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_render_resolution(str(image), executable.encode(), int(width), int(height),
                                                1 if multiplayer else 0, out, size)
        if status >= 0:
            break
        size = -status
    lines = out.value.decode("utf-8", "replace").splitlines()
    if status == 1:
        for line in lines[:-1]:
            print(line)
        raise SystemExit(lines[-1] if lines else "Render resolution failed")
    w, h, extra = (int(v) for v in lines[0].split())
    for line in lines[1:]:
        print(line)
    return (w, h), extra
