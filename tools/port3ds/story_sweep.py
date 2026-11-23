#!/usr/bin/env python3
"""Story sweep by checkpoints: the port against the GameCube at key story points (backlog lane B 2).

    tools/port3ds/story_sweep.py                          # every point of tools/port3ds/story_points.json
    tools/port3ds/story_sweep.py --only ch1_,ch2_stg2_24  # names starting with these
    tools/port3ds/story_sweep.py --report                 # rebuild the table and the HTML from results.json

Each point is a row of the game's own story table (src/event.c, the table the E3 scenario select
uses). Both builds boot with the same switches:

    storyjump=<textId>[.n] determ gwdump=<every> shotat=<frames> [padscript=...] [extra]

storyjump= sets the story up there as the game's scenario select does (GSW 0, every set-up function
up to that row, the partner) and warps to the row's map at the title (port3ds/game/test_mode.c).
The port runs in Azahar (run_azahar.py, which waits for a free slot of the machine-wide limit), the
GameCube debug DOL in Dolphin (run_dolphin.py --gcdebug --shots), one of each at a time, side by
side. Then per point:

- the gwdump streams are compared as gw_diff.py does (all fields but the raw GlobalWork hash);
- each shot pair (the port's EFB, the GameCube's XFB, both 608x480 at the same game frame) is diffed
  as gallery.py does (mean absolute difference of the largest channel, 0-255). Azahar's all-black
  shots are left out.

Result per point, worst first: port-crash, hang (the port stopped before the last frame: a stall or
timeout), gc-failed (the Dolphin run failed), state-diverged (the first frame and fields), image-diff
(a shot taken before the divergence scores over --image-threshold), ok. A divergence whose first
fields are only rng/evtgw (sound timing: backlog lane F 5) is marked "rng".

Results go to build/story-sweep/: results.json, table.md, index.html, port/<point>/fNNNNN.png,
gc/<point>/fNNNNN.png, diff/, logs/. A point already in results.json is skipped unless --redo.

Exit status: 0 done (findings are in the results), 1 nothing could run, 2 usage error.
"""

import argparse
import concurrent.futures as cf
import html
import json
import re
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gallery  # noqa: E402
import gw_diff  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
OUT = REPO / "build" / "story-sweep"
POINTS = REPO / "tools" / "port3ds" / "story_points.json"
RNG_ONLY = {"rng", "evtgw", "gwv"}
LOCK = threading.Lock()


def mash(start, frames, period=30):
    """A pressed for 2 frames every `period` game frames from `start` (dialogue, cutscene prompts)."""
    steps = [f"wait*{start}"]
    n = (frames - start) // period
    steps += [f"a*2,wait*{period - 2}"] * max(n, 0)
    return ",".join(steps)


def point_switches(p, d):
    frames = p.get("frames", d["frames"])
    shots = p.get("shots", d["shots"])
    every = d["every"]
    sw = [f"storyjump={p['storyjump']}", "determ", f"gwdump={every}", "shotat=" + ",".join(map(str, shots))]
    pad = p.get("padscript")
    if pad == "mash":
        sw.append("padscript=" + mash(d.get("mash_from", 640), frames))
    elif pad:
        sw.append("padscript=" + pad)
    if p.get("extra"):
        sw.append(p["extra"])
    return " ".join(sw), frames, shots, every


def runner_args(a):
    return argparse.Namespace(cia=a.cia, stall=a.stall, gc_timeout=a.gc_timeout, reuse_gc=a.reuse_gc)


def port_job(p, sw, frames, shots, every, a):
    name = p["name"]
    last = OUT / "logs" / name / "port"
    until = rf"gwdump f={frames + every} "
    with LOCK:
        print(f"story: port {name}: {sw[:160]}", flush=True)
    ok, reason, text = gallery.run_port(runner_args(a), sw, until, a.port_timeout_base + frames / a.port_fps, last)
    gallery.collect_port_shots(OUT / "port" / name)
    with LOCK:
        print(f"story: port {name}: {'ok' if ok else 'FAILED'} {reason}", flush=True)
    return ok, reason, text


def gc_job(p, sw, frames, shots, every, a):
    name = p["name"]
    with LOCK:
        print(f"story: dolphin {name}", flush=True)
    ok, reason, text = gallery.run_gc(runner_args(a), sw, frames + every, OUT / "gc" / name, OUT / "logs" / name / "gc")
    with LOCK:
        print(f"story: dolphin {name}: {'ok' if ok else 'FAILED'} {reason}", flush=True)
    if not ok:
        # Dolphin's Vulkan backend (needed for the shots) has crashed in some scenes (SIGSEGV, the Glitz Pit arena)
        # where the Null backend runs through: get the state stream without shots
        ok, r2, text = gallery.run_gc(runner_args(a), sw, frames + every, None, OUT / "logs" / name / "gc-noshots")
        reason = f"{reason}; without shots: {'ok' if ok else r2}"
        with LOCK:
            print(f"story: dolphin {name} without shots: {'ok' if ok else 'FAILED'} {r2}", flush=True)
    return ok, reason, text


def first_diff(port_text, gc_text, upto):
    """gw_diff's comparison on frame-aligned records up to frame `upto` (the last record of a run can be cut)."""
    ra = {f: r for f, r in gw_diff.parse(port_text).items() if f <= upto}
    rb = {f: r for f, r in gw_diff.parse(gc_text).items() if f <= upto}
    common = sorted(set(ra) & set(rb))
    last_same = None
    for f in common:
        x, y = ra[f], rb[f]
        fields = [k for k in sorted(set(x) | set(y)) if k not in gw_diff.CROSS_SKIP and k not in ("f", "wf")]
        diff = [k for k in fields if x.get(k) != y.get(k)]
        if diff:
            return {"at": f, "last_equal": last_same, "fields": {k: [x.get(k), y.get(k)] for k in diff},
                    "records": len(common)}
        last_same = f
    return {"at": None, "last_equal": last_same, "records": len(common)}


def last_record(text):
    recs = gw_diff.parse(text)
    if not recs:
        return {}
    f = max(recs)
    r = recs[f]
    return {"f": f, "map": r.get("map"), "seq": r.get("seq"), "gsw0": r.get("gsw0"), "inbtl": r.get("inbtl")}


def log_lines(text, pat, n=12):
    return [gallery.strip(l) for l in text.splitlines() if re.search(pat, l)][:n]


def score_shots(name, shots, a):
    pairs = []
    for f in shots:
        pp, gp = OUT / "port" / name / f"f{f:05d}.png", OUT / "gc" / name / f"f{f:05d}.png"
        e = {"f": f, "port": pp.exists(), "gc": gp.exists()}
        if pp.exists() and gp.exists():
            x, y = gallery.load_image(pp), gallery.load_image(gp)
            if gallery.uniform(x):
                e["note"] = "port shot is one colour (Azahar's black shots)"
                if gallery.uniform(y):
                    e["note"] = "both shots are one colour"
            else:
                st, d = gallery.diff_stats(x, y, a.pixel_threshold)
                e.update(st)
                (OUT / "diff" / name).mkdir(parents=True, exist_ok=True)
                gallery.save_diff(d, OUT / "diff" / name / f"f{f:05d}.png")
        pairs.append(e)
    return pairs


def classify(r, a):
    port, gc = r["port"], r["gc"]
    if port.get("crash") or "CRASH" in port.get("reason", ""):
        return "port-crash"
    if not port.get("ok"):
        return "hang"
    if not gc.get("ok"):
        return "gc-failed"
    div = r.get("divergence", {})
    at = div.get("at")
    pre = [p["mean"] for p in r["shots"] if "mean" in p and (at is None or p["f"] < at)]
    r["image_pre"] = max(pre) if pre else None
    allm = [p["mean"] for p in r["shots"] if "mean" in p]
    r["image_all"] = max(allm) if allm else None
    if at is not None:
        r["rng_only"] = set(div["fields"]) <= RNG_ONLY
        return "state-diverged"
    if r["image_pre"] is not None and r["image_pre"] > a.image_threshold:
        return "image-diff"
    return "ok"


def run_point(p, d, a, pool_port, pool_gc):
    sw, frames, shots, every = point_switches(p, d)
    fp = pool_port.submit(port_job, p, sw, frames, shots, every, a)
    fg = pool_gc.submit(gc_job, p, sw, frames, shots, every, a)
    return p, sw, frames, shots, fp, fg


def finish(p, sw, frames, shots, fp, fg, a):
    pok, preason, ptext = fp.result()
    gok, greason, gtext = fg.result()
    r = {"name": p["name"], "chapter": p["chapter"], "kind": p.get("kind"), "title": p.get("title"),
         "storyjump": p["storyjump"], "map": p.get("map"), "switches": sw, "frames": frames,
         "port": {"ok": pok, "reason": preason, "last": last_record(ptext), "crash": "[CRASH]" in ptext, "text_tail": "\n".join(ptext.splitlines()[-30:]),
                  "lines": log_lines(ptext, r"storyjump:|warp:|\[CRASH\]|battle: (start|end)|fatal|watchdog")},
         "gc": {"ok": gok, "reason": greason, "last": last_record(gtext),
                "lines": log_lines(gtext, r"storyjump:|warp:|battle: (start|end)")}}
    r["divergence"] = first_diff(ptext, gtext, frames)
    r["shots"] = score_shots(p["name"], shots, a)
    r["result"] = classify(r, a)
    return r


def load():
    p = OUT / "results.json"
    return json.loads(p.read_text()) if p.exists() else {}


def save(results):
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "results.json").write_text(json.dumps(results, indent=1))


def summary_cell(r):
    res = r["result"]
    if res == "state-diverged":
        dv = r["divergence"]
        fields = ",".join(list(dv["fields"])[:4])
        res += f" f={dv['at']} ({fields}{'; rng/event only' if r.get('rng_only') else ''})"
    elif res in ("hang", "port-crash"):
        res += f" ({r['port']['reason'][:60]}; last f={r['port']['last'].get('f')})"
    elif res == "gc-failed":
        res += f" ({r['gc']['reason'][:60]})"
    return res


def report(results, order):
    rows = [results[n] for n in order if n in results]
    md = ["| point | story jump | map | result | image pre-div (max mean) | image all | port end | GC end |",
          "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        pe, ge = r["port"]["last"], r["gc"]["last"]
        md.append(f"| {r['name']} ({r['title']}) | {r['storyjump']} | {r['map']} | {summary_cell(r).replace('|', '/')} | "
                  f"{r.get('image_pre')} | {r.get('image_all')} | f={pe.get('f')} {pe.get('map')} seq={pe.get('seq')} | "
                  f"f={ge.get('f')} {ge.get('map')} seq={ge.get('seq')} |")
    (OUT / "table.md").write_text("\n".join(md) + "\n")
    secs = []
    for r in rows:
        trs = []
        for s in r["shots"]:
            cells = ""
            for sub, ok in (("port", s["port"]), ("gc", s["gc"]), ("diff", "mean" in s)):
                src = f"{sub}/{r['name']}/f{s['f']:05d}.png"
                cells += f"<td><img src={src} loading=lazy></td>" if ok else "<td>-</td>"
            stat = f"f={s['f']} mean {s['mean']} max {s['max']} over {s['over']}%" if "mean" in s else f"f={s['f']} {s.get('note', 'no pair')}"
            trs.append(f"<tr>{cells}<td>{html.escape(stat)}</td></tr>")
        bad = r["result"] != "ok"
        lines = "".join(f"<li>{html.escape(x)}</li>" for x in r["port"]["lines"])
        div = r["divergence"]
        dv = (f"first difference f={div['at']}, last equal {div['last_equal']}: "
              + "; ".join(f"{k}: port {v[0]} gc {v[1]}" for k, v in div["fields"].items())) if div.get("at") else \
            f"{div.get('records')} records equal"
        secs.append(f'<section class="{"bad" if bad else ""}"><h2>{html.escape(r["name"])} - {html.escape(r["title"] or "")}'
                    f' <small>{html.escape(summary_cell(r))}</small></h2><p>{html.escape(r["switches"][:300])}</p>'
                    f'<p>{html.escape(dv)}</p><ul>{lines}</ul><table><tr><th>port (Azahar)</th><th>GameCube (Dolphin)</th>'
                    f'<th>|diff| x4</th><th></th></tr>{"".join(trs)}</table></section>')
    page = f"""<!doctype html><meta charset=utf-8><title>story sweep</title>
<style>body{{font:13px sans-serif;margin:16px}}img{{width:304px;image-rendering:pixelated}}
section{{border-top:1px solid #ccc;padding:6px 0}}.bad h2{{color:#b00}}td{{vertical-align:top}}</style>
<h1>Story sweep: {len(rows)} points</h1>{''.join(secs)}"""
    (OUT / "index.html").write_text(page)
    counts = {}
    for r in rows:
        counts[r["result"]] = counts.get(r["result"], 0) + 1
    print(f"story: {len(rows)} points: {counts}; {(OUT / 'table.md').relative_to(REPO)}, index.html")


def main():
    ap = argparse.ArgumentParser(description="Story checkpoints, the port against the GameCube.")
    ap.add_argument("--points", type=Path, default=POINTS)
    ap.add_argument("--only", default="", help="comma-separated name prefixes")
    ap.add_argument("--redo", action="store_true")
    ap.add_argument("--report", action="store_true", help="only rebuild table.md and index.html")
    ap.add_argument("--cia", type=Path, default=REPO / "build/3ds/game/ttyd.cia")
    ap.add_argument("--stall", type=float, default=120)
    ap.add_argument("--port-fps", type=float, default=8.0, help="game frames per second Azahar manages (timeouts)")
    ap.add_argument("--port-timeout-base", type=float, default=240)
    ap.add_argument("--gc-timeout", type=float, default=1800)
    ap.add_argument("--reuse-gc", action="store_true", help="reuse a Dolphin log made with exactly these switches")
    ap.add_argument("--pixel-threshold", type=int, default=32)
    ap.add_argument("--image-threshold", type=float, default=12.0,
                    help="max mean |diff| of the shots before the divergence for an ok (edge filtering and the XFB's YUV "
                    "4:2:2 round trip give about 4-8 in busy scenes with nothing wrong)")
    ap.add_argument("--no-dolphin", action="store_true", help="the port only (results then have no comparison)")
    a = ap.parse_args()

    doc = json.loads(a.points.read_text())
    d = doc["defaults"]
    pts = doc["points"]
    if a.only:
        pre = tuple(x for x in a.only.split(",") if x)
        pts = [p for p in pts if p["name"].startswith(pre)]
    order = [p["name"] for p in doc["points"]]
    results = load()
    if a.report:
        for r in results.values():  # thresholds may have changed
            r["result"] = classify(r, a)
        save(results)
        report(results, order)
        return 0
    gallery.AZ_PROFILE = REPO / "build" / "story-azahar"
    gallery.DOL_PROFILE = REPO / "build" / "story-dolphin"
    gallery.DOL_PORT = 55677
    todo = [p for p in pts if a.redo or p["name"] not in results]
    print(f"story: {len(todo)} of {len(pts)} points to run", flush=True)
    if a.no_dolphin:
        def gc_stub(*_):
            return False, "not run (--no-dolphin)", ""
        global gc_job
        gc_job = gc_stub  # noqa: F811
    with cf.ThreadPoolExecutor(1) as pool_port, cf.ThreadPoolExecutor(1) as pool_gc:
        jobs = [run_point(p, d, a, pool_port, pool_gc) for p in todo]
        for job in jobs:
            r = finish(*job, a)
            results[r["name"]] = r
            save(results)
            with LOCK:
                print(f"story: {r['name']}: {summary_cell(r)}", flush=True)
    report(results, order)
    return 0 if results else 1


if __name__ == "__main__":
    sys.exit(main())
