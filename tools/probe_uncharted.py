"""Prepare and run Uncharted: The Nathan Drake Collection on the local runtime.

The launcher (launcher/uncharted_launcher.pyw) starts this with --boot --seconds 0. Its
environment carries the options: UNCHARTED_DIAGNOSTICS=0 leaves out the measurement logging and
live trigger files, GOW3_VBLANK_HZ=0 unlocks the frame rate (480 Hz vblank, menu limit).
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import shutil
import struct

ROOT = Path(__file__).resolve().parent.parent

# Code patches, applied to the linked image by byte pattern (hex, "??" = any byte). A patch lists
# variants (description, [(pattern, offset in pattern, original bytes, replacement bytes), ...]):
# the first variant whose pattern occurs exactly once is applied. The variants cover the game
# versions 1.00 and 1.02 (the update recompiled all three executables).
def pattern_regex(text):
    import re
    text = text.replace(" ", "")
    return re.compile(b"".join(b"." if text[i:i + 2] == "??" else re.escape(bytes.fromhex(text[i:i + 2]))
                               for i in range(0, len(text), 2)), re.S)


# The wait for a frame's GPU end-of-pipe stamp gives up after 2.0 s (vucomiss [2.0f]; ja). On PC
# the command processor can pause that long (shader compilation, readbacks), never on a PS4.
# 1.00 then asserts m_gfxEopTick of frame N-3 in FrameBegin (fg-draw-mgr.cpp:234; U1 0x3fd816);
# 1.02 halts right there (int 0x41; U1 0x4fdf96). NOP the `ja` that leaves the loop on timeout.
# The three games have the same loop; only the rip-relative offset of the 2.0f differs.
GPU_WAIT_PATCH = ("wait for GPU frame completion without the 2 s timeout", [
    ("c5f258c0 c5f82e05 ???????? 7739 f390 beffffffff", 12, bytes.fromhex("7739"), bytes.fromhex("9090")),
    ("c5f258c0 c5f82e05 ???????? 7710 f390 498b00", 12, bytes.fromhex("7710"), bytes.fromhex("9090")),
])
EBOOT_PATCHES = [
    GPU_WAIT_PATCH,
    # Silent sound effects (footsteps, gunfire, ambience; music and speech fine): the sound config
    # loader (biglib audio acm 0x515a50) opens reverbs/mixers/environments/prototypes.txt through
    # the sound file callback 0x8ef80 with flag 1, which `testb $5,%cl` at 0x8f1da turns into O_CREAT.
    # Our Fios2 rejects READ|CREAT on read-only /app0 with SCE_FIOS_ERROR_READ_ONLY (0x80820015,
    # mapped to EROFS 0x1e) before any kernel call, so no sound prototypes load and every effect
    # request resolves to nothing. Test bit 2 only: a read request opens read-only. (1.00 and 1.02)
    ("sound config files open read-only (no O_CREAT on /app0)", [
        ("4a8d542b14f6c1050f95c00fb6c0c1e0068db000020000f6c106", 5,
         bytes.fromhex("f6c105"), bytes.fromhex("f6c104")),
    ]),
]

# Uncharted 2's player torch aims with the camera. The torch update (Flashlight Active
# 0x69a190) aims the light along the torch joint (camera mode 9: the camera's horizontal forward,
# 0x69afbd), and then, every frame, the player light (record 0 of the light table 0x1157760) is
# re-aimed: target = pos + dir * K, dir = the camera's forward vector (0x104e820 + 0x270),
# pos = target - dir * K (function 0x698b50 and a copy inlined next to UpdatePadLightBar,
# 0x3fd78d). Turn the camera to face Nate and the light falls on the wall behind him. Skip both
# re-aims and take the joint path in every camera mode: the light follows the torch, like the
# beam drawn from it. Optional (UNCHARTED_TORCH_FORWARD=0 keeps the original); 1.02 only (1.00
# lays this code out differently).
TORCH_FORWARD_PATCHES = [
    ("Uncharted 2 torch: no re-aim along the camera (frame update copy)", [
        ("488d05cc9fd500 8b4850 84c9 7440 8a50", 12, bytes.fromhex("7440"), bytes.fromhex("eb40")),
    ]),
    ("Uncharted 2 torch: no re-aim along the camera (flashlight.cpp)", [
        ("488d0509ecab00 8b4850 84c9 7440 8a50", 12, bytes.fromhex("7440"), bytes.fromhex("eb40")),
    ]),
    ("Uncharted 2 torch: aimed by its joint in every camera mode", [
        ("833df040de0009 0f8534010000 4885c00f842b010000", 7,
         bytes.fromhex("0f8534010000"), bytes.fromhex("90e934010000")),
    ]),
]

# Idle job workers sleep (all three games, 1.02). The engine's 6 NdJobWorkerThreads spin on their
# job queues with `pause` unless the job manager's sleep flag (byte +0x59, set by its own
# SetSleepMode, U3 0xd96440) is on; with it on, an idle worker waits on the manager's condition
# variable after 1000 spins (10 ms timeout) and every job submission broadcasts to wake them. On
# the PS4 each worker owns a core and the flag stays off; on a 4-core PC the six spinning workers
# took 390% of 400% CPU (GPU command thread 5%, title screen 19-27 FPS on an RTX 5080). Treat the
# flag as on in the worker loop, its re-check before waiting, and the submit path.
# UNCHARTED_JOB_SLEEP=0 keeps the original.
JOB_SLEEP_PATCHES = [
    ("idle job workers sleep (worker loop)", [
        ("4d8d7001 807f5900 74?? 4981f8e8030000 7c??", 8, bytes.fromhex("74"), bytes.fromhex("9090")),
    ]),
    ("idle job workers sleep (re-check before waiting)", [
        ("498b07 80785900 74?? 4889c7 4883ef80", 7, bytes.fromhex("74"), bytes.fromhex("9090")),
    ]),
    ("idle job workers sleep (job submission wakes them)", [
        ("80785900 0f84????0000 4883c060", 4, bytes.fromhex("0f84"), bytes.fromhex("660f1f440000")),
    ]),
]

# The games' own Options > Controls > Aiming > Sensitivity (1.02): a 10-notch slider from 0 to 1.0
# with 0.5 in the middle (setting 0x1abac2d6; Reset Defaults sets 0.32). The aim speed is mixed
# linearly from two tuning values by it, and the aiming code asserts it is at most 1 ("sensitivity
# <= 1.0f"). Mouse aiming goes through the stick there, so the game's aim speed caps it: lift the
# check and raise the slider's top. The value is a float in the save file, read with or without
# this. UNCHARTED_AIM_RANGE=0 keeps the original sliders.
AIM_RANGE_PATCHES = {
    # Uncharted 1: the check at 0x276308; the slider (0x1453e4) holds steps, min, mid and max at
    # +0x20..+0x2c and gets mid 2.5, max 5.0: a notch is 0.5, the second one the old maximum.
    "eboot.bin": [
        ("Uncharted 1 aim sensitivity above 1 (aim speed range check)", [
            ("c5f82e05 ???????? 763d c5fa1145f4", 8, bytes.fromhex("763d"), bytes.fromhex("eb3d")),
        ]),
        ("Uncharted 1 aim sensitivity slider up to 5", [
            ("488d0dd5738000 48894818 48c740200a000000 48b9 0000003f0000803f 48894828", 21,
             bytes.fromhex("0000003f0000803f"), bytes.fromhex("000020400000a040")),
        ]),
    ],
    # Uncharted 2: the check in 0x719d80; the slider item (0x5d25b1) takes its max from a constant
    # (mid = max / 2): point it at a 5.0, so a notch is 0.5 and the second one the old maximum.
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 aim sensitivity above 1 (aim speed range check)", [
            ("c5f82e05 ???????? 7649 488b05", 8, bytes.fromhex("7649"), bytes.fromhex("eb49")),
        ]),
        ("Uncharted 2 aim sensitivity slider up to 5", [
            ("81f9d6c2ba1a 0f85???????? 4989ff 4c8d35???????? c5fa1005 75fa8800", 26,
             bytes.fromhex("75fa8800"), bytes.fromhex("21fa8800")),
        ]),
    ],
    # Uncharted 3: the check in UpdateAiming3Settings (0x927fe5). The slider's value -> notch
    # (0x62bf60) and notch -> value (0x62c090) functions keep min/mid/max in registers (0, 0.5,
    # 1.0); the aim setting's branch in each is rewritten in place: the same per-profile address
    # (array 0x17f97a0 + profile * 64 + 0x20, in fewer bytes: imul, then sign extension) and max =
    # 1.0 doubled twice = 4.0. The left half keeps its 0.1 notches, the right half goes 0.5 to 4.0.
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 aim sensitivity above 1 (aim speed range check)", [
            ("c5fa1015???????? c5f82ed0 7351 488b05", 12, bytes.fromhex("7351"), bytes.fromhex("eb51")),
        ]),
        ("Uncharted 3 aim sensitivity slider up to 4 (value -> notch)", [
            ("81f9d6c2ba1a7547 488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b", 8,
             bytes.fromhex("488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b"),
             bytes.fromhex("6b05dd631901404898488d0df4d71c014801c80f58c90f58c9eb2c90")),
        ]),
        ("Uncharted 3 aim sensitivity slider up to 4 (notch -> value)", [
            ("3dd6c2ba1a7520 488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36", 7,
             bytes.fromhex("488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36"),
             bytes.fromhex("6b0da6621901404863c9488d15bcd61c014801d1c5f828da0f58c90f58c9eb36")),
        ]),
    ],
}

# FrameBegin (ndlib fg-draw-mgr.cpp, all three games, 1.02) frees frame N-3's instances and
# asserts that frame's end-of-pipe GPU timestamp is set (m_gfxEopTick of the render frame params,
# 32 slots: +0xa28, Uncharted 1 +0xa08). Players' crash logs (GitHub #11/#12/#17, RTX 3050 and
# 5060, and ours) show it after 40-55 ms stalls (shader compiles, waits for a GPU that fell behind):
# the timestamp, written when the GPU gets there (or after a ray-cast readback lands), was not
# there yet. On the PS4 the GPU never lags that far. The assert's path (59 bytes up to where the
# check continues) becomes a wait for the timestamp: pause in a loop until it is set, at most
# 50 million times (about 0.1-1.5 s by CPU), then carry on. rdx (frame N-3) is kept; the rest of
# the path's registers were scratch there. UNCHARTED_FRAME_WAIT=0 keeps the assert.
def frame_begin_wait(offset):
    code = (bytes.fromhex("488dbc08") + offset.to_bytes(4, "little")  # lea rdi, [rax + rcx + offset]
            + bytes.fromhex("be80f0fa02")  # mov esi, 50000000
            + bytes.fromhex("48833f00")  # loop: cmp qword ptr [rdi], 0
            + bytes.fromhex("7506")  # jne done
            + bytes.fromhex("f390")  # pause
            + bytes.fromhex("ffce")  # dec esi
            + bytes.fromhex("75f4")  # jnz loop
            + bytes.fromhex("eb20"))  # done: jmp to where the check continues
    return code + b"\xcc" * (59 - len(code))


FRAME_BEGIN_ASSERTS = {  # executable: (the check and its assert path, timestamp offset)
    "eboot.bin": ("4883bc08080a000000753b488b057035be004885c0742d488d3dd2053400488d35710434004889d3"
                  "488d15fd053400488d0dd01c350041b8ea0000004531c9ffd04889dacd41", 0xa08),
    "big2-ps4_Shipping.elf": ("4883bc08280a000000753b488b05609b99004885c0742d488d3dad5f2e00488d354c5e2e00"
                              "4889d3488d15d85f2e00488d0d38ff210041b8ea0000004531c9ffd04889dacd41", 0xa28),
    "big3-ps4_Shipping.elf": ("4883bc08280a000000753b488b05f06edb004885c0742d488d3d7df56400488d351cf46400"
                              "4889d3488d15a8f56400488d0d6409570041b8ea0000004531c9ffd04889dacd41", 0xa28),
}
FRAME_BEGIN_WAIT_PATCHES = {
    executable: [(f"FrameBegin waits for frame N-3's GPU timestamp (no m_gfxEopTick assert)", [
        (code, 11, bytes.fromhex(code)[11:], frame_begin_wait(offset)),
    ])]
    for executable, (code, offset) in FRAME_BEGIN_ASSERTS.items()
}

# Uncharted 3 (1.02) Player::UpdateFocus (player.cpp:1897, 0xbcee10) springs the point Nate looks
# at ([player + 0xef0], velocity + 0xf00) towards the target the player gives (kept at rbp-0x80) and
# asserts the result is not NaN (GitHub #13: crash climbing the fort in chapter 8). Once NaN it
# stays NaN, and the function's own reset (0xbcf27a, taken when the point is over 6 m off) never
# runs for it: NaN compares false. The assert's path (53 bytes) now takes that reset instead when
# the target itself is a number (velocity register xmm5 zeroed first, as on the reset's usual
# way in); a NaN target still asserts. UNCHARTED_FOCUS_RESET=0 keeps the original.
FOCUS_NAN_PATCHES = {
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 player focus: reset instead of asserting on NaN", [
            ("c4e37904cb00c5f0c2c900c5f850c183f80f743a488b05f73302014885c07427488d3d74ff8900"
             "488d3546fd8900488d1583ff8900488d0d6ece7d0041b86a0700004531c9ffd0cd41", 20,
             bytes.fromhex("488b05f73302014885c07427488d3d74ff8900488d3546fd8900488d1583ff8900"
                           "488d0d6ece7d0041b86a0700004531c9ffd0cd41"),
             bytes.fromhex("c5d057edc5f8284580c5f8c2c800c5f850c183f80f0f84a5feffffcd41") + b"\xcc" * 24),
        ]),
    ],
    # Uncharted 2 (1.02) PlayerBase::UpdateFocus (player-base.cpp:1101, 0x7272e0): the same spring
    # (point [player + 0x2b0], velocity + 0x2c0, target at rbp-0x60) and the same NaN assert (GitHub
    # #24, and the user's own U2 session on 16 Oct). Its reset when the point is far off (0x72769a)
    # snaps the point to the target. The assert's path (53 bytes) now clears the velocity and takes
    # that snap when the target is a number; a NaN target still asserts.
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 player focus: reset instead of asserting on NaN", [
            ("c4e37904c900 c5f0c2c900 c5f850c1 83f80f 743a 488b05???????? 4885c0 7427 488d3d????????"
             " 488d35???????? 488d15???????? 488d0d???????? 41b84d040000 4531c9 ffd0 cd41", 20,
             bytes.fromhex("488b05e267e4004885c07427488d3d7d4c7600488d352c4b7600488d158c4c7600"
                           "488d0dbdcb6c0041b84d0400004531c9ffd0cd41"),
             bytes.fromhex("c5f8284da0c5f0c2d100c5f850c283f80f7511c5e857d2c5f82993c0020000"
                           "e9e7feffffcd41") + b"\xcc" * 15),
        ]),
    ],
}

# Everything unlocked (launcher -> Advanced -> Unlock all chapters, UNCHARTED_UNLOCK=1): the three
# games keep Bluepoint's tweak flags "Unlock Chapter Select by default" and "Unlock Brutal
# difficulty by default" (debug menu; +2/+1 after the Crushing flag, which the Collection already
# sets). At start the game fills the first and the chapter one from the *bp-ucctweaks* table
# (Crushing on, chapters off); Brutal starts off. The chapter flag is now set where the table is
# read, Brutal where it is cleared (1.02; U1 has two copies of that constructor).
UNLOCK_PATCHES = {
    "eboot.bin": [
        (f"Uncharted 1 chapter select unlocked ({copy})", [
            (f"e8{call} 4885c0 7411 8a08 888b302c0000 8a4001 8883322c0000", 18,
             bytes.fromhex("8a40018883322c0000"), bytes.fromhex("c683322c0000016690")),
        ]) for copy, call in (("1", "fe250000"), ("2", "f51d0000"))
    ] + [
        (f"Uncharted 1 Brutal difficulty unlocked ({copy})", [
            (f"e8{call} c683302c000001 c683312c000000", 12,
             bytes.fromhex("c683312c000000"), bytes.fromhex("c683312c000001")),
        ]) for copy, call in (("1", "ea3b0000"), ("2", "e1330000"))
    ],
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 chapter select unlocked", [
            ("833800 0f9583a02d0000 83780400 0f9583a22d0000", 10,
             bytes.fromhex("837804000f9583a22d0000"), bytes.fromhex("c683a22d0000010f1f4000")),
        ]),
        ("Uncharted 2 Brutal difficulty unlocked", [
            ("c683a02d000001 c683a12d000000 c683a22d000000", 7,
             bytes.fromhex("c683a12d000000"), bytes.fromhex("c683a12d000001")),
        ]),
    ],
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 chapter select unlocked", [
            ("833800 0f9583602c0000 83780400 0f9583622c0000", 10,
             bytes.fromhex("837804000f9583622c0000"), bytes.fromhex("c683622c0000010f1f4000")),
        ]),
        ("Uncharted 3 Brutal difficulty unlocked", [
            ("c683602c000001 c683612c000000 c683622c000000", 7,
             bytes.fromhex("c683612c000000"), bytes.fromhex("c683612c000001")),
        ]),
    ],
}

PATCHES = {
    "eboot.bin": EBOOT_PATCHES,
    "big2-ps4_Shipping.elf": [GPU_WAIT_PATCH],
    "big3-ps4_Shipping.elf": [GPU_WAIT_PATCH],
}
EXECUTABLES = tuple(PATCHES)
# Exit code of sceSystemServiceLoadExec (runtime_services.c): the game asked for another
# executable of the Collection; GOW3_LOADEXEC_FILE holds its /app0 path and arguments.
LOADEXEC_EXIT = 30


def job_sleep_enabled():
    """JOB_SLEEP_PATCHES: on unless UNCHARTED_JOB_SLEEP=0 (launcher: Idle game threads -> Spin).
    Spinning job threads starve 4-6 core CPUs (GPU thread 5% of a core), heat laptops and, on a
    16-core desktop, used about 4 more cores for 2% fewer FPS."""
    return os.environ.get("UNCHARTED_JOB_SLEEP", "auto").strip().lower() not in ("0", "off", "no")


def apply_patches(image, patches):
    data = bytearray(image.read_bytes())
    for description, variants in patches:
        for pattern, offset, original, replacement in variants:
            found = [match.start() for match in pattern_regex(pattern).finditer(data)]
            if len(found) != 1:
                continue
            at = found[0] + offset
            if data[at:at + len(original)] != original:
                raise SystemExit(f"Patch '{description}': unexpected bytes at {at:#x}")
            data[at:at + len(replacement)] = replacement
            print(f"Patched: {description} (image offset {at:#x})")
            break
        else:
            # An unknown game version: start without it.
            print(f"Patch '{description}' NOT applied: no variant matches this game version")
    image.write_bytes(data)


def game_executable():
    """The runtime as Uncharted.exe (Task Manager, Windows' graphics settings and overlays show
    this name), copied from the build output whenever that is newer."""
    built = ROOT / "gow3_pc/out/gow3-probe.exe"
    game_exe = ROOT / "gow3_pc/out/Uncharted.exe"
    try:
        if not game_exe.exists() or built.stat().st_mtime > game_exe.stat().st_mtime:
            shutil.copy2(built, game_exe)
    except OSError as error:  # still running from a previous start: use the build output
        print(f"Using {built.name}: {error}")
        return built
    return game_exe


def game_window_icon(game):
    """The game's own icon (sce_sys/icon0.png) as a BMP for the window (SDL loads only BMP)."""
    icon = ROOT / "launcher/assets/window_icon.bmp"
    if icon.exists():
        return icon
    try:
        from PIL import Image
        icon.parent.mkdir(parents=True, exist_ok=True)
        Image.open(game / "sce_sys/icon0.png").convert("RGB").resize((64, 64)).save(icon)
        return icon
    except Exception as error:  # Pillow missing or no icon: the default icon stays
        print(f"No window icon: {error}")
        return None


# Render resolution (Drake's Fortune). The engine's display settings (constructors and the
# 1080p/720p mode switch, all within ~0x700 bytes) store the size in several fields: display
# and render size (+0x08/+0x18/+0x24/+0x38), aspect, tile grid (+0x40 = width/8, +0x44 =
# height/4, as the engine's own SetResolution computes them) and a larger size at +0x30/+0x34
# (2560x1216 at 1080p), scaled with the rest. UNCHARTED_RESOLUTION=<width>x<height>.
class ResolutionTooLarge(Exception):
    pass


# Drake's Fortune above what its GPU area holds: these 16:9 sizes in turn (width/8, height/4 whole).
U1_FALLBACK_SIZES = [(3200, 1800), (2880, 1620), (2560, 1440), (1920, 1080)]


def resolution_patch(data, width, height, move=True):
    import re
    # mov [rdi/rax+8],1920; mov [rdi/rax+0xc],1080 (1.00 starts with rdi, 1.02 with rax)
    anchor = pattern_regex("c7 ?? 08 80070000 c7 ?? 0c 38040000")
    match = anchor.search(data)
    if not match:
        raise SystemExit("Resolution patch: display settings code not found")
    first = match.start()
    start, end = first - 0x40, first + 0x800
    region = bytearray(data[start:end])
    stores = [
        (0x08, 1920, width), (0x18, 1920, width), (0x24, 1920, width), (0x38, 1920, width),
        (0x0c, 1080, height), (0x1c, 1080, height), (0x28, 1080, height), (0x3c, 1080, height),
        (0x40, 240, width // 8), (0x44, 270, height // 4),
        (0x30, 2560, 2560 * width // 1920), (0x34, 1216, 1216 * height // 1080),
    ]
    # Diagnostics: UNCHARTED_RES_KEEP=<hex field offsets> leaves those fields at 1080p.
    keep = {int(v, 16) for v in os.environ.get("UNCHARTED_RES_KEEP", "").replace(",", " ").split()}
    stores = [store for store in stores if store[0] not in keep]
    changed = 0
    for base in (0x47, 0x40):  # [rdi+disp] and [rax+disp]
        for disp, old, new in stores:
            old_bytes = bytes([0xc7, base, disp]) + old.to_bytes(4, "little")
            new_bytes = bytes([0xc7, base, disp]) + new.to_bytes(4, "little")
            changed += region.count(old_bytes)
            region[:] = region.replace(old_bytes, new_bytes)
    packed_old = bytes.fromhex("48b8") + (1920).to_bytes(4, "little") + (1080).to_bytes(4, "little")
    packed_new = bytes.fromhex("48b8") + width.to_bytes(4, "little") + height.to_bytes(4, "little")
    changed += region.count(packed_old)
    region[:] = region.replace(packed_old, packed_new)
    data[start:end] = region
    # The render targets come from a fixed "Render Target Heap" (497 MiB at 1080p) inside the
    # 2988 MiB GPU (garlic) block of the engine's memory table; both grow with the pixel count
    # (+10%), and the direct memory reported to the game by the same amount (GOW3_DMEM_MB).
    mib = 1 << 20
    factor = max(1.0, width * height / (1920 * 1080))

    def table_entry(entry_id, parent, align, min_size):
        """The memory table entry (id, parent, size, alignment, 0) by structure; sizes differ per game."""
        found = []
        key = struct.pack("<II", entry_id, parent)
        at = data.find(key)
        while at >= 0:
            size, entry_align, pad = struct.unpack_from("<QQQ", data, at + 8)
            if entry_align == align and pad == 0 and size >= min_size:
                found.append((at, size))
            at = data.find(key, at + 1)
        if len(found) != 1:
            raise SystemExit(f"Resolution patch: memory table entry {entry_id:#x} found {len(found)} times")
        return found[0]

    # Memory areas (a table entry's second field is the context it lies in): entry 2 = CPU
    # ("onion", context 5) and entry 3 = GPU ("garlic", context 6). An area holds at most 4096 MiB
    # (MemoryArea keeps one physical page id per MiB in a 4096-entry array), so above ~1620p the
    # Render Target Heap moves to the CPU area; on PC both areas are the same memory. There it is
    # created before the level and animation heaps (0x82, 0x80) and pushes them up by its size,
    # past 2 GiB. Uncharted 2 and 3 don't mind; Drake's Fortune can't address them there: its
    # offset pointers (BP/Shared TGenericOffsetPtr.h GenerateOffset) cover [0, 2 GiB) (its own
    # image: rodata strings are encoded too) and [0x1000000000, +2 GiB) ("offsetTableIndex != -1"
    # assert at 4K, pointer 0x10e92cbd1c). Context 8 (on-demand direct memory) doesn't take the
    # heap (null heap at boot). So Drake's Fortune stops where the GPU area is full (move=False).
    onion_at, onion_old = table_entry(2, 2, 0x100000, 0x40000000)
    garlic_at, garlic_old = table_entry(3, 2, 0x100000, 0x80000000)
    heap_at, heap_old = table_entry(0x85, 6, 0x80, 0x10000000)       # Render Target Heap
    rt_at, rt_old = table_entry(0x84, 6, 0x80, 0x4000000)            # Render Target
    step = 64 * mib  # the engine's direct memory blocks are large-page multiples

    def layout(margin):
        def grown(old):
            return old + (-(-int(old * (factor * margin - 1)) // step) * step if factor > 1 else 0)

        heap_new, rt_new = grown(heap_old), grown(rt_old)
        onion_new, garlic_new, heap_context = onion_old, garlic_old + (heap_new - heap_old) + (rt_new - rt_old), 6
        if garlic_new >= area_limit:
            if not move:
                raise ResolutionTooLarge(f"{width}x{height}: the render targets don't fit the GPU area")
            # Area sizes must be whole MiB (MemoryArea::Initialize asserts): the moved heap is
            # rounded up, and the GPU area's rounding goes to the other render target heap.
            heap_new = -(-heap_new // step) * step
            onion_new = onion_old + heap_new
            rest = garlic_old - heap_old - rt_old
            garlic_new = -(-(rest + rt_new) // step) * step
            rt_new = garlic_new - rest
            heap_context = 5
        # Uncharted 3 1.02 maps its CPU area 100 MiB larger than the table says (4132 MiB at 4K
        # wrapped to 36 MiB, "HeapArena::Allocate: out of memory"): keep 128 MiB below the limit.
        if onion_new + headroom >= area_limit or garlic_new >= area_limit:
            raise ResolutionTooLarge(f"{width}x{height} needs more memory than the game's areas hold")
        return heap_new, rt_new, onion_new, garlic_new, heap_context

    # The render target heaps grow with the pixel count plus 10%, less when that doesn't fit.
    area_limit, headroom = 4096 * mib, 128 * mib
    for margin in (1.1, 1.05, 1.0):
        try:
            heap_new, rt_new, onion_new, garlic_new, heap_context = layout(margin)
            break
        except ResolutionTooLarge:
            if margin == 1.0:
                raise
    extra = (onion_new - onion_old) + (garlic_new - garlic_old)
    struct.pack_into("<IIQ", data, heap_at, 0x85, heap_context, heap_new)
    struct.pack_into("<Q", data, rt_at + 8, rt_new)
    struct.pack_into("<Q", data, onion_at + 8, onion_new)
    struct.pack_into("<Q", data, garlic_at + 8, garlic_new)
    # The memory table check (ValidateContext: lea rsi,[rbp-0x20 or -0x30]; movabs rdx,<total>)
    # expects the direct memory blocks (context 2) to add up to 4608 MiB.
    total_check = pattern_regex("488d75 ?? 48ba 0000002001000000")
    found = [m.start() for m in total_check.finditer(data)]
    if len(found) != 1:
        raise SystemExit(f"Resolution patch: memory total check found {len(found)} times")
    data[found[0] + 6:found[0] + 14] = (0x120000000 + extra).to_bytes(8, "little")
    print(f"Patched: render resolution {width}x{height} ({changed} stores; render target heaps "
          f"{heap_new // mib} + {rt_new // mib} MiB, the first in the {'CPU' if heap_context == 5 else 'GPU'} area)")
    return extra // mib


GAME_KEYS = {"eboot.bin": "u1", "big2-ps4_Shipping.elf": "u2", "big3-ps4_Shipping.elf": "u3"}
UPDATE_SUFFIXES = ("-patch", "-update", "_patch", "_update")


def find_update(game):
    """The game's update (e.g. 1.02, extracted from its PKG): UNCHARTED_UPDATE (a folder, or 0 for
    none), else a folder next to the game's named like it plus -patch/-update (any case), with or
    without uroot. An update folder holds eboot.bin and sce_sys/param.sfo."""
    chosen = os.environ.get("UNCHARTED_UPDATE", "")
    if chosen == "0":
        return None
    if chosen:
        candidates = [Path(chosen), Path(chosen) / "uroot"]
    else:
        title = game.parent if game.name.lower() == "uroot" else game
        names = {title.name.lower() + suffix for suffix in UPDATE_SUFFIXES}
        try:
            siblings = [entry for entry in title.parent.iterdir() if entry.name.lower() in names]
        except OSError:
            siblings = []
        candidates = [path for entry in siblings for path in (entry / "uroot", entry)]
    for candidate in candidates:
        if (candidate / "eboot.bin").is_file() and (candidate / "sce_sys/param.sfo").is_file():
            return candidate.resolve()
    if chosen:
        print(f"UNCHARTED_UPDATE={chosen} holds no eboot.bin and sce_sys/param.sfo; starting without it")
    return None


def sfo_value(game, key):
    try:
        sys.path.insert(0, str(ROOT / "gow3_pc/scripts"))
        from patches import game_sfo
        return game_sfo(game).get(key)
    except Exception:
        return None
    finally:
        sys.path.pop(0)


def game_view(game, update):
    """The game with its update laid over it, as a folder of links (gow3_pc/scripts/game_update.py).
    It is made on the game's drive when possible: file links can't cross drives (else copies)."""
    sys.path.insert(0, str(ROOT / "gow3_pc/scripts"))
    try:
        from game_update import create_view
    finally:
        sys.path.pop(0)
    places = [ROOT / "analysis/views"]
    if Path(game.anchor).resolve() != Path(ROOT.anchor).resolve():
        places.insert(0, game.parent.parent / ".uncharted-views")
    for place in places:
        try:
            return create_view(game, update, place)
        except OSError as error:
            print(f"Game view in {place} failed: {error}")
    raise SystemExit("Could not combine the game and its update")


def ini_value(path, key, default):
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.split("=", 1)[0].strip() == key and "=" in line:
                return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return default


def run_once(args, executable, guest_args, loadexec):
    base = args.game.resolve()
    update = find_update(base)
    if not update:
        return run_game(args, base, executable, guest_args, loadexec)
    print(f"Update {sfo_value(update, 'APP_VER') or '?'}: {update}")
    try:
        view = game_view(base, update)
    except SystemExit as error:
        # exFAT/FAT32 drives (USB sticks) hold no links ([WinError 1] Incorrect function): the
        # game still starts, as dumped, instead of not at all.
        print(f"{error}: starting the game without its update. exFAT and FAT32 drives (USB "
              "sticks) cannot hold the links that combine them: copy the update's files over the "
              "game folder, or keep both on an NTFS drive.")
        return run_game(args, base, executable, guest_args, loadexec)
    try:
        return run_game(args, view, executable, guest_args, loadexec)
    finally:
        sys.path.insert(0, str(ROOT / "gow3_pc/scripts"))
        try:
            from game_update import remove_view
            remove_view(view)
        except OSError as error:
            print(f"Game view {view} not removed: {error}")
        finally:
            sys.path.pop(0)


def run_game(args, game, executable, guest_args, loadexec):
    out = ROOT / "analysis" / ("uncharted" if executable == "eboot.bin" else Path(executable).stem)
    user = ROOT / "analysis/uncharted-user"
    user.mkdir(parents=True, exist_ok=True)
    scripts = ROOT / "gow3_pc/scripts"
    for script in ("prepare.py", "link_modules.py", "content_profile.py"):
        prepare_command = [sys.executable, str(scripts / script), str(game), "--out", str(out)]
        if script != "content_profile.py":
            prepare_command += ["--executable", executable]
        subprocess.run(prepare_command, check=True)
    patches = list(PATCHES[executable])
    if executable == "big2-ps4_Shipping.elf" and os.environ.get("UNCHARTED_TORCH_FORWARD", "1") != "0":
        patches.extend(TORCH_FORWARD_PATCHES)
    if os.environ.get("UNCHARTED_AIM_RANGE", "1") != "0":
        patches.extend(AIM_RANGE_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_FRAME_WAIT", "1") != "0":
        patches.extend(FRAME_BEGIN_WAIT_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_UNLOCK", "0") == "1":
        patches.extend(UNLOCK_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_FOCUS_RESET", "1") != "0":
        patches.extend(FOCUS_NAN_PATCHES.get(executable, []))
    if job_sleep_enabled():
        patches.extend(JOB_SLEEP_PATCHES)
        # Diagnostics: UNCHARTED_JOB_SLEEP_SPINS=<n> idle spins before a worker sleeps (1000).
        if spins := os.environ.get("UNCHARTED_JOB_SLEEP_SPINS", "").strip():
            patches.append(("idle job workers: spins before sleeping", [
                ("4d8d7001 807f5900 ???? 4981f8e8030000 7c??", 13, bytes.fromhex("e8030000"),
                 int(spins).to_bytes(4, "little")),
            ]))
    apply_patches(out / "boot-linked.bin", patches)
    # Settings per game (the launcher keeps them apart): the started game's own, also when the
    # Collection menu switches games.
    game_key = GAME_KEYS[executable]
    resolution = (os.environ.get(f"UNCHARTED_RESOLUTION_{game_key.upper()}")
                  or os.environ.get("UNCHARTED_RESOLUTION", "")).lower()
    width, height = (int(value) for value in resolution.split("x")) if resolution else (1920, 1080)
    render_size = (1920, 1080)
    if (width, height) != (1920, 1080):
        # (The earlier Uncharted 3 failures above 1080p - "Command context dcb full", a corrupted
        # pointer while loading - came with asynchronous readbacks, which also broke it at 1080p.)
        image = out / "boot-linked.bin"
        data = bytearray(image.read_bytes())
        # Drake's Fortune keeps its render targets in the GPU area (see resolution_patch): above
        # ~1620p it renders at the largest size that fits there, and the output is upscaled.
        move = executable != "eboot.bin"
        candidates = [(width, height)] + [size for size in U1_FALLBACK_SIZES if size[0] * size[1] < width * height]
        if abs(width / height - 16 / 9) > 0.01:
            # Ultrawide: smaller sizes of the same shape (width/8 and height/4 whole).
            candidates = [(width, height)] + [
                (int(width * scale) // 8 * 8, int(height * scale) // 4 * 4)
                for scale in (0.9, 0.8, 0.75, 2 / 3, 0.6, 0.5)]
        extra_mib = 0
        for candidate in candidates:
            patched = bytearray(data)
            try:
                extra_mib = resolution_patch(patched, *candidate, move=move)
            except ResolutionTooLarge as error:
                print(f"Render resolution {error}")
                continue
            if candidate != (width, height):
                print(f"Drake's Fortune renders at {candidate[0]}x{candidate[1]} instead of {width}x{height}")
            data, render_size = patched, candidate
            break
        image.write_bytes(data)
    else:
        extra_mib = 0
    env = os.environ.copy()
    # Measurement logging and live trigger files (frame/fault/wait statistics every 5 s, GPU
    # checkpoints, capture/readback/DMA triggers). On unless the launcher turns it off.
    diagnostics = env.get("UNCHARTED_DIAGNOSTICS", "1") != "0"
    msys = Path(env.get("MSYS2_ROOT", "C:/msys64"))
    env["PATH"] = str(msys / "clang64/bin") + os.pathsep + env.get("PATH", "")
    env["GOW3_GPU_USER_DIR"] = str(user)
    settings = user / f"probe-{game_key}.ini"
    if not settings.exists() and (user / "probe.ini").exists():
        shutil.copy2(user / "probe.ini", settings)  # earlier versions shared one file
    # UNCHARTED_CONFIG_FILE: another settings file (tests that must not touch the player's).
    env["GOW3_CONFIG"] = os.environ.get("UNCHARTED_CONFIG_FILE") or str(settings)
    # Frame rates above 60 need the fast vblank (GOW3_VBLANK_HZ=0); an explicit one is kept.
    if "GOW3_VBLANK_HZ" not in os.environ:
        limit = ini_value(Path(env["GOW3_CONFIG"]), "fps_limit", "60")
        if limit.isdigit() and (int(limit) == 0 or int(limit) > 60):
            env["GOW3_VBLANK_HZ"] = "0"
    multithreaded = os.environ.get(f"UNCHARTED_MULTITHREADED_{game_key.upper()}")
    # Drake's Fortune: its weapons (and at times other objects) drew with another draw's look on
    # some frames with multithreaded rendering (flat grey guns, the player and GitHub #23, 18 Oct
    # 2026), and it gains nothing from it (4 cores: 101.8 vs 103.2 FPS). Off until that is fixed;
    # UNCHARTED_U1_MULTITHREADED_TEST=1 keeps the launcher's choice (tests).
    if executable == "eboot.bin" and multithreaded == "1" and \
            os.environ.get("UNCHARTED_U1_MULTITHREADED_TEST") != "1":
        print("Multithreaded rendering: off for Drake's Fortune (its weapons flicker with it)")
        multithreaded = "0"
    if multithreaded is not None:
        env["GOW3_DRAW_PIPE"] = multithreaded
        env["GOW3_VK_RECORD_THREAD"] = multithreaded
    # Keyboard and mouse (launcher Controls tab, per game; explicit GOW3_* values win).
    for launcher_key, runtime_key in (("KEY_MAP", "GOW3_KEY_MAP"), ("MOUSE_MAP", "GOW3_MOUSE_MAP"),
                                      ("MOUSE", "GOW3_MOUSE"), ("MOUSE_SENS", "GOW3_MOUSE_SENS"),
                                      ("MOUSE_INVERT", "GOW3_MOUSE_INVERT"), ("PROMPTS", "GOW3_PROMPTS"),
                                      ("PAD_MAP", "GOW3_PAD_MAP")):
        value = os.environ.get(f"UNCHARTED_{launcher_key}_{game_key.upper()}")
        if value is not None and runtime_key not in os.environ:
            env[runtime_key] = value
    # GPU readbacks are asynchronous. Unprotected (the runtime default, 2) crashed Uncharted 3
    # within a minute in the Cartagena museum (guest reads of 0xffff... pointers, a compute ring
    # with a garbage indirect buffer) and let characters fly or NPCs fall from the sky (U2, U3,
    # 16 Oct, also on an RTX 5060 laptop). Waiting for every readback (0) was correct but cost U2
    # ~20 FPS. Protected (1) since 16 Oct: new GPU data stays read-protected until it lands,
    # recurring results (the same piece read back again, e.g. luminance) stay readable one frame
    # old as on the console, and pages where the CPU reads other data beside GPU bytes are left
    # readable (BufferCache::noisy_pages): U2 and U3 at 60 FPS, U3 crash-free for 6 minutes.
    # GOW3_ASYNC_READBACK in the environment overrides it (0 waits, 1 protected, 2 unprotected).
    if executable in ("big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf"):
        env.setdefault("GOW3_ASYNC_READBACK", "1")
    # Uncharted 2 reads back data its 64-wide copy shader (cs 6a6422a9) writes on the GPU: read
    # back with the submission it arrived a frame late, and NPCs floated in the air while Nate's
    # feet sank into the floor (also in 0.1.1 and other ports). It now comes back at the frame's
    # end-of-pipe fence, which waits for it (16 Oct; precise readbacks fixed it too, at ~50 FPS).
    # 17 Oct: Uncharted 3 too (user: Nate's legs flicker a little, NPCs float a bit; the same
    # engine and copy shader). Two 90 s U3 runs at 1440p: stable, 56-58 FPS.
    if executable in ("big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf"):
        env.setdefault("GOW3_COPY64_EOP_READBACK", "1")
    if executable == "big2-ps4_Shipping.elf":
        # Its aim depth of field reads the focus distance from a 1x1 render target the GPU writes;
        # never copied back it stayed 0 and everything behind Nate was blurred while aiming. Tiny
        # GPU-written images now come back with the frame's end-of-pipe fence (16 Oct). Drake's
        # Fortune and Uncharted 3 aim sharp without it, so they skip the copy and the wait.
        env.setdefault("GOW3_TINY_IMAGE_READBACK", "1")
    # The PS4's system language (launcher -> Advanced -> Language; runtime_services.c): each game's
    # main menu offers it next to English (Square). 1 = English (US).
    language = os.environ.get("UNCHARTED_LANGUAGE", "").strip()
    if language.isdigit():
        env.setdefault("GOW3_LANGUAGE", language)
    # Temporal anti-aliasing (in-game menu -> Image: TAA, FSR 3.1/FSR 4 Native AA, DLAA) at the
    # render size, as in the reference Uncharted port (docs: analysis/STATUS.md): the camera comes
    # from the motion blur's camera velocity pass (shader f4b18264; all three games share the post
    # processing), and the anti-aliasing runs on the lit scene before the post processing starts
    # (compute aec539f0). UNCHARTED_TEMPORAL_AA=0 switches it off (no camera tracking at all).
    if os.environ.get("UNCHARTED_TEMPORAL_AA", "1") == "0":
        env["GOW3_UPSCALER"] = "none"
    else:
        env.setdefault("GOW3_VELOCITY_PS", "f4b18264")
        env.setdefault("GOW3_UPSCALE_BEFORE_CS", "aec539f0")
        env["GOW3_SCENE_SIZE"] = f"{render_size[0]}x{render_size[1]}"
        # FSR 4's model files (gow3_pc/tools/fetch_fsr4_assets.sh), when installed; DLAA needs
        # gow3_dlss.dll and nvngx_dlss.dll beside Uncharted.exe.
        fsr4 = ROOT / "gow3_pc/fsr4_shaders"
        if fsr4.is_dir():
            env.setdefault("GOW3_FSR4_DIR", str(fsr4))
        # The renderer's own default is FSR 3; here it starts off until it is picked.
        config = Path(env["GOW3_CONFIG"])
        if ini_value(config, "upscaler", None) is None:
            try:
                text = config.read_text(encoding="utf-8", errors="replace") if config.exists() else ""
                if text and not text.endswith("\n"):
                    text += "\n"
                config.write_text(text + "upscaler=off\n", encoding="utf-8")
            except OSError:
                env.setdefault("GOW3_UPSCALER", "off")
    if extra_mib:
        env["GOW3_DMEM_MB"] = str(5056 + extra_mib + 64)
    # The movie player reads its (1080p) movies with the render size: the decoder scales them to it
    # (gpu/shim/videodec2; else movies come out green and tiled above 1080p).
    if render_size != (1920, 1080):
        env["GOW3_MOVIE_SIZE"] = f"{render_size[0]}x{render_size[1]}"
    pad_file=user / "probe-pad.txt"
    pad_file.write_text("",encoding="ascii")
    env["GOW3_PAD_FILE"] = str(pad_file)
    # Start with a serialized renderer while investigating Uncharted loading.
    # The base runtime enables these concurrency optimizations for GOW3.
    env.setdefault("GOW3_DRAW_PIPE", "0")
    env.setdefault("GOW3_VK_RECORD_THREAD", "0")
    env.setdefault("GOW3_NATIVE_TSC", "1")
    # Grey-world diagnosis: timestamped opens/reads of level packs and streams (boot.log).
    # env.setdefault("GOW3_FILE_TRACE", ".pak")  # pack read trace (grey-world diagnosis, done)
    # Frame captures on demand: write a pass count (e.g. 600) into this file; captures land in
    # analysis/uncharted-user/captures. Every draw's/dispatch's buffer addresses are listed.
    if diagnostics:
        env.setdefault("GOW3_CAPTURE_TRIGGER", str(user / "capture.trigger"))
        env.setdefault("GOW3_CAPTURE_ALL_BUFFERS", "1")
    # Grey-world diagnosis: GPU-side writes into the lighting record table the ground shader reads.
    # env.setdefault("GOW3_WATCH_RANGE", "1038e00000:1038f00000")  # done: writer was cs 2d215c3b
    # Dark-interior diagnosis: who writes the 256x8 light ramp LUT at 0x1134ab0000 (all zero).
    # (done: the LUT writer is ps 2d7325a4) Temple frame drops: who writes the two windows the CPU
    # keeps writing into after the GPU (each CPU write downloads with a full GPU wait).
    # (done: GDS-to-memory DMA copies and cs 6a6422a9; fixed by lazy writes and async readbacks)
    # if executable == "eboot.bin":
    #     env.setdefault("GOW3_WATCH_RANGE", "1036200000:1036300000,1061e00000:1061f00000")
    # Grey-world fix test: the ground's constants are written on the GPU (copy shader 2d215c3b) and
    # read from guest memory by the CPU-side SRT walker. Relaxed readbacks (1) never sync GPU writes
    # to guest memory unless the CPU later writes that page; Precise (2) reads them back on CPU reads.
    # env.setdefault("GOW3_READBACKS", "2")  # Precise hangs on Continue (18:35): not usable
    # Retest 14 Oct 21:5x after the capture-job and draw-prep deadlock fixes: hair/cobweb shadow
    # casters read light matrices that a GPU copy shader writes in the same submission.
    # Result: no visual change, ~3 FPS slower, no hang on Continue any more. Off.
    # U2 flashlight test (15 Oct): the light sits exactly at the camera (its source, a GPU-written
    # joint transform, may read as zero). The file analysis/uncharted-user/precise-readbacks turns
    # Precise readbacks on for the next start.
    if (user / "precise-readbacks").exists():
        env.setdefault("GOW3_READBACKS", "2")
        print("Readbacks: precise (analysis/uncharted-user/precise-readbacks)")
    # Test settings for the next start without code changes: KEY=VALUE lines (# comments) in
    # analysis/uncharted-user/test-env.txt; they override everything set above.
    test_env = user / "test-env.txt"
    if test_env.exists():
        for line in test_env.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                env[key.strip()] = value.strip()
                print(f"Test setting: {key.strip()}={value.strip()}")
    # GPU-written constants back to guest memory for the SRT walker: write "auto"/"off" or
    # "<lo> <hi>" (hex range, one-off) into this file. GOW3_SYNC_GPU_CONSTANTS=1 starts it on.
    if diagnostics:
        env.setdefault("GOW3_READBACK_TRIGGER", str(user / "readback.trigger"))
    # Grey-world fix (confirmed 14 October 2026): GPU-written constants back to guest memory each submission.
    env.setdefault("GOW3_SYNC_GPU_CONSTANTS", "1")
    # Sound-effects diagnosis (gunfire/ambience silent, music fine): Ajm instances, failed jobs,
    # RIFF jobs and 5 s decoder totals in boot.log.
    # env.setdefault("GOW3_AUDIO_TRACE", "1")  # sound diagnosis (done: config read-only patch)
    # env.setdefault("GOW3_FILE_TRACE", "sound1/config")  # sound config opens/reads (done)
    # Sound config load diagnosis: open callback entry, path result, Fios open result, loader result.
    # env.setdefault("GOW3_EXEC_WATCH", "515cc9")  # config loader open result (0 = ok, 0x1e = EROFS)
    # Dark-interior diagnosis: CPU writes to the light ramp LUT texels (0x1134ab0000, all zero).
    # (done: 0 hits from startup; nothing writes the LUT on the CPU)
    # if executable == "eboot.bin":
    #     env.setdefault("GOW3_EXEC_WATCH", "w:1134ab0000,w:1134ab0100,w:1134ab4000,w:1134ab8000")
    # Dark-interior test: with async shader compilation a draw whose pipeline is still compiling
    # is skipped (PipelineCache::GetGraphicsPipeline returns nullptr). A one-time draw at level
    # load (suspect: the writer of the LUT above) would then never run. Synchronous compilation
    # for this test; overrides async_shaders in probe.ini.
    if executable == "eboot.bin":
        env.setdefault("GOW3_ASYNC_SHADERS", "0")
    # Frame drops in the temple (45-57 FPS): GPU thread waits in DownloadMemory for CPU read faults
    # on GPU-modified pages. Every 5 s: fault counts/time, hottest guest instructions and windows.
    if diagnostics:
        env.setdefault("GOW3_FAULT_STATS", "1")
    # Frame rate diagnosis: flip rate/frame time spread every 5 s, compile time on the GPU thread,
    # and details of frames over 40 ms.
    if diagnostics:
        env.setdefault("GOW3_FRAME_STATS", "1")
    # DMA sync mode for the tiled lighting shader, switchable live: write "full", "off" or
    # "pointers <KiB>" into this file (the full sync costs ~10 ms/frame in the temple).
    if diagnostics:
        env.setdefault("GOW3_DMA_TRIGGER", str(user / "dma.trigger"))
    # DMA shaders (tiled lighting; MUBUF ADDR64 shadow cascade loads of hair/cobwebs/characters):
    # make 256 KiB windows at the guest addresses in their user data resident and current,
    # instead of uploading every CPU-dirty resident range before each such draw ("full").
    env.setdefault("GOW3_DMA_SYNC", "pointers 256")
    # GPU time per render pass/dispatch, printed every 5 s (frame-rate diagnosis). Uncharted's
    # frames are marked at the display pass after submitting the recorded work.
    # Off for play: it ends every render pass to time it, which costs frame time itself.
    # env.setdefault("GOW3_GPU_PROFILE", "1")
    # Which code waits for the GPU (scheduler waits by caller), reported with the frame stats.
    if diagnostics:
        env.setdefault("GOW3_PERF_DIAG", "1")
    # Uncharted's tiled lighting shader (cs 0x43b8ee5e) reads per-light records with an
    # index computed on the GPU (waterfall loop over v_readfirstlane); the CPU-side SRT walker
    # cannot pre-read those, so they are read from guest memory on the GPU.
    env.setdefault("GOW3_DIRECT_MEMORY_ACCESS", "1")
    # GPU checkpoints (which draw a lost device was on) cost ~2% of the GPU command thread in heavy
    # Uncharted 2 scenes: only on request (UNCHARTED_GPU_CHECKPOINTS=1), no longer with the
    # performance log every player has on (16 Oct).
    if os.environ.get("UNCHARTED_GPU_CHECKPOINTS") == "1":
        env.setdefault("GOW3_GPU_CHECKPOINTS", "1")
    # The game asserts when the GPU finishes a frame more than 2 s late (FrameBegin,
    # m_gfxEopTick); cached pipelines avoid recompiling every shader on each run.
    # Set GOW3_PIPELINE_CACHE=0 when testing shader recompiler changes.
    env.setdefault("GOW3_PIPELINE_CACHE", "1")
    env.setdefault("GOW3_PIPELINE_BINARIES", "1")
    env["GOW3_TOGGLE_FILE"] = str(user / "disabled-optimizations.txt")
    env["GOW3_LOADEXEC_FILE"] = str(loadexec)
    if args.capture:
        capture=out / "captures" / str(time.time_ns())
        capture.mkdir(parents=True)
        trigger=capture / "trigger"
        env["GOW3_FRAME_DUMP_TRIGGER"]=str(trigger)
        env["GOW3_DUMP_DIR"]=str(capture)
        trigger.touch()
    else:
        # On-demand image dumps (no trigger file at start, so nothing is dumped until asked):
        # create analysis/uncharted/dumps/trigger, plus guest-images.trigger listing addresses.
        if diagnostics:
            dumps=out / "dumps"
            dumps.mkdir(parents=True, exist_ok=True)
            env.setdefault("GOW3_FRAME_DUMP_TRIGGER", str(dumps / "trigger"))
            env.setdefault("GOW3_DUMP_DIR", str(dumps))
    window_icon = game_window_icon(game)
    if window_icon:
        env.setdefault("GOW3_WINDOW_ICON", str(window_icon))
    command = [str(game_executable()), str(out / "boot-linked.bin"),
               "--no-game-hooks", "--low-image", "--content-profile", str(out / "content.bin"),
               "--app0", str(game), "--user", str(user)]
    # guest_args is the game's complete argv; argv[0] is the program name.
    command += ["--guest-path", guest_args[0] if guest_args else "/app0/" + executable]
    for value in guest_args[1:]:
        command += ["--guest-arg", value]
    # Diagnostics: UNCHARTED_RENDERDOC=<RenderDoc folder or renderdoccmd.exe> loads RenderDoc's
    # Vulkan layer (its capture library) into the game: F12 captures a frame (RenderDoc's default
    # folder, %TEMP%\RenderDoc). Found through this process's environment only; nothing is
    # registered, and the game's log stays as it is.
    renderdoc = os.environ.get("UNCHARTED_RENDERDOC")
    if renderdoc:
        layer_dir = Path(renderdoc)
        layer_dir = layer_dir if layer_dir.is_dir() else layer_dir.parent
        if (layer_dir / "renderdoc.json").is_file():
            env["VK_ADD_LAYER_PATH"] = str(layer_dir) + (os.pathsep + env["VK_ADD_LAYER_PATH"]
                                                         if env.get("VK_ADD_LAYER_PATH") else "")
            env["VK_INSTANCE_LAYERS"] = "VK_LAYER_RENDERDOC_Capture"
            env["ENABLE_VULKAN_RENDERDOC_CAPTURE"] = "1"
            env["GOW3_PIPELINE_BINARIES"] = "0"
            print(f"RenderDoc: Vulkan layer from {layer_dir}")
    if not args.boot:
        command.append("--check-imports")
    if args.cpu_only:
        command.append("--cpu-only")
    name = "boot" if args.boot else "check-imports"
    if args.cpu_only:
        name += "-cpu"
    result = {"executable": executable, "command": command, "guest_execution_requested": args.boot, "timeout_seconds": args.seconds}
    if args.capture:
        result["capture_directory"]=str(capture)
    start = time.monotonic()
    with (out / (name + ".log")).open("wb") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            if args.capture:
                while process.poll() is None:
                    remaining=args.seconds-(time.monotonic()-start) if args.seconds else 3
                    if remaining<=0:
                        raise subprocess.TimeoutExpired(command,args.seconds)
                    try:
                        process.wait(timeout=min(3,remaining))
                    except subprocess.TimeoutExpired:
                        trigger.touch()
                result["exit_code"]=process.returncode
            else:
                result["exit_code"] = process.wait(timeout=args.seconds or None)
            result["timed_out"] = False
        except subprocess.TimeoutExpired:
            process.kill()
            result["exit_code"] = process.wait()
            result["timed_out"] = True
    result["elapsed_seconds"] = round(time.monotonic() - start, 3)
    (out / (name + ".json")).write_text(json.dumps(result, indent=2), encoding="utf-8")
    attempt = out / "attempts" / (name + "-" + str(time.time_ns()))
    attempt.mkdir(parents=True)
    for suffix in (".log", ".json"):
        shutil.copy2(out / (name + suffix), attempt / (name + suffix))
    print(json.dumps(result, indent=2))
    print(f"Log: {out / (name + '.log')}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=ROOT / "game/CUSA02344/uroot")
    parser.add_argument("--boot", action="store_true", help="Execute guest code after preparation")
    parser.add_argument("--executable", choices=EXECUTABLES, default="eboot.bin")
    parser.add_argument("--guest-arg", action="append", default=None,
                        help="The game's argv, program name first (repeatable). A game started directly gets "
                             "the Collection menu's argv: eboot.bin ispartofuccollection")
    parser.add_argument("--collection-menu", action="store_true",
                        help="Start eboot.bin with the Collection's game selection instead of Drake's Fortune")
    parser.add_argument("--cpu-only", action="store_true")
    parser.add_argument("--capture", action="store_true", help="Request a renderer frame dump every three seconds")
    parser.add_argument("--seconds", type=int, default=30, help="External process time limit; 0 disables it")
    args = parser.parse_args()
    if args.seconds < 0:
        parser.error("--seconds must be nonnegative")
    user = ROOT / "analysis/uncharted-user"
    user.mkdir(parents=True, exist_ok=True)
    loadexec = user / "loadexec.txt"
    executable = args.executable
    guest_args = args.guest_arg
    if guest_args is None:
        # As the Collection menu starts each game: "ispartofuccollection" makes eboot.bin start
        # Drake's Fortune itself (without it, it shows the Collection's game selection, whose
        # cursor is on the last game played: the U1 card started U2/U3 after one press of Cross).
        if executable == "eboot.bin" and args.collection_menu:
            guest_args = ["/app0/eboot.bin"]
        else:
            guest_args = ["eboot.bin", "ispartofuccollection"]
    while True:
        loadexec.unlink(missing_ok=True)
        result = run_once(args, executable, guest_args, loadexec)
        if not args.boot or result["timed_out"] or result["exit_code"] != LOADEXEC_EXIT or not loadexec.exists():
            break
        # The Collection switched games (menu -> Uncharted 2/3, or back to the menu).
        lines = loadexec.read_text(encoding="utf-8", errors="replace").splitlines()
        target = Path(lines[0].strip()).name if lines else ""
        if target not in PATCHES:
            print(f"Game asked to start {lines[0] if lines else '(nothing)'}; not part of the Collection")
            break
        # LoadExec's argv is the new program's complete argv (the menu passes "eboot.bin" as argv[0]
        # for Uncharted 2/3; an extra leading argument makes them skip the menu and load nothing).
        executable, guest_args = target, lines[1:] or ["/app0/" + target]
        print(f"Collection: switching to {executable} {' '.join(guest_args)}")
    return 124 if result["timed_out"] else result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
