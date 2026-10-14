"""Drive a probe run from boot into the saved game: tap Cross through the menus, then hold
optional movement. Writes the runtime's scripted pad file (see drive_game.py)."""
import argparse
from pathlib import Path
import subprocess
import time

PAD = Path(__file__).resolve().parent.parent / "analysis/uncharted-user/probe-pad.txt"


class GameClosed(Exception):
    pass


def game_running():
    result = subprocess.run(["tasklist", "/FI", "IMAGENAME eq gow3-probe.exe", "/NH"],
                            capture_output=True, text=True)
    return "gow3-probe.exe" in result.stdout


def write(tokens):
    # Stop as soon as the game is gone, so scripted input never outlives its run.
    if not game_running():
        raise GameClosed
    PAD.write_text(tokens, encoding="ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--menu-seconds", type=int, default=90, help="How long to tap Cross")
    parser.add_argument("--move-seconds", type=int, default=0, help="Then walk forward this long")
    args = parser.parse_args()
    # Wait up to a minute for the game to start.
    deadline = time.monotonic() + 60
    while not game_running() and time.monotonic() < deadline:
        time.sleep(1)
    try:
        end = time.monotonic() + args.menu_seconds
        while time.monotonic() < end:
            write("cross")
            time.sleep(0.15)
            write("")
            time.sleep(3)
        end = time.monotonic() + args.move_seconds
        turn = 0
        while time.monotonic() < end:
            # Walk forward while slowly turning the camera, so new geometry streams in.
            write("ly=0 rx=%d" % (200 if turn % 2 else 60))
            time.sleep(2)
            turn += 1
    except GameClosed:
        pass
    finally:
        PAD.write_text("", encoding="ascii")
    print("Released scripted pad input")


if __name__ == "__main__":
    main()
