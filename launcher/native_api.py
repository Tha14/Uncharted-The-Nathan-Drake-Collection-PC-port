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


# ---------------------------------------------------------------------------------------------- the online section


class OnlineSection:
    """The Multiplayer pages' online section, all of it in the package's optional u3mp/patcher/online/u3_online.dll
    (u3o_section_*: its page tree, clicks and typed text, its timing, the game's environment)."""

    @classmethod
    def load(cls, path, user_dir, state):
        """The section, or None when the package has no such DLL (or an older one without the section)."""
        try:
            dll = ctypes.CDLL(str(path))
            if dll.u3o_abi() < 5:
                return None
        except (OSError, AttributeError):
            return None
        section = cls(dll)
        if not dll.u3o_section_open(str(user_dir), json.dumps(state).encode("utf-8")):
            return None
        return section

    def __init__(self, dll):
        c_char_p, c_int = ctypes.c_char_p, ctypes.c_int
        dll.u3o_section_open.argtypes = [ctypes.c_wchar_p, c_char_p]
        dll.u3o_section_tick.argtypes = [c_char_p, c_char_p, c_int]
        dll.u3o_section_page.argtypes = [c_char_p, c_char_p, c_int]
        dll.u3o_section_act.argtypes = [c_char_p, c_char_p, c_char_p, c_char_p, c_char_p, c_int]
        dll.u3o_section_handoff.argtypes = [c_char_p, c_char_p, c_char_p, c_int]
        for name in ("u3o_section_open", "u3o_section_tick", "u3o_section_page", "u3o_section_act",
                     "u3o_section_handoff"):
            getattr(dll, name).restype = c_int
        dll.u3o_section_close.restype = None
        self.dll = dll
        self.closed = False

    def _json(self, fn, *args):
        size = 1 << 16
        while True:
            out = ctypes.create_string_buffer(size)
            n = fn(*args, out, size)
            if n >= 0:
                text = out.raw[:n].decode("utf-8", "replace")
                ctypes.memset(out, 0, size)   # the hand-off's values are secret
                return json.loads(text) if text else {}
            size = -n + 1

    def tick(self, host):
        return self._json(self.dll.u3o_section_tick, json.dumps(host).encode("utf-8"))

    def page(self, game):
        return self._json(self.dll.u3o_section_page, game.encode("ascii")) or []

    def act(self, wid, kind, value, host):
        return self._json(self.dll.u3o_section_act, str(wid).encode("utf-8"), str(kind).encode("utf-8"),
                          json.dumps(value).encode("utf-8"), json.dumps(host).encode("utf-8"))

    def handoff(self, game, host):
        return self._json(self.dll.u3o_section_handoff, game.encode("ascii"), json.dumps(host).encode("utf-8"))

    def close(self):
        if not self.closed:
            self.closed = True
            self.dll.u3o_section_close()


# ---------------------------------------------------------------------------------------------- shortcuts.cpp


def shortcuts(root, request):
    """TOOLS' desktop shortcuts: {"kind": "info" | "error", "text"} (request: which, games, python, script)."""
    dll = _native()
    dll.un_shortcuts.argtypes = [ctypes.c_wchar_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    dll.un_shortcuts.restype = ctypes.c_int
    size = 1 << 14
    while True:
        out = ctypes.create_string_buffer(size)
        status = dll.un_shortcuts(str(root), json.dumps(request).encode("utf-8"), out, size)
        if status >= 0:
            break
        size = -status
    return json.loads(out.value.decode("utf-8", "replace"))


# ---------------------------------------------------------------------------------------------- actions.cpp


def action(request):
    """One step of a TOOLS action: {"dialog" | "work" | "done", "step", "data", "config", "progress", "result"}."""
    return _json_call("un_action", request)


def action_poll():
    """The running install: {"running", "text"}."""
    dll = _native()
    dll.un_action_poll.argtypes = [ctypes.c_char_p, ctypes.c_int]
    dll.un_action_poll.restype = ctypes.c_int
    out = ctypes.create_string_buffer(4096)
    dll.un_action_poll(out, 4096)
    return json.loads(out.value.decode("utf-8", "replace"))


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


def _json_call(name, state):
    dll = _native()
    fn = getattr(dll, name)
    fn.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    fn.restype = ctypes.c_int
    size = 1 << 16
    while True:
        out = ctypes.create_string_buffer(size)
        status = fn(json.dumps(state).encode("utf-8"), out, size)
        if status >= 0:
            break
        size = -status
    if status != 0:
        raise ValueError(name)
    return json.loads(out.value.decode("utf-8"))


def play_questions(state):
    """PLAY's questions before the game starts: [{"id", "text", "remember"}] (state: root, config, game_dir,
    update_dir)."""
    return _json_call("un_play_questions", state)


def play_end(state):
    """What to say when the game ended: {"status", "detail", "ask"} (state: root, game_dir, update_dir, code,
    stopping, started_at)."""
    return _json_call("un_play_end", state)


# ---------------------------------------------------------------------------------------------- pages.cpp


def page_act(page, wid, kind, value, state):
    """What a click on a native page's control changes ({"config", "ini", "game"}): un_page_act for the launcher's
    state {"config", "ini", "root", "gpu"}. The Python side uses it where it still applies one (the reset to Medium)."""
    import json
    dll = _native()
    dll.un_page_act.argtypes = [ctypes.c_char_p] * 6 + [ctypes.c_int]
    dll.un_page_act.restype = ctypes.c_int
    text = _text(dll.un_page_act, page.encode(), wid.encode(), kind.encode(), json.dumps(value).encode(),
                 json.dumps(state).encode("utf-8"))
    return json.loads(text) if text else {}
