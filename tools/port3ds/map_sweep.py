#!/usr/bin/env python3
"""Visit every map of the disc in Azahar and report which ones load.

The game's sweep switch (port3ds/game/vi.c) warps to each map listed in
sdmc:/3ds/ttyd/sweep.txt, stays --frames game frames once the map is up,
saves sweep/<map>.ppm and logs "sweep: <map> ok". This script writes that
list, runs tools/port3ds/run_azahar.py, and reads the log. When a run ends
early (a crash, a stall, a battle the map starts), the next run resumes with
the map after the one that was being visited, so one bad map costs one run.

The stg_* battle-background maps are not map-change targets: the game runs a
battle on hei_01 (story 30) that loads each one, and the result reads
"ok (battle stage)" (see PORT_3DS_TESTING.md, "Map sweep").

Results go to build/map-sweep/: results.json (map -> ok, timeout, stopped,
crash, stall) and the screenshots as PNG when Pillow is installed.

    tools/port3ds/map_sweep.py                      # every map, story 0
    tools/port3ds/map_sweep.py --maps gor_01,hei_00 --story 30
    tools/port3ds/map_sweep.py --area tik --frames 300
"""
import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_usage  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SDMC = ROOT / "build/azahar-profile/data/azahar-emu/sdmc"
GAME_DIR = SDMC / "3ds/ttyd"
OUT = ROOT / "build/map-sweep"


def disc_maps(iso):
    """Map names: the directories under m/ in the disc's file system table."""
    with open(iso, "rb") as f:
        f.seek(0x424)
        fst_off, fst_size = struct.unpack(">II", f.read(8))
        f.seek(fst_off)
        fst = f.read(fst_size)
    n = struct.unpack(">I", fst[8:12])[0]
    strtab = fst[n * 12:]

    def name(o):
        return strtab[o:strtab.index(b"\0", o)].decode("ascii")

    maps, ends = [], []  # ends: (index past the directory, name)
    for i in range(1, n):
        w0, _, end = struct.unpack(">III", fst[i * 12:i * 12 + 12])
        while ends and i >= ends[-1][0]:
            ends.pop()
        if w0 >> 24:
            if [e[1] for e in ends] == ["m"]:
                maps.append(name(w0 & 0xFFFFFF))
            ends.append((end, name(w0 & 0xFFFFFF)))
    return sorted(maps)


LAST = ROOT / "build/azahar-last"
EXTRA_SWITCHES = ""  # more game switches, e.g. " memmap"
EXTRA = []  # more run_azahar.py arguments (--profile, --last, --gdb ...)


SECONDS_PER_MAP = 20.0  # the load plus the 120-frame stay, with margin (Azahar, no frame limit)
BASE_SECONDS = 25.0     # boot and fastboot
MAX_MAPS_PER_RUN = 60   # keeps a run inside the --long cap (1800 s) and one bad map from costing a long run


def estimate_timeout(n_maps, frames):
    return BASE_SECONDS + n_maps * SECONDS_PER_MAP * max(frames, 60) / 120.0


def run(build, todo, story, frames, party, timeout, fastboot=True):
    """One Azahar run over todo; returns {map: result} for the maps it reached."""
    GAME_DIR.mkdir(parents=True, exist_ok=True)
    (GAME_DIR / "sweep.txt").write_text("".join(f"{m} {story}\n" for m in todo))
    shutil.rmtree(GAME_DIR / "sweep", ignore_errors=True)
    switches = f"sweep={frames} party={party} " + ("fastboot" if fastboot else "nofastboot") + EXTRA_SWITCHES
    cmd = [sys.executable, str(ROOT / "tools/port3ds/run_azahar.py"), "--build", str(build),
           "--switches", switches, "--log", "3ds/ttyd/latest.log", "--until", r"sweep: (done|stopped)",
           *emu_usage.time_args(timeout), "--stall", "60"] + EXTRA
    child = subprocess.Popen(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        out, err = child.communicate()
    except BaseException:
        # let run_azahar.py stop its Azahar (subprocess.run would SIGKILL it and orphan the emulator)
        child.terminate()
        try:
            child.wait(timeout=30)
        except subprocess.TimeoutExpired:
            child.kill()
        raise
    proc = subprocess.CompletedProcess(cmd, child.returncode, out, err)
    log = (LAST / "game_log.txt").read_text(errors="replace")
    # keep the verify/warning lines of every chunk (the game log of a chunk is overwritten by the next one)
    keep = [l for l in log.splitlines() if re.search(r"verify|mismatch|missed|stale|warn|error|fatal|crash|sweep: \S+ (ok|timeout)", l, re.I)]
    OUT.mkdir(parents=True, exist_ok=True)
    with open(OUT / "verify.log", "a") as vf:
        vf.write("\n".join(keep) + "\n")
    results, current = {}, None
    for line in log.splitlines():
        m = re.search(r"sweep: (\S+) \(story", line)
        if m:
            current = m.group(1)
            continue
        m = re.search(r"sweep: (\S+) not in the map table", line)
        if m:
            results[m.group(1)] = {"result": "not in map table", "line": line.strip()}
            continue
        m = re.search(r"sweep: (\S+) (ok|timeout)", line)
        if m:
            results[m.group(1)] = {"result": m.group(2), "line": line.strip()}
            continue
        m = re.search(r"sweep: stopped after (\S+)", line)
        if m:
            results[m.group(1)]["result"] = "stopped"
            results[m.group(1)]["line"] = line.strip()
    if current is not None and current not in results:
        crash = [l for l in log.splitlines() if "[CRASH]" in l or "fatal" in l.lower()]
        if crash:
            results[current] = {"result": "crash", "line": crash[0].strip()}
        else:
            results[current] = {"result": "stall", "line": (proc.stdout.strip().splitlines() or [""])[-1]}
        crashlog = OUT / "logs" / f"{current}.txt"
        crashlog.parent.mkdir(parents=True, exist_ok=True)
        crashlog.write_text(log)
    shots = GAME_DIR / "sweep"
    if shots.is_dir():
        (OUT / "shots").mkdir(parents=True, exist_ok=True)
        for ppm in shots.glob("*.ppm"):
            try:
                from PIL import Image
                Image.open(ppm).save(OUT / "shots" / (ppm.stem + ".png"))
            except ImportError:
                shutil.copy(ppm, OUT / "shots" / ppm.name)
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=ROOT / "build/3ds/game/ttyd.cia")
    ap.add_argument("--iso", type=Path, default=ROOT / "iso/game.iso")
    ap.add_argument("--maps", help="comma-separated maps (default: every map on the disc)")
    ap.add_argument("--area", help="only maps whose name starts with this")
    ap.add_argument("--story", type=int, default=0, help="GSW 0 for every map")
    ap.add_argument("--party", type=int, default=1)
    ap.add_argument("--frames", type=int, default=120, help="game frames on each map before its screenshot")
    ap.add_argument("--nofastboot", action="store_true", help="do not press A through the boot screens")
    ap.add_argument("--timeout", type=float, help="seconds per Azahar run (default: estimated from the maps in the run; "
                    "over 60 s the run is a --long run, max 1800)")
    ap.add_argument("--chunk", type=int, default=MAX_MAPS_PER_RUN, help="most maps in one Azahar run")
    ap.add_argument("--profile", type=Path, help="Azahar profile folder for a second instance (see run_azahar.py)")
    ap.add_argument("--last", type=Path, help="run_azahar's log folder (default build/azahar-last)")
    ap.add_argument("--out", type=Path, help="results folder (default build/map-sweep)")
    ap.add_argument("--switches", default="", help="more game switches for every map (e.g. memmap)")
    ap.add_argument("--gdb-port", type=int, help="enable Azahar's GDB stub on this port (for pc_sample.py)")
    args = ap.parse_args()
    global SDMC, GAME_DIR, OUT, LAST, EXTRA_SWITCHES
    if args.switches:
        EXTRA_SWITCHES = " " + args.switches
    if args.profile:
        SDMC = args.profile.resolve() / "data/azahar-emu/sdmc"
        GAME_DIR = SDMC / "3ds/ttyd"
        EXTRA.extend(["--profile", str(args.profile.resolve())])
    if args.last:
        LAST = args.last.resolve()
        EXTRA.extend(["--last", str(LAST)])
    if args.out:
        OUT = args.out.resolve()
    if args.gdb_port:
        EXTRA.extend(["--gdb", "--gdb-port", str(args.gdb_port), "--allow-hw-faults"])

    maps = args.maps.split(",") if args.maps else disc_maps(args.iso)
    if args.area:
        maps = [m for m in maps if m.startswith(args.area)]
    OUT.mkdir(parents=True, exist_ok=True)
    results = {}
    todo = list(maps)
    while todo:
        batch = todo[:args.chunk]
        timeout = args.timeout or min(emu_usage.MAX_LONG, estimate_timeout(len(batch), args.frames))
        got = run(args.build, batch, args.story, args.frames, args.party, timeout, not args.nofastboot)
        results.update(got)
        reached = [m for m in batch if m in got]
        if not reached:
            results[batch[0]] = {"result": "not reached", "line": ""}
            todo = todo[1:]
            continue
        last = max(todo.index(m) for m in reached)
        todo = todo[last + 1:]
        print(f"map_sweep: {len(results)}/{len(maps)} maps, {len(todo)} left", flush=True)
    (OUT / "results.json").write_text(json.dumps(results, indent=1, sort_keys=True))
    bad = {m: r for m, r in results.items() if r["result"] not in ("ok", "not in map table")}
    for m, r in sorted(bad.items()):
        print(f"{m}: {r['result']}: {r['line']}")
    print(f"map_sweep: {len(results) - len(bad)} of {len(results)} maps ok; results in {OUT}/results.json")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
