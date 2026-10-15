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

PATCHES = {
    "eboot.bin": EBOOT_PATCHES,
    "big2-ps4_Shipping.elf": [GPU_WAIT_PATCH],
    "big3-ps4_Shipping.elf": [GPU_WAIT_PATCH],
}
EXECUTABLES = tuple(PATCHES)
# Exit code of sceSystemServiceLoadExec (runtime_services.c): the game asked for another
# executable of the Collection; GOW3_LOADEXEC_FILE holds its /app0 path and arguments.
LOADEXEC_EXIT = 30


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
    view = game_view(base, update)
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
    apply_patches(out / "boot-linked.bin", PATCHES[executable])
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
        limit = ini_value(settings, "fps_limit", "60")
        if limit.isdigit() and (int(limit) == 0 or int(limit) > 60):
            env["GOW3_VBLANK_HZ"] = "0"
    multithreaded = os.environ.get(f"UNCHARTED_MULTITHREADED_{game_key.upper()}")
    if multithreaded is not None:
        env["GOW3_DRAW_PIPE"] = multithreaded
        env["GOW3_VK_RECORD_THREAD"] = multithreaded
    # Asynchronous GPU readbacks, read-protected until they land (launcher option per game; an
    # explicit GOW3_ASYNC_READBACK wins). Drake's Fortune dies while loading with them (15 Oct).
    async_readback = os.environ.get(f"UNCHARTED_ASYNC_READBACK_{game_key.upper()}")
    if async_readback is not None and "GOW3_ASYNC_READBACK" not in os.environ:
        env["GOW3_ASYNC_READBACK"] = "1" if async_readback == "1" and executable != "eboot.bin" else "0"
    env["GOW3_UPSCALER"] = "none"
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
    if diagnostics:
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
