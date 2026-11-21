#!/usr/bin/env python3
"""Run the GameCube game in Dolphin headless and read game memory by symbol.

    tools/port3ds/run_dolphin.py --frames 600 --read gp:4 --deref gp:0x100 --json out.json

Boots iso/game.iso (or --dol with the ISO as the disc) with no window, no
video output and the GDB stub enabled, in an isolated Dolphin user folder
(build/dolphin-profile/), so your own Dolphin settings are untouched.

Frames are counted with a breakpoint on marioStMain, which the game calls
once per frame, so "--frames 600" means after 600 game updates. Symbols come
from the decomp's ELF (build/<version>/MarioSt.elf). The decomp builds a
byte-identical DOL, so its symbols are valid for the retail disc too.

  --read NAME[:SIZE]     read SIZE bytes (default: the symbol's size) at NAME
  --deref NAME:SIZE      read the 4-byte pointer at NAME, then SIZE bytes at
                         the address it holds (for example gp -> GlobalWork)
  --addr 0xADDR:SIZE     read raw memory

Results are printed as hex and, with --json, written as
{"frame": N, "reads": {label: {"addr": A, "hex": "..."}}}.
Exit status: 0 ok, 1 failure, 2 usage or setup error.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfsyms import load_symbols  # noqa: E402
from gdbrsp import RSP, RSPError  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
PROFILE = REPO / "build" / "dolphin-profile"
LAST = REPO / "build" / "dolphin-last"


def die(msg, code=1):
    print(f"run_dolphin: FAIL: {msg}")
    sys.exit(code)


def parse_size(text):
    return int(text, 0)


def main():
    ap = argparse.ArgumentParser(description="Run TTYD in headless Dolphin and read memory by symbol.")
    ap.add_argument("--version", default="G8ME01")
    ap.add_argument("--iso", type=Path, default=REPO / "iso" / "game.iso")
    ap.add_argument("--dol", type=Path, help="boot this DOL with --iso as the disc")
    ap.add_argument("--elf", type=Path, help="symbol source (default build/<version>/MarioSt.elf)")
    ap.add_argument("--frames", type=int, default=0, help="game frames (marioStMain calls) to run first")
    ap.add_argument("--read", action="append", default=[])
    ap.add_argument("--deref", action="append", default=[])
    ap.add_argument("--addr", action="append", default=[])
    ap.add_argument("--json", type=Path)
    ap.add_argument("--port", type=int, default=55555)
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()

    elf = args.elf or REPO / "build" / args.version / "MarioSt.elf"
    if not elf.is_file():
        die(f"{elf} not found; build the decomp first (python configure.py --version {args.version} && ninja)", 2)
    if not args.iso.is_file():
        die(f"{args.iso} not found", 2)
    syms = load_symbols(elf)

    PROFILE.mkdir(parents=True, exist_ok=True)
    LAST.mkdir(parents=True, exist_ok=True)
    cmd = ["dolphin-emu-nogui", "-u", str(PROFILE), "-p", "headless", "-v", "Null",
           "-C", f"Dolphin.General.GDBPort={args.port}",
           "-C", "Dolphin.Core.CPUCore=1",
           # The JIT only honours breakpoints with debugging enabled.
           "-C", "Dolphin.Interface.DebugModeEnabled=True",
           "-C", "Dolphin.Core.EmulationSpeed=0",
           "-C", "Dolphin.Interface.ConfirmStop=False",
           "-C", "Dolphin.Analytics.PermissionAsked=True"]
    if args.dol:
        cmd += ["-C", f"Dolphin.Core.DefaultISO={args.iso.resolve()}", "-e", str(args.dol.resolve())]
    else:
        cmd += ["-e", str(args.iso.resolve())]

    log = open(LAST / "dolphin_stdout.txt", "w")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    rsp = None
    result = {"frame": 0, "reads": {}}
    start = time.monotonic()
    try:
        rsp = RSP("127.0.0.1", args.port, connect_timeout=30.0, timeout=args.timeout)
        rsp.stop_reason()

        if args.frames > 0:
            bp = syms["marioStMain"].addr
            rsp.set_breakpoint(bp)
            for frame in range(args.frames):
                if time.monotonic() - start > args.timeout:
                    die(f"timeout after {frame} frames")
                rsp.cont()
            rsp.clear_breakpoint(bp)
            result["frame"] = args.frames

        def record(label, addr, size):
            data = rsp.read(addr, size)
            result["reads"][label] = {"addr": addr, "hex": data.hex()}
            print(f"{label} @ 0x{addr:08X} ({size} bytes): {data[:64].hex()}{'...' if size > 64 else ''}")

        for spec in args.read:
            name, _, size = spec.partition(":")
            if name not in syms:
                die(f"unknown symbol {name}", 2)
            record(name, syms[name].addr, parse_size(size) if size else syms[name].size)
        for spec in args.deref:
            name, _, size = spec.partition(":")
            if name not in syms or not size:
                die(f"--deref needs a known symbol and a size: {spec}", 2)
            ptr = struct.unpack(">I", rsp.read(syms[name].addr, 4))[0]
            record(f"*{name}", ptr, parse_size(size))
        for spec in args.addr:
            addr, _, size = spec.partition(":")
            record(addr, int(addr, 0), parse_size(size or "4"))
    except RSPError as e:
        die(str(e))
    finally:
        if rsp is not None:
            rsp.close()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        log.close()

    if args.json:
        args.json.write_text(json.dumps(result, indent=1))
    print(f"run_dolphin: OK ({result['frame']} frames, {time.monotonic() - start:.1f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
