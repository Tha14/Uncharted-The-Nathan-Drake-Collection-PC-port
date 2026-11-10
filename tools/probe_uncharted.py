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

ROOT = Path(__file__).resolve().parent.parent

# The code patches applied to each game's linked image (the fixes, the unlocks, the cheats that need code, ultrawide,
# the job workers, Uncharted 3's exposure) and the settings that choose them: launcher/native/patch_tables.cpp.
EXECUTABLES = ("eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf")
# Exit code of sceSystemServiceLoadExec (runtime_services.c): the game asked for another
# executable of the Collection; GOW3_LOADEXEC_FILE holds its /app0 path and arguments.
LOADEXEC_EXIT = 30


def apply_image_patches(image, executable):
    """Applies the game's code patches for the current settings to the linked image (launcher/native/patch_tables.cpp
    and patcher.cpp): prints what was patched; SystemExit when a patch's place holds other bytes (the image is left as
    it was)."""
    import game_patcher
    game_patcher.apply_image_patches(image, executable)


def game_executable():
    """The runtime as Uncharted.exe (Task Manager, Windows' graphics settings and overlays show this name), copied
    from the build output whenever they differ (launcher/native/starter_files.cpp; UNCHARTED_TEST_PROBE_EXE=1 and
    UNCHARTED_PROBE_EXE=<file> for tests)."""
    import game_patcher
    path, _, message = game_patcher.starter_files("game_executable", ROOT).partition("\n")
    if message:
        print(message)
    return Path(path)


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
    """The render resolution in the linked image `data` (launcher/native/patcher.cpp: the display settings' stores,
    the screen wipe, the render target heaps and the memory total check). Returns how many MiB the memory areas
    grew; ResolutionTooLarge when the size doesn't fit (move=False: Drake's Fortune keeps its render targets in the
    GPU area), SystemExit when the game's code is not as expected."""
    import game_patcher
    return game_patcher.resolution_patch(data, width, height, move, ResolutionTooLarge)


GAME_KEYS = {"eboot.bin": "u1", "big2-ps4_Shipping.elf": "u2", "big3-ps4_Shipping.elf": "u3"}


def find_update(game):
    """The game's update (e.g. 1.02, extracted from its PKG): UNCHARTED_UPDATE (a folder, or 0 for
    none), else a folder next to the game's named like it plus -patch/-update (any case), with or
    without uroot. An update folder holds eboot.bin and sce_sys/param.sfo (launcher/native/system.cpp)."""
    import game_patcher
    found, message = (game_patcher.system("find_update", game) + "\n").split("\n")[:2]
    if message:
        print(message)
    return Path(found) if found else None


def sfo_value(game, key):
    """A value of the folder's sce_sys/param.sfo, None when unreadable (launcher/native/system.cpp)."""
    import game_patcher
    text = game_patcher.system("sfo", game)
    return json.loads(text).get(key) if text else None


def game_version(game):
    """APP_VER of the game folder; 01.02 when its executables are 1.02 although param.sfo says
    otherwise (dumps with the update merged in that kept the 1.00 param.sfo), 01.00 when param.sfo
    says 01.02 but the executables are 1.00 (GitHub #148, #176): Uncharted 2's torch code as 1.02
    compiled it and its 1.00 UpdateFocus code (launcher/native/system.cpp)."""
    import game_patcher
    return game_patcher.system("starter_version", game)


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


def boot_view(base, update):
    """For drives that hold no links (exFAT, FAT32): copies of the files the prepare scripts read (the executables,
    sce_module, sce_sys), the update's over the game's, in analysis/views, copied again only when a source changes
    (launcher/native/starter_files.cpp). The runtime reads the update over the game itself (--app0-overlay).
    OSError when they could not be copied."""
    import game_patcher
    return Path(game_patcher.starter_files("boot_view", ROOT, base, update))


def run_once(args, executable, guest_args, loadexec):
    base = args.game.resolve()
    update = find_update(base)
    if not update:
        version = game_version(base)
        if version and version < "01.02":
            title = base.parent if base.name.lower() == "uroot" else base
            print(f"Game version {version} without update 1.02: the fixes written for 1.02 are skipped "
                  f"(the patches marked NOT applied). Extract update 1.02 next to the game folder as "
                  f"{title.name}-patch.")
        return run_game(args, base, executable, guest_args, loadexec)
    print(f"Update {sfo_value(update, 'APP_VER') or '?'}: {update}")
    if (update_version := game_version(update)) and update_version < "01.02":
        print(f"The update folder holds version {update_version} executables (its param.sfo says "
              f"otherwise): the fixes written for 1.02 are skipped. Extract update 1.02 again.")
    try:
        if os.environ.get("UNCHARTED_TEST_OVERLAY") == "1":  # test: the drive-without-links path
            raise SystemExit("UNCHARTED_TEST_OVERLAY=1")
        view = game_view(base, update)
    except SystemExit as error:
        # exFAT/FAT32 drives (USB sticks) hold no links ([WinError 1] Incorrect function): the
        # runtime lays the update over the game itself, and the prepare scripts read copies of
        # the few files they need.
        print(f"{error}: the runtime lays the update over the game instead (drive without links)")
        try:
            boot = boot_view(base, update)
        except OSError as copy_error:
            print(f"Update files not copied ({copy_error}): starting the game without its update")
            return run_game(args, base, executable, guest_args, loadexec)
        return run_game(args, boot, executable, guest_args, loadexec, app0=base, overlays=[update])
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


def instance_suffix():
    """UNCHARTED_INSTANCE=<n> (n >= 2; co-op tests, 25 Oct 2026): another game on this PC with its
    own folders (analysis/<game>-i<n>, analysis/uncharted-user-i<n>), so that two instances can
    run side by side. Instance 1, the default,
    uses the normal folders."""
    import game_patcher
    return game_patcher.starter("instance_suffix")


def user_dir():
    """The user folder (saves, settings, caches) of this instance; a new instance's starts with copies of the main
    one's settings, saves, trophies and shader caches (launcher/native/starter_files.cpp)."""
    import game_patcher
    path, _, message = game_patcher.starter_files("user_dir", ROOT, instance_suffix()).partition("\n")
    if message:
        print(message)
    return Path(path)


def u3mp_image(executable):
    """Uncharted 3 multiplayer, set by the launcher (Multiplayer tab, u3mp/patcher/u3mp_import.py):
    UNCHARTED_U3MP_IMAGE (the linked image), UNCHARTED_U3MP_IMAGE_SHA256 (its digest),
    UNCHARTED_U3MP_OVERLAYS (folders, os.pathsep between them, the first wins), UNCHARTED_U3MP_VERSION.
    Anything missing or a digest mismatch: None, and Uncharted 3 starts as usual."""
    import game_patcher
    message, _, found = game_patcher.starter("u3mp_image", executable).partition("\n")
    if message:
        print(message)
    if not found:
        return None
    info = json.loads(found)
    return {"image": Path(info["image"]), "overlays": [Path(p) for p in info["overlays"]], "version": info["version"]}


def run_game(args, game, executable, guest_args, loadexec, app0=None, overlays=()):
    """game: the folder the prepare scripts read; app0 (default: game) with overlays laid over
    it is what the game sees as /app0."""
    out = ROOT / "analysis" / (("uncharted" if executable == "eboot.bin" else Path(executable).stem)
                               + instance_suffix())
    user = user_dir()
    user.mkdir(parents=True, exist_ok=True)
    scripts = ROOT / "gow3_pc/scripts"
    for script in ("prepare.py", "link_modules.py", "content_profile.py"):
        prepare_command = [sys.executable, str(scripts / script), str(game), "--out", str(out)]
        if script != "content_profile.py":
            prepare_command += ["--executable", executable]
        subprocess.run(prepare_command, check=True)
    mp = u3mp_image(executable)
    if mp:
        # Uncharted 3 multiplayer (launcher -> Multiplayer): the image the launcher's import linked on
        # this PC replaces the prepared one. It already holds the port's code patches it was tested
        # with, so the patches are not applied again (some would not find their bytes).
        shutil.copyfile(mp["image"], out / "boot-linked.bin")
        print(f"Uncharted 3 multiplayer v{mp['version']}: {mp['image']} with {len(mp['overlays'])} overlays")
    else:
        apply_image_patches(out / "boot-linked.bin", executable)
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
            # Ultrawide: smaller sizes of the same shape (width/16 and height/4 whole: Uncharted 3
            # asserts widths in steps of 16 in its post processing).
            candidates = [(width, height)] + [
                (int(width * scale) // 16 * 16, int(height * scale) // 4 * 4)
                for scale in (0.9, 0.8, 0.75, 2 / 3, 0.6, 0.5)]
        extra_mib = 0
        for candidate in candidates:
            patched = bytearray(data)
            try:
                extra_mib = resolution_patch(patched, *candidate, move=move)
            except ResolutionTooLarge as error:
                print(f"Render resolution {error}")
                continue
            except SystemExit as error:
                if not mp:
                    raise
                # The multiplayer image keeps the PS4's render size if the patch does not fit it.
                print(f"Uncharted 3 multiplayer: render resolution not changed ({error})")
                break
            if candidate != (width, height):
                print(f"Drake's Fortune renders at {candidate[0]}x{candidate[1]} instead of {width}x{height}")
            data, render_size = patched, candidate
            break
        image.write_bytes(data)
    else:
        extra_mib = 0
    # The runtime's options (GOW3_* environment values) from the launcher's (UNCHARTED_*), the game and its render
    # size: cheats and extra quality, renderer and readback settings, the controls, upscaling, diagnostics and the
    # trigger files (launcher/native/runtime_env.cpp, which keeps the reasons for each).
    import game_patcher
    env, capture, trigger = game_patcher.runtime_env(executable, game, game_key, user, out, render_size, (width, height),
                                                     extra_mib, loadexec, args.capture)
    window_icon = game_window_icon(game)
    if window_icon:
        env.setdefault("GOW3_WINDOW_ICON", str(window_icon))
    command = [str(game_executable()), str(out / "boot-linked.bin"),
               "--no-game-hooks", "--low-image", "--content-profile", str(out / "content.bin"),
               "--app0", str(app0 or game), "--user", str(user)]
    # Uncharted 2 multiplayer (u2mp, set by the launcher): UNCHARTED_U2MP_OVERLAY, a folder made from the
    # player's own game (u2mp/patcher/u2mp_overlay.py), laid over /app0 ahead of the update.
    # os.pathsep between folders (the menu overlay, then the PS3 import); the first holding a file wins.
    if executable == "big2-ps4_Shipping.elf":
        for u2mp in os.environ.get("UNCHARTED_U2MP_OVERLAY", "").split(os.pathsep):
            if u2mp and Path(u2mp).is_dir():
                command += ["--app0-overlay", u2mp]
    # Uncharted 3 multiplayer: its four overlays (the first wins) ahead of the update.
    for overlay in [*(mp["overlays"] if mp else ()), *overlays]:
        command += ["--app0-overlay", str(overlay)]
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
        process = None
        if mp and os.name == "nt":
            # As the multiplayer runner starts it: ASLR off for this child (tools/u3mp_process.py).
            try:
                from u3mp_process import Popen as U3mpPopen
                process = U3mpPopen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
            except (OSError, RuntimeError, ValueError) as error:
                print(f"Uncharted 3 multiplayer: started without its address layout ({error})")
        if process is None:
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        # Freeze watchdog (needs the Frame stats lines, i.e. diagnostics on). UNCHARTED_HANG_SECONDS
        # sets its wait (30 s), 0 turns it off.
        hang_seconds = int(os.environ.get("UNCHARTED_HANG_SECONDS", "30") or 0)
        # The watchdog (launcher/native/hang.cpp, tools/hang_report.py): the runtime logs a Frame stats line every 5 s
        # while frames are shown; none for that long while the game runs writes every thread's stack to
        # hang-<time>.txt beside the log (small enough to attach to an issue) and, in the source tree or with
        # UNCHARTED_HANG_DUMP=1, a minidump to dumps/ (the newest 2 stay). Once per freeze; frames re-arm it.
        watchdog = 0
        if os.name == "nt" and args.boot and hang_seconds > 0 and env.get("GOW3_FRAME_STATS") == "1":
            import hang_report
            watchdog = hang_report.hang_watchdog(process, out / (name + ".log"), out / "dumps", hang_seconds, ROOT)
        # Resource tracking for long sessions (memory leaks; launcher/native/memstat.cpp): memory-<time>.csv beside
        # the log, one row every UNCHARTED_MEMORY_SECONDS (10; 0 turns it off); each session's goes with its attempt.
        memory_csv = None
        memory_seconds = float(os.environ.get("UNCHARTED_MEMORY_SECONDS", "10") or 0)
        if os.name == "nt" and args.boot and memory_seconds > 0:
            import hang_report
            memory_csv = hang_report.memory_monitor(process, out, memory_seconds)
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
    # The game has ended: the log can take the freeze reports' names (the launcher looks there).
    hang_reports = []
    if args.boot and watchdog:
        import hang_report
        hang_reports = hang_report.hang_reports(watchdog)
    if args.boot and hang_reports:
        with open(out / (name + ".log"), "ab") as log:
            log.write("".join(f"\nHang watchdog: the game froze; report {report}" for report in hang_reports)
                      .encode("utf-8") + b"\n")
    attempt = out / "attempts" / (name + "-" + str(time.time_ns()))
    attempt.mkdir(parents=True)
    for suffix in (".log", ".json"):
        shutil.copy2(out / (name + suffix), attempt / (name + suffix))
    for report in hang_reports if args.boot else []:
        shutil.copy2(report, attempt / report.name)
    if args.boot and memory_csv is not None and memory_csv.exists():
        shutil.copy2(memory_csv, attempt / memory_csv.name)
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
    user = user_dir()
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
        if target not in EXECUTABLES:
            print(f"Game asked to start {lines[0] if lines else '(nothing)'}; not part of the Collection")
            break
        # LoadExec's argv is the new program's complete argv (the menu passes "eboot.bin" as argv[0]
        # for Uncharted 2/3; an extra leading argument makes them skip the menu and load nothing).
        executable, guest_args = target, lines[1:] or ["/app0/" + target]
        print(f"Collection: switching to {executable} {' '.join(guest_args)}")
    return 124 if result["timed_out"] else result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
