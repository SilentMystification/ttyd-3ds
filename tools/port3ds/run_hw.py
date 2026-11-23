#!/usr/bin/env python3
"""Run a build on the New 3DS and follow its log.

    N3DS_IP=192.168.1.50 tools/port3ds/run_hw.py --switches "map=gor_01" --until "warp: gor_01" --timeout 300
    tools/port3ds/run_hw.py --ip 192.168.1.50 --build build/3ds/probe/ttyd_probe.3dsx --until "PROBE DONE"

1. Sends the .3dsx with 3dslink (the console must be in the Homebrew
   Launcher, waiting for netload: press Y there). The switches become the
   program's arguments, which the game reads as it reads debug3ds.txt.
2. Connects to the game's live log (TCP port 17492; a netloaded game opens
   it by itself) and copies every line to the terminal and to
   build/hw-last/log.txt. The game keeps its last lines in a ring and sends
   them on connect, so nothing before the connection is lost.
   The switches always include "livelog" (the game opens the log port for
   any start), unless the switches include "nolivelog". It also adds
   "fastboot" (A pressed through the boot screens) unless "nofastboot" is given.
   For a game started from the HOME Menu (the CIA), add "livelog" to
   sdmc:/3ds/ttyd/debug3ds.txt, start it, and attach with --no-send:
       tools/port3ds/run_hw.py --no-send --until "warp:" --timeout 300
3. Stops on --until (pass), a [CRASH] line (fail), --timeout, or the game
   closing the connection (fail unless --until already matched).

The full log is also on the SD card: sdmc:/3ds/ttyd/latest.log (and previous.log,
the run before), for ftpd.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LIVE_PORT = 17492
OUT = ROOT / "build/hw-last"


def find_3dslink():
    for p in (os.environ.get("DEVKITPRO", "/opt/devkitpro") + "/tools/bin/3dslink", "3dslink"):
        if Path(p).exists() or p == "3dslink":
            return p
    return "3dslink"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("N3DS_IP"), help="console address (default: $N3DS_IP)")
    ap.add_argument("--build", type=Path, default=ROOT / "build/3ds/game/ttyd.3dsx")
    ap.add_argument("--switches", default="", help="space-separated switches, passed as arguments")
    ap.add_argument("--until", help="regex in the log that means success")
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--connect-timeout", type=float, default=60.0, help="seconds to retry the live log connection")
    ap.add_argument("--no-send", action="store_true", help="only connect to a game that is already running")
    args = ap.parse_args()
    if not args.ip:
        print("run_hw: set N3DS_IP or pass --ip", file=sys.stderr)
        return 2

    OUT.mkdir(parents=True, exist_ok=True)
    sw = args.switches.split()
    if "nofastboot" in sw:
        sw.remove("nofastboot")
    elif "fastboot" not in sw:
        sw.append("fastboot")  # presses A through the boot screens
    if "nolivelog" in sw:
        sw.remove("nolivelog")
    elif "livelog" not in sw:
        sw.append("livelog")
    if not args.no_send:
        cmd = [find_3dslink(), "-a", args.ip, "-r", "10", str(args.build)] + sw
        print("run_hw: " + " ".join(cmd), flush=True)
        if subprocess.run(cmd).returncode != 0:
            print("run_hw: FAIL: 3dslink could not send the build (is the Homebrew Launcher waiting? press Y)")
            return 1

    sock = None
    start = time.monotonic()
    last_err = -1e9
    print("run_hw: waiting for live log...", flush=True)
    while sock is None:
        try:
            sock = socket.create_connection((args.ip, LIVE_PORT), timeout=5)
        except OSError as e:
            now = time.monotonic()
            if now - start >= args.connect_timeout:
                print(f"run_hw: FAIL: no live log on {args.ip}:{LIVE_PORT} after {args.connect_timeout:.0f} s "
                      f"({e}; the game did not start, or crashed early: see sdmc:/3ds/ttyd/latest.log)")
                return 1
            if now - last_err >= 10:
                print(f"run_hw: connect failed: {e}", flush=True)
                last_err = now
            time.sleep(1)
    deadline = time.monotonic() + args.timeout
    sock.settimeout(1.0)
    until = re.compile(args.until) if args.until else None
    passed = False
    text = ""
    buf = b""
    with open(OUT / "log.txt", "w") as log:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                chunk = b""
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                s = line.decode("utf-8", "replace")
                print(s, flush=True)
                log.write(s + "\n")
                log.flush()
                text += s + "\n"
                if "[CRASH]" in s:
                    # collect the rest of the report
                    deadline = min(deadline, time.monotonic() + 3)
            if until and until.search(text):
                passed = True
                break
    sock.close()
    if "[CRASH]" in text:
        print("run_hw: FAIL: crash (report above; build/hw-last/log.txt)")
        return 1
    if passed:
        print(f"run_hw: PASS: matched {args.until!r}")
        return 0
    if until is None:
        print("run_hw: log closed or timed out (no --until given)")
        return 0
    print("run_hw: FAIL: " + ("timeout" if time.monotonic() >= deadline else "the game closed the log"))
    return 1


if __name__ == "__main__":
    sys.exit(main())
