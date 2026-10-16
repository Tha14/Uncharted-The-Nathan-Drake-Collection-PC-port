#!/usr/bin/env python3
"""Uncharted: The Nathan Drake Collection - launcher (Windows, Tkinter + Pillow).

Pick a game, set display and performance options (presets for low-end and high-end PCs) and play.
Runtime options are kept in analysis/uncharted-user/probe.ini, the same file the in-game menu
(Insert / R3+L2) reads and writes; launcher-only options in %APPDATA%/Uncharted Launcher.
The game is started through tools/probe_uncharted.py (preparation, code patches, Collection game
switches), with the options below passed as environment variables.
"""
import ctypes
import json
import os
from pathlib import Path
import queue
import shutil
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
import webbrowser
from tkinter import messagebox

from PIL import Image, ImageDraw, ImageTk

import updater  # launcher/updater.py: updates from the GitHub releases

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "launcher" / "assets"
DEFAULT_GAME_DIR = ROOT / "game" / "CUSA02344" / "uroot"
STARTER_EXE = ROOT / "Uncharted Launcher.exe"  # the packaged build's starter (tools/package_windows.py)
USER_DIR = ROOT / "analysis" / "uncharted-user"
INI_FILE = USER_DIR / "probe.ini"
RUN_SCRIPT = ROOT / "tools" / "probe_uncharted.py"
CONFIG_FILE = Path(os.environ.get("APPDATA", Path.home())) / "Uncharted Launcher" / "launcher.json"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
APP_TITLE = "Uncharted: The Nathan Drake Collection"
# The author's links in the launcher header: label, address, color, hover color.
LINKS = (("YouTube", "https://www.youtube.com/@BilalReloaded/videos", "#d6302a", "#ef4038"),
         ("GitHub", "https://github.com/bc160201983", "#24292f", "#3a4048"))

GAMES = [
    {"key": "u1", "number": "1", "title": "DRAKE'S FORTUNE", "name": "Uncharted: Drake's Fortune", "subtitle": "Starts Uncharted 1 directly",
     "exe": "eboot.bin", "log": "uncharted"},
    {"key": "u2", "number": "2", "title": "AMONG THIEVES", "name": "Uncharted 2: Among Thieves", "subtitle": "Starts Uncharted 2 directly",
     "exe": "big2-ps4_Shipping.elf", "log": "big2-ps4_Shipping"},
    {"key": "u3", "number": "3", "title": "DRAKE'S DECEPTION", "name": "Uncharted 3: Drake's Deception", "subtitle": "Starts Uncharted 3 directly",
     "exe": "big3-ps4_Shipping.elf", "log": "big3-ps4_Shipping"},
]

# Palette: the Collection's parchment and gold over a dark leather brown.
BG = "#120e0a"
PANEL = "#1b1510"
PANEL_HI = "#251d16"
LINE = "#3a2e22"
TEXT = "#f1e6cf"
MUTED = "#a8977a"
DIM = "#6f604b"
GOLD = "#d4a24c"
GOLD_HI = "#e9bc66"
INK = "#2a1c0e"
STOP_RED = "#b5533c"

# At most 120: faster, the game's movement timing breaks (Drake's Fortune at 140-160 FPS: running
# slows, Nate fell through the world). The runtime caps older, faster settings at 120 too.
MAX_FPS = 120
FPS_LIMITS = [(30, "30"), (40, "40"), (45, "45"), (50, "50"), (55, "55"), (60, "60"),
              (MAX_FPS, str(MAX_FPS))]

# Runtime options in probe.ini (key -> default) and launcher-only ones.
INI_DEFAULTS = {
    "display_mode": "windowed", "vsync": "1", "fps_limit": "60", "frames_queued": "1",
    "async_shaders": "1", "fsr1": "0", "rcas": "1", "rcas_strength": "75", "show_fps": "1",
}
LAUNCHER_DEFAULTS = {"game": "u1", "diagnostics": True, "game_dir": "", "check_updates": True}
GAME_KEYS = ("u1", "u2", "u3")
GAME_SETTING_DEFAULTS = {"preset": "medium", "resolution": "1920x1080", "multithreaded": False,
                         "async_readback": False, "controls": None, "mouse": True, "mouse_sens": "1.0",
                         "mouse_invert": False}

# Keyboard and mouse controls (runtime_pad.c): action -> up to two bindings, "key:<SDL key name>"
# or "mouse:<left|middle|right|x1|x2|wheel_up|wheel_down>". Stick directions take keys only.
CONTROL_GROUPS = (
    ("MOVEMENT", (("ls_up", "Move forward"), ("ls_down", "Move back"), ("ls_left", "Move left"),
                  ("ls_right", "Move right"), ("rs_up", "Look up"), ("rs_down", "Look down"),
                  ("rs_left", "Look left"), ("rs_right", "Look right"))),
    ("WEAPONS (D-PAD)", (("left", "D-pad left"), ("right", "D-pad right"), ("up", "D-pad up"),
                         ("down", "D-pad down"))),
    ("ACTIONS", (("cross", "Jump, climb  (Cross)"), ("circle", "Cover, roll, drop  (Circle)"),
                 ("square", "Melee  (Square)"), ("triangle", "Reload, pick up  (Triangle)"),
                 ("l2", "Aim  (L2)"), ("r2", "Fire  (R2)"), ("l1", "Grenade aim  (L1)"),
                 ("r1", "Grenade throw  (R1)"), ("l3", "L3"), ("r3", "R3"),
                 ("options", "Pause menu  (Options)"), ("touchpad_left", "Touchpad left"),
                 ("touchpad_right", "Touchpad right"))),
)
STICK_ACTIONS = {"ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right"}
DEFAULT_CONTROLS = {
    "ls_up": ["key:W"], "ls_down": ["key:S"], "ls_left": ["key:A"], "ls_right": ["key:D"],
    "rs_up": ["key:Up"], "rs_down": ["key:Down"], "rs_left": ["key:Left"], "rs_right": ["key:Right"],
    "left": ["key:1", "mouse:wheel_down"], "right": ["key:2", "mouse:wheel_up"], "up": ["key:3"],
    "down": ["key:4"],
    "cross": ["key:Space"], "circle": ["key:Left Ctrl"], "square": ["key:F"], "triangle": ["key:R"],
    "l2": ["mouse:right"], "r2": ["mouse:left"], "l1": ["key:Q", "mouse:x1"], "r1": ["key:E", "mouse:x2"],
    "l3": ["key:Left Shift"], "r3": ["key:V", "mouse:middle"], "options": ["key:Escape", "key:Return"],
    "touchpad_left": ["key:Tab"], "touchpad_right": ["key:Backspace"],
}
MOUSE_SENSITIVITIES = [("0.5", "0.5x"), ("0.75", "0.75x"), ("1.0", "1x"), ("1.5", "1.5x"), ("2.0", "2x"),
                       ("3.0", "3x")]
MOUSE_LABELS = {"left": "Left mouse", "middle": "Middle mouse", "right": "Right mouse", "x1": "Mouse back",
                "x2": "Mouse forward", "wheel_up": "Wheel up", "wheel_down": "Wheel down"}
# Tk key names -> SDL key names (letters and digits map to themselves, upper case).
TK_TO_SDL = {
    "space": "Space", "Return": "Return", "Escape": "Escape", "Tab": "Tab", "BackSpace": "Backspace",
    "Shift_L": "Left Shift", "Shift_R": "Right Shift", "Control_L": "Left Ctrl", "Control_R": "Right Ctrl",
    "Alt_L": "Left Alt", "Alt_R": "Right Alt", "Caps_Lock": "CapsLock", "Up": "Up", "Down": "Down",
    "Left": "Left", "Right": "Right", "Delete": "Delete", "Home": "Home", "End": "End", "Prior": "PageUp",
    "Next": "PageDown", "minus": "-", "equal": "=", "bracketleft": "[", "bracketright": "]",
    "backslash": "\\", "semicolon": ";", "apostrophe": "'", "grave": "`", "period": ".", "slash": "/",
    "KP_Divide": "Keypad /", "KP_Multiply": "Keypad *", "KP_Subtract": "Keypad -", "KP_Add": "Keypad +",
    "KP_Enter": "Keypad Enter", "KP_Decimal": "Keypad .", "Pause": "Pause", "Scroll_Lock": "ScrollLock",
}
TK_TO_SDL.update({f"F{n}": f"F{n}" for n in range(1, 13)})
TK_TO_SDL.update({f"KP_{n}": f"Keypad {n}" for n in range(10)})
# Keys the runtime keeps for itself: Insert opens the in-game menu, F9 records the pad.
RESERVED_KEYS = {"Insert", "F9", "comma"}


def merged_controls(controls):
    merged = {action: list(bindings) for action, bindings in DEFAULT_CONTROLS.items()}
    for action, bindings in (controls or {}).items():
        if action in merged:
            merged[action] = list(bindings)[:2]
    return merged


def control_maps(controls):
    """GOW3_KEY_MAP and GOW3_MOUSE_MAP for a game's controls (every action listed, so unbound ones
    lose the runtime's defaults)."""
    keys, mouse = [], []
    for action, bindings in merged_controls(controls).items():
        names = [b[4:] for b in bindings if b.startswith("key:")]
        keys += [f"{action}={name}" for name in names] or [f"{action}="]
        if action not in STICK_ACTIONS:
            buttons = [b[6:] for b in bindings if b.startswith("mouse:")]
            mouse += [f"{action}={name}" for name in buttons] or [f"{action}="]
    return ",".join(keys), ",".join(mouse)


def binding_label(binding):
    if not binding:
        return "—"
    kind, _, name = binding.partition(":")
    return MOUSE_LABELS.get(name, name) if kind == "mouse" else name


def is_game_dir(folder):
    """The Collection's game folder (CUSA02344): eboot.bin and sce_sys/param.sfo."""
    return bool(folder) and (Path(folder) / "eboot.bin").is_file() and \
        (Path(folder) / "sce_sys" / "param.sfo").is_file()


UPDATE_SUFFIXES = ("-patch", "-update", "_patch", "_update")


def game_update_dir(folder):
    """The game's update (e.g. 1.02) next to its folder, found as tools/probe_uncharted.py does:
    named like the game folder plus -patch or -update, with or without uroot."""
    folder = Path(folder)
    title = folder.parent if folder.name.lower() == "uroot" else folder
    names = {title.name.lower() + suffix for suffix in UPDATE_SUFFIXES}
    try:
        for entry in title.parent.iterdir():
            if entry.name.lower() in names:
                for candidate in (entry / "uroot", entry):
                    if is_game_dir(candidate):
                        return candidate
    except OSError:
        pass
    return None


def app_version(folder):
    """APP_VER of a game folder (sce_sys/param.sfo)."""
    try:
        data = (Path(folder) / "sce_sys" / "param.sfo").read_bytes()
        _, _, keys, values, count = struct.unpack_from("<IIIII", data)
        for i in range(count):
            key_at, _, length, _, value_at = struct.unpack_from("<HHIII", data, 20 + i * 16)
            key = data[keys + key_at:data.index(bytes(1), keys + key_at)].decode()
            if key == "APP_VER":
                return data[values + value_at:values + value_at + length].rstrip(bytes(1)).decode()
    except (OSError, ValueError, struct.error):
        pass
    return None


def monitor_refresh_hz():
    try:
        user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
        dc = user32.GetDC(0)
        hz = gdi32.GetDeviceCaps(dc, 116)  # VREFRESH
        user32.ReleaseDC(0, dc)
        return hz if hz > 1 else 60
    except Exception:
        return 60


def fps_limit_value(value):
    """A listed limit; unlimited (0) and faster ones (earlier versions offered 144-240) are 120."""
    listed = {str(v) for v, _ in FPS_LIMITS}
    if value in listed:
        return value
    try:
        number = int(value)
    except (TypeError, ValueError):
        return "60"
    return str(MAX_FPS) if number == 0 or number > MAX_FPS else "60"


RESOLUTIONS = [("1280x720", "720p"), ("1600x900", "900p"), ("1920x1080", "1080p"), ("2560x1440", "1440p"),
               ("2880x1620", "1620p"), ("3840x2160", "4K")]

# Quality presets: the game has one detail level (the PS4's); quality here is the resolution the
# game draws at, how it reaches the screen (FSR 1, sharpening) and the frame rate. Above 1080p the
# render resolution is new (October 2026): confirm in play. All three games draw at 4K
# natively except Drake's Fortune, which stops at 1620p (its memory layout) and upscales.
PRESETS = {
    "low": {
        "title": "LOW", "summary": "60 FPS  •  720p + FSR 1",
        "detail": "For weak GPUs: the game draws at 720p (about twice as fast as 1080p) and FSR 1 "
                  "scales it up to the screen.",
        "resolution": "1280x720",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "1", "rcas": "1", "rcas_strength": "75"},
    },
    "medium": {
        "title": "MEDIUM", "summary": "60 FPS  •  as on PS4",
        "detail": "The PS4 original: 1080p at 60 FPS. Right for most PCs.",
        "resolution": "1920x1080",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "0", "rcas": "1", "rcas_strength": "75"},
    },
    "high": {
        "title": "HIGH", "summary": "60 FPS  •  1440p",
        "detail": "Sharper: the game draws at 1440p. On 4K monitors FSR 1 scales it up to the screen.",
        "resolution": "2560x1440",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": None, "rcas": "1", "rcas_strength": "50"},
    },
    "ultra": {
        "title": "ULTRA", "summary": "4K  •  up to 120 FPS",
        "detail": "Native 4K (Drake's Fortune: 1620p + FSR 1) with the frame rate unlocked up to 120 "
                  "(experimental). New, confirm in play; needs a fast GPU.",
        "resolution": "3840x2160",
        "values": {"fps_limit": str(MAX_FPS), "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": None, "rcas": "1", "rcas_strength": "50"},
    },
}
PRESET_ORDER = ("low", "medium", "high", "ultra")


def screen_size():
    try:
        user32 = ctypes.windll.user32
        return user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)
    except Exception:
        return 1920, 1080


def fsr1_for(resolution):
    """FSR 1 when the game's image is smaller than the screen (it upscales; else no effect)."""
    width, height = (int(v) for v in resolution.split("x"))
    screen_w, screen_h = screen_size()
    return "1" if width < screen_w and height < screen_h else "0"


def preset_values(name):
    values = dict(PRESETS[name]["values"])
    if values["fsr1"] is None:
        values["fsr1"] = fsr1_for(PRESETS[name]["resolution"])
    return values


# ---------------------------------------------------------------------------------------------
# Settings files

def ini_file(game):
    """Each game keeps its own runtime settings (tools/probe_uncharted.py passes the same file)."""
    return USER_DIR / f"probe-{game}.ini"


def ini_lines(game):
    """The game's settings file, or the shared one of earlier versions as its starting point."""
    for path in (ini_file(game), INI_FILE):
        try:
            return path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
    return ["# Uncharted settings (launcher and in-game menu: Insert / R3+L2)"]


def read_ini(game):
    values = dict(INI_DEFAULTS)
    for line in ini_lines(game):
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    values["fps_limit"] = fps_limit_value(values["fps_limit"])
    return values


def ini_mtime(game):
    try:
        return ini_file(game).stat().st_mtime_ns
    except OSError:
        return None


def write_ini(game, updates):
    """Updates keys in place and keeps every other line (the in-game menu writes more keys)."""
    lines = ini_lines(game)
    done = set()
    for i, line in enumerate(lines):
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in updates:
                lines[i] = f"{key}={updates[key]}"
                done.add(key)
    lines += [f"{key}={value}" for key, value in updates.items() if key not in done]
    path = ini_file(game)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_config():
    config = dict(LAUNCHER_DEFAULTS)
    try:
        config.update(json.loads(CONFIG_FILE.read_text(encoding="utf-8")))
    except (OSError, ValueError):
        pass
    # Version 2: render resolutions above 1080p crash in gameplay; earlier presets chose them.
    if config.get("version", 1) < 2:
        config["resolution"] = "1920x1080"
    # Version 3: preset, resolution and multithreading per game; the shared values start all three.
    games = config.setdefault("games", {})
    resolutions = [r for r, _ in RESOLUTIONS]
    for game in GAME_KEYS:
        entry = games.setdefault(game, {})
        for name, default in GAME_SETTING_DEFAULTS.items():
            entry.setdefault(name, config.get(name, default))
        entry["preset"] = {"balanced": "medium"}.get(entry["preset"], entry["preset"])
        if entry["resolution"] not in resolutions:
            entry["resolution"] = "1920x1080"
    # Version 4: the presets draw at 720p / 1080p / 1440p / 4K (all were 1080p before).
    if config.get("version", 1) < 4:
        for game, entry in games.items():
            if entry["preset"] in PRESETS:
                entry["resolution"] = PRESETS[entry["preset"]]["resolution"]
                write_ini(game, preset_values(entry["preset"]))
    # Version 5: every preset queues 2 frames (with 1 the CPU waits for the GPU each frame: heavy
    # Uncharted 2 scenes ran at 45 instead of 57 FPS). Not with multithreaded rendering, where
    # Uncharted 2 needs 1.
    if config.get("version", 1) < 5:
        for game, entry in games.items():
            if entry["preset"] in PRESETS and not entry.get("multithreaded"):
                write_ini(game, {"frames_queued": "2"})
    # Version 6: every preset queues 1 frame again: with 2, GPU results the game reads came a frame
    # later and Uncharted 2's Nate and NPCs stood above the floor (16 Oct). Custom settings stay.
    if config.get("version", 1) < 6:
        for game, entry in games.items():
            if entry["preset"] in PRESETS:
                write_ini(game, {"frames_queued": "1"})
    for name in GAME_SETTING_DEFAULTS:
        config.pop(name, None)
    config["version"] = 6
    return config


def write_config(config):
    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    CONFIG_FILE.write_text(json.dumps(config, indent=2), encoding="utf-8")


# ---------------------------------------------------------------------------------------------
# Drawing helpers and widgets

def round_rect(canvas, x1, y1, x2, y2, r, **kw):
    points = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2, x2 - r, y2,
              x1 + r, y2, x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
    return canvas.create_polygon(points, smooth=True, **kw)


class Toggle(tk.Canvas):
    def __init__(self, parent, scale, value, command):
        self.s = scale
        w, h = int(46 * scale), int(24 * scale)
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.w, self.h, self.value, self.command = w, h, bool(value), command
        self.bind("<Button-1>", lambda e: self.set(not self.value, notify=True))
        self.draw()

    def set(self, value, notify=False):
        self.value = bool(value)
        self.draw()
        if notify:
            self.command(self.value)

    def draw(self):
        self.delete("all")
        pad = max(1, int(2 * self.s))
        track = GOLD if self.value else LINE
        round_rect(self, 1, 1, self.w - 1, self.h - 1, self.h // 2, fill=track, outline=track)
        d = self.h - 2 * pad - 2
        x = self.w - pad - d - 1 if self.value else pad + 1
        self.create_oval(x, pad + 1, x + d, pad + 1 + d, fill=INK if self.value else MUTED, outline="")


class Segmented(tk.Frame):
    """A row of choices; the selected one is gold."""

    def __init__(self, parent, options, value, command, font):
        super().__init__(parent, bg=LINE, padx=1, pady=1)
        self.command, self.labels, self.value = command, {}, value
        for i, (option, label) in enumerate(options):
            item = tk.Label(self, text=label, font=font, padx=12, pady=5, cursor="hand2", bd=0)
            item.grid(row=0, column=i, padx=(0 if i == 0 else 1, 0))
            item.bind("<Button-1>", lambda e, v=option: self.set(v, notify=True))
            item.bind("<Enter>", lambda e, w=item, v=option: v != self.value and w.config(bg=PANEL_HI))
            item.bind("<Leave>", lambda e: self.restyle())
            self.labels[option] = item
        self.restyle()

    def set(self, value, notify=False):
        self.value = value
        self.restyle()
        if notify:
            self.command(value)

    def restyle(self):
        for option, item in self.labels.items():
            selected = option == self.value
            item.config(bg=GOLD if selected else PANEL, fg=INK if selected else TEXT)


class Launcher:
    def __init__(self, root):
        self.root = root
        self.s = max(1.0, root.winfo_fpixels("1i") / 96.0)
        self.config = read_config()
        if self.config.get("game") not in GAME_KEYS:
            self.config["game"] = "u1"
        self.reload_ini()
        self.process = None
        self.output = queue.Queue()
        self.last_output = 0.0
        self.game_started = False
        self.widgets = {}
        self.images = []
        self.fonts()
        root.title(f"{APP_TITLE} - Launcher by BilalReloaded")
        root.configure(bg=BG)
        root.resizable(False, False)
        self.width = self.px(1180)
        try:
            root.iconbitmap(default=str(ASSETS / "uncharted.ico"))
        except tk.TclError:
            pass
        self.build()
        self.restore_window_position()
        write_config(self.config)  # keeps one-time updates (read_config) even without a change
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(150, self.poll)
        # Updates (launcher/updater.py): what an installed one left behind goes; a newer release
        # shows the UPDATE button.
        self.update_release = None
        self.update_busy = False
        self.update_result = None
        root.after(3000, lambda: updater.cleanup(ROOT))
        if self.config.get("check_updates", True):
            root.after(1500, self.check_updates)

    def restore_window_position(self):
        """The window opens where it was last closed, if that spot is still on a screen."""
        position = self.config.get("window_xy")
        if not (isinstance(position, list) and len(position) == 2):
            return
        x, y = int(position[0]), int(position[1])
        self.root.update_idletasks()
        screen_w, screen_h = self.root.winfo_vrootwidth(), self.root.winfo_vrootheight()
        if -50 <= x <= max(0, screen_w - 200) and 0 <= y <= max(0, screen_h - 200):
            self.root.geometry(f"+{x}+{y}")

    def px(self, value):
        return int(value * self.s)

    def fonts(self):
        self.f_title = ("Georgia", 40, "bold")
        self.f_subtitle = ("Segoe UI Semibold", 11)
        self.f_card_number = ("Georgia", 34, "bold")
        self.f_card_title = ("Georgia", 14, "bold")
        self.f_body = ("Segoe UI", 10)
        self.f_body_bold = ("Segoe UI Semibold", 10)
        self.f_small = ("Segoe UI", 9)
        self.f_tab = ("Segoe UI Semibold", 10)
        self.f_play = ("Georgia", 17, "bold")

    # -- layout -----------------------------------------------------------------------------

    def build(self):
        self.build_hero()
        self.build_cards()
        self.build_footer()
        self.build_tabs()
        self.select_game(self.config.get("game", "u1"), save=False)
        self.show_tab(self.config.get("tab", "setup") if self.config.get("tab") in self.pages else "setup",
                      save=False)

    def build_hero(self):
        w, h = self.width, self.px(250)
        art = None
        for name in ("pic1.png", "pic0.png"):
            if (self.game_dir() / "sce_sys" / name).exists():
                art = Image.open(self.game_dir() / "sce_sys" / name).convert("RGB")
                break
        if art is None:
            art = Image.new("RGB", (1920, 1080), "#d9c9a6")
        # Fill the width; keep Drake (right) and the top of the parchment in view.
        scale = w / art.width
        art = art.resize((w, int(art.height * scale)), Image.LANCZOS)
        top = int(art.height * 0.08)
        art = art.crop((0, top, w, top + h))
        # Fade into the dark panel below.
        fade = Image.new("L", (w, h), 0)
        draw = ImageDraw.Draw(fade)
        start = int(h * 0.55)
        for y in range(start, h):
            draw.line([(0, y), (w, y)], fill=int(255 * ((y - start) / (h - start)) ** 1.6))
        dark = Image.new("RGB", (w, h), BG)
        art = Image.composite(dark, art, fade)
        photo = ImageTk.PhotoImage(art)
        self.images.append(photo)
        canvas = tk.Canvas(self.root, width=w, height=h, bg=BG, highlightthickness=0)
        canvas.pack(side="top")
        canvas.create_image(0, 0, image=photo, anchor="nw")
        x, y = self.px(48), self.px(58)
        canvas.create_text(x, y, text="UNCHARTED", font=self.f_title, fill="#3b2914", anchor="w")
        canvas.create_text(x + self.px(3), y + self.px(44), text="T H E   N A T H A N   D R A K E   C O L L E C T I O N",
                           font=self.f_subtitle, fill="#8a6326", anchor="w")
        canvas.create_line(x + self.px(3), y + self.px(66), x + self.px(330), y + self.px(66), fill="#b98d45", width=2)
        canvas.create_text(x + self.px(3), y + self.px(86),
                           text="DRAKE'S FORTUNE  •  AMONG THIEVES  •  DRAKE'S DECEPTION",
                           font=self.f_small, fill="#6b4c22", anchor="w")
        self.draw_links(canvas, w)

    def draw_links(self, canvas, width):
        """The author's YouTube channel and GitHub as badges in the top right of the hero."""
        right, top, height = width - self.px(22), self.px(18), self.px(30)
        for label, url, color, hover in reversed(LINKS):
            tag = f"link-{label}"
            text = canvas.create_text(0, 0, text=label, font=self.f_body_bold, fill="white", anchor="w", tags=tag)
            x1, _, x2, _ = canvas.bbox(text)
            badge_w = (x2 - x1) + self.px(46)
            left = right - badge_w
            mid = top + height // 2
            canvas.coords(text, left + self.px(34), mid)
            badge = round_rect(canvas, left, top, right, top + height, height // 2, fill=color, outline=color, tags=tag)
            canvas.tag_lower(badge, text)
            # Icon: YouTube's play triangle on white, GitHub's code brackets.
            icon_x = left + self.px(12)
            if label == "YouTube":
                round_rect(canvas, icon_x, mid - self.px(7), icon_x + self.px(16), mid + self.px(7), self.px(4),
                           fill="white", outline="white", tags=tag)
                canvas.create_polygon(icon_x + self.px(6), mid - self.px(4), icon_x + self.px(6), mid + self.px(4),
                                      icon_x + self.px(12), mid, fill=color, outline=color, tags=tag)
            else:
                canvas.create_text(icon_x + self.px(8), mid, text="</>", font=self.f_small, fill="white", tags=tag)
            canvas.tag_bind(tag, "<Button-1>", lambda e, u=url: webbrowser.open(u))
            canvas.tag_bind(tag, "<Enter>", lambda e, b=badge, c=hover: (canvas.itemconfig(b, fill=c, outline=c),
                                                                       canvas.config(cursor="hand2")))
            canvas.tag_bind(tag, "<Leave>", lambda e, b=badge, c=color: (canvas.itemconfig(b, fill=c, outline=c),
                                                                       canvas.config(cursor="")))
            right = left - self.px(10)

    def build_cards(self):
        frame = tk.Frame(self.root, bg=BG)
        frame.pack(side="top", fill="x", padx=self.px(40), pady=(0, self.px(14)))
        self.cards = {}
        gap = self.px(16)
        card_w = (self.width - 2 * self.px(40) - 2 * gap) // 3
        card_h = self.px(92)
        for i, game in enumerate(GAMES):
            canvas = tk.Canvas(frame, width=card_w, height=card_h, bg=BG, highlightthickness=0, cursor="hand2")
            canvas.grid(row=0, column=i, padx=(0 if i == 0 else gap, 0))
            canvas.bind("<Button-1>", lambda e, k=game["key"]: self.select_game(k))
            canvas.bind("<Enter>", lambda e, k=game["key"]: self.draw_card(k, hover=True))
            canvas.bind("<Leave>", lambda e, k=game["key"]: self.draw_card(k))
            self.cards[game["key"]] = (canvas, game, card_w, card_h)

    def draw_card(self, key, hover=False):
        canvas, game, w, h = self.cards[key]
        selected = key == self.config.get("game")
        canvas.delete("all")
        fill = PANEL_HI if selected or hover else PANEL
        outline = GOLD if selected else ("#5a4733" if hover else LINE)
        round_rect(canvas, 2, 2, w - 2, h - 2, self.px(10), fill=fill, outline=outline, width=2 if selected else 1)
        canvas.create_text(self.px(22), h // 2, text=game["number"], font=self.f_card_number,
                           fill=GOLD if selected else DIM, anchor="w")
        canvas.create_text(self.px(70), h // 2 - self.px(12), text=game["title"], font=self.f_card_title,
                           fill=TEXT, anchor="w")
        canvas.create_text(self.px(71), h // 2 + self.px(14), text=game["subtitle"], font=self.f_small,
                           fill=MUTED, anchor="w")
        if selected:
            canvas.create_text(w - self.px(18), self.px(16), text="SELECTED", font=("Segoe UI Semibold", 7),
                               fill=GOLD, anchor="e")

    def select_game(self, key, save=True):
        if key not in self.cards:
            key = "u1"
        changed = key != self.config.get("game")
        self.config["game"] = key
        for k in self.cards:
            self.draw_card(k)
        if save:
            write_config(self.config)
        if changed:
            # Each game keeps its own settings: show the selected game's.
            self.reload_ini()
            self.refresh_controls()
        if hasattr(self, "status") and not self.process:
            self.set_status(f"Ready to play {self.current_game()['name']}.")

    def game_settings(self):
        """Preset, render resolution and multithreading of the selected game."""
        return self.config["games"][self.config["game"]]

    def save_ini(self, updates):
        write_ini(self.config["game"], updates)
        self.ini_seen = ini_mtime(self.config["game"])

    def reload_ini(self):
        """The selected game's settings from its file (the in-game menu writes it too)."""
        self.ini = read_ini(self.config["game"])
        self.ini_seen = ini_mtime(self.config["game"])

    def follow_ini(self):
        """Shows changes the in-game menu saved (Insert, then closing the menu)."""
        if ini_mtime(self.config["game"]) != getattr(self, "ini_seen", None):
            self.reload_ini()
            self.refresh_controls()

    def refresh_controls(self):
        """Every control shows the selected game's settings."""
        for key in INI_DEFAULTS:
            widget = self.widgets.get(key)
            if isinstance(widget, Toggle):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]) == "1")
            elif isinstance(widget, Segmented):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]))
        settings = self.game_settings()
        if isinstance(self.widgets.get("resolution"), Segmented):
            self.widgets["resolution"].set(settings["resolution"])
        if isinstance(self.widgets.get("multithreaded"), Toggle):
            self.widgets["multithreaded"].set(settings["multithreaded"])
        if isinstance(self.widgets.get("mouse"), Toggle):
            self.widgets["mouse"].set(settings.get("mouse", True))
            self.widgets["mouse_sens"].set(settings.get("mouse_sens", "1.0"))
            self.widgets["mouse_invert"].set(settings.get("mouse_invert", False))
        if hasattr(self, "binding_slots"):
            self.refresh_bindings()
        self.sync_sharpening()
        if hasattr(self, "preset_cards"):
            self.redraw_presets()
        self.update_fps_note()
        if hasattr(self, "ini_label"):
            self.ini_label.config(text=f"Settings: {ini_file(self.config['game'])}")

    def game_dir(self):
        folder = self.config.get("game_dir") or ""
        return Path(folder) if folder else DEFAULT_GAME_DIR

    def choose_game_dir(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(
            title="Select the Uncharted game folder (the one with eboot.bin and sce_sys)",
            initialdir=str(self.game_dir() if self.game_dir().exists() else ROOT))
        if not folder:
            return False
        if not is_game_dir(folder):
            messagebox.showerror(APP_TITLE, "That folder has no eboot.bin and sce_sys/param.sfo.\n\n"
                                            "Choose the game's root folder (CUSA02344).")
            return False
        self.config["game_dir"] = folder
        write_config(self.config)
        if hasattr(self, "game_dir_label"):
            self.game_dir_label.config(text=f"Game files: {folder}")
            self.update_label.config(text=self.update_text())
        return True

    def update_text(self):
        update = game_update_dir(self.game_dir())
        if update:
            return f"Game update {app_version(update) or '?'} is used: {update}"
        folder = self.game_dir()
        title = folder.parent if folder.name.lower() == "uroot" else folder
        return (f"Game version {app_version(folder) or '?'}, no update found. For the 1.02 fixes, "
                f"extract the update next to the game folder as {title.name}-patch.")

    def current_game(self):
        return next(g for g in GAMES if g["key"] == self.config.get("game"))

    def build_tabs(self):
        outer = tk.Frame(self.root, bg=BG)
        outer.pack(side="top", fill="both", expand=True, padx=self.px(40))
        strip = tk.Frame(outer, bg=BG)
        strip.pack(side="top", fill="x")
        self.tab_buttons = {}
        for name, label in (("setup", "QUICK SETUP"), ("display", "DISPLAY"), ("performance", "PERFORMANCE"),
                            ("controls", "CONTROLS"), ("advanced", "ADVANCED")):
            cell = tk.Frame(strip, bg=BG)
            cell.pack(side="left", padx=(0, self.px(26)))
            text = tk.Label(cell, text=label, font=self.f_tab, bg=BG, fg=MUTED, cursor="hand2")
            text.pack(side="top", pady=(0, self.px(6)))
            bar = tk.Frame(cell, bg=BG, height=max(2, self.px(2)))
            bar.pack(side="top", fill="x")
            for widget in (cell, text):
                widget.bind("<Button-1>", lambda e, n=name: self.show_tab(n))
            self.tab_buttons[name] = (text, bar)
        tk.Frame(outer, bg=LINE, height=1).pack(side="top", fill="x")
        self.body = tk.Frame(outer, bg=BG, height=self.px(330))
        self.body.pack(side="top", fill="both", expand=True, pady=(self.px(14), 0))
        self.body.pack_propagate(False)
        self.pages = {
            "setup": self.page_setup(),
            "display": self.page_display(),
            "performance": self.page_performance(),
            "controls": self.page_controls(),
            "advanced": self.page_advanced(),
        }

    def show_tab(self, name, save=True):
        if save and self.config.get("tab") != name:
            self.config["tab"] = name
            write_config(self.config)
        for tab, (text, bar) in self.tab_buttons.items():
            text.config(fg=TEXT if tab == name else MUTED)
            bar.config(bg=GOLD if tab == name else BG)
        for tab, page in self.pages.items():
            if tab == name:
                page.pack(fill="both", expand=True)
            else:
                page.pack_forget()

    # -- pages ------------------------------------------------------------------------------

    def page(self):
        return tk.Frame(self.body, bg=BG)

    def row(self, page, title, hint, control_factory):
        row = tk.Frame(page, bg=BG)
        row.pack(side="top", fill="x", pady=(0, self.px(10)))
        text = tk.Frame(row, bg=BG)
        text.pack(side="left", fill="x", expand=True)
        tk.Label(text, text=title, font=self.f_body_bold, bg=BG, fg=TEXT, anchor="w").pack(side="top", fill="x")
        if hint:
            tk.Label(text, text=hint, font=self.f_small, bg=BG, fg=MUTED, anchor="w", justify="left",
                     wraplength=self.px(560)).pack(side="top", fill="x")
        control = control_factory(row)
        control.pack(side="right", padx=(self.px(20), 0))
        return control

    def ini_toggle(self, page, key, title, hint):
        def factory(parent):
            return Toggle(parent, self.s, self.ini.get(key, "0") == "1",
                          lambda v, k=key: self.change(k, "1" if v else "0"))
        self.widgets[key] = self.row(page, title, hint, factory)

    def ini_choice(self, page, key, title, hint, options):
        def factory(parent):
            return Segmented(parent, options, self.ini.get(key, INI_DEFAULTS.get(key)), lambda v, k=key: self.change(k, v),
                             self.f_small)
        self.widgets[key] = self.row(page, title, hint, factory)

    def page_setup(self):
        page = self.page()
        tk.Label(page, text="Pick a quality level. Every setting can still be changed in the Display and Performance tabs.",
                 font=self.f_body, bg=BG, fg=MUTED, anchor="w").pack(side="top", fill="x", pady=(0, self.px(12)))
        grid = tk.Frame(page, bg=BG)
        grid.pack(side="top", fill="x")
        self.preset_cards = {}
        gap = self.px(16)
        card_w = (self.width - 2 * self.px(40) - 3 * gap) // 4
        card_h = self.px(150)
        for i, name in enumerate(PRESET_ORDER):
            canvas = tk.Canvas(grid, width=card_w, height=card_h, bg=BG, highlightthickness=0, cursor="hand2")
            canvas.grid(row=0, column=i, padx=(0 if i == 0 else gap, 0))
            canvas.bind("<Button-1>", lambda e, n=name: self.apply_preset(n))
            canvas.bind("<Enter>", lambda e, n=name: self.draw_preset(n, hover=True))
            canvas.bind("<Leave>", lambda e, n=name: self.draw_preset(n))
            self.preset_cards[name] = (canvas, card_w, card_h)
        self.custom_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=MUTED, anchor="w")
        self.custom_note.pack(side="top", fill="x", pady=(self.px(10), 0))
        self.root.after_idle(self.redraw_presets)
        return page

    def draw_preset(self, name, hover=False):
        canvas, w, h = self.preset_cards[name]
        preset = PRESETS[name]
        selected = self.game_settings().get("preset") == name
        canvas.delete("all")
        round_rect(canvas, 2, 2, w - 2, h - 2, self.px(10), fill=PANEL_HI if selected or hover else PANEL,
                   outline=GOLD if selected else ("#5a4733" if hover else LINE), width=2 if selected else 1)
        canvas.create_text(self.px(20), self.px(26), text=preset["title"], font=self.f_card_title,
                           fill=GOLD if selected else TEXT, anchor="w")
        canvas.create_text(self.px(20), self.px(50), text=preset["summary"], font=self.f_body_bold,
                           fill=TEXT, anchor="w")
        canvas.create_text(self.px(20), self.px(70), text=preset["detail"], font=self.f_small, fill=MUTED,
                           anchor="nw", width=w - self.px(40))

    def redraw_presets(self):
        for name in self.preset_cards:
            self.draw_preset(name)
        custom = self.game_settings().get("preset") not in PRESETS
        name = self.current_game()["name"]
        self.custom_note.config(text=f"{name}: custom settings (changed by hand)." if custom
                                else f"Settings are kept per game: these are {name}'s.")

    def apply_preset(self, name):
        settings = self.game_settings()
        settings["resolution"] = PRESETS[name]["resolution"]
        if "resolution" in self.widgets:
            self.widgets["resolution"].set(settings["resolution"])
        for key, value in preset_values(name).items():
            self.ini[key] = value
            widget = self.widgets.get(key)
            if isinstance(widget, Toggle):
                widget.set(value == "1")
            elif isinstance(widget, Segmented):
                widget.set(value)
        self.sync_sharpening()
        settings["preset"] = name
        write_config(self.config)
        self.save_ini({k: self.ini[k] for k in INI_DEFAULTS})
        self.redraw_presets()
        self.update_fps_note()

    def page_display(self):
        page = self.page()

        def resolution(parent):
            return Segmented(parent, RESOLUTIONS, self.game_settings()["resolution"],
                             self.change_resolution, self.f_small)
        self.widgets["resolution"] = self.row(
            page, "Render resolution",
            "The resolution the game draws at (the PS4's is 1080p). Above 1080p is new: if a game crashes "
            "there, use 1080p with FSR 1 below. Drake's Fortune draws at most 1620p and upscales.", resolution)
        self.ini_choice(page, "display_mode", "Display mode", "Windowed, borderless window or exclusive fullscreen.",
                        [("windowed", "Windowed"), ("borderless", "Borderless"), ("fullscreen", "Fullscreen")])
        self.ini_toggle(page, "vsync", "VSync", "No tearing. With frame rates above the refresh rate the newest "
                                               "frame is shown (no extra delay).")
        self.ini_toggle(page, "fsr1", "Upscale with FSR 1",
                        "When the game draws below your screen's resolution, FSR 1 upscales it sharper than plain scaling.")

        def sharpening(parent):
            value = self.ini.get("rcas_strength", "75") if self.ini.get("rcas", "1") == "1" else "off"
            return Segmented(parent, [("off", "Off"), ("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%")],
                             value, self.change_sharpening, self.f_small)
        self.widgets["sharpening"] = self.row(page, "Sharpening (FSR RCAS)", "Sharpens the final image.", sharpening)
        self.ini_toggle(page, "show_fps", "Show FPS counter", "A small performance overlay in a corner of the screen.")
        return page

    def change_sharpening(self, value):
        if value == "off":
            self.change("rcas", "0")
        else:
            self.ini["rcas"] = "1"
            self.change("rcas_strength", value)
            self.change("rcas", "1")

    def sync_sharpening(self):
        widget = self.widgets.get("sharpening")
        if widget:
            widget.set(self.ini.get("rcas_strength", "75") if self.ini.get("rcas", "1") == "1" else "off")

    def page_performance(self):
        page = self.page()
        self.ini_choice(page, "fps_limit", "Frame rate limit",
                        "60 is the PS4's. 120 unlocks the frame rate (experimental): if the game runs too fast "
                        "or Nate falls through the world, choose 60. Faster breaks the game's movement.",
                        [(str(v), label) for v, label in FPS_LIMITS])
        self.ini_choice(page, "frames_queued", "Frames queued", "1 (recommended). 2 is faster in heavy scenes, but "
                                                               "GPU results reach the game a frame later: in "
                                                               "Uncharted 2 Nate and NPCs then float above the floor.",
                        [("1", "1"), ("2", "2"), ("0", "Unlimited")])
        self.ini_toggle(page, "async_shaders", "Compile shaders in the background",
                        "Fewer stutters in new areas. Drake's Fortune always compiles them first (its lighting needs it).")

        def multithreaded(parent):
            return Toggle(parent, self.s, self.game_settings()["multithreaded"],
                          lambda v: self.change_game_setting("multithreaded", v))
        self.widgets["multithreaded"] = self.row(
            page, "Multithreaded rendering (experimental)",
            "Prepares draws on a second CPU thread: more FPS on CPUs with several cores. "
            "Turn it off if the game hangs while loading. Uncharted 2: use it with 1 frame queued "
            "(with 2 it can crash after a few minutes).", multithreaded)

        self.fps_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=GOLD, anchor="w")
        self.fps_note.pack(side="top", fill="x")
        self.update_fps_note()
        return page

    def update_fps_note(self):
        if not hasattr(self, "fps_note"):
            return
        limit = self.ini.get("fps_limit", "60")
        hz = monitor_refresh_hz()
        if int(limit) > 60:
            self.fps_note.config(text=f"Frame rate unlocked up to {limit} FPS (your monitor: {hz} Hz). "
                                      "Applied when the game starts.")
        else:
            self.fps_note.config(text="")

    def page_controls(self):
        page = self.page()
        settings = self.game_settings()
        top = tk.Frame(page, bg=BG)
        top.pack(side="top", fill="x")
        tk.Label(top, text="Mouse", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse"] = Toggle(top, self.s, settings.get("mouse", True),
                                       lambda v: self.change_game_setting("mouse", v))
        self.widgets["mouse"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(top, text="Sensitivity", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_sens"] = Segmented(top, MOUSE_SENSITIVITIES, settings.get("mouse_sens", "1.0"),
                                               lambda v: self.change_game_setting("mouse_sens", v), self.f_small)
        self.widgets["mouse_sens"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(top, text="Invert Y", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_invert"] = Toggle(top, self.s, settings.get("mouse_invert", False),
                                              lambda v: self.change_game_setting("mouse_invert", v))
        self.widgets["mouse_invert"].pack(side="left", padx=(self.px(8), 0))
        self.flat_button(top, "Reset controls", self.reset_controls).pack(side="right")
        self.controls_hint = tk.Label(
            page, text="Click a slot, then press a key or a mouse button (right-click a slot to clear it). "
                       "In game the mouse is captured; Alt+Tab or Insert (menu) frees it. A controller "
                       "always works too.", font=self.f_small, bg=BG, fg=MUTED, anchor="w", justify="left",
            wraplength=self.px(1080))
        self.controls_hint.pack(side="top", fill="x", pady=(self.px(6), self.px(4)))
        holder = tk.Frame(page, bg=BG)
        holder.pack(side="top", fill="both", expand=True)
        canvas = tk.Canvas(holder, bg=BG, highlightthickness=0)
        bar = tk.Scrollbar(holder, orient="vertical", command=canvas.yview)
        inner = tk.Frame(canvas, bg=BG)
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        def scroll(event):
            if self.capturing is None:
                canvas.yview_scroll(int(-event.delta / 120), "units")
        canvas.bind("<Enter>", lambda e: canvas.bind_all("<MouseWheel>", scroll))
        canvas.bind("<Leave>", lambda e: canvas.unbind_all("<MouseWheel>"))
        self.capturing = None
        self.binding_slots = {}
        columns = [tk.Frame(inner, bg=BG), tk.Frame(inner, bg=BG)]
        columns[0].grid(row=0, column=0, sticky="n", padx=(0, self.px(36)))
        columns[1].grid(row=0, column=1, sticky="n")
        for index, (group, actions) in enumerate(CONTROL_GROUPS):
            column = columns[0 if index < 2 else 1]
            tk.Label(column, text=group, font=self.f_small, bg=BG, fg=GOLD, anchor="w").pack(
                side="top", fill="x", pady=(self.px(6 if index % 2 else 2), self.px(2)))
            for action, label in actions:
                row = tk.Frame(column, bg=BG)
                row.pack(side="top", fill="x", pady=self.px(1))
                tk.Label(row, text=label, font=self.f_small, bg=BG, fg=TEXT, anchor="w",
                         width=26).pack(side="left")
                for slot in (0, 1):
                    cell = tk.Label(row, text="", font=self.f_small, bg=PANEL, fg=TEXT, width=14,
                                    pady=self.px(3), cursor="hand2", highlightthickness=1,
                                    highlightbackground=LINE)
                    cell.pack(side="left", padx=(self.px(6), 0))
                    cell.bind("<Button-1>", lambda e, a=action, i=slot: self.start_capture(a, i))
                    cell.bind("<Button-3>", lambda e, a=action, i=slot: self.set_binding(a, i, None))
                    self.binding_slots[(action, slot)] = cell
        self.refresh_bindings()
        return page

    def game_controls(self):
        return merged_controls(self.game_settings().get("controls"))

    def refresh_bindings(self):
        controls = self.game_controls()
        for (action, slot), cell in self.binding_slots.items():
            bindings = controls.get(action, [])
            capturing = self.capturing == (action, slot)
            cell.config(text="Press a key…" if capturing else binding_label(bindings[slot] if slot < len(bindings) else None),
                        bg=PANEL_HI if capturing else PANEL, fg=GOLD if capturing else TEXT)

    def start_capture(self, action, slot):
        self.stop_capture()
        self.capturing = (action, slot)
        self.refresh_bindings()
        self.root.focus_set()
        # After this click: the next key, mouse button or wheel step is the binding.
        self.root.after(200, self.arm_capture)

    def arm_capture(self):
        if self.capturing is None:
            return
        self.root.bind_all("<KeyPress>", self.capture_key)
        self.root.bind_all("<ButtonPress>", self.capture_button)
        self.root.bind_all("<MouseWheel>", self.capture_wheel)

    def stop_capture(self):
        if getattr(self, "capturing", None) is None:
            return
        self.capturing = None
        for sequence in ("<KeyPress>", "<ButtonPress>", "<MouseWheel>"):
            self.root.unbind_all(sequence)
        self.refresh_bindings()

    def capture_key(self, event):
        keysym = event.keysym
        if keysym in RESERVED_KEYS:
            self.controls_hint.config(text=f"{keysym} is used by the game window (Insert opens the menu, F9 "
                                           "records input, comma separates bindings). Choose another key.",
                                      fg=STOP_RED)
            self.stop_capture()
            return "break"
        name = TK_TO_SDL.get(keysym) or (keysym.upper() if len(keysym) == 1 and keysym.isalnum() else None)
        if not name:
            self.controls_hint.config(text=f"The key {keysym} can't be used. Choose another key.", fg=STOP_RED)
            self.stop_capture()
            return "break"
        action, slot = self.capturing
        self.set_binding(action, slot, f"key:{name}")
        return "break"

    def capture_button(self, event):
        names = {1: "left", 2: "middle", 3: "right", 4: "x1", 5: "x2"}
        if event.num in names:
            self.capture_mouse(names[event.num])
        return "break"

    def capture_wheel(self, event):
        self.capture_mouse("wheel_up" if event.delta > 0 else "wheel_down")
        return "break"

    def capture_mouse(self, name):
        action, slot = self.capturing
        if action in STICK_ACTIONS:
            self.controls_hint.config(text="Stick directions take keys; the mouse moves the camera by itself.",
                                      fg=STOP_RED)
            self.stop_capture()
            return
        self.set_binding(action, slot, f"mouse:{name}")

    def set_binding(self, action, slot, binding):
        controls = self.game_controls()
        if binding:
            # A key or button does one thing: it leaves the action it had.
            for other, bindings in controls.items():
                if binding in bindings and (other, bindings.index(binding)) != (action, slot):
                    bindings.remove(binding)
        bindings = controls[action]
        while len(bindings) <= slot:
            bindings.append(None)
        bindings[slot] = binding
        controls[action] = [b for b in bindings if b][:2]
        self.game_settings()["controls"] = controls
        write_config(self.config)
        self.controls_hint.config(
            text="Saved for " + self.current_game()["name"] + ". Click a slot, then press a key or a mouse button "
                 "(right-click a slot to clear it).", fg=MUTED)
        self.capturing = None
        for sequence in ("<KeyPress>", "<ButtonPress>", "<MouseWheel>"):
            self.root.unbind_all(sequence)
        self.refresh_bindings()

    def reset_controls(self):
        name = self.current_game()["name"]
        if not messagebox.askyesno(APP_TITLE, f"Reset {name}'s keyboard and mouse controls?"):
            return
        settings = self.game_settings()
        settings.update({"controls": None, "mouse": True, "mouse_sens": "1.0", "mouse_invert": False})
        write_config(self.config)
        self.refresh_controls()

    def page_advanced(self):
        page = self.page()

        def diagnostics(parent):
            return Toggle(parent, self.s, self.config.get("diagnostics", True),
                          lambda v: self.change_config("diagnostics", v))
        self.row(page, "Performance log", "Writes frame rate and GPU wait statistics to the log every 5 seconds, "
                                          "for troubleshooting. A very small cost.", diagnostics)

        def torch_forward(parent):
            return Toggle(parent, self.s, self.config.get("torch_forward", True),
                          lambda v: self.change_config("torch_forward", v))
        self.row(page, "Torch follows Nate (Uncharted 2)",
                 "The torch lights where Nate's torch points. Off: as on PS4, it aims where the camera "
                 "looks (turn the camera to face Nate and it lights the wall behind him). "
                 "Needs update 1.02.", torch_forward)

        def check_updates(parent):
            return Toggle(parent, self.s, self.config.get("check_updates", True),
                          lambda v: self.change_config("check_updates", v))
        version = updater.installed_version(ROOT)
        self.row(page, "Check for updates at start",
                 f"Installed: {updater.version_text(version) if version else 'source tree (updates with git)'}. "
                 "New versions come from the project's GitHub releases; the launcher asks before it "
                 "installs one.", check_updates)
        buttons = tk.Frame(page, bg=BG)
        buttons.pack(side="top", fill="x", pady=(self.px(6), self.px(10)))
        for label, action in (("Open log folder", self.open_logs), ("Open save folder", self.open_saves),
                              ("Desktop shortcut", self.desktop_shortcut), ("Clear shader cache", self.clear_cache),
                              ("Reset all settings", self.reset),
                              ("Check for updates", lambda: self.check_updates(manual=True))):
            self.flat_button(buttons, label, action).pack(side="left", padx=(0, self.px(10)))
        folder_row = tk.Frame(page, bg=BG)
        folder_row.pack(side="top", fill="x", pady=(0, self.px(4)))
        self.game_dir_label = tk.Label(folder_row, text=f"Game files: {self.game_dir()}", font=self.f_small,
                                       bg=BG, fg=DIM, anchor="w")
        self.game_dir_label.pack(side="left")
        self.flat_button(folder_row, "Change game folder", self.choose_game_dir).pack(
            side="left", padx=(self.px(10), 0))
        self.update_label = tk.Label(page, text=self.update_text(), font=self.f_small, bg=BG, fg=DIM,
                                     anchor="w", justify="left")
        self.update_label.pack(side="top", fill="x")
        self.ini_label = tk.Label(page, text=f"Settings: {ini_file(self.config['game'])}", font=self.f_small,
                                  bg=BG, fg=DIM, anchor="w")
        self.ini_label.pack(side="top", fill="x")
        tk.Label(page, text="In game: Insert or R3 + L2 opens the menu (display, frame rate, overlay).",
                 font=self.f_small, bg=BG, fg=DIM, anchor="w").pack(side="top", fill="x")
        return page

    def flat_button(self, parent, label, action):
        button = tk.Label(parent, text=label, font=self.f_small, bg=PANEL, fg=TEXT, padx=self.px(12), pady=self.px(6),
                          cursor="hand2", highlightthickness=1, highlightbackground=LINE)
        button.bind("<Button-1>", lambda e: action())
        button.bind("<Enter>", lambda e: button.config(bg=PANEL_HI))
        button.bind("<Leave>", lambda e: button.config(bg=PANEL))
        return button

    # -- footer -----------------------------------------------------------------------------

    def build_footer(self):
        footer = tk.Frame(self.root, bg=PANEL)
        footer.pack(side="bottom", fill="x")
        tk.Frame(self.root, bg=LINE, height=1).pack(side="bottom", fill="x")
        inner = tk.Frame(footer, bg=PANEL)
        inner.pack(fill="x", padx=self.px(40), pady=self.px(14))
        left = tk.Frame(inner, bg=PANEL)
        left.pack(side="left", fill="x", expand=True)
        self.status = tk.Label(left, text="", font=self.f_body_bold, bg=PANEL, fg=TEXT, anchor="w")
        self.status.pack(side="top", fill="x")
        self.substatus = tk.Label(left, text="Insert or R3 + L2 in game opens the menu.", font=self.f_small,
                                  bg=PANEL, fg=MUTED, anchor="w")
        self.substatus.pack(side="top", fill="x")
        w, h = self.px(210), self.px(54)
        self.play_button = tk.Canvas(inner, width=w, height=h, bg=PANEL, highlightthickness=0, cursor="hand2")
        self.play_button.pack(side="right")
        self.play_button.bind("<Button-1>", lambda e: self.play_or_stop())
        self.play_button.bind("<Enter>", lambda e: self.draw_play(hover=True))
        self.play_button.bind("<Leave>", lambda e: self.draw_play())
        self.draw_play()
        self.update_button = tk.Label(inner, text="UPDATE", font=self.f_body_bold, bg=PANEL_HI, fg=GOLD,
                                      padx=self.px(16), pady=self.px(10), cursor="hand2",
                                      highlightthickness=1, highlightbackground=GOLD)
        self.update_button.bind("<Button-1>", lambda e: self.install_update())
        self.set_status(f"Ready to play {self.current_game()['name']}." if self.config.get("game") else "Ready.")

    def draw_play(self, hover=False):
        canvas = self.play_button
        w, h = int(canvas["width"]), int(canvas["height"])
        canvas.delete("all")
        running = self.process is not None
        fill = (STOP_RED if running else (GOLD_HI if hover else GOLD))
        round_rect(canvas, 2, 2, w - 2, h - 2, self.px(8), fill=fill, outline=fill)
        canvas.create_text(w // 2, h // 2, text="STOP" if running else "PLAY", font=self.f_play,
                           fill=TEXT if running else INK)

    def set_status(self, text, detail=None):
        self.status.config(text=text)
        if detail is not None:
            self.substatus.config(text=detail)

    # -- updates (launcher/updater.py) ------------------------------------------------------

    def check_updates(self, manual=False):
        current = updater.installed_version(ROOT)
        if current is None:
            if manual:
                messagebox.showinfo(APP_TITLE, "This copy runs from the source tree: update it with git.")
            return
        if self.update_busy:
            return
        self.update_busy = True
        if manual:
            self.set_status("Checking for updates...", "")

        def work():
            try:
                self.update_result = (updater.latest_release(current), None)
            except Exception as error:  # offline, GitHub's rate limit
                self.update_result = (None, str(error) or error.__class__.__name__)
        threading.Thread(target=work, daemon=True).start()
        self.root.after(200, lambda: self.update_checked(manual))

    def update_checked(self, manual):
        if self.update_result is None:
            self.root.after(200, lambda: self.update_checked(manual))
            return
        release, error = self.update_result
        self.update_result = None
        self.update_busy = False
        if release:
            self.update_release = release
            version = updater.version_text(release["version"])
            self.update_button.config(text=f"UPDATE TO {version}")
            self.update_button.pack(side="right", padx=(0, self.px(14)))
            if not self.process:
                self.set_status(f"Version {version} is available.",
                                "UPDATE installs it: saves and settings are kept, the launcher restarts.")
        elif manual:
            current = updater.version_text(updater.installed_version(ROOT))
            if error:
                self.set_status("Could not check for updates.", error)
            else:
                self.set_status("No update found.", f"Version {current} is the newest.")

    def install_update(self):
        release = self.update_release
        if not release or self.update_busy:
            return
        if self.process:
            messagebox.showinfo(APP_TITLE, "Close the game first: the update replaces the game's runtime.")
            return
        version = updater.version_text(release["version"])
        if not messagebox.askyesno(APP_TITLE, f"Install version {version} ({release['size'] / 1e6:.0f} MB "
                                   "download)?\n\nSaves, settings and the shader cache are kept. The "
                                   "launcher closes, installs the update and starts again.\n\n"
                                   f"What's new: {release['page']}"):
            return
        self.update_busy = True
        self.update_progress = (0, release["size"])
        self.update_error = None
        self.update_staged = None

        def progress(done, total):
            self.update_progress = (done, total)

        def work():
            try:
                archive = updater.download(release, ROOT, progress)
                self.update_staged = updater.stage(archive, ROOT, release["version"])
            except Exception as error:
                self.update_error = str(error) or error.__class__.__name__
        threading.Thread(target=work, daemon=True).start()
        self.root.after(200, self.update_tick)

    def update_tick(self):
        if self.update_error:
            self.update_busy = False
            self.set_status("The update failed.", "The installed version is unchanged.")
            messagebox.showerror(APP_TITLE, f"The update could not be installed:\n\n{self.update_error}")
            return
        if self.update_staged:
            self.set_status("Installing the update...", "The launcher starts again in a moment.")
            try:
                updater.start_apply(ROOT, self.update_staged)
            except OSError as error:
                self.update_busy = False
                messagebox.showerror(APP_TITLE, f"The update could not be started:\n\n{error}")
                return
            self.config["window_xy"] = [self.root.winfo_x(), self.root.winfo_y()]
            write_config(self.config)
            self.root.destroy()
            return
        done, total = self.update_progress
        amount = f"{100 * done / total:.0f}%" if total else f"{done / 1e6:.0f} MB"
        self.set_status(f"Downloading the update... {amount}", "Keep the launcher open.")
        self.root.after(200, self.update_tick)

    # -- settings changes -------------------------------------------------------------------

    def change(self, key, value):
        self.ini[key] = value
        self.save_ini({key: value})
        if self.game_settings().get("preset") != "custom":
            self.game_settings()["preset"] = "custom"
            write_config(self.config)
            self.redraw_presets()
        if key == "fps_limit":
            self.update_fps_note()

    def change_resolution(self, value):
        self.game_settings()["resolution"] = value
        self.game_settings()["preset"] = "custom"
        write_config(self.config)
        self.redraw_presets()

    def change_game_setting(self, key, value):
        self.game_settings()[key] = value
        write_config(self.config)

    def change_config(self, key, value):
        self.config[key] = value
        write_config(self.config)

    # -- game -------------------------------------------------------------------------------

    def play_or_stop(self):
        if self.process:
            self.stop()
        else:
            self.play()

    def play(self):
        if not RUN_SCRIPT.exists():
            messagebox.showerror(APP_TITLE, f"Start script missing:\n{RUN_SCRIPT}")
            return
        if not is_game_dir(self.game_dir()):
            messagebox.showinfo(APP_TITLE, "Select your Uncharted: The Nathan Drake Collection game folder "
                                           "(CUSA02344, the folder with eboot.bin and sce_sys).")
            if not self.choose_game_dir():
                return
        self.follow_ini()  # the in-game menu's last changes, not older values kept here
        self.save_ini({k: self.ini.get(k, v) for k, v in INI_DEFAULTS.items()})
        write_config(self.config)
        game = self.current_game()
        env = os.environ.copy()
        env["UNCHARTED_DIAGNOSTICS"] = "1" if self.config.get("diagnostics", True) else "0"
        env["UNCHARTED_TORCH_FORWARD"] = "1" if self.config.get("torch_forward", True) else "0"
        # Every game's own options: the Collection menu starts the others itself, and
        # tools/probe_uncharted.py applies the started game's (its settings file decides the
        # vblank rate for frame rates above 60).
        env.pop("GOW3_VBLANK_HZ", None)
        for key in GAME_KEYS:
            settings = self.config["games"][key]
            env[f"UNCHARTED_RESOLUTION_{key.upper()}"] = settings["resolution"]
            env[f"UNCHARTED_MULTITHREADED_{key.upper()}"] = "1" if settings["multithreaded"] else "0"
            keys, mouse = control_maps(settings.get("controls"))
            env[f"UNCHARTED_KEY_MAP_{key.upper()}"] = keys
            env[f"UNCHARTED_MOUSE_MAP_{key.upper()}"] = mouse
            env[f"UNCHARTED_MOUSE_{key.upper()}"] = "1" if settings.get("mouse", True) else "0"
            env[f"UNCHARTED_MOUSE_SENS_{key.upper()}"] = settings.get("mouse_sens", "1.0")
            env[f"UNCHARTED_MOUSE_INVERT_{key.upper()}"] = "1" if settings.get("mouse_invert") else "0"
        python = Path(sys.executable)
        if python.name.lower() == "pythonw.exe" and python.with_name("python.exe").exists():
            python = python.with_name("python.exe")
        command = [str(python), "-u", str(RUN_SCRIPT), "--boot", "--seconds", "0", "--executable", game["exe"],
                   "--game", str(self.game_dir())]
        # The game window may take the foreground (Windows only lets the foreground process do
        # that; the game starts two processes down): fullscreen and keyboard input need the focus.
        try:
            ctypes.windll.user32.AllowSetForegroundWindow(-1)  # ASFW_ANY
        except (AttributeError, OSError):
            pass
        try:
            self.process = subprocess.Popen(command, cwd=str(ROOT), env=env, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                            creationflags=NO_WINDOW, text=True, encoding="utf-8", errors="replace")
        except OSError as error:
            messagebox.showerror(APP_TITLE, f"Could not start the game:\n{error}")
            self.process = None
            return
        self.started_at = time.time()
        self.last_output = time.time()
        self.game_started = False
        # The start script's output (preparation, patches, the runtime's log path): kept for
        # "the game does not start" reports.
        try:
            self.run_log = open(USER_DIR / "launcher-last-run.log", "w", encoding="utf-8")
            self.run_log.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {' '.join(command)}\n")
        except OSError:
            self.run_log = None
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.set_status(f"Starting {game['name']}...", "Preparing game files and code patches.")
        self.draw_play()

    def read_output(self, process):
        log = getattr(self, "run_log", None)
        for line in process.stdout:
            self.output.put(line.rstrip())
            if log:
                try:
                    log.write(line)
                    log.flush()
                except (OSError, ValueError):
                    log = None
        if log:
            try:
                log.write(f"exit code {process.wait()}\n")
                log.close()
            except (OSError, ValueError):
                pass

    def poll(self):
        while True:
            try:
                line = self.output.get_nowait()
            except queue.Empty:
                break
            self.last_output = time.time()
            if line.startswith("Collection: switching to"):
                target = line.split("switching to", 1)[1].strip().split()[0]
                name = next((g["name"] for g in GAMES if g["exe"] == target), target)
                self.set_status(f"Switching to {name}...", "The Collection is starting another game.")
                self.game_started = False
        self.poll_count = getattr(self, "poll_count", 0) + 1
        if self.poll_count % 7 == 0:  # about once a second
            self.follow_ini()
        if self.process:
            if self.process.poll() is not None:
                self.finished(self.process.returncode)
            elif not self.game_started and time.time() - self.last_output > 2.5:
                self.game_started = True
                self.set_status(f"Playing {self.current_game()['name']}",
                                "Insert or R3 + L2 opens the in-game menu. STOP closes the game.")
        self.root.after(150, self.poll)

    def latest_log(self):
        logs = [ROOT / "analysis" / g["log"] / "boot.log" for g in GAMES]
        logs = [log for log in logs if log.exists()]
        return max(logs, key=lambda p: p.stat().st_mtime) if logs else None

    def crash_reason(self):
        log = self.latest_log()
        if not log or log.stat().st_mtime < self.started_at:
            return None
        try:
            tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-80:]
        except OSError:
            return None
        for i, line in enumerate(tail):
            if line.startswith("STOP:") or "Unreachable code" in line or "Assertion" in line:
                context = [l for l in tail[max(0, i - 2):i + 1] if l.strip()]
                return "\n".join(context)[-600:]
        return None

    def finished(self, code):
        self.process = None
        self.draw_play()
        if code in (0, None) or getattr(self, "stopping", False):
            self.stopping = False
            self.set_status("Game closed.", "Ready to play again.")
            return
        reason = self.crash_reason()
        self.set_status("The game stopped unexpectedly.", f"Exit code {code}. See the log for details.")
        if reason:
            if messagebox.askyesno(APP_TITLE, f"The game stopped with an error:\n\n{reason}\n\nOpen the log folder?"):
                self.open_logs()

    def stop(self):
        if not self.process:
            return
        self.stopping = True
        subprocess.run(["taskkill", "/PID", str(self.process.pid), "/T", "/F"], creationflags=NO_WINDOW,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.set_status("Stopping the game...")

    # -- advanced actions -------------------------------------------------------------------

    def open_logs(self):
        log = self.latest_log()
        folder = log.parent if log else ROOT / "analysis"
        os.startfile(folder)

    def open_saves(self):
        folder = USER_DIR / "savedata"
        os.startfile(folder if folder.exists() else USER_DIR)

    def clear_cache(self):
        cache = USER_DIR / "cache"
        if not cache.exists():
            messagebox.showinfo(APP_TITLE, "There is no shader cache yet.")
            return
        if self.process:
            messagebox.showwarning(APP_TITLE, "Close the game first.")
            return
        if not messagebox.askyesno(APP_TITLE, "Delete the shader cache?\n\nShaders are compiled again the next "
                                              "time you play, which brings back stutters for a while."):
            return
        shutil.rmtree(cache, ignore_errors=True)
        messagebox.showinfo(APP_TITLE, "Shader cache cleared.")

    def desktop_shortcut(self):
        desktop = Path(os.environ.get("USERPROFILE", Path.home())) / "Desktop"
        link = desktop / "Uncharted.lnk"
        pythonw = Path(sys.executable).with_name("pythonw.exe")
        target = pythonw if pythonw.exists() else Path(sys.executable)
        arguments = f"\"{Path(__file__).resolve()}\""
        if STARTER_EXE.exists():
            target, arguments = STARTER_EXE, ""
        script = (f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut('{link}');"
                  f"$s.TargetPath='{target}';$s.Arguments='{arguments}';"
                  f"$s.WorkingDirectory='{ROOT}';$s.IconLocation='{ASSETS / 'uncharted.ico'}';"
                  f"$s.Description='{APP_TITLE}';$s.Save()")
        result = subprocess.run(["powershell", "-NoProfile", "-Command", script], creationflags=NO_WINDOW,
                                capture_output=True, text=True)
        if result.returncode == 0:
            messagebox.showinfo(APP_TITLE, f"Shortcut created:\n{link}")
        else:
            messagebox.showerror(APP_TITLE, f"Could not create the shortcut:\n{result.stderr.strip()}")

    def reset(self):
        name = self.current_game()["name"]
        if not messagebox.askyesno(APP_TITLE, f"Reset {name}'s settings to the Medium preset?"):
            return
        self.game_settings().update(GAME_SETTING_DEFAULTS)
        self.ini.update(INI_DEFAULTS)
        self.refresh_controls()
        self.apply_preset("medium")

    def close(self):
        if self.process and not messagebox.askyesno(
                APP_TITLE, "The game is still running. Close the launcher anyway? (The game keeps running.)"):
            return
        self.config["window_xy"] = [self.root.winfo_x(), self.root.winfo_y()]
        write_config(self.config)
        self.root.destroy()


def main():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    # Its own taskbar identity: without it the launcher's window is grouped under Python
    # (pythonw.exe) and the taskbar shows Python's icon instead of Uncharted's.
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("BilalReloaded.Uncharted.Launcher")
    except Exception:
        pass
    root = tk.Tk()
    Launcher(root)
    root.mainloop()


if __name__ == "__main__":
    main()
