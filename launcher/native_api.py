"""The launcher's calls into uncharted_native.dll (launcher/native): the settings core (config.cpp). Python only passes
values in and out; the work is in the library."""
import ctypes
import json
from pathlib import Path

NATIVE = Path(__file__).resolve().parent / "native" / "uncharted_native.dll"
_dll = None


def _native():
    global _dll
    if _dll is None:
        _dll = ctypes.CDLL(str(NATIVE))
        _dll.un_settings_table.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_gpu_recommendation.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_gpu_query.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        _dll.un_monitor_refresh_hz.restype = ctypes.c_int
        for name in ("un_display_table", "un_resolution_options"):
            getattr(_dll, name).argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_aspect_resolution.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_game_sizes.argtypes = [ctypes.c_char_p] * 4 + [ctypes.c_char_p, ctypes.c_int]
        for name in ("un_upscaling_choice", "un_anti_aliasing_choice"):
            getattr(_dll, name).argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_upscaling_options.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_ini_read.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_ini_write.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p]
        _dll.un_system_tables.argtypes = [ctypes.c_char_p, ctypes.c_int]
        _dll.un_system.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
        _dll.un_pid_alive.argtypes = [ctypes.c_ulong]
        for name in ("un_settings_table", "un_ini_read", "un_ini_write", "un_system_tables", "un_system", "un_wine",
                     "un_pid_alive"):
            getattr(_dll, name).restype = ctypes.c_int
    return _dll


def _text(call, *args):
    size = 1 << 15
    while True:
        out = ctypes.create_string_buffer(size)
        status = call(*args, out, size)
        if status >= 0:
            return out.value.decode("utf-8", "replace")
        size = -status


def _rows(text):
    return [line.split("\t") for line in text.splitlines() if line]


def table(name):
    return _rows(_text(_native().un_settings_table, name.encode()))


def ini_defaults():
    return {key: value for key, value in table("ini_defaults")}


def fps_limits():
    return [(int(value), label) for value, label in table("fps_limits")]


def presets():
    """{name: {title, summary, detail, resolution, upscale, values}} in the cards' order."""
    out = {}
    for name, title, summary, detail, resolution, upscale, values in table("presets"):
        out[name] = {"title": title, "summary": summary, "detail": detail, "resolution": resolution,
                     "upscale": upscale, "values": dict(pair.split("=", 1) for pair in values.split(";") if pair)}
    return out


def ini_read(user_dir, game):
    return {key: value for key, value in _rows(_text(_native().un_ini_read, str(user_dir), game.encode()))}


def config_read(config_file, prefix_file, user_dir, wine):
    """The launcher's settings (launcher.json with defaults, the Wine merge and the version migrations) as a dict."""
    import json
    dll = _native()
    dll.un_config_read.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int, ctypes.c_char_p,
                                   ctypes.c_int]
    return json.loads(_text(dll.un_config_read, str(config_file), str(prefix_file), str(user_dir), int(bool(wine))))


def config_write(config_file, config):
    """Writes the settings (the library keeps the file's layout and writes it safely); False when it can't."""
    import json
    dll = _native()
    dll.un_config_write.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p]
    return dll.un_config_write(str(config_file), json.dumps(config).encode("utf-8")) == 0


def _call(name, *args):
    dll = _native()
    function = getattr(dll, name)
    function.restype = ctypes.c_int
    return _text(function, *args)


def gpu_recommendation(logs):
    """(card name or None, integrated True/False/None, preset or None) for Quick Setup: the card the game used last
    (its newest log), else Windows' strongest known adapter."""
    name, integrated, preset = _call("un_gpu_recommendation", "\n".join(str(p) for p in logs)).split("\t")
    return name or None, {"1": True, "0": False}.get(integrated), preset or None


def gpu_query(what, name, integrated=None):
    """what: "tier" | "recommend" | "igpu_class" | "integrated"; "" answers come back as None."""
    flag = -1 if integrated is None else int(bool(integrated))
    return _call("un_gpu_query", what.encode(), (name or "").encode("utf-8"), flag) or None


def monitor_refresh_hz():
    return _native().un_monitor_refresh_hz()


def display_table(name):
    return [tuple(row) for row in _rows(_call("un_display_table", name.encode()))]


def resolution_options(aspect):
    return [tuple(row) for row in _rows(_call("un_resolution_options", aspect.encode()))]


def aspect_resolution(resolution, aspect):
    return _call("un_aspect_resolution", resolution.encode(), aspect.encode())


def game_sizes(resolution, aspect, upscale, upscaling):
    render, output = _call("un_game_sizes", resolution.encode(), aspect.encode(), upscale.encode(),
                           upscaling.encode()).split("\t")
    return render, output or None


def upscaling_choice(upscaler, fsr1):
    return _call("un_upscaling_choice", upscaler.encode(), fsr1.encode())


def anti_aliasing_choice(upscaler, launcher_aa):
    return _call("un_anti_aliasing_choice", upscaler.encode(), (launcher_aa or "").encode())


def upscaling_options(root):
    return [tuple(row) for row in _rows(_call("un_upscaling_options", str(root)))]


def controls_tables():
    """The Controls page's tables (launcher/native/controls.cpp), in the launcher's Python types."""
    import json
    dll = _native()
    dll.un_controls_tables.argtypes = [ctypes.c_char_p, ctypes.c_int]
    t = json.loads(_call("un_controls_tables"))
    pairs = lambda rows: [tuple(row) for row in rows]  # noqa: E731
    return {
        "CONTROL_GROUPS": tuple((title, tuple(tuple(a) for a in actions)) for title, actions in t["CONTROL_GROUPS"]),
        "PAD_DEFAULTS": t["PAD_DEFAULTS"], "PAD_FIXED": t["PAD_FIXED"], "SDL_BUTTONS": tuple(t["SDL_BUTTONS"]),
        "PAD_NAMES": {k: tuple(v) for k, v in t["PAD_NAMES"].items()}, "STICK_ACTIONS": set(t["STICK_ACTIONS"]),
        "DEFAULT_CONTROLS": t["DEFAULT_CONTROLS"], "PROMPT_MODES": pairs(t["PROMPT_MODES"]),
        "MOUSE_SENSITIVITIES": pairs(t["MOUSE_SENSITIVITIES"]), "MOUSE_FACTORS": pairs(t["MOUSE_FACTORS"]),
        "MOUSE_AIM_FACTORS": pairs(t["MOUSE_AIM_FACTORS"]), "MOUSE_LABELS": t["MOUSE_LABELS"],
        "TK_TO_SDL": t["TK_TO_SDL"], "RESERVED_KEYS": set(t["RESERVED_KEYS"])}


def controls(what, value):
    """Controls conversion (launcher/native/controls.cpp): "to_ini", "from_ini", "merged", "merged_pad"."""
    import json
    dll = _native()
    dll.un_controls.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    return json.loads(_call("un_controls", what.encode(), json.dumps(value).encode("utf-8")))


def ini_write(user_dir, game, updates):
    request = "".join(f"{key}\t{value}\n" for key, value in updates.items()).encode("utf-8")
    if _native().un_ini_write(str(user_dir), game.encode(), request) != 0:
        raise OSError(f"could not write {Path(user_dir) / f'probe-{game}.ini'}")


# ---------------------------------------------------------------------------------------------- system.cpp


def system_tables():
    """GAMES (list of dicts), LANGUAGES ((id, name) pairs), CHEATS ((name, title, hint) triples)."""
    t = json.loads(_text(_native().un_system_tables))
    return {"GAMES": t["GAMES"], "LANGUAGES": [tuple(x) for x in t["LANGUAGES"]],
            "CHEATS": tuple(tuple(x) for x in t["CHEATS"])}


def wine():
    return bool(_native().un_wine())


def system(what, arg=""):
    """A system.cpp answer as text, or None when there is none."""
    size = 1 << 12
    while True:
        out = ctypes.create_string_buffer(size)
        status = _native().un_system(what.encode(), str(arg), out, size)
        if status > 0:
            return None
        if status == 0:
            text = out.value.decode("utf-8", "replace")
            return text if text or what == "app_version" else None
        size = -status


def pid_alive(pid):
    try:
        return bool(pid) and bool(_native().un_pid_alive(int(pid)))
    except (ValueError, OverflowError, ctypes.ArgumentError):
        return False
