#!/usr/bin/env python3
"""Uncharted: The Nathan Drake Collection - launcher (Windows, Tkinter + Pillow).

Pick a game, set display and performance options (presets for low-end and high-end PCs) and play.
Runtime options are kept in analysis/uncharted-user/probe.ini, the same file the in-game menu
(Insert / R3+L2) reads and writes; launcher-only options in %APPDATA%/Uncharted Launcher (under
Wine/Proton in ~/.local/share/uncharted-launcher, the same for every prefix).
The game is started through tools/probe_uncharted.py (preparation, code patches, Collection game
switches), with the options below passed as environment variables.
"""
import ctypes
import json
import math
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
from tkinter import messagebox

from PIL import Image, ImageDraw, ImageTk

import native_api  # launcher/native_api.py: the launcher's native code (uncharted_native.dll)
import updater  # launcher/updater.py: updates from the GitHub releases

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "launcher" / "assets"
DEFAULT_GAME_DIR = ROOT / "game" / "CUSA02344" / "uroot"
STARTER_EXE = ROOT / "Uncharted Launcher.exe"  # the packaged build's starter (tools/package_windows.py)
USER_DIR = ROOT / "analysis" / "uncharted-user"
RUN_SCRIPT = ROOT / "tools" / "probe_uncharted.py"
PREFIX_CONFIG_FILE = Path(os.environ.get("APPDATA", Path.home())) / "Uncharted Launcher" / "launcher.json"


# Wine detection and paths, and the drives that make the games freeze, are the native code's
# (launcher/native/system.cpp).
WINE = native_api.wine()


def slow_drive_reason(path):
    """Why the drive holding `path` makes the games freeze, or None (20 Oct 2026). On a laptop the port and game on an
    external USB drive (an NVMe SSD in a USB case, exFAT) froze for 0.2-0.8 s dozens of times a minute while the port
    wrote its shader cache, logs and saves; reports USB, SD/MMC and removable drives and FAT/exFAT volumes; None
    elsewhere, under Wine and for network paths."""
    return native_api.system("slow_drive", path)


def unix_path(path):
    """The Linux path of a Windows path under Wine, else None."""
    return native_api.system("unix_path", path)


def windows_path(path):
    """The Windows path of a Linux path under Wine (in this prefix's drive letters), else None."""
    return native_api.system("windows_path", path) if path else None


def shared_config_file():
    """Under Wine: launcher.json in the Linux user's data folder (the same for every Steam shortcut's prefix)."""
    folder = native_api.system("shared_config")
    return Path(folder) if folder else None


CONFIG_FILE = (WINE and shared_config_file()) or PREFIX_CONFIG_FILE
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
APP_TITLE = "Uncharted: The Nathan Drake Collection"
# Credits shown on the Advanced page: the anti-aliasing technologies.
CREDITS = ("Credits: DLSS and DLAA use NVIDIA DLSS technology. NVIDIA, RTX and DLSS are trademarks of "
           "NVIDIA Corporation. AMD FidelityFX Super Resolution (FSR 3.1, FSR 4) by AMD. Uncharted camera "
           "motion for anti-aliasing after SuperMedo's Uncharted port.")
# Uncharted 2 multiplayer (the Multiplayer tab while Uncharted 2 is chosen): u2mp/patcher makes its files
# from the player's own game into U2MP_DIR; nothing game-derived ships with the launcher.
U2MP_DIR = USER_DIR / "u2mp"


# Uncharted 3 multiplayer (the Multiplayer tab while Uncharted 1 or 3 is chosen): u3mp/patcher holds the
# import (u3mp_import.py), the overlay recipe and the helper builds; everything game-derived is made on the
# player's PC in the multiplayer data folder.
U3MP_DIR = ROOT / "u3mp" / "patcher"


def launcher_version():
    """The launcher release (VERSION next to the launcher in a package, tools/VERSION in the source tree)."""
    return native_api.system("launcher_version", ROOT)


def recipe_version():
    """The Uncharted 3 multiplayer recipe shipped with the launcher (u3mp/patcher/recipe/VERSION)."""
    return native_api.system("recipe_version", U3MP_DIR)


def version_suffix(imported=""):
    """' - Unofficial Port - Launcher Version: x - Recipe Version: y[ - Imported vZ]', shown after the launcher's and
    the games' window titles; Z is the Uncharted 3 multiplayer build of the current import (state.json image version)."""
    tail = f" - Imported v{imported}" if imported else ""
    return f" - Unofficial Port - Launcher Version: {launcher_version()} - Recipe Version: {recipe_version()}{tail}"


def u3mp_default_data():
    return Path(native_api.system("u3mp_default_data"))


def u3mp_release():
    """The newest multiplayer build shipped with the launcher (u3mp/patcher/build/v<version>/build.json)."""
    text = native_api.system("u3mp_release", U3MP_DIR)
    return json.loads(text) if text else None


def pid_alive(pid):
    """Whether a process with this id still runs (Windows)."""
    return native_api.pid_alive(pid)


# The games, the cheats (Advanced page: name, title, hint; tools/probe_uncharted.py turns on the games' own switches
# for them) and the PS4 system languages the Collection has text for (id as sceSystemServiceParamGetInt reports it,
# name) are the native code's (launcher/native/system.cpp), as are the speech packs a copy of the game has.
globals().update(native_api.system_tables())

# Palette: near black with the Collection's yellow-gold accent. Pages group their rows in rounded
# cards (CARD); controls inside a card sit on FIELD.
BG = "#0c0c0e"
PANEL = "#141417"
PANEL_HI = "#1e1e23"
CARD = "#151519"
CARD_LINE = "#25252c"
FIELD = "#222228"
FIELD_HI = "#2c2c34"
LINE = "#2f2f37"
TEXT = "#f2efe8"
MUTED = "#a3a09a"
DIM = "#6c6a70"
GOLD = "#e8b23e"
GOLD_HI = "#f7c75e"
INK = "#16110a"
STOP_RED = "#c4553f"

# At most 120: faster, the game's movement timing breaks (Drake's Fortune at 140-160 FPS: running
# slows, Nate fell through the world). The runtime caps older, faster settings at 120 too.
# The frame-rate limits, the runtime settings' defaults and the presets are the native settings core's
# (launcher/native/config.cpp).
FPS_LIMITS = native_api.fps_limits()
MAX_FPS = FPS_LIMITS[-1][0]
INI_DEFAULTS = native_api.ini_defaults()
LAUNCHER_DEFAULTS = {"game": "u1", "diagnostics": True, "game_dir": "", "check_updates": True, "language": "1",
                     "unlock": False}

def voice_languages(game_dir):
    """Language ids with speech in this copy of the game (its *_ps4.psarc speech packs)."""
    return set((native_api.system("voices", game_dir) or "").split()) if game_dir else set()


GAME_KEYS = ("u1", "u2", "u3")
GAME_SETTING_DEFAULTS = {"preset": "medium", "resolution": "1920x1080", "aspect": "16:9", "multithreaded": False, "save_profile": "1",
                         "async_readback": False, "controls": None, "mouse": True, "mouse_sens": "1.0",
                         "mouse_invert": False, "prompts": "auto", "mouse_aim_sens": "1.0",
                         "mouse_vertical": "1.0", "mouse_invert_x": False, "vibration": True}

# Keyboard and mouse controls (runtime_pad.c): action -> up to two bindings, "key:<SDL key name>"
# or "mouse:<left|middle|right|x1|x2|wheel_up|wheel_down>". Stick directions take keys only.
# The Controls page's tables are the native code's (launcher/native/controls.cpp): the actions and their groups,
# the controller buttons (defaults, fixed ones, SDL order, names), the default key and mouse bindings, the prompt
# modes, the mouse steps, mouse button labels, Tk -> SDL key names and the keys the runtime keeps for itself.
globals().update(native_api.controls_tables())


def merged_pad_controls(pad_controls):
    return native_api.controls("merged_pad", pad_controls)


def _pad_font(size):
    from PIL import ImageFont
    for name in ("segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def pad_icon(button, style, height):
    """A controller button's icon (PIL RGBA image, `height` pixels tall): Xbox or PlayStation."""
    s = 4  # drawn 4x, then scaled down (smooth edges)
    H = height * s
    ps = style == "playstation"
    body, rim, ink = (32, 32, 34, 255), (150, 150, 150, 255), (240, 240, 240, 255)
    if "+" in button:  # a combination: the icons side by side with a plus
        parts = [pad_icon(b, style, height) for b in button.split("+")]
        plus_w = height // 2
        img = Image.new("RGBA", (sum(i.width for i in parts) + plus_w * (len(parts) - 1), height))
        x = 0
        for i, part in enumerate(parts):
            img.alpha_composite(part, (x, 0))
            x += part.width
            if i + 1 < len(parts):
                d = ImageDraw.Draw(img)
                cx, cy, r = x + plus_w // 2, height // 2, max(2, height // 6)
                d.line((cx - r, cy, cx + r, cy), fill=ink, width=max(1, height // 12))
                d.line((cx, cy - r, cx, cy + r), fill=ink, width=max(1, height // 12))
                x += plus_w
        return img

    def canvas(width_factor):
        W = int(H * width_factor)
        return Image.new("RGBA", (W, H)), W

    def finish(img):
        return img.resize((max(1, img.width // s), height), Image.LANCZOS)

    def text(d, W, label, color, size_factor=0.55, dy=0):
        font = _pad_font(max(8, int(H * size_factor)))
        box = d.textbbox((0, 0), label, font=font)
        d.text(((W - (box[2] - box[0])) / 2 - box[0], (H - (box[3] - box[1])) / 2 - box[1] + dy), label,
               font=font, fill=color)

    line = max(2, H // 12)
    if button in ("a", "b", "x", "y"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((line, line, W - line, H - line), fill=body, outline=rim, width=line)
        if ps:
            color = {"a": (124, 156, 255, 255), "b": (255, 102, 102, 255), "x": (240, 140, 210, 255),
                     "y": (64, 214, 160, 255)}[button]
            c, r, w = W / 2, H * 0.22, int(H * 0.08)
            if button == "a":
                d.line((c - r, c - r, c + r, c + r), fill=color, width=w)
                d.line((c - r, c + r, c + r, c - r), fill=color, width=w)
            elif button == "b":
                d.ellipse((c - r, c - r, c + r, c + r), outline=color, width=w)
            elif button == "x":
                d.rectangle((c - r * 0.85, c - r * 0.85, c + r * 0.85, c + r * 0.85), outline=color, width=w)
            else:
                d.polygon([(c, c - r * 1.05), (c - r * 1.1, c + r * 0.75), (c + r * 1.1, c + r * 0.75)],
                          outline=color, width=w)
        else:
            color = {"a": (108, 194, 74, 255), "b": (229, 71, 59, 255), "x": (63, 142, 230, 255),
                     "y": (244, 196, 48, 255)}[button]
            text(d, W, button.upper(), color, 0.6)
        return finish(img)
    if button in ("leftshoulder", "rightshoulder"):
        img, W = canvas(1.55)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((line, H * 0.14, W - line, H * 0.86), radius=H * 0.3, fill=body, outline=rim, width=line)
        text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.46)
        return finish(img)
    if button in ("lefttrigger", "righttrigger"):
        img, W = canvas(1.25)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((line, line, W - line, H - line), radius=H * 0.32, fill=body, outline=rim, width=line)
        d.rectangle((line * 2, H * 0.55, W - line * 2, H - line * 2), fill=body)
        text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.44)
        return finish(img)
    if button in ("leftstick", "rightstick", "lstick", "rstick"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((line, line, W - line, H - line), fill=body, outline=rim, width=line)
        if button in ("lstick", "rstick"):
            d.ellipse((H * 0.24, H * 0.24, H * 0.76, H * 0.76), outline=rim, width=line)
            text(d, W, "L" if button == "lstick" else "R", ink, 0.38)
        else:
            text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.4)
        return finish(img)
    if button.startswith("dp"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        a, b = H * 0.31, H * 0.69
        arms = {"dpup": (a, line, b, a + line), "dpdown": (a, b - line, b, H - line), "dpleft": (line, a, a + line, b),
                "dpright": (b - line, a, H - line, b)}
        d.rectangle((a, line, b, H - line), fill=body, outline=rim, width=line)
        d.rectangle((line, a, H - line, b), fill=body, outline=rim, width=line)
        d.rectangle((a + line, a, b - line, b), fill=body)
        d.rectangle((a, a + line, b, b - line), fill=body)
        x0, y0, x1, y1 = arms[button]
        d.rectangle((x0 + line, y0 + line, x1 - line, y1 - line), fill=(250, 200, 90, 255))
        return finish(img)
    if button in ("start", "back"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((H * 0.1, H * 0.1, H * 0.9, H * 0.9), fill=body, outline=rim, width=line)
        c = H / 2
        if button == "start":  # three lines: Xbox Menu, PlayStation Options
            for k in (-1, 0, 1):
                y = c + k * H * 0.13
                d.line((c - H * 0.17, y, c + H * 0.17, y), fill=ink, width=line)
        elif ps:  # Share/Create: a small arrow up
            d.line((c, c - H * 0.18, c, c + H * 0.16), fill=ink, width=line)
            d.line((c - H * 0.12, c - H * 0.06, c, c - H * 0.18, c + H * 0.12, c - H * 0.06), fill=ink, width=line)
        else:  # View: two windows
            d.rectangle((c - H * 0.17, c - H * 0.15, c + H * 0.05, c + H * 0.05), outline=ink, width=line)
            d.rectangle((c - H * 0.05, c - H * 0.03, c + H * 0.17, c + H * 0.17), fill=ink)
        return finish(img)
    # Anything else (touchpad, paddles, misc): its name on a rounded key.
    label = PAD_NAMES.get(button, (button, button))[1 if ps else 0]
    img, W = canvas(max(1.0, 0.36 * len(label)))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((line, H * 0.14, W - line, H * 0.86), radius=H * 0.25, fill=body, outline=rim, width=line)
    text(d, W, label, ink, 0.4)
    return finish(img)


class Gamepads:
    """Controllers through SDL3, the game's own library (gow3_pc/out in a release, MSYS2 here):
    which buttons are held, and whether the first one is a PlayStation controller."""
    PLAYSTATION_TYPES = (4, 5, 6)  # SDL_GAMEPAD_TYPE_PS3, PS4, PS5

    def __init__(self):
        self.sdl = None
        self.pads = {}
        msys = Path(os.environ.get("MSYS2_ROOT", "C:/msys64"))
        for path in (ROOT / "gow3_pc/out/SDL3.dll", msys / "clang64/bin/SDL3.dll", ROOT.parent / "msys64/clang64/bin/SDL3.dll"):
            if not path.is_file():
                continue
            try:
                sdl = ctypes.CDLL(str(path))
            except OSError:
                continue
            sdl.SDL_SetHint.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            sdl.SDL_SetHint.restype = ctypes.c_bool
            sdl.SDL_Init.argtypes = [ctypes.c_uint32]
            sdl.SDL_Init.restype = ctypes.c_bool
            sdl.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
            sdl.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_uint32)
            sdl.SDL_OpenGamepad.argtypes = [ctypes.c_uint32]
            sdl.SDL_OpenGamepad.restype = ctypes.c_void_p
            sdl.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
            sdl.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
            sdl.SDL_GetGamepadButton.restype = ctypes.c_bool
            sdl.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
            sdl.SDL_GetGamepadAxis.restype = ctypes.c_int16
            sdl.SDL_GetGamepadType.argtypes = [ctypes.c_void_p]
            sdl.SDL_GetGamepadType.restype = ctypes.c_int
            sdl.SDL_free.argtypes = [ctypes.c_void_p]
            # The launcher has no SDL window: controller input arrives anyway.
            sdl.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
            if sdl.SDL_Init(0x2000):  # SDL_INIT_GAMEPAD
                self.sdl = sdl
            break

    def refresh(self):
        if not self.sdl:
            return []
        self.sdl.SDL_PumpEvents()
        count = ctypes.c_int(0)
        ids = self.sdl.SDL_GetGamepads(ctypes.byref(count))
        current = [ids[i] for i in range(count.value)] if ids else []
        if ids:
            self.sdl.SDL_free(ctypes.cast(ids, ctypes.c_void_p))
        for jid in current:
            if jid not in self.pads:
                pad = self.sdl.SDL_OpenGamepad(jid)
                if pad:
                    self.pads[jid] = pad
        for jid in [j for j in self.pads if j not in current]:
            self.sdl.SDL_CloseGamepad(self.pads.pop(jid))
        self.sdl.SDL_UpdateGamepads()
        return list(self.pads.values())

    def held(self):
        down = set()
        for pad in self.refresh():
            for index, name in enumerate(SDL_BUTTONS):
                if self.sdl.SDL_GetGamepadButton(pad, index):
                    down.add(name)
            for axis, name in ((4, "lefttrigger"), (5, "righttrigger")):
                if self.sdl.SDL_GetGamepadAxis(pad, axis) > 16000:
                    down.add(name)
        return down

    def style(self):
        for pad in self.refresh():
            return "playstation" if self.sdl.SDL_GetGamepadType(pad) in self.PLAYSTATION_TYPES else "xbox"
        return None


def merged_controls(controls):
    return native_api.controls("merged", controls)


# The controls in each game's settings file: the game reads them there, and its in-game menu (Insert / R3+L2,
# Controls tab) changes them too (launcher/native/controls.cpp converts both ways).
def controls_to_ini(settings):
    return native_api.controls("to_ini", settings)


def controls_from_ini(values, settings):
    """A game's controls from its settings file into the launcher's settings; False when the file has none yet."""
    result = native_api.controls("from_ini", {"values": values, "settings": settings})
    settings.clear()
    settings.update(result["settings"])
    return result["found"]


def binding_label(binding):
    return native_api.controls("label", binding)


def is_game_dir(folder):
    """The Collection's game folder (CUSA02344): eboot.bin and sce_sys/param.sfo."""
    return bool(folder) and native_api.system("is_game_dir", folder) == "1"


def game_update_dir(folder):
    """The game's update (e.g. 1.02) next to its folder, found as tools/probe_uncharted.py does: named like the game
    folder plus -patch or -update, with or without uroot."""
    found = native_api.system("update_dir", folder)
    return Path(found) if found else None


def app_version(folder):
    """APP_VER of a game folder (sce_sys/param.sfo); 01.02 when the Uncharted 2 executable is 1.02 although param.sfo
    says otherwise, and 01.00 when it is 1.00 although param.sfo says 01.02 (#148, #176)."""
    return native_api.system("app_version", folder)


def dark_title_bar(window):
    """The window's title bar in Windows' dark style, to match the launcher (GitHub #215).
    Windows 10 20H1+ and 11 (attribute 20; 19 on older Windows 10 builds); a no-op elsewhere."""
    if os.name != "nt" or WINE:
        return
    try:
        hwnd = ctypes.windll.user32.GetParent(window.winfo_id()) or window.winfo_id()
        value = ctypes.c_int(1)
        for attribute in (20, 19):
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, attribute, ctypes.byref(value),
                                                          ctypes.sizeof(value)) == 0:
                break
    except (AttributeError, OSError):
        pass


def desktop_folder():
    """The user's Desktop folder (SHGetKnownFolderPath: OneDrive or a redirected Desktop, #39)."""
    return Path(native_api.system("desktop"))


def monitor_refresh_hz():
    return native_api.monitor_refresh_hz()


# The Display page's tables and size arithmetic are the native code's (launcher/native/display.cpp): resolutions,
# aspect ratios (ultrawide widens the view, 16:10 shows more at the top and bottom), upscaling qualities, and the
# temporal upscalers (FSR 3.1, FSR 4, DLSS: they smooth edges themselves, so they take the runtime's "upscaler").
RESOLUTIONS = native_api.display_table("resolutions")
ASPECTS = native_api.display_table("aspects")
UPSCALE_PRESETS = native_api.display_table("upscale_presets")
TEMPORAL_UPSCALERS = tuple(row[0] for row in native_api.display_table("temporal_upscalers"))


def upscaling_choice(ini):
    """The upscaler a game's settings file holds: a temporal one, else FSR 1, else "off"."""
    return native_api.upscaling_choice(ini.get("upscaler", INI_DEFAULTS["upscaler"]), ini.get("fsr1", "0"))


def anti_aliasing_choice(ini, settings):
    """TAA or off (kept in the launcher's settings while a temporal upscaler takes the runtime's setting)."""
    return native_api.anti_aliasing_choice(ini.get("upscaler", INI_DEFAULTS["upscaler"]), settings.get("aa"))


def game_sizes(settings, upscaling):
    """The size a game draws at and, with upscaling, the size it is upscaled to (else None)."""
    return native_api.game_sizes(settings["resolution"], settings.get("aspect", "16:9"),
                                 settings.get("upscale", "off"), upscaling)


def aspect_resolution(resolution, aspect):
    return native_api.aspect_resolution(resolution, aspect)


def resolution_options(aspect):
    """The resolutions labelled by the height the game draws at this aspect ratio."""
    return native_api.resolution_options(aspect)

# Quality presets: the game has one detail level (the PS4's); quality here is the resolution the
# game draws at, the upscaling, the sharpening and the frame rate. Above 1080p the
# render resolution is new (October 2026): confirm in play. All three games draw at 4K
# natively except Drake's Fortune, which stops at 1620p (its memory layout) and upscales.
# IGPU card adapted from PR #223 by memeguyoffical (the card, its values and the iGPU detection).
PRESETS = native_api.presets()
PRESET_ORDER = ("igpu", "low", "medium", "high", "ultra")


def upscaling_options():
    """FSR 1 and FSR 3.1 always; FSR 4 and DLSS when their files are installed."""
    return native_api.upscaling_options(ROOT)


def preset_values(name):
    return dict(PRESETS[name]["values"])


def gpu_detect():
    """(card, integrated, preset) for Quick Setup: the card the game used last (its newest log), else Windows'
    strongest known adapter (launcher/native/display.cpp)."""
    return native_api.gpu_recommendation([ROOT / "analysis" / g["log"] / "boot.log" for g in GAMES])


def gpu_recommendation(name, integrated=None):
    """The Quick Setup preset for a card (IGPU for low-end integrated graphics, else its tier)."""
    return native_api.gpu_query("recommend", name, integrated)


def gpu_tier(name):
    return native_api.gpu_query("tier", name)


def igpu_class(name):
    """How strong an integrated GPU is: "weak", "mainstream" or "strong" (the IGPU preset's own defaults)."""
    return native_api.gpu_query("igpu_class", name)


# ---------------------------------------------------------------------------------------------
# Settings files

def ini_file(game):
    """Each game keeps its own runtime settings (tools/probe_uncharted.py passes the same file)."""
    return USER_DIR / f"probe-{game}.ini"


def read_ini(game):
    """The game's runtime settings (launcher/native/config.cpp): defaults, then its file's keys."""
    return native_api.ini_read(USER_DIR, game)


def ini_mtime(game):
    try:
        return ini_file(game).stat().st_mtime_ns
    except OSError:
        return None


def write_ini(game, updates):
    """Updates keys in place and keeps every other line (the in-game menu writes more keys)."""
    native_api.ini_write(USER_DIR, game, updates)


def read_config():
    """The launcher's settings (launcher/native/config.cpp): launcher.json with its defaults, the Wine merge with
    the prefix's own file and the version migrations."""
    return native_api.config_read(CONFIG_FILE, PREFIX_CONFIG_FILE, USER_DIR, WINE)


def write_config(config):
    """Saves the settings safely (the library writes through a temporary file); a locked file keeps them for the
    next write and the launcher keeps running."""
    if not native_api.config_write(CONFIG_FILE, config):
        print(f"Could not write {CONFIG_FILE}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------------------------
# Drawing helpers and widgets

def round_rect(canvas, x1, y1, x2, y2, r, **kw):
    """A rectangle with true circular corners (polygon along quarter circles)."""
    r = max(0, min(r, (x2 - x1) / 2, (y2 - y1) / 2))
    points = []
    steps = max(4, int(r / 2))
    for cx, cy, start in ((x2 - r, y1 + r, -90), (x2 - r, y2 - r, 0), (x1 + r, y2 - r, 90), (x1 + r, y1 + r, 180)):
        for i in range(steps + 1):
            a = math.radians(start + 90 * i / steps)
            points += [cx + r * math.cos(a), cy + r * math.sin(a)]
    return canvas.create_polygon(points, **kw)


def recolor(widget, mapping):
    """Gives a widget and everything inside it the card colours (mapping: old bg -> new bg); the
    launcher's canvas widgets draw themselves again."""
    try:
        bg = widget.cget("bg")
        if bg in mapping:
            widget.configure(bg=mapping[bg]) if not isinstance(widget, Pill) else tk.Canvas.configure(widget, bg=mapping[bg])
    except tk.TclError:
        pass
    for child in widget.winfo_children():
        recolor(child, mapping)
    if hasattr(widget, "draw"):
        try:
            widget.draw()
        except Exception:  # noqa: BLE001 - only looks
            pass


class Pill(tk.Canvas):
    """A rounded button. kind: primary (yellow), secondary (dark), outline (yellow outline)."""

    def __init__(self, parent, text, action, font, scale, kind="secondary", padx=16, pady=7):
        self.font = tkfont.Font(font=font)
        self.s, self.text, self.action, self.kind = scale, text, action, kind
        self.padx, self.pady, self.hover, self.enabled = int(padx * scale), int(pady * scale), False, True
        super().__init__(parent, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.bind("<Button-1>", lambda e: self.enabled and self.action())
        self.bind("<Enter>", lambda e: self.set_hover(True))
        self.bind("<Leave>", lambda e: self.set_hover(False))
        self.layout()

    def layout(self):
        self.w = self.font.measure(self.text) + 2 * self.padx
        self.h = self.font.metrics("linespace") + 2 * self.pady
        tk.Canvas.configure(self, width=self.w, height=self.h)
        self.draw()

    def configure(self, cnf=None, **kw):
        if cnf is None and not kw:
            return tk.Canvas.configure(self)
        kw = dict(cnf or {}, **kw)
        if "text" in kw:
            self.text = kw.pop("text")
            self.layout()
        kw.pop("fg", None)
        if kw:
            tk.Canvas.configure(self, **kw)
    config = configure

    def set_hover(self, hover):
        self.hover = hover
        self.draw()

    def draw(self):
        self.delete("all")
        w, h, r = self.w, self.h, self.h // 2
        if self.kind == "primary":
            fill, outline, fg = (GOLD_HI if self.hover else GOLD), "", INK
        elif self.kind == "outline":
            fill, outline, fg = (FIELD if self.hover else self["bg"]), GOLD, GOLD
        else:
            fill, outline, fg = (FIELD_HI if self.hover else FIELD), LINE, TEXT
        round_rect(self, 1, 1, w - 1, h - 1, r, fill=fill, outline=outline or fill)
        self.create_text(w / 2, h / 2, text=self.text, font=self.font, fill=fg)


class Card(tk.Canvas):
    """A rounded box that groups rows (Launcher.organize packs a page's widgets into .inner)."""

    def __init__(self, parent, scale, title=None, note=None, title_font=None, note_font=None, wrap=900):
        super().__init__(parent, bg=parent["bg"], highlightthickness=0, height=10)
        self.s, self.pad = scale, int(18 * scale)
        self.inner = tk.Frame(self, bg=CARD)
        self.window = self.create_window(self.pad, self.pad, window=self.inner, anchor="nw")
        self.title, self.note = title or "", note or ""
        if title:
            head = tk.Frame(self.inner, bg=CARD)
            head.card_head = True
            head.pack(side="top", fill="x", pady=(0, int(10 * scale)))
            accent = tk.Canvas(head, width=int(4 * scale), height=int(16 * scale), bg=CARD, highlightthickness=0)
            accent.pack(side="left", padx=(0, int(10 * scale)))
            round_rect(accent, 0, 0, int(4 * scale), int(16 * scale), int(2 * scale), fill=GOLD, outline=GOLD)
            tk.Label(head, text=title, font=title_font, bg=CARD, fg=TEXT, anchor="w").pack(side="left")
            if note:
                note_label = tk.Label(self.inner, text=note, font=note_font, bg=CARD, fg=MUTED, anchor="w",
                                      justify="left", wraplength=int(wrap * scale))
                note_label.pack(side="top", fill="x", pady=(0, int(10 * scale)))
                note_label.card_head = True
        self.inner.bind("<Configure>", self.relayout)
        self.bind("<Configure>", self.relayout)

    def relayout(self, event=None):
        w = self.winfo_width()
        h = self.inner.winfo_reqheight() + 2 * self.pad
        if int(self["height"]) != h:
            self.configure(height=h)
        if w > 1:
            self.itemconfigure(self.window, width=w - 2 * self.pad)
        self.delete("bg")
        if w > 1:
            round_rect(self, 1, 1, w - 1, h - 1, int(14 * self.s), fill=CARD, outline=CARD_LINE, tags="bg")
            self.tag_lower("bg")


class TabPill(tk.Canvas):
    """A tab: a rounded pill, yellow while selected."""

    def __init__(self, parent, text, font, scale, command):
        self.font = tkfont.Font(font=font)
        self.text, self.selected, self.hover = text, False, False
        self.w = self.font.measure(text) + int(36 * scale)
        self.h = self.font.metrics("linespace") + int(16 * scale)
        super().__init__(parent, width=self.w, height=self.h, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.bind("<Button-1>", lambda e: command())
        self.bind("<Enter>", lambda e: self.set(hover=True))
        self.bind("<Leave>", lambda e: self.set(hover=False))
        self.draw()

    def set(self, selected=None, hover=None):
        if selected is not None:
            self.selected = selected
        if hover is not None:
            self.hover = hover
        self.draw()

    def draw(self):
        self.delete("all")
        if self.selected:
            round_rect(self, 1, 1, self.w - 1, self.h - 1, self.h // 2, fill=GOLD, outline=GOLD)
        elif self.hover:
            round_rect(self, 1, 1, self.w - 1, self.h - 1, self.h // 2, fill=FIELD, outline=FIELD)
        self.create_text(self.w / 2, self.h / 2, text=self.text, font=self.font,
                         fill=INK if self.selected else (TEXT if self.hover else MUTED))


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


class Chip(tk.Canvas):
    """A small on/off pill, for the performance counter's items (Display tab)."""

    def __init__(self, parent, scale, font, label, value, command):
        self.s, self.label, self.command = scale, label, command
        self.value = bool(value)
        self.font = tkfont.Font(font=font)
        pad_x, pad_y = int(10 * scale), int(5 * scale)
        w = self.font.measure(label) + 2 * pad_x
        h = self.font.metrics("linespace") + 2 * pad_y
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.w, self.h = w, h
        self.bind("<Button-1>", lambda e: self.set(not self.value, notify=True))
        self.draw()

    def set(self, value, notify=False):
        self.value = bool(value)
        self.draw()
        if notify:
            self.command(self.value)

    def draw(self):
        self.delete("all")
        fill = GOLD if self.value else PANEL
        round_rect(self, 1, 1, self.w - 1, self.h - 1, self.h // 2, fill=fill,
                   outline=GOLD if self.value else LINE)
        self.create_text(self.w / 2, self.h / 2, text=self.label, font=self.font,
                         fill=INK if self.value else MUTED)


class Segmented(tk.Canvas):
    """A row of choices in a rounded track; the selected one is a yellow pill."""

    def __init__(self, parent, options, value, command, font, recommended=None):
        self.s = max(1.0, parent.winfo_fpixels("1i") / 96.0)
        self.font = tkfont.Font(font=font)
        self.command, self.value, self.recommended = command, value, recommended
        self.labels = {option: self.text(option, label) for option, label in options}
        self.visible = list(self.labels)
        self.enabled, self.hover, self.spans = True, None, []
        super().__init__(parent, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.bind("<Button-1>", self.click)
        self.bind("<Motion>", self.motion)
        self.bind("<Leave>", lambda e: self.motion(None))
        self.layout()

    def layout(self):
        pad, inset = int(12 * self.s), int(3 * self.s)
        self.h = self.font.metrics("linespace") + int(12 * self.s) + 2 * inset
        x, self.spans = inset, []
        for option in self.visible:
            w = self.font.measure(self.labels[option]) + 2 * pad
            self.spans.append((option, x, x + w))
            x += w
        self.w = x + inset
        tk.Canvas.configure(self, width=self.w, height=self.h)
        self.draw()

    def at(self, x):
        return next((option for option, x1, x2 in self.spans if x1 <= x < x2), None)

    def click(self, event):
        option = self.at(event.x)
        if self.enabled and option is not None:
            self.set(option, notify=True)

    def motion(self, event):
        option = self.at(event.x) if event is not None else None
        if option != self.hover:
            self.hover = option
            self.draw()

    def show(self, options):
        """Only these choices, in their order (FSR 1 has no Native AA)."""
        self.visible = [o for o in options if o in self.labels]
        self.layout()

    def set_enabled(self, enabled):
        """A disabled row shows its choice greyed and takes no clicks."""
        self.enabled = enabled
        tk.Canvas.configure(self, cursor="hand2" if enabled else "arrow")
        self.draw()

    def text(self, option, label):
        # The recommended choice carries a star.
        return f"{label}  ★" if option == self.recommended else label

    def relabel(self, options):
        """New labels for the same choices (the resolutions at another aspect ratio)."""
        for option, label in options:
            if option in self.labels:
                self.labels[option] = self.text(option, label)
        self.layout()

    def set(self, value, notify=False):
        self.value = value
        self.draw()
        if notify:
            self.command(value)

    def restyle(self):
        self.draw()

    def draw(self):
        self.delete("all")
        inset = int(3 * self.s)
        round_rect(self, 1, 1, self.w - 1, self.h - 1, self.h // 2, fill=FIELD, outline=LINE)
        for option, x1, x2 in self.spans:
            selected = option == self.value
            if selected:
                fill = GOLD if self.enabled else FIELD_HI
                round_rect(self, x1, inset, x2, self.h - inset, (self.h - 2 * inset) // 2, fill=fill, outline=fill)
            elif option == self.hover and self.enabled:
                round_rect(self, x1, inset, x2, self.h - inset, (self.h - 2 * inset) // 2, fill=FIELD_HI,
                           outline=FIELD_HI)
            fg = (INK if selected else TEXT) if self.enabled else MUTED
            self.create_text((x1 + x2) / 2, self.h / 2, text=self.labels[option], font=self.font, fill=fg)


class Dropdown(tk.Canvas):
    """A choice from a longer list: the current one in a rounded box, and a menu of all on click."""

    def __init__(self, parent, options, value, command, font):
        self.s = max(1.0, parent.winfo_fpixels("1i") / 96.0)
        self.options, self.command, self.font = options, command, font
        self.tkfont = tkfont.Font(font=font)
        self.hover = False
        self.w = max([self.tkfont.measure(label) for _, label in options] or [40]) + int(52 * self.s)
        self.h = self.tkfont.metrics("linespace") + int(16 * self.s)
        super().__init__(parent, width=self.w, height=self.h, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.bind("<Button-1>", lambda e: self.open())
        self.bind("<Enter>", lambda e: self.set_hover(True))
        self.bind("<Leave>", lambda e: self.set_hover(False))
        self.set(value)

    def set_hover(self, hover):
        self.hover = hover
        self.draw()

    def set(self, value, notify=False):
        self.value = value
        self.label = next((label for option, label in self.options if option == value), value)
        self.draw()
        if notify:
            self.command(value)

    def draw(self):
        self.delete("all")
        fill = FIELD_HI if self.hover else FIELD
        round_rect(self, 1, 1, self.w - 1, self.h - 1, int(10 * self.s), fill=fill, outline=LINE)
        self.create_text(int(14 * self.s), self.h / 2, text=self.label, font=self.font, fill=TEXT, anchor="w")
        self.create_text(self.w - int(16 * self.s), self.h / 2, text="▾", font=self.font, fill=GOLD)

    def open(self):
        menu = tk.Menu(self, tearoff=0, bg=PANEL_HI, fg=TEXT, activebackground=GOLD, activeforeground=INK,
                       font=self.font, bd=0)
        for option, label in self.options:
            menu.add_command(label=("✓  " if option == self.value else "     ") + label,
                             command=lambda v=option: self.set(v, notify=True))
        menu.tk_popup(self.winfo_rootx(), self.winfo_rooty() + self.winfo_height())


class ScrollBar(tk.Canvas):
    """A thin scrollbar in the launcher's colors for a scrolling canvas (its yscrollcommand)."""

    def __init__(self, parent, scale, target):
        super().__init__(parent, width=max(6, int(10 * scale)), bg=parent["bg"], highlightthickness=0)
        self.target, self.first, self.last, self.hover, self.grab = target, 0.0, 1.0, False, None
        self.bind("<Configure>", lambda e: self.draw())
        self.bind("<Enter>", lambda e: self.highlight(True))
        self.bind("<Leave>", lambda e: self.highlight(False))
        self.bind("<Button-1>", self.press)
        self.bind("<B1-Motion>", self.drag)
        self.bind("<ButtonRelease-1>", self.release)

    def set(self, first, last):
        self.first, self.last = float(first), float(last)
        self.draw()

    def highlight(self, hover):
        self.hover = hover
        self.draw()

    def draw(self):
        self.delete("all")
        w, h = self.winfo_width(), self.winfo_height()
        if h <= 1 or self.last - self.first >= 1:
            return
        x1, x2, r = 2, w - 2, max(1, (w - 4) // 2)
        round_rect(self, x1, 1, x2, h - 1, r, fill=PANEL, outline=PANEL)
        top = min(int(self.first * h), h - 24)
        bottom = min(h, max(int(self.last * h), top + 24))
        color = MUTED if self.hover or self.grab is not None else DIM
        round_rect(self, x1, top + 1, x2, bottom - 1, r, fill=color, outline=color)

    def press(self, event):
        position = event.y / max(1, self.winfo_height())
        if self.first <= position <= self.last:
            self.grab = position - self.first
        else:  # on the track: jump there, then drag from the thumb's middle
            self.grab = (self.last - self.first) / 2
            self.target.yview_moveto(position - self.grab)

    def drag(self, event):
        if self.grab is not None:
            self.target.yview_moveto(event.y / max(1, self.winfo_height()) - self.grab)

    def release(self, event):
        self.grab = None
        self.draw()


class Launcher:
    def __init__(self, root, direct=None):
        self.root = root
        # Direct start (--play u1|u2|u3, GitHub #25): the game starts at once with the saved
        # settings, without the launcher window, which closes with the game.
        self.direct = direct
        self.s = max(1.0, root.winfo_fpixels("1i") / 96.0)
        self.config = read_config()
        if self.config.get("game") not in GAME_KEYS:
            self.config["game"] = "u1"
        for key in GAME_KEYS:
            if not controls_from_ini(read_ini(key), self.config["games"][key]):
                write_ini(key, controls_to_ini(self.config["games"][key]))
        self.reload_ini()
        self.process = None
        self.output = queue.Queue()
        self.last_output = 0.0
        self.game_started = False
        self.widgets = {}
        self.images = []
        # The graphics card (as last detected; detect_gpu checks again) for the Quick Setup advice.
        self.gpu_name = self.config.get("gpu")
        self.recommended = gpu_recommendation(self.gpu_name)
        self.fonts()
        root.title(APP_TITLE + self.title_suffix())
        root.configure(bg=BG)
        root.resizable(True, True)
        self.width = self.px(1180)
        try:
            root.iconbitmap(default=str(ASSETS / "uncharted.ico"))
        except tk.TclError:
            pass
        self.build()
        root.update_idletasks()
        dark_title_bar(root)
        root.minsize(root.winfo_reqwidth(), root.winfo_reqheight())
        self.fit_to_screen()
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
        if self.config.get("check_updates", True) and not direct:
            root.after(1500, self.check_updates)
        if not direct:
            self.detect_gpu()
        if direct:
            self.select_game(direct, save=False)
            root.after(100, self.play_direct)

    def restore_window_position(self):
        """The window opens where it was last closed, if that spot is still on a screen."""
        position = self.config.get("window_xy")
        if not (isinstance(position, list) and len(position) == 2):
            return
        x, y = int(position[0]), int(position[1])
        self.root.update_idletasks()
        screen_w, screen_h = self.root.winfo_vrootwidth(), self.root.winfo_vrootheight()
        if -50 <= x <= max(0, screen_w - 200) and 0 <= y <= max(0, screen_h - 200):
            # All of the window on the screen (fit_to_screen may have made it taller than it was).
            y = min(y, max(0, self.root.winfo_screenheight() - self.root.winfo_reqheight() - self.px(70)))
            self.root.geometry(f"+{x}+{y}")

    def fit_to_screen(self):
        """More room for the settings when the screen is tall enough (fewer pages need to scroll)."""
        spare = self.root.winfo_screenheight() - self.px(110) - self.root.winfo_reqheight()
        if spare > 0:
            self.body.config(height=self.body.winfo_reqheight() + min(spare, self.px(240)))
            self.root.update_idletasks()

    def detect_gpu(self):
        """Finds the graphics card in the background (Windows' list takes a moment) and marks the
        Quick Setup preset that suits it. The game log's "(IntegratedGpu)" is exact; the model name
        is the fallback, and all there is when the card has never run the game."""
        found = []
        threading.Thread(target=lambda: found.append(gpu_detect()), daemon=True).start()

        def done():
            if not found:
                self.root.after(200, done)
                return
            name, _, preset = found[0]
            if not name:
                return
            self.gpu_name, self.recommended = name, preset
            if self.config.get("gpu") != name:
                self.config["gpu"] = name
                write_config(self.config)
            if hasattr(self, "preset_cards"):
                self.redraw_presets()
        self.root.after(200, done)

    def px(self, value):
        return int(value * self.s)

    def fonts(self):
        self.f_title = ("Georgia", 40, "bold")
        self.f_subtitle = ("Segoe UI Semibold", 11)
        self.f_card_number = ("Georgia", 30, "bold")
        self.f_card_title = ("Georgia", 14, "bold")
        self.f_body = ("Segoe UI", 10)
        self.f_body_bold = ("Segoe UI Semibold", 10)
        self.f_small = ("Segoe UI", 9)
        self.f_tab = ("Segoe UI Semibold", 10)
        self.f_caps = ("Segoe UI Semibold", 8)
        self.f_section = ("Segoe UI Semibold", 12)
        self.f_play = ("Georgia", 17, "bold")

    # -- layout -----------------------------------------------------------------------------

    def build(self):
        self.build_hero()
        self.build_cards()
        self.build_update_banner()
        self.build_footer()
        self.build_tabs()
        self.select_game(self.config.get("game", "u1"), save=False)
        self.show_tab(self.config.get("tab", "setup") if self.config.get("tab") in self.pages else "setup",
                      save=False)

    def build_hero(self):
        w, h = self.width, self.px(170)
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
        top = int(art.height * 0.02)
        art = art.crop((0, top, w, top + h))
        # Fade into the dark panel below.
        fade = Image.new("L", (w, h), 0)
        draw = ImageDraw.Draw(fade)
        start = int(h * 0.62)
        for y in range(start, h):
            draw.line([(0, y), (w, y)], fill=int(255 * ((y - start) / (h - start)) ** 1.6))
        dark = Image.new("RGB", (w, h), BG)
        art = Image.composite(dark, art, fade)
        photo = ImageTk.PhotoImage(art)
        self.images.append(photo)
        canvas = tk.Canvas(self.root, width=w, height=h, bg=BG, highlightthickness=0)
        canvas.pack(side="top")
        canvas.create_image(0, 0, image=photo, anchor="nw")
        x, y = self.px(48), self.px(52)
        canvas.create_text(x, y, text="UNCHARTED", font=self.f_title, fill="#3b2914", anchor="w")
        canvas.create_text(x + self.px(3), y + self.px(44), text="T H E   N A T H A N   D R A K E   C O L L E C T I O N",
                           font=self.f_subtitle, fill="#8a6326", anchor="w")
        canvas.create_line(x + self.px(3), y + self.px(66), x + self.px(330), y + self.px(66), fill="#b98d45", width=2)

    def build_update_banner(self):
        """A gold bar under the game cards while Uncharted 3's multiplayer files update (u3mp_gate): hidden
        otherwise (an empty slot keeps its place in the layout)."""
        self.banner_slot = tk.Frame(self.root, bg=BG, height=1)
        self.banner_slot.pack(side="top", fill="x", padx=self.px(40))
        self.banner = tk.Frame(self.banner_slot, bg=GOLD)
        self.banner_title = tk.Label(self.banner, text="", font=self.f_body_bold, bg=GOLD, fg=INK, anchor="w",
                                     justify="left")
        self.banner_title.pack(side="top", fill="x", padx=self.px(16), pady=(self.px(10), 0))
        self.banner_detail = tk.Label(self.banner, text="", font=self.f_small, bg=GOLD, fg=INK, anchor="w",
                                      justify="left", wraplength=self.px(1000))
        self.banner_detail.pack(side="top", fill="x", padx=self.px(16), pady=(self.px(2), self.px(4)))
        self.banner_bar = tk.Canvas(self.banner, height=self.px(8), bg=GOLD, highlightthickness=0)
        self.banner_bar.pack(side="top", fill="x", padx=self.px(16), pady=(0, self.px(10)))
        self.banner_shown = False
        self.u3mp_block = ""

    def build_cards(self):
        frame = tk.Frame(self.root, bg=BG)
        frame.pack(side="top", fill="x", padx=self.px(40), pady=(0, self.px(12)))
        self.cards = {}
        gap = self.px(16)
        card_w = (self.width - 2 * self.px(40) - 2 * gap) // 3
        card_h = self.px(72)
        for i, game in enumerate(GAMES):
            canvas = tk.Canvas(frame, width=card_w, height=card_h, bg=BG, highlightthickness=0, cursor="hand2")
            canvas.grid(row=0, column=i, padx=(0 if i == 0 else gap, 0))
            canvas.bind("<Button-1>", lambda e, k=game["key"]: self.select_game(k))
            canvas.bind("<Double-Button-1>", lambda e, k=game["key"]: self.play_game(k))
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
        canvas.create_text(self.px(66), h // 2 - self.px(10), text=game["title"], font=self.f_card_title,
                           fill=TEXT, anchor="w")
        canvas.create_text(self.px(67), h // 2 + self.px(13), text=game["subtitle"], font=self.f_small,
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
        self.update_multiplayer_tab()
        if hasattr(self, "play_button"):
            self.u3mp_gate()

    def u2_net_tab(self):
        """Uncharted 2's Multiplayer page (u2mp, page_multiplayer) is shown instead of Uncharted 3's."""
        return "multiplayer" in getattr(self, "tab_cells", {}) and self.config.get("game") == "u2"

    def update_multiplayer_tab(self):
        """The Multiplayer tab shows only while Uncharted 2 is chosen (u2mp); Uncharted 3's
        Multiplayer tab (u3mp) the rest of the time."""
        u3 = getattr(self, "tab_cells", {}).get("u3mp")
        if u3 is not None:
            if self.u2_net_tab():
                u3.pack_forget()
                if getattr(self, "current_tab", None) == "u3mp":
                    self.show_tab("multiplayer", save=False)
            elif not u3.winfo_manager():
                u3.pack(side="left", padx=(0, self.px(26)))
                if getattr(self, "current_tab", None) == "multiplayer":
                    self.show_tab("u3mp", save=False)
        cell = getattr(self, "tab_cells", {}).get("multiplayer")
        if cell is None:
            return
        if self.config.get("game") == "u2":
            if not cell.winfo_manager():
                cell.pack(side="left", padx=(0, self.px(26)))
        else:
            cell.pack_forget()
            if getattr(self, "current_tab", None) == "multiplayer":
                self.show_tab("setup", save=False)

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
        if controls_from_ini(self.ini, self.game_settings()):
            write_config(self.config)

    def save_controls(self):
        """The selected game's controls, also into its settings file (the game reads them there)."""
        write_config(self.config)
        self.save_ini(controls_to_ini(self.game_settings()))

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
        # The performance counter's items are Chip pills (overlay_panel): the loop above skips them.
        for key, widget in getattr(self, "overlay_chips", {}).items():
            if isinstance(widget, Chip):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]) == "1")
        settings = self.game_settings()
        if isinstance(self.widgets.get("resolution"), Segmented):
            self.widgets["resolution"].set(settings["resolution"])
        self.refresh_render_size()
        if isinstance(self.widgets.get("aspect"), Segmented):
            self.widgets["aspect"].set(settings.get("aspect", "16:9"))
        if isinstance(self.widgets.get("upscale"), Segmented):
            self.widgets["upscale"].set(settings.get("upscale", "off"))
        self.refresh_render_size()
        if isinstance(self.widgets.get("multithreaded"), Toggle):
            self.widgets["multithreaded"].set(settings["multithreaded"])
        if isinstance(self.widgets.get("save_profile"), Segmented):
            self.widgets["save_profile"].set(str(settings.get("save_profile", "1")))
        if isinstance(self.widgets.get("mouse"), Toggle):
            self.widgets["mouse"].set(settings.get("mouse", True))
            self.widgets["mouse_sens"].set(settings.get("mouse_sens", "1.0"))
            self.widgets["mouse_invert"].set(settings.get("mouse_invert", False))
            self.widgets["mouse_aim_sens"].set(settings.get("mouse_aim_sens", "1.0"))
            self.widgets["mouse_vertical"].set(settings.get("mouse_vertical", "1.0"))
            self.widgets["mouse_invert_x"].set(settings.get("mouse_invert_x", False))
            self.widgets["vibration"].set(settings.get("vibration", True))
            self.widgets["prompts"].set(settings.get("prompts", "auto"))
            self.refresh_pad_slots()
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
        if not folder:
            return DEFAULT_GAME_DIR
        if WINE and not Path(folder).exists():
            # Chosen in another prefix (another Steam shortcut): the same folder by its Linux path.
            here = windows_path(self.config.get("game_dir_unix"))
            if here and Path(here).exists():
                return Path(here)
        return Path(folder)

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
        if WINE:
            self.config["game_dir_unix"] = unix_path(folder)
        write_config(self.config)
        if hasattr(self, "game_dir_label"):
            self.game_dir_label.config(text=f"Game files: {folder}")
            self.update_label.config(text=self.update_text())
        return True

    def chosen_update(self):
        """The update folder chosen in Advanced (Choose update folder), if it is still there."""
        folder = self.config.get("update_dir") or ""
        if folder:
            for candidate in (Path(folder), Path(folder) / "uroot"):
                if is_game_dir(candidate):
                    return candidate
        return None

    def update_dir(self):
        """The update the games use: the chosen folder, else one next to the game folder."""
        return self.chosen_update() or game_update_dir(self.game_dir())

    def update_text(self):
        update = self.update_dir()
        if update:
            return f"Game update {app_version(update) or '?'} is used: {update}"
        folder = self.game_dir()
        version = app_version(folder)
        if version and version >= "01.02":
            return f"Game version {version} (the update is in the game folder)."
        title = folder.parent if folder.name.lower() == "uroot" else folder
        return (f"Game version {version or '?'}, no update found. For the 1.02 fixes, choose your update 1.02 "
                f"folder (or its .pkg) under GAME UPDATE.")

    def current_game(self):
        return next(g for g in GAMES if g["key"] == self.config.get("game"))

    def build_tabs(self):
        outer = tk.Frame(self.root, bg=BG)
        outer.pack(side="top", fill="both", expand=True, padx=self.px(40))
        # The tabs: rounded pills in a dark rounded strip; the selected one is yellow.
        strip_card = tk.Canvas(outer, bg=BG, highlightthickness=0, height=self.px(48))
        strip_card.pack(side="top", fill="x")
        strip = tk.Frame(strip_card, bg=PANEL)
        strip_window = strip_card.create_window(self.px(6), self.px(6), window=strip, anchor="nw")

        def strip_layout(event=None):
            w, h = strip_card.winfo_width(), strip.winfo_reqheight() + self.px(12)
            if int(strip_card["height"]) != h:
                strip_card.configure(height=h)
            strip_card.delete("bg")
            if w > 1:
                round_rect(strip_card, 1, 1, w - 1, h - 1, h // 2, fill=PANEL, outline=CARD_LINE, tags="bg")
                strip_card.tag_lower("bg")
                strip_card.itemconfigure(strip_window, width=w - self.px(12))
        strip.bind("<Configure>", strip_layout)
        strip_card.bind("<Configure>", strip_layout)
        self.tab_buttons = {}
        self.tab_cells = {}
        tabs = [("setup", "QUICK SETUP"), ("display", "DISPLAY"), ("performance", "PERFORMANCE"),
                ("controls", "CONTROLS"), ("cheats", "CHEATS"), ("advanced", "ADVANCED"), ("tools", "TOOLS"),
                ("u3mp", "MULTIPLAYER"), ("multiplayer", "MULTIPLAYER"),  # u3mp, or u2mp's while Uncharted 2 is chosen
                ("launcher", "LAUNCHER")]
        for name, label in tabs:
            cell = tk.Frame(strip, bg=PANEL)
            cell.pack(side="left", padx=(0, self.px(4)))
            pill = TabPill(cell, label, self.f_tab, self.s, lambda n=name: self.show_tab(n))
            pill.pack(side="left")
            self.tab_buttons[name] = pill
            self.tab_cells[name] = cell
        self.body = tk.Frame(outer, bg=BG, height=self.px(330))
        self.body.pack(side="top", fill="both", expand=True, pady=(self.px(14), 0))
        self.body.pack_propagate(False)
        # Pages taller than the space under the tabs scroll (Display, Performance, Advanced, Controls).
        self.scroll_canvases = set()
        self.root.bind_all("<MouseWheel>", self.wheel)
        self.pages = {
            "setup": self.page_setup(),
            "display": self.page_display(),
            "performance": self.page_performance(),
            "controls": self.page_controls(),
            "cheats": self.page_cheats(),
            "advanced": self.page_advanced(),
            "u3mp": self.page_u3mp(),
            "multiplayer": self.page_multiplayer(),
            "launcher": self.page_launcher(),
        }
        self.pages["tools"] = self.tools_page   # made by page_advanced (its shortcut, Steam and folder buttons)
        for page in self.pages.values():
            self.organize(page)

    def show_tab(self, name, save=True):
        if name == "multiplayer" and self.config.get("game") != "u2":
            name = "setup"
        if name == "u3mp" and self.u2_net_tab():
            name = "multiplayer"
        self.current_tab = name
        if save and self.config.get("tab") != name:
            self.config["tab"] = name
            write_config(self.config)
        for tab, pill in self.tab_buttons.items():
            pill.set(selected=tab == name)
        for tab, page in self.pages.items():
            holder = getattr(page, "holder", page)
            if tab == name:
                holder.pack(fill="both", expand=True)
            else:
                holder.pack_forget()

    def wheel(self, event):
        """The mouse wheel scrolls the page under the pointer, when it is taller than the window."""
        if getattr(self, "capturing", None) is not None:  # a Controls slot is waiting for an input
            return
        try:
            widget = self.root.winfo_containing(event.x_root, event.y_root)
        except (KeyError, tk.TclError):  # over a menu
            return
        while widget is not None:
            if str(widget) in self.scroll_canvases:
                if tuple(widget.yview()) != (0.0, 1.0):
                    step = int(-event.delta / 120) or (-1 if event.delta > 0 else 1)
                    widget.yview_scroll(step, "units")
                return
            widget = widget.master

    # -- pages ------------------------------------------------------------------------------

    def page(self, scroll=True):
        """A tab's page: the frame its rows go in. With `scroll`, it scrolls (with a scrollbar)
        when the rows are taller than the space under the tabs; show_tab packs its holder."""
        if not scroll:
            return tk.Frame(self.body, bg=BG)
        holder = tk.Frame(self.body, bg=BG)
        canvas = tk.Canvas(holder, bg=BG, highlightthickness=0)
        bar = ScrollBar(holder, self.s, canvas)
        inner = tk.Frame(canvas, bg=BG)
        window = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side="left", fill="both", expand=True)

        def layout(event=None):
            width, height, need = canvas.winfo_width(), canvas.winfo_height(), inner.winfo_reqheight()
            if height <= 1:  # not shown yet
                return
            canvas.itemconfigure(window, width=width)
            canvas.configure(scrollregion=(0, 0, width, max(need, height)))
            if need > height and not bar.winfo_manager():
                bar.pack(side="right", fill="y", padx=(self.px(8), 0), before=canvas)
            elif need <= height and bar.winfo_manager():
                bar.pack_forget()
                canvas.yview_moveto(0)
        inner.bind("<Configure>", layout)
        canvas.bind("<Configure>", layout)
        self.scroll_canvases.add(str(canvas))
        inner.holder = holder
        return inner

    def section(self, page, title, note="", column=""):
        """Starts a group of rows: organize() puts what follows (up to the next section) in a rounded card
        with this heading."""
        marker = tk.Frame(page, bg=BG, height=1)
        marker.pack(side="top", fill="x")
        marker.section = (title, note)
        marker.column = column   # "right": the Qt window puts this section in the right column
        return marker

    def organize(self, page):
        """Moves a page's widgets into rounded cards, one per section() marker, in the same order and with
        the same pack options; widgets before the first marker stay as they are. Rows in a card are
        separated by a thin line."""
        slaves = page.pack_slaves()
        if not any(hasattr(w, "section") for w in slaves):
            return
        infos = [(w, w.pack_info()) for w in slaves]
        for w, _ in infos:
            w.pack_forget()
        mapping = {BG: CARD, PANEL: FIELD, PANEL_HI: FIELD_HI}
        card, last_row = None, False
        for w, info in infos:
            info = {k: v for k, v in info.items() if k != "in"}
            if hasattr(w, "section"):
                title, note = w.section
                card = Card(page, self.s, title, note, self.f_section, self.f_small, wrap=820)
                card.column = getattr(w, "column", "")
                card.pack(side="top", fill="x", pady=(0, self.px(14)))
                w.destroy()
                last_row = False
                continue
            if card is None:
                w.pack(in_=page, **info)
                continue
            is_row = getattr(w, "is_row", False)
            if is_row and last_row:
                line = tk.Frame(page, bg=CARD_LINE, height=1)
                line.is_sep = True
                line.pack(in_=card.inner, side="top", fill="x", pady=(0, self.px(10)))
                line.lift(card)
            if is_row and info.get("pady") in (0, "0", (0, self.px(10)), f"0 {self.px(10)}"):
                info["pady"] = (0, self.px(10))
            w.pack(in_=card.inner, **info)
            w.tk.call("raise", w._w, card._w)
            recolor(w, mapping)
            last_row = is_row
        # Widgets the page shows later (pack(after=...) into a card: upscaling quality, render size) are drawn
        # above the cards too, in card colours.
        for w in page.winfo_children():
            if not isinstance(w, Card):
                w.tk.call("raise", w._w)
                if not w.winfo_manager():
                    recolor(w, mapping)

    def row(self, page, title, hint, control_factory):
        row = tk.Frame(page, bg=BG)
        row.pack(side="top", fill="x", pady=(0, self.px(10)))
        row.is_row = True
        text = tk.Frame(row, bg=BG)
        text.pack(side="left", fill="x", expand=True)
        row.title_label = tk.Label(text, text=title, font=self.f_body_bold, bg=BG, fg=TEXT, anchor="w")
        row.title_label.pack(side="top", fill="x")
        row.hint_label = None
        if hint:
            row.hint_label = tk.Label(text, text=hint, font=self.f_small, bg=BG, fg=MUTED, anchor="w", justify="left",
                                      wraplength=self.px(560))
            row.hint_label.pack(side="top", fill="x")
        control = control_factory(row)
        control.pack(side="right", padx=(self.px(20), 0))
        row.control = control   # qt_backend.py reads a row as title, hint and control
        return control

    def ini_toggle(self, page, key, title, hint):
        def factory(parent):
            return Toggle(parent, self.s, self.ini.get(key, "0") == "1",
                          lambda v, k=key: self.change(k, "1" if v else "0"))
        self.widgets[key] = self.row(page, title, hint, factory)

    def ini_choice(self, page, key, title, hint, options, recommended=None):
        def factory(parent):
            return Segmented(parent, options, self.ini.get(key, INI_DEFAULTS.get(key)), lambda v, k=key: self.change(k, v),
                             self.f_small, recommended)
        self.widgets[key] = self.row(page, title, hint, factory)

    def page_setup(self):
        page = self.page()
        self.section(page, "Quality level")
        self.setup_intro = tk.Label(page, text="", font=self.f_body, bg=BG, fg=MUTED, anchor="w")
        self.setup_intro.pack(side="top", fill="x", pady=(0, self.px(12)))
        grid = tk.Frame(page, bg=BG)
        grid.pack(side="top", fill="x")
        self.preset_cards = {}
        gap = self.px(16)
        # Five presets in 3 + 2: five in one row would squeeze the descriptions at this width.
        columns = 3
        card_w = (self.width - 2 * self.px(40) - (columns - 1) * gap) // columns
        card_h = self.px(170)
        for i, name in enumerate(PRESET_ORDER):
            row, column = divmod(i, columns)
            canvas = tk.Canvas(grid, width=card_w, height=card_h, bg=BG, highlightthickness=0, cursor="hand2")
            canvas.grid(row=row, column=column, padx=(0 if column == 0 else gap, 0),
                        pady=(0 if row == 0 else gap, 0), sticky="w")
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
        if name == self.recommended:
            badge = canvas.create_text(w - self.px(20), self.px(26), text="RECOMMENDED", font=("Segoe UI Semibold", 7),
                                       fill=INK, anchor="e")
            x1, y1, x2, y2 = canvas.bbox(badge)
            pill = round_rect(canvas, x1 - self.px(8), y1 - self.px(3), x2 + self.px(8), y2 + self.px(3), self.px(8),
                              fill=GOLD, outline=GOLD)
            canvas.tag_lower(pill, badge)

    def redraw_presets(self):
        for name in self.preset_cards:
            self.draw_preset(name)
        if self.recommended:
            label = "iGPU" if self.recommended == "igpu" else PRESETS[self.recommended]["title"].title()
            self.setup_intro.config(text=f"Recommended for your graphics card ({self.gpu_name}): {label}. "
                                         "Every setting can still be changed in Display and Performance.")
        else:
            self.setup_intro.config(text="Pick a quality level. Every setting can still be changed in Display "
                                         "and Performance.")
        custom = self.game_settings().get("preset") not in PRESETS
        name = self.current_game()["name"]
        self.custom_note.config(text=f"{name}: custom settings (changed by hand)." if custom
                                else f"Settings are kept per game: these are {name}'s.")

    def apply_preset(self, name):
        settings = self.game_settings()
        resolution = PRESETS[name]["resolution"]
        upscale = PRESETS[name]["upscale"]
        values = preset_values(name)
        if name == "igpu":
            # IGPU card adapted from PR #223 by memeguyoffical. One card for every low-end
            # integrated GPU: its class chooses how far below 1080p the game draws and the frame
            # limit (Vega and the 2-CU Radeon need 30 at 360p; Radeon 680M/760M/780M and Iris Xe
            # reach 60 at 540p; Radeon 880M/890M and integrated Arc at 720p).
            kind = igpu_class(self.gpu_name)
            if kind == "weak":
                upscale, values["fps_limit"] = "ultra", "30"
            elif kind == "strong":
                upscale = "quality"
        settings["resolution"] = resolution
        settings["upscale"] = upscale
        if "resolution" in self.widgets:
            self.widgets["resolution"].set(resolution)
        for key, value in values.items():
            self.ini[key] = value
            widget = self.widgets.get(key)
            if isinstance(widget, Toggle):
                widget.set(value == "1")
            elif isinstance(widget, Segmented):
                widget.set(value)
        if name == "igpu":
            # The command thread is the limit on an integrated GPU, and multithreaded rendering
            # takes part of it. Drake's Fortune keeps it off (its gun and camera misbehave with it).
            settings["multithreaded"] = self.current_game()["key"] != "u1"
            if isinstance(self.widgets.get("multithreaded"), Toggle):
                self.widgets["multithreaded"].set(settings["multithreaded"])
            settings["aa"] = "off"
        self.sync_sharpening()
        settings["preset"] = name
        write_config(self.config)
        self.save_ini({k: self.ini[k] for k in INI_DEFAULTS})
        self.refresh_render_size()
        self.redraw_presets()
        self.update_fps_note()

    def page_display(self):
        page = self.page()

        # The screen's shape first, then the resolution at that shape.
        def aspect(parent):
            return Segmented(parent, ASPECTS, self.game_settings().get("aspect", "16:9"),
                             self.change_aspect, self.f_small)
        self.section(page, "Resolution and upscaling")
        self.widgets["aspect"] = self.row(
            page, "Aspect ratio",
            "For ultrawide monitors (21:9, 32:9), with Borderless or Fullscreen: the games show more at "
            "the sides and the same view top to bottom as on a 16:9 screen. 16:10 (Steam Deck: 800p is "
            "1280x800) fills the screen and shows more at the top and bottom.", aspect)

        def resolution(parent):
            return Segmented(parent, resolution_options(self.game_settings().get("aspect", "16:9")),
                             self.game_settings()["resolution"], self.change_resolution, self.f_small)
        self.widgets["resolution"] = self.row(
            page, "Resolution",
            "How sharp the picture is; higher needs a faster graphics card (the PS4 drew 1080p). With "
            "upscaling below, the game draws less and is upscaled to it. If a game crashes above 1080p, "
            "go back to 1080p. Drake's Fortune draws at most 1620p and scales up.", resolution)
        self.render_size_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=GOLD, anchor="w")
        self.render_size_note.pack(side="top", fill="x", pady=(0, self.px(10)))
        self.refresh_render_size()
        def upscaling(parent):
            return Segmented(parent, upscaling_options(), upscaling_choice(self.ini), self.change_upscaling,
                             self.f_small)
        self.widgets["upscaling"] = self.row(
            page, "Upscaling",
            "More FPS: the game draws below the resolution and each frame is upscaled to it; the HUD and "
            "menus stay sharp at the resolution. FSR 1 is the lightest and works on any card; FSR 3.1, "
            "FSR 4 and DLSS (NVIDIA RTX cards) also use the previous frames: more detail, and they smooth "
            "edges themselves. Also in the in-game menu.", upscaling)

        # Shown with an upscaler only (refresh_upscale), right under it.
        def upscale(parent):
            box = tk.Frame(parent, bg=BG)
            self.widgets["upscale"] = Segmented(
                box, UPSCALE_PRESETS, self.game_settings().get("upscale", "off"),
                lambda v: (self.change_game_setting("upscale", v), self.refresh_render_size()), self.f_small)
            self.widgets["upscale"].pack(side="top", anchor="e")
            self.upscale_note = tk.Label(box, text="", font=self.f_small, bg=BG, fg=GOLD, anchor="e")
            self.upscale_note.pack(side="top", anchor="e", pady=(self.px(4), 0))
            return box
        self.upscale_row = self.row(
            page, "Upscaling quality",
            "How much below the resolution the game draws: Quality 1/1.5, Balanced 1/1.7, Performance 1/2, "
            "Ultra Performance 1/3 of its width and height. Native AA (DLAA with DLSS) draws at the "
            "resolution and only smooths edges.", upscale).master

        def anti_aliasing(parent):
            return Segmented(parent, [("off", "Off"), ("taa", "TAA")],
                             anti_aliasing_choice(self.ini, self.game_settings()), self.change_anti_aliasing,
                             self.f_small)
        self.widgets["aa"] = self.row(
            page, "Anti-aliasing",
            "Smooths jagged, shimmering edges, with upscaling Off or FSR 1. FSR 3.1, FSR 4 and DLSS do "
            "it themselves.", anti_aliasing)
        self.refresh_render_size()
        self.section(page, "Picture")
        self.ini_choice(page, "display_mode", "Display mode",
                        "A normal window, a borderless window (quick Alt+Tab) or fullscreen.",
                        [("windowed", "Windowed"), ("borderless", "Borderless"), ("fullscreen", "Fullscreen")])
        self.ini_toggle(page, "vsync", "VSync", "Stops tearing (a line across the picture when the camera turns). "
                                               "Recommended on.")

        def sharpening(parent):
            value = self.ini.get("rcas_strength", "75") if self.ini.get("rcas", "1") == "1" else "off"
            return Segmented(parent, [("off", "Off"), ("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%")],
                             value, self.change_sharpening, self.f_small)
        self.widgets["sharpening"] = self.row(page, "Sharpening", "Makes the final picture crisper (AMD FidelityFX RCAS).",
                                              sharpening)
        self.section(page, "Performance counter")
        self.ini_toggle(page, "show_fps", "Show performance counter",
                        "FPS, temperatures and memory in a corner of the screen. Insert, then the Overlay "
                        "tab, changes it in the game too.")
        self.overlay_panel(page)
        return page

    # The performance counter's items: the ini key and the label on its pill.
    OVERLAY_ITEMS = (("overlay_fps", "FPS"), ("overlay_frametime", "Frame time"), ("overlay_gpu", "GPU"),
                     ("overlay_cpu", "CPU"), ("overlay_ram", "RAM"), ("overlay_vram", "VRAM"),
                     ("overlay_upscaler", "Upscaler"))

    def overlay_panel(self, page):
        """What the performance counter shows, where and how big (Display tab; probe-u*.ini's
        show_fps and overlay_* keys, the ones the in-game menu's Overlay tab writes). It is built
        on its own: an error here leaves the rest of the page and the launcher working."""
        try:
            self._overlay_panel(page)
        except Exception:
            log_launcher_error(*sys.exc_info())
            self.overlay_chips = {}

    def _overlay_panel(self, page):
        def items(parent):
            box = tk.Frame(parent, bg=BG)
            self.overlay_chips = {}
            for key, label in self.OVERLAY_ITEMS:
                chip = Chip(box, self.s, self.f_small, label,
                            self.ini.get(key, INI_DEFAULTS[key]) == "1",
                            lambda v, k=key: self.change(k, "1" if v else "0"))
                chip.pack(side="left", padx=(self.px(6), 0))
                self.overlay_chips[key] = chip
            return box
        self.row(page, "What it shows",
                 "The frame rate, the frame time in milliseconds, the graphics card and the processor "
                 "with their temperatures, and memory. Pick any; with nothing picked the counter is "
                 "just its box.", items)
        self.ini_choice(page, "overlay_corner", "Counter corner", "Where the counter sits.",
                        [("0", "Top left"), ("1", "Top right"), ("2", "Bottom left"),
                         ("3", "Bottom right")])
        self.ini_choice(page, "overlay_layout", "Counter layout", "One line, or one item per line.",
                        [("0", "One line"), ("1", "Stacked")])
        self.ini_choice(page, "overlay_scale", "Counter size", "Text size of the counter.",
                        [("75", "75%"), ("100", "100%"), ("125", "125%"), ("150", "150%"),
                         ("200", "200%")])
        self.ini_choice(page, "overlay_opacity", "Counter background",
                        "How dark the counter's box is. None leaves the text on the picture.",
                        [("0", "None"), ("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%")])

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
        tk.Label(page, text="★ marks the recommended choice.", font=self.f_small, bg=BG, fg=DIM,
                 anchor="w").pack(side="top", fill="x", pady=(0, self.px(6)))
        self.section(page, "Frame rate")
        self.ini_choice(page, "fps_limit", "Frame rate limit",
                        "60 is how the games were made. Higher is smoother but experimental: if the game runs too "
                        "fast or Nate falls through the floor, go back to 60. With VSync, your monitor's refresh "
                        f"rate ({monitor_refresh_hz()} Hz) is the smoothest.",
                        [(str(v), label) for v, label in FPS_LIMITS], recommended="60")
        self.ini_choice(page, "frames_queued", "Frames queued",
                        "Keep 1. 2 can add a few FPS in heavy scenes, but in Uncharted 2 Nate and other "
                        "characters then float a little above the floor.",
                        [("1", "1"), ("2", "2"), ("0", "Unlimited")], recommended="1")
        self.section(page, "Processor")
        self.ini_toggle(page, "async_shaders", "Compile shaders in the background",
                        "Fewer stutters when you reach new areas. Recommended on.")

        def multithreaded(parent):
            return Toggle(parent, self.s, self.game_settings()["multithreaded"],
                          lambda v: self.change_game_setting("multithreaded", v))
        self.widgets["multithreaded"] = self.row(
            page, "Multithreaded rendering (experimental)",
            "Prepares the picture on a second CPU core: more FPS where the CPU is the limit, mostly "
            "in Uncharted 2 and 3 (on by default in Uncharted 3; keep Frames queued at 1 with it). "
            "Turn it off if a game hangs while loading. Drake's Fortune always runs without it.",
            multithreaded)

        def job_threads(parent):
            value = str(self.config.get("job_sleep", "auto"))
            return Segmented(parent, (("auto", "Auto"), ("on", "Sleep"), ("off", "Spin")),
                             value if value in ("auto", "on", "off") else "auto",
                             lambda v: self.change_config("job_sleep", v), self.f_small, recommended="auto")
        self.row(page, "Idle game threads",
                 "Sleep: the game's helper threads rest while they wait, which keeps 4-6 core CPUs fast "
                 "and saves power. Spin keeps their cores busy, as on the PS4: smoother on CPUs with "
                 "many cores. Auto picks Spin in Uncharted 1 on CPUs with 8 or more threads (it smooths "
                 "the camera there) and Sleep in Uncharted 2 and 3.", job_threads)

        # 1 Nov 2026 (0.8.0): experimental, so off unless the player picks it (not yet tried on a
        # real 2 GB card).
        def video_memory(parent):
            value = str(self.config.get("low_vram", "off"))
            return Segmented(parent, (("off", "Off"), ("on", "On"), ("auto", "Auto")),
                             value if value in ("auto", "on", "off") else "off",
                             lambda v: self.change_config("low_vram", v), self.f_small)
        self.section(page, "Video memory")
        self.row(page, "Save video memory (experimental)",
                 "For graphics cards with 2-4 GB: smaller memory reserves, about 0.5 GB less video "
                 "memory (Uncharted 2 at 720p: 2.1 -> 1.5 GB). New and not yet tried on a real 2 GB "
                 "card. Auto turns it on for cards with 4 GB or less.", video_memory)

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
        page = self.page(scroll=False)
        settings = self.game_settings()
        self.section(page, "Mouse")
        def toggle(key, default):
            return lambda parent: Toggle(parent, self.s, settings.get(key, default),
                                         lambda v, k=key: self.change_game_setting(k, v))

        def choice(key, options, default):
            return lambda parent: Segmented(parent, options, settings.get(key, default),
                                            lambda v, k=key: self.change_game_setting(k, v), self.f_small)
        self.widgets["mouse"] = self.row(page, "Mouse", "", toggle("mouse", True))
        self.widgets["mouse_sens"] = self.row(page, "Sensitivity", "", choice("mouse_sens", MOUSE_SENSITIVITIES, "1.0"))
        self.widgets["mouse_aim_sens"] = self.row(page, "Aiming", "", choice("mouse_aim_sens", MOUSE_AIM_FACTORS, "1.0"))
        self.widgets["mouse_vertical"] = self.row(page, "Up/down", "", choice("mouse_vertical", MOUSE_FACTORS, "1.0"))
        self.widgets["mouse_invert"] = self.row(page, "Invert Y", "", toggle("mouse_invert", False))
        self.widgets["mouse_invert_x"] = self.row(page, "Invert X", "", toggle("mouse_invert_x", False))
        self.section(page, "Controller")
        # Controller vibration (#166): off stops every rumble.
        self.widgets["vibration"] = self.row(page, "Controller vibration", "", toggle("vibration", True))
        self.widgets["prompts"] = self.row(
            page, "Button icons", "Auto follows the device you play with (a controller's own buttons).",
            lambda parent: Segmented(parent, PROMPT_MODES, settings.get("prompts", "auto"), self.change_prompts,
                                     self.f_small))
        self.section(page, "Bindings")
        self.controls_hint = tk.Label(
            page, text="Click a slot, then press a key or a mouse button (right-click a slot to clear it). "
                       "In game the mouse is captured; Alt+Tab or Insert (menu) frees it. A controller "
                       "always works too.", font=self.f_small, bg=BG, fg=MUTED, anchor="w", justify="left",
            wraplength=self.px(1080))
        self.controls_hint.pack(side="top", fill="x", pady=(0, self.px(6)))
        reset = tk.Frame(page, bg=BG)
        reset.pack(side="top", fill="x", pady=(0, self.px(8)))
        self.flat_button(reset, "Reset controls", self.reset_controls).pack(side="left")
        holder = tk.Frame(page, bg=BG)
        holder.pack(side="top", fill="both", expand=True)
        canvas = tk.Canvas(holder, bg=BG, highlightthickness=0)
        bar = ScrollBar(holder, self.s, canvas)
        inner = tk.Frame(canvas, bg=BG)
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        self.scroll_canvases.add(str(canvas))
        self.capturing = None
        self.binding_slots = {}
        self.pad_slots = {}
        self.pad_capture = None
        if not hasattr(self, "gamepads"):
            self.gamepads = Gamepads()
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
                         width=17).pack(side="left")
                for slot in (0, 1):
                    cell = tk.Label(row, text="", font=self.f_small, bg=PANEL, fg=TEXT, width=14,
                                    pady=self.px(3), cursor="hand2", highlightthickness=1,
                                    highlightbackground=LINE)
                    cell.pack(side="left", padx=(self.px(6), 0))
                    cell.bind("<Button-1>", lambda e, a=action, i=slot: self.start_capture(a, i))
                    cell.bind("<Button-3>", lambda e, a=action, i=slot: self.set_binding(a, i, None))
                    self.binding_slots[(action, slot)] = cell
                # The controller button: its icon; a click, then a controller button rebinds it.
                pad_cell = tk.Label(row, text="", font=self.f_small, bg=PANEL, fg=TEXT, width=14, height=1,
                                    highlightthickness=1, highlightbackground=LINE, compound="center")
                pad_cell.pack(side="left", padx=(self.px(6), 0))
                if action in PAD_DEFAULTS:
                    pad_cell.config(cursor="hand2")
                    pad_cell.bind("<Button-1>", lambda e, a=action: self.start_pad_capture(a))
                    pad_cell.bind("<Button-3>", lambda e, a=action: self.set_pad_binding(a, None))
                self.pad_slots[action] = pad_cell
        self.refresh_bindings()
        self.refresh_pad_slots()
        return page

    def game_controls(self):
        return merged_controls(self.game_settings().get("controls"))

    def change_prompts(self, value):
        self.change_game_setting("prompts", value)
        self.refresh_pad_slots()

    def pad_style(self):
        style = self.game_settings().get("prompts", "auto")
        if style in ("xbox", "playstation"):
            return style
        return self.gamepads.style() or "xbox"  # Auto: the connected controller's

    def refresh_pad_slots(self):
        style = self.pad_style()
        controls = merged_pad_controls(self.game_settings().get("pad_controls"))
        if not hasattr(self, "pad_images"):
            self.pad_images = {}
        height = self.px(22)
        for action, cell in getattr(self, "pad_slots", {}).items():
            if self.pad_capture == action:
                cell.config(image="", text="Press a button…", fg=GOLD, bg=PANEL_HI, width=14, height=1)
                continue
            button = controls.get(action) if action in PAD_DEFAULTS else PAD_FIXED.get(action)
            if not button:
                cell.config(image="", text="—", fg=TEXT, bg=PANEL, width=14, height=1)
                continue
            # GitHub #214: a PlayStation controller's touchpad click is both touchpad actions (the
            # finger's side picks one); Share / L1+Share are the stand-ins for other controllers.
            if style == "playstation" and button == PAD_DEFAULTS.get(action, PAD_FIXED.get(action)) \
                    and action in ("touchpad_left", "touchpad_right"):
                side = "left" if action == "touchpad_left" else "right"
                cell.config(image="", text=f"Touchpad {side}", fg=TEXT, bg=PANEL, width=14, height=1)
                continue
            key = (button, style, height)
            if key not in self.pad_images:
                self.pad_images[key] = ImageTk.PhotoImage(pad_icon(button, style, height))
            cell.config(image=self.pad_images[key], text="", bg=PANEL, width=self.px(100), height=self.px(24))

    def start_pad_capture(self, action):
        self.stop_capture()
        if not self.gamepads.sdl:
            self.controls_hint.config(text="No controller support found (SDL3.dll).", fg=STOP_RED)
            return
        self.pad_capture = action
        self.pad_capture_started = time.monotonic()
        self.pad_capture_held = self.gamepads.held()  # buttons already down do not count
        if not self.gamepads.pads:
            self.controls_hint.config(text="Connect a controller, then click the slot again.", fg=STOP_RED)
            self.pad_capture = None
            return
        self.controls_hint.config(text="Press a button on the controller (Escape cancels). Triggers and sticks "
                                       "stay Aim, Fire, Move and Look.", fg=MUTED)
        self.refresh_pad_slots()
        self.root.bind_all("<Escape>", lambda e: self.stop_pad_capture())
        self.root.after(30, self.poll_pad_capture)

    def poll_pad_capture(self):
        if self.pad_capture is None:
            return
        held = self.gamepads.held()
        new = [b for b in SDL_BUTTONS if b in held and b not in self.pad_capture_held]
        self.pad_capture_held &= held
        if any(t in held and t not in self.pad_capture_held for t in ("lefttrigger", "righttrigger")):
            self.controls_hint.config(text="The triggers stay Aim and Fire. Press a button.", fg=STOP_RED)
        if new:
            self.set_pad_binding(self.pad_capture, new[0])
            return
        if time.monotonic() - self.pad_capture_started > 8:
            self.stop_pad_capture()
            return
        self.root.after(30, self.poll_pad_capture)

    def stop_pad_capture(self):
        self.pad_capture = None
        self.root.unbind_all("<Escape>")
        self.refresh_pad_slots()

    def set_pad_binding(self, action, button):
        settings = self.game_settings()
        pad_controls = dict(settings.get("pad_controls") or {})
        merged = merged_pad_controls(pad_controls)
        if button:
            # A button does one thing: the action that had it swaps to this one's old button.
            for other, current in merged.items():
                if current == button and other != action:
                    pad_controls[other] = merged[action]
        pad_controls[action] = button or ""
        settings["pad_controls"] = {a: b for a, b in merged_pad_controls(pad_controls).items() if b != PAD_DEFAULTS.get(a)}
        self.save_controls()
        self.pad_capture = None
        self.root.unbind_all("<Escape>")
        self.refresh_pad_slots()
        self.controls_hint.config(text="Saved for " + self.current_game()["name"] + ". Right-click a controller "
                                       "slot to clear it; Reset controls restores the defaults.", fg=MUTED)

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
        # Tk 9 (the bundled Python 3.14) numbers the side buttons 8 and 9 on Windows (#152); 4 and
        # 5 were Tk 8's numbers.
        names = {1: "left", 2: "middle", 3: "right", 4: "x1", 5: "x2", 8: "x1", 9: "x2"}
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
        self.save_controls()
        self.controls_hint.config(
            text="Saved for " + self.current_game()["name"] + ". Click a slot, then press a key or a mouse button "
                 "(right-click a slot to clear it).", fg=MUTED)
        self.capturing = None
        for sequence in ("<KeyPress>", "<ButtonPress>", "<MouseWheel>"):
            self.root.unbind_all(sequence)
        self.refresh_bindings()

    def reset_controls(self):
        name = self.current_game()["name"]
        if not messagebox.askyesno(APP_TITLE, f"Reset {name}'s keyboard, mouse and controller controls?"):
            return
        settings = self.game_settings()
        settings.update({"controls": None, "mouse": True, "mouse_sens": "1.0", "mouse_invert": False,
                         "prompts": "auto", "pad_controls": None, "mouse_aim_sens": "1.0",
                         "mouse_vertical": "1.0", "mouse_invert_x": False, "vibration": True})
        self.save_controls()
        self.refresh_controls()

    def page_cheats(self):
        page = self.page()
        self.section(page, "Developer switches")
        tk.Label(page, text="The games' own developer switches, applied when a game starts. They need update 1.02.",
                 font=self.f_small, bg=BG, fg=MUTED, anchor="w").pack(side="top", fill="x", pady=(0, self.px(6)))
        for name, title, hint in CHEATS:
            def cheat(parent, key="cheat_" + name):
                return Toggle(parent, self.s, self.config.get(key, False), lambda v, k=key: self.change_config(k, v))
            self.row(page, title, hint, cheat)
        return page

    def page_advanced(self):
        page = self.page()

        # Save profiles (#66): profile N plays as PS4 user N, with its saves in savedata\N.
        def save_profile(parent):
            return Segmented(parent, (("1", "Profile 1"), ("2", "Profile 2"), ("3", "Profile 3")),
                             str(self.game_settings().get("save_profile", "1")),
                             lambda v: self.change_game_setting("save_profile", v), self.f_small)
        self.section(page, "Game")
        self.widgets["save_profile"] = self.row(
            page, "Save profile",
            "Separate saves for this game: each profile keeps its own progress and in-game settings, "
            "like another PS4 user. Profile 1 has your saves from earlier versions; switching never "
            "deletes anything.", save_profile)

        def language(parent):
            voices = voice_languages(self.game_dir())
            options = [(code, label + ("  (+ voices)" if code in voices else "")) for code, label in LANGUAGES]
            return Dropdown(parent, options, self.config.get("language", "1"),
                            lambda v: self.change_config("language", v), self.f_small)
        self.widgets["language"] = self.row(
            page, "Language",
            "Menus and subtitles in another language: pick it here, then press Square in a game's main "
            "menu to switch. Languages marked + voices are also spoken in your copy of the game.", language)

        # GitHub #199/#195/#204/#212: Uncharted 3 too bright on some PCs (pavlikdcx's exposure patch,
        # tools/probe_uncharted.py u3_exposure_patch).
        def u3_exposure(parent):
            return Segmented(parent, (("0", "Game's"), ("-0.3", "-0.3"), ("-0.6", "-0.6"), ("-1.0", "-1.0")),
                             str(self.config.get("u3_exposure", "0")),
                             lambda v: self.change_config("u3_exposure", v), self.f_small, recommended="0")
        self.row(page, "Uncharted 3 exposure",
                 "If Uncharted 3 looks washed out or too bright on your PC (bright skies and highlights), "
                 "lower its exposure here: -0.3 a little, -1.0 about half as bright. Game's = unchanged.",
                 u3_exposure)

        def speakers(parent):
            return Segmented(parent, (("auto", "Auto"), ("2", "Stereo"), ("6", "5.1"), ("8", "7.1")),
                             str(self.config.get("speakers", "auto")),
                             lambda v: self.change_config("speakers", v), self.f_small, recommended="auto")
        self.row(page, "Speakers",
                 "The games have their own stereo, 5.1 and 7.1 sound mixes. Auto picks the one that matches "
                 "your Windows sound output (Sound settings -> speaker setup); headphones get stereo.",
                 speakers)

        def menu_shortcut(parent):
            # 29 Oct 2026 (GitHub #300): L2 + R3 also zooms the scope with some guns. Written to
            # probe-u*.ini like the in-game menu does (menu_shortcut, gow3_settings.cpp); a custom
            # combination set in the game stays until another choice is made here.
            def save(value):  # all three games: the menu is the same in each
                self.ini["menu_shortcut"] = value
                for game in ("u1", "u2", "u3"):
                    if game != self.config["game"]:
                        write_ini(game, {"menu_shortcut": value})
                self.save_ini({"menu_shortcut": value})
            return Segmented(parent, (("lt_r3", "L2 + R3"), ("view_menu", "Select + Start"),
                                      ("keyboard", "Keyboard only")),
                             self.ini.get("menu_shortcut", "lt_r3"), save, self.f_small)
        self.row(page, "In-game menu shortcut",
                 "The controller buttons that open the in-game menu. L2 + R3 also zooms the scope with some "
                 "guns: pick Select + Start (View + Menu on Xbox) or Keyboard only (Insert) then.", menu_shortcut)

        def anisotropy(parent):
            return Segmented(parent, (("0", "Game"), ("4", "4x"), ("8", "8x"), ("16", "16x")),
                             str(self.config.get("anisotropy", "0")),
                             lambda v: self.change_config("anisotropy", v), self.f_small, recommended="16")
        self.section(page, "Extra quality")
        self.row(page, "Texture filtering",
                 "Sharper ground and wall textures seen at an angle, in all three games. 16x costs "
                 "almost no frame rate. Game = as on the PS4.", anisotropy)

        def hires_shadows(parent):
            return Toggle(parent, self.s, self.config.get("hires_shadows", False),
                          lambda v: self.change_config("hires_shadows", v))
        self.row(page, "Sharper lamp shadows (Uncharted 2 and 3)",
                 "The game's own high-resolution shadow buffer for lamps and flashlights (off on the "
                 "PS4). Extra quality, being tested. Needs update 1.02.", hires_shadows)

        def full_res_textures(parent):
            return Toggle(parent, self.s, self.config.get("full_res_textures", False),
                          lambda v: self.change_config("full_res_textures", v))
        self.row(page, "Full-resolution textures (Uncharted 2 and 3)",
                 "Always the sharpest version of every texture, no blurry ones while the game streams "
                 "(off on the PS4). Extra quality, being tested. Needs update 1.02.", full_res_textures)

        def no_dof(parent):
            return Toggle(parent, self.s, self.config.get("no_dof", False),
                          lambda v: self.change_config("no_dof", v))
        self.row(page, "No depth of field (Uncharted 2 and 3)",
                 "Turns off the background blur (while aiming, in cutscenes and in gameplay). Needs "
                 "update 1.02.", no_dof)

        def movie_stretch(parent):
            return Toggle(parent, self.s, self.config.get("movie_stretch", False),
                          lambda v: self.change_config("movie_stretch", v))
        self.row(page, "Stretch movies to the screen",
                 "The prerecorded cutscenes are 16:9 videos. Off: they keep their shape with black bars on "
                 "ultrawide screens. On: they fill the whole screen, stretched.", movie_stretch)

        def unlock(parent):
            return Toggle(parent, self.s, self.config.get("unlock", False),
                          lambda v: self.change_config("unlock", v))
        self.section(page, "Unlocks")
        self.row(page, "Unlock all chapters",
                 "Every chapter in Chapter Select and the Brutal difficulty from the start, in all three "
                 "games. Your saves and progress stay as they are. Needs update 1.02.", unlock)

        def unlock_rewards(parent):
            return Toggle(parent, self.s, self.config.get("unlock_rewards", False),
                          lambda v: self.change_config("unlock_rewards", v))
        self.row(page, "Unlock all rewards",
                 "Every reward in Bonuses (skins, weapons, render modes, art galleries, gameplay tweaks) "
                 "and every story cutscene, while this is on. Nothing is saved: turned off, the menus show "
                 "your own progress again. Needs update 1.02.", unlock_rewards)

        def diagnostics(parent):
            return Toggle(parent, self.s, self.config.get("diagnostics", True),
                          lambda v: self.change_config("diagnostics", v))
        self.section(page, "Launcher")
        self.row(page, "Performance log", "Writes frame rate statistics to the log every 5 seconds. Helps when "
                                          "you report a problem.", diagnostics)

        def torch_forward(parent):
            return Toggle(parent, self.s, self.config.get("torch_forward", True),
                          lambda v: self.change_config("torch_forward", v))
        self.row(page, "Torch follows Nate (Uncharted 2)",
                 "The torch lights where Nate points it. Off: as on the PS4, it lights where the camera "
                 "looks. Needs update 1.02.", torch_forward)

        advanced = page
        page = self.tools_page = self.page()   # the TOOLS tab

        def section(title, actions, note=""):
            self.section(page, title.capitalize(), note)
            line = tk.Frame(page, bg=BG)
            line.pack(side="top", fill="x")
            for label, action in actions:
                self.flat_button(line, label, action).pack(side="left", padx=(0, self.px(10)))
        section("SHORTCUTS", (("Desktop shortcut", self.desktop_shortcut), ("Game shortcuts", self.game_shortcuts)),
                "Game shortcuts: one per game on the desktop, each starts its game directly.")
        section("STEAM", (("Add to Steam", lambda: self.add_to_steam(per_game=False)),
                          ("Add games to Steam", lambda: self.add_to_steam(per_game=True))),
                "A non-Steam game in your library: the Collection, or one entry per game.")
        section("FOLDERS", (("Open log folder", self.open_logs), ("Open save folder", self.open_saves),
                            ("Change game folder", self.choose_game_dir),
                            ("Install game from .pkg", self.install_game_pkg),
                            ("Check game files", self.check_game_files)))
        self.game_dir_label = tk.Label(page, text=f"Game files: {self.game_dir()}", font=self.f_small,
                                       bg=BG, fg=DIM, anchor="w")
        self.game_dir_label.pack(side="top", fill="x", pady=(self.px(6), 0))
        self.update_label = tk.Label(page, text=self.update_text(), font=self.f_small, bg=BG, fg=DIM,
                                     anchor="w", justify="left")
        self.update_label.pack(side="top", fill="x")
        self.ini_label = tk.Label(page, text=f"Settings: {ini_file(self.config['game'])}", font=self.f_small,
                                  bg=BG, fg=DIM, anchor="w")
        self.ini_label.pack(side="top", fill="x")
        section("Game update", (("Choose update folder", self.choose_update_folder),
                                ("Install from .pkg", self.install_game_update)),
                "Your extracted update 1.02 folder, wherever it is, or the update's .pkg file.")
        section("TROUBLESHOOTING", (("Check for updates", lambda: self.check_updates(manual=True)),
                                    ("Clear shader cache", self.clear_cache), ("Reset all settings", self.reset)))
        self.section(page, "Credits")
        # Credits for the anti-aliasing technologies (NVIDIA's DLSS license asks for its attribution).
        tk.Label(page, text=CREDITS, font=self.f_small, bg=BG, fg=DIM, anchor="w", justify="left",
                 wraplength=self.width - 2 * self.px(40) - self.px(30)).pack(side="top", fill="x",
                                                                              pady=(self.px(10), 0))
        return advanced

    def page_launcher(self):
        """The launcher itself: how its updates install."""
        page = self.page()
        self.section(page, "Updates")
        version = updater.installed_version(ROOT)

        def mode(parent):
            return Segmented(parent, (("auto", "Automatic"), ("manual", "Manual")),
                             self.config.get("update_mode", "manual"), self.change_update_mode, self.f_small)
        self.row(page, "Launcher updates",
                 "Automatic: a new version installs by itself when no game or import is running. Manual: the "
                 "UPDATE button installs it.", mode)

        def check_updates(parent):
            return Toggle(parent, self.s, self.config.get("check_updates", True),
                          lambda v: self.change_config("check_updates", v))
        self.row(page, "Check for updates at start",
                 f"Installed: {updater.version_text(version) if version else 'source tree (updates with git)'}.",
                 check_updates)

        def check_now(parent):
            return self.flat_button(parent, "Check now", lambda: self.check_updates(manual=True))
        self.row(page, "Check for a new version", "", check_now)
        return page

    def change_update_mode(self, value):
        self.change_config("update_mode", value)
        if value == "auto" and self.update_release and not self.update_busy:
            self.update_button.pack_forget()
            self.auto_install()
        elif value == "manual" and self.update_release and not self.update_busy:
            self.update_button.pack(side="right", padx=(0, self.px(14)))

    def auto_install(self):
        """Automatic updates: install the found version as soon as no game or import is running."""
        if self.config.get("update_mode") != "auto" or not self.update_release or self.update_busy:
            return
        if self.process or self.import_running():
            self.root.after(60000, self.auto_install)
            return
        self.install_update(auto=True)

    def flat_button(self, parent, label, action):
        """A secondary action: a dark rounded button."""
        return Pill(parent, label, action, self.f_small, self.s, kind="secondary", padx=14, pady=7)

    # -- Uncharted 3 multiplayer (u3mp/patcher) ---------------------------------------------

    def u3mp_dns(self):
        """The community server's DNS (IPv4) for the game-data download: the Multiplayer tab field, else the 'dns='
        line of the in-game RPCN settings (analysis/uncharted-user/u3-rpcn.cfg, written by the helper). Only that
        one value is read; it is passed to the import and never written anywhere else."""
        value = (self.config.get("u3mp_dns") or "").strip()
        if value:
            return value
        try:
            for line in (USER_DIR / "u3-rpcn.cfg").read_text(encoding="utf-8", errors="replace").splitlines():
                if line.startswith("dns="):
                    return line[4:].strip()
        except OSError:
            pass
        return ""

    def u3mp_data(self):
        return Path(self.config.get("u3mp_data") or u3mp_default_data())

    def u3mp_state(self):
        try:
            return json.loads((self.u3mp_data() / "state.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def u3mp_ready(self, state=None):
        """(ready, why not): an imported multiplayer image of the shipped release, and its overlays."""
        state = self.u3mp_state() if state is None else state
        release = u3mp_release()
        image = state.get("image") or {}
        if state.get("status") not in ("imported", "partial"):
            return False, "not imported"
        # dc1 modules without their modules.bin crash Uncharted 3 at boot (u3mp_import.py CRITICAL_STEPS)
        broken = [s for s in ("convert-dc1", "dc-recipes") if s in (state.get("failed") or [])]
        if broken:
            return False, "the import step %s failed: press RE-IMPORT / UPDATE" % ", ".join(broken)
        if not release or image.get("sha256") != release.get("image_sha256"):
            return False, "imported for another multiplayer build: import again"
        if not Path(image.get("path", "")).is_file():
            return False, "the multiplayer image is missing: import again"
        if not [p for p in state.get("overlays", []) if Path(p).is_dir()]:
            return False, "the overlays are missing: import again"
        return True, ""

    def u3mp_pending(self, state=None):
        """What a launcher update changed in an existing import: "image" (a new multiplayer build: the image is
        relinked, seconds), "recipe" (new or changed conversion steps: rebuild_overlays.py reruns only those), both
        ("image+recipe") or "" (up to date / never imported)."""
        state = self.u3mp_state() if state is None else state
        if state.get("status") not in ("imported", "partial"):
            return ""
        parts = []
        release = u3mp_release()
        if release and (state.get("image") or {}).get("sha256") != release.get("image_sha256"):
            parts.append("image")
        try:
            sys.path.insert(0, str(U3MP_DIR))
            import u3mp_version
            shipped = u3mp_version.shipped_id(U3MP_DIR)
        except Exception:  # noqa: BLE001 - an incomplete patcher folder: nothing to compare
            shipped = ""
        finally:
            if sys.path and sys.path[0] == str(U3MP_DIR):
                sys.path.pop(0)
        recorded = state.get("recipe_id")
        if shipped and (recorded != shipped if recorded else
                        state.get("recipe_version") != self.u3mp_recipe_version()):
            # an update build of this recipe that ended worse than the import is not retried at every start
            # (Import retries it); a finished one waiting to be swapped in always counts
            if state.get("update_failed_for") != shipped or (state.get("next") or {}).get("recipe_id") == shipped:
                parts.append("recipe")
        return "+".join(parts)

    def u3mp_recipe_version(self):
        try:
            return (U3MP_DIR / "recipe" / "VERSION").read_text(encoding="utf-8").strip()
        except OSError:
            return ""

    def u3mp_auto_update(self):
        """After a launcher update: bring an existing import up to date in the background, with the folders it was
        imported from (no dialogs; the status line and log show it)."""
        try:
            if self.u3mp_running():
                return
            pending = self.u3mp_pending()
            if pending:
                self.u3mp_import(silent=True, image_only=(pending == "image"),
                                 reason="the launcher update brought a new multiplayer "
                                        + ("build" if pending == "image" else "version"))
        except (OSError, tk.TclError):
            pass

    def u3mp_block_reason(self):
        """Why Uncharted 3 cannot start yet: "running" (an update of an existing import is rebuilding the
        multiplayer files), "pending" (a launcher update brought a new build or recipe that is not applied yet), or
        "" (nothing to wait for; a first import in progress does not block: Uncharted 3 then starts without it)."""
        if not hasattr(self, "u3mp_status"):
            return ""
        try:
            state = self.u3mp_state()
            if self.u3mp_running():
                return "running" if state.get("image") else ""
            if state.get("status") in ("imported", "partial") and self.u3mp_pending(state):
                return "pending"
        except (OSError, tk.TclError):
            pass
        return ""

    def u3mp_blocked(self):
        """PLAY is held for Uncharted 3 while its multiplayer update is pending or running."""
        game = self.config.get("game")
        return not self.process and ((game == "u3" and bool(getattr(self, "u3mp_block", ""))) or
                                     (game == "u2" and bool(getattr(self, "u2mp_block", ""))))

    def u3mp_gate(self):
        """Every second (u3mp_poll) and on a game change: the gold update bar under the game cards and the PLAY
        button follow the multiplayer update state."""
        reason = self.u3mp_block_reason()
        reason2 = self.u2mp_block_reason()
        changed = reason != getattr(self, "u3mp_block", "") or reason2 != getattr(self, "u2mp_block", "")
        self.u3mp_block = reason
        self.u2mp_block = reason2
        if not hasattr(self, "banner"):
            return
        if reason2 and not reason:
            n, total = getattr(self, "u2mp_progress", (0, 0))
            if reason2 == "running":
                pct = f" ({int(100 * n / total)}%)" if total else ""
                self.banner_title.config(text="UNCHARTED 2 MULTIPLAYER UPDATE IN PROGRESS" + pct +
                                              " - Uncharted 2 can be played when it finishes")
                step = self.u2mp_step.cget("text") if hasattr(self, "u2mp_step") else ""
                self.banner_detail.config(text=(step + "\n" if step else "") +
                                          "The launcher update changed the multiplayer files; only what changed is "
                                          "rebuilt. Uncharted 1 and 3 can be played meanwhile, and closing the "
                                          "launcher does not stop the update (details: Multiplayer tab).")
            else:
                self.banner_title.config(text="UNCHARTED 2 MULTIPLAYER UPDATE NEEDED - it starts in a moment")
                self.banner_detail.config(text="The launcher update brought new multiplayer files. Uncharted 2 can "
                                               "be played once they are rebuilt.")
            self.banner_bar.delete("all")
            width = self.banner_bar.winfo_width()
            self.banner_bar.create_rectangle(0, 0, width, self.px(8), fill=INK, outline=INK)
            if total and width > 1 and reason2 == "running":
                self.banner_bar.create_rectangle(0, 0, int(width * min(1.0, n / total)), self.px(8), fill=TEXT,
                                                 outline=TEXT)
            if not self.banner_shown:
                self.banner.pack(side="top", fill="x", pady=(0, self.px(12)))
                self.banner_shown = True
        elif reason:
            n, total, _ = getattr(self, "u3mp_progress", (0, 0, ""))
            if reason == "running":
                pct = f" ({int(100 * n / total)}%)" if total else ""
                self.banner_title.config(text="UNCHARTED 3 MULTIPLAYER UPDATE IN PROGRESS" + pct +
                                              " - Uncharted 3 can be played when it finishes")
                step = self.u3mp_step.cget("text") if hasattr(self, "u3mp_step") else ""
                self.banner_detail.config(text=(step + "\n" if step else "") +
                                          "The launcher update changed the multiplayer files; only what changed is "
                                          "rebuilt. Uncharted 1 and 2 can be played meanwhile, and closing the "
                                          "launcher does not stop the update (details: Multiplayer tab)." +
                                          ("\nUncharted 2's multiplayer files are updating too." if reason2 else ""))
            else:
                self.banner_title.config(text="UNCHARTED 3 MULTIPLAYER UPDATE NEEDED - it starts in a moment")
                self.banner_detail.config(text="The launcher update brought new multiplayer files. Uncharted 3 can "
                                               "be played once they are rebuilt.")
            self.banner_bar.delete("all")
            width = self.banner_bar.winfo_width()
            self.banner_bar.create_rectangle(0, 0, width, self.px(8), fill=INK, outline=INK)
            if total and width > 1 and reason == "running":
                self.banner_bar.create_rectangle(0, 0, int(width * min(1.0, n / total)), self.px(8), fill=TEXT,
                                                 outline=TEXT)
            if not self.banner_shown:
                self.banner.pack(side="top", fill="x", pady=(0, self.px(12)))
                self.banner_shown = True
        elif self.banner_shown:
            self.banner.pack_forget()
            self.banner_slot.configure(height=1)   # Tk keeps a frame's size when its last child goes
            self.banner_shown = False
        if changed:
            self.draw_play()
            if not self.process and hasattr(self, "status"):
                if self.u3mp_blocked():
                    self.set_status(f"Uncharted {'2' if self.config.get('game') == 'u2' else '3'} is updating its "
                                    "multiplayer files.", "PLAY unlocks when the update finishes.")
                else:
                    self.set_status(f"Ready to play {self.current_game()['name']}.")

    def u3mp_confirm_play(self):
        """play(): Uncharted 3 does not start while its multiplayer update is pending or running (u3mp_gate); a
        pending one starts now if it did not yet."""
        if self.config.get("game") == "u2":
            return self.u2mp_confirm_play()
        if self.config.get("game") != "u3" or not hasattr(self, "u3mp_status"):
            return True
        reason = self.u3mp_block_reason()
        if not reason:
            return True
        try:
            pending = self.u3mp_pending()
            if pending and not self.u3mp_running():
                self.u3mp_import(silent=True, image_only=(pending == "image"),
                                 reason="the launcher update brought a new multiplayer "
                                        + ("build" if pending == "image" else "version"))
        except (OSError, tk.TclError):
            pass
        self.u3mp_gate()
        messagebox.showinfo(
            APP_TITLE, "Uncharted 3 is updating its multiplayer files after the launcher update.\n\nIt can be played "
                       "as soon as the update finishes (progress at the top of the launcher). Uncharted 1 and 2 can "
                       "be played meanwhile.")
        return False

    def u3mp_env(self, env):
        """play(): Uncharted 3 starts with the multiplayer image and overlays (tools/probe_uncharted.py
        u3mp_image) when the toggle is on and the import is complete."""
        for key in ("UNCHARTED_U3MP_IMAGE", "UNCHARTED_U3MP_IMAGE_SHA256", "UNCHARTED_U3MP_OVERLAYS",
                    "UNCHARTED_U3MP_VERSION"):
            env.pop(key, None)
        if not self.u3mp_on():
            return
        if getattr(self, "u3mp_skip_once", False) or (self.u3mp_running() and not self.u3mp_state().get("staging")):
            self.u3mp_skip_once = False
            print("Uncharted 3 multiplayer files are being imported or updated: Uncharted 3 starts without them")
            return
        state = self.u3mp_state()
        ready, why = self.u3mp_ready(state)
        if not ready:
            print(f"Uncharted 3 multiplayer is on but {why}: Uncharted 3 starts without it")
            return
        env["UNCHARTED_U3MP_IMAGE"] = state["image"]["path"]
        # the file as written (with the online profile keys from the player's EBOOT); older imports: the release digest
        env["UNCHARTED_U3MP_IMAGE_SHA256"] = state["image"].get("file_sha256") or state["image"]["sha256"]
        env["UNCHARTED_U3MP_VERSION"] = state["image"].get("version", "")
        env["UNCHARTED_U3MP_OVERLAYS"] = os.pathsep.join(p for p in state.get("overlays", []) if Path(p).is_dir())
        self.u3mp_extension_env(env)

    def u2mp_env(self, env):
        """play(): UNCHARTED_U2MP_OVERLAY, the folder tools/probe_uncharted.py lays over Uncharted 2
        (u2mp/patcher/u2mp_overlay.py: the main menu with Multiplayer, made from the player's own game),
        and UNCHARTED_U2MP=1, the runtime's restored multiplayer functions (gow3_pc/src/u2mp).
        Set for every start: the Collection menu may start Uncharted 2 from any game."""
        env.pop("UNCHARTED_U2MP_OVERLAY", None)
        env.pop("UNCHARTED_U2MP", None)
        for key in ("UNCHARTED_U2MP_NAME", "UNCHARTED_U2MP_JOIN"):
            env.pop(key, None)
        # Like Uncharted 3: multiplayer is on by itself once an import has finished (no toggle), and off while
        # there is none or it is running (user, 2026-11-09).
        state = self.u2mp_state()
        imported = U2MP_DIR / "import"
        if state.get("status") not in ("imported", "partial") or not (imported / "u2data").is_dir():
            print("Uncharted 2 multiplayer: no finished import, Uncharted 2 starts without it")
            return
        if self.u2mp_running():
            print("Uncharted 2 multiplayer files are being imported or updated: Uncharted 2 starts without them")
            return
        try:
            sys.path.insert(0, str(ROOT / "u2mp" / "patcher"))
            import u2mp_overlay
            overlay = u2mp_overlay.build(Path(self.game_dir()), U2MP_DIR / "overlay", self.chosen_update())
        except Exception as error:  # noqa: BLE001 - Uncharted 2 starts as usual without it
            print(f"Uncharted 2 multiplayer files not made ({error}): Uncharted 2 starts without them")
            return
        # The imported PS3 assets (u2mp_import.py) come after the menu overlay; the first folder holding a file wins.
        layers = [str(overlay), str(imported)]
        env["UNCHARTED_U2MP_OVERLAY"] = os.pathsep.join(layers)
        env["UNCHARTED_U2MP"] = "1"  # the runtime's u2mp module (gow3_pc/src/u2mp): the restored functions
        env["UNCHARTED_U2MP_NAME"] = "Drake"   # the name and join rows were removed (user, 2026-11-09)
        # Online play like Uncharted 3: the player signs in to RPCN on this page (the private module's section);
        # the game then uses that session through the local relay (UNCHARTED_U2MP_RELAY) and plays under the
        # account's name. Without the module or a signed-in session, Uncharted 2 has LAN play only.
        env.pop("UNCHARTED_U2MP_RELAY", None)
        env.pop("UNCHARTED_U2MP_ONLINE_DLL", None)
        online_dll = U3MP_DIR / "online" / "u2_online.dll"   # comes with the online extension
        if online_dll.is_file():
            env["UNCHARTED_U2MP_ONLINE_DLL"] = str(online_dll)
        try:
            module = self.u3mp_extension_module()
            hook = getattr(module, "handoff_env", None) if module is not None else None
            if hook is not None:
                hook(env, {"user_dir": USER_DIR, "game": "u2"})
        except Exception:
            log_launcher_error(*sys.exc_info())

    def page_multiplayer(self):
        """Uncharted 2 multiplayer (u2mp), laid out like Uncharted 3's page (page_u3mp). Shown only while
        Uncharted 2 is chosen (update_multiplayer_tab)."""
        page = self.page()
        banner = tk.Frame(page, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        # the intro box is not shown (its status line is in the Import section)
        tk.Frame(banner, bg=GOLD, width=self.px(4)).pack(side="left", fill="y")
        about = tk.Frame(banner, bg=PANEL)
        about.pack(side="left", fill="both", expand=True, padx=self.px(18), pady=self.px(14))
        head = tk.Frame(about, bg=PANEL)
        head.pack(side="top", fill="x")
        tk.Label(head, text="Uncharted 2 Multiplayer", font=self.f_card_title, bg=PANEL, fg=TEXT).pack(side="left")
        tk.Label(head, text="IN DEVELOPMENT", font=self.f_caps, bg=GOLD, fg=INK, padx=self.px(7),
                 pady=self.px(1)).pack(side="left", padx=(self.px(12), 0))
        tk.Label(about, text="Among Thieves' multiplayer, being restored. For now: the Multiplayer entry in "
                             "Uncharted 2's main menu and LAN parties. The import takes what the PS4 game lacks from "
                             "your own PS3 Uncharted 2, on this PC: nothing is downloaded and no game file comes "
                             "with the launcher.",
                 font=self.f_small, bg=PANEL, fg=MUTED, anchor="w", justify="left",
                 wraplength=self.px(760)).pack(side="top", fill="x", pady=(self.px(5), 0))
        self.u2mp_status = tk.Label(page, text="", font=self.f_body_bold, bg=BG, fg=TEXT, anchor="w",
                                    justify="left", wraplength=self.px(760))

        def folder_row(title, hint, label_key, actions):
            def factory(parent):
                box = tk.Frame(parent, bg=BG)
                for label, action in actions:
                    self.flat_button(box, label, action).pack(side="left", padx=(self.px(8), 0))
                return box
            self.row(page, title, hint, factory)
            label = tk.Label(page, text="", font=self.f_small, bg=BG, fg=DIM, anchor="w", justify="left",
                             wraplength=self.px(760))
            label.pack(side="top", fill="x", pady=(0, self.px(10)))
            self.u2mp_labels[label_key] = label

        self.u2mp_labels = {}
        if not self.config.get("u2mp_ps3") and self.config.get("u3mp_ps3_u2"):
            self.config["u2mp_ps3"] = self.config["u3mp_ps3_u2"]   # the folder chosen for Uncharted 3's import
        self.section(page, "Your PS3 game")
        folder_row("PS3 Uncharted 2 folder",
                   "Your PS3 Uncharted 2 folder (the one with USRDIR, e.g. NPUA80698).",
                   "u2mp_ps3", (("Choose", lambda: self.u2mp_choose("u2mp_ps3", "Choose the PS3 Uncharted 2 folder")),
                                ("Clear", lambda: self.u2mp_set("u2mp_ps3", ""))))
        folder_row("RPCS3 game data folder",
                   "RPCS3's dev_hdd0/game folder (start the game once in RPCS3 first). Found by itself when your "
                   "game folder is inside it.",
                   "u2mp_ps3_data", (("Choose", lambda: self.u2mp_choose("u2mp_ps3_data",
                                                                         "Choose RPCS3's dev_hdd0/game folder")),
                                     ("Clear", lambda: self.u2mp_set("u2mp_ps3_data", ""))))
        self.section(page, "Storage")
        folder_row("Multiplayer data folder",
                   "Where the imported files go.",
                   "data", (("Open", lambda: self.u3mp_open(U2MP_DIR / "import")),))

        self.section(page, "Import")
        actions = tk.Frame(page, bg=BG)
        actions.pack(side="top", fill="x", pady=(self.px(6), self.px(8)))
        self.u2mp_button = self.gold_button(actions, "IMPORT", self.u2mp_import)
        self.u2mp_button.pack(side="left")
        self.u2mp_cancel_button = self.flat_button(actions, "Cancel", self.u2mp_cancel)
        self.flat_button(actions, "Open import log", lambda: self.u3mp_open(U2MP_DIR / "import" / "import.log")).pack(
            side="left", padx=(self.px(10), 0))
        self.u2mp_step = tk.Label(actions, text="", font=self.f_small, bg=BG, fg=MUTED, anchor="w")
        self.u2mp_step.pack(side="left", fill="x", expand=True, padx=(self.px(14), 0))
        self.u2mp_bar = tk.Canvas(page, height=self.px(6), bg=PANEL, highlightthickness=0)
        self.u2mp_bar.pack(side="top", fill="x")
        self.u2mp_status.pack(side="top", fill="x", pady=(self.px(8), 0))
        self.u2mp_log = tk.Text(page, height=9, font=("Consolas", 8), bg=PANEL, fg=MUTED, relief="flat",
                                highlightthickness=1, highlightbackground=LINE, wrap="none", state="disabled")
        # (the log box is not shown: the progress bar and Open import log say it all)
        tk.Label(page, text="Runs in the background, even while you play.",
                 font=self.f_small, bg=BG, fg=DIM, anchor="w", justify="left",
                 wraplength=self.px(760)).pack(side="top", fill="x", pady=(self.px(8), 0))
        self.u2mp_proc = None
        self.u2mp_auto = ""  # why the running import was started by the launcher (an update), else ""
        self.u2mp_log_pos = 0
        self.u2mp_progress = (0, 0)
        self.u2mp_refresh()
        self.root.after(1000, self.u2mp_poll)
        self.root.after(4500, self.u2mp_auto_update)
        self.section(page, "RPCN online play", column="right")
        self.u2mp_extension(page)
        return page

    def u2mp_extension(self, page):
        """The RPCN section on Uncharted 2's page too: the private module's attach_view(launcher, page, context)
        adds a second view of the one RPCN session (RPCN refuses a second sign-in of an account). A module
        without attach_view shows nothing here."""
        try:
            module = self.u3mp_extension_module()
            hook = getattr(module, "attach_view", None) if module is not None else None
            if hook is not None:
                hook(self, page, {
                    "user_dir": USER_DIR, "bg": BG, "panel": PANEL, "panel_hi": PANEL_HI, "line": LINE,
                    "text": TEXT, "muted": MUTED, "dim": DIM, "gold": GOLD, "gold_hi": GOLD_HI, "ink": INK,
                    "red": STOP_RED, "game": "u2"})
        except Exception:
            log_launcher_error(*sys.exc_info())
            tk.Label(page, text="The online section could not be loaded (see launcher-error.log).",
                     font=self.f_small, bg=BG, fg=MUTED, anchor="w").pack(side="top", fill="x", pady=(self.px(12), 0))

    def u2mp_set(self, key, value):
        self.change_config(key, value)
        self.u2mp_refresh()

    def u2mp_choose(self, key, title):
        from tkinter import filedialog
        folder = filedialog.askdirectory(title=title, parent=self.root, mustexist=True)
        if folder:
            self.u2mp_set(key, folder)

    def u2mp_state(self):
        try:
            return json.loads((U2MP_DIR / "import" / "state.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def u2mp_running(self):
        """The import runs: started here, or (the launcher was closed and opened again) its state names a live pid."""
        if getattr(self, "u2mp_proc", None) is not None and self.u2mp_proc.poll() is None:
            return True
        state = self.u2mp_state()
        return state.get("status") == "running" and pid_alive(state.get("pid"))

    def u2mp_shipped_id(self):
        """The fingerprint of the installed Uncharted 2 import (u2mp/patcher/u2mp_version.py), computed once."""
        if getattr(self, "u2mp_shipped", None) is None:
            try:
                sys.path.insert(0, str(ROOT / "u2mp" / "patcher"))
                import u2mp_version
                self.u2mp_shipped = u2mp_version.shipped_id()
            except Exception:  # noqa: BLE001 - an incomplete patcher folder: nothing to compare
                self.u2mp_shipped = ""
            finally:
                if sys.path and sys.path[0] == str(ROOT / "u2mp" / "patcher"):
                    sys.path.pop(0)
        return self.u2mp_shipped

    def u2mp_pending(self, state=None):
        """True when a launcher update changed the Uncharted 2 import (its steps or converters) since the last
        import: the import then reruns by itself and redoes only the steps whose fingerprint changed."""
        state = self.u2mp_state() if state is None else state
        if state.get("status") not in ("imported", "partial"):
            return False
        # only an import this PC ran (it writes import.log next to its state); a state.json copied in is not one
        if not (U2MP_DIR / "import" / "import.log").is_file():
            return False
        shipped = self.u2mp_shipped_id()
        # an update that ended worse is not retried at every start (Import retries it)
        return bool(shipped) and state.get("recipe_id") != shipped and state.get("update_failed_for") != shipped

    def u2mp_auto_update(self):
        """After a launcher update: bring an existing Uncharted 2 import up to date in the background, with the
        folders it was imported from (no dialogs; the status line and log show it)."""
        try:
            if not self.u2mp_running() and self.u2mp_pending():
                self.u2mp_import(silent=True, reason="the launcher update changed the Uncharted 2 multiplayer files")
        except (OSError, tk.TclError):
            pass

    def u2mp_block_reason(self):
        """Like u3mp_block_reason, for Uncharted 2: "running" (an update of an existing import), "pending" (not
        applied yet) or "" (a first import does not block: Uncharted 2 then starts without it)."""
        if not hasattr(self, "u2mp_status"):
            return ""
        try:
            state = self.u2mp_state()
            if self.u2mp_running():
                return "running" if state.get("recipe_id") or state.get("updating") else ""
            if self.u2mp_pending(state):
                return "pending"
        except (OSError, tk.TclError):
            pass
        return ""

    def u2mp_confirm_play(self):
        """play(): Uncharted 2 does not start while its multiplayer update is pending or running."""
        if self.config.get("game") != "u2" or not self.u2mp_block_reason():
            return True
        try:
            if not self.u2mp_running() and self.u2mp_pending():
                self.u2mp_import(silent=True, reason="the launcher update changed the Uncharted 2 multiplayer files")
        except (OSError, tk.TclError):
            pass
        self.u3mp_gate()
        messagebox.showinfo(
            APP_TITLE, "Uncharted 2 is updating its multiplayer files after the launcher update.\n\nIt can be played "
                       "as soon as the update finishes (progress at the top of the launcher). Uncharted 1 and 3 can "
                       "be played meanwhile.")
        return False

    def u2mp_refresh(self):
        if not hasattr(self, "u2mp_status"):
            return
        state = self.u2mp_state()
        status = state.get("status")
        running = self.u2mp_running()
        failed = state.get("failed") or []
        if running:
            text, colour = (("Updating the multiplayer files (launcher update; only what changed is redone)..."
                             if getattr(self, "u2mp_auto", "") or state.get("updating") else "Importing..."), GOLD)
        elif status == "imported":
            text, colour = f"Imported ({state.get('finished', '?')}).", TEXT
        elif status == "partial":
            text = "Imported with problems: " + (f"step(s) {', '.join(failed)} failed; " if failed else "") + \
                   "some files could not be made (see the import log)."
            colour = GOLD
        elif status == "failed":
            text, colour = f"Import failed: {state.get('error', '') or 'see the import log'}", STOP_RED
        elif status == "running":
            text, colour = "The last import was interrupted. Press Import to continue it.", GOLD
        else:
            text, colour = "Not imported.", MUTED
        self.u2mp_status.config(text=text, fg=colour)
        self.u2mp_labels["u2mp_ps3"].config(text=self.config.get("u2mp_ps3") or "None chosen yet.")
        data = self.u2mp_data_folder()
        self.u2mp_labels["u2mp_ps3_data"].config(
            text=(data + ("" if self.config.get("u2mp_ps3_data") else "  (found by itself)")) if data else
            "Not found: start Uncharted 2 GOTY once in RPCS3, then choose RPCS3's dev_hdd0/game folder.")
        self.u2mp_labels["data"].config(text=str(U2MP_DIR / "import"))
        self.u2mp_button.config(text="RE-IMPORT / UPDATE" if status in ("imported", "partial", "failed", "running")
                                else "IMPORT")
        if running and not self.u2mp_cancel_button.winfo_manager():
            self.u2mp_cancel_button.pack(side="left", padx=(self.px(10), 0), after=self.u2mp_button)
        elif not running and self.u2mp_cancel_button.winfo_manager():
            self.u2mp_cancel_button.pack_forget()
        self.u2mp_draw_bar()

    def u2mp_draw_bar(self):
        n, total = self.u2mp_progress
        bar = self.u2mp_bar
        bar.delete("all")
        width = bar.winfo_width()
        if total and width > 1:
            bar.create_rectangle(0, 0, int(width * min(1.0, n / total)), self.px(6), fill=GOLD, outline=GOLD)

    def u2mp_data_folder(self):
        """The PS3 game-data folder for the import: the RPCS3 row, else RPCS3's dev_hdd0/game found above the chosen
        Uncharted 2 folder or among the Uncharted 3 page's folders (one holding <id>DATA2 folders), else ""."""
        chosen = str(self.config.get("u2mp_ps3_data", "") or "")
        if chosen and Path(chosen).is_dir():
            return chosen
        candidates = [self.config.get("u2mp_ps3", "")] + list(self.config.get("u3mp_ps3_u3", []) or [])
        for folder in candidates:
            if not folder:
                continue
            p = Path(folder)
            for q in [p] + list(p.parents)[:5]:
                try:
                    if q.is_dir() and any(c.is_dir() and c.name.upper().endswith("DATA2") for c in q.iterdir()):
                        return str(q)
                except OSError:
                    continue
        return ""

    def u2mp_python(self):
        """A Python with numpy for the import: the launcher's own, else one the py launcher lists; None if none has it."""
        own = Path(sys.executable)
        if own.name.lower() == "pythonw.exe" and own.with_name("python.exe").exists():
            own = own.with_name("python.exe")
        candidates = [own]
        try:
            listing = subprocess.run(["py", "-0p"], capture_output=True, text=True, timeout=10,
                                     creationflags=NO_WINDOW).stdout
            for line in listing.splitlines():
                path = line.split()[-1] if line.split() else ""
                if path.lower().endswith("python.exe"):
                    candidates.append(Path(path))
        except (OSError, subprocess.SubprocessError):
            pass
        for python in candidates:
            try:
                if python.exists() and subprocess.run([str(python), "-c", "import numpy"], capture_output=True,
                                                      timeout=60, creationflags=NO_WINDOW).returncode == 0:
                    return python
            except (OSError, subprocess.SubprocessError):
                continue
        return None

    def u2mp_install_numpy(self):
        """pip install --user numpy into the launcher's Python, else into another Python the py launcher lists (a
        version without numpy wheels fails and the next is tried). Returns the Python that then has numpy, or None."""
        own = Path(sys.executable)
        if own.name.lower() == "pythonw.exe" and own.with_name("python.exe").exists():
            own = own.with_name("python.exe")
        candidates = [own]
        try:
            listing = subprocess.run(["py", "-0p"], capture_output=True, text=True, timeout=10,
                                     creationflags=NO_WINDOW).stdout
            candidates += [Path(line.split()[-1]) for line in listing.splitlines()
                           if line.split() and line.split()[-1].lower().endswith("python.exe")]
        except (OSError, subprocess.SubprocessError):
            pass
        self.root.config(cursor="watch")
        self.root.update_idletasks()
        try:
            for python in candidates:
                if not python.exists():
                    continue
                try:
                    result = subprocess.run([str(python), "-m", "pip", "install", "--user", "--only-binary=:all:",
                                             "--disable-pip-version-check", "numpy"], capture_output=True, text=True,
                                            timeout=900, creationflags=NO_WINDOW)
                    print(f"numpy install with {python}: exit {result.returncode}\n{result.stdout[-2000:]}"
                          f"{result.stderr[-2000:]}")
                    if result.returncode == 0 and subprocess.run([str(python), "-c", "import numpy"],
                                                                 capture_output=True, timeout=60,
                                                                 creationflags=NO_WINDOW).returncode == 0:
                        return python
                except (OSError, subprocess.SubprocessError):
                    continue
        finally:
            self.root.config(cursor="")
        return None

    def u2mp_import(self, silent=False, reason=""):
        """IMPORT / RE-IMPORT, or (silent) the automatic update after a launcher update: u2mp/patcher/u2mp_import.py
        (below normal priority) into analysis/uncharted-user/u2mp/import; it keeps running when the launcher closes."""
        if self.u2mp_running():
            if not silent:
                messagebox.showinfo(APP_TITLE, "The import is already running.")
            return False
        ps3 = self.config.get("u2mp_ps3")
        if not ps3:
            if not silent:
                messagebox.showinfo(APP_TITLE, "Choose your PS3 Uncharted 2 folder first.")
            return False
        out = U2MP_DIR / "import"
        out.mkdir(parents=True, exist_ok=True)
        python = self.u2mp_python()
        if python is None and not silent and messagebox.askyesno(
                APP_TITLE, "The Uncharted 2 import needs Python's numpy package (for the characters, maps and menu "
                           "pictures), and no Python on this PC has it.\n\nInstall numpy now? (pip downloads it from "
                           "the Python Package Index into your user packages; this takes a minute.)"):
            python = self.u2mp_install_numpy()
        if python is None:
            if not silent:
                messagebox.showerror(APP_TITLE, "The Uncharted 2 import needs Python's numpy (for the characters, maps "
                                                "and menu pictures), and no Python on this PC has it.\n\nInstall it "
                                                "with:  py -m pip install numpy\n(the launcher's release package "
                                                "already carries it).")
            return False
        command = [str(python), "-u", str(ROOT / "u2mp" / "patcher" / "u2mp_import.py"), "--ps4", str(self.game_dir()),
                   "--ps3", ps3, "--out", str(out)]
        # GOTY data only (the user's decision, 2026-11-08): no map pack folder; the game data folder (net-patch9,
        # spawn paks) from the RPCS3 row or found next to the chosen folders.
        data = self.u2mp_data_folder()
        if data:
            command += ["--ps3-data", data]
        if silent:
            command += ["--update"]
        env = os.environ.copy()
        env["PYTHONIOENCODING"] = "utf-8"
        flags = NO_WINDOW | getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
        try:
            log = open(out / "launcher-run.log", "w", encoding="utf-8")
            self.u2mp_proc = subprocess.Popen(command, cwd=str(ROOT / "u2mp" / "patcher"), env=env, stdout=log,
                                              stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=flags)
            log.close()
        except OSError as error:
            if not silent:
                messagebox.showerror(APP_TITLE, f"Could not start the import:\n{error}")
            else:
                print(f"Could not start the Uncharted 2 multiplayer update: {error}")
            self.u2mp_proc = None
            return False
        self.u2mp_auto = reason if silent else ""
        self.u2mp_log_pos = 0
        self.u2mp_progress = (0, 0)
        self.u2mp_log.config(state="normal")
        self.u2mp_log.delete("1.0", "end")
        self.u2mp_log.config(state="disabled")
        self.u2mp_step.config(text=("Updating the multiplayer files: " + reason + "...") if silent else
                              "Starting the import...")
        self.u2mp_refresh()
        return True

    def u2mp_cancel(self):
        pid = self.u2mp_proc.pid if getattr(self, "u2mp_proc", None) is not None and self.u2mp_proc.poll() is None \
            else self.u2mp_state().get("pid")
        if pid and messagebox.askyesno(APP_TITLE, "Stop the import? Finished files are kept; Import continues it."):
            subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], creationflags=NO_WINDOW,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.u2mp_step.config(text="Import stopped.")
            self.root.after(500, self.u2mp_refresh)

    def u2mp_poll(self):
        """Every second: new lines of the import's output, its progress events (@U2MP ...), the status."""
        try:
            path = U2MP_DIR / "import" / "launcher-run.log"
            if path.exists():
                if path.stat().st_size < self.u2mp_log_pos:   # a new run rewrote the file
                    self.u2mp_log_pos = 0
                with open(path, "rb") as f:
                    f.seek(self.u2mp_log_pos)
                    chunk = f.read()
                cut = chunk.rfind(b"\n") + 1
                if cut:
                    self.u2mp_log_pos += cut
                    self.u2mp_lines(chunk[:cut].decode("utf-8", "replace").splitlines())
            proc = getattr(self, "u2mp_proc", None)
            if proc is not None and proc.poll() is not None:
                self.u2mp_proc = None
                self.u2mp_auto = ""
                if proc.returncode not in (0, None) and \
                        not self.u2mp_step.cget("text").startswith(("Import stopped", "Import failed")):
                    self.u2mp_step.config(text="Import failed: see the import log.")
                self.u2mp_refresh()
            elif self.u2mp_running() != self.u2mp_status.cget("text").startswith(("Importing...", "Updating the")):
                self.u2mp_refresh()
        except (OSError, tk.TclError):
            pass
        self.root.after(1000, self.u2mp_poll)

    def u2mp_lines(self, lines):
        shown = []
        for line in lines:
            if line.startswith("@U2MP "):
                try:
                    ev = json.loads(line[6:])
                except ValueError:
                    continue
                if ev.get("phase") == "recipe" and ev.get("step"):
                    self.u2mp_steps = (ev["step"], ev["total"], ev.get("title", ""))
                    self.u2mp_progress = (ev["step"] - 1, ev["total"])
                    self.u2mp_step.config(text=f"Step {ev['step']}/{ev['total']}: {ev.get('title', '')[:90]}")
                elif ev.get("phase") == "recipe" and ev.get("status") == "failed":
                    shown.append(f"!! step {ev.get('id')} failed: {ev.get('note', '')}")
                elif ev.get("phase") == "item" and ev.get("total"):
                    step, steps, title = getattr(self, "u2mp_steps", (1, 1, ""))
                    self.u2mp_progress = ((step - 1) + ev.get("n", 0) / ev["total"], steps)
                    self.u2mp_step.config(text=f"Step {step}/{steps}: file {ev.get('n', 0)}/{ev['total']}: "
                                               f"{ev.get('key', '')[:80]}")
                elif ev.get("phase") == "done":
                    self.u2mp_progress = (1, 1)
                    self.u2mp_step.config(text="Import finished." if ev.get("status") in ("imported", "partial")
                                          else "Import failed: " + ev.get("error", ""))
                    self.root.after(200, self.u2mp_refresh)
                continue
            shown.append(line)
        if shown:
            self.u2mp_log.config(state="normal")
            self.u2mp_log.insert("end", "\n".join(shown) + "\n")
            excess = int(self.u2mp_log.index("end-1c").split(".")[0]) - 400
            if excess > 0:
                self.u2mp_log.delete("1.0", f"{excess}.0")
            self.u2mp_log.see("end")
            self.u2mp_log.config(state="disabled")
        self.u2mp_draw_bar()

    def page_u3mp(self):
        """Uncharted 3 multiplayer: import from the player's own PS3 and PS4 copies (u3mp/patcher/u3mp_import.py),
        then Uncharted 3 starts with the restored multiplayer."""
        page = self.page()
        banner = tk.Frame(page, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        # the intro box is not shown (its status line is in the Import section)
        tk.Frame(banner, bg=GOLD, width=self.px(4)).pack(side="left", fill="y")
        about = tk.Frame(banner, bg=PANEL)
        about.pack(side="left", fill="both", expand=True, padx=self.px(18), pady=self.px(14))
        head = tk.Frame(about, bg=PANEL)
        head.pack(side="top", fill="x")
        tk.Label(head, text="Uncharted 3 Multiplayer", font=self.f_card_title, bg=PANEL, fg=TEXT).pack(side="left")
        tk.Label(head, text="BETA", font=self.f_caps, bg=GOLD, fg=INK, padx=self.px(7),
                 pady=self.px(1)).pack(side="left", padx=(self.px(12), 0))
        release = u3mp_release()
        tk.Label(about, text="Drake's Deception's competitive, co-op and LAN party modes, restored. The import "
                             "converts the multiplayer files from your own PS3 Uncharted 3 and this PS4 Collection, "
                             "on this PC: nothing is downloaded and no game file comes with the launcher. "
                             f"Build v{release['version'] if release else '?'}; supported PS4 game: "
                             f"{release['game'] if release else '?'}.",
                 font=self.f_small, bg=PANEL, fg=MUTED, anchor="w", justify="left",
                 wraplength=self.px(760)).pack(side="top", fill="x", pady=(self.px(5), 0))
        self.u3mp_status = tk.Label(page, text="", font=self.f_body_bold, bg=BG, fg=TEXT, anchor="w",
                                    justify="left", wraplength=self.px(760))

        def folder_row(title, hint, label_key, actions):
            def factory(parent):
                box = tk.Frame(parent, bg=BG)
                for label, action in actions:
                    self.flat_button(box, label, action).pack(side="left", padx=(self.px(8), 0))
                return box
            self.row(page, title, hint, factory)
            label = tk.Label(page, text="", font=self.f_small, bg=BG, fg=DIM, anchor="w", justify="left",
                             wraplength=self.px(760))
            label.pack(side="top", fill="x", pady=(0, self.px(10)))
            self.u3mp_labels[label_key] = label

        self.u3mp_labels = {}
        self.section(page, "Set up multiplayer", "New here? Do these four steps in order.")
        folder_row("1. Add your PS3 Uncharted 3 folders",
                   "Your PS3 game folder and RPCS3's dev_hdd0/game folder (update, game data, map packs). Add both.",
                   "ps3_u3", (("Add folder", self.u3mp_add_folder), ("Clear", self.u3mp_clear_folders)))
        # (no PS3 Uncharted 2 folder row: no multiplayer file comes from Uncharted 2 today; a folder chosen before
        # still goes to the import)
        folder_row("2. Choose the decrypted EBOOT (update 1.19)",
                   "Required: the decrypted EBOOT.elf of your own copy of the game.",
                   "ps3_elf", (("Choose file", self.u3mp_choose_elf),
                               ("Clear", lambda: self.u3mp_set("u3mp_ps3_elf", ""))))
        def dns_entry(parent):
            var = tk.StringVar(value=self.config.get("u3mp_dns", ""))
            entry = tk.Entry(parent, textvariable=var, width=18, font=self.f_body, bg=PANEL, fg=TEXT,
                             insertbackground=TEXT, relief="flat")

            def save(*_):
                self.change_config("u3mp_dns", var.get().strip())
            var.trace_add("write", save)
            return entry
        # (no DNS box here: since 0.10.43 the import downloads nothing; the server address is on the RPCN section)
        folder_row("3. Choose where the files go",
                   "The multiplayer data folder: the converted files (about 9 GB).",
                   "data", (("Change", self.u3mp_choose_data), ("Open", lambda: self.u3mp_open(self.u3mp_data()))))


        tk.Label(page, text="4. Import", font=self.f_body_bold, bg=BG, fg=TEXT, anchor="w").pack(
            side="top", fill="x", pady=(self.px(4), 0))
        actions = tk.Frame(page, bg=BG)
        actions.pack(side="top", fill="x", pady=(self.px(6), self.px(8)))
        self.u3mp_button = self.gold_button(actions, "IMPORT", self.u3mp_import)
        self.u3mp_button.pack(side="left")
        self.u3mp_cancel_button = self.flat_button(actions, "Cancel", self.u3mp_cancel)
        self.flat_button(actions, "Open import log", lambda: self.u3mp_open(self.u3mp_data() / "import.log")).pack(
            side="left", padx=(self.px(10), 0))
        self.u3mp_step = tk.Label(actions, text="", font=self.f_small, bg=BG, fg=MUTED, anchor="w")
        self.u3mp_step.pack(side="left", fill="x", expand=True, padx=(self.px(14), 0))
        self.u3mp_bar = tk.Canvas(page, height=self.px(6), bg=PANEL, highlightthickness=0)
        self.u3mp_bar.pack(side="top", fill="x")
        self.u3mp_status.pack(side="top", fill="x", pady=(self.px(8), 0))
        self.u3mp_log = tk.Text(page, height=9, font=("Consolas", 8), bg=PANEL, fg=MUTED, relief="flat",
                                highlightthickness=1, highlightbackground=LINE, wrap="none", state="disabled")
        # (the log box is not shown: the progress bar and Open import log say it all)
        tk.Label(page, text="Takes a few hours and keeps running when the launcher is closed. After a launcher "
                            "update only what changed is redone.",
                 font=self.f_small, bg=BG, fg=DIM, anchor="w", justify="left",
                 wraplength=self.px(760)).pack(side="top", fill="x", pady=(self.px(8), 0))
        self.section(page, "Storage")
        def keep_toggle(parent):
            self.u3mp_keep_widget = Toggle(parent, self.s, self.config.get("u3mp_keep_ps3", False), self.u3mp_keep_ps3)
            return self.u3mp_keep_widget
        self.row(page, "Keep only the PS3 files updates need (saves space)",
                 "Saves disk space only. Keeps the PS3 files updates need (about 14 GB of 51 GB), so you can "
                 "delete your PS3 Uncharted 3 folders.",
                 keep_toggle)
        label = tk.Label(page, text="", font=self.f_small, bg=BG, fg=DIM, anchor="w", justify="left",
                         wraplength=self.px(760))
        label.pack(side="top", fill="x", pady=(0, self.px(10)))
        self.u3mp_labels["keep_ps3"] = label
        self.u3mp_proc = None
        self.u3mp_auto = ""  # why the running import was started by the launcher (an update), else ""
        self.u3mp_log_pos = 0
        self.u3mp_progress = (0, 0, "")
        self.u3mp_refresh()
        self.root.after(1000, self.u3mp_poll)
        self.root.after(4000, self.u3mp_auto_update)
        self.section(page, "RPCN online play", column="right")
        self.u3mp_extension(page)
        return page

    def u3mp_extension(self, page):
        """An optional extra section at the end of this page: u3mp/patcher/online/u3_online.py when the
        package has that file. Its attach(launcher, page, context) builds the section and starts what it needs
        (its frame's <Destroy> ends it). Without the file the page shows nothing more; a failure is logged."""
        self.u3mp_online = None
        try:
            module = self.u3mp_extension_module(reload=True)
            if module is None:
                return
            self.u3mp_online = module.attach(self, page, {
                "user_dir": USER_DIR, "bg": BG, "panel": PANEL, "panel_hi": PANEL_HI, "line": LINE, "text": TEXT,
                "muted": MUTED, "dim": DIM, "gold": GOLD, "gold_hi": GOLD_HI, "ink": INK, "red": STOP_RED})
        except Exception:
            log_launcher_error(*sys.exc_info())
            tk.Label(page, text="The online section could not be loaded (see launcher-error.log).",
                     font=self.f_small, bg=BG, fg=MUTED, anchor="w").pack(side="top", fill="x", pady=(self.px(12), 0))

    def u3mp_extension_module(self, reload=False):
        """The optional module u3mp/patcher/online/u3_online.py (loaded once; reload=True loads it again), or None
        when the package does not have it."""
        module = None if reload else sys.modules.get("u3_online")
        if module is not None:
            return module
        path = U3MP_DIR / "online" / "u3_online.py"
        if not path.is_file():
            return None
        import importlib.util
        spec = importlib.util.spec_from_file_location("u3_online", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules["u3_online"] = module
        spec.loader.exec_module(module)
        return module

    def u3mp_extension_env(self, env):
        """play(), when Uncharted 3 starts with multiplayer: the optional module may add to the game's environment
        (its handoff_env(env, context)). Without the module, or without that function, nothing changes."""
        try:
            module = self.u3mp_extension_module()
            hook = getattr(module, "handoff_env", None) if module is not None else None
            if hook is not None:
                hook(env, {"user_dir": USER_DIR})
        except Exception:
            log_launcher_error(*sys.exc_info())

    def u3mp_set(self, key, value):
        self.change_config(key, value)
        self.u3mp_refresh()

    def u3mp_on(self):
        """Uncharted 3 starts with multiplayer whenever an import finished (imported, or partial with the
        multiplayer files usable): there is no switch for it. Never imported: the normal Uncharted 3."""
        return self.u3mp_state().get("status") in ("imported", "partial")

    def u3mp_add_folder(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(title="Choose a PS3 Uncharted 3 folder (game disc or RPCS3 dev_hdd0/game)",
                                         parent=self.root, mustexist=True)
        if folder:
            folders = [f for f in self.config.get("u3mp_ps3_u3", []) if f != folder] + [folder]
            self.u3mp_set("u3mp_ps3_u3", folders)

    def u3mp_clear_folders(self):
        self.u3mp_set("u3mp_ps3_u3", [])

    def u3mp_choose_u2(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(title="Choose the PS3 Uncharted 2 folder", parent=self.root, mustexist=True)
        if folder:
            self.u3mp_set("u3mp_ps3_u2", folder)

    def u3mp_choose_elf(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(title="Choose the decrypted EBOOT.elf (Uncharted 3 update 1.19)",
                                          parent=self.root, filetypes=[("Decrypted PS3 executable", "*.elf"),
                                                                       ("All files", "*.*")])
        if path:
            self.u3mp_set("u3mp_ps3_elf", path)

    def u3mp_choose_data(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(title="Choose the folder for the multiplayer files (about 9 GB)",
                                         parent=self.root)
        if folder:
            self.u3mp_set("u3mp_data", str(Path(folder) / "U3 Multiplayer"))

    def u3mp_open(self, path):
        path = Path(path)
        target = path if path.exists() else path.parent
        if target.exists():
            os.startfile(target)

    def u3mp_running(self):
        if self.u3mp_proc is not None and self.u3mp_proc.poll() is None:
            return True
        state = self.u3mp_state()
        return state.get("status") == "running" and pid_alive(state.get("pid"))

    def title_suffix(self):
        """version_suffix with the imported Uncharted 3 multiplayer build, when there is an import."""
        state = self.u3mp_state()
        imported = ""
        if state.get("status") in ("imported", "partial"):
            imported = str((state.get("image") or {}).get("version") or "")
        return version_suffix(imported)

    def u3mp_refresh(self):
        try:
            self.root.title(APP_TITLE + self.title_suffix())   # an import or update may have changed the build
        except tk.TclError:
            pass
        if not hasattr(self, "u3mp_status"):
            return
        state = self.u3mp_state()
        running = self.u3mp_running()
        ready, why = self.u3mp_ready(state)
        status = state.get("status")
        image = state.get("image") or {}
        if running:
            text, colour = ("Updating the multiplayer files (launcher update; only what changed is redone)..."
                            if getattr(self, "u3mp_auto", "") else "Importing..."), GOLD
        elif ready and status == "imported":
            text, colour = f"Imported v{image.get('version', '?')} (recipe {state.get('recipe_version', '?')}).", TEXT
        elif ready:
            failed = state.get("failed", [])
            text = (f"Imported v{image.get('version', '?')} with problems: {len(failed)} step(s) failed "
                    f"({', '.join(failed[:4])}{'...' if len(failed) > 4 else ''}). Multiplayer can start, but "
                    "the files those steps make are missing.")
            colour = GOLD
        elif status == "failed":
            text, colour = f"Failed at {state.get('failed_at', '?')}: {state.get('error', '')}", STOP_RED
        elif status == "running":
            text, colour = "The last import was interrupted. Press Import to continue it.", GOLD
        elif status == "image-only":
            text, colour = "Multiplayer image made; the files are not imported yet.", MUTED
        elif state and not ready and why:
            text, colour = f"Not ready: {why}.", GOLD
        else:
            text, colour = "Not imported.", MUTED
        self.u3mp_status.config(text=text, fg=colour)
        if self.u3mp_on() and not ready and not running:
            self.u3mp_status.config(text=text + " Uncharted 3 starts without multiplayer until then.")
        folders = self.config.get("u3mp_ps3_u3", [])
        self.u3mp_labels["ps3_u3"].config(text="\n".join(folders) if folders else "None chosen yet.")
        if "ps3_u2" in self.u3mp_labels:
            self.u3mp_labels["ps3_u2"].config(text=self.config.get("u3mp_ps3_u2") or "Not used.")
        self.u3mp_labels["ps3_elf"].config(text=self.config.get("u3mp_ps3_elf") or
                                           state.get("ps3_elf") or "Not chosen: looked for in your PS3 folders.")
        self.u3mp_labels["data"].config(text=str(self.u3mp_data()))
        kept = state.get("ps3_kept") or {}
        if not self.config.get("u3mp_keep_ps3", False):
            keep_text = "Off: the import reads your PS3 folders."
        elif kept.get("complete"):
            keep_text = (f"On: {kept.get('size', 0) / 1e9:.1f} GB kept in {kept.get('folder', '')}. Your PS3 "
                         "Uncharted 3 folders are no longer needed by the import; you can delete them.")
            if kept.get("error"):
                keep_text += f" Last refresh: {kept['error']}."
        elif kept.get("error"):
            keep_text = f"On, not done: {kept['error']}."
        else:
            keep_text = "On: the PS3 files are kept when the next import or update finishes."
        self.u3mp_labels["keep_ps3"].config(text=keep_text)
        self.u3mp_button.config(text="RE-IMPORT / UPDATE" if status in ("imported", "partial", "failed", "running",
                                                                        "image-only") else "IMPORT")
        if running and not self.u3mp_cancel_button.winfo_manager():
            self.u3mp_cancel_button.pack(side="left", padx=(self.px(10), 0), after=self.u3mp_button)
        elif not running and self.u3mp_cancel_button.winfo_manager():
            self.u3mp_cancel_button.pack_forget()
        self.u3mp_draw_bar()

    def u3mp_draw_bar(self):
        n, total, _ = self.u3mp_progress
        bar = self.u3mp_bar
        bar.delete("all")
        width = bar.winfo_width()
        if total and width > 1:
            bar.create_rectangle(0, 0, int(width * min(1.0, n / total)), self.px(6), fill=GOLD, outline=GOLD)

    def u3mp_keep_ps3(self, on):
        """The "keep only the PS3 files updates need" switch: the importer makes (--keep-ps3-only) or deletes
        (--keep-ps3-off) <data>/ps3-kept; the work and its checks are in u3mp/patcher/ps3keep.py."""
        kept = (self.u3mp_state().get("ps3_kept") or {}).get("complete")
        if not on and kept and not any(Path(f).is_dir() for f in self.config.get("u3mp_ps3_u3", [])):
            if not messagebox.askyesno(APP_TITLE, "Your PS3 Uncharted 3 folders are not there any more, so the kept "
                                                  "PS3 files are the only copy the import can use. Turning this off "
                                                  "deletes them, and updates then need your PS3 game again.\n\n"
                                                  "Turn it off anyway?"):
                self.u3mp_keep_widget.set(True)
                return
        if self.u3mp_running():
            self.u3mp_keep_widget.set(not on)
            messagebox.showinfo(APP_TITLE, "The import is running. Change this when it has finished.")
            return
        self.change_config("u3mp_keep_ps3", bool(on))
        if on and self.u3mp_state().get("status") in ("imported", "partial"):
            self.u3mp_import(keep="only")
        elif not on and (self.u3mp_data() / "ps3-kept").exists():
            self.u3mp_import(keep="off")
        self.u3mp_refresh()

    def u3mp_import(self, silent=False, image_only=False, reason="", keep=""):
        """IMPORT / RE-IMPORT, or (silent) the automatic update after a launcher update: the same import with the
        saved folders, no dialogs; image_only relinks just the multiplayer image; keep "only" / "off" makes or deletes
        the kept PS3 files (no import)."""
        if self.u3mp_running():
            if not silent:
                messagebox.showinfo(APP_TITLE, "The import is already running.")
            return False
        game = self.game_dir()
        if not is_game_dir(game):
            if not silent:
                messagebox.showinfo(APP_TITLE, "Choose your PS4 Uncharted: The Nathan Drake Collection folder first "
                                               "(Advanced -> Folders -> Change game folder).")
            return False
        folders = [f for f in self.config.get("u3mp_ps3_u3", []) if Path(f).is_dir()]
        # PS3 folders deleted after "keep only the PS3 files updates need": the importer reads the kept copy
        kept = (self.u3mp_state().get("ps3_kept") or {}).get("complete")
        if not folders and not image_only and not kept and keep != "off":
            if not silent:
                messagebox.showinfo(APP_TITLE, "Add your PS3 Uncharted 3 folder(s) first: the game disc's folder and "
                                               "RPCS3's dev_hdd0/game.")
            return False
        data = self.u3mp_data()
        try:
            data.mkdir(parents=True, exist_ok=True)
            free = shutil.disk_usage(data).free
        except OSError as error:
            if not silent:
                messagebox.showerror(APP_TITLE, f"The multiplayer data folder can't be used:\n{error}")
            return False
        if free < 12 * 1024 ** 3 and not image_only and not keep and (silent or not messagebox.askyesno(
                APP_TITLE, f"{data.anchor} has {free / 1024 ** 3:.0f} GB free; the import needs about 12 GB. "
                           "Start anyway?")):
            if silent:
                print(f"Multiplayer update not started: {data.anchor} has {free / 1024 ** 3:.0f} GB free")
            return False
        python = Path(sys.executable)
        if python.name.lower() == "pythonw.exe" and python.with_name("python.exe").exists():
            python = python.with_name("python.exe")
        command = [str(python), "-u", str(U3MP_DIR / "u3mp_import.py"), "--ps4", str(game),
                   "--data", str(data), "--port", str(ROOT)]
        if self.update_dir():
            command += ["--ps4-update", str(self.update_dir())]
        for folder in folders:
            command += ["--ps3-u3", folder]
        if self.config.get("u3mp_ps3_u2"):
            command += ["--ps3-u2", self.config["u3mp_ps3_u2"]]
        if self.config.get("u3mp_ps3_elf"):
            command += ["--ps3-elf", self.config["u3mp_ps3_elf"]]
        if self.u3mp_dns():
            command += ["--dns", self.u3mp_dns()]
        if image_only:
            command += ["--image-only"]
        if keep:
            command += ["--keep-ps3-" + keep]
        elif self.config.get("u3mp_keep_ps3", False):
            command += ["--keep-ps3"]
        env = os.environ.copy()
        env["PYTHONIOENCODING"] = "utf-8"
        # below normal priority: an import or update never slows a running game down
        flags = NO_WINDOW | getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
        try:
            log_start = (data / "import.log").stat().st_size if keep and (data / "import.log").exists() else 0
            log = open(data / "import.log", "a" if keep else "w", encoding="utf-8")
            self.u3mp_proc = subprocess.Popen(command, cwd=str(U3MP_DIR), env=env, stdout=log, stderr=subprocess.STDOUT,
                                              stdin=subprocess.DEVNULL, creationflags=flags)
            log.close()
        except OSError as error:
            if not silent:
                messagebox.showerror(APP_TITLE, f"Could not start the import:\n{error}")
            else:
                print(f"Could not start the multiplayer update: {error}")
            self.u3mp_proc = None
            return False
        self.u3mp_auto = reason if silent else ""
        self.u3mp_log_pos = log_start  # a keep run appends to the import log: show only its lines
        self.u3mp_progress = (0, 0, "")
        self.u3mp_log.config(state="normal")
        self.u3mp_log.delete("1.0", "end")
        self.u3mp_log.config(state="disabled")
        self.u3mp_step.config(text=("Updating the multiplayer files: " + reason + "...") if silent else
                              ("Keeping the PS3 files..." if keep == "only" else "Removing the kept PS3 files..." if keep else "Starting the import..."))
        self.u3mp_refresh()
        return True

    def u3mp_cancel(self):
        pid = self.u3mp_proc.pid if self.u3mp_proc is not None and self.u3mp_proc.poll() is None else \
            self.u3mp_state().get("pid")
        if pid and messagebox.askyesno(APP_TITLE, "Stop the import? Finished steps are kept; Import continues it."):
            subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], creationflags=NO_WINDOW,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.u3mp_step.config(text="Import stopped.")
            self.root.after(500, self.u3mp_refresh)

    def u3mp_poll(self):
        """Every second: new lines of the import log, its progress events (@U3MP ...), the status."""
        try:
            path = self.u3mp_data() / "import.log"
            if path.exists():
                with open(path, "rb") as f:
                    f.seek(self.u3mp_log_pos)
                    chunk = f.read()
                cut = chunk.rfind(b"\n") + 1
                if cut:
                    self.u3mp_log_pos += cut
                    self.u3mp_lines(chunk[:cut].decode("utf-8", "replace").splitlines())
            if self.u3mp_proc is not None and self.u3mp_proc.poll() is not None:
                self.u3mp_proc = None
                self.u3mp_auto = ""
                self.u3mp_refresh()
            elif self.u3mp_running() != self.u3mp_status.cget("text").startswith(("Importing...", "Updating the")):
                self.u3mp_refresh()
        except (OSError, tk.TclError):
            pass
        try:
            self.u3mp_gate()
        except (OSError, tk.TclError):
            pass
        self.root.after(1000, self.u3mp_poll)

    def u3mp_lines(self, lines):
        shown = []
        for line in lines:
            if line.startswith("@U3MP "):
                try:
                    ev = json.loads(line[6:])
                except ValueError:
                    continue
                if ev.get("phase") == "image":
                    self.u3mp_step.config(text="Multiplayer image: " + ev.get("status", ""))
                elif ev.get("phase") == "recipe" and ev.get("step"):
                    self.u3mp_progress = (ev["step"] - (0 if ev.get("status") != "running" else 1), ev["total"],
                                          ev.get("id", ""))
                    self.u3mp_step.config(text=f"Step {ev['step']}/{ev['total']}: {ev.get('title', '')[:90]}")
                elif ev.get("phase") == "recipe" and ev.get("status") == "failed":
                    shown.append(f"!! step {ev.get('id')} failed: {ev.get('note', '')}")
                elif ev.get("phase") == "ps3keep":
                    self.u3mp_step.config(text=ev.get("note", ""))
                elif ev.get("phase") == "done":
                    n, total, _ = self.u3mp_progress
                    self.u3mp_progress = (total, total, "")
                    self.u3mp_step.config(text="Import finished." if ev.get("status") in ("imported", "partial")
                                          else "Import failed: " + ev.get("error", ""))
                    self.root.after(200, self.u3mp_refresh)
                continue
            shown.append(line)
        if shown:
            self.u3mp_log.config(state="normal")
            self.u3mp_log.insert("end", "\n".join(shown) + "\n")
            excess = int(self.u3mp_log.index("end-1c").split(".")[0]) - 400
            if excess > 0:
                self.u3mp_log.delete("1.0", f"{excess}.0")
            self.u3mp_log.see("end")
            self.u3mp_log.config(state="disabled")
        self.u3mp_draw_bar()

    # -- multiplayer (preview) --------------------------------------------------------------

    def text_entry(self, parent, key, default, width=18):
        """A one-line text box whose text is kept in the launcher's settings under key: a framed
        well (gold frame while typing) with room around the text."""
        value = tk.StringVar(value=str(self.config.get(key, default)))
        well = tk.Frame(parent, bg=BG, highlightthickness=1, highlightbackground=LINE)
        entry = tk.Entry(well, textvariable=value, width=width, font=self.f_body, bg=BG, fg=TEXT,
                         insertbackground=GOLD, relief="flat", highlightthickness=0)
        entry.pack(fill="x", expand=True, padx=self.px(8), pady=self.px(4))
        entry.bind("<FocusIn>", lambda e: well.config(highlightbackground=GOLD))
        entry.bind("<FocusOut>", lambda e: well.config(highlightbackground=LINE))
        well.bind("<Button-1>", lambda e: entry.focus_set())
        value.trace_add("write", lambda *a: self.change_config(key, value.get().strip()))
        return well

    def gold_button(self, parent, label, action, small=False):
        """A main action (Import, Host, Join): a yellow rounded button, like PLAY."""
        return Pill(parent, label, action, self.f_tab if small else self.f_body_bold, self.s, kind="primary",
                    padx=14 if small else 22, pady=5 if small else 9)

    def caps_label(self, parent, text, bg=PANEL):
        return tk.Label(parent, text=text, font=self.f_caps, bg=bg, fg=DIM, anchor="w")

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
        self.update_button = Pill(inner, "UPDATE", self.install_update, self.f_body_bold, self.s, kind="outline",
                                  padx=18, pady=11)
        self.set_status(f"Ready to play {self.current_game()['name']}." if self.config.get("game") else "Ready.")

    def draw_play(self, hover=False):
        canvas = self.play_button
        w, h = int(canvas["width"]), int(canvas["height"])
        canvas.delete("all")
        running = self.process is not None
        if not running and self.u3mp_blocked():
            # Uncharted 3's multiplayer update is pending or running (u3mp_gate): PLAY is held until it finishes
            round_rect(canvas, 2, 2, w - 2, h - 2, self.px(8), fill=PANEL_HI, outline=DIM)
            canvas.create_text(w // 2, h // 2, text="UPDATING...", font=self.f_play, fill=MUTED)
            canvas.config(cursor="arrow")
            return
        canvas.config(cursor="hand2")
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
            if self.config.get("update_mode") == "auto":
                if not self.process:
                    self.set_status(f"Version {version} is available.", "It installs by itself in a moment.")
                self.root.after(1500, self.auto_install)
                return
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

    IMPORT_BUSY = ("A multiplayer import or update is still running in the background. Wait until it has finished "
                   "(Multiplayer tab), then press UPDATE again: the update replaces the files the import is using.")

    def import_running(self):
        """An Uncharted 3 or Uncharted 2 multiplayer import runs with the launcher's Python and tools."""
        try:
            return bool(self.u3mp_running() or (hasattr(self, "u2mp_running") and self.u2mp_running()))
        except Exception:  # noqa: BLE001 - never block an update on a broken state file
            return False

    def install_update(self, auto=False):
        release = self.update_release
        if not release or self.update_busy:
            return
        if self.process:
            messagebox.showinfo(APP_TITLE, "Close the game first: the update replaces the game's runtime.")
            return
        if self.import_running():
            messagebox.showinfo(APP_TITLE, self.IMPORT_BUSY)
            return
        version = updater.version_text(release["version"])
        if not auto and not messagebox.askyesno(APP_TITLE, f"Install version {version} ({release['size'] / 1e6:.0f} MB "
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
        if self.update_staged and self.import_running():
            # an import started while the update downloaded: it uses the launcher's Python, which the update
            # replaces (Windows refuses to move files in use); the update is kept and installs at the next try
            self.update_busy = False
            self.update_staged = None
            self.set_status("Update downloaded.", "Press UPDATE again when the multiplayer import has finished.")
            messagebox.showinfo(APP_TITLE, self.IMPORT_BUSY)
            return
        if self.update_staged:
            self.set_status("Installing the update...", "The launcher starts again in a moment.")
            try:
                updater.start_apply(ROOT, self.update_staged)
            except OSError as error:
                self.update_busy = False
                messagebox.showerror(APP_TITLE, f"The update could not be started:\n\n{error}")
                return
            try:
                self.config["window_xy"] = [self.root.winfo_x(), self.root.winfo_y()]
                write_config(self.config)
            finally:
                # The updater waits for this process to exit: close even if the settings failed.
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
        if key in ("upscaler", "fsr1"):
            self.refresh_render_size()

    def change_upscaling(self, value):
        """An upscaler: a temporal one takes the runtime's anti-aliasing setting (TAA is kept for
        Off and FSR 1); FSR 1 has no Native AA (it would do nothing)."""
        settings = self.game_settings()
        aa = anti_aliasing_choice(self.ini, settings)
        settings["aa"] = aa
        if value == "fsr1" and settings.get("upscale", "off") == "off":
            settings["upscale"] = "quality"
        write_config(self.config)
        self.ini["fsr1"] = "1" if value == "fsr1" else "0"
        self.save_ini({"fsr1": self.ini["fsr1"]})
        self.change("upscaler", value if value in TEMPORAL_UPSCALERS else aa)

    def change_anti_aliasing(self, value):
        self.change_game_setting("aa", value)
        self.change("upscaler", value)

    def change_resolution(self, value):
        self.game_settings()["resolution"] = value
        self.game_settings()["preset"] = "custom"
        write_config(self.config)
        self.redraw_presets()
        self.refresh_render_size()

    def change_aspect(self, value):
        self.change_game_setting("aspect", value)
        self.refresh_render_size()

    def refresh_render_size(self):
        """The resolution labels and the exact render size for the selected game's aspect ratio."""
        settings = self.game_settings()
        aspect = settings.get("aspect", "16:9")
        if isinstance(self.widgets.get("resolution"), Segmented):
            self.widgets["resolution"].relabel(resolution_options(aspect))
        if hasattr(self, "render_size_note"):
            # With upscaling on, the line under its presets shows the sizes instead.
            render, output = game_sizes(settings, upscaling_choice(self.ini))
            self.render_size_note.config(
                text="" if output else f"Render size: {render.replace('x', ' × ')}")
            if output and self.render_size_note.winfo_manager():
                self.render_size_note.pack_forget()
            elif not output and not self.render_size_note.winfo_manager() and "aspect" in self.widgets:
                self.render_size_note.pack(side="top", fill="x", pady=(0, self.px(10)),
                                           after=self.widgets["resolution"].master)
        self.refresh_upscale()

    def refresh_upscale(self):
        """The upscaler, its quality row (with an upscaler only, right under it) with the size the
        selected game draws at, and the anti-aliasing (greyed with a temporal upscaler)."""
        if not hasattr(self, "upscale_row"):
            return
        settings = self.game_settings()
        upscaling = upscaling_choice(self.ini)
        self.widgets["upscaling"].set(upscaling)
        if upscaling != "off":
            if not self.upscale_row.winfo_manager():
                self.upscale_row.pack(side="top", fill="x", pady=(0, self.px(10)),
                                      after=self.widgets["upscaling"].master)
        elif self.upscale_row.winfo_manager():
            self.upscale_row.pack_forget()
        quality = self.widgets["upscale"]
        presets = [option for option, _ in UPSCALE_PRESETS]
        quality.show(presets[1:] if upscaling == "fsr1" else presets)
        quality.relabel([("off", "DLAA" if upscaling == "dlss" else "Native AA")])
        quality.set(settings.get("upscale", "off"))
        aa = self.widgets["aa"]
        aa.set(anti_aliasing_choice(self.ini, settings))
        aa.set_enabled(upscaling not in TEMPORAL_UPSCALERS)
        render, output = game_sizes(settings, upscaling)
        self.upscale_note.config(
            text=f"Draws {render.replace('x', ' × ')}, upscaled to {output.replace('x', ' × ')}"
            if output else "")

    def change_game_setting(self, key, value):
        self.game_settings()[key] = value
        if key in ("mouse", "mouse_sens", "mouse_invert", "prompts", "mouse_aim_sens", "mouse_vertical",
                   "mouse_invert_x", "vibration"):
            self.save_controls()
        else:
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

    def play_game(self, key):
        """A double-click on a game's card: choose it and play."""
        self.select_game(key)
        if not self.process:
            self.play()

    def play(self):
        """Start the chosen game."""
        if not RUN_SCRIPT.exists():
            messagebox.showerror(APP_TITLE, f"Start script missing:\n{RUN_SCRIPT}")
            return
        if not is_game_dir(self.game_dir()):
            messagebox.showinfo(APP_TITLE, "Select your Uncharted: The Nathan Drake Collection game folder "
                                           "(CUSA02344, the folder with eboot.bin and sce_sys).")
            if not self.choose_game_dir():
                return
        if not self.confirm_without_update() or not self.confirm_update_files() or \
                not self.confirm_slow_drive() or not self.u3mp_confirm_play():
            return
        self.follow_ini()  # the in-game menu's last changes, not older values kept here
        self.save_ini({k: self.ini.get(k, v) for k, v in INI_DEFAULTS.items()})
        write_config(self.config)
        game = self.current_game()
        env = os.environ.copy()
        env["GOW3_TITLE_SUFFIX"] = self.title_suffix()   # the game window's title (gow3gpu.cpp), the same as the launcher's
        env["UNCHARTED_DIAGNOSTICS"] = "1" if self.config.get("diagnostics", True) else "0"
        env["UNCHARTED_TORCH_FORWARD"] = "1" if self.config.get("torch_forward", True) else "0"
        env["UNCHARTED_JOB_SLEEP"] = {"off": "0", "on": "1"}.get(str(self.config.get("job_sleep", "auto")), "auto")
        # renderer_vulkan/vk_instance.cpp: 1 on, auto (cards of 4.5 GiB or less), anything else off
        env["GOW3_LOW_VRAM"] = {"on": "1", "auto": "auto"}.get(str(self.config.get("low_vram", "off")), "0")
        env["UNCHARTED_LANGUAGE"] = str(self.config.get("language", "1"))
        env["UNCHARTED_SPEAKERS"] = str(self.config.get("speakers", "auto"))
        env["UNCHARTED_U3_EXPOSURE"] = str(self.config.get("u3_exposure", "0"))
        env["UNCHARTED_ANISOTROPY"] = str(self.config.get("anisotropy", "0"))
        env["UNCHARTED_HIRES_SHADOWS"] = "1" if self.config.get("hires_shadows", False) else "0"
        env["UNCHARTED_FULL_RES_TEXTURES"] = "1" if self.config.get("full_res_textures", False) else "0"
        env["UNCHARTED_NO_DOF"] = "1" if self.config.get("no_dof", False) else "0"
        env["GOW3_MOVIE_STRETCH"] = "1" if self.config.get("movie_stretch", False) else "0"
        env["UNCHARTED_UNLOCK"] = "1" if self.config.get("unlock", False) else "0"
        env["UNCHARTED_UNLOCK_REWARDS"] = "1" if self.config.get("unlock_rewards", False) else "0"
        self.u3mp_env(env)  # Uncharted 3 multiplayer (Multiplayer tab)
        self.u2mp_env(env)  # Uncharted 2 multiplayer (Multiplayer tab while Uncharted 2 is chosen)
        if self.chosen_update():
            env["UNCHARTED_UPDATE"] = str(self.chosen_update())  # tools/probe_uncharted.py find_update
        env["UNCHARTED_CHEATS"] = ",".join(name for name, *_ in CHEATS if self.config.get("cheat_" + name, False))
        # Every game's own options: the Collection menu starts the others itself, and
        # tools/probe_uncharted.py applies the started game's (its settings file decides the
        # vblank rate for frame rates above 60).
        env.pop("GOW3_VBLANK_HZ", None)
        for key in GAME_KEYS:
            settings = self.config["games"][key]
            # With upscaling the game draws below the resolution chosen, upscaled to it
            # (tools/probe_uncharted.py).
            render, output = game_sizes(settings, upscaling_choice(read_ini(key)))
            env[f"UNCHARTED_RESOLUTION_{key.upper()}"] = render
            if output:
                env[f"UNCHARTED_OUTPUT_RES_{key.upper()}"] = output
            else:
                env.pop(f"UNCHARTED_OUTPUT_RES_{key.upper()}", None)
            env[f"UNCHARTED_MULTITHREADED_{key.upper()}"] = "1" if settings["multithreaded"] else "0"
            env[f"UNCHARTED_SAVE_PROFILE_{key.upper()}"] = str(settings.get("save_profile", "1"))
            # Controls: in each game's settings file (save_controls), where its menu changes them.
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
        for line in reversed(tail):
            if line.startswith("Hang watchdog:") and " report " in line:
                return ("The game froze. A report of where it stopped was saved:\n"
                        + line.split(" report ", 1)[-1] + "\n\nPlease attach it with the log folder's "
                        "boot.log to a GitHub issue.")
        # A crash (exit code 3): the runtime's fault report names where it happened. Players
        # reported just "error code 3" (GitHub #24, #27); the report is what tells the cause.
        for i, line in enumerate(tail):
            if line.startswith("Fault:"):
                where = next((l.strip() for l in tail[i + 1:i + 6] if l.lstrip().startswith(("host #0", "#0"))), "")
                return (line.strip() + (f"\n{where}" if where else "") + "\n\nPlease attach boot.log from the "
                        "log folder (and your graphics card, game version and settings) to a GitHub issue.")
        for i, line in enumerate(tail):
            if line.startswith("STOP:") or "Unreachable code" in line or "Assertion" in line:
                context = [l for l in tail[max(0, i - 2):i + 1] if l.strip()]
                return "\n".join(context)[-600:]
        return None

    def confirm_update_files(self):
        """An update folder with missing or damaged files (#43: a bin.psarc the games couldn't read
        stopped all three at start with "ASSERTION: root") is named before the game starts."""
        update = self.update_dir()
        if not update:
            return True
        try:
            problems = self.update_installer().check_update(update)
        except Exception:  # the check itself must never keep a game from starting
            return True
        if not problems:
            return True
        return messagebox.askokcancel(
            APP_TITLE, "Your game update folder has missing or damaged files:\n\n" + "\n".join(problems[:6]) +
            f"\n\n{update}\n\nThe games will probably stop at start. Extract the update again (or use Install "
            "from .pkg under Advanced -> GAME UPDATE). Play anyway?")

    def confirm_without_update(self):
        """Without update 1.02 most of the port's game fixes (crash fixes among them) are skipped:
        asks once per game folder (a 1.02 copy with the update merged in reports 1.02 itself)."""
        folder = self.game_dir()
        version = app_version(folder)
        if self.update_dir() or not version or version >= "01.02" or \
                self.config.get("warned_no_update") == str(folder):
            return True
        title = folder.parent if folder.name.lower() == "uroot" else folder
        if not messagebox.askokcancel(
                APP_TITLE, f"Your game is version {version.lstrip('0')}, without update 1.02.\n\n"
                "Most of this port's fixes, crash fixes among them, are written for update 1.02, so "
                "without it the games crash more often.\n\nDump update 1.02 from your PS4 and extract it "
                f"next to the game folder as {title.name}-patch (the Advanced tab then shows it).\n\n"
                "Play anyway?"):
            return False
        self.config["warned_no_update"] = str(folder)
        write_config(self.config)
        return True

    def confirm_slow_drive(self):
        """The port or the game on a USB, SD or FAT/exFAT drive (slow_drive_reason): asks once per
        pair of folders."""
        places = []
        for label, folder in (("This port", ROOT), ("The game", self.game_dir())):
            try:
                reason = slow_drive_reason(folder)
            except (OSError, ValueError, ctypes.ArgumentError):
                reason = None
            if reason:
                places.append(f"{label} ({folder}): {reason}.")
        key = f"{ROOT}|{self.game_dir()}"
        if not places or self.config.get("warned_slow_drive") == key:
            return True
        if not messagebox.askokcancel(
                APP_TITLE, "\n".join(places) + "\n\nOn such drives the games freeze for up to a second, "
                "again and again, while the port writes its shader cache, logs and saves (Windows writes "
                "to them without a cache).\n\nFor smooth play, copy this port folder and the game folder "
                "to an internal drive (an SSD such as C:) and start the launcher from there.\n\n"
                "Play anyway?"):
            return False
        self.config["warned_slow_drive"] = key
        write_config(self.config)
        return True

    def play_direct(self):
        self.play()
        if not self.process:  # no game folder chosen, or the start failed (already reported)
            self.root.destroy()

    def finished(self, code):
        self.process = None
        self.draw_play()
        reason = self.crash_reason()
        # A freeze is reported however the game was closed (STOP, or Task Manager).
        froze = bool(reason) and reason.startswith("The game froze")
        if (code in (0, None) or getattr(self, "stopping", False)) and not froze:
            self.stopping = False
            self.set_status("Game closed.", "Ready to play again.")
        else:
            self.stopping = False
            self.set_status("The game froze." if froze else "The game stopped unexpectedly.",
                            "A report was saved; see the log folder." if froze
                            else f"Exit code {code}. See the log for details.")
            if reason:
                # 24 Oct 2026 (#148, #163, #172): crashes of a 1.00 game were reported as port bugs;
                # most of the port's fixes are written for update 1.02.
                version = app_version(self.game_dir())
                if not self.update_dir() and version and version < "01.02":
                    reason += (f"\n\nYour game is version {version.lstrip('0')} without update 1.02, so most of "
                               "the port's crash fixes are skipped. Install update 1.02 first (Advanced -> "
                               "GAME UPDATE).")
                if messagebox.askyesno(APP_TITLE, f"The game stopped with an error:\n\n{reason}\n\nOpen the log folder?"):
                    self.open_logs()
        if self.direct:
            self.root.after(200, self.root.destroy)

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

    def make_shortcut(self, name, description, play=None):
        """A desktop shortcut to the launcher, or with `play` (u1/u2/u3) to that game directly."""
        link = desktop_folder() / f"{name}.lnk"
        pythonw = Path(sys.executable).with_name("pythonw.exe")
        target = pythonw if pythonw.exists() else Path(sys.executable)
        arguments = f"\"{Path(__file__).resolve()}\""
        if STARTER_EXE.exists():
            target, arguments = STARTER_EXE, ""
        if play:
            arguments = f"{arguments} --play {play}".strip()
        def ps(value):  # a PowerShell single-quoted string ("Drake's" doubles the quote)
            return "'" + str(value).replace("'", "''") + "'"
        # Messages in UTF-8 (a German or Portuguese Windows answers in its own language).
        script = ("[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;"
                  f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut({ps(link)});"
                  f"$s.TargetPath={ps(target)};$s.Arguments={ps(arguments)};"
                  f"$s.WorkingDirectory={ps(ROOT)};$s.IconLocation={ps(ASSETS / 'uncharted.ico')};"
                  f"$s.Description={ps(description)};$s.Save()")
        result = subprocess.run(["powershell", "-NoProfile", "-Command", script], creationflags=NO_WINDOW,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        return link, result

    def update_installer(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("install_update", ROOT / "tools" / "install_update.py")
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        return installer

    def choose_update_folder(self):
        """An update the player already extracted, anywhere: checked and remembered (nothing is
        copied); the games get it as UNCHARTED_UPDATE."""
        if self.process:
            messagebox.showwarning(APP_TITLE, "Close the game first.")
            return
        game = self.game_dir()
        if not is_game_dir(game):
            messagebox.showwarning(APP_TITLE, "Choose the game folder first (Change game folder).")
            return
        from tkinter import filedialog
        folder = filedialog.askdirectory(title="Choose the extracted update 1.02 folder", parent=self.root,
                                         mustexist=True)
        if not folder:
            return
        installer = self.update_installer()
        try:
            update = installer.check_folder(folder, game)
        except installer.UpdateError as error:
            messagebox.showerror(APP_TITLE, f"This folder can't be used:\n{error}")
            return
        self.config["update_dir"] = str(update)
        write_config(self.config)
        self.update_label.config(text=self.update_text())
        messagebox.showinfo(APP_TITLE, f"Game update {app_version(update) or '?'} is used from now on:\n{update}")

    def install_game_update(self):
        """The player's own update PKG unpacked next to the game folder (tools/install_update.py;
        the port ships no game files)."""
        if self.process:
            messagebox.showwarning(APP_TITLE, "Close the game first.")
            return
        game = self.game_dir()
        if not is_game_dir(game):
            messagebox.showwarning(APP_TITLE, "Choose the game folder first (Change game folder).")
            return
        from tkinter import filedialog
        pkg = filedialog.askopenfilename(title="Choose the game update (.pkg)", parent=self.root,
                                         filetypes=[("PS4 package", "*.pkg"), ("All files", "*.*")])
        if not pkg:
            return
        installer = self.update_installer()
        existing = installer.existing_update(game)
        if existing and not messagebox.askyesno(
                APP_TITLE, f"There is an update folder already:\n{existing}\n\nReplace it with this update?"):
            return
        state = {"step": "Checking the update..."}

        def work():
            try:
                state["path"] = installer.install(pkg, game, log=lambda text: state.__setitem__("step", text))
            except installer.UpdateError as error:
                state["error"] = str(error)
            except Exception as error:  # a disk or permission problem: shown as it is
                state["error"] = f"{type(error).__name__}: {error}"

        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            if worker.is_alive():
                self.update_label.config(text=state["step"])
                self.root.after(200, poll)
                return
            if "path" in state:  # the installed one is used, also over a folder chosen before
                self.config["update_dir"] = str(state["path"])
                write_config(self.config)
            self.update_label.config(text=self.update_text())
            if "error" in state:
                messagebox.showerror(APP_TITLE, f"The update was not installed:\n{state['error']}")
            else:
                messagebox.showinfo(APP_TITLE, f"Update installed:\n{state['path']}\n\nThe games use it from "
                                               "their next start.")
        poll()

    def check_game_files(self):
        """Every file of the base game present with the right size (tools/check_game_files.py: a
        list of names and sizes, no game data). 29 Oct 2026: Uncharted 3 chapter 8 stopped for
        players whose dump lacked one script file (#247, #296)."""
        folder = self.game_dir()
        if not folder:
            messagebox.showwarning(APP_TITLE, "Choose the game folder first.")
            return
        try:
            result = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "check_game_files.py"), "check",
                                     str(folder)], capture_output=True, text=True, timeout=600,
                                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except (OSError, subprocess.SubprocessError) as error:
            messagebox.showerror(APP_TITLE, f"The check could not run:\n\n{error}")
            return
        lines = result.stdout.splitlines()
        if result.returncode == 0:
            messagebox.showinfo(APP_TITLE, "Your game files are complete.\n\n" + (lines[0] if lines else ""))
            return
        problems = lines[1:]
        shown = "\n".join(problems[:15]) + (f"\n... and {len(problems) - 15} more" if len(problems) > 15 else "")
        messagebox.showerror(APP_TITLE, "Your game files are incomplete, so the game will stop where it needs "
                             "them. Extract or dump the base game again.\n\n" + (lines[0] if lines else "")
                             + "\n\n" + shown)

    def install_game_pkg(self):
        """The player's own game PKG (for example the US copy, CUSA02320) unpacked into a folder
        they choose, with the metadata the launcher needs (tools/install_update.py install_game;
        the port ships no game files). That folder becomes the game folder."""
        if self.process:
            messagebox.showwarning(APP_TITLE, "Close the game first.")
            return
        from tkinter import filedialog
        pkg = filedialog.askopenfilename(title="Choose the game (.pkg), not the update", parent=self.root,
                                         filetypes=[("PS4 package", "*.pkg"), ("All files", "*.*")])
        if not pkg:
            return
        parent = filedialog.askdirectory(title="Choose where to put the game (about 60 GB)", parent=self.root,
                                         initialdir=str(Path(pkg).parent))
        if not parent:
            return
        installer = self.update_installer()
        state = {"step": "Checking the game package..."}

        def work():
            try:
                state["path"] = installer.install_game(pkg, parent,
                                                       log=lambda text: state.__setitem__("step", text))
            except installer.UpdateError as error:
                state["error"] = str(error)
            except Exception as error:  # a disk or permission problem: shown as it is
                state["error"] = f"{type(error).__name__}: {error}"

        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            if worker.is_alive():
                self.game_dir_label.config(text=state["step"])
                self.root.after(200, poll)
                return
            if "path" in state:
                self.config["game_dir"] = str(state["path"])
                if WINE:
                    self.config["game_dir_unix"] = unix_path(str(state["path"]))
                write_config(self.config)
            self.game_dir_label.config(text=f"Game files: {self.game_dir()}")
            self.update_label.config(text=self.update_text())
            if "error" in state:
                messagebox.showerror(APP_TITLE, f"The game was not installed:\n{state['error']}")
            else:
                messagebox.showinfo(APP_TITLE, f"Game installed:\n{state['path']}\n\nIt is the game folder now. "
                                               "Next, install update 1.02: GAME UPDATE -> Install from .pkg.")
        poll()

    def desktop_shortcut(self):
        link, result = self.make_shortcut("Uncharted", APP_TITLE)
        if result.returncode == 0:
            messagebox.showinfo(APP_TITLE, f"Shortcut created:\n{link}")
        else:
            messagebox.showerror(APP_TITLE, f"Could not create the shortcut:\n{result.stderr.strip()}")

    def game_shortcuts(self):
        """One desktop shortcut per game: starts it directly with the launcher's settings."""
        made, failed = [], []
        for game in GAMES:
            name = game["name"].replace(":", " -")
            link, result = self.make_shortcut(name, f"{game['name']} (starts directly)", play=game["key"])
            (made if result.returncode == 0 else failed).append(link.name if result.returncode == 0
                                                                else result.stderr.strip())
        if failed:
            messagebox.showerror(APP_TITLE, "Could not create the shortcuts:\n" + "\n".join(failed))
        else:
            messagebox.showinfo(APP_TITLE, "Shortcuts created on the desktop:\n" + "\n".join(made) +
                                "\n\nEach starts its game directly with the launcher's settings. From a command "
                                "prompt: \"Uncharted Launcher.exe\" --play u1 (or u2, u3).")

    def add_to_steam(self, per_game=False):
        """Adds the Collection (or each game, started with --play) to Steam's library as a non-Steam
        game, with pictures made from the game's own files (launcher/steam_library.py)."""
        try:
            import steam_library
        except ImportError as error:
            messagebox.showerror(APP_TITLE, f"Add to Steam is missing a launcher file:\n{error}")
            return
        while steam_library.steam_running():
            if not messagebox.askretrycancel(
                    APP_TITLE, "Steam is open. Steam saves its own list of games when it closes, so a game "
                               "added now would be lost.\n\nClose Steam completely (Steam menu -> Exit, or "
                               "right-click its icon by the clock -> Exit), then press Retry."):
                return
        games = [(game["key"], game["name"]) for game in GAMES] if per_game else None
        targets = steam_library.launch_targets(ROOT, STARTER_EXE, sys.executable,
                                               Path(__file__).resolve(), games)
        names = [steam_library.COLLECTION_NAME] + [game["name"] for game in GAMES]
        game_dir = self.game_dir() if is_game_dir(self.game_dir()) else None
        self.root.config(cursor="watch")
        self.root.update_idletasks()
        try:
            results = steam_library.add_to_steam(targets, ASSETS / "uncharted.ico", game_dir, all_names=names)
        except steam_library.SteamError as error:
            if error.kind == "no_steam":
                text = ("Steam was not found on this PC (no Steam account folder in Steam's userdata). "
                        "Install Steam and sign in once, then try again.")
                if WINE:
                    text += ("\n\nOn Linux, add the launcher from Steam itself: Games -> Add a Non-Steam "
                             "Game to My Library.")
            else:
                text = ("Steam's list of non-Steam games could not be read, so it was left as it is:\n"
                        f"{error.detail}")
            messagebox.showerror(APP_TITLE, text)
            return
        except OSError as error:
            messagebox.showerror(APP_TITLE, f"Could not write Steam's list of games:\n{error}")
            return
        finally:
            self.root.config(cursor="")
        lines = []
        for folder, name, result in results:
            who = f"{name} ({folder.name})" if name else f"Steam account {folder.name}"
            changes = [f"{count} {word}" for count, word in ((result["added"], "added"),
                                                            (result["updated"], "updated"),
                                                            (result["removed"], "replaced")) if count]
            lines.append(f"{who}: {', '.join(changes)}")
        added = "the three games" if per_game else f"\"{steam_library.COLLECTION_NAME}\""
        pictures = ("Library pictures come from your game files."
                    if game_dir else "No library pictures: choose the game folder first, then add again.")
        backups = [str(result["backup"]) for _, _, result in results if result["backup"]]
        messagebox.showinfo(APP_TITLE, f"Added {added} to Steam:\n" + "\n".join(lines) +
                            f"\n\n{pictures}" +
                            (f"\n\nThe previous list was saved as:\n" + "\n".join(backups) if backups else "") +
                            f"\n\nStart Steam again: {'they are' if per_game else 'it is'} in your Library.")

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
    # --play u1|u2|u3 (or 1/2/3): start that game directly (desktop game shortcuts, GitHub #25).
    direct = None
    if "--play" in sys.argv[1:]:
        at = sys.argv.index("--play")
        value = sys.argv[at + 1].lower() if at + 1 < len(sys.argv) else ""
        direct = value if value in GAME_KEYS else f"u{value}" if f"u{value}" in GAME_KEYS else None
    root = tk.Tk()
    if direct:
        root.withdraw()
    # Errors in button and timer callbacks go to the error log too (pythonw has no console).
    root.report_callback_exception = lambda kind, value, trace: log_launcher_error(kind, value, trace)
    Launcher(root, direct)
    root.mainloop()


def log_launcher_error(kind, value, trace):
    """Appends an error to analysis/uncharted-user/launcher-error.log: the launcher runs under
    pythonw, so an error at start used to close it without a word (GitHub #21, 0.2.1)."""
    import traceback
    text = "".join(traceback.format_exception(kind, value, trace))
    path = USER_DIR / "launcher-error.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a", encoding="utf-8") as log:
            log.write(time.strftime("%Y-%m-%d %H:%M:%S ") + APP_TITLE + "\n" + text + "\n")
    except OSError:
        pass
    return text, path


if __name__ == "__main__":
    try:
        main()
    except Exception:
        text, path = log_launcher_error(*sys.exc_info())
        try:
            fallback = tk.Tk()
            fallback.withdraw()
            messagebox.showerror(APP_TITLE, f"The launcher could not start:\n\n{text[-1500:]}\n"
                                            f"This was saved to:\n{path}")
            fallback.destroy()
        except Exception:
            pass
        raise
