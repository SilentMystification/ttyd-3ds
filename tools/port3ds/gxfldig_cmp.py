#!/usr/bin/env python3
"""Compares the gxfldig lines of two logs (switch gxfldig=1, or gxsplit=2; plat/gpu.c's gxfldig section).

Each run digests the register state of its frame lists at every draw; under `determ` frame N is the same game state
in both runs, so equal frame lists give equal lines. Typical use: K (gxone=1 gxchunk=1 gximmfl=1 gxfldig=1) against
gxsplit=2 with the same switches otherwise.

  gxfldig_cmp.py A.log B.log [--max 10]

Prints the records compared, equal, different (first ones with their draw counts) and the records only one log has.
With gxfldigdraw=F in both runs it also names the first draw of frame F whose digest differs.
Exit status 0 only when every record both logs have is equal and at least one was compared."""
import argparse
import re
import sys

REC = re.compile(r"gxfldig frame (\d+) rec (\d+): draws (\d+) steps (\d+) hash ([0-9a-f]+)")
DRAW = re.compile(r"gxfldig draw (\d+)\.(\d+)\.(\d+): ([0-9a-f]+)")


def load(path):
    recs, draws = {}, {}
    with open(path, errors="replace") as f:
        for line in f:
            m = REC.search(line)
            if m:
                recs[(int(m[1]), int(m[2]))] = (int(m[3]), int(m[4]), m[5])
                continue
            m = DRAW.search(line)
            if m:
                draws[(int(m[1]), int(m[2]), int(m[3]))] = m[4]
    return recs, draws


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--max", type=int, default=10)
    args = ap.parse_args()
    ra, da = load(args.a)
    rb, db = load(args.b)
    both = sorted(set(ra) & set(rb))
    diff = [k for k in both if ra[k] != rb[k]]
    print(f"records: {len(ra)} in A, {len(rb)} in B, {len(both)} compared, {len(both) - len(diff)} equal, "
          f"{len(diff)} differ; only A {len(set(ra) - set(rb))}, only B {len(set(rb) - set(ra))}")
    for k in diff[: args.max]:
        print(f"  frame {k[0]} rec {k[1]}: A draws {ra[k][0]} steps {ra[k][1]} hash {ra[k][2]} | "
              f"B draws {rb[k][0]} steps {rb[k][1]} hash {rb[k][2]}")
    dboth = sorted(set(da) & set(db))
    if dboth:
        dd = [k for k in dboth if da[k] != db[k]]
        print(f"draws: {len(dboth)} compared, {len(dd)} differ" +
              (f", first frame {dd[0][0]} rec {dd[0][1]} draw {dd[0][2]}" if dd else ""))
    return 0 if both and not diff else 1


if __name__ == "__main__":
    sys.exit(main())
