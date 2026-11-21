#!/usr/bin/env python3
"""Run the paper-cut PC port of TTYD as a reference, from scripts.

    tools/port3ds/run_papercut.py --map gor_01 --frames 600 --shot 400
    tools/port3ds/run_papercut.py --frames 300 --print "*gp" --print "gp->mapName"
    tools/port3ds/run_papercut.py --ptype GlobalWork

paper-cut is expected next to this repo (../paper-cut) and built with:

    cd ../paper-cut
    PATH=/usr/lib/llvm-21/bin:$PATH cmake -S . -B build -G Ninja \\
      --toolchain cmake/toolchains/linux-clang.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
      -DTTYD_VERSION=G8ME01 -DTTYD_BACKEND=aurora -DSDL_X11_XTEST=OFF
    ninja -C build

Runs use an isolated user folder (build/papercut-profile/) with the intro
skipped, paper-cut's deterministic 60 Hz clock (TTYD_FAST), an offscreen
window and Discord status disabled. Screenshots go to build/papercut-last/.

--print runs the game under gdb (paper-cut is a child of gdb, so no ptrace
permission is needed), stops at the start of the Nth marioStMain call and
prints each expression with paper-cut's debug info. --ptype prints a type's
layout with field offsets (paper-cut is 64-bit: pointers are 8 bytes, so
offsets differ from the GameCube and 3DS layouts).

Exit status: paper-cut's own (0 ok, 3 map not reached), 2 on setup errors.
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PAPERCUT = REPO.parent / "paper-cut"
PROFILE = REPO / "build" / "papercut-profile"
LAST = REPO / "build" / "papercut-last"
EXE = {"G8ME01": "ttyd-us", "G8MJ01": "ttyd-jp", "G8MP01": "ttyd-eu"}


def main():
    ap = argparse.ArgumentParser(description="Run paper-cut (TTYD PC port) from scripts.")
    ap.add_argument("--version", default="G8ME01", choices=sorted(EXE))
    ap.add_argument("--iso", type=Path, default=REPO / "iso" / "game.iso")
    ap.add_argument("--map", help="warp to this map (optionally map,bero)")
    ap.add_argument("--frames", type=int, default=600, help="exit after N frames (on --map, if given)")
    ap.add_argument("--shot", help="frame or frame range to screenshot, e.g. 400 or 100-200")
    ap.add_argument("--input", help="paper-cut input script (TTYD_INPUT)")
    ap.add_argument("--print", dest="prints", action="append", default=[], help="gdb expression to print")
    ap.add_argument("--ptype", help="print a type's layout with offsets and exit")
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()

    exe = PAPERCUT / "build" / EXE[args.version]
    if not exe.is_file():
        print(f"run_papercut: {exe} not found; build paper-cut first (see --help)")
        return 2

    if args.ptype:
        r = subprocess.run(["gdb", "-batch", "-nx", "-ex", f"ptype /o {args.ptype}", str(exe)],
                           capture_output=True, text=True)
        print(r.stdout or r.stderr)
        return r.returncode

    if not args.iso.is_file():
        print(f"run_papercut: {args.iso} not found")
        return 2

    PROFILE.mkdir(parents=True, exist_ok=True)
    (PROFILE / "settings.txt").write_text("skip_intro=1\nfullscreen=0\n")
    if LAST.exists():
        shutil.rmtree(LAST)
    LAST.mkdir(parents=True)

    env = dict(os.environ)
    env.update({
        "TTYD_OFFSCREEN": "1",
        "SDL_VIDEODRIVER": env.get("SDL_VIDEODRIVER", "x11"),
        "TTYD_FAST": "1",
        "TTYD_USER_DIR": str(PROFILE),
        "TTYD_NO_LAUNCHER": "1",
        "TTYD_EXIT_FRAMES": str(args.frames),
        "TTYD_WATCHDOG": "60",
        "TTYD_DISCORD_IPC": str(PROFILE / "no-discord"),
        "TTYD_SHOT_DIR": str(LAST),
    })
    if args.map:
        env["TTYD_MAP"] = args.map
    if args.shot:
        env["TTYD_SHOT"] = args.shot
    if args.input:
        env["TTYD_INPUT"] = args.input

    cmd = [str(exe), str(args.iso.resolve())]
    if args.prints:
        # gdb stops the run itself; keep paper-cut's own exit well past it.
        env["TTYD_EXIT_FRAMES"] = str(args.frames + 3600)
        gdb = ["gdb", "-batch", "-nx",
               "-ex", "set pagination off", "-ex", "set print pretty on",
               "-ex", "handle SIGPIPE SIGUSR1 SIGUSR2 nostop noprint",
               "-ex", "break marioStMain",
               "-ex", f"ignore 1 {max(args.frames - 1, 0)}",
               "-ex", "run"]
        for expr in args.prints:
            gdb += ["-ex", f"print {expr}"]
        gdb += ["-ex", "kill", "--args"] + cmd
        cmd = gdb

    with open(LAST / "stdout.txt", "w") as out:
        try:
            r = subprocess.run(cmd, env=env, stdout=out, stderr=subprocess.STDOUT, timeout=args.timeout,
                               cwd=str(PAPERCUT))
            code = r.returncode
        except subprocess.TimeoutExpired:
            print(f"run_papercut: FAIL: timeout after {args.timeout:.0f}s")
            return 1

    if args.prints:
        text = (LAST / "stdout.txt").read_text(errors="replace")
        start = text.find("Breakpoint 1, ")
        print(text[start:] if start >= 0 else text[-3000:])
        return 0 if start >= 0 else 1

    shots = sorted(p.name for p in LAST.glob("*.png"))
    print(f"run_papercut: exit {code}; screenshots: {', '.join(shots) or 'none'} in {LAST.relative_to(REPO)}/")
    return code


if __name__ == "__main__":
    sys.exit(main())
