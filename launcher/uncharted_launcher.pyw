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
from tkinter import messagebox

from PIL import Image, ImageDraw, ImageTk

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

GAMES = [
    {"key": "u1", "number": "1", "title": "DRAKE'S FORTUNE", "name": "Uncharted: Drake's Fortune", "subtitle": "Opens the Collection menu",
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

FPS_LIMITS = [(30, "30"), (60, "60"), (120, "120"), (144, "144"), (165, "165"), (240, "240"), (0, "Unlimited")]

# Runtime options in probe.ini (key -> default) and launcher-only ones.
INI_DEFAULTS = {
    "display_mode": "windowed", "vsync": "1", "fps_limit": "60", "frames_queued": "1",
    "async_shaders": "1", "fsr1": "0", "rcas": "1", "rcas_strength": "75", "show_fps": "1",
}
LAUNCHER_DEFAULTS = {"game": "u1", "diagnostics": True, "game_dir": ""}
GAME_KEYS = ("u1", "u2", "u3")
GAME_SETTING_DEFAULTS = {"preset": "medium", "resolution": "1920x1080", "multithreaded": False}


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


def high_end_fps_limit():
    """The highest listed limit the monitor shows, at least 120."""
    hz = monitor_refresh_hz()
    best = 120
    for value, _ in FPS_LIMITS:
        if value and 120 <= value <= hz:
            best = value
    return best


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
                  "scales it up to the screen. Two frames queued keep the frame rate even.",
        "resolution": "1280x720",
        "values": {"fps_limit": "60", "frames_queued": "2", "vsync": "1", "async_shaders": "1",
                   "fsr1": "1", "rcas": "1", "rcas_strength": "75"},
    },
    "medium": {
        "title": "MEDIUM", "summary": "60 FPS  •  as on PS4",
        "detail": "The PS4 original: 1080p at 60 FPS with the lowest input delay. Right for most PCs.",
        "resolution": "1920x1080",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "0", "rcas": "1", "rcas_strength": "75"},
    },
    "high": {
        "title": "HIGH", "summary": "60 FPS  •  1440p",
        "detail": "Sharper: the game draws at 1440p (new, confirm in play). On 4K monitors FSR 1 scales "
                  "it up to the screen.",
        "resolution": "2560x1440",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": None, "rcas": "1", "rcas_strength": "50"},
    },
    "ultra": {
        "title": "ULTRA", "summary": "4K  •  above 60 FPS",
        "detail": "Native 4K (Drake's Fortune: 1620p + FSR 1) with the frame rate unlocked up to your "
                  "monitor's refresh rate. New, confirm in play; needs a fast GPU.",
        "resolution": "3840x2160",
        "values": {"fps_limit": None, "frames_queued": "2", "vsync": "1", "async_shaders": "1",
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
    if values["fps_limit"] is None:
        values["fps_limit"] = str(high_end_fps_limit())
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
    return values


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
    for name in GAME_SETTING_DEFAULTS:
        config.pop(name, None)
    config["version"] = 4
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
        self.ini = read_ini(self.config["game"])
        self.process = None
        self.output = queue.Queue()
        self.last_output = 0.0
        self.game_started = False
        self.widgets = {}
        self.images = []
        self.fonts()
        root.title(f"{APP_TITLE} - Launcher")
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
            self.ini = read_ini(key)
            self.refresh_controls()
        if hasattr(self, "status") and not self.process:
            self.set_status(f"Ready to play {self.current_game()['name']}.")

    def game_settings(self):
        """Preset, render resolution and multithreading of the selected game."""
        return self.config["games"][self.config["game"]]

    def save_ini(self, updates):
        write_ini(self.config["game"], updates)

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
                            ("advanced", "ADVANCED")):
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
                        "Above 60 the frame rate is unlocked (experimental): if the game runs too fast, choose 60.",
                        [(str(v), label) for v, label in FPS_LIMITS])
        self.ini_choice(page, "frames_queued", "Frames queued", "1 has the lowest input delay; 2 keeps the frame "
                                                               "rate steadier on slower PCs.",
                        [("1", "1"), ("2", "2"), ("0", "Unlimited")])
        self.ini_toggle(page, "async_shaders", "Compile shaders in the background",
                        "Fewer stutters in new areas. Drake's Fortune always compiles them first (its lighting needs it).")

        def multithreaded(parent):
            return Toggle(parent, self.s, self.game_settings()["multithreaded"],
                          lambda v: self.change_game_setting("multithreaded", v))
        self.widgets["multithreaded"] = self.row(
            page, "Multithreaded rendering (experimental)",
            "Prepares draws on a second CPU thread: more FPS on CPUs with several cores. "
            "Turn it off if the game hangs while loading.", multithreaded)
        self.fps_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=GOLD, anchor="w")
        self.fps_note.pack(side="top", fill="x")
        self.update_fps_note()
        return page

    def update_fps_note(self):
        if not hasattr(self, "fps_note"):
            return
        limit = self.ini.get("fps_limit", "60")
        hz = monitor_refresh_hz()
        if limit in ("0",) or int(limit or 60) > 60:
            self.fps_note.config(text=f"Frame rate unlocked (your monitor: {hz} Hz). Applied when the game starts.")
        else:
            self.fps_note.config(text="")

    def page_advanced(self):
        page = self.page()

        def diagnostics(parent):
            return Toggle(parent, self.s, self.config.get("diagnostics", True),
                          lambda v: self.change_config("diagnostics", v))
        self.row(page, "Performance log", "Writes frame rate and GPU wait statistics to the log every 5 seconds, "
                                          "for troubleshooting. A very small cost.", diagnostics)
        buttons = tk.Frame(page, bg=BG)
        buttons.pack(side="top", fill="x", pady=(self.px(6), self.px(10)))
        for label, action in (("Open log folder", self.open_logs), ("Open save folder", self.open_saves),
                              ("Desktop shortcut", self.desktop_shortcut), ("Clear shader cache", self.clear_cache),
                              ("Reset all settings", self.reset)):
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
        self.save_ini({k: self.ini.get(k, v) for k, v in INI_DEFAULTS.items()})
        write_config(self.config)
        game = self.current_game()
        env = os.environ.copy()
        env["UNCHARTED_DIAGNOSTICS"] = "1" if self.config.get("diagnostics", True) else "0"
        # Every game's own options: the Collection menu starts the others itself, and
        # tools/probe_uncharted.py applies the started game's (its settings file decides the
        # vblank rate for frame rates above 60).
        env.pop("GOW3_VBLANK_HZ", None)
        for key in GAME_KEYS:
            settings = self.config["games"][key]
            env[f"UNCHARTED_RESOLUTION_{key.upper()}"] = settings["resolution"]
            env[f"UNCHARTED_MULTITHREADED_{key.upper()}"] = "1" if settings["multithreaded"] else "0"
        python = Path(sys.executable)
        if python.name.lower() == "pythonw.exe" and python.with_name("python.exe").exists():
            python = python.with_name("python.exe")
        command = [str(python), "-u", str(RUN_SCRIPT), "--boot", "--seconds", "0", "--executable", game["exe"],
                   "--game", str(self.game_dir())]
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
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.set_status(f"Starting {game['name']}...", "Preparing game files and code patches.")
        self.draw_play()

    def read_output(self, process):
        for line in process.stdout:
            self.output.put(line.rstrip())

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
    root = tk.Tk()
    Launcher(root)
    root.mainloop()


if __name__ == "__main__":
    main()
