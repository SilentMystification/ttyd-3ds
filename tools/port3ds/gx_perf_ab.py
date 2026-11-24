#!/usr/bin/env python3
"""Same-build A/B of GX switches over a set of scenes, in Azahar.

    tools/port3ds/gx_perf_ab.py --a "" --b "gxbatch=1" --scenes gor_01,hei_01
    tools/port3ds/gx_perf_ab.py --a "gxthread=2 gxthreadsplit=0 gxpipeline=1" \\
        --b "gxthread=2 gxthreadsplit=0 gxpipeline=1 gxbatch=1"

Each run is one run_azahar.py (one Azahar at a time, through its slot lock) with
`<scene switches> <config> gxthreadstats=60`, stopped when the `gxthread N frame
<to + 1>:` line appears. The `gxthread` lines of the windows that end in
(from, to + 1] are averaged: frame, core-0 busy, fence wait, translator and
render ms (render minus its vblank wait). Azahar's times are ratios between two
runs of one build, not console times (docs/PORT_3DS_60FPS.md).

The warp is at game frame 595 and takes about 40 s of wall time to reach, so a
run needs more than run_azahar.py's 60 s default: --long is passed (default
100 s; slow scenes need more).
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SCENES = {
    "gor_01": "map=gor_01 story=30",
    "gor_00": "map=gor_00 story=30",
    "gon_02": "map=gon_02 story=30",
    "aaa_00": "map=aaa_00",
    "tik_01": "map=tik_01 story=30",
    "hei_01": "map=hei_01 story=30",
    "jin_00": "map=jin_00 story=30",
    "battle": "map=gon_02 story=30 battle=1 btlauto=perfect",
    "eki_00": "map=eki_00 story=30",
    "usu_00": "map=usu_00 story=30",
    "pik_00": "map=pik_00 story=30",
    "pause": "map=gor_01 story=30 padscript=wait*700,start*3",
}
TEN = "gor_01,gon_02,aaa_00,tik_01,hei_01,jin_00,battle,eki_00,usu_00,pik_00"

LINE = re.compile(r"gxthread (\d) frame (\d+): game us: frame (\d+) busy (\d+) fences (\d+) idle (\d+) \| "
                  r"translator (\d+) \(copydisp (\d+)\) render (\d+)")
RENDER = re.compile(r"gxthread   render \(last frame\): (\d+) us, (\d+) of them")
BATCH = re.compile(r"gxbatch frame (\d+): per frame (\d+) opens drawing (\d+) lists")


def run(scene, config, args, tag):
    last = ROOT / "build" / f"azahar-ab-{tag}"
    sw = f"{SCENES[scene]} {config} gxthreadstats=60".replace("  ", " ")
    cmd = [str(ROOT / "tools/port3ds/run_azahar.py"), "--build", str(args.build), "--log", "3ds/ttyd/latest.log",
           "--switches", sw, "--until", rf"gxthread \d frame {args.to + 1}:", "--long", str(args.long),
           "--last", str(last)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    log = last / "game_log.txt"
    text = log.read_text(errors="replace") if log.exists() else ""
    rows, render, batch = [], [], []
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        m = LINE.search(ln)
        if m and args.frm < int(m.group(2)) <= args.to + 1:
            rows.append([int(x) for x in m.groups()[2:]])
            for nxt in lines[i + 1:i + 3]:
                rm = RENDER.search(nxt)
                if rm:
                    render.append(int(rm.group(1)) - int(rm.group(2)))
        bm = BATCH.search(ln)
        if bm and args.frm < int(bm.group(1)) <= args.to + 1:
            batch.append((int(bm.group(2)), int(bm.group(3))))
    if r.returncode != 0 or not rows:
        tail = (r.stdout + r.stderr).strip().splitlines()[-1:] or ["?"]
        return None, f"run failed ({tail[0]})"
    n = len(rows)
    avg = [sum(c) / n / 1000.0 for c in zip(*rows)]
    out = {"frame": avg[0], "busy": avg[1], "fence": avg[2], "translator": avg[4],
           "render": (sum(render) / len(render) / 1000.0) if render else avg[6], "windows": n}
    note = ""
    if batch:
        note = f"batches {batch[-1][0]} drawing {batch[-1][1]} lists/frame"
    return out, note


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=ROOT / "build/3ds/game/ttyd.cia")
    ap.add_argument("--a", required=True, help="switches of configuration A (may be empty)")
    ap.add_argument("--b", default=None, help="switches of configuration B (omit to measure A alone)")
    ap.add_argument("--scenes", default=TEN, help=f"comma-separated, from: {', '.join(SCENES)}")
    ap.add_argument("--from", dest="frm", type=int, default=721, help="windows after this game frame count")
    ap.add_argument("--to", type=int, default=840, help="last game frame measured")
    ap.add_argument("--long", type=float, default=100, help="run_azahar.py limit per run in seconds")
    ap.add_argument("--repeat", type=int, default=1, help="runs per cell (the minimum busy time is kept)")
    args = ap.parse_args()
    configs = [("A", args.a)] + ([("B", args.b)] if args.b is not None else [])
    print(f"A: '{args.a}'" + (f"\nB: '{args.b}'" if args.b is not None else ""))
    print(f"windows ending in frames {args.frm + 1}..{args.to + 1}; ms per frame (Azahar)")
    print(f"{'scene':8} {'cfg':3} {'frame':>7} {'busy':>7} {'fence':>7} {'transl':>7} {'render':>7}  note")
    bad = 0
    for scene in args.scenes.split(","):
        if scene not in SCENES:
            print(f"{scene}: unknown scene")
            bad = 1
            continue
        for name, cfg in configs:
            best, note = None, ""
            for k in range(args.repeat):
                res, n = run(scene, cfg, args, f"{name.lower()}")
                if res is not None and (best is None or res["busy"] + res["translator"] < best["busy"] + best["translator"]):
                    best, note = res, n
                elif res is None:
                    note = n
            if best is None:
                print(f"{scene:8} {name:3} {note}")
                bad = 1
                continue
            print(f"{scene:8} {name:3} {best['frame']:7.1f} {best['busy']:7.1f} {best['fence']:7.1f} "
                  f"{best['translator']:7.1f} {best['render']:7.1f}  {note}", flush=True)
    return bad


if __name__ == "__main__":
    sys.exit(main())
