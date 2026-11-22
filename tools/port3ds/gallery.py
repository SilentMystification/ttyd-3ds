#!/usr/bin/env python3
"""Screenshot galleries, the 3DS port against the GameCube debug DOL (backlog A5).

Effects (fx=) on a quiet map, several per boot:

    tools/port3ds/gallery.py fx --items 0-80,n64:0-102 --name fx_all
    tools/port3ds/gallery.py fx --items kemuri,n64:32 --map gor_01 --offsets 6,20

Battles (battle=), a list per boot, shots at offsets after each start:

    tools/port3ds/gallery.py battle --map hei_01 --items 0-3 --mode perfect --name hei

Shots at fixed game frames of one run (no fx=/battle=):

    tools/port3ds/gallery.py frames --switches "map=gon_02 story=30" --frames 648,656,664 --name gon02

Rebuild the report from images already taken:

    tools/port3ds/gallery.py report --name fx_all

How it works. Both builds run with the same switches plus "determ", so the
same game frames show the same scene (docs/PORT_3DS_TEST_INFRA.md). The test
switches fxshots=/btlshots=/shotat= make the port save the EFB of the frame
(gallery/fNNNNN.ppm on its SD card, 608x480) and make the debug DOL stop at
tst_shot_point, where run_dolphin.py --shots reads the XFB (608x480 YUYV, the
EFB copied 1:1 with no copy filter). So the images align pixel for pixel with
no cropping or scaling; the only conversion is the GameCube's YUV 4:2:2 back
to RGB (BT.601 studio range), which costs about 1-2 levels of colour and
half the horizontal chroma resolution. The port runs in Azahar through
run_azahar.py (slot-locked, headless) in its own profile, the debug DOL in
Dolphin through run_dolphin.py in its own profile and port, one at a time.

Per pair of images: mean and max absolute difference (largest channel, 0-255)
and the share of pixels over --pixel-threshold. An item's score is its worst
shot's mean. The report (build/gallery/<name>/index.html) lists the items by
score, side by side with an amplified difference image; items over
--review-threshold are marked for review. Azahar sometimes saves an all-black
shot of a frame that drew normally: such a shot is marked and left out of the
score. results.json holds everything, including each item's log lines (spawn,
alive checks, warnings, crashes).

Exit status: 0 done (differences are reported, not failures), 1 a run failed
in a way that left no results, 2 usage error.
"""

import argparse
import html
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
OUT_ROOT = REPO / "build" / "gallery"
AZ_PROFILE = REPO / "build" / "gallery-azahar"
DOL_PROFILE = REPO / "build" / "gallery-dolphin"
DOL_PORT = 55631
WARN = re.compile(r"(TEV: .*approximated.*|GX: .*|\[CRASH\].*|determ: .*busy.*|.*[Ff]ault.*|.*abort.*)$")


def load_image(path):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int16)


def uniform(a):
    return int(a.max()) == int(a.min())


def diff_stats(a, b, threshold):
    import numpy as np
    d = np.abs(a - b).max(axis=-1)
    return {"mean": round(float(d.mean()), 2), "max": int(d.max()), "over": round(float((d > threshold).mean() * 100), 2)}, d


def save_diff(d, path):
    import numpy as np
    from PIL import Image
    Image.fromarray(np.clip(d * 4, 0, 255).astype(np.uint8), "L").save(path)


def parse_list(text):
    """'0-3,kemuri,n64:5-7' -> ['0','1','2','3','kemuri','n64:5','n64:6','n64:7']"""
    out = []
    for word in filter(None, text.split(",")):
        pre = "n64:" if word.startswith("n64:") else ""
        body = word[len(pre):]
        m = re.fullmatch(r"(\d+)-(\d+)", body)
        if m:
            out += [f"{pre}{i}" for i in range(int(m.group(1)), int(m.group(2)) + 1)]
        else:
            out.append(word)
    return out


def ensure_azahar_profile():
    sd = AZ_PROFILE / "data" / "azahar-emu" / "sdmc" / "3ds" / "ttyd"
    sd.mkdir(parents=True, exist_ok=True)
    iso = sd / "game.iso"
    if not iso.exists():
        iso.hardlink_to(REPO / "iso" / "game.iso")
    return sd


def run_port(args, switches, until, timeout, last):
    """One Azahar run; returns (passed, reason, log text). Shots land in <SD>/3ds/ttyd/gallery/."""
    sd = ensure_azahar_profile()
    shutil.rmtree(sd / "gallery", ignore_errors=True)
    cmd = [sys.executable, str(REPO / "tools/port3ds/run_azahar.py"), "--build", str(args.cia), "--profile",
           str(AZ_PROFILE), "--last", str(last), "--switches", switches, "--until", until, "--timeout", str(timeout),
           "--stall", str(args.stall)]
    print("gallery: port:", switches[:200], flush=True)
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    reason = (r.stdout.strip().splitlines() or ["?"])[-1]
    log = sd / "log.txt"
    text = log.read_text(errors="replace") if log.exists() else ""
    (last / "game_log.txt").parent.mkdir(parents=True, exist_ok=True)
    (last / "game_log.txt").write_text(text)
    return r.returncode == 0, reason, text


def collect_port_shots(dst):
    from PIL import Image
    src = AZ_PROFILE / "data" / "azahar-emu" / "sdmc" / "3ds" / "ttyd" / "gallery"
    dst.mkdir(parents=True, exist_ok=True)
    n = 0
    for p in sorted(src.glob("f*.ppm")):
        Image.open(p).save(dst / (p.stem + ".png"))
        n += 1
    return n


def run_gc(args, switches, stopframe, shots, last):
    """One Dolphin run of the debug DOL; returns (ok, reason, log text)."""
    last.mkdir(parents=True, exist_ok=True)
    words = switches.split()
    if "fastboot" not in words:
        words.append("fastboot")
    cmd = [sys.executable, "-u", str(REPO / "tools/port3ds/run_dolphin.py"), "--gcdebug", "--profile", str(DOL_PROFILE),
           "--port", str(DOL_PORT), "--switches", " ".join(words + [f"stopframe={stopframe}"]), "--break",
           "gcdbg_stop", "--read-log", "--timeout", str(args.gc_timeout), "--json", str(last / "dolphin.json")]
    if shots is not None:
        cmd += ["--shots", str(shots)]
    print(f"gallery: dolphin: to frame {stopframe}", flush=True)
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    (last / "dolphin_out.txt").write_text(r.stdout + r.stderr)
    text = ""
    if (last / "dolphin.json").exists():
        text = json.loads((last / "dolphin.json").read_text()).get("log", "")
    (last / "gc_log.txt").write_text(text)
    reason = (r.stdout.strip().splitlines() or ["?"])[-1]
    return r.returncode == 0, reason, text


def strip(line):
    return re.sub(r"^#\d+ ", "", line)


# ---- fx ----

def parse_fx(text, batch):
    """Per item of the batch, in order: spawn frame, the alive checks, warnings until the next spawn."""
    res, cur, i = {}, None, 0
    for raw in text.splitlines():
        line = strip(raw)
        m = re.match(r"fx: (\S+) is not an effect set name or index", line)
        if m and i < len(batch):
            res[batch[i]] = {"status": "not an effect", "lines": [line]}
            i += 1
            continue
        m = re.match(r"fx: (\S+) at frame (\d+) .*event (-?\d+)", line)
        if m and i < len(batch):
            cur = batch[i]
            i += 1
            res[cur] = {"status": "spawned", "set": m.group(1), "spawn": int(m.group(2)), "event": int(m.group(3)),
                        "lines": [line], "warnings": []}
            continue
        if cur is None:
            continue
        r = res[cur]
        m = re.match(r"fx: \S+ 2 frames after the spawn: (.*)$", line)
        if m:
            r["alive2"] = m.group(1)
            r["lines"].append(line)
            continue
        m = re.match(r"fx: \S+ after (\d+) frames: (.*)$", line)
        if m:
            r["end"] = m.group(2)
            r["lines"].append(line)
            continue
        if WARN.search(line):
            r["warnings"].append(line)
        if "[CRASH]" in line:
            r["status"] = "crash"
    return res


def cmd_fx(args):
    out = OUT_ROOT / args.name
    items = parse_list(args.items)
    offsets = [int(o) for o in args.offsets.split(",")]
    results = load_results(out)
    todo = [it for it in items if it not in results.get("items", {}) or args.redo]
    results.setdefault("items", {})
    results["kind"] = "fx"
    results["map"] = args.map
    bn = 0
    while todo:
        batch = todo[:args.batch]
        bn += 1
        tag = f"b{len(results.get('batches', [])) + 1:02d}"
        sw = (f"map={args.map} story={args.story} determ fx={','.join(batch)} fxgap={args.gap} "
              f"fxshots={','.join(map(str, offsets))} {args.extra}").strip()
        frames_needed = 700 + args.wait + len(batch) * args.gap + 60
        ok, reason, ptext = run_port(args, sw, r"fx: done", args.port_timeout_base + frames_needed / args.port_fps,
                                     out / "logs" / f"port_{tag}")
        collect_port_shots(out / "port")
        pres = parse_fx(ptext, batch)
        done = [it for it in batch if it in pres]
        if not ok:
            # the last spawned item is the suspect: it crashed or hung the game
            last = next((it for it in reversed(batch) if it in pres and pres[it]["status"] == "spawned"), None)
            if last is not None:
                pres[last]["status"] = "crash" if "CRASH" in ptext or "CRASH" in reason else "stopped"
                pres[last]["lines"].append(f"run: {reason}")
                pres[last]["lines"] += [strip(l) for l in ptext.splitlines()[-12:]]
            elif batch:
                # nothing spawned: count the first as the cause so the sweep moves on
                pres[batch[0]] = {"status": "stopped", "lines": [f"run: {reason}"], "warnings": []}
                done = [batch[0]]
        gres = {}
        if args.dolphin and done:
            spawned = [pres[it]["spawn"] for it in done if "spawn" in pres[it]]
            stop = (max(spawned) if spawned else 800) + max(offsets) + 5
            gok, greason, gtext = run_gc(args, sw, stop, out / "gc", out / "logs" / f"gc_{tag}")
            gres = parse_fx(gtext, batch)
            if not gok:
                print(f"gallery: dolphin: {greason}")
        for it in done:
            r = pres[it]
            r["gc"] = gres.get(it)
            r["shots"] = [r["spawn"] + o for o in offsets] if "spawn" in r else []
            r["gc_shots"] = ([gres[it]["spawn"] + o for o in offsets] if gres.get(it) and "spawn" in gres[it]
                             else r["shots"])
            results["items"][it] = r
        results.setdefault("batches", []).append({"tag": tag, "items": batch, "port": reason, "done": len(done)})
        save_results(out, results)
        todo = [it for it in todo if it not in done]
        if not done:
            print("gallery: a batch made no progress, stopping")
            break
    report(out, results, args)
    return 0


# ---- battle ----

def parse_battles(text):
    """battle: start / end lines and the btlauto lines between them, per battle id."""
    res, cur = {}, None
    for raw in text.splitlines():
        line = strip(raw)
        m = re.match(r"battle: start (\d+) \((.*?)\) on (\S+) at frame (\d+)", line)
        if m:
            cur = m.group(1)
            res[cur] = {"name": m.group(2), "map": m.group(3), "start": int(m.group(4)), "turns": 0, "acs": [],
                        "lines": [line], "warnings": []}
            continue
        m = re.match(r"battle: (\S+) is not in area|battle: setup (\d+) .*chose no group|battle: no NPC", line)
        if m:
            key = m.group(1) or m.group(2) or "?"
            res.setdefault(key, {"lines": [], "warnings": []})["status"] = line
            continue
        if cur is None:
            continue
        r = res[cur]
        m = re.match(r"battle: end result (-?\d+) after (\d+) frames", line)
        if m:
            r["result"], r["frames"] = int(m.group(1)), int(m.group(2))
            r["lines"].append(line)
            cur = None
            continue
        if line.startswith("btlauto: turn "):
            r["turns"] += 1
        m = re.match(r"btlauto: action command ends at frame \d+, result (-?\d+)", line)
        if m:
            r["acs"].append(int(m.group(1)))
        if WARN.search(line):
            r["warnings"].append(line)
    return res


def cmd_battle(args):
    out = OUT_ROOT / args.name
    items = parse_list(args.items)
    offsets = [int(o) for o in args.offsets.split(",")] if args.offsets else []
    results = load_results(out)
    results.setdefault("items", {})
    results["kind"] = "battle"
    todo = [it for it in items if it not in results["items"] or args.redo]
    while todo:
        batch = todo[:args.batch]
        tag = f"b{len(results.get('batches', [])) + 1:02d}"
        sw = (f"map={args.map} story={args.story} determ battle={','.join(batch)} btlauto={args.mode} "
              + (f"btlshots={','.join(map(str, offsets))} " if offsets else "")
              + (f"gwdump={args.gwdump} " if args.gwdump else "") + args.extra).strip()
        until = r"battle: (done|end result)" if len(batch) == 1 else r"battle: done"
        ok, reason, ptext = run_port(args, sw, until, args.port_timeout_base + len(batch) * args.battle_frames
                                     / args.port_fps, out / "logs" / f"port_{tag}")
        if offsets:
            collect_port_shots(out / "port")
        pres = parse_battles(ptext)
        done = []
        for it in batch:
            r = pres.get(it)
            if r is None:
                continue
            if "start" in r and "result" not in r:
                r["status"] = "crash" if "[CRASH]" in ptext else f"no end ({reason})"
                r["lines"] += [strip(l) for l in ptext.splitlines()[-10:]]
            elif "result" in r:
                r["status"] = "ended"
            r["shots"] = [r["start"] + o for o in offsets] if "start" in r else []
            results["items"][it] = r
            done.append(it)
            if r.get("status", "").startswith(("crash", "no end")):
                break
        if args.dolphin and done:
            starts = [pres[it]["start"] for it in done if "start" in pres[it]]
            ends = [pres[it]["start"] + pres[it].get("frames", 0) for it in done if "start" in pres[it]]
            stop = max(ends + [s + max(offsets or [0]) for s in starts] + [800]) + 10
            gok, greason, gtext = run_gc(args, sw, stop, (out / "gc") if offsets else None, out / "logs" / f"gc_{tag}")
            gres = parse_battles(gtext)
            for it in done:
                results["items"][it]["gc"] = gres.get(it)
                if gres.get(it) and "start" in gres[it]:
                    results["items"][it]["gc_shots"] = [gres[it]["start"] + o for o in offsets]
        results.setdefault("batches", []).append({"tag": tag, "items": batch, "port": reason, "done": len(done)})
        save_results(out, results)
        if not done:
            for it in batch[:1]:
                results["items"][it] = {"status": f"not reached ({reason})", "lines": [], "warnings": []}
            done = batch[:1]
            save_results(out, results)
        todo = [it for it in todo if it not in done]
    if offsets:
        report(out, results, args)
    return 0


# ---- frames ----

def cmd_frames(args):
    out = OUT_ROOT / args.name
    frames = [int(f) for f in args.frames.split(",")]
    sw = f"{args.switches} determ shotat={','.join(map(str, frames))} {args.extra}".strip()
    ok, reason, ptext = run_port(args, sw, rf"shot: f={max(frames)} ",
                                 args.port_timeout_base + max(frames) / args.port_fps, out / "logs" / "port")
    collect_port_shots(out / "port")
    if args.dolphin:
        run_gc(args, sw, max(frames) + 2, out / "gc", out / "logs" / "gc")
    results = {"kind": "frames", "items": {f"f{f}": {"status": reason, "shots": [f], "lines": [], "warnings": []}
                                            for f in frames}}
    save_results(out, results)
    report(out, results, args)
    return 0 if ok else 1


# ---- results and report ----

def load_results(out):
    p = out / "results.json"
    return json.loads(p.read_text()) if p.exists() else {}


def save_results(out, results):
    out.mkdir(parents=True, exist_ok=True)
    (out / "results.json").write_text(json.dumps(results, indent=1))


def score_items(out, results, args):
    for it, r in results["items"].items():
        pairs = []
        for i, f in enumerate(r.get("shots", [])):
            g = (r.get("gc_shots") or r["shots"])[i]
            pp, gp = out / "port" / f"f{f:05d}.png", out / "gc" / f"f{g:05d}.png"
            entry = {"port": pp.name if pp.exists() else None, "gc": gp.name if gp.exists() else None}
            if pp.exists() and gp.exists():
                a, b = load_image(pp), load_image(gp)
                if uniform(a):
                    entry["note"] = "port shot is one colour (Azahar's black shots)"
                else:
                    st, d = diff_stats(a, b, args.pixel_threshold)
                    entry.update(st)
                    (out / "diff").mkdir(exist_ok=True)
                    dp = out / "diff" / f"{it.replace(':', '_')}_{i}.png"
                    save_diff(d, dp)
                    entry["diff"] = dp.name
            pairs.append(entry)
        r["pairs"] = pairs
        means = [p["mean"] for p in pairs if "mean" in p]
        r["score"] = max(means) if means else None


def report(out, results, args):
    score_items(out, results, args)
    save_results(out, results)
    items = sorted(results["items"].items(), key=lambda kv: -(kv[1].get("score") or -1))
    review = [it for it, r in items if (r.get("score") or 0) > args.review_threshold]
    rows = []
    for it, r in items:
        imgs = []
        for p in r.get("pairs", []):
            cells = "".join(f'<td>{f"<img src={sub}/{p[k]} loading=lazy>" if p.get(k) else "-"}</td>'
                            for k, sub in (("port", "port"), ("gc", "gc"), ("diff", "diff")))
            stat = (f"mean {p['mean']} max {p['max']} over {p['over']}%" if "mean" in p else p.get("note", "no pair"))
            imgs.append(f"<tr>{cells}<td>{html.escape(stat)}</td></tr>")
        info = [r.get("status", ""), f"spawn {r['spawn']}" if "spawn" in r else "",
                f"alive+2: {r['alive2']}" if "alive2" in r else "", f"end: {r['end']}" if "end" in r else ""]
        warn = "".join(f"<li>{html.escape(w)}</li>" for w in r.get("warnings", []))
        flag = " review" if it in review else ""
        rows.append(f'<section class="item{flag}"><h2>{html.escape(it)} {html.escape(r.get("set", r.get("name", "")))}'
                    f' <small>score {r.get("score")}</small></h2><p>{html.escape(" | ".join(x for x in info if x))}</p>'
                    f'<ul>{warn}</ul><table><tr><th>port (Azahar)</th><th>GameCube (Dolphin)</th>'
                    f'<th>|diff| x4</th><th></th></tr>{"".join(imgs)}</table></section>')
    page = f"""<!doctype html><meta charset=utf-8><title>gallery {html.escape(args.name)}</title>
<style>body{{font:13px sans-serif;margin:16px;background:#fff;color:#111}}img{{width:304px;image-rendering:pixelated}}
section{{border-top:1px solid #ccc;padding:6px 0}}.review h2{{color:#b00}}td{{vertical-align:top}}</style>
<h1>Gallery {html.escape(args.name)} ({results.get('kind')})</h1>
<p>{len(items)} items, {len(review)} over the review threshold {args.review_threshold} (mean |diff|, 0-255).
Sorted by score (the worst shot's mean difference). Images are 608x480: the port's EFB and the GameCube's XFB.</p>
{''.join(rows)}"""
    (out / "index.html").write_text(page)
    print(f"gallery: {len(items)} items, {len(review)} for review; report {out.relative_to(REPO)}/index.html")


def main():
    ap = argparse.ArgumentParser(description="Port vs GameCube screenshot galleries.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--name", required=True, help="output folder build/gallery/<name>/")
        p.add_argument("--cia", type=Path, default=REPO / "build/3ds/game/ttyd.cia")
        p.add_argument("--story", type=int, default=30)
        p.add_argument("--extra", default="", help="more switches for both builds")
        p.add_argument("--no-dolphin", dest="dolphin", action="store_false", help="the port only")
        p.add_argument("--stall", type=float, default=120)
        p.add_argument("--port-fps", type=float, default=12.0, help="game frames per second Azahar manages (timeouts)")
        p.add_argument("--port-timeout-base", type=float, default=120)
        p.add_argument("--gc-timeout", type=float, default=1800)
        p.add_argument("--pixel-threshold", type=int, default=32)
        p.add_argument("--review-threshold", type=float, default=8.0)
        p.add_argument("--redo", action="store_true", help="run items already in results.json again")

    p = sub.add_parser("fx")
    common(p)
    p.add_argument("--items", required=True, help="effect names or indices, ranges allowed: 0-80,n64:0-102")
    p.add_argument("--map", default="gor_01")
    p.add_argument("--batch", type=int, default=24, help="effects per boot")
    p.add_argument("--gap", type=int, default=60, help="fxgap: frames between spawns")
    p.add_argument("--wait", type=int, default=60)
    p.add_argument("--offsets", default="6,20", help="shot frames after each spawn")
    p.set_defaults(func=cmd_fx)

    p = sub.add_parser("battle")
    common(p)
    p.add_argument("--items", required=True, help="battle setup ids (or names) of the map's area, ranges allowed")
    p.add_argument("--map", required=True)
    p.add_argument("--mode", default="perfect", choices=["perfect", "fail"])
    p.add_argument("--batch", type=int, default=8, help="battles per boot")
    p.add_argument("--offsets", default="", help="shot frames after each battle start")
    p.add_argument("--gwdump", type=int, default=0)
    p.add_argument("--battle-frames", type=float, default=4000, help="frames to allow per battle (timeouts)")
    p.set_defaults(func=cmd_battle)

    p = sub.add_parser("frames")
    common(p)
    p.add_argument("--switches", required=True)
    p.add_argument("--frames", required=True, help="game frames to shoot")
    p.set_defaults(func=cmd_frames)

    p = sub.add_parser("report")
    p.add_argument("--name", required=True)
    p.add_argument("--pixel-threshold", type=int, default=32)
    p.add_argument("--review-threshold", type=float, default=8.0)
    p.set_defaults(func=lambda a: report(OUT_ROOT / a.name, load_results(OUT_ROOT / a.name), a) or 0)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
