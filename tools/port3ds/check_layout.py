#!/usr/bin/env python3
"""Compare data object sizes between the GameCube build and the 3DS objects.

    tools/port3ds/check_layout.py [--version G8ME01] [--build build/3ds/dev]

Pointers are 4 bytes on both machines and the port builds game code with
int-sized enums (MWCC -enum int), so every global or static data object
should have the same size in the 3DS objects as in the matching GameCube ELF.
A different size means a struct is laid out differently (an enum width, a
packing difference, a wrong typedef), which breaks data read from the disc
and save files. Lists every mismatch; exit status 1 if there are any.

Objects that exist only on one side (compiler-generated literals, port
additions) are ignored. Main-executable objects only: REL symbols are local
in the 3DS REL objects and are compared from the per-REL ELF symbol tables
of the GameCube RELs in a later step.
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfsyms import load_symbols  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"


def arm_object_sizes(objects):
    sizes = {}
    for chunk in range(0, len(objects), 200):
        r = subprocess.run([NM, "-S", "--defined-only", *map(str, objects[chunk:chunk + 200])],
                           capture_output=True, text=True, check=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[2] in "DdBbRrCc":
                name, size = parts[3], int(parts[1], 16)
                sizes.setdefault(name, set()).add(size)
    return sizes


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", default="G8ME01")
    ap.add_argument("--build", type=Path, default=REPO / "build" / "3ds" / "dev")
    args = ap.parse_args()

    gc = load_symbols(REPO / "build" / args.version / "MarioSt.elf")
    objs = sorted((args.build / "CMakeFiles" / "ttyd_dol.dir").rglob("*.o"))
    if not objs:
        sys.exit(f"check_layout: no objects under {args.build}; build ttyd_dol first")
    arm = arm_object_sizes(objs)

    bad = []
    compared = 0
    for name, sym in gc.items():
        if sym.kind != "object" or name not in arm or sym.size == 0:
            continue
        compared += 1
        if sym.size not in arm[name]:
            bad.append((name, sym.size, sorted(arm[name])))
    for name, g, a in sorted(bad):
        print(f"{name}: GameCube {g:#x}, 3DS {', '.join(hex(x) for x in a)}")
    print(f"check_layout: {compared} objects compared, {len(bad)} size mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
