#!/usr/bin/env python3
"""The collector: a session folder (session.py) in, a report and doc patches out. The docs are NOT changed.

    collect.py SESSION_DIR [--out DIR]       default DIR = SESSION_DIR/results

Writes, under the results folder:
    report.md             every step's status, then the filled result tables (probe, calib, baseline, pairs, bisect,
                          hang survival, texdt, heap, retail, QTM, updater/supervisor) and what is missing
    results.json          the same numbers for tools
    patches/*.patch       unified diffs (a/docs/... b/docs/...) for `git apply`: the GPU_RULES "New 3DS" column,
                          the 60FPS calib columns, and generated "Console session" blocks for the other docs
    baseline.md           the hw_baseline-style table

Inputs it reads: SESSION_DIR/steps.json, the step folders (latest.log, bisect.json), fetched/ (gpuprobe.txt,
calib.txt, calib_pmu.txt).
"""
import argparse
import difflib
import json
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import hw_baseline  # noqa: E402

PAIRS = [  # (A, B, question) of the runbook's D1 table
    ("D", "C3", "translator thread on core 2 (gxthread=2 gxpipeline=1)"),
    ("C3", "K", "one GX_ProcessCommandList per run (gxone gxchunk)"),
    ("K", "I", "gximmfl=1"),
    ("C3", "gpucore1", "render thread on core 1"),
    ("K", "kg1", "K with the render thread on core 1"),
    ("C3", "par", "job worker 1, map name index, mapDisp job"),
    ("gpucore1", "all", "the three together"),
    ("D", "old", "the new defaults against the old"),
    ("D", "noqtm", "QTM question 2: is QTM worth it"),
    ("D", "recheap", "QTM question 3: the record in the heap"),
    ("D", "log_heap", "QTM test 4: the log in the heap"),
    ("D", "vramtex", "QTM test 5: textures in VRAM"),
    ("vramtex", "vramtex_res", "QTM test 6: vramtexreserve=256 vramtexmax=512"),
    ("vramtex", "vramfx", "QTM test 7: copy targets under the tier"),
    ("D", "qtm_idx", "QTM test 8: index arenas in QTM"),
    ("ring_base", "ring_qtm", "QTM test 9: the GX ring in QTM"),
    ("K", "frame_qtm", "native frame list in QTM"),
]


# ---------------------------------------------------------------- parsers

def parse_probe(text):
    """gpuprobe.txt -> {case: {group, danger, expected, result, ms, note}}. Fixed columns first (the C format
    is "%-14s %-9s %-4d %-32s %-6s %6d  %s"), a regex when a field overflowed."""
    out = {}
    for line in text.splitlines():
        if not line.strip() or line.startswith("case "):
            continue
        m = re.match(r"^(\S+)\s+(\S+)\s+(\d+)\s+(.*?)\s+(OK|WRONG|HANG|FAULT|ERR|SKIP|-)\s+(\d+)\s{2}(.*)$", line)
        if m:
            out[m.group(1)] = dict(group=m.group(2), danger=int(m.group(3)), expected=m.group(4).strip(), result=m.group(5),
                                   ms=int(m.group(6)), note=m.group(7).strip())
    return out


def num(s):
    return float(s.replace(",", ""))


def parse_calib(text):
    out, group = {}, ""
    for line in text.splitlines():
        g = re.match(r"^\[(.+)\]$", line.strip())
        if g:
            group = g.group(1)
            continue
        m = re.match(r"^(\S+)\s+([\d,]+) ops\s+([\d,.]+) ms\s+([\d,.]+) ns/op(?:\s+([\d,.]+) MB/s)?", line)
        if m:
            out[m.group(1)] = dict(group=group, ns=num(m.group(4)), mbs=num(m.group(5)) if m.group(5) else None)
    return out


def parse_pmu(text):
    out = {}
    for line in text.splitlines():
        m = re.match(r"^(\S+)\s+cycles/op\s+([\d,.]+)\s+dmiss/op\s+([\d,.]+)\s+imiss/op\s+([\d,.]+)", line)
        if m:
            out[m.group(1)] = dict(cycles=num(m.group(2)), dmiss=num(m.group(3)), imiss=num(m.group(4)))
    return out


def baseline_rows(rec):
    log = Path(rec.get("log", ""))
    if not log.is_file():
        return []
    m = rec.get("meta", {})
    return hw_baseline.parse(log.read_text(errors="replace"), rec["tag"], m.get("first", 781), m.get("windows", 3))


def mean(rows, k):
    return sum(r[k] for r in rows) / len(rows)


def read(rec):
    p = Path(rec.get("log", ""))
    return p.read_text(errors="replace") if rec.get("log") and p.is_file() else ""


# ---------------------------------------------------------------- tables

def md_table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def steps_table(records):
    rows = []
    for r in records:
        n = len(r.get("attempts", []))
        rows.append((r["id"], r.get("phase", ""), r["status"], n or "", (r.get("title", "") or "")[:60],
                     (r.get("why", "") or "")[:80].replace("|", "/")))
    return md_table(["step", "phase", "status", "attempts", "what", "why not PASS"], rows)


def baseline_section(records):
    res = {}  # (scene, config) -> list of row-lists (repeats)
    for r in records:
        if r.get("kind") != "baseline" or r["status"] != "PASS":
            continue
        rows = baseline_rows(r)
        if rows:
            m = r["meta"]
            res.setdefault((m["scene"], m["config"]), {})[m["repeat"]] = rows
    if not res:
        return "", {}, {}
    scenes = list(dict.fromkeys(s for s, _ in res))
    configs = list(dict.fromkeys(c for _, c in res))
    out = ["| scene | config | frame ms | core-0 busy ms | render ms | frame min-max | windows |", "|---|---|---|---|---|---|---|"]
    agg = {}
    for scene in scenes:
        for cfg in configs:
            reps = res.get((scene, cfg))
            if not reps:
                continue
            for rep, rows in sorted(reps.items()):
                fr = [x["frame"] for x in rows]
                agg[(scene, cfg, rep)] = {k: mean(rows, k) for k in ("frame", "busy", "render")}
                out.append(f"| {scene} | {cfg}{'#' + str(rep) if rep > 1 else ''} | {mean(rows, 'frame'):.1f} | "
                           f"{mean(rows, 'busy'):.1f} | {mean(rows, 'render'):.1f} | {min(fr):.1f}-{max(fr):.1f} | {len(rows)} |")
    return "\n".join(out), agg, res


def pairs_section(agg):
    if not agg:
        return ""
    noise = {}
    for (scene, cfg, rep), v in agg.items():
        if cfg == "D" and rep == 2 and (scene, "D", 1) in agg:
            noise[scene] = {k: abs(v[k] - agg[(scene, "D", 1)][k]) for k in v}
    rows = []
    for a, b, q in PAIRS:
        for key in ("frame", "busy", "render"):
            pass
        cells, outside, seen = [], 0, 0
        for scene in dict.fromkeys(s for s, _, _ in agg):
            if (scene, a, 1) in agg and (scene, b, 1) in agg:
                seen += 1
                d = {k: agg[(scene, b, 1)][k] - agg[(scene, a, 1)][k] for k in ("frame", "busy", "render")}
                nz = noise.get(scene, {}).get("busy", 0.5)
                big = abs(d["busy"]) > max(nz, 0.1) or abs(d["frame"]) > 0.5
                outside += big
                cells.append(f"{scene} frame {d['frame']:+.1f} busy {d['busy']:+.1f} render {d['render']:+.1f}{'*' if big else ''}")
        if seen:
            rows.append((f"{a} -> {b}", q, "<br>".join(cells), f"{outside} of {seen}"))
    if not rows:
        return ""
    head = "B - A in ms (frame, core-0 busy, render); * = outside the D repeat noise (busy) or frame above 0.5 ms"
    nz = ", ".join(f"{s}: busy {v['busy']:.2f} ms" for s, v in noise.items()) or "no D repeat run"
    return f"{head}\n\nNoise (D against the repeat of D): {nz}\n\n" + md_table(["pair", "question", "difference", "scenes outside noise"], rows)


def bisect_section(records, out):
    parts = []
    for r in records:
        if r.get("type") != "bisect":
            continue
        p = Path(r["dir"]) / "bisect.json"
        if not p.is_file():
            continue
        b = json.loads(p.read_text())
        out["bisect"] = {k: b[k] for k in ("status", "culprit", "confirmed", "runs")}
        lines = [f"Status: **{b['status']}**" + (f"; culprit draw **{b['culprit']}**" +
                 (" (confirmed: skipping it removes the hang)" if b["confirmed"] else " (NOT confirmed)") if b["culprit"] is not None else ""),
                 f"Runs: {b['runs']}", ""]
        lines.append(md_table(["run", "switch", "result", "hang lines"],
                              [(t["label"], t["extra"] or "(full record)", "HANG" if t["hang"] else "no hang", t["hang_lines"]) for t in b["table"]]))
        parts.append("\n".join(lines))
    return "\n\n".join(parts)


def hangsurv_section(records, out):
    lines = []
    allText = "\n".join(read(r) for r in records)
    m = re.search(r"gpu: start-up check[^\n]*", allText)
    lines.append(f"- B1 start-up check: {m.group(0) if m else 'no `gpu: start-up check` line in any collected log'}")
    for r in records:
        if r["id"] in ("B2", "B3", "B3r"):
            txt = read(r)
            keys = [x for x in re.findall(r"gpu: [^\n]*(?:reset #\d+ done|no more resets|marked dirty)[^\n]*", txt)][:4]
            lines.append(f"- {r['id']}: **{r['status']}** {r.get('why', '')}" + ("".join(f"\n  - `{k[:150]}`" for k in keys)))
    out["hangsurv"] = lines
    return "\n".join(lines)


def grep_lines(records, kinds, rx, limit=40):
    out = []
    for r in records:
        if r.get("kind") in kinds:
            for ln in read(r).splitlines():
                if re.search(rx, ln):
                    out.append((r["id"], ln.strip()))
    return out[:limit]


def retail_section(records, out):
    cells = {}
    for r in records:
        if r.get("kind") != "retail" or r["status"] == "SKIP":
            continue
        txt = read(r)
        hits = re.findall(r"bench (\d+)-(\d+): frame (\d+) busy (\d+) render (\d+) frames (\d+) vblanks (\d+)", txt)
        m = r["meta"]
        if hits:
            h = hits[-1]
            stale = "" if f"hwb={r.get('tag', '')}" in txt else " (log carries no tag of this run: may be the previous run's)"
            cells[(m["scene"], m["cell"])] = (int(h[2]), int(h[3]), int(h[4]), stale)
        else:
            cells[(m["scene"], m["cell"])] = None
    if not cells:
        return ""
    rows = []
    for scene in dict.fromkeys(s for s, _ in cells):
        row = [scene]
        for c in (1, 2, 3):
            v = cells.get((scene, c))
            row.append("-" if not v else f"frame {v[0]} us, busy {v[1]} us, render {v[2]} us{v[3]}")
        rows.append(row)
    out["retail"] = {f"{s}/{c}": v for (s, c), v in cells.items()}
    return md_table(["scene", "cell 1: release, retail=0", "cell 2: runtime retail=1", "cell 3: TTYD_RETAIL build"], rows)


# ---------------------------------------------------------------- doc patches

def diff_patch(rel, old, new):
    return "".join(difflib.unified_diff(old.splitlines(True), new.splitlines(True), f"a/{rel}", f"b/{rel}"))


def fill_table(text, header_re, key_col, fills):
    """Fills cells of the first table whose header matches header_re. fills: {key: {col: value}}, empty cells only."""
    lines = text.splitlines(True)
    inside = False
    for i, ln in enumerate(lines):
        if not ln.startswith("|"):
            inside = False if inside else inside
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        if not inside:
            if re.search(header_re, ln):
                inside = True
            continue
        key = cells[key_col].strip("`") if key_col < len(cells) else ""
        if key in fills:
            for col, val in fills[key].items():
                if col < len(cells) and not cells[col]:
                    cells[col] = val.replace("|", "/")
            lines[i] = "| " + " | ".join(cells) + " |\n"
    return "".join(lines)


def probe_patch(root, probe, extra_lines):
    rel = "docs/PORT_3DS_GPU_RULES.md"
    text = (root / rel).read_text()
    fills = {c: {5: f"{v['result']}" + (f": {v['note']}" if v["note"] else "")[:140]} for c, v in probe.items() if v["result"] != "-"}
    new = fill_table(text, r"\| Case \| Group \| Expected \| Azahar \| Danger \| New 3DS", 0, fills)
    if extra_lines:
        new += "\n### Console log lines (generated by collect.py)\n\n```\n" + "\n".join(extra_lines) + "\n```\n"
    return rel, text, new


def calib_patch(root, calib):
    rel = "docs/PORT_3DS_60FPS.md"
    text = (root / rel).read_text()
    fills = {}
    for name, v in calib.items():
        fills[name] = {4: f"{v['ns']:,.2f}", 5: f"{v['mbs']:,.2f}" if v["mbs"] else ""}
    # ratio needs the Azahar cell: second pass over the table
    lines = text.splitlines(True)
    inside = False
    for i, ln in enumerate(lines):
        if not ln.startswith("|"):
            inside = False
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        if not inside:
            inside = "New 3DS ns/op" in ln and "ratio" in ln
            continue
        key = cells[1].strip("`") if len(cells) > 6 else ""
        if key in calib and len(cells) >= 7:
            v = calib[key]
            if not cells[4]:
                cells[4] = f"{v['ns']:,.2f}"
            if not cells[5] and v["mbs"]:
                cells[5] = f"{v['mbs']:,.2f}"
            if not cells[6] and cells[2]:
                try:
                    cells[6] = f"{v['ns'] / num(cells[2]):.2f}"
                except (ValueError, ZeroDivisionError):
                    pass
            lines[i] = "| " + " | ".join(cells) + " |\n"
    return rel, text, "".join(lines)


def append_patch(root, rel, block):
    text = (root / rel).read_text()
    stamp = time.strftime("%Y-%m-%d")
    new = text.rstrip("\n") + f"\n\n## Console session {stamp} (generated by tools/port3ds/console_session/collect.py)\n\n" \
        "Move each part into the section the runbook names, then delete this heading.\n\n" + block.strip("\n") + "\n"
    return rel, text, new


# ---------------------------------------------------------------- main

def collect(session, results, doc, root):
    session, results, root = Path(session), Path(results), Path(root)
    results.mkdir(parents=True, exist_ok=True)
    (results / "patches").mkdir(exist_ok=True)
    records = json.loads((session / "steps.json").read_text())
    data = {"steps": {r["id"]: r["status"] for r in records}}
    md = [f"# Console session report ({time.strftime('%Y-%m-%d %H:%M')})", "", f"Session folder: `{session}`", "",
          "## Steps", "", steps_table(records), ""]
    manual = [(r["id"], r["manual"]) for r in records if r.get("manual") and r["status"] != "SKIP"]
    patches = {}

    # probe
    pt = session / "fetched/gpuprobe.txt"
    probe = parse_probe(pt.read_text(errors="replace")) if pt.is_file() else {}
    if probe:
        data["probe"] = probe
        cnt = {}
        for v in probe.values():
            cnt[v["result"]] = cnt.get(v["result"], 0) + 1
        loglines = [ln for _, ln in grep_lines(records, ("probe",), r"gpuprobe: (cp_thru|dt )")]
        md += ["## gpuprobe (PORT_3DS_GPU_RULES.md, \"New 3DS\" column)", "", f"{len(probe)} cases: " +
               ", ".join(f"{k} {v}" for k, v in sorted(cnt.items())), "",
               md_table(["case", "group", "danger", "expected", "console", "note"],
                        [(c, v["group"], v["danger"], v["expected"], v["result"], v["note"][:90].replace("|", "/"))
                         for c, v in probe.items() if v["result"] != "-"]), ""]
        if loglines:
            md += ["Log lines:", "", "```", *loglines[:30], "```", ""]
        patches["gpu_rules"] = probe_patch(root, probe, loglines[:30])
    else:
        md += ["## gpuprobe", "", "No gpuprobe.txt fetched.", ""]

    # calib
    ct = session / "fetched/calib.txt"
    calib = parse_calib(ct.read_text(errors="replace")) if ct.is_file() else {}
    pmu = parse_pmu((session / "fetched/calib_pmu.txt").read_text(errors="replace")) if (session / "fetched/calib_pmu.txt").is_file() else {}
    if calib:
        data["calib"] = calib
        flat = [n for n in ("read_seq_16K", "read_seq_256K", "read_seq_1M", "read_seq_16M") if n in calib]
        warn = ""
        if len(flat) == 4 and len({round(calib[n]["ns"], 1) for n in flat}) == 1:
            warn += "\n**WARNING: the read rows are flat: the timer or the cache is wrong.**"
        if "branch_random_16K" in calib and "branch_sorted_16K" in calib and calib["branch_random_16K"]["ns"] <= calib["branch_sorted_16K"]["ns"]:
            warn += "\n**WARNING: branch_random_16K is not slower than branch_sorted_16K.**"
        md += ["## calib (PORT_3DS_60FPS.md 2a)", "", f"{len(calib)} rows; {len(pmu)} rows with ARM11 counters." + warn, "",
               md_table(["group", "benchmark", "ns/op", "MB/s", "cycles/op", "dmiss/op", "imiss/op"],
                        [(v["group"], n, f"{v['ns']:,.2f}", f"{v['mbs']:,.2f}" if v["mbs"] else "",
                          pmu[n]["cycles"] if n in pmu else "", pmu[n]["dmiss"] if n in pmu else "", pmu[n]["imiss"] if n in pmu else "")
                         for n, v in calib.items()]), ""]
        patches["60fps"] = calib_patch(root, calib)
    else:
        md += ["## calib", "", "No calib.txt fetched.", ""]

    # baseline and pairs
    table, agg, _ = baseline_section(records)
    if table:
        data["baseline"] = {f"{s}/{c}#{r}": v for (s, c, r), v in agg.items()}
        (results / "baseline.md").write_text(table + "\n")
        md += ["## hw_baseline and A/B pairs (PORT_3DS_GX_PERF.md, PORT_3DS_60FPS.md 2c, PORT_3DS_GAMEPAR.md, PORT_3DS_QTM.md 9)", "", table, ""]
        pr = pairs_section(agg)
        if pr:
            md += ["### Pairs", "", pr, ""]
        patches["gx_perf"] = ("docs/PORT_3DS_GX_PERF.md", table, pr)
    else:
        md += ["## hw_baseline", "", "No baseline run passed.", ""]

    # hang survival and bisect
    hs = hangsurv_section(records, data)
    md += ["## GPU hang survival (PORT_3DS_TESTING.md)", "", hs, ""]
    bs = bisect_section(records, data)
    md += ["## gor_01 hang bisect (PORT_3DS_TESTING.md \"GPU hang\", owner-questions 4b)", "", bs or "Not run.", ""]

    # verify, texdt, heap, loadprof
    for title, kinds, rx in (("verify modes", ("verify",), r"differ \d+ records over|mismatches \d+ missed"),
                             ("texdt verify", ("texdt",), r"texdt: (made|verify)"),
                             ("texdt map-load cost", ("texdt_cost",), r"\[LOADPROF\]|texframe:"),
                             ("job round trip", ("jobprobe",), r"job|wake"),
                             ("heap", ("heap",), r"heap")):
        ls = grep_lines(records, kinds, rx, 14)
        sts = ", ".join(f"{r['id']} {r['status']}" for r in records if r.get("kind") in kinds)
        md += [f"## {title}", "", sts or "Not run.", ""]
        if ls:
            md += ["```", *[f"{i}: {ln[:200]}" for i, ln in ls], "```", ""]
        data.setdefault("lines", {})[title] = ls
    # retail
    rt = retail_section(records, data)
    md += ["## Retail (PORT_3DS_TESTING.md \"Retail mode\")", "", rt or "Not run.", ""]
    # QTM
    q = [r for r in records if r.get("phase") == "G"]
    md += ["## QTM gates", "", md_table(["step", "status", "why"], [(r["id"], r["status"], r.get("why", "")[:90]) for r in q]) if q else "Not run.", ""]
    # updater / supervisor
    us = [r for r in records if r.get("kind") in ("updater", "detcia", "supervisor", "restore")]
    md += ["## Updater, deterministic CIA, supervisor (PORT_3DS_TESTING.md, CTR_DEVKIT_PLAN.md H1-H6)", "",
           md_table(["step", "status", "detail"], [(r["id"], r["status"], (r.get("why") or r.get("detail", ""))[:110].replace("|", "/")) for r in us]) if us else "Not run.", ""]
    md += ["## Still manual (the owner looks at the console)", ""] + ([f"- {i}: {t}" for i, t in manual] or ["- none recorded"]) + [""]

    # patches
    for key, (rel, old, new) in patches.items():
        if key == "gx_perf":
            text = f"### hw_baseline\n\n{old}\n" + (f"\n### Pairs\n\n{new}\n" if new else "")
            rel, old, new = append_patch(root, rel, text)
        if new != old:
            (results / "patches" / f"{Path(rel).stem}.patch").write_text(diff_patch(rel, old, new))
    extra = []
    if bs:
        extra.append(("docs/PORT_3DS_TESTING.md", "### GPU hang survival\n\n" + hs + "\n\n### gor_01 hang bisect\n\n" + bs + (
            "\n\n### Retail mode\n\n" + rt if rt else "")))
    if q:
        extra.append(("docs/PORT_3DS_QTM.md", "### Section 9 console rows\n\n" + (pairs_section(agg) or "(no pair data)")))
    if us:
        extra.append(("docs/CTR_DEVKIT_PLAN.md", "### Console test results H1-H6\n\n" + md_table(
            ["step", "status", "detail"], [(r["id"], r["status"], (r.get("why") or r.get("detail", ""))[:110].replace("|", "/")) for r in us])))
    for rel, block in extra:
        r_, old, new = append_patch(root, rel, block)
        (results / "patches" / f"{Path(rel).stem}.patch").write_text(diff_patch(r_, old, new))
    (results / "report.md").write_text("\n".join(md) + "\n")
    (results / "results.json").write_text(json.dumps(data, indent=1, default=str))
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("session")
    ap.add_argument("--out")
    ap.add_argument("--manifest", default=str(HERE / "manifest.json"))
    a = ap.parse_args()
    root = HERE.parents[2]
    doc = json.loads(Path(a.manifest).read_text())
    res = collect(a.session, a.out or Path(a.session) / "results", doc, root)
    print(f"collect: {res}/report.md, patches in {res}/patches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
