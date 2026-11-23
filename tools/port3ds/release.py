#!/usr/bin/env python3
"""Build the release artifacts: US, JP and PAL, into build/release/<version>/.

    tools/port3ds/release.py [--allow-dirty] [--skip-build] [--regions US,JP,PAL]

Writes ttyd-3ds-<version>-<region>.cia and .3dsx, SHA256SUMS and README.md
(docs/USER_GUIDE.md). The version is port3ds/VERSION, with "-dev+<shorthash>"
appended unless the tree is clean and HEAD is tagged v<version>. A dirty tree
is refused without --allow-dirty. The checklist is docs/RELEASE.md.

The artifacts hold code and generated placeholder art only; the game's data is
read from the player's own disc at run time (docs/PORT_3DS_PLAN.md 6.1). The
packager checks that no artifact is a disc image or unreasonably large.
"""

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
REGIONS = {"US": "game", "JP": "game_jp", "PAL": "game_pal"}


def git(*args):
    r = subprocess.run(["git", *args], cwd=REPO, capture_output=True, text=True)
    return r.returncode, r.stdout.strip()


def version(allow_dirty):
    core = (REPO / "port3ds" / "VERSION").read_text().strip()
    _, dirty = git("status", "--porcelain", "--untracked-files=no")
    if dirty and not allow_dirty:
        sys.exit("release.py: the working tree is dirty (commit, or use --allow-dirty):\n" + dirty)
    tagged, _ = git("describe", "--exact-match", "--tags", "--match", f"v{core}", "HEAD")
    if tagged == 0 and not dirty:
        return core
    return f"{core}-dev+{git('rev-parse', '--short', 'HEAD')[1]}"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--allow-dirty", action="store_true", help="build from a dirty tree")
    ap.add_argument("--skip-build", action="store_true", help="package the existing build/3ds outputs")
    ap.add_argument("--regions", default="US,JP,PAL")
    args = ap.parse_args()

    ver = version(args.allow_dirty)
    regions = [r.strip().upper() for r in args.regions.split(",")]
    for r in regions:
        if r not in REGIONS:
            sys.exit(f"release.py: unknown region {r}")
    out = REPO / "build" / "release" / ver
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    print(f"release.py: version {ver} -> {out}")

    for r in regions:
        if not args.skip_build:
            print(f"release.py: building {r}", flush=True)
            subprocess.run([str(REPO / "tools/port3ds/build.sh"), ".", f"-DREGION={r}"], cwd=REPO, check=True)
        bdir = REPO / "build" / "3ds" / REGIONS[r]
        for ext in ("cia", "3dsx"):
            src = bdir / f"ttyd.{ext}"
            if not src.is_file():
                sys.exit(f"release.py: {src} was not built")
            dst = out / f"ttyd-3ds-{ver}-{r.lower()}.{ext}"
            shutil.copyfile(src, dst)
            data = dst.read_bytes()
            if data[0x1C:0x20] == b"\xc2\x33\x9f\x3d":  # the GameCube disc magic
                sys.exit(f"release.py: {dst.name} looks like a disc image")
            if len(data) > 64 << 20:
                sys.exit(f"release.py: {dst.name} is {len(data)} bytes: too big for code and placeholder art")

    shutil.copyfile(REPO / "docs" / "USER_GUIDE.md", out / "README.md")
    files = sorted(p for p in out.iterdir() if p.name != "SHA256SUMS")
    (out / "SHA256SUMS").write_text("".join(f"{sha256(p)}  {p.name}\n" for p in files))
    for p in sorted(out.iterdir()):
        print(f"  {p.stat().st_size:>10}  {p.name}")
    print((out / "SHA256SUMS").read_text(), end="")


if __name__ == "__main__":
    main()
