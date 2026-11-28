#!/usr/bin/env python3
"""fakegame: a host-only stand-in for the game's devupdate server, to drill the session pipeline without Azahar.

    fakegame.py --port 5070 [--inject faults.json] [--culprit 417]

It speaks devupdate's wire protocol (ping, push, restart, report, files get/ls/put) and, per launch, writes a
latest.log and SD files that look like the real game's for the switches it was given: gpuprobe (gpuprobe.txt,
journal, the dies-at-rt_deleted resume), calib, gxthread windows (baseline configs change the numbers), the
gpuhangtest / gpuhangrecover / gpuhangtimes survival lines, gpudrawcut/gpudrawskip against a ground-truth culprit
draw (the hang bisect), crashtest, bench, texdt, heaplog, verify, retail. NOT the real game: every number is made up
and only the line formats follow plat/*.c and game/*.c. Use: selftest.py, and the pipeline drill in the runbook.

--inject: a JSON list of one-shot faults {"match": "A2a-1", "fault": "crash|nostart|silent|hang_forever"}; a launch whose
switches contain the match gets the fault once (crash: the crash screen with recovery mode; nostart: the process
never writes a log; silent: nothing answers for 3 s).
"""
import argparse
import json
import re
import socket
import struct
import threading
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HDR = struct.Struct("<4sHHIIIIQ32s")
CALIB_NAMES = re.findall(r"\| `([a-zA-Z0-9_]+)` \|", (ROOT / "docs/PORT_3DS_60FPS.md").read_text()
                         .split("### 2a.")[1].split("### 2b.")[0])
PROBE_CASES = re.findall(r"^\| ([a-z][a-z0-9_]*) \| ([a-z]+) \| [^|]* \| [^|]* \| (\d) \|",
                         (ROOT / "docs/PORT_3DS_GPU_RULES.md").read_text(), re.M)


class Game:
    def __init__(self, culprit, inject, scale):
        self.files = {}          # SD card: "gpuprobe.txt" -> bytes (the "@/" folder)
        self.culprit, self.inject, self.scale = culprit, inject, scale
        self.log = ""            # the full latest.log of this process
        self.t0 = 0.0
        self.alive = True        # answers on devupdate
        self.recovery = False
        self.silent_until = 0.0
        self.nostart = False
        self.lock = threading.Lock()
        self.pending_crash = False
        self.log_prev = ""
        self.calls = []
        self.launch("", boot=False)

    # what the process has written by now: the first line at once, the rest after a short run time
    def visible(self):
        if self.nostart:
            return self.log_prev
        dt = time.monotonic() - self.t0
        lines = self.log.splitlines(True)
        n = len(lines) if dt >= 0.8 * self.scale else min(len(lines), 2 if dt >= 0.2 * self.scale else 0)
        return "".join(lines[:n])

    def launch(self, args, boot=True):
        with self.lock:
            self.log_prev = self.log
            self.nostart = False
            self.silent_until = 0.0
            self.recovery = False
            self.t0 = time.monotonic()
            self.calls.append(args)
            self.log = self.run(args) if boot else ""
            if self.pending_crash:
                self.pending_crash = False
                self.crash(args, "the process died")
            for f in list(self.inject):
                if f["match"] and f["match"] in args:
                    self.inject.remove(f)
                    print(f"fakegame: inject {f['fault']} into launch {args[:60]!r}", flush=True)
                    if f["fault"] == "crash":
                        self.crash(args, "injected crash")
                    elif f["fault"] == "nostart":
                        self.nostart = True
                    elif f["fault"] == "silent":
                        self.silent_until = time.monotonic() + 3
                    elif f["fault"] == "hang_forever":
                        self.log = self.log.split("\n")[0] + "\n[I] gpu: fence wait for seq 9 (hung)\n" * 3
                    break

    def crash(self, args, why):
        base = self.log if self.log else f'[I] boot: switches "{args}"\n'
        self.log = base + f"[E] [CRASH] v1 {why}\n[E] watchdog: crash screen, recovery mode\n"
        self.recovery = True

    # ---------------------------------------------------------------- what a run does
    def run(self, args):
        sw = dict((w.split("=", 1) + [""])[:2] for w in args.split())
        L = [f'[I] boot: switches "{args}"']

        def f(key, default=""):
            return sw.get(key, default)

        if "crashtest" in sw:
            self.log = "\n".join(L) + "\n"
            self.crash(args, "crashtest " + f("crashtest"))
            return self.log
        if "gpuprobe" in sw:
            return "\n".join(L + self.probe(sw)) + "\n"
        if "calib" in sw:
            return "\n".join(L + self.calib(sw)) + "\n"
        gx = int(f("gxthread", "0") or 0)
        cfg_fast = gx == 2
        frame_us = 16700 if cfg_fast else 33300
        busy = 9000 if cfg_fast else 22000
        if "old" in args or "gxdirty=0" in sw:
            busy += 3000
        if "gxone" in sw:
            busy += 500
        if "gpucore" in sw:
            busy += 300
        if "noqtm" in sw:
            busy += 700
        if "texdt=2" in args:
            L.append("[I] texdt: made 12, issued 12, cpu on render thread 0, cpu no job slot 0, no staging block 0, "
                     "arena peak 3 of 16 granules; verify queued 12 ok 12 BAD 0 skipped 0")
        if "loadprof" in sw:
            L.append("[I] [LOADPROF] mapchange gor_01: 30 frames, wall 900 ms | after (game-thread busy per frame, ms): first 10")
        if "texframe" in sw:
            L.append("[I] texframe: 12 lk 3 up 1 hash 2/100B async 1/1 | us: total 400 conv 100 (dec 50 create 50) hash 10")
        if "heaplog" in sw:
            L.append("[I] heap until map change from gor_01: peak arena 28 MiB at least 4500 KiB free")
            L.append("[I] heap: free 4600 KiB")
        if "jobprobe" in sw:
            L.append("[I] job round trip: mean 35 us per wake")
        if "vramstats" in sw:
            L.append("[I] vramstats: frame 61: per frame 20 textures bound from linear (400 KB), 5 from VRAM (100 KB), 25 binds")
        if "gxchunk=2" in args:
            L.append("[I] gxchunk frame 61: lists submitted 4, citro3d splits 0, copied 0; verify draws 100 differ 0 records over 0")
            L.append("[I] gxdirect frame 61: verify checks 100 mismatches 0 missed 0 stale 0")
        if "gpuhangtest" in sw:
            self.hang_test(L, sw)
        hang = False
        cut, skip = f("gpudrawcut", ""), f("gpudrawskip", "")
        if "gpudump" in sw and ("map=gor_01" in args):
            if cut != "":
                hang = int(cut) > self.culprit
            elif skip != "":
                hang = int(skip.split("-")[0]) != self.culprit
            else:
                hang = True
            if hang:
                L += ["[W] gpu: fence wait for seq 33 (2000 ms)", "[I] gpudump: hang record 33 frame 887 -> gpuhang/hang_f887_r33.bin",
                      "[W] gpu: GPU hang (fence): reset #1 done, GSPGPU_ResetGpuCore 00000000; synthetic P3D",
                      "[I] gpu: citro3d state marked dirty after GPU reset #1"]
                self.files["gpuhang/hang_f887_r33.bin"] = b"\1\0\0\0" * 64
                self.files["gpuhang/hang_f887_r33.txt"] = b"record 33 frame 887\n"
        if "bench" in sw:
            a, _, b = f("bench").partition(",")
            ret = "retail=1" in args or "retail" in sw
            L.append(f"[I] bench {a}-{b}: frame {frame_us - (500 if ret else 0)} busy {busy - (300 if ret else 0)} render 8000 "
                     f"frames 120 vblanks {frame_us // 16700 * 120}")
        if "gxthreadstats" in sw:
            for fr in range(61, 902, 60):
                L.append(f"[I] gxthread {gx} frame {fr}: game us: frame {frame_us} busy {busy} fences 500 idle 3000 | "
                         f"translator 4000 (copydisp 100) render 9000")
                L.append("[I] gxthread   render (last frame): 9500 us, 700 of them")
        if "devupdate" in sw and "updated from" not in "\n".join(L):
            L.append("[I] devupdate: updated from #1 abcdef0")
        return "\n".join(L) + "\n"

    def hang_test(self, L, sw):
        n = int(sw.get("gpuhangtimes", "1") or 1)
        L.append("[W] gpuhangtest: the render thread stops before C3D_FrameBegin of record 700")
        if "gpuhangrecover" not in sw:
            L.append("[W] gpu: fence wait for seq 9 (2000 ms)")
            return
        for i in range(1, n + 1):
            if i > 3:
                L.append("[E] gpu: the GPU hung again after 3 resets within 10 s (fence): no more resets, the watchdog takes over")
                self.pending_crash = True
                break
            L.append(f"[W] gpu: GPU hang (fence): reset #{i} done, GSPGPU_ResetGpuCore 00000000; synthetic P3D")
            L.append(f"[I] gpu: citro3d state marked dirty after GPU reset #{i}")

    def probe(self, sw):
        journal = self.files.get("gpuprobe.journal", b"").decode()
        fresh = "probefresh" in sw
        if fresh:
            journal = ""
        done, began = {}, set()
        for ln in journal.splitlines():
            p = ln.split()
            if p[0] == "B":
                began.add(p[1])
            elif p[0] == "R":
                done[p[1]] = (p[2], " ".join(p[4:]))
        only = sw.get("probe", "")
        L = [f"[I] gpuprobe: {len(PROBE_CASES)} cases, timeout 200 ms"]
        died = False
        for cid, grp, dng in PROBE_CASES:
            dng = int(dng)
            toks = only.split(",")
            if only and not any(cid == t or grp == t or (t.endswith("*") and cid.startswith(t[:-1])) for t in toks):
                continue
            if cid in began and cid not in done:
                done[cid] = ("FAULT", "the process died or froze in this case (journal)")
                journal += f"R {cid} FAULT 0 the process died or froze in this case (journal)\n"
            if cid in done:
                continue
            if "probesafe" in sw and dng >= 3:
                done[cid] = ("SKIP", "danger 3 skipped by request")
                continue
            journal += f"B {cid}\n"
            if cid == "rt_deleted":
                self.files["gpuprobe.journal"] = journal.encode()
                self.write_probe(done)
                L.append("[E] svcBreak in citro3d (render target deleted inside a frame)")
                self.pending_crash = True
                died = True
                break
            res = "OK"
            note = "86 us" if cid.startswith("cb_") else ""
            if cid in ("sc_w0", "sc_h0"):
                res, note = "HANG", "GPU reset worked"
            if cid == "rt_dim80":
                res, note = "WRONG", "guard check works"
            if cid in ("cb_qtm", "vb_qtm", "ib_qtm"):
                note = "OK: osConvertVirtToPhys ok, flush rc 0"
            done[cid] = (res, note)
            journal += f"R {cid} {res} 5 {note}\n"
            L.append(f"[I] gpuprobe: {cid:<14} {res:<6} 5 ms  [?] {note}")
        L.append("[I] gpuprobe: cp_thru: single 8192 words 86 us, burst 41 us, 8 calls 120 us")
        L.append("[I] gpuprobe: dt rgba8 flip 1: 10 of 10 sizes equal")
        self.files["gpuprobe.journal"] = journal.encode()
        self.write_probe(done)
        for cid, grp, dng in PROBE_CASES:
            if cid in done and done[cid][0] == "OK" and cid.endswith("_a8"):
                self.files[f"gpuprobe/{cid}.bin"] = b"\0" * 16
        self.files["gpuprobe/ctl_recover.bin"] = b"\0" * 16
        if not died:
            L.append("[I] GPUPROBE DONE: %d OK, %d not OK (0 WRONG, 0 HANG, 0 FAULT, 0 ERR, 0 SKIP; 0 of %d cases from the journal)"
                     % (sum(1 for v in done.values() if v[0] == "OK"), 1, len(PROBE_CASES)))
        return L

    def write_probe(self, done):
        rows = ["%-14s %-9s %-4s %-32s %-6s %6s  %s" % ("case", "group", "dng", "expected", "result", "ms", "note")]
        for cid, grp, dng in PROBE_CASES:
            r = done.get(cid)
            rows.append("%-14s %-9s %-4d %-32s %-6s %6d  %s" % (cid, grp, int(dng), "OK", r[0] if r else "-", 5 if r else 0, r[1] if r else ""))
        self.files["gpuprobe.txt"] = ("\n".join(rows) + "\n").encode()

    def calib(self, sw):
        L = ["[I] CALIB start: %d benchmarks, 100 ms a run, on the console, QTM on" % len(CALIB_NAMES)]
        txt = ["TTYD 3DS calibration (timing)", "build fake", "running on: console", ""]
        pmu = list(txt)
        for i, n in enumerate(CALIB_NAMES):
            ns = 10.0 + i * 3.5
            mb = f" {300.0 + i:>10,.2f} MB/s" if "memcpy" in n or "memset" in n or n.startswith("read_seq") else ""
            txt.append("%-22s %12d ops %10.2f ms %12s ns/op%s" % (n, 1000, ns, f"{ns:,.2f}", mb))
            pmu.append("%-22s cycles/op %10.2f  dmiss/op %9.2f  imiss/op %9.2f" % (n, ns * 0.8, 0.1, 0.0))
            L.append(f"[I] CALIB {n:<22} 1000 ops {ns:9.2f} ms {ns:11.2f} ns/op")
        self.files["calib.txt"] = ("\n".join(txt) + "\n").encode()
        if "calibpmu" in sw:
            self.files["calib_pmu.txt"] = ("\n".join(pmu) + "\n").encode()
        L.append("[I] CALIB DONE: %d benchmarks in 1000 ms" % len(CALIB_NAMES))
        return L

    # ---------------------------------------------------------------- the wire
    def pong(self):
        if not self.alive or time.monotonic() < self.silent_until:
            return None
        return f"pong title=000400000FD88600 version=fake kind=devupdate free=1000 recovery={1 if self.recovery else 0} pending=0 am=ok"


def recv_n(c, n):
    b = b""
    while len(b) < n:
        chunk = c.recv(min(65536, n - len(b)))
        if not chunk:
            raise OSError("closed")
        b += chunk
    return b


def handle(g, c):
    h = recv_n(c, 64)
    _m, _v, cmd, flags, size, crc, alen, tid, _t = HDR.unpack(h)
    area = recv_n(c, alen) if alen else b""
    if flags & 4 and area:
        area = area[1 + area[0]:]
    argtext = area.decode(errors="replace")
    pong = g.pong()
    if pong is None:
        return  # silent: the client times out
    out = b""
    if cmd == 1:
        out = f"{pong}\ndone\n".encode()
    elif cmd == 2:
        data = recv_n(c, size)
        if zlib.crc32(data) & 0xFFFFFFFF != crc:
            out = b"error 5 crc mismatch\n"
        else:
            out = f"installed {tid:016x}\ndone\n".encode()
            c.sendall(out)
            if flags & 1:
                time.sleep(0.2)
                g.launch(argtext)
            return
    elif cmd == 3:
        c.sendall(b"restarting\ndone\n")
        g.launch(argtext)
        return
    elif cmd == 4:
        text = g.visible().encode()
        out = f"report {len(text)}\n".encode() + text + b"\ndone\n"
    elif cmd == 6:
        op, _, path = argtext.partition(" ")
        rel = path[2:] if path.startswith("@/") else path
        if op == "put":
            g.files[rel] = recv_n(c, size) if size else b""
            out = b"done\n"
        elif op == "get":
            d = g.visible().encode() if rel == "latest.log" else g.files.get(rel)
            if rel == "crash.txt":
                d = g.visible().encode() if g.recovery else None
            if d is None:
                out = f"error 21 no such file {rel}\n".encode()
            else:
                out = f"file {len(d)} {zlib.crc32(d) & 0xFFFFFFFF:08x}\n".encode() + d + b"\ndone\n"
        elif op == "ls":
            rel = rel.rstrip("/")
            names = sorted({k[len(rel) + 1:].split("/")[0] for k in g.files if rel and k.startswith(rel + "/")})
            if not names and rel:
                out = f"error 21 no such folder {rel}\n".encode()
            else:
                out = "".join(f"entry - {len(g.files[rel + '/' + n])} {n}\n" for n in names).encode() + b"done\n"
        else:
            out = b"error 4 bad files op\n"
    else:
        out = f"error 4 unknown command {cmd}\n".encode()
    c.sendall(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--inject", help="JSON list of one-shot faults")
    ap.add_argument("--culprit", type=int, default=417, help="the draw that hangs (the bisect's ground truth)")
    ap.add_argument("--scale", type=float, default=1.0, help="multiplier of the (short) run times")
    a = ap.parse_args()
    g = Game(a.culprit, json.loads(Path(a.inject).read_text()) if a.inject else [], a.scale)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", a.port))
    srv.listen(8)
    print(f"fakegame: devupdate on 127.0.0.1:{a.port}", flush=True)
    while True:
        c, _ = srv.accept()

        def one(c=c):
            with c:
                c.settimeout(10)
                try:
                    handle(g, c)
                except OSError:
                    pass

        threading.Thread(target=one, daemon=True).start()


if __name__ == "__main__":
    main()
