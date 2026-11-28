#!/usr/bin/env python3
"""The unattended console session (docs/PORT_3DS_CONSOLE_RUNBOOK.md, "Unattended session").

    session.py plan   [--only A,B,C]                       print the steps; touches nothing
    session.py run    --ip 192.168.8.221 --owner-go ...    run the manifest (the owner gates this: see below)
    session.py run    --ip 127.0.0.1 --port 5060 --sv-port 5061 --dry azahar    the dry run against Azahar + azsv.py
    session.py export-loop OUT.json                        the static run steps as a plain `devupdate.py loop` manifest
    session.py prep                                        the PC builds the session needs (commands only; --build runs them)

Every run step is one `devupdate.py loop` entry: the supervisor system module pushes, launches, watches the
heartbeat and fetches files; this driver adds what `loop` does not do: retries with switch changes, the
recovery policy, result checks (expect / fail_on), automatic searches (the gpudrawcut bisect), gates on earlier
results (QTM rows on the cb_qtm probe) and the hand-off to collect.py.

A run needs the owner's go for any address except loopback: pass --owner-go. Without it the driver refuses.

Recovery policy, per step:
  1. attempt fails (push/launch/no start/timeout/crash/ended, or an expect/fail_on check fails);
  2. `supervisor terminate --gpureset 2` (PM terminate, GPU reset), then the next attempt (the loop entry
     launches the game again; retry_drop removes switch words such as probefresh on attempt 2+);
  3. after the last attempt the step is recorded FAIL and the session goes on with the next step;
  4. when the supervisor stops answering (loop exit 2) the driver waits for it up to --sv-wait seconds (a reboot
     takes 60-90 s), asks it for `reboot` once, and ends the session with exit 2 and a resume state when it does not
     come back (`run --resume` continues with the first step without a result).
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
TOOLS = HERE.parent
DEVUPDATE = ROOT / "libs/ctr-devkit/devupdate/devupdate.py"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(HERE))

import collect as collect_mod  # noqa: E402
import hw_baseline  # noqa: E402

_spec = importlib.util.spec_from_file_location("devupdate", DEVUPDATE)
du = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(du)

LOOPBACK = ("127.0.0.1", "localhost", "::1")
DONE_STATES = ("PASS", "FAIL", "SKIP")
INFRA = ("push failed", "launch failed", "no start")


def load_manifest(path):
    doc = json.loads(Path(path).read_text())
    doc.setdefault("vars", {})
    doc.setdefault("defaults", {})
    return doc


def expand(text, vars_):
    """{VAR} from the manifest's vars (also inside vars); unknown names stay as written."""
    for _ in range(3):
        text = re.sub(r"\{([A-Za-z0-9_]+)\}", lambda m: vars_.get(m.group(1), m.group(0)), text)
    return text


def cfg_switches(name, doc):
    table = {**hw_baseline.CONFIGS, **doc.get("extra_configs", {})}
    return table[name]


def baseline_runs(step, doc, settle, windows):
    """A baseline step -> its run steps (one per scene and config; "D#2" is a repeat of D)."""
    scenes = expand(step["scenes"], doc["vars"]).split(",")
    first = hw_baseline.aligned_settle(settle, hw_baseline.WINDOW)
    last = first + (windows - 1) * hw_baseline.WINDOW
    by_lower = {k.lower(): k for k in {**hw_baseline.CONFIGS, **doc.get("extra_configs", {})}}
    by_lower.update(hw_baseline.ALIASES)
    out = []
    for scene in scenes:
        for raw in step["configs"]:
            cfg, _, rep = raw.partition("#")
            cfg = by_lower.get(cfg.lower(), cfg)
            sw = " ".join(w for w in (hw_baseline.SCENES[scene], cfg_switches(cfg, doc),
                                      f"gxthreadstats={hw_baseline.WINDOW}", "fastboot", "livelog") if w)
            sid = f"{step['id']}.{scene}.{cfg}" + (f"_r{rep}" if rep else "")
            out.append({**{k: v for k, v in step.items() if k not in ("type", "scenes", "configs")},
                        "id": sid, "type": "run", "kind": "baseline", "switches": sw,
                        "until": rf"gxthread \d frame {last}:", "timeout_s": step.get("timeout_s", 300),
                        "meta": {"group": step["id"], "scene": scene, "config": cfg, "repeat": int(rep or 1),
                                 "first": first, "windows": windows}})
    return out


class Session:
    def __init__(self, args):
        self.args = args
        self.doc = load_manifest(args.manifest)
        self.defaults = {**self.doc["defaults"]}
        self.out = Path(args.out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.logf = self.out / "session.log"
        self.state = self.out / "steps.json"
        self.records = json.loads(self.state.read_text()) if (args.resume and self.state.exists()) else []
        self.dry = None
        if args.dry:
            d = self.doc.get("dry", {})
            self.dry = {**d, **d.get(args.dry, {})}
            self.defaults.update(d.get("defaults", {}))
        cias = {k: str(ROOT / v) for k, v in self.doc.get("cias", {}).items()}
        for kv in args.cia or []:
            k, _, v = kv.partition("=")
            cias[k] = str(Path(v).resolve())
        self.vars = {**self.doc["vars"], "OUT": str(self.out / "fetched"), **cias}
        self.vars["release"] = cias["release"]
        self.net = ["--ip", args.ip, "--port", str(args.port), "--timeout", "30"]
        self.sv_net = [*self.net, "--sv-port", str(args.sv_port)]
        self.abort = None

    # ---- plumbing ----
    def log(self, text):
        line = f"{time.strftime('%H:%M:%S')} {text}"
        print(f"session: {text}", flush=True)
        with open(self.logf, "a") as f:
            f.write(line + "\n")

    def save(self):
        self.state.write_text(json.dumps(self.records, indent=1))

    def namespace(self, sv=False, timeout=30):
        return argparse.Namespace(ip=self.args.ip, port=self.args.sv_port if sv else self.args.port,
                                  sv_port=self.args.sv_port, token="", timeout=timeout)

    def devupdate(self, argv, timeout=120):
        """devupdate.py <argv> with the session's address; (rc, output)."""
        sub = ["--sv-port", str(self.args.sv_port)] if argv[0] in ("supervisor", "loop") else []
        cmd = [sys.executable, str(DEVUPDATE), *self.net, argv[0], *sub, *argv[1:]]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            return r.returncode, r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            return 124, "timeout"

    def sv_alive(self, tries=3):
        return du.sv_ping(self.namespace(sv=True), tries=tries, timeout=10) is not None

    def find(self, sid):
        return next((r for r in reversed(self.records) if r["id"] == sid), None)

    # ---- planning ----
    def plan(self):
        steps = []
        settle, windows = self.args.settle, self.args.windows
        for s in self.doc["steps"]:
            if s.get("type") == "baseline":
                runs = baseline_runs(s, self.doc, settle, windows)
                for r in runs:
                    r["phase"] = s["phase"]
                if self.dry and s["id"] in self.dry.get("baseline", {}):
                    cut = self.dry["baseline"][s["id"]]
                    runs = [r for r in runs if r["meta"]["scene"] in cut["scenes"] and r["meta"]["config"] in cut["configs"]]
                steps += runs
            else:
                steps.append(dict(s))
        if self.dry:
            steps += [dict(x) for x in self.dry.get("extra_steps", [])]
            only = self.dry.get("only")
            if only:
                steps = [s for s in steps if s["id"] in only or s["id"].split(".")[0] in only]
                order = {i: n for n, i in enumerate(only)}
                steps.sort(key=lambda s: order.get(s["id"], order.get(s["id"].split(".")[0], 999)))
            for s in steps:
                s.update(self.dry.get("overrides", {}).get(s["id"], {}))
                s.update(self.dry.get("overrides", {}).get(s["id"].split(".")[0], {}) if "." in s["id"] else {})
        if self.args.only:
            want = set(self.args.only.split(","))
            steps = [s for s in steps if s["id"] in want or s.get("phase") in want or s["id"].split(".")[0] in want]
        if self.args.skip:
            skip = set(self.args.skip.split(","))
            steps = [s for s in steps if s["id"] not in skip and s["id"].split(".")[0] not in skip]
        if not self.args.with_destructive:
            steps = [s for s in steps if not s.get("destructive")]
        if not self.args.with_optional:
            steps = [s for s in steps if not s.get("optional")]
        return steps

    # ---- one loop entry ----
    def entry_for(self, s, attempt, extra_switches=""):
        d = {**self.defaults, **{k: v for k, v in s.items() if k in self.defaults or k in
                                 ("cia", "switches", "until", "frames", "timeout_s", "watch", "collect")}}
        sw = expand(d.get("switches", ""), self.vars) + (" " + extra_switches if extra_switches else "")
        words = sw.split()
        if attempt > 1:
            drop = set(s.get("retry_drop", []))
            words = [w for w in words if w.split("=")[0] not in drop]
        tag = f"{s['id']}-{attempt}"
        if s.get("tag", self.defaults.get("tag", True)):
            words.append(f"hwb={tag}")
        until = d.get("until", "")
        if until and s.get("tag", self.defaults.get("tag", True)):
            # from the start of the boot line that carries this run's tag (the line itself may hold the pattern)
            # zero-width, so the loop's result detail stays empty (a multi-line match breaks results.tsv)
            until = rf"(?m)^(?=[^\n]*hwb={re.escape(tag)}\b)(?=[\s\S]*?(?:{until}))"
        name = re.sub(r"[^A-Za-z0-9._-]", "_", s["id"])[:40]
        scale = self.args.timeout_scale
        e = {k: v for k, v in d.items() if k in du.LOOP_DEFAULTS or k == "collect"}
        e.update(name=name, cia=str(self.vars[d["cia"]]) if d.get("cia") else "", switches=" ".join(words), until=until,
                 timeout_s=int(d.get("timeout_s", 300) * scale), frames=int(d.get("frames", 0)),
                 watch=int(d.get("watch", 0)), collect=list(d.get("collect", [])))
        for k in ("push_timeout_s", "start_timeout_s", "reboot_wait_s"):
            e[k] = int(e[k] * scale) if scale > 1 else e[k]
        return e, tag, name

    def run_loop(self, s, attempt, extra_switches=""):
        """One attempt as `devupdate.py loop`. Returns a dict: result, detail, rc, text, dir, tag."""
        e, tag, name = self.entry_for(s, attempt, extra_switches)
        adir = self.out / "steps" / re.sub(r"[^A-Za-z0-9._-]", "_", s["id"]) / f"attempt{attempt}"
        adir.mkdir(parents=True, exist_ok=True)
        man = adir / "loop-entry.json"
        man.write_text(json.dumps({"entries": [e]}, indent=1))
        loopdir = adir / "loop"
        cmd = [sys.executable, str(DEVUPDATE), *self.net, "loop", "--sv-port", str(self.args.sv_port), str(man), "--run", "r",
               "--out", str(loopdir)]
        budget = e["timeout_s"] + e["push_timeout_s"] * 2 + e["start_timeout_s"] * 3 + e["reboot_wait_s"] + 300
        self.log(f"[{s['id']}] attempt {attempt}: {e['switches']}" + (f"  (push {Path(e['cia']).name})" if e["cia"] else ""))
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=budget)
            rc, out = r.returncode, r.stdout + r.stderr
        except subprocess.TimeoutExpired:
            rc, out = 124, "the loop runner did not end within its budget"
        (adir / "loop.stdout").write_text(out)
        res, detail = "no result", out.strip().splitlines()[-1:] and out.strip().splitlines()[-1]
        rows = (loopdir / "r/results.tsv").read_text().splitlines()[1:] if (loopdir / "r/results.tsv").exists() else []
        if rows:
            p = rows[-1].split("\t")
            res, detail = p[2], p[3]
        edir = loopdir / "r" / f"01_{name}"
        text = (edir / "latest.log").read_text(errors="replace") if (edir / "latest.log").exists() else ""
        crash = (edir / "crash.txt").read_text(errors="replace") if (edir / "crash.txt").exists() else ""
        if text:
            (adir / "latest.log").write_text(text)
        # collect_dirs: whole app folders (gpuprobe/*.bin, gpuhang/*, shots/*), supervisor first
        for d in s.get("collect_dirs", []):
            self.fetch_dir(d, adir / d)
        # the named files also go to the session's fetched/ (the newest probe table is a superset of the older)
        for f in s.get("collect", []):
            src = edir / f
            if src.exists():
                dst = self.out / "fetched" / f
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(src, dst)
        for d in s.get("collect_dirs", []):
            if (adir / d).is_dir():
                shutil.copytree(adir / d, self.out / "fetched" / d, dirs_exist_ok=True)
        return {"result": res, "detail": detail or "", "rc": rc, "text": text, "crash": crash, "dir": str(adir),
                "tag": tag, "entry": e}

    def fetch_dir(self, remote, dest):
        for a in (self.namespace(sv=True, timeout=30), self.namespace()):
            try:
                items = du.files_ls(a, f"@/{remote}")
            except (RuntimeError, OSError):
                continue
            for is_dir, _, name in items:
                if is_dir:
                    continue
                try:
                    data = du.files_get(a, f"@/{remote}/{name}")
                except (RuntimeError, OSError):
                    continue
                dest.mkdir(parents=True, exist_ok=True)
                (dest / name).write_bytes(data)
            return

    # ---- verdicts ----
    def verdict(self, s, r):
        """(ok, why) for one attempt."""
        res, text = r["result"], r["text"]
        if r["rc"] == 2:
            return False, "supervisor gone"
        crash_ok = s.get("expect_crash")
        accept = s.get("accept_results") or ["until", "frames"] + (["crash", "ended"] if crash_ok else [])
        if res not in accept:
            return False, f"loop result {res}: {r['detail']}"
        scope = text
        if s.get("tag", self.defaults.get("tag", True)) and r["tag"]:
            i = text.rfind(f"hwb={r['tag']}")
            scope = text[i:] if i >= 0 else text
        scope = scope + "\n" + r["crash"]
        fails = [] if crash_ok else s.get("fail_on", self.defaults.get("fail_on", []))
        for rx in fails:
            m = re.search(rx, scope)
            if m:
                return False, f"fail_on matched: {m.group(0)[:80]}"
        for rx in s.get("expect", []):
            if not re.search(rx, scope):
                return False, f"expected line missing: {rx}"
        if s.get("kind") == "baseline":
            m = s["meta"]
            rows = hw_baseline.parse(text, r["tag"], m["first"], m["windows"])
            if len(rows) < m["windows"]:
                return False, f"only {len(rows)} of {m['windows']} windows"
        return True, ""

    def recover(self, why):
        """After a failed attempt: terminate the game with the GPU reset; the next attempt launches it again."""
        self.log(f"  recovery after: {why}: supervisor terminate --gpureset 2")
        rc, out = self.devupdate(["supervisor", "terminate", "--gpureset", "2"], timeout=60)
        self.log(f"  terminate rc {rc}: {' '.join(out.split())[:160]}")
        return rc == 0

    def wait_supervisor(self):
        end = time.monotonic() + self.args.sv_wait
        asked = False
        while time.monotonic() < end:
            if self.sv_alive(tries=1):
                self.log("  supervisor answers again")
                return True
            if not asked and time.monotonic() > end - self.args.sv_wait * 0.6:
                asked = True
                self.log("  supervisor silent: asking for reboot (it may be gone; harmless if so)")
                self.devupdate(["supervisor", "reboot"], timeout=20)
            time.sleep(10)
        return False

    # ---- gates ----
    def gate_ok(self, g):
        if not g:
            return True, ""
        if "step_ok" in g:
            r = self.find(g["step_ok"])
            ok = bool(r and r["status"] == "PASS")
            return ok, f"step {g['step_ok']} is {r['status'] if r else 'not run'}"
        if "file" in g:
            ok = (ROOT / g["file"]).exists()
            return ok, f"{g['file']} {'exists' if ok else 'is missing'}"
        if "probe" in g:
            table = self.out / "fetched/gpuprobe.txt"
            if not table.exists():
                return False, "no gpuprobe.txt fetched yet"
            probe = collect_mod.parse_probe(table.read_text(errors="replace"))
            want = g.get("result", "OK")
            bad = [c for c in g["probe"] if probe.get(c, {}).get("result") != want]
            return not bad, ("probe " + ", ".join(f"{c}={probe.get(c, {}).get('result', 'absent')}" for c in g["probe"]))
        return True, ""

    # ---- steps ----
    def do_run(self, s):
        attempts = int(s.get("retries", self.defaults.get("retries", 1))) + 1
        rec = {"id": s["id"], "type": "run", "kind": s.get("kind", "run"), "phase": s.get("phase", ""),
               "title": s.get("title", ""), "meta": s.get("meta", {}), "attempts": [], "manual": s.get("manual", "")}
        for n in range(1, attempts + 1):
            r = self.run_loop(s, n)
            ok, why = self.verdict(s, r)
            rec["attempts"].append({"n": n, "result": r["result"], "detail": r["detail"], "ok": ok, "why": why,
                                    "tag": r["tag"], "dir": r["dir"]})
            rec["tag"], rec["dir"], rec["result"], rec["detail"] = r["tag"], r["dir"], r["result"], r["detail"]
            rec["log"] = str(Path(r["dir"]) / "latest.log")
            self.log(f"[{s['id']}] attempt {n}: {'PASS' if ok else 'FAIL'}  loop {r['result']}  {why}")
            if ok:
                rec["status"], rec["why"] = "PASS", ""
                return rec
            rec["status"], rec["why"] = "FAIL", why
            if r["rc"] == 2:
                if not self.wait_supervisor():
                    self.abort = f"supervisor gone during {s['id']}"
                    return rec
                continue
            if n < attempts:
                self.recover(why)
        if rec["status"] == "FAIL" and s.get("kind") != "probe":
            self.recover("last attempt of " + s["id"])  # leave the console in a known state for the next step
        return rec

    def do_cmd(self, s):
        argv = [expand(a, self.vars) for a in s["argv"]]
        Path(self.out / "fetched").mkdir(parents=True, exist_ok=True)
        self.log(f"[{s['id']}] devupdate {' '.join(argv)}")
        rc, out = self.devupdate(argv, timeout=int(s.get("timeout_s", 300)))
        adir = self.out / "steps" / s["id"]
        adir.mkdir(parents=True, exist_ok=True)
        (adir / "output.txt").write_text(out)
        ok = rc == s.get("expect_rc", 0) and (not s.get("expect_out") or re.search(s["expect_out"], out) is not None)
        why = "" if ok else f"rc {rc}, output: {' '.join(out.split())[:160]}"
        if ok and s.get("wait_sv_s"):
            time.sleep(15)
            ok = self.sv_wait_back(int(s["wait_sv_s"]))
            why = "" if ok else "supervisor did not answer after the reboot"
        self.log(f"[{s['id']}] {'PASS' if ok else 'FAIL'} {why}")
        return {"id": s["id"], "type": "cmd", "kind": s.get("kind", "cmd"), "phase": s.get("phase", ""),
                "title": s.get("title", ""), "status": "PASS" if ok else "FAIL", "why": why, "result": f"rc {rc}",
                "detail": ' '.join(out.split())[:300], "dir": str(adir), "manual": s.get("manual", ""), "attempts": []}

    def sv_wait_back(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if self.sv_alive(tries=1):
                return True
            time.sleep(10)
        return False

    # ---- the hang bisect ----
    def do_bisect(self, s):
        base = expand(s["base"], self.vars)
        draws = int(s.get("draws", 586))
        rec = {"id": s["id"], "type": "bisect", "kind": "bisect", "phase": s.get("phase", ""), "title": s.get("title", ""),
               "attempts": [], "manual": s.get("manual", "")}
        table = []
        n_run = [0]
        step_t = {**s, "switches": base, "tag": True, "retries": 1, "fail_on": [], "expect": [],
                  "accept_results": ["until", "crash", "ended", "timeout"]}

        def probe(label, extra, k=None):
            """True = hang, False = no hang, None = the run could not be made."""
            n_run[0] += 1
            sub = {**step_t, "id": f"{s['id']}.{n_run[0]:02d}_{label}"}
            for attempt in (1, 2):
                r = self.run_loop(sub, attempt, extra)
                if r["result"] in INFRA or r["rc"] == 2 or not r["text"]:
                    self.log(f"  bisect run {label}: no usable run ({r['result']}), attempt {attempt}")
                    if r["rc"] == 2 and not self.wait_supervisor():
                        self.abort = "supervisor gone during the bisect"
                        return None
                    self.recover("bisect run")
                    continue
                scope = r["text"][r["text"].rfind(f"hwb={r['tag']}"):] if f"hwb={r['tag']}" in r["text"] else r["text"]
                hang = bool(re.search(s["hang_regex"], scope) or "no more resets" in scope or "[CRASH]" in scope
                            or r["result"] == "crash")
                n = len(re.findall(s["hang_regex"], scope))
                table.append({"label": label, "extra": extra, "k": k, "hang": hang, "hang_lines": n, "result": r["result"],
                              "log": str(Path(r["dir"]) / "latest.log")})
                self.log(f"  bisect {label:<16} {extra or '(full record)':<22} -> {'HANG' if hang else 'no hang'} ({n} hang lines)")
                if hang:
                    self.recover("a hang left the game in a bad state")
                return hang
            return None

        verdict = {"status": "", "culprit": None, "confirmed": False}
        h = probe("C1_repro", "")
        if h is None:
            verdict["status"] = "aborted: no usable run"
        elif not h:
            verdict["status"] = "no hang on the full record: no repro, nothing to bisect"
        else:
            sus_found = None
            for sd in s.get("suspects", []):
                hs = probe(f"C2_skip{sd}", f"gpudrawskip={sd}")
                if hs is False:
                    sus_found = sd
                    break
                if hs is None:
                    break
            if sus_found is not None:
                a = probe(f"C2_cut{sus_found}", f"gpudrawcut={sus_found}")
                b = probe(f"C2_cut{sus_found + 1}", f"gpudrawcut={sus_found + 1}")
                if a is False and b is True:
                    verdict.update(status="culprit found (suspect)", culprit=sus_found, confirmed=True)
            if verdict["culprit"] is None and self.abort is None:
                lo, hi = 0, draws  # lo: no hang (no draws), hi: hang (the full record)
                aborted = False
                while hi - lo > 1:
                    mid = (lo + hi) // 2
                    hm = probe(f"C3_cut{mid}", f"gpudrawcut={mid}", mid)
                    if hm is None:
                        aborted = True
                        break
                    lo, hi = (lo, mid) if hm else (mid, hi)
                if aborted:
                    verdict["status"] = "aborted: no usable run"
                else:
                    sk = probe(f"C3_skip{hi - 1}", f"gpudrawskip={hi - 1}")
                    verdict["culprit"] = hi - 1
                    verdict["confirmed"] = sk is False
                    verdict["status"] = ("culprit found" if sk is False else
                                         "smallest hanging cut found, but skipping that draw does not remove the hang: "
                                         "not monotone or two draws involved; read the table")
        # the full-record repro's hang files (gpuhang/*) came through collect_dirs of probe()
        verdict["table"] = table
        verdict["runs"] = n_run[0]
        adir = self.out / "steps" / s["id"]
        adir.mkdir(parents=True, exist_ok=True)
        (adir / "bisect.json").write_text(json.dumps(verdict, indent=1))
        self.log(f"[{s['id']}] bisect: {verdict['status']}" + (f" draw {verdict['culprit']}" if verdict["culprit"] is not None else ""))
        rec.update(status="PASS" if verdict["culprit"] is not None or "no repro" in verdict["status"] else "FAIL",
                   why=verdict["status"], result=verdict["status"], detail=f"{n_run[0]} runs", dir=str(adir))
        return rec

    def run(self):
        steps = self.plan()
        done = {r["id"] for r in self.records if r["status"] in DONE_STATES} if self.args.resume else set()
        if not self.args.resume:
            self.records = []
        self.log(f"{len(steps)} steps, session dir {self.out}")
        if not self.sv_alive():
            self.log("STOP: the supervisor does not answer (one-time setup not done, or the address is wrong)")
            return 2
        for s in steps:
            if s["id"] in done:
                continue
            ok, why = self.gate_ok(s.get("gate"))
            need = [self.vars.get(s["cia"])] if s.get("cia") else []
            miss = [p for p in need if p and not Path(p).exists()]
            if not ok or miss:
                rec = {"id": s["id"], "type": s.get("type", "run"), "kind": s.get("kind", ""), "phase": s.get("phase", ""),
                       "title": s.get("title", ""), "status": "SKIP", "why": why if not ok else f"CIA missing: {miss[0]}",
                       "attempts": []}
                self.log(f"[{s['id']}] SKIP: {rec['why']}")
            else:
                t = s.get("type", "run")
                rec = (self.do_cmd(s) if t == "cmd" else self.do_bisect(s) if t == "bisect" else self.do_run(s))
            self.records.append(rec)
            self.save()
            if self.abort:
                self.log(f"STOP: {self.abort}; state in {self.state}; continue with: session.py run --resume --out {self.out}")
                return 2
        counts = {k: sum(1 for r in self.records if r["status"] == k) for k in DONE_STATES}
        self.log(f"done: {counts}")
        for r in self.records:
            print(f"  {r['status']:<5} {r['id']:<22} {r.get('why', '')[:90]}")
        collect_mod.collect(self.out, self.out / "results", self.doc, ROOT)
        self.log(f"results: {self.out / 'results'}")
        return 1 if counts["FAIL"] else 0


def export_loop(args):
    s = Session(args)
    entries = []
    for st in s.plan():
        if st.get("type") in ("cmd", "bisect"):
            continue
        e, _, _ = s.entry_for(st, 1)
        entries.append({k: v for k, v in e.items() if v not in ("", 0, [], None) or k in ("name", "switches")})
    Path(args.output).write_text(json.dumps({"defaults": {k: v for k, v in s.defaults.items() if k in du.LOOP_DEFAULTS},
                                             "entries": entries}, indent=1))
    print(f"session: {len(entries)} loop entries -> {args.output}")
    return 0


def prep(args):
    cmds = ["tools/port3ds/build.sh --dev                       # build/3ds/game-dev/ttyd.cia (probe, calib, hang tests)",
            "tools/port3ds/build.sh                             # build/3ds/game/ttyd.cia (release: all timing)",
            "tools/port3ds/build.sh --retail                    # build/3ds/game-retail/ttyd.cia (retail cell 3)",
            "tools/port3ds/build.sh libs/ctr-devkit/supervisor  # build/3ds/supervisor/ctrdk-supervisor.cia (one-time install)",
            "# deterministic CIA (E2, optional; no ccache):",
            "tools/port3ds/build.sh --dev . -DTTYD_CIA_DETERMINISTIC=ON && cp build/3ds/game-dev/ttyd.cia build/ttyd_det_a.cia "
            "&& rm build/3ds/game-dev/ttyd.cia",
            "tools/port3ds/build.sh --dev . -DTTYD_CIA_DETERMINISTIC=ON && cmp build/3ds/game-dev/ttyd.cia build/ttyd_det_a.cia && echo identical",
            "tools/port3ds/build.sh --dev . -DTTYD_CIA_DETERMINISTIC=OFF   # reset the build cache"]
    print("\n".join(cmds))
    if args.build:
        for c in cmds:
            if c.startswith("tools/"):
                subprocess.run(c.split("#")[0], shell=True, cwd=ROOT, check=False)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["plan", "run", "export-loop", "prep"])
    ap.add_argument("output", nargs="?", help="export-loop: the manifest file to write")
    ap.add_argument("--manifest", default=str(HERE / "manifest.json"))
    ap.add_argument("--ip", default=os.environ.get("N3DS_IP", "127.0.0.1"))
    ap.add_argument("--port", type=int, default=5050, help="devupdate port (Azahar session: its own)")
    ap.add_argument("--sv-port", type=int, default=du.SV_PORT)
    ap.add_argument("--out", default=None, help="the session folder (default build/console-session/<time>)")
    ap.add_argument("--only", help="comma list of step ids or phases (A,B,C,D,G,E,R,Z)")
    ap.add_argument("--skip", help="comma list of step ids to leave out")
    ap.add_argument("--resume", action="store_true", help="continue a session folder: skip steps that have a result")
    ap.add_argument("--owner-go", action="store_true", help="the owner said go: required for any address but loopback")
    ap.add_argument("--with-destructive", action="store_true", help="also the supervisor reboot (H5)")
    ap.add_argument("--with-optional", action="store_true")
    ap.add_argument("--dry", choices=["azahar", "fake"], help="the dry-run profile of the manifest (azahar: Azahar + azsv.py; "
                    "fake: fakegame.py + azsv.py, host only)")
    ap.add_argument("--cia", action="append", metavar="KEY=PATH", help="use this file for a CIA key of the manifest (dev, release, retail, det)")
    ap.add_argument("--timeout-scale", type=float, default=1.0, help="multiply every timeout (Azahar is slower)")
    ap.add_argument("--settle", type=int, default=hw_baseline.DEFAULT_SETTLE)
    ap.add_argument("--windows", type=int, default=3)
    ap.add_argument("--sv-wait", type=int, default=240, help="seconds to wait for a silent supervisor before the session stops")
    ap.add_argument("--build", action="store_true", help="prep: run the builds")
    args = ap.parse_args()
    if args.cmd == "prep":
        return prep(args)
    if args.cmd == "run" and args.ip not in LOOPBACK and not args.owner_go:
        print("session: refusing to contact", args.ip, "without --owner-go (every console push needs the owner's go)")
        return 2
    args.out = args.out or str(ROOT / "build/console-session" / time.strftime("%Y%m%d-%H%M%S"))
    if args.dry:
        args.settle, args.windows = 61, 2
    if args.cmd == "export-loop":
        return export_loop(args)
    s = Session(args)
    if args.cmd == "plan":
        for st in s.plan():
            print(f"{st['id']:<24} {st.get('phase', ''):<2} {st.get('type', 'run'):<7} "
                  f"{('push ' + st['cia'] + '; ') if st.get('cia') else ''}"
                  f"{expand(st.get('switches', ' '.join(st.get('argv', []))), s.vars)[:110]}")
        return 0
    return s.run()


if __name__ == "__main__":
    sys.exit(main())
