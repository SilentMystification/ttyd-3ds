#!/usr/bin/env python3
"""Hardware baseline for the 60 fps work (docs/PORT_3DS_60FPS.md, Step 2): the ten scenes of
PORT_3DS_GX_PERF.md under today's defaults (config D) or the A/B configs of the console runbook, in one owner session.

    N3DS_IP=192.168.8.221 tools/port3ds/hw_baseline.py --dry-run          # print the commands only
    N3DS_IP=192.168.8.221 tools/port3ds/hw_baseline.py                    # the full run (30 runs)
    tools/port3ds/hw_baseline.py --scenes gor_01,hei_01 --configs D       # a part of it
    tools/port3ds/hw_baseline.py --configs D,C3,K,I,gpucore1,par,all,old,noqtm,recheap   # --configs list: the switches
    tools/port3ds/hw_baseline.py --ip 127.0.0.1 --port 5060 ...           # an Azahar azahar-session

This script talks to the console only when it is run without --dry-run (the planner, with the owner's
go). The game on the console must be the development CIA, already installed and running with the
devupdate switch (docs/PORT_3DS_60FPS.md Step 2 has the exact order). For each scene and configuration:

  1. `devupdate.py restart "<scene switches> <config> gxthreadstats=60 fastboot livelog hwb=<id>"`
  2. `run_hw.py --no-send` attaches to the live log (port 17492) until the log shows the boot line of
     THIS run (the id in `hwb=`) and then the end of the third measured window.
  3. The `gxthread` window lines (the same lines gx_perf_ab.py reads) are parsed: frame ms, core-0
     busy ms, render ms (render minus its vblank wait) per window, then the mean of the windows.

The scene warps at about game frame 595; windows end at frames 61, 121, ... (every 60). The windows
that count are the first three that end at or after --settle (default 781: frames 781, 841, 901). The
table goes to build/hw-baseline/baseline.md (and results.json, one log per run in the same folder).
Per-run time on the console: boot and warp about 40-60 s, the windows 30-45 s; budget 1.5-2 minutes a
run, about 45-60 minutes for all 30.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
from gx_perf_ab import LINE, RENDER, SCENES, TEN  # noqa: E402  (the scene switches and the log formats, one copy)

DEVUPDATE = ROOT / "libs/ctr-devkit/devupdate/devupdate.py"
RUN_HW = HERE / "run_hw.py"
HW_LAST = ROOT / "build/hw-last/log.txt"  # run_hw.py always writes here
OUT = ROOT / "build/hw-baseline"
C3 = "gxthread=2 gxpipeline=1"
PAR = "jobw1=1 mapnameidx=1 mapdispjob=1"
# The A/B configurations of docs/PORT_3DS_CONSOLE_RUNBOOK.md (step D). D is today's defaults: the switches that
# were once explicit (gxdirty, gxdirect, gximmdirect, gxsdl, gxrecfuse, gxreclgroup, texasync, texfast, dlspread)
# are default on; "old" turns them all off again. Names are case-insensitive on the command line.
CONFIGS = {
    "D": "",                                   # the defaults (gxthread=0)
    "C3": C3,                                  # D + translator thread on core 2
    "K": "gxone=1 gxchunk=1",                  # D + one GX_ProcessCommandList per run
    "I": "gxone=1 gxchunk=1 gximmfl=1",        # K + index arena physical base once per record
    "gpucore1": f"{C3} gpucore=1",             # C3 + render thread on core 1
    "kg1": "gxone=1 gxchunk=1 gpucore=1",      # K + render thread on core 1
    "par": f"{C3} {PAR}",                      # C3 + job worker 1, map name index, mapDisp job
    "all": f"{C3} gpucore=1 {PAR}",            # C3 + gpucore=1 + par
    "old": "gxdirty=0 gxdirect=0 gximmdirect=0 gxsdl=0 gxrecfuse=0 gxreclgroup=0 texasync=0 texfast=0 dlspread=0 tex16=1",
    "noqtm": "noqtm",                          # QTM question 2 (PORT_3DS_QTM.md section 9)
    "recheap": "place=gx_record:heap",         # QTM question 3
    "vramtex": "vramtex=1 vramstats=60",       # QTM tests 5-7
}
ALIASES = {"default": "D", "gxdirty": "D", "gxdirect": "D"}  # the old names: now all the defaults
WINDOW = 60
DEFAULT_SETTLE = 781


def aligned_settle(settle, window):
    """Windows end at 1 + k * window (the first frame of the stats is frame 1): the first end >= settle."""
    k = -(-(settle - 1) // window)
    return 1 + k * window


def run_switches(scene, cfg, window, run_id):
    return " ".join(w for w in (SCENES[scene], CONFIGS[cfg], f"gxthreadstats={window}", "fastboot", "livelog",
                                f"hwb={run_id}") if w)


def commands(args, scene, cfg, run_id, last_frame):
    sw = run_switches(scene, cfg, WINDOW, run_id)
    net = ["--ip", args.ip] if args.ip else []
    restart = [sys.executable, str(DEVUPDATE), *net, *(["--port", str(args.port)] if args.port else []), "restart", sw]
    until = rf"hwb={run_id}[\s\S]*gxthread \d frame {last_frame}:"
    attach = [sys.executable, str(RUN_HW), "--no-send", *net, "--until", until, "--timeout", str(args.timeout),
              "--connect-timeout", str(args.connect_timeout)]
    return sw, restart, attach


def parse(text, run_id, first, count):
    """The window rows of one run: only lines after the boot line that carries this run's id."""
    lines = text.splitlines()
    start = None
    for i, ln in enumerate(lines):
        if f"hwb={run_id}" in ln and "switches" in ln:
            start = i
    if start is None:
        return []
    rows = []
    lines = lines[start:]
    for i, ln in enumerate(lines):
        m = LINE.search(ln)
        if not m or int(m.group(2)) < first:
            continue
        v = [int(x) for x in m.groups()[2:]]  # frame busy fences idle translator copydisp render
        render = v[6]
        for nxt in lines[i + 1:i + 3]:
            rm = RENDER.search(nxt)
            if rm:
                render = int(rm.group(1)) - int(rm.group(2))  # minus the vblank wait
        rows.append({"end": int(m.group(2)), "frame": v[0] / 1000.0, "busy": v[1] / 1000.0, "render": render / 1000.0})
        if len(rows) == count:
            break
    return rows


def mean(rows, key):
    return sum(r[key] for r in rows) / len(rows)


def run_one(args, scene, cfg, last_frame, first):
    run_id = f"{int(time.time()) % 100000000:x}"
    sw, restart, attach = commands(args, scene, cfg, run_id, last_frame)
    tag = f"{scene}_{cfg}"
    err = ""
    for attempt in range(1, args.attempts + 1):
        deadline = time.monotonic() + args.restart_wait  # the console (or Azahar) may still be relaunching
        while True:
            r = subprocess.run(restart, capture_output=True, text=True)
            if r.returncode == 0 or time.monotonic() > deadline:
                break
            time.sleep(10)
        if r.returncode != 0:
            err = f"restart failed: {(r.stdout + r.stderr).strip().splitlines()[-1:] or ['?']}"
            time.sleep(10)
            continue
        for _ in range(4):  # the first connection may reach the old process, whose log closes as it relaunches
            HW_LAST.unlink(missing_ok=True)  # run_hw.py rewrites it; never read a stale one
            r = subprocess.run(attach, capture_output=True, text=True)
            text = HW_LAST.read_text(errors="replace") if HW_LAST.exists() else ""
            if f"hwb={run_id}" in text and re.search(r'switches "[^"]*hwb=' + run_id, text) or "[CRASH]" in text:
                break
            time.sleep(5)
        (args.out / f"{tag}.log").write_text(text)
        if "[CRASH]" in text:
            return None, "crash (see the log)"
        rows = parse(text, run_id, first, args.windows)
        if len(rows) >= args.windows:
            return rows, ""
        err = f"only {len(rows)} windows ({(r.stdout + r.stderr).strip().splitlines()[-1:] or ['?']})"
    return None, err


def table(results, configs, scenes):
    out = ["| scene | config | frame ms | core-0 busy ms | render ms | frame min-max | windows |", "|---|---|---|---|---|---|---|"]
    for scene in scenes:
        for cfg in configs:
            res = results.get(f"{scene}/{cfg}")
            if not res or not res.get("rows"):
                out.append(f"| {scene} | {cfg} | - | - | - | - | {res['note'] if res else 'not run'} |")
                continue
            rows = res["rows"]
            fr = [r["frame"] for r in rows]
            out.append(f"| {scene} | {cfg} | {mean(rows, 'frame'):.1f} | {mean(rows, 'busy'):.1f} | "
                       f"{mean(rows, 'render'):.1f} | {min(fr):.1f}-{max(fr):.1f} | {len(rows)} |")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("N3DS_IP"), help="console (or 127.0.0.1) address; default $N3DS_IP")
    ap.add_argument("--port", type=int, default=None, help="devupdate port (default 5050; an azahar-session gives its own)")
    ap.add_argument("--scenes", default=TEN, help=f"comma list from: {', '.join(SCENES)} (default: the ten)")
    ap.add_argument("--configs", default="D", help=f"comma list from: {', '.join(CONFIGS)} (default D; 'list' prints "
                    "the switches of every config)")
    ap.add_argument("--settle", type=int, default=DEFAULT_SETTLE, help="first game frame a counted window may end at")
    ap.add_argument("--windows", type=int, default=3, help="windows counted per run")
    ap.add_argument("--timeout", type=float, default=300, help="seconds per run for the windows (after the connection)")
    ap.add_argument("--connect-timeout", type=float, default=120, help="seconds to retry the live log connection")
    ap.add_argument("--restart-wait", type=float, default=120, help="seconds to retry a refused restart (the title is still relaunching)")
    ap.add_argument("--attempts", type=int, default=2, help="restarts per run when it fails")
    ap.add_argument("--out", type=Path, default=OUT, help="folder for the table, the JSON and the logs")
    ap.add_argument("--dry-run", action="store_true", help="print the commands; touch nothing")
    args = ap.parse_args()
    if args.configs == "list":
        for name, sw in CONFIGS.items():
            print(f"{name:9} {sw or '(defaults)'}")
        return 0
    by_lower = {k.lower(): k for k in CONFIGS}
    by_lower.update(ALIASES)
    scenes = args.scenes.split(",")
    configs = [by_lower.get(c.lower(), c) for c in args.configs.split(",")]
    bad = [s for s in scenes if s not in SCENES] + [c for c in configs if c not in CONFIGS]
    if bad:
        print(f"hw_baseline: unknown: {', '.join(bad)}", file=sys.stderr)
        return 2
    if not args.ip and not args.dry_run:
        print("hw_baseline: set N3DS_IP or pass --ip", file=sys.stderr)
        return 2
    args.ip = args.ip or "$N3DS_IP"
    first = aligned_settle(args.settle, WINDOW)
    last_frame = first + (args.windows - 1) * WINDOW
    if args.dry_run:
        print(f"# {len(scenes) * len(configs)} runs; windows ending at frames {first}..{last_frame}; "
              f"each run: restart, then attach to the live log")
        for scene in scenes:
            for cfg in configs:
                sw, restart, attach = commands(args, scene, cfg, "<id>", last_frame)
                print(f"\n# {scene} / {cfg}")
                print(shlex.join(restart).replace("'$N3DS_IP'", '"$N3DS_IP"'))
                print(shlex.join(attach).replace("'$N3DS_IP'", '"$N3DS_IP"'))
        return 0
    args.out.mkdir(parents=True, exist_ok=True)
    results = {}
    for scene in scenes:
        for cfg in configs:
            print(f"hw_baseline: {scene} / {cfg} ...", flush=True)
            rows, note = run_one(args, scene, cfg, last_frame, first)
            results[f"{scene}/{cfg}"] = {"rows": rows, "note": note}
            if rows:
                print(f"hw_baseline:   frame {mean(rows, 'frame'):.1f} ms, busy {mean(rows, 'busy'):.1f}, "
                      f"render {mean(rows, 'render'):.1f} ({len(rows)} windows)", flush=True)
            else:
                print(f"hw_baseline:   FAILED: {note}", flush=True)
            (args.out / "results.json").write_text(json.dumps(results, indent=1))
            (args.out / "baseline.md").write_text(table(results, configs, scenes) + "\n")
    print()
    print(table(results, configs, scenes))
    return 0 if all(r["rows"] for r in results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
