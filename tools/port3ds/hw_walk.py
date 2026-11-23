#!/usr/bin/env python3
"""A console walk: a list of test runs (one map each, optionally with a recorded pad file) that the
New 3DS works through by itself, with nobody pressing anything.

    N3DS_IP=192.168.1.50 tools/port3ds/hw_walk.py --name m4 --maps gor_01,hei_00,tik_00 --story 30 --frames 600
    tools/port3ds/hw_walk.py --name pads --maps gor_01 --pad gor_01=build/pads/gor01.tpad
    tools/port3ds/hw_walk.py --name custom --manifest walk.json         # any entries (see devupdate.py --help)
    tools/port3ds/hw_walk.py --name m4 --maps gor_01,hei_00 --dry-run   # the manifest and the estimate only

A WALK IS A MILESTONE CHECK, NOT A ROUTINE ONE. It ties up the console for tens of minutes and wears
nothing but time; routine checks (a map, a fix) stay in Azahar (run_azahar.py, map_sweep.py). So the
list is always explicit (--maps, --list or --manifest; there is no "every map"), the entry count and the
estimated time are printed before anything starts, a long walk needs --yes, and there is a 10 s pause
(Ctrl-C) before the first entry.

What it does (all through libs/ctr-devkit/devupdate/devupdate.py, so the title must run with the
devupdate switch, or have been started by `devupdate.py bootstrap`):
 1. builds the manifest (build/hw-walk/<name>/manifest.json), uploads the pad files and the queue;
 2. `queue start`: the title relaunches itself for entry 1 with its switches (+ padfile=...), and for
    each next entry when the previous one ends (its until pattern, its frame count or its timeout; a
    crash is recorded and the walk goes on);
 3. follows the live log (TCP 17492, across the relaunches) into live.log and polls `queue status`;
 4. at the end pulls results.txt and everything the entries collected (screenshots), and writes
    build/hw-walk/<name>/results.json, shots/<entry>/*.png and a summary table.
Exit status 1 if any entry did not pass (pass = its until matched or its frame count was reached).

Azahar has no APT relaunch: use `devupdate.py azahar-session start --supervise ...` and --ip 127.0.0.1
to try a walk there (docs/PORT_3DS_TESTING.md, "Console walk").
"""
import argparse
import json
import os
import re
import socket
import struct
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "libs/ctr-devkit/devupdate"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import devupdate  # noqa: E402

LIVE_PORT = 17492
BOOT_S = 45      # process start to the game loop, with fastboot
RELAUNCH_S = 12  # end of an entry to the next process
FPS = 30         # a pessimistic frame rate for a map on the console
MAX_PLAIN = 12   # more entries than this need --yes
PAD_END = r"^\[PAD\] padfile end"
OK_RESULTS = ("pass", "frames")


def pad_frames(path):
    """The frame count in a TPAD file's header."""
    head = Path(path).read_bytes()[:16]
    if head[:4] != b"TPAD":
        raise SystemExit(f"hw_walk: {path} is not a TPAD file")
    return struct.unpack_from("<I", head, 8)[0]


def entries_from_maps(args):
    maps = []
    if args.maps:
        maps += [m for m in args.maps.split(",") if m]
    if args.list:
        for line in Path(args.list).read_text().splitlines():
            line = line.split("#")[0].strip()
            if line:
                maps.append(line)
    pads = dict(p.split("=", 1) for p in args.pad)
    if args.maps_file_check:
        try:
            import map_sweep
            known = set(map_sweep.disc_maps(ROOT / "iso/game.iso"))
            bad = [m.split()[0] for m in maps if m.split()[0] not in known]
            if bad:
                raise SystemExit(f"hw_walk: not maps on the disc: {', '.join(bad)}")
        except (OSError, ImportError):
            pass
    entries = []
    for item in maps:
        parts = item.split()
        m = parts[0]
        story = int(parts[1]) if len(parts) > 1 else args.story
        pad = pads.get(m)
        sw = [f"map={m}", f"story={story}", f"party={args.party}"]
        if not args.no_fastboot:
            sw.insert(0, "fastboot")
        if not args.no_livelog:
            sw.append("livelog")
        e = {"name": m, "timeout_s": args.timeout}
        frames = args.frames
        if pad:
            e["padfile"] = pad
            e["until"] = PAD_END
            frames = 0
            e["timeout_s"] = max(args.timeout, int(pad_frames(pad) / FPS) + 120)
        else:
            e["frames"] = frames
        if args.shots:
            last = pad_frames(pad) if pad else frames
            sw += [f"shots=1", f"shotfrom={max(0, last - 40)}", f"shotmax={args.shots}"]
            e["collect"] = ["shots"]
        e["switches"] = " ".join(sw + args.switches.split())
        entries.append(e)
    return entries


def estimate(entries):
    """(expected seconds, worst-case seconds)."""
    exp = worst = 0
    for e in entries:
        t = int(e.get("timeout_s") or 300)
        frames = int(e.get("frames") or 0)
        pad = e.get("padfile")
        if pad and Path(pad).is_file():
            frames = pad_frames(pad)
        run = (frames / FPS if frames else t / 2) + 15
        exp += BOOT_S + min(run, t) + RELAUNCH_S
        worst += BOOT_S + t + RELAUNCH_S
    return exp, worst


def fmt_min(s):
    return f"{s / 60:.0f} min" if s >= 90 else f"{s:.0f} s"


class LiveLog(threading.Thread):
    """The game's live log across relaunches: every new line to live.log; the interesting ones to the terminal."""

    SHOW = re.compile(r"\[QUEUE\]|\[CRASH\]|devupdate:|warp:|\[PAD\] padfile (end|first)")

    def __init__(self, ip, path, quiet):
        super().__init__(daemon=True)
        self.ip, self.path, self.quiet, self.stop = ip, path, quiet, False
        self.seen, self.lines = set(), 0

    def run(self):
        with open(self.path, "a") as out:
            while not self.stop:
                try:
                    s = socket.create_connection((self.ip, LIVE_PORT), timeout=3)
                except OSError:
                    time.sleep(1)
                    continue
                s.settimeout(1.0)
                buf = b""
                while not self.stop:
                    try:
                        chunk = s.recv(8192)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode("utf-8", "replace")
                        m = re.match(r"#(\d+) (.*)", line)
                        if m and m.group(2).startswith("TTYD 3DS v"):
                            if (m.group(1), m.group(2)) not in self.seen:
                                self.seen = set()  # a new process: its numbers start over
                        key = (m.group(1), m.group(2)) if m else (None, line)
                        if key in self.seen:
                            continue  # the ring replay after a reconnect
                        self.seen.add(key)
                        out.write(line + "\n")
                        out.flush()
                        self.lines += 1
                        if not self.quiet and self.SHOW.search(line):
                            print("  " + line, flush=True)
                s.close()
                time.sleep(1)


def png_shots(dest):
    try:
        from PIL import Image
    except ImportError:
        return
    for ppm in dest.rglob("*.ppm"):
        try:
            Image.open(ppm).save(ppm.with_suffix(".png"))
            ppm.unlink()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True, help="the walk's name (results in build/hw-walk/<name>/)")
    ap.add_argument("--ip", default=os.environ.get("N3DS_IP"), help="console address (default $N3DS_IP)")
    ap.add_argument("--port", type=int, default=5050)
    ap.add_argument("--token", default="")
    ap.add_argument("--maps", help="comma-separated maps, one entry each")
    ap.add_argument("--list", help='a file: one "<map> [story]" per line')
    ap.add_argument("--manifest", type=Path, help="a devupdate queue manifest (JSON) instead of maps")
    ap.add_argument("--pad", action="append", default=[], metavar="MAP=FILE.tpad",
                    help="play this TPAD file on that map (the entry ends when it does)")
    ap.add_argument("--story", type=int, default=30, help="story so far (GSW 0) for maps without their own (default 30)")
    ap.add_argument("--party", type=int, default=1)
    ap.add_argument("--frames", type=int, default=600, help="game frames per map (from the process start)")
    ap.add_argument("--timeout", type=int, default=300, help="seconds per entry (from the process start)")
    ap.add_argument("--shots", type=int, default=0, metavar="N", help="N screenshots at the end of each entry (0 = none)")
    ap.add_argument("--switches", default="", help="more game switches for every map entry")
    ap.add_argument("--no-fastboot", action="store_true")
    ap.add_argument("--no-livelog", action="store_true")
    ap.add_argument("--yes", action="store_true", help=f"no pause before the start; needed for more than {MAX_PLAIN} entries")
    ap.add_argument("--dry-run", action="store_true", help="write the manifest and print the estimate, nothing else")
    ap.add_argument("--no-start", action="store_true", help="upload the queue but do not start it")
    ap.add_argument("--follow-only", action="store_true", help="a walk is already running: follow it and pull the results")
    ap.add_argument("--stall", type=float, default=240, help="give up when the console does not answer or the "
                    "entry does not advance for this long beyond its timeout")
    ap.add_argument("--quiet", action="store_true", help="live log lines only to live.log")
    ap.add_argument("--no-disc-check", dest="maps_file_check", action="store_false",
                    help="do not check the map names against iso/game.iso")
    args = ap.parse_args()
    if not args.ip:
        ap.error("give --ip or set N3DS_IP (Azahar: --ip 127.0.0.1)")
    if args.follow_only:
        entries, defaults = [], {}
    elif args.manifest:
        entries, defaults, _ = devupdate.load_manifest(args.manifest)
    elif args.maps or args.list:
        entries, defaults = entries_from_maps(args), {}
    else:
        ap.error("give an explicit list: --maps, --list or --manifest (there is no default \"every map\"; "
                 "a console walk is for milestones, routine checks run in Azahar)")
    out = ROOT / "build/hw-walk" / args.name
    out.mkdir(parents=True, exist_ok=True)
    da = argparse.Namespace(ip=args.ip, port=args.port, token=args.token, timeout=30.0)

    if entries:
        (out / "manifest.json").write_text(json.dumps({"defaults": defaults, "entries": entries}, indent=1))
        exp, worst = estimate(entries)
        print(f"hw_walk: {len(entries)} entries ({', '.join(e['name'] for e in entries[:8])}"
              f"{', ...' if len(entries) > 8 else ''})")
        print(f"hw_walk: estimated {fmt_min(exp)}, at most {fmt_min(worst)} if every entry times out; "
              f"manifest in {out / 'manifest.json'}")
        if len(entries) > MAX_PLAIN and not args.yes and not args.dry_run:
            print(f"hw_walk: more than {MAX_PLAIN} entries: a console walk is for milestones. Pass --yes if you mean it.")
            return 2
        if args.dry_run:
            return 0
        if not args.yes:
            print("hw_walk: starting in 10 s (Ctrl-C to cancel)", flush=True)
            time.sleep(10)

    pong = devupdate.wait_ping(da, 10)
    if pong is None:
        print(f"hw_walk: no devupdate answer on {args.ip}:{args.port}. Start the title with the devupdate switch "
              f"(debug3ds.txt), or `devupdate.py bootstrap`.")
        return 2
    print(f"hw_walk: console: {pong}")
    live = LiveLog(args.ip, out / "live.log", args.quiet)
    (out / "live.log").write_text("")
    live.start()
    started = time.monotonic()
    if entries:
        devupdate.request(da, "queue", "clear")
        if devupdate.queue_add_entries(da, entries, defaults=defaults) != 0:
            print("hw_walk: could not queue the entries")
            return 1
        if args.no_start:
            print("hw_walk: queued; `devupdate.py queue start` begins the walk")
            return 0
        last, lines = devupdate.request(da, "queue", "start")
        print(f"hw_walk: queue start: {last}: {' / '.join(l for l in lines if l != 'done')}")
        if last != "done":
            return 1

    worst_entry = max([int(e.get("timeout_s") or 300) for e in entries] or [600])
    last_progress, last_key, state = time.monotonic(), None, {}
    while True:
        time.sleep(5)
        st = devupdate.queue_status(da, 4)
        if st is not None:
            state = st
            key = (st.get("state"), st.get("index"), st.get("results"))
            if key != last_key:
                print(f"hw_walk: [{(time.monotonic() - started) / 60:5.1f} min] queue {st.get('state')} entry {st.get('index')} "
                      f"of {st.get('total')}, {st.get('results')} done", flush=True)
                last_key, last_progress = key, time.monotonic()
            if st.get("state") == "done":
                break
        if time.monotonic() - last_progress > worst_entry + args.stall:
            print(f"hw_walk: no progress for {time.monotonic() - last_progress:.0f} s "
                  f"({'the console does not answer' if st is None else 'the entry did not end'}); giving up")
            break
    time.sleep(2)
    live.stop = True

    rows = devupdate.queue_pull(da, out)
    rows = {r["index"]: r for r in rows}
    summary = []
    for i, e in enumerate(entries, 1) if entries else []:
        r = rows.get(i, {"index": i, "name": e["name"], "result": "missing", "frames": 0, "ms": 0, "outdir": "",
                         "detail": "no result line (the walk stopped before this entry)", "files": []})
        summary.append(r)
    if not entries:
        summary = [rows[k] for k in sorted(rows)]
    png_shots(out)
    for r in summary:
        r["ok"] = r["result"] in OK_RESULTS
    (out / "results.json").write_text(json.dumps({"name": args.name, "console": args.ip, "final_state": state,
                                                  "entries": summary}, indent=1))
    print()
    print(f"{'#':>3} {'entry':<24} {'result':<8} {'frames':>7} {'time':>8}  files  detail")
    for r in summary:
        print(f"{r['index']:>3} {r['name']:<24} {r['result']:<8} {r['frames']:>7} {r['ms'] / 1000:7.1f}s  "
              f"{len(r.get('files', [])):>5}  {r['detail']}")
    bad = [r for r in summary if not r["ok"]]
    print(f"hw_walk: {len(summary) - len(bad)} of {len(summary)} entries passed; results in {out / 'results.json'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
