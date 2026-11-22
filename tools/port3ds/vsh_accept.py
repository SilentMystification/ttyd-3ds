#!/usr/bin/env python3
"""Acceptance tests of the GX vertex-shader path (docs/PORT_3DS_GPU_OFFLOAD.md).

Runs a scene in Azahar (through run_azahar.py, one instance at a time) with
the frame clear set to magenta, a burst of consecutive EFB shots and their
depth buffers, and gxstats; then compares runs.

    vsh_accept.py run --scene gor_01 --tag cpuA --switches "gxvsh=0"
    vsh_accept.py run --scene gon_02 --story 30 --tag vsh --switches "gxvsh=1 gxvshdecal=0"
    vsh_accept.py report --scene gor_01 --ref cpuA --noise cpuB --test vsh [--test vsh_d1]

Each run goes to build/vsh-accept/<scene>/<tag>/ (shots NNNNN.ppm, NNNNN.dep,
game_log.txt). The tests, per scene:

  1. image diff: same-numbered shots of two runs, per-pixel max channel
     difference; clusters of differing pixels. The game is not deterministic
     between runs (it animates by time), so two CPU-path runs give the noise
     floor; a test run passes when it is within that floor.
  2. depth diff: the same for the depth buffers (24-bit units), over pixels
     both runs drew; "order flips" are pixels whose colour differs by more
     than 32 while the depth differs by at most 16 units (another surface won
     a near tie: potential z-fighting).
  3. seam test: magenta (the clear colour, untouched by any draw) inside
     geometry, i.e. bounded by drawn pixels within 3 pixels on both sides
     (left and right, or above and below). Expected: no more than the CPU run.
  4. flicker test: in consecutive shots of one run, pixels that change by more
     than 32 and change back (A-B-A, within 4). A count above the CPU run's
     is z-fighting.

No game knowledge beyond the switches passed in.
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "build" / "vsh-accept"
MAGENTA = np.array([255, 0, 255], dtype=np.uint8)


def sdmc_dir(profile):
    return profile / "data" / "azahar-emu" / "sdmc" / "3ds" / "ttyd"


def cmd_run(a):
    dest = OUT / a.scene / a.tag
    profile = (REPO / "build" / "azahar-profile") if a.profile is None else Path(a.profile)
    shots = sdmc_dir(profile) / "shots"
    if shots.exists():
        for f in shots.iterdir():
            f.unlink()
    last = a.first + a.count - 1
    sw = (f"map={a.scene} " + (f"story={a.story} " if a.story is not None else "") +
          f"gxstats=60 gxclearrgb=FF00FF gxdepthshot shots=1 shotfrom={a.first} shotmax={a.count} " + a.switches)
    args = [sys.executable, str(REPO / "tools/port3ds/run_azahar.py"), "--build", str(a.build), "--log",
            "3ds/ttyd/log.txt", "--switches", sw, "--until", f"shot shots/{last:05d}.ppm: this record",
            "--timeout", str(a.timeout), "--stall", "180", "--last", str(dest / "azahar")]
    if a.profile is not None:
        args += ["--profile", str(profile)]
    print("vsh_accept: run", a.scene, a.tag, ":", sw, flush=True)
    rc = subprocess.call(args)
    dest.mkdir(parents=True, exist_ok=True)
    n = 0
    if shots.exists():
        for f in sorted(shots.iterdir()):
            if f.suffix in (".ppm", ".dep"):
                shutil.copy2(f, dest / f.name)
                n += 1
    log = dest / "azahar" / "game_log.txt"
    if log.exists():
        shutil.copy2(log, dest / "game_log.txt")
    print(f"vsh_accept: {n} files in {dest} (run_azahar rc {rc})")
    return rc


def read_ppm(p):
    data = p.read_bytes()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    return np.frombuffer(data[m.end():m.end() + w * h * 3], dtype=np.uint8).reshape(h, w, 3)


def read_dep(p):
    data = p.read_bytes()
    m = re.match(rb"DEP1 (\d+) (\d+)\n", data)
    w, h = int(m.group(1)), int(m.group(2))
    words = np.frombuffer(data[m.end():m.end() + w * h * 4], dtype="<u4").reshape(h, w)
    return (words & 0xFFFFFF).astype(np.int64)


def shots(tag_dir):
    out = {}
    for p in sorted(tag_dir.glob("*.ppm")):
        img = read_ppm(p)
        if img.max() == 0:
            continue  # Azahar's occasional all-black shot
        out[int(p.stem)] = p
    return out


def clusters(mask, min_size=1):
    """Sizes of 4-connected components of a boolean mask (pure Python, small masks)."""
    ys, xs = np.nonzero(mask)
    pts = set(zip(ys.tolist(), xs.tolist()))
    sizes = []
    while pts:
        stack = [pts.pop()]
        n = 0
        while stack:
            y, x = stack.pop()
            n += 1
            for q in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                if q in pts:
                    pts.remove(q)
                    stack.append(q)
        if n >= min_size:
            sizes.append(n)
    return sorted(sizes, reverse=True)


def image_diff(da, db):
    sa, sb = shots(da), shots(db)
    common = sorted(set(sa) & set(sb))
    res = []
    for k in common:
        a, b = read_ppm(sa[k]).astype(np.int16), read_ppm(sb[k]).astype(np.int16)
        d = np.abs(a - b).max(axis=2)
        big = d > 32
        cl = clusters(big) if big.sum() < 40000 else [int(big.sum())]
        res.append(dict(frame=k, max=int(d.max()), mean=float(d.mean()), over8=float((d > 8).mean() * 100),
                        over32=int(big.sum()), clusters=len(cl), biggest=cl[0] if cl else 0))
    return res


def depth_diff(da, db):
    common = sorted(set(int(p.stem) for p in da.glob("*.dep")) & set(int(p.stem) for p in db.glob("*.dep")))
    sa, sb = shots(da), shots(db)
    res = []
    for k in common:
        if k not in sa or k not in sb:
            continue
        za, zb = read_dep(da / f"{k:05d}.dep"), read_dep(db / f"{k:05d}.dep")
        ca, cb = read_ppm(sa[k]), read_ppm(sb[k])
        drawn = ~(np.all(ca == MAGENTA, axis=2) | np.all(cb == MAGENTA, axis=2))
        dz = np.abs(za - zb)[drawn]
        dc = np.abs(ca.astype(np.int16) - cb.astype(np.int16)).max(axis=2)[drawn]
        if dz.size == 0:
            continue
        hist = [int((dz == 0).sum()), int((dz == 1).sum()), int(((dz >= 2) & (dz < 4)).sum()),
                int(((dz >= 4) & (dz < 16)).sum()), int(((dz >= 16) & (dz < 256)).sum()), int((dz >= 256).sum())]
        flips = int(((dc > 32) & (dz <= 16)).sum())
        res.append(dict(frame=k, max=int(dz.max()), mean=float(dz.mean()), hist=hist, flips=flips,
                        zero_depth=bool(za.max() == 0 and zb.max() == 0)))
    return res


def seam_count(img):
    mag = np.all(img == MAGENTA, axis=2)
    drawn = ~mag
    r = 3
    h, w = mag.shape

    def near(arr, axis, sign):
        out = np.zeros_like(arr)
        for k in range(1, r + 1):
            sh = np.roll(arr, sign * k, axis=axis)
            if axis == 1:
                if sign > 0:
                    sh[:, :k] = False
                else:
                    sh[:, -k:] = False
            else:
                if sign > 0:
                    sh[:k, :] = False
                else:
                    sh[-k:, :] = False
            out |= sh
        return out

    inside = mag & ((near(drawn, 1, 1) & near(drawn, 1, -1)) | (near(drawn, 0, 1) & near(drawn, 0, -1)))
    return int(inside.sum()), int(mag.sum())


def flicker(d):
    s = shots(d)
    ks = sorted(s)
    tot = 0
    worst = 0
    n = 0
    imgs = {}
    for i in range(len(ks) - 2):
        a, b, c = ks[i], ks[i + 1], ks[i + 2]
        if b != a + 1 or c != b + 1:
            continue
        for k in (a, b, c):
            if k not in imgs:
                imgs[k] = read_ppm(s[k]).astype(np.int16)
        x, y, z = imgs[a], imgs[b], imgs[c]
        f = ((np.abs(x - y).max(axis=2) > 32) & (np.abs(y - z).max(axis=2) > 32) & (np.abs(x - z).max(axis=2) <= 4))
        cnt = int(f.sum())
        tot += cnt
        worst = max(worst, cnt)
        n += 1
    return dict(triplets=n, total=tot, worst=worst)


def gxstats(d):
    log = d / "game_log.txt"
    if not log.exists():
        return {}
    out = {}
    for line in log.read_text(errors="replace").splitlines():
        m = re.search(r"gxstats frame (\d+): avg of \d+ frames: (\d+) us/frame", line)
        if m:
            out.setdefault("frames", []).append((int(m.group(1)), int(m.group(2))))
        m = re.search(r"gxstats   vsh: draws shader (\d+).*?cpu-eligible (\d+) ineligible (\d+).*?groups (\d+) "
                      r"shader (\d+) eligible (\d+) \| switches up (\d+) down (\d+) \(total (\d+)\) splits (\d+)", line)
        if m:
            out.setdefault("vsh", []).append(tuple(int(x) for x in m.groups()))
        m = re.search(r"gxstats   us: dl (\d+) \(decode (\d+)\) imm (\d+) \(decode (\d+)\) state (\d+) xform (\d+) "
                      r"emit (\d+) cache (\d+)", line)
        if m:
            out.setdefault("us", []).append(tuple(int(x) for x in m.groups()))
        m = re.search(r"render (\d+) us gpu (\d+) us", line)
        if m:
            out.setdefault("render", []).append(int(m.group(1)))
        m = re.search(r"vsh decals.*?(\d+) of (\d+) frames with a trigger \((\d+) by an immediate", line)
        if m:
            out.setdefault("decal", []).append(tuple(int(x) for x in m.groups()))
    return out


def summarize_img(res):
    if not res:
        return "no common shots"
    return (f"{len(res)} shots: max {max(r['max'] for r in res)}, mean {np.mean([r['mean'] for r in res]):.2f}, "
            f"worst {max(r['over8'] for r in res):.2f}% > 8, px > 32: worst {max(r['over32'] for r in res)}, "
            f"biggest cluster {max(r['biggest'] for r in res)}")


def summarize_depth(res):
    if not res:
        return "no depth"
    if all(r["zero_depth"] for r in res):
        return "depth buffer read back as all zero (readback failed)"
    h = np.sum([r["hist"] for r in res], axis=0)
    tot = h.sum()
    return (f"{len(res)} shots: max |dz| {max(r['max'] for r in res)}, mean {np.mean([r['mean'] for r in res]):.2f}; "
            f"dz 0: {h[0] * 100 / tot:.2f}%, 1: {h[1] * 100 / tot:.2f}%, 2-3: {h[2] * 100 / tot:.2f}%, "
            f"4-15: {h[3] * 100 / tot:.3f}%, 16-255: {h[4] * 100 / tot:.3f}%, 256+: {h[5] * 100 / tot:.3f}%; "
            f"order flips (colour > 32, dz <= 16): total {sum(r['flips'] for r in res)}, worst {max(r['flips'] for r in res)}")


def cmd_report(a):
    base = OUT / a.scene
    ref = base / a.ref
    lines = [f"## {a.scene}"]
    runs = [a.ref] + ([a.noise] if a.noise else []) + a.test
    for t in runs:
        st = gxstats(base / t)
        sh = shots(base / t)
        seams = [seam_count(read_ppm(p)) for _, p in sorted(sh.items())]
        fl = flicker(base / t)
        us = st.get("us", [])
        last = us[-1] if us else None
        vsh = st.get("vsh", [])
        frames = st.get("frames", [])
        lines.append(f"- {t}: {len(sh)} shots; seams inside geometry: total {sum(s[0] for s in seams)}, worst "
                     f"{max((s[0] for s in seams), default=0)} (magenta total worst {max((s[1] for s in seams), default=0)}); "
                     f"flicker A-B-A: {fl['total']} px over {fl['triplets']} triplets (worst {fl['worst']})")
        if frames:
            lines.append(f"  - gxstats frames: " + ", ".join(f"{f}: {u / 1000:.1f} ms" for f, u in frames[-4:]))
        if last:
            lines.append(f"  - last window us: dl {last[0]} imm {last[2]} state {last[4]} xform {last[5]} emit {last[6]} "
                         f"cache {last[7]}; render {st.get('render', [0])[-1]} us")
        if vsh:
            v = vsh[-1]
            lines.append(f"  - vsh: shader draws {v[0]}, cpu-eligible {v[1]}, ineligible {v[2]}, groups {v[3]} "
                         f"(shader {v[4]}, eligible {v[5]}); switches in the window up {v[6]} down {v[7]}, total {v[8]}; "
                         f"splits {v[9]}")
            ups = [x[6] + x[7] for x in vsh]
            lines.append(f"  - switches per 60-frame window, all windows: {ups}")
        if st.get("decal"):
            lines.append(f"  - decal triggers: last window {st['decal'][-1]}")
    if a.noise:
        lines.append(f"- image {a.ref} vs {a.noise} (noise floor): {summarize_img(image_diff(ref, base / a.noise))}")
        lines.append(f"- depth {a.ref} vs {a.noise} (noise floor): {summarize_depth(depth_diff(ref, base / a.noise))}")
    for t in a.test:
        lines.append(f"- image {a.ref} vs {t}: {summarize_img(image_diff(ref, base / t))}")
        lines.append(f"- depth {a.ref} vs {t}: {summarize_depth(depth_diff(ref, base / t))}")
    text = "\n".join(lines)
    print(text)
    (base / "report.md").write_text(text + "\n")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--scene", required=True)
    r.add_argument("--story", type=int)
    r.add_argument("--tag", required=True)
    r.add_argument("--switches", default="")
    r.add_argument("--first", type=int, default=1140, help="first presented frame shot")
    r.add_argument("--count", type=int, default=24, help="consecutive shots")
    r.add_argument("--build", type=Path, default=REPO / "build/3ds/game/ttyd.cia")
    r.add_argument("--profile")
    r.add_argument("--timeout", type=int, default=1200)
    p = sub.add_parser("report")
    p.add_argument("--scene", required=True)
    p.add_argument("--ref", required=True)
    p.add_argument("--noise")
    p.add_argument("--test", action="append", default=[])
    a = ap.parse_args()
    return cmd_run(a) if a.cmd == "run" else cmd_report(a)


if __name__ == "__main__":
    sys.exit(main())
