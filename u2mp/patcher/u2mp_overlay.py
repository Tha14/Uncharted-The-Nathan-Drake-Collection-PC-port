"""Uncharted 2 multiplayer (u2mp): the /app0 overlay the launcher lays over Uncharted 2.

Made on the player's PC from their own PS4 game; nothing game-derived ships with the launcher.

  u2data/build/main/dc1/ps4/menu.bin   the main menu with a Multiplayer button

The Collection's menu script still holds the multiplayer pages (PAGE_MULTIPLAYER and the rest) and
the MENU_MULTIPLAYER text, but no button leads there. The main menu keeps a hidden button for the
multiplayer medals page (BUTTON_MULTIMEDALS, a go-to-page button with a visibility condition); this
turns it into Multiplayer: label MENU_MULTIPLAYER, target PAGE_MULTIPLAYER, always visible.

menu.bin is Naughty Dog DC data: 64-bit little-endian pointers are offsets into the file, names
are NUL-terminated strings, and every name has a 32-bit id, the CRC-32 of the name (MSB first,
polynomial 0x04C11DB7, no initial value, no final xor). A menu control record starts with a
pointer to its name:
  +0x00 name           +0x08 name id
  +0x20 label          +0x28 label id
  +0x30 action (u32)   +0x34 visibility condition (u32, 0 = always)
  +0x48 target page    +0x50 target page id
Every value is checked before anything is changed; the source file is only read.

Rebuilt only when the source changed (state.json). The launcher passes the overlay as
UNCHARTED_U2MP_OVERLAY (tools/probe_uncharted.py lays it over /app0 for Uncharted 2 only).

usage: python u2mp/patcher/u2mp_overlay.py <game folder> <overlay folder> [--update <folder>]
"""
import argparse
import json
import struct
import sys
from pathlib import Path

MENU = Path("u2data/build/main/dc1/ps4/menu.bin")
VERSION = 1  # bump when the overlay changes for the same sources


def name_id(name):
    """A DC name's id: CRC-32, MSB first, polynomial 0x04C11DB7, init 0, no final xor."""
    crc = 0
    for byte in name.encode("ascii"):
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else crc << 1
            crc &= 0xFFFFFFFF
    return crc


def cstring(data, offset):
    """The NUL-terminated ASCII name at offset, or None."""
    if not 0 < offset < len(data):
        return None
    end = data.find(b"\0", offset, offset + 96)
    if end <= offset:
        return None
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError:
        return None


def strings(data, name):
    """Offsets of every whole copy of `name` (a NUL-terminated string not part of a longer name)."""
    found, needle, at = [], name.encode("ascii") + b"\0", 0
    while (at := data.find(needle, at)) >= 0:
        before = data[at - 1] if at else 0
        if not (chr(before).isalnum() or before == ord("_")):
            found.append(at)
        at += 1
    return found


def records_named(data, name):
    """Offsets of 8-byte-aligned pointers to a copy of `name`: the records that start with that name."""
    targets = set(strings(data, name))
    return [at for at in range(0, len(data) - 0x58, 8)
            if struct.unpack_from("<Q", data, at)[0] in targets]


def patch_menu(data):
    """Turn the main menu's hidden medals button into Multiplayer. Returns the patched records."""
    pages = strings(data, "PAGE_MULTIPLAYER")
    if not pages:
        raise ValueError("menu.bin has no PAGE_MULTIPLAYER (not Uncharted 2?)")
    label_id, page_id = name_id("MENU_MULTIPLAYER"), name_id("PAGE_MULTIPLAYER")
    done = []
    for record in records_named(data, "BUTTON_MULTIMEDALS"):
        own_id, = struct.unpack_from("<I", data, record + 0x08)
        target, = struct.unpack_from("<Q", data, record + 0x48)
        target_id, = struct.unpack_from("<I", data, record + 0x50)
        if own_id != name_id("BUTTON_MULTIMEDALS") or cstring(data, target) != "PAGE_MULTIMEDALS" \
                or target_id != name_id("PAGE_MULTIMEDALS"):
            continue
        struct.pack_into("<Q", data, record + 0x28, label_id)
        struct.pack_into("<Q", data, record + 0x48, pages[0])
        struct.pack_into("<Q", data, record + 0x50, page_id)
        struct.pack_into("<I", data, record + 0x34, 0)
        done.append(record)
    if not done:
        raise ValueError("menu.bin has no hidden medals button (not the Uncharted 2 1.02 menu?)")
    return done


def find(roots, rel):
    for root in roots:
        for base in (root, root / "uroot"):
            if (base / rel).is_file():
                return base / rel
    return None


def stamp(path):
    st = path.stat()
    return [str(path), st.st_size, int(st.st_mtime)]


def build(game, out, update=None, log=print):
    """Make (or keep) the overlay in `out`; returns `out`, or raises with the reason."""
    menu = find([r for r in (update, game) if r], MENU)
    if menu is None:
        raise FileNotFoundError(f"{MENU} not found in the game")
    want = {"version": VERSION, "menu": stamp(menu)}
    state = out / "state.json"
    try:
        if json.loads(state.read_text(encoding="utf-8")) == want and (out / MENU).is_file():
            return out
    except (OSError, ValueError):
        pass
    data = bytearray(menu.read_bytes())
    records = patch_menu(data)
    target = out / MENU
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_suffix(".tmp")
    tmp.write_bytes(data)
    tmp.replace(target)
    state.write_text(json.dumps(want), encoding="utf-8")
    log(f"u2mp: Multiplayer button in the main menu ({len(records)} records) -> {target}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--update", type=Path)
    a = ap.parse_args()
    build(a.game, a.out, a.update)
    return 0


if __name__ == "__main__":
    sys.exit(main())
