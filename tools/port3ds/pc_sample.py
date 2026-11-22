#!/usr/bin/env python3
"""Statistical PC sampling profiler for a running 3DS process.

Attaches through a GDB remote stub, then repeatedly: interrupt, read the PC
and LR of every thread, continue, sleep. Afterwards it symbolises the samples
with the ELF's symbol table and prints a flat profile per thread (self time
with top callers taken from LR, plus a "self or direct caller" inclusive
view) and an overall top list. Nothing here is specific to one game: the ELF
and the process are arguments.

Azahar (stub on 24689, already attached to the running title; start it with
run_azahar.py --gdb, which waits for a debugger: connect, continue, and
let the game reach the part to measure before sampling):

    tools/port3ds/pc_sample.py --elf build/3ds/game/ttyd.elf --duration 60

Hardware (Luma3DS Rosalina; enable the debugger first, see "Profiling on
hardware" in docs/PORT_3DS_TESTING.md; the CIA must already be running):

    N3DS_IP=192.168.1.223 tools/port3ds/pc_sample.py --pid <PID> --port 4000 \\
        --elf build/elfs/<build>.elf --duration 30 --interval 20 \\
        --threads game=<tid>,render=<tid> --json build/pc_sample.json

--pid is the process id from the Rosalina menu (Process list). --title <hex
title id> asks Rosalina for its process list over the protocol
(qXfer:osdata) and picks the matching process; that is untested against
Rosalina (see the notes in the testing doc), so use --pid if it fails.
--build [NAME] picks the ELF from build/elfs/ (newest, or the one whose
name contains NAME) instead of --elf.

Caller information is the LR at the sample. It is the real caller only while
the function has not yet pushed LR or called something (leaf code), so
"callers" are an approximation; treat a caller list as a hint.

Safety: the target is always detached and continued on exit, Ctrl-C and
errors. Exit codes: 0 ok, 1 no samples, 2 usage/connection.
"""
import argparse
import bisect
import collections
import json
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import RSP, RSPError  # noqa: E402
import elfsyms  # noqa: E402
from crash_report import find_addr2line  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
SLOW = 0.200  # seconds; one stop-read-continue slower than this triggers a back-off
IDLE_RE = r"^svc(Sleep|Wait|Arbitrate|ReplyAndReceive|Reply)|^__?svc|^syncs?_wait"


class Target:
    """Owns the connection and guarantees the process is released."""

    def __init__(self, rsp):
        self.rsp = rsp
        self.stopped = False
        self.attached = False

    def interrupt(self):
        self.rsp.interrupt()
        self.stopped = True

    def cont(self):
        self.rsp.send("c")
        self.stopped = False

    def release(self):
        """Always leaves the target running and detached."""
        rsp = self.rsp
        try:
            if not self.stopped:
                try:
                    rsp.sock.settimeout(3.0)
                    rsp.interrupt()
                except (OSError, RSPError):
                    pass
                self.stopped = True
            try:
                rsp.sock.settimeout(5.0)
                rsp.detach()
            except (OSError, RSPError):
                pass
            try:
                rsp.send("c")  # harmless if detach already resumed it
            except OSError:
                pass
        finally:
            rsp.close()


def rosalina_pid_for_title(rsp, title):
    """Best effort: standard GDB osdata process list. Unverified on Rosalina."""
    want = title.lower().replace("0x", "")
    xml = ""
    off = 0
    while True:
        reply = rsp.command(f"qXfer:osdata:read:processes:{off:x},fff")
        if not reply or reply[0] not in "ml":
            raise RSPError(f"Rosalina has no process list over the protocol (reply {reply!r}); use --pid")
        xml += reply[1:]
        off += len(reply) - 1
        if reply[0] == "l":
            break
    for item in ET.fromstring(xml).iter("item"):
        cols = {c.get("name"): (c.text or "") for c in item.iter("column")}
        if want in " ".join(cols.values()).lower():
            return int(cols["pid"], 16 if re.search("[a-f]", cols["pid"]) else 10)
    raise RSPError(f"no process matching title {title} in the process list")


class Symbols:
    def __init__(self, elf):
        funcs = [s for s in elfsyms.load_symbols(elf).values() if s.kind == "func"]
        funcs.sort(key=lambda s: s.addr & ~1)
        self.addrs = [s.addr & ~1 for s in funcs]
        self.funcs = funcs
        self.cache = {}

    def name(self, addr):
        a = addr & ~1
        hit = self.cache.get(a)
        if hit is None:
            i = bisect.bisect_right(self.addrs, a) - 1
            if i >= 0:
                s = self.funcs[i]
                # size-less symbols (assembly stubs) cover up to the next symbol
                hit = s.name if (s.size == 0 or a < (s.addr & ~1) + s.size) else f"?{a:08x}"
            else:
                hit = f"?{a:08x}"
            self.cache[a] = hit
        return hit


def addr2line(elf, addrs):
    tool = find_addr2line()
    if not tool or not addrs:
        return {}
    res = subprocess.run([tool, "-e", str(elf), "-C", "-a", "-f"] + [f"{a:x}" for a in addrs],
                         capture_output=True, text=True, timeout=120)
    lines = res.stdout.splitlines()
    out = {}
    for i, a in enumerate(addrs):
        try:
            out[a] = re.sub(r"^.*?/work/", "", lines[i * 3 + 2])
        except IndexError:
            pass
    return out


def sample(args, tgt, deadline, samples):
    """Appends (t, tid, pc, lr) to samples (so Ctrl-C keeps them); returns the stats."""
    rsp = tgt.rsp
    interval = args.interval / 1000.0
    n_stops = n_slow = 0
    slowest = 0.0
    start = time.monotonic()
    first = True
    while time.monotonic() < deadline:
        t0 = time.monotonic()
        if not first:
            tgt.interrupt()
        first = False
        try:
            tids = rsp.thread_ids()
            for tid, r in rsp.core_registers_many(tids).items():
                samples.append((t0 - start, tid, r[15], r[14]))
        finally:
            tgt.cont()
        n_stops += 1
        took = time.monotonic() - t0
        slowest = max(slowest, took)
        if took > SLOW:
            n_slow += 1
            if interval < 1.0:
                interval = min(1.0, max(interval * 2, took))
                print(f"pc_sample: warning: one sample took {took * 1000:.0f} ms; interval backed off to "
                      f"{interval * 1000:.0f} ms", file=sys.stderr)
        time.sleep(interval)
    elapsed = time.monotonic() - start
    return {"stops": n_stops, "elapsed_s": round(elapsed, 3),
                     "rate_hz": round(n_stops / elapsed, 2) if elapsed else 0,
                     "slow_samples": n_slow, "slowest_ms": round(slowest * 1000, 1),
                     "final_interval_ms": round(interval * 1000, 1)}


def pct(n, d):
    return 100.0 * n / d if d else 0.0


def analyse(samples, syms, top, idle_re, names):
    by_thread = collections.defaultdict(list)
    for _, tid, pc, lr in samples:
        by_thread[tid].append((pc, lr))
    idle = re.compile(idle_re)
    threads = []
    busy_total = sum(1 for _, _, pc, _ in samples if not idle.search(syms.name(pc)))
    overall_self = collections.Counter()
    overall_pc = collections.defaultdict(collections.Counter)
    for tid, obs in sorted(by_thread.items()):
        selfc, incl = collections.Counter(), collections.Counter()
        callers = collections.defaultdict(collections.Counter)
        pcs = collections.defaultdict(collections.Counter)
        busy = 0
        for pc, lr in obs:
            f, c = syms.name(pc), syms.name(lr)
            if idle.search(f):
                pass
            else:
                busy += 1
            selfc[f] += 1
            callers[f][c] += 1
            pcs[f][pc & ~1] += 1
            incl[f] += 1
            if c != f:
                incl[c] += 1
            overall_self[f] += 1
            overall_pc[f][pc & ~1] += 1
        name = names.get(tid) or selfc.most_common(1)[0][0]
        threads.append({
            "tid": tid, "name": name, "named": tid in names, "samples": len(obs), "busy": busy,
            "busy_pct": pct(busy, len(obs)), "share_of_busy_pct": pct(busy, busy_total),
            "self": [{"func": f, "n": n, "pct": pct(n, len(obs)),
                      "top_pc": pcs[f].most_common(1)[0][0],
                      "callers": [{"func": c, "n": k} for c, k in callers[f].most_common(3)]}
                     for f, n in selfc.most_common(top)],
            "inclusive": [{"func": f, "n": n, "pct": pct(n, len(obs))} for f, n in incl.most_common(top)],
        })
    total = len(samples)
    overall = [{"func": f, "n": n, "pct": pct(n, total), "top_pc": overall_pc[f].most_common(1)[0][0]}
               for f, n in overall_self.most_common(30)]
    return threads, overall


def resolve_elf(args):
    if args.build is not None:
        cands = sorted((ROOT / "build/elfs").glob("*.elf"), key=lambda p: p.stat().st_mtime, reverse=True)
        if args.build:
            cands = [c for c in cands if args.build in c.name]
        if not cands:
            sys.exit("pc_sample: no matching ELF in build/elfs/")
        return cands[0]
    if args.elf:
        return args.elf
    sys.exit("pc_sample: give --elf or --build")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", type=Path, help="ELF with symbols for the running build")
    ap.add_argument("--build", nargs="?", const="", metavar="NAME",
                    help="use build/elfs/ (newest .elf, or the one whose name contains NAME)")
    ap.add_argument("--host", help="default: $N3DS_IP with --pid/--title, else 127.0.0.1")
    ap.add_argument("--port", type=int, help="default: 4000 with --pid/--title (Rosalina), else 24689 (Azahar)")
    ap.add_argument("--pid", type=lambda s: int(s, 0), help="Rosalina: process id to attach to")
    ap.add_argument("--title", help="Rosalina: hex title id; looks the pid up in the process list (untested)")
    ap.add_argument("--duration", type=float, default=30.0, help="seconds of sampling")
    ap.add_argument("--interval", type=float, default=20.0, help="milliseconds between samples")
    ap.add_argument("--threads", default="", help="names for threads: name=tid,name=tid (tid hex, as shown)")
    ap.add_argument("--top", type=int, default=10, help="entries per thread list")
    ap.add_argument("--idle-re", default=IDLE_RE,
                    help="functions that count as blocked (not busy) for the busy shares")
    ap.add_argument("--json", type=Path, help="also write the result as JSON")
    ap.add_argument("--no-lines", action="store_true", help="skip addr2line file:line lookups")
    args = ap.parse_args()

    elf = resolve_elf(args)
    hw = args.pid is not None or args.title is not None
    host = args.host or (os.environ.get("N3DS_IP") if hw else None) or "127.0.0.1"
    port = args.port or (4000 if hw else 24689)
    names = {}
    for part in filter(None, args.threads.split(",")):
        n, _, t = part.partition("=")
        names[int(t, 16)] = n

    t0 = time.monotonic()
    syms = Symbols(elf)
    print(f"pc_sample: {len(syms.funcs)} function symbols from {elf} ({time.monotonic() - t0:.1f} s)",
          file=sys.stderr)

    try:
        rsp = RSP(host, port, connect_timeout=10, timeout=10)
    except RSPError as e:
        print(f"pc_sample: {e}", file=sys.stderr)
        return 2
    tgt = Target(rsp)
    samples, stats = [], {}
    try:
        try:
            rsp.command("qSupported:multiprocess-;swbreak-;hwbreak-")
        except RSPError:
            pass
        if hw:
            pid = args.pid if args.pid is not None else rosalina_pid_for_title(rsp, args.title)
            rsp.attach(pid)
            tgt.attached = True
            print(f"pc_sample: attached to pid {pid:#x} on {host}:{port}", file=sys.stderr)
        # attached (Rosalina) or waiting for a debugger (Azahar): the target is stopped now
        tgt.stopped = True
        rsp.stop_reason()
        print(f"pc_sample: sampling {args.duration:g} s every {args.interval:g} ms (Ctrl-C to stop early)",
              file=sys.stderr)
        try:
            stats = sample(args, tgt, time.monotonic() + args.duration, samples)
        except KeyboardInterrupt:
            print("pc_sample: interrupted, releasing the target", file=sys.stderr)
    except (RSPError, OSError) as e:
        print(f"pc_sample: {e}", file=sys.stderr)
    finally:
        tgt.release()

    if not samples:
        print("pc_sample: no samples", file=sys.stderr)
        return 1
    if not stats:  # Ctrl-C inside sample(): rate unknown
        stats = {"stops": len({x[0] for x in samples})}
    threads, overall = analyse(samples, syms, args.top, args.idle_re, names)

    if not args.no_lines:
        want = {e["top_pc"] for t in threads for e in t["self"]} | {e["top_pc"] for e in overall}
        try:
            lines = addr2line(elf, sorted(want))
        except (OSError, subprocess.SubprocessError):
            lines = {}
        for e in overall + [e for t in threads for e in t["self"]]:
            e["where"] = lines.get(e["top_pc"], "")

    print(f"\nsamples: {len(samples)} over {stats.get('stops', '?')} stops; "
          f"rate {stats.get('rate_hz', '?')} Hz; elapsed {stats.get('elapsed_s', '?')} s; "
          f"slow (>200 ms) {stats.get('slow_samples', '?')}, slowest {stats.get('slowest_ms', '?')} ms")
    print(f"\n{'thread':<28}{'samples':>8}{'busy%':>8}{'share of busy%':>16}")
    for t in threads:
        print(f"{t['name'][:22] + ' ' + format(t['tid'], 'x'):<28}{t['samples']:>8}{t['busy_pct']:>8.1f}"
              f"{t['share_of_busy_pct']:>16.1f}")
    for t in threads:
        print(f"\n== thread {t['tid']:x} {t['name']}{'' if t['named'] else ' (auto name)'}: "
              f"{t['samples']} samples, {t['busy_pct']:.1f}% busy, {t['share_of_busy_pct']:.1f}% of all busy")
        print("  self%    n  function  <- callers (LR)")
        for e in t["self"]:
            cs = ", ".join(f"{c['func']} {c['n']}" for c in e["callers"])
            print(f"  {e['pct']:5.1f} {e['n']:4d}  {e['func']}  <- {cs}" + (f"   [{e['where']}]" if e.get("where") else ""))
        print("  inclusive (self or direct caller):")
        for e in t["inclusive"]:
            print(f"  {e['pct']:5.1f} {e['n']:4d}  {e['func']}")
    print("\n== overall top 30 (self, all threads)")
    for e in overall:
        print(f"  {e['pct']:5.1f} {e['n']:5d}  {e['func']}" + (f"   [{e['where']}]" if e.get("where") else ""))

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"elf": str(elf), "stats": stats, "threads": threads,
                                         "overall": overall}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
