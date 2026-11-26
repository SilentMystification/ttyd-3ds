#!/usr/bin/env python3
"""Byte identity of `determ` shots between two switch sets of one build, in Azahar.

    tools/port3ds/determ_ab.py --a "gxbatch=1 gxstatecache=1" --b "gxbatch=1 gxstatecache=1 gxdirty=1" \\
        --scenes gor_01,battle --first 780 --count 6

Each (scene, config) is one run_azahar.py with `<scene switches> determ shots=1 shotfrom=F shotmax=N <config>`,
stopped at the last shot. The shots (EFB, 608x480 PPM) of A and B are compared byte for byte, frame by frame.
Each shot also gets a content line: the share of pixels that are not the most common colour and the number of
distinct colours, so a blank or single-colour shot is visible (identical blank shots prove nothing).
Shots stay in build/determ-ab/<scene>/<a|b>/ (PNG copies with --png).
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gx_perf_ab import SCENES  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "determ-ab"


def read_ppm(p):
    data = p.read_bytes()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():m.end() + w * h * 3], dtype=np.uint8).reshape(h, w, 3)


def content(img):
    flat = img.reshape(-1, 3).astype(np.uint32)
    keys = flat[:, 0] << 16 | flat[:, 1] << 8 | flat[:, 2]
    vals, counts = np.unique(keys, return_counts=True)
    return 1.0 - counts.max() / keys.size, len(vals)


def run(scene, config, args, tag, build):
    dest = OUT / scene / tag
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    profile = ROOT / "build" / "azahar-profile"
    shots = profile / "data" / "azahar-emu" / "sdmc" / "3ds" / "ttyd" / "shots"
    if shots.exists():
        for f in shots.iterdir():
            f.unlink()
    # the SD writes of the last shots finish after their log lines: shoot 4 more and wait for the last of them
    last = args.first + args.count + 3
    sw = f"{SCENES[scene]} determ shots=1 shotfrom={args.first} shotmax={args.count + 4} {config}".strip()
    cmd = [sys.executable, str(ROOT / "tools/port3ds/run_azahar.py"), "--build", str(build), "--log",
           "3ds/ttyd/latest.log", "--switches", sw, "--until", f"shot shots/{last:05d}.ppm: this record",
           "--long", str(args.long), "--stall", str(args.stall), "--last", str(dest / "azahar")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    n = 0
    if shots.exists():
        for f in sorted(shots.iterdir()):
            # NNNNN.ppm (the EFB) and, with stereo on, NNNNN.L.ppm / NNNNN.R.ppm (the eyes' screens)
            num = f.name.split(".")[0]
            if f.suffix == ".ppm" and num.isdigit() and args.first <= int(num) < args.first + args.count:
                shutil.copy2(f, dest / f.name)
                n += f.name.count(".") == 1
    if r.returncode != 0:
        tail = (r.stdout + r.stderr).strip().splitlines()[-1:] or ["?"]
        print(f"{scene} {tag}: run_azahar rc {r.returncode}: {tail[0]}", flush=True)
    return dest, n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=ROOT / "build/3ds/game/ttyd.cia")
    ap.add_argument("--build-b", type=Path, default=None,
                    help="B's build (default --build): compare two builds, e.g. a TTYD_RETAIL build against the base")
    ap.add_argument("--stall", type=float, default=120,
                    help="run_azahar.py --stall (0 for retail mode: no log lines before the shots)")
    ap.add_argument("--a", required=True, help="switches of configuration A (may be empty)")
    ap.add_argument("--b", required=True, help="switches of configuration B")
    ap.add_argument("--scenes", default="gor_01,battle")
    ap.add_argument("--first", type=int, default=780, help="first presented frame shot")
    ap.add_argument("--count", type=int, default=6, help="consecutive shots")
    ap.add_argument("--long", type=float, default=200, help="run_azahar.py limit per run in seconds")
    ap.add_argument("--png", action="store_true", help="also write PNG copies of A's shots (needs Pillow)")
    args = ap.parse_args()
    print(f"A: '{args.a}'\nB: '{args.b}'\nshots {args.first}..{args.first + args.count - 1}")
    bad = 0
    for scene in args.scenes.split(","):
        da, na = run(scene, args.a, args, "a", args.build)
        db, nb = run(scene, args.b, args, "b", args.build_b or args.build)
        same = 0
        frames = sorted({p.name for p in da.glob("*.ppm")} | {p.name for p in db.glob("*.ppm")})
        for name in frames:
            pa, pb = da / name, db / name
            if not pa.exists() or not pb.exists():
                print(f"{scene} {name}: only in {'A' if pa.exists() else 'B'}")
                continue
            eq = pa.read_bytes() == pb.read_bytes()
            same += eq
            img = read_ppm(pa)
            share, ncol = content(img)
            print(f"{scene} {name}: {'identical' if eq else 'DIFFERENT'}; A content: {share * 100:.1f}% of pixels "
                  f"off the commonest colour, {ncol} colours")
            if args.png:
                try:
                    from PIL import Image
                    Image.fromarray(img).save(pa.with_suffix(".png"))
                except ImportError:
                    pass
        print(f"{scene}: {same} of {len(frames)} identical (A {na} shots, B {nb})", flush=True)
        if same != len(frames) or len(frames) < args.count:
            bad = 1
    return bad


if __name__ == "__main__":
    sys.exit(main())
