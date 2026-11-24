#!/usr/bin/env python3
"""Per-frame GlobalWork comparison: the first frame where two runs differ.

The game writes one "gwdump f=<frame> wf=<frames since warp> seq=... map=...
..." line every N frames with the switch gwdump=N (port3ds/game/test_mode.c,
compiled into the 3DS port and the GameCube debug DOL). This tool collects
two such streams and reports the first frame whose fields differ, and which.

Compare two logs that already exist:

    tools/port3ds/gw_diff.py --logs build/az-a/game_log.txt build/az-b/game_log.txt

Run the same switches and padscript in the port (Azahar) and in Dolphin (the
debug DOL, configure.py --gcdebug), then compare:

    tools/port3ds/gw_diff.py --switches "map=gor_01" --padscript "wait*700,cr*60" \\
        --frames 1200 --every 10

Both runs get "determ gwdump=<every>". Records are matched on the frame
number, or with --align warp on the frames since the first map change (the
boot sequences of the two builds need not take the same number of frames).
Fields compared: all of them between two runs of one build; between the port
and Dolphin, all but "gw" (the raw GlobalWork hash, which holds pointers and
byte order). --ignore drops more fields.

Exit status: 0 no difference, 1 a difference (or a run failed), 2 usage error.
"""

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_usage  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
LINE = re.compile(r"gwdump (f=\d+ .*)$")
CROSS_SKIP = {"gw"}


def parse(text):
    """{frame: {field: value}} from gwdump lines (in a log or a buffer)."""
    out = {}
    for line in text.splitlines():
        m = LINE.search(line)
        if not m:
            continue
        rec = {}
        for word in m.group(1).split():
            k, _, v = word.partition("=")
            rec[k] = v
        # a record is two lines with the same f
        out.setdefault(int(rec["f"]), {}).update(rec)
    return out


def index(recs, align):
    if align == "frame":
        return recs
    out = {}
    for rec in recs.values():
        wf = int(rec.get("wf", "0"))
        if wf > 0:
            out[wf] = rec
    return out


def compare(a, b, align, skip, label_a, label_b):
    ia, ib = index(a, align), index(b, align)
    common = sorted(set(ia) & set(ib))
    key = "f" if align == "frame" else "wf"
    if not common:
        print(f"gw_diff: no common {key} values ({label_a}: {len(ia)} records, {label_b}: {len(ib)})")
        return 1, None
    last_same = None
    for k in common:
        ra, rb = ia[k], ib[k]
        fields = [f for f in sorted(set(ra) | set(rb)) if f not in skip and f not in ("f", "wf")]
        diff = [f for f in fields if ra.get(f) != rb.get(f)]
        if diff:
            print(f"gw_diff: first difference at {key}={k} ({label_a} f={ra['f']}, {label_b} f={rb['f']})"
                  + (f", last equal {key}={last_same}" if last_same is not None else ", no equal record before"))
            for f in diff:
                print(f"  {f}: {label_a} {ra.get(f)}  {label_b} {rb.get(f)}")
            return 1, {"key": key, "at": k, "fields": {f: [ra.get(f), rb.get(f)] for f in diff},
                       "last_equal": last_same}
        last_same = k
    print(f"gw_diff: {len(common)} records equal ({key} {common[0]}..{common[-1]}, "
          f"fields compared: {len([f for f in ia[common[0]] if f not in skip]) - 2})")
    return 0, None


def prepare_card(spec):
    """Gives both runs the same memory card: the port's sdmc card folder and Dolphin's GCI-folder card for the
    gcdebug profile are emptied, then get the one GCI file when spec is a path. The file-select screen draws the
    save data, so a different card changes the text, and with it the RNG calls."""
    port_dir = REPO / "build" / "azahar-profile" / "data" / "azahar-emu" / "sdmc" / "3ds" / "ttyd" / "card"
    dol_root = REPO / "build" / "dolphin-profile-gcdebug" / "GC"
    dol_dir = next(iter(sorted(dol_root.glob("*/Card A"))), dol_root / "USA" / "Card A")
    src = None if spec == "empty" else Path(spec)
    if src is not None and not src.is_file():
        print(f"gw_diff: --card {spec}: no such file")
        return False
    for d in (port_dir, dol_dir):
        d.mkdir(parents=True, exist_ok=True)
        for f in d.iterdir():
            if f.is_file():
                f.unlink()
        if src is not None:
            shutil.copyfile(src, d / src.name)
    state = "empty" if src is None else f"{src.name} sha1 {hashlib.sha1(src.read_bytes()).hexdigest()[:12]}"
    print(f"gw_diff: card state: {state} (port {port_dir.relative_to(REPO)}, dolphin {dol_dir.relative_to(REPO)})")
    return True


def run_port(args, switches):
    log = REPO / "build" / "gw-diff" / "port"
    cmd = [sys.executable, str(REPO / "tools/port3ds/run_azahar.py"), "--build", str(args.cia),
           "--switches", switches, "--until", f"gwdump f={args.frames} ", *emu_usage.time_args(args.timeout),
           "--stall", "60", "--allow-hw-faults", "--last", str(log)]
    print("gw_diff: port:", " ".join(cmd[2:]))
    r = subprocess.run(cmd, cwd=REPO)
    path = log / "game_log.txt"
    if r.returncode != 0 or not path.exists():
        print("gw_diff: the port run failed")
        return None
    return path.read_text(errors="replace")


def run_dolphin(args, switches):
    out = REPO / "build" / "gw-diff" / "dolphin.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    words = switches.split()
    if "nofastboot" in words:
        words.remove("nofastboot")
    elif "fastboot" not in words:
        words.append("fastboot")  # as run_azahar.py adds it for the port
    cmd = [sys.executable, str(REPO / "tools/port3ds/run_dolphin.py"), "--gcdebug", "--switches",
           " ".join(words + [f"stopframe={args.frames}"]), "--break", "gcdbg_stop", "--read-log", "--json", str(out),
           *emu_usage.time_args(args.timeout), "--port", str(args.dolphin_port),
           "--profile", str(REPO / "build" / "dolphin-profile-gcdebug")]
    print("gw_diff: dolphin:", " ".join(cmd[2:]))
    r = subprocess.run(cmd, cwd=REPO)
    if r.returncode != 0 or not out.exists():
        print("gw_diff: the Dolphin run failed")
        return None
    data = json.loads(out.read_text())
    if data.get("log_lost"):
        print(f"gw_diff: WARNING: the debug DOL's log buffer was full, {data['log_lost']} lines lost "
              "(use a larger --every or gwdumpfrom=/gwdumpto=)")
    return data.get("log", "")


def main():
    ap = argparse.ArgumentParser(description="First divergent frame between two gwdump streams.")
    ap.add_argument("--logs", nargs=2, type=Path, metavar=("A", "B"), help="compare two existing logs")
    ap.add_argument("--switches", default="", help="switches for both runs (determ and gwdump are added)")
    ap.add_argument("--padscript", default="", help="padscript for both runs")
    ap.add_argument("--frames", type=int, default=1200, help="game frames to run")
    ap.add_argument("--every", type=int, default=10, help="gwdump interval in frames")
    ap.add_argument("--align", choices=["frame", "warp"], default="frame")
    ap.add_argument("--ignore", default="", help="comma-separated fields not to compare")
    ap.add_argument("--cross", action="store_true", help="with --logs: the logs come from different builds")
    ap.add_argument("--cia", type=Path, default=REPO / "build/3ds/game/ttyd.cia")
    ap.add_argument("--timeout", type=float, help="seconds per run (default 30 + frames/12: the port in Azahar, determ; "
                    "a run over 60 s is a --long run)")
    ap.add_argument("--dolphin-port", type=int, default=55571)
    ap.add_argument("--card", default="empty", metavar="empty|PATH.gci",
                    help="the memory card both runs start with: no save files (default) or exactly this GCI")
    ap.add_argument("--json", type=Path, help="write the result here")
    args = ap.parse_args()

    if args.timeout is None:
        args.timeout = 30 + args.frames / 12.0
    skip = {f for f in args.ignore.split(",") if f}
    if args.logs:
        a, b = (p.read_text(errors="replace") for p in args.logs)
        labels = ("A", "B")
        if args.cross:
            skip |= CROSS_SKIP
    else:
        sw = f"{args.switches} determ gwdump={args.every}"
        if args.padscript:
            sw += f" padscript={args.padscript}"
        if not prepare_card(args.card):
            return 2
        a = run_port(args, sw)
        b = run_dolphin(args, sw) if a is not None else None
        if a is None or b is None:
            return 1
        labels = ("port", "dolphin")
        skip |= CROSS_SKIP
    ra, rb = parse(a), parse(b)
    print(f"gw_diff: {labels[0]} {len(ra)} records, {labels[1]} {len(rb)} records")
    status, result = compare(ra, rb, args.align, skip, *labels)
    if args.json:
        args.json.write_text(json.dumps({"status": status, "difference": result}, indent=1))
    return status


if __name__ == "__main__":
    sys.exit(main())
