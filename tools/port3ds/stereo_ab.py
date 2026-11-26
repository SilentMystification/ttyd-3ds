#!/usr/bin/env python3
"""Byte identity of stereo `determ` shots between two switch sets of one build, in Azahar.

    tools/port3ds/stereo_ab.py --a "" --b "gxdirect=1" --scenes gor_01 --first 780 --count 2

Like determ_ab.py, with `stereoforce=1` (simulated 3D slider) added. Every shot is saved as the EFB
(N.ppm) plus the left and right eye targets (N.L.ppm, N.R.ppm); all three are compared byte for byte
between A and B. Shots stay in build/stereo-ab/<scene>/<a|b>/.
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gx_perf_ab import SCENES  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "stereo-ab"


def run(scene, config, args, tag):
    dest = OUT / scene / tag
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    profile = ROOT / "build" / "azahar-profile"
    shots = profile / "data" / "azahar-emu" / "sdmc" / "3ds" / "ttyd" / "shots"
    if shots.exists():
        for f in shots.iterdir():
            f.unlink()
    last = args.first + args.count + 3
    sw = (f"{SCENES[scene]} determ stereoforce=1 shots=1 shotfrom={args.first} shotmax={args.count + 4} "
          f"{config}").strip()
    cmd = [sys.executable, str(ROOT / "tools/port3ds/run_azahar.py"), "--build", str(args.build), "--log",
           "3ds/ttyd/latest.log", "--switches", sw, "--until", f"shot shots/{last:05d}.ppm: this record",
           "--long", str(args.long), "--stall", "120", "--last", str(dest / "azahar")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        tail = (r.stdout + r.stderr).strip().splitlines()[-1:] or ["?"]
        print(f"{scene} {tag}: run_azahar rc {r.returncode}: {tail[0]}", flush=True)
    if shots.exists():
        for f in sorted(shots.iterdir()):
            if f.suffix == ".ppm" and args.first <= int(f.stem.split(".")[0]) < args.first + args.count:
                shutil.copy2(f, dest / f.name)
    return dest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=ROOT / "build/3ds/game/ttyd.cia")
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--scenes", default="gor_01")
    ap.add_argument("--first", type=int, default=780)
    ap.add_argument("--count", type=int, default=2)
    ap.add_argument("--long", type=float, default=200)
    args = ap.parse_args()
    print(f"A: '{args.a}'\nB: '{args.b}'\nstereoforce=1, shots {args.first}..{args.first + args.count - 1}")
    bad = 0
    for scene in args.scenes.split(","):
        da, db = run(scene, args.a, args, "a"), run(scene, args.b, args, "b")
        names = sorted({p.name for p in da.glob("*.ppm")} | {p.name for p in db.glob("*.ppm")})
        same = 0
        for name in names:
            pa, pb = da / name, db / name
            if not pa.exists() or not pb.exists():
                print(f"{scene} {name}: only in {'A' if pa.exists() else 'B'}")
                continue
            eq = pa.read_bytes() == pb.read_bytes()
            same += eq
            print(f"{scene} {name}: {'identical' if eq else 'DIFFERENT'} ({pa.stat().st_size} bytes)")
        # left and right must differ from each other, or the shot is not stereo
        for f in sorted(da.glob("*.L.ppm")):
            r = f.with_name(f.name.replace(".L.", ".R."))
            if r.exists():
                print(f"{scene} {f.stem.split('.')[0]}: left differs from right in A: {f.read_bytes() != r.read_bytes()}")
        print(f"{scene}: {same} of {len(names)} identical", flush=True)
        if same != len(names) or len(names) < 3 * args.count:
            bad = 1
    return bad


if __name__ == "__main__":
    sys.exit(main())
