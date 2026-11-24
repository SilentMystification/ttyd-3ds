#!/usr/bin/env python3
"""Pull, show and compare qtmbench results (libs/ctr-devkit/qtmbench).

    tools/port3ds/qtm_bench.py pull [--out build/qtmbench/<name>]   # devupdate files get, then show
    tools/port3ds/qtm_bench.py run [--switches "..."] [--out ...]   # push the CIA, wait for DONE, pull
    tools/port3ds/qtm_bench.py show results.json [--suite cpu_bw]
    tools/port3ds/qtm_bench.py diff old.json new.json [--min-change 5]

The console must be on the network with qtmbench running (it idles with
devupdate after its run); the address is $N3DS_IP or --ip (Azahar:
--ip 127.0.0.1). Results on the console: sdmc:/3ds/qtmbench/results.json,
results.txt and log.txt, read through devupdate's read-only folder
("@/...").
"""

import argparse
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEVUPDATE = REPO / "libs" / "ctr-devkit" / "devupdate" / "devupdate.py"
CIA = REPO / "build" / "3ds" / "qtmbench" / "qtmbench.cia"
MEM_ORDER = ["heap", "linear", "qtm", "vram"]


def du(args, ip, check=True, capture=False):
    cmd = [sys.executable, "-I", str(DEVUPDATE)]
    if ip:
        cmd += ["--ip", ip]
    cmd += args
    return subprocess.run(cmd, check=check, text=True, capture_output=capture)


def pull(ip, out):
    out.mkdir(parents=True, exist_ok=True)
    for name in ("results.json", "results.txt", "log.txt"):
        du(["files", "get", "@/" + name, "-o", str(out / name)], ip)
    print(f"pulled into {out}")
    return out / "results.json"


def load(path):
    with open(path) as f:
        return json.load(f)


def fmt_size(s):
    if not s:
        return "-"
    if s >= 1 << 20 and s % (1 << 20) == 0:
        return f"{s >> 20}M"
    if s >= 1024 and s % 1024 == 0:
        return f"{s >> 10}K"
    return str(s)


def fmt_val(v):
    if v is None or (isinstance(v, float) and math.isnan(v)):
        return "-"
    a = abs(v)
    if a >= 1000:
        return f"{v:.0f}"
    if a >= 10:
        return f"{v:.1f}"
    return f"{v:.3g}"


def mem_key(m):
    return (MEM_ORDER.index(m) if m in MEM_ORDER else len(MEM_ORDER), m)


def table(rows, cols, cell, row_label="", width=11):
    out = [f"{row_label:<24}" + "".join(f" {c:>{width}}" for c in cols)]
    for r in rows:
        out.append(f"{str(r):<24}" + "".join(f" {cell(r, c):>{width}}" for c in cols))
    return "\n".join(out)


def by(recs, **kw):
    return [r for r in recs if all(r.get(k) == v for k, v in kw.items())]


def show(data, only=None):
    info = data.get("info", {})
    for k, v in info.items():
        print(f"{k:<20} {v}")
    recs = data["records"]
    suites = []
    for r in recs:
        if r["suite"] not in suites:
            suites.append(r["suite"])
    for suite in suites:
        if only and suite not in only:
            continue
        rs = by(recs, suite=suite)
        print(f"\n== {suite} ==")
        if suite in ("cpu_bw", "cpu_lat", "cpu_rand", "cache"):
            tests = sorted({r["test"] for r in rs}, key=lambda t: [r["test"] for r in rs].index(t))
            for test in tests:
                for core in sorted({r["core"] for r in rs if r["test"] == test}):
                    sub = by(rs, test=test, core=core)
                    mems = sorted({r["mem"] for r in sub}, key=mem_key)
                    sizes = sorted({r["size"] for r in sub})
                    idx = {(r["size"], r["mem"]): r["value"] for r in sub}
                    unit = sub[0]["unit"]
                    print()
                    print(table([fmt_size(s) for s in sizes], mems,
                                lambda s, m, idx=idx, sizes=sizes: fmt_val(idx.get((sizes[[fmt_size(x) for x in sizes].index(s)], m))),
                                f"{test} c{core} [{unit}]"))
        elif suite in ("gpu_read", "gpu_write", "coherency", "contend", "gpu_check"):
            tests = []
            for r in rs:
                if r["test"] not in tests:
                    tests.append(r["test"])
            mems = []
            for r in rs:
                if r["mem"] not in mems:
                    mems.append(r["mem"])
            if suite != "contend":
                mems.sort(key=mem_key)
            idx = {(r["test"], r["mem"]): r for r in rs}

            def cell(t, m, idx=idx, suite=suite):
                r = idx.get((t, m))
                if r is None:
                    return ""
                if suite == "coherency":
                    return f"{fmt_val(r['value'])}{'@' + fmt_size(r['size'])}"
                return fmt_val(r["value"])

            if suite == "coherency":
                tests = sorted({(r["test"]) for r in rs})
                rows = []
                for t in tests:
                    for s in sorted({r["size"] for r in by(rs, test=t)}):
                        rows.append((t, s))
                idx2 = {(r["test"], r["size"], r["mem"]): r["value"] for r in rs}
                print(table([f"{t}@{fmt_size(s)}" for t, s in rows], mems,
                            lambda lab, m: fmt_val(idx2.get((lab.split("@")[0], next(s for t, s in rows if f"{t}@{fmt_size(s)}" == lab), m))),
                            "% stale words", width=9))
            else:
                unit = rs[0]["unit"]
                print(table(tests, mems, cell, f"[{unit}]", width=12 if suite == "contend" else 11))
        else:
            for r in rs:
                core = f"c{r['core']}" if r["core"] >= 0 else "-"
                aux = "" if r.get("aux") is None else f" ({fmt_val(r['aux'])})"
                print(f"{r['test']:<24} {r['mem']:<20} {core:<3} {fmt_size(r['size']):>5} "
                      f"{fmt_val(r['value']):>10} {r['unit']:<6}{aux} {r.get('note', '')}")


def diff(a, b, min_change):
    ia = {r["key"]: r for r in a["records"]}
    ib = {r["key"]: r for r in b["records"]}
    print(f"{'key':<52} {'old':>10} {'new':>10} {'change':>8}")
    shown = 0
    for k, rb in ib.items():
        ra = ia.get(k)
        va = None if ra is None else ra["value"]
        vb = rb["value"]
        if va is None and vb is None:
            continue
        if va is None or vb is None or va == 0:
            print(f"{k:<52} {fmt_val(va):>10} {fmt_val(vb):>10} {'new' if ra is None else '':>8}")
            shown += 1
            continue
        ch = (vb - va) / abs(va) * 100.0
        if abs(ch) >= min_change:
            print(f"{k:<52} {fmt_val(va):>10} {fmt_val(vb):>10} {ch:>+7.1f}%")
            shown += 1
    for k in ia:
        if k not in ib:
            print(f"{k:<52} {fmt_val(ia[k]['value']):>10} {'-':>10} {'gone':>8}")
            shown += 1
    print(f"\n{shown} rows differ by >= {min_change}% (units: MB/s higher is better; ns/us/x lower is better)")


def wait_done(ip, timeout):
    """Polls the console's log for QTMBENCH DONE."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(10)
        tmp = REPO / "build" / "qtmbench" / ".log_poll.txt"
        tmp.parent.mkdir(parents=True, exist_ok=True)
        r = du(["files", "get", "@/log.txt", "-o", str(tmp)], ip, check=False, capture=True)
        if r.returncode == 0 and "QTMBENCH DONE" in tmp.read_text(errors="replace"):
            return True
        print(f"  waiting ({time.time() - t0:.0f} s)")
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("N3DS_IP"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pull", help="fetch results.json/.txt and log.txt from the console, then show")
    p.add_argument("--out", type=Path, default=None)
    p.add_argument("--quiet", action="store_true")
    p = sub.add_parser("run", help="push the CIA with switches, wait for QTMBENCH DONE, pull, show")
    p.add_argument("--cia", type=Path, default=CIA)
    p.add_argument("--switches", default="")
    p.add_argument("--timeout", type=float, default=600)
    p.add_argument("--out", type=Path, default=None)
    p = sub.add_parser("show", help="print the tables of a results.json")
    p.add_argument("json", type=Path)
    p.add_argument("--suite", action="append")
    p = sub.add_parser("diff", help="compare two results.json files")
    p.add_argument("old", type=Path)
    p.add_argument("new", type=Path)
    p.add_argument("--min-change", type=float, default=5.0, help="percent; smaller changes are hidden")
    args = ap.parse_args()

    stamp = time.strftime("%Y%m%d-%H%M%S")
    if args.cmd == "pull":
        out = args.out or REPO / "build" / "qtmbench" / stamp
        path = pull(args.ip, out)
        if not args.quiet:
            show(load(path))
    elif args.cmd == "run":
        du(["push", str(args.cia), "--switches", args.switches, "--wait", "90"], args.ip)
        if not wait_done(args.ip, args.timeout):
            sys.exit("qtm_bench: no QTMBENCH DONE in the log; pull by hand later")
        out = args.out or REPO / "build" / "qtmbench" / stamp
        show(load(pull(args.ip, out)))
    elif args.cmd == "show":
        show(load(args.json), args.suite)
    elif args.cmd == "diff":
        diff(load(args.old), load(args.new), args.min_change)


if __name__ == "__main__":
    main()
