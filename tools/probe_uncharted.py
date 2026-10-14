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

# Code patches for Drake's Fortune (eboot.bin), applied to the linked image by unique byte
# pattern: (description, pattern, offset in pattern, original bytes, replacement bytes).
EBOOT_PATCHES = [
    # The wait for a frame's GPU end-of-pipe stamp (0x3fd6xx) gives up after 2.0 s, and
    # FrameBegin then asserts m_gfxEopTick of frame N-3 (fg-draw-mgr.cpp:234). On PC the
    # command processor can pause that long (shader compilation, readbacks), never on a PS4.
    # NOP the `ja` that leaves the loop on timeout (vucomiss [2.0f]; ja +0x39 at 0x3fd816).
    ("wait for GPU frame completion without the 2 s timeout",
     bytes.fromhex("c5f82e054a3540007739"), 8, bytes.fromhex("7739"), bytes.fromhex("9090")),
    # Silent sound effects (footsteps, gunfire, ambience; music and speech fine): the sound config
    # loader (biglib audio acm 0x515a50) opens reverbs/mixers/environments/prototypes.txt through
    # the sound file callback 0x8ef80 with flag 1, which `testb $5,%cl` at 0x8f1da turns into O_CREAT.
    # Our Fios2 rejects READ|CREAT on read-only /app0 with SCE_FIOS_ERROR_READ_ONLY (0x80820015,
    # mapped to EROFS 0x1e) before any kernel call, so no sound prototypes load and every effect
    # request resolves to nothing. Test bit 2 only: a read request opens read-only.
    ("sound config files open read-only (no O_CREAT on /app0)",
     bytes.fromhex("4a8d542b14f6c1050f95c00fb6c0c1e0068db000020000f6c106"), 5,
     bytes.fromhex("f6c105"), bytes.fromhex("f6c104")),
]

# Uncharted 2 and 3 share the frame-completion wait with Drake's Fortune byte for byte (same
# fg-draw-mgr.cpp m_gfxEopTick assert); only the rip-relative offset of the 2.0f differs.
def gpu_wait_patch(displacement):
    return ("wait for GPU frame completion without the 2 s timeout",
            bytes.fromhex("c5f258c0c5f82e05" + displacement + "7739f390beffffffff"), 12,
            bytes.fromhex("7739"), bytes.fromhex("9090"))

PATCHES = {
    "eboot.bin": EBOOT_PATCHES,
    "big2-ps4_Shipping.elf": [gpu_wait_patch("ca9d6400")],
    "big3-ps4_Shipping.elf": [gpu_wait_patch("a298aa00")],
}
EXECUTABLES = tuple(PATCHES)
# Exit code of sceSystemServiceLoadExec (runtime_services.c): the game asked for another
# executable of the Collection; GOW3_LOADEXEC_FILE holds its /app0 path and arguments.
LOADEXEC_EXIT = 30


def apply_patches(image, patches):
    data = bytearray(image.read_bytes())
    for description, pattern, offset, original, replacement in patches:
        count = data.count(pattern)
        if count != 1:
            raise SystemExit(f"Patch '{description}': pattern found {count} times, expected 1")
        at = data.find(pattern) + offset
        if data[at:at + len(original)] != original:
            raise SystemExit(f"Patch '{description}': unexpected bytes at {at:#x}")
        data[at:at + len(replacement)] = replacement
        print(f"Patched: {description} (image offset {at:#x})")
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
def resolution_patch(data, width, height):
    anchor = bytes.fromhex("c7470880070000c7470c38040000")  # mov [rdi+8],1920; mov [rdi+0xc],1080
    first = data.find(anchor)
    if first < 0:
        raise SystemExit("Resolution patch: display settings code not found")
    start, end = first - 0x40, first + 0x800
    region = bytearray(data[start:end])
    stores = [
        (0x08, 1920, width), (0x18, 1920, width), (0x24, 1920, width), (0x38, 1920, width),
        (0x0c, 1080, height), (0x1c, 1080, height), (0x28, 1080, height), (0x3c, 1080, height),
        (0x40, 240, width // 8), (0x44, 270, height // 4),
        (0x30, 2560, 2560 * width // 1920), (0x34, 1216, 1216 * height // 1080),
    ]
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

    # Memory areas: entry 2 = CPU ("onion", context 5), entry 3 = GPU ("garlic", context 6); a
    # table entry's second field is its context. An area holds at most 4096 MiB (MemoryArea
    # keeps one physical page id per MiB in a 4096-entry array), so above ~1440p the Render
    # Target Heap moves to the CPU area; on PC both areas are the same memory.
    onion_at, onion_old = table_entry(2, 2, 0x100000, 0x40000000)
    garlic_at, garlic_old = table_entry(3, 2, 0x100000, 0x80000000)
    heap_at, heap_old = table_entry(0x85, 6, 0x80, 0x10000000)       # Render Target Heap
    rt_at, rt_old = table_entry(0x84, 6, 0x80, 0x4000000)            # Render Target
    step = 64 * mib  # the engine's direct memory blocks are large-page multiples

    def grown(old):
        return old + (-(-int(old * (factor * 1.1 - 1)) // step) * step if factor > 1 else 0)

    heap_new, rt_new = grown(heap_old), grown(rt_old)
    onion_new, garlic_new, heap_context = onion_old, garlic_old + (heap_new - heap_old) + (rt_new - rt_old), 6
    area_limit = 4096 * mib
    if garlic_new >= area_limit:
        # Area sizes must be whole MiB (MemoryArea::Initialize asserts): the moved heap is
        # rounded up, and the GPU area's rounding goes to the other render target heap.
        heap_new = -(-heap_new // step) * step
        onion_new = onion_old + heap_new
        rest = garlic_old - heap_old - rt_old
        garlic_new = -(-(rest + rt_new) // step) * step
        rt_new = garlic_new - rest
        heap_context = 5
    if onion_new >= area_limit or garlic_new >= area_limit:
        raise SystemExit(f"Resolution patch: {width}x{height} needs more memory than the game's areas hold")
    extra = (onion_new - onion_old) + (garlic_new - garlic_old)
    struct.pack_into("<IIQ", data, heap_at, 0x85, heap_context, heap_new)
    struct.pack_into("<Q", data, rt_at + 8, rt_new)
    struct.pack_into("<Q", data, onion_at + 8, onion_new)
    struct.pack_into("<Q", data, garlic_at + 8, garlic_new)
    # The memory table check (lea rsi,[rbp-0x20]; movabs rdx,<total>) expects the direct
    # memory blocks to add up to 4608 MiB.
    total_old = bytes.fromhex("488d75e048ba") + (0x120000000).to_bytes(8, "little")
    if data.count(total_old) != 1:
        raise SystemExit(f"Resolution patch: memory total check found {data.count(total_old)} times")
    at = data.find(total_old)
    data[at:at + len(total_old)] = bytes.fromhex("488d75e048ba") + (0x120000000 + extra).to_bytes(8, "little")
    print(f"Patched: render resolution {width}x{height} ({changed} stores; render target heaps "
          f"{heap_new // mib} + {rt_new // mib} MiB in the {'CPU' if heap_context == 5 else 'GPU'} area)")
    return extra // mib


GAME_KEYS = {"eboot.bin": "u1", "big2-ps4_Shipping.elf": "u2", "big3-ps4_Shipping.elf": "u3"}


def ini_value(path, key, default):
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.split("=", 1)[0].strip() == key and "=" in line:
                return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return default


def run_once(args, executable, guest_args, loadexec):
    game = args.game.resolve()
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
    if (width, height) != (1920, 1080):
        # Uncharted 3 above 1080p: at 4K its compute command buffers fill up ("Command context
        # dcb full"), and at 1440p loading into the game hits a corrupted pointer: buffers
        # sized for 1080p overflow. Until those are found it runs at 1080p.
        if executable == "big3-ps4_Shipping.elf" and width * height > 1920 * 1080:
            print(f"Uncharted 3: {width}x{height} is not supported yet, using 1920x1080")
            width, height = 1920, 1080
        image = out / "boot-linked.bin"
        data = bytearray(image.read_bytes())
        extra_mib = resolution_patch(data, width, height)
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
    env["GOW3_CONFIG"] = str(settings)
    # Frame rates above 60 need the fast vblank (GOW3_VBLANK_HZ=0); an explicit one is kept.
    if "GOW3_VBLANK_HZ" not in os.environ:
        limit = ini_value(settings, "fps_limit", "60")
        if limit.isdigit() and (int(limit) == 0 or int(limit) > 60):
            env["GOW3_VBLANK_HZ"] = "0"
    multithreaded = os.environ.get(f"UNCHARTED_MULTITHREADED_{game_key.upper()}")
    if multithreaded is not None:
        env["GOW3_DRAW_PIPE"] = multithreaded
        env["GOW3_VK_RECORD_THREAD"] = multithreaded
    env["GOW3_UPSCALER"] = "none"
    if extra_mib:
        env["GOW3_DMEM_MB"] = str(5056 + extra_mib + 64)
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
                        help="The game's argv, program name first (repeatable). Uncharted 2/3 started directly "
                             "get the Collection menu's argv: eboot.bin ispartofuccollection")
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
        guest_args = ["/app0/eboot.bin"] if executable == "eboot.bin" else ["eboot.bin", "ispartofuccollection"]
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
