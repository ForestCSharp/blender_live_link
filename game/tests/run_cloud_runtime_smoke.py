#!/usr/bin/env python3
"""Launch the game and fake-send the procedural cloud smoke scene."""

import argparse
import os
import pathlib
import signal
import subprocess
import sys


GAME_ROOT = pathlib.Path(__file__).resolve().parents[1]
REPO_ROOT = GAME_ROOT.parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--windowed", action="store_true",
        help="use the normal game window instead of fullscreen")
    parser.add_argument(
        "--timings", action="store_true",
        help="print periodic GPU pass timings")
    camera_group = parser.add_mutually_exclusive_group()
    camera_group.add_argument(
        "--ground-view", action="store_true",
        help="aim at the ground receiver to inspect moving cloud shadows")
    camera_group.add_argument(
        "--horizon-view", action="store_true",
        help="aim two degrees above the horizon to inspect temporal stability")
    args = parser.parse_args()

    sender_env = os.environ.copy()
    sender_env.setdefault("CLOUD_SMOKE_CONNECT_SECONDS", "60")
    sender_env.setdefault("CLOUD_SMOKE_HOLD_SECONDS", "30")
    if args.ground_view:
        sender_env["CLOUD_SMOKE_CAMERA"] = "ground"
    elif args.horizon_view:
        sender_env["CLOUD_SMOKE_CAMERA"] = "horizon"
    sender = subprocess.Popen(
        [sys.executable, str(GAME_ROOT / "tests/send_cloud_runtime_smoke.py")],
        cwd=REPO_ROOT,
        env=sender_env,
    )

    game_env = os.environ.copy()
    if args.timings:
        game_env["GAME2_PRINT_GPU_TIMINGS"] = "1"
    # Deliberately do not set GAME2_RENDER_SCALE: the smoke run exercises the
    # engine's normal default dynamic-resolution percentage.
    game_command = [str(GAME_ROOT / "bin/game")]
    if not args.windowed:
        game_command.append("--fullscreen")

    game = None
    try:
        game = subprocess.Popen(game_command, cwd=GAME_ROOT, env=game_env)
        return game.wait()
    except KeyboardInterrupt:
        if game and game.poll() is None:
            game.send_signal(signal.SIGINT)
            game.wait()
        return 130
    finally:
        if sender.poll() is None:
            sender.terminate()
        sender.wait()


if __name__ == "__main__":
    raise SystemExit(main())
