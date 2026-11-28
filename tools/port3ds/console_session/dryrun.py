#!/usr/bin/env python3
"""The pipeline drill: the whole manifest end to end without the console.

    dryrun.py fake                          # fakegame.py + azsv.py on loopback: host only, a few minutes, faults injected
    dryrun.py azahar --cia dev=PATH         # one headless Azahar (azahar-session --supervise) + azsv.py, the real game
                                            # (needs a free Azahar slot: AZAHAR_MAX, and a dev CIA built from port3ds)

It starts the stand-ins, runs `session.py run --dry <backend>` against them (never a console address), checks the
session's own verdicts against what the drill expects, runs the collector, and stops the stand-ins. Exit 0 when every
check holds. The checks are the proof that push, restart, files, report, retries, the recovery path, the bisect, the
gates and the collection work; what they cannot prove is listed in the runbook ("What the dry run cannot show").
"""
import argparse
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PY = sys.executable


def fake_cia(path):
    """A CIA the title-ID walk of devupdate.py accepts (as in devupdate.py selftest)."""
    hdr = struct.pack("<IHHIIII", 0x20, 0, 0, 0, 0, 0x200, 0) + b"\0" * 8
    tmd = struct.pack(">I", 0x10004) + b"\0" * (0x100 + 0x3C) + b"\0" * 0x4C + struct.pack(">Q", 0x000400000FD88600)
    Path(path).write_bytes(hdr.ljust(64, b"\0") + tmd + b"\0" * 1024)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port, seconds=10):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def check(name, ok, detail=""):
    print(f"  {'ok  ' if ok else 'FAIL'} {name}{': ' + detail if detail and not ok else ''}")
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("backend", choices=["fake", "azahar"])
    ap.add_argument("--cia", action="append", default=[], metavar="KEY=PATH")
    ap.add_argument("--out", help="the session folder (default: a temp folder)")
    ap.add_argument("--port", type=int, help="azahar: the session's devupdate port")
    ap.add_argument("--timeout-scale", type=float, default=1.0)
    ap.add_argument("--keep", action="store_true", help="leave the stand-ins running")
    a = ap.parse_args()
    tmp = Path(tempfile.mkdtemp(prefix="cs-dry-"))
    out = Path(a.out) if a.out else tmp / "session"
    procs = []
    sv_port = free_port()
    try:
        if a.backend == "fake":
            game_port = free_port()
            fake_cia(tmp / "game.cia")
            inject = [
                {"match": "probefresh", "fault": "crash"},            # A2a: the probe dies in its first run, the retry resumes
                {"match": "hwb=A4.gor_01.D-1", "fault": "hang_forever"},  # a hung baseline run: timeout, recovery, retry
                {"match": "hwb=D1.gor_01.C3-1", "fault": "nostart"},      # no first heartbeat: loop's own terminate + launch
            ]
            (tmp / "inject.json").write_text(json.dumps(inject))
            procs.append(subprocess.Popen([PY, str(HERE / "fakegame.py"), "--port", str(game_port), "--inject", str(tmp / "inject.json")],
                                          stdout=open(tmp / "fakegame.log", "w"), stderr=subprocess.STDOUT))
            cias = ["dev=" + str(tmp / "game.cia"), "release=" + str(tmp / "game.cia")]
        else:
            game_port = a.port
            cias = a.cia
        if not wait_port(game_port, 15):
            print("dryrun: the game stand-in does not answer on", game_port)
            return 2
        procs.append(subprocess.Popen([PY, str(HERE / "azsv.py"), "--up-port", str(game_port), "--port", str(sv_port)],
                                      stdout=open(tmp / "azsv.log", "w"), stderr=subprocess.STDOUT))
        if not wait_port(sv_port, 10):
            print("dryrun: azsv does not answer")
            return 2
        cmd = [PY, str(HERE / "session.py"), "run", "--dry", a.backend, "--ip", "127.0.0.1", "--port", str(game_port),
               "--sv-port", str(sv_port), "--out", str(out), "--sv-wait", "30", "--timeout-scale", str(a.timeout_scale),
               *[x for c in cias for x in ("--cia", c)]]
        print("dryrun:", " ".join(cmd[1:]))
        t0 = time.monotonic()
        r = subprocess.run(cmd)
        print(f"dryrun: session exit {r.returncode} after {time.monotonic() - t0:.0f} s; folder {out}")
        steps = {x["id"]: x for x in json.loads((out / "steps.json").read_text())}
        ok = True
        ok &= check("every planned step has a record", len(steps) >= 24, str(len(steps)))
        for sid in ("A1", "A3a", "A3b", "B2", "B3", "B3r", "D1v", "D3v1", "D4a", "R1", "R2", "H1", "H2p", "H4", "H3", "H3p", "H4bl"):
            ok &= check(f"{sid} PASS", steps.get(sid, {}).get("status") == "PASS", str(steps.get(sid, {}).get("why")))
        ok &= check("X1 FAIL after its retry (forced failure, session went on)",
                    steps.get("X1", {}).get("status") == "FAIL" and len(steps["X1"]["attempts"]) == 2)
        ok &= check("a step after the failure ran (E1a)", "E1a" in steps)
        ok &= check("A4 run: first attempt failed, retry passed" if a.backend == "fake" else "A4 baseline passed",
                    all(s["status"] == "PASS" for k, s in steps.items() if k.startswith("A4.")) and
                    (a.backend != "fake" or any(len(s["attempts"]) == 2 for k, s in steps.items() if k.startswith("A4."))))
        ok &= check("D1 baseline runs passed", all(s["status"] == "PASS" for k, s in steps.items() if k.startswith("D1.")) and
                    any(k.startswith("D1.") for k in steps))
        if a.backend == "fake":
            ok &= check("A2a: the probe died once, the retry (no probefresh) passed",
                        steps["A2a"]["status"] == "PASS" and len(steps["A2a"]["attempts"]) == 2)
            ok &= check("A2c: dies at rt_deleted, the retry ends with DONE", steps["A2c"]["status"] == "PASS")
            b = json.loads((out / "steps/C/bisect.json").read_text())
            ok &= check("bisect found draw 417 and confirmed it", b["culprit"] == 417 and b["confirmed"], str(b["status"]))
            ok &= check("bisect used at most 14 runs", b["runs"] <= 14, str(b["runs"]))
            ok &= check("the nostart run was recovered by the loop (D1 C3 passed)", steps.get("D1.gor_01.C3", {}).get("status") == "PASS")
        res = out / "results"
        ok &= check("report.md written", (res / "report.md").is_file())
        ok &= check("patches written", len(list((res / "patches").glob("*.patch"))) >= 3, str(list((res / "patches").glob("*.patch"))))
        for p in sorted((res / "patches").glob("*.patch")):
            g = subprocess.run(["git", "apply", "--check", "-p1", str(p)], cwd=ROOT, capture_output=True, text=True)
            ok &= check(f"git apply --check {p.name}", g.returncode == 0, g.stderr.strip()[:200])
        print("dryrun: PASS" if ok else "dryrun: FAIL")
        return 0 if ok else 1
    finally:
        if not a.keep:
            for p in procs:
                p.terminate()


if __name__ == "__main__":
    sys.exit(main())
