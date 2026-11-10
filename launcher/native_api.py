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
        _dll.un_pad_init.argtypes = [ctypes.c_wchar_p]
        _dll.un_pad_state.argtypes = [ctypes.c_char_p, ctypes.c_int]
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


# ---------------------------------------------------------------------------------------------- gamepads.cpp


def pad_init(root):
    return bool(_native().un_pad_init(str(root)))


def pad_state():
    """(number of controllers, first one's style or None, set of held buttons)."""
    count, style, held = (_text(_native().un_pad_state).splitlines() + ["", ""])[:3]
    return int(count or 0), style or None, set(held.split())


# ---------------------------------------------------------------------------------------------- play.cpp


def play_env(config, user_dir):
    """The game's environment values from the launcher's settings, in play()'s order: (name, value) pairs, value None
    to remove the name. KeyError when the settings lack a game's values."""
    dll = _native()
    dll.un_play_env.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
    dll.un_play_env.restype = ctypes.c_int
    size = 1 << 15
    while True:
        out = ctypes.create_string_buffer(size)
        status = dll.un_play_env(json.dumps(config).encode("utf-8"), str(user_dir), out, size)
        if status >= 0:
            break
        size = -status
    text = out.value.decode("utf-8", "replace")
    if status != 0:
        raise KeyError(text)
    return [(name, value) for name, value in json.loads(text)]


class PlaySession:
    """The running game starter (launcher/native/play.cpp): its output goes to `log` and comes back line by line.
    pid; poll() -> the exit code once it ended, else None; returncode; take() -> (new lines, exit code or None);
    stop() ends it and everything it started."""

    def __init__(self, command, env, cwd, log):
        dll = _native()
        dll.un_play_start.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        dll.un_play_start.restype = ctypes.c_int
        dll.un_play_poll.argtypes = [ctypes.c_char_p, ctypes.c_int]
        dll.un_play_poll.restype = ctypes.c_int
        dll.un_play_stop.restype = ctypes.c_int
        request = {"command": [str(c) for c in command], "env": [[str(k), str(v)] for k, v in env.items()],
                   "cwd": str(cwd), "log": str(log) if log else None}
        out = ctypes.create_string_buffer(1 << 12)
        status = dll.un_play_start(json.dumps(request).encode("utf-8"), out, len(out))
        text = out.value.decode("utf-8", "replace")
        if status != 0:
            raise OSError(text)
        self.pid = int(text)
        self.returncode = None

    def take(self):
        size = 1 << 16
        while True:
            out = ctypes.create_string_buffer(size)
            status = _native().un_play_poll(out, size)
            if status >= 0:
                break
            size = -status
        state = json.loads(out.value.decode("utf-8", "replace"))
        if state["exit"] is not None:
            self.returncode = state["exit"]
        return state["lines"], state["exit"]

    def poll(self):
        return self.returncode

    def stop(self):
        _native().un_play_stop()


def crash_reason(root, started_at):
    """Why the game stopped, from its newest boot.log written since started_at (time.time()); None when it tells none."""
    dll = _native()
    dll.un_play_crash_reason.argtypes = [ctypes.c_wchar_p, ctypes.c_double, ctypes.c_char_p, ctypes.c_int]
    dll.un_play_crash_reason.restype = ctypes.c_int
    size = 1 << 12
    while True:
        out = ctypes.create_string_buffer(size)
        status = dll.un_play_crash_reason(str(root), float(started_at), out, size)
        if status >= 0 or status == 2:
            break
        size = -status
    return None if status == 2 else out.value.decode("utf-8", "replace")


# ---------------------------------------------------------------------------------------------- gamefiles.cpp


def check_game_files(root, game_dir):
    """The game folder against the base game's file list (tools/game_files_102.json.gz): (status, report lines).
    status 0: every file is there with its size; 1: some are missing or have another size; 2: the list could not be
    read (the report says why). The first line counts them, then "missing <file>" / "size    <file>" lines."""
    dll = _native()
    dll.un_check_game_files.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_int]
    dll.un_check_game_files.restype = ctypes.c_int
    size = 1 << 20
    while True:
        out = ctypes.create_string_buffer(size)
        status = dll.un_check_game_files(str(root), str(game_dir), out, size)
        if status >= 0:
            return status, out.value.decode("utf-8", "replace").splitlines()
        size = -status


# ---------------------------------------------------------------------------------------------- gameupdate.cpp

_OS_ERRORS = {2: FileNotFoundError, 3: FileNotFoundError, 5: PermissionError, 32: PermissionError,
              80: FileExistsError, 183: FileExistsError}


class GameUpdate:
    """The game's update and the game from the player's own PKG files (launcher/native/gameupdate.cpp, with PkgTool
    from tools/pkgtool). A reason for the player raises GameUpdate.UpdateError; a file system failure an OSError."""

    class UpdateError(Exception):
        pass

    def __init__(self, root):
        self.root = str(root)
        dll = _native()
        dll.un_game_update.argtypes = [ctypes.c_char_p, ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        dll.un_game_update.restype = ctypes.c_int
        dll.un_game_update_step.argtypes = [ctypes.c_char_p, ctypes.c_int]
        dll.un_game_update_step.restype = ctypes.c_int
        self._dll = dll

    def _run(self, what, *args):
        size = 1 << 16
        while True:
            out = ctypes.create_string_buffer(size)
            status = self._dll.un_game_update(what.encode(), self.root, json.dumps([str(a) for a in args]).encode(),
                                              out, size)
            if status >= 0:
                break
            size = -status
        text = out.value.decode("utf-8", "replace")
        if status == 1:
            raise self.UpdateError(text)
        if status == 3:
            code = text[len("[WinError "):].split("]")[0] if text.startswith("[WinError ") else ""
            raise _OS_ERRORS.get(int(code) if code.isdigit() else 0, OSError)(text)
        if status:
            raise ValueError(what)
        return text

    def _step(self):
        out = ctypes.create_string_buffer(1024)
        return out.value.decode("utf-8", "replace") if self._dll.un_game_update_step(out, 1024) == 0 else ""

    def _watched(self, what, log, *args):
        """An install, its steps passed to log as they change (the work runs in this thread, the polling in another)."""
        import threading
        done, last = threading.Event(), [""]

        def report():
            step = self._step()
            if step and step != last[0] and log:
                last[0] = step
                log(step)

        def watch():
            while not done.wait(0.1):
                report()
        watcher = threading.Thread(target=watch, daemon=True)
        watcher.start()
        try:
            return Path(self._run(what, *args))
        finally:
            done.set()
            watcher.join()
            report()   # the last step, when it was quicker than the polling

    def check_update(self, folder):
        """What is wrong with an update's uroot for the port (missing files, unreadable bin.psarc archives)."""
        return self._run("check_update", folder).splitlines()

    def check_folder(self, folder, game):
        """An update the player extracted anywhere: its uroot, checked."""
        return Path(self._run("check_folder", folder, game))

    def existing_update(self, game):
        """The update folder already next to the game, or None."""
        found = self._run("existing_update", game)
        return Path(found) if found else None

    def install(self, pkg, game, log=print):
        """The update PKG unpacked next to the game as <game folder>-patch: its uroot."""
        return self._watched("install", log, pkg, game)

    def install_game(self, pkg, parent, log=print):
        """The player's own game PKG unpacked into the folder <parent>, as <title id>: its uroot (the game folder)."""
        return self._watched("install_game", log, pkg, parent)
