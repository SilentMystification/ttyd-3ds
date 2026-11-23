#!/usr/bin/env python3
"""Machine-wide emulator usage log: written by run_azahar.py and run_dolphin.py, summarised here.

    tools/port3ds/emu_usage.py [--log FILE] [--top N]

The log is ${XDG_CACHE_HOME:-~/.cache}/ttyd3ds/emu_runs.log, one tab-separated line per run:
    start time, emulator, worktree path, seconds waited for a slot, seconds run, result, switches (120 chars)
The summary lists, per worktree and emulator: runs, total and longest run time, total slot wait; then the
longest runs. See docs/PORT_3DS_TEST_INFRA.md, "Run length".
"""

import argparse
import os
import sys
import time
from collections import defaultdict
from pathlib import Path


def log_path():
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "ttyd3ds" / "emu_runs.log"


DEFAULT_TIMEOUT = 60.0
MAX_LONG = 1800.0


def resolve_timeout(tool, timeout, long_s):
    """The run's time limit in seconds: 60 by default; a longer run must say --long SECONDS (max 1800).
    Returns (seconds, error message or None)."""
    if long_s is not None:
        if not 0 < long_s <= MAX_LONG:
            return DEFAULT_TIMEOUT, f"--long takes 1 to {MAX_LONG:.0f} seconds"
        print(f"{tool}: long run ({long_s:.0f} s) requested", flush=True)
        return long_s, None
    if timeout is not None and timeout > DEFAULT_TIMEOUT:
        return DEFAULT_TIMEOUT, (f"--timeout {timeout:.0f} is over the {DEFAULT_TIMEOUT:.0f} s default: runs are "
                                 f"short and targeted; a run that needs more must pass --long SECONDS (max {MAX_LONG:.0f})")
    return (DEFAULT_TIMEOUT if timeout is None else timeout), None


def time_args(seconds):
    """run_azahar/run_dolphin arguments for a run limit: --timeout up to the 60 s default, else --long (max 1800)."""
    seconds = float(seconds)
    if seconds <= DEFAULT_TIMEOUT:
        return ["--timeout", f"{seconds:g}"]
    return ["--long", f"{min(seconds, MAX_LONG):g}"]


def record(emulator, worktree, waited, ran, result, switches, started):
    """Append one line; never raises (a log problem must not fail a test run)."""
    try:
        p = log_path()
        p.parent.mkdir(parents=True, exist_ok=True)
        clean = lambda s: " ".join(str(s).split())
        line = "\t".join([time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(started)), emulator, str(worktree),
                          f"{waited:.1f}", f"{ran:.1f}", clean(result)[:80], clean(switches)[:120]])
        with open(p, "a") as f:  # one short write: O_APPEND keeps concurrent lines whole
            f.write(line + "\n")
    except OSError:
        pass


def load(path):
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text(errors="replace").splitlines():
        f = line.split("\t")
        if len(f) < 6:
            continue
        try:
            rows.append({"start": f[0], "emu": f[1], "tree": f[2], "wait": float(f[3]), "ran": float(f[4]),
                         "result": f[5], "switches": f[6] if len(f) > 6 else ""})
        except ValueError:
            pass
    return rows


def main():
    ap = argparse.ArgumentParser(description="Summarise the emulator usage log.")
    ap.add_argument("--log", type=Path, default=log_path())
    ap.add_argument("--top", type=int, default=10)
    args = ap.parse_args()
    rows = load(args.log)
    if not rows:
        print(f"emu_usage: no runs in {args.log}")
        return 0
    per = defaultdict(list)
    for r in rows:
        per[(r["tree"], r["emu"])].append(r)
    print(f"emu_usage: {len(rows)} runs in {args.log}")
    print(f"{'runs':>5} {'total s':>9} {'max s':>8} {'wait s':>9} {'timeouts':>12} {'stalls':>12}  emulator  worktree")
    for (tree, emu), rs in sorted(per.items(), key=lambda kv: -sum(r["ran"] for r in kv[1])):
        # A timeout or stall is a run that ran to its limit: the time it wasted is what to watch.
        to = [r for r in rs if r["result"].startswith("FAIL: timeout")]
        st = [r for r in rs if r["result"].startswith("FAIL: stall")]
        print(f"{len(rs):5d} {sum(r['ran'] for r in rs):9.0f} {max(r['ran'] for r in rs):8.0f} "
              f"{sum(r['wait'] for r in rs):9.0f} {len(to):4d} {sum(r['ran'] for r in to):7.0f}s "
              f"{len(st):4d} {sum(r['ran'] for r in st):7.0f}s  {emu:<8}  {tree}")
    print(f"\nlongest {args.top} runs:")
    for r in sorted(rows, key=lambda r: -r["ran"])[:args.top]:
        print(f"{r['ran']:7.0f}s  {r['start']}  {r['emu']:<8} {Path(r['tree']).name}  {r['result'][:30]}  {r['switches'][:60]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
