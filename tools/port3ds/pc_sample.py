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
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
import armdata
import datasample
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


class Ctx:
    """Everything one profiling run collects (it survives reconnects with --follow)."""

    def __init__(self, args, image, syms):
        self.args = args
        self.syms = syms
        self.idle = re.compile(args.idle_re)
        self.image = image
        self.samples = []   # (t, tid, pc, lr, sp, tag)
        self.accesses = []  # (tid, pc, addr, size, is_write, tag)
        self.cov = collections.Counter()
        self.stops = self.slow = 0
        self.slowest = 0.0
        self.interval = args.interval / 1000.0
        self.busy_time = 0.0
        self.start = time.monotonic()
        self.tag = ""
        self.tag_pos = 0
        self.tag_re = re.compile(args.tag_re) if args.tag_log else None

    def poll_tag(self):
        if self.tag_re is None:
            return
        try:
            with open(self.args.tag_log, "rb") as f:
                size = os.fstat(f.fileno()).st_size
                if size < self.tag_pos:
                    self.tag_pos = 0
                f.seek(self.tag_pos)
                new = f.read()
                self.tag_pos = size
        except OSError:
            return
        for m in self.tag_re.finditer(new.decode("latin-1")):
            self.tag = m.group(1)

    def record(self, tid, r):
        pc = r[15]
        self.samples.append((time.monotonic() - self.start, tid, pc, r[14], r[13], self.tag))
        if not self.args.data:
            return
        if self.idle.search(self.syms.name(pc)):
            self.cov["blocked-in-svc"] += 1  # a thread parked in a wait: its stack pops are not work
            return
        insn = self.image.word(pc & ~3)
        if insn is None or pc & 1:
            self.cov["unreadable-or-thumb"] += 1
            return
        acc = armdata.decode(insn, r, pc)
        if acc is None:
            self.cov["neon" if armdata.is_neon_mem(insn) else "non-memory"] += 1
            return
        self.cov["memory"] += 1
        for addr, size, wr in acc:
            self.accesses.append((tid, pc, addr, size, wr, self.tag))


def sample(ctx, tgt, deadline):
    """One attached session: appends to ctx; returns when the deadline passes."""
    rsp = tgt.rsp
    first = True
    while time.monotonic() < deadline:
        t0 = time.monotonic()
        if not first:
            tgt.interrupt()
        first = False
        try:
            ctx.poll_tag()
            for tid, r in rsp.core_registers_many(rsp.thread_ids()).items():
                ctx.record(tid, r)
        finally:
            tgt.cont()
        ctx.stops += 1
        took = time.monotonic() - t0
        ctx.slowest = max(ctx.slowest, took)
        if took > SLOW:
            ctx.slow += 1
            if ctx.interval < 1.0 and not ctx.args.fixed_interval:
                ctx.interval = min(1.0, max(ctx.interval * 2, took))
                print(f"pc_sample: warning: one sample took {took * 1000:.0f} ms; interval backed off to "
                      f"{ctx.interval * 1000:.0f} ms", file=sys.stderr)
        time.sleep(ctx.interval)


def pct(n, d):
    return 100.0 * n / d if d else 0.0


def analyse(samples, syms, top, idle_re, names):
    by_thread = collections.defaultdict(list)
    for _, tid, pc, lr, _sp, _tag in samples:
        by_thread[tid].append((pc, lr))
    idle = re.compile(idle_re)
    threads = []
    busy_total = sum(1 for _, _, pc, _, _, _ in samples if not idle.search(syms.name(pc)))
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
    ap.add_argument("--fixed-interval", action="store_true", help="never back the interval off")
    ap.add_argument("--threads", default="", help="names for threads: name=tid,name=tid (tid hex, as shown)")
    ap.add_argument("--cores", default="", help="core of a thread: tid=core,... (default: game 0, render 2, audio/dvd 1 by name)")
    ap.add_argument("--top", type=int, default=10, help="entries per thread list")
    ap.add_argument("--idle-re", default=IDLE_RE,
                    help="functions that count as blocked (not busy) for the busy shares")
    ap.add_argument("--json", type=Path, help="also write the result as JSON")
    ap.add_argument("--no-lines", action="store_true", help="skip addr2line file:line lookups")
    ap.add_argument("--follow", action="store_true",
                    help="reconnect when the stub goes away (Azahar restarted by map_sweep.py); wait up to --connect-timeout each time")
    ap.add_argument("--connect-timeout", type=float, default=10.0)
    ap.add_argument("--data", action="store_true",
                    help="also sample the data addresses of load/store instructions (ARM mode), named with --memmap and the ELF")
    ap.add_argument("--memmap", type=Path, help="game log with [MEMMAP] lines (port3ds/game/memmap_dump.c, switch memmap)")
    ap.add_argument("--tag-log", type=Path, help="game log tailed during sampling; the last --tag-re match tags each sample")
    ap.add_argument("--tag-re", default=r"sweep: (\S+) \(story", help="regex whose group 1 is the tag (default: the map sweep's current map)")
    ap.add_argument("--tag-prefix", type=int, default=3, help="characters of the tag that make its area (default 3: gor_01 -> gor)")
    ap.add_argument("--start-after", help="regex in --tag-log: let the target run (no sampling) until it matches; --duration counts from then")
    ap.add_argument("--start-timeout", type=float, default=300.0)
    ap.add_argument("--qtm-mb", type=float, default=1.6, help="QTM budget the candidate table is judged against")
    ap.add_argument("--min-shared", type=int, default=5)
    args = ap.parse_args()

    elf = resolve_elf(args)
    hw = args.pid is not None or args.title is not None
    host = args.host or (os.environ.get("N3DS_IP") if hw else None) or "127.0.0.1"
    port = args.port or (4000 if hw else 24689)
    names = {}
    for part in filter(None, args.threads.split(",")):
        n, _, t = part.partition("=")
        names[int(t, 16)] = n
    cores = {}
    for part in filter(None, args.cores.split(",")):
        t, _, c = part.partition("=")
        cores[int(t, 16)] = int(c)

    t0 = time.monotonic()
    syms = Symbols(elf)
    image = datasample.ElfImage(elf) if args.data else None
    print(f"pc_sample: {len(syms.funcs)} function symbols from {elf} ({time.monotonic() - t0:.1f} s)",
          file=sys.stderr)

    ctx = Ctx(args, image, syms)
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))  # report what was sampled
    deadline = ctx.start + args.duration
    attached_once = False
    try:
        while time.monotonic() < deadline:
            try:
                rsp = RSP(host, port, connect_timeout=args.connect_timeout if (args.follow or not attached_once) else 1,
                          timeout=10)
            except RSPError as e:
                print(f"pc_sample: {e}", file=sys.stderr)
                if not args.follow:
                    return 2
                continue
            tgt = Target(rsp)
            try:
                try:
                    rsp.command("qSupported:multiprocess-;swbreak-;hwbreak-")
                except RSPError:
                    pass
                if hw:
                    pid = args.pid if args.pid is not None else rosalina_pid_for_title(rsp, args.title)
                    rsp.attach(pid)
                    print(f"pc_sample: attached to pid {pid:#x} on {host}:{port}", file=sys.stderr)
                # attached (Rosalina) or waiting for a debugger (Azahar): the target is stopped now
                tgt.stopped = True
                rsp.stop_reason()
                if not attached_once:
                    print(f"pc_sample: sampling {args.duration:g} s every {args.interval:g} ms (Ctrl-C to stop early)",
                          file=sys.stderr)
                if args.start_after and not attached_once:
                    pat = re.compile(args.start_after)
                    tgt.cont()
                    wait_end = time.monotonic() + args.start_timeout
                    while time.monotonic() < wait_end:
                        try:
                            if pat.search(Path(args.tag_log).read_text(errors="replace")):
                                break
                        except OSError:
                            pass
                        time.sleep(0.5)
                    else:
                        print("pc_sample: --start-after never matched", file=sys.stderr)
                        return 1
                    tgt.interrupt()
                    ctx.start = time.monotonic()
                    deadline = ctx.start + args.duration
                    print("pc_sample: --start-after matched, sampling", file=sys.stderr)
                attached_once = True
                sample(ctx, tgt, deadline)
            except (RSPError, OSError) as e:
                print(f"pc_sample: {e}", file=sys.stderr)
                if not args.follow:
                    break
            finally:
                tgt.release()
    except KeyboardInterrupt:
        print("pc_sample: interrupted, releasing the target", file=sys.stderr)

    samples = ctx.samples
    if not samples:
        print("pc_sample: no samples", file=sys.stderr)
        return 1
    elapsed = time.monotonic() - ctx.start
    stats = {"stops": ctx.stops, "elapsed_s": round(elapsed, 3),
             "rate_hz": round(ctx.stops / elapsed, 2) if elapsed else 0,
             "slow_samples": ctx.slow, "slowest_ms": round(ctx.slowest * 1000, 1),
             "final_interval_ms": round(ctx.interval * 1000, 1)}
    threads, overall = analyse(samples, syms, args.top, args.idle_re, names)

    if not args.no_lines:
        want = {e["top_pc"] for t in threads for e in t["self"]} | {e["top_pc"] for e in overall}
        try:
            lines = addr2line(elf, sorted(want))
        except (OSError, subprocess.SubprocessError):
            lines = {}
        for e in overall + [e for t in threads for e in t["self"]]:
            e["where"] = lines.get(e["top_pc"], "")

    print(f"\nsamples: {len(samples)} over {stats['stops']} stops; "
          f"rate {stats['rate_hz']} Hz; elapsed {stats['elapsed_s']} s; "
          f"slow (>200 ms) {stats['slow_samples']}, slowest {stats['slowest_ms']} ms")
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

    data = None
    if args.data:
        total = sum(ctx.cov.values())
        cov = {k: v for k, v in ctx.cov.items()}
        print(f"\n== data sampling coverage: {total} thread samples; "
              + ", ".join(f"{k} {v} ({100 * v / total:.1f}%)" for k, v in sorted(cov.items(), key=lambda kv: -kv[1])))
        memmap = datasample.read_memmap(args.memmap) if args.memmap else []
        stacks = datasample.infer_stacks(samples, names)
        dm = datasample.DataMap(elf, image, memmap, stacks)
        corem = {t: (cores.get(t) if t in cores else datasample.core_of(names.get(t, ""))) for t in {s[1] for s in samples}}
        fname = lambda pc: syms.name(pc)
        tnames = names

        def run(accs, title, brief=False):
            ent = datasample.aggregate([a[:5] for a in accs], dm, tnames, corem, fname)
            res = datasample.build(ent, len(accs), 25, args.qtm_mb, args.min_shared)
            print("\n" + datasample.format_text(res, title, args.qtm_mb, brief))
            return res

        data = {"coverage": cov, "overall": run(ctx.accesses, "overall")}
        areas = collections.defaultdict(list)
        for a in ctx.accesses:
            if a[5]:
                areas[a[5][:args.tag_prefix]].append(a)
        data["areas"] = {}
        for area, accs in sorted(areas.items()):
            data["areas"][area] = run(accs, f"area {area}", True)
        data["maps_seen"] = sorted({s[5] for s in samples if s[5]})

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"elf": str(elf), "stats": stats, "threads": threads,
                                         "overall": overall, "data": data}, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
