#!/usr/bin/env python3
"""azsv: a stand-in for the supervisor system module, for the dry run against Azahar (or the fake game).

    azsv.py --up-port 5060 --port 5061          # in front of an `azahar-session start --supervise` on 5060

Azahar cannot run a system module beside the game (the real supervisor needs PM, NS and GSP; its F16 row in
docs/CTR_DEVKIT_PLAN.md). The loop runner (`devupdate.py loop`) and session.py talk to the supervisor's protocol on
--sv-port, so this process answers that protocol and turns it into what devupdate's own server (the game) can do:

  ping, status      built from the game's devupdate answer and its latest.log: game_state, heartbeat (counted from
                    the launch id in the log, or a changed log head), frame (the last `frame N:` of a gxthread
                    line), flags 4 when the game is in recovery mode (crash screen) or the log has [CRASH]
  launch SWITCHES   devupdate `restart` with the switches (the Azahar session must run with --supervise)
  push              the CIA goes to devupdate's push (flags 0), then a `restart` with the same switches
  files             passed through (get/ls, "@/" read-only folder)
  terminate         NOT possible: reported as "pm=0 emulated" and the status shows the game stopped until the next
                    launch; the process is not killed and there is no GPU reset
  watch N           a small watchdog: no frame change for N s -> `restart` with the last switches (max 2)
  reboot, exit      answered, no effect

What this cannot show: that PM starts the module and keeps it beside the game, PMAPP_TerminateTitle on a hung
process, GSPGPU_ResetGpuCore from the supervisor, NS_RebootToTitle, the heartbeat service (`ctrdk:sv`).
"""
import argparse
import importlib.util
import re
import socket
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
_spec = importlib.util.spec_from_file_location("devupdate", ROOT / "libs/ctr-devkit/devupdate/devupdate.py")
du = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(du)

HDR = struct.Struct("<4sHHIIIIQ32s")


class Emu:
    def __init__(self, ip, up_port):
        self.up = argparse.Namespace(ip=ip, port=up_port, token="", timeout=10, no_report=True)
        self.t0 = time.monotonic()
        self.launch_id = 0
        self.launch_t = 0.0
        self.pre_head = ""
        self.switches = ""
        self.terminated = False
        self.watch_s = 0
        self.relaunches = 0
        self.frame, self.frame_t = 0, time.monotonic()
        self.lock = threading.Lock()
        self.last = "none"
        self.hist = []

    # -- upstream (the game's devupdate server)
    def up_ping(self):
        return du.ping_line(self.up, timeout=3)

    def up_log(self):
        try:
            return du.files_get(argparse.Namespace(**{**vars(self.up), "timeout": 8}), "@/latest.log").decode(errors="replace")
        except (RuntimeError, OSError):
            return ""

    def up_restart(self, switches):
        sw = du.switch_bytes(switches)
        link = du.Link(self.up)
        try:
            with link.connect() as s:
                s.sendall(du.header("restart", args=sw, token="") + sw)
                return link.lines(s, lambda *a: None)
        except OSError as e:
            return f"error {e}"

    def mark_launch(self, switches):
        with self.lock:
            self.launch_id += 1
            self.launch_t = time.monotonic()
            self.pre_head = self.up_log()[:300]
            self.terminated = False
            self.switches = switches
            return f"{switches} launchid={self.launch_id}".strip()

    def status(self):
        pong = self.up_ping()
        text = self.up_log() if pong else ""
        with self.lock:
            lid, lt, term = self.launch_id, self.launch_t, self.terminated
        started = False
        if lid and text:
            started = f"launchid={lid}" in text or (text[:300] != self.pre_head and len(text) > 0)
        elif not lid and pong:
            started = True  # the game was already running when azsv came up
        frames = re.findall(r"frame (\d+):", text)
        frame = int(frames[-1]) if frames else 0
        if frame != self.frame:
            self.frame, self.frame_t = frame, time.monotonic()
        flags = 0
        if pong and "recovery=1" in pong.split():
            flags |= 4
        if started and "[CRASH]" in text:
            flags |= 4
        state = "stopped" if (term or (not pong and started)) else "running"
        since = int(time.monotonic() - lt) + 1 if (started and lid) else (1 if started else 0)
        return state, started, since, frame, flags, pong

    def watchdog(self):
        while True:
            time.sleep(1)
            if not self.watch_s:
                continue
            state, started, since, frame, flags, pong = self.status()
            if started and state == "running" and not (flags & 4) and time.monotonic() - self.frame_t > self.watch_s:
                if self.relaunches < 2:
                    self.relaunches += 1
                    self.last = f"watchdog relaunch {self.relaunches}"
                    self.hist.append(f"{time.strftime('%H:%M:%S')} {self.last}")
                    print(f"azsv: {self.last}", flush=True)
                    sw = self.mark_launch(self.switches)
                    self.up_restart(sw)
                    self.frame_t = time.monotonic()


def recv_n(c, n):
    b = b""
    while len(b) < n:
        chunk = c.recv(min(65536, n - len(b)))
        if not chunk:
            raise OSError("closed")
        b += chunk
    return b


def relay(emu, frame, c):
    """Send the raw request to the game's server and copy the reply to the client until it closes."""
    try:
        with socket.create_connection((emu.up.ip, emu.up.port), timeout=15) as u:
            u.settimeout(120)
            u.sendall(frame)
            while True:
                chunk = u.recv(65536)
                if not chunk:
                    break
                c.sendall(chunk)
    except OSError as e:
        c.sendall(f"error 2 azsv: the game does not answer on devupdate ({e})\n".encode())


def handle(emu, c):
    h = recv_n(c, 64)
    _m, _v, cmd, flags, size, crc, alen, tid, _t = HDR.unpack(h)
    area = recv_n(c, alen) if alen else b""
    argtext = area.decode(errors="replace")
    if flags & 4 and area:  # FLAG_VERSION: a length-prefixed version text before the switches
        argtext = area[1 + area[0]:].decode(errors="replace")
    out = []
    emu.hist.append(f"{time.strftime('%H:%M:%S')} cmd {cmd:#x} {argtext[:80]}")
    if cmd == 1:
        state, started, since, frame, fl, pong = emu.status()
        out = [f"pong supervisor version=azsv kind=emulation uptime={int(time.monotonic() - emu.t0)} game=0004000000000000 "
               f"state={state} pid=0 proto=1", "done"]
    elif cmd == 0x101:
        state, started, since, frame, fl, pong = emu.status()
        out = ["status kind=emulation", f"status game_state={state}",
               (f"status heartbeat={since} frame={frame} flags={fl} age_ms=0 stale_ms=0 since_launch={since}" if started
                else "status heartbeat=none"),
               f"status watch={emu.watch_s} relaunches={emu.relaunches} max_relaunches=2",
               f"status switches={emu.switches}", f"status last={emu.last}", "done"]
    elif cmd == 0x102:
        emu.terminated = True
        emu.last = "terminate (emulated)"
        out = ["terminate pm=0 emulated: no process is killed, no GPU reset", "done"]
    elif cmd == 0x103:
        sw = emu.mark_launch(argtext)
        if emu.up_ping() is None:
            out = ["error 30 azsv: no game process to relaunch (Azahar cannot start a title by itself)"]
        else:
            emu.last = "launch"
            threading.Thread(target=emu.up_restart, args=(sw,), daemon=True).start()
            out = ["launch rc=00000000 (emulated: devupdate restart)", "done"]
    elif cmd == 0x104:
        emu.watch_s = int(argtext or 0)
        emu.relaunches = 0
        emu.frame_t = time.monotonic()
        out = [f"watch {emu.watch_s}", "done"]
    elif cmd in (0x105, 0x106):
        out = ["ok (emulated: no effect)", "done"]
    elif cmd == 2:
        payload = recv_n(c, size)
        sw = emu.mark_launch(argtext)
        up_frame = HDR.pack(b"CDU1", 1, 2, flags & 4, size, crc, alen, tid, b"\0" * 32) + area + payload
        try:
            with socket.create_connection((emu.up.ip, emu.up.port), timeout=15) as u:
                u.settimeout(180)
                u.sendall(up_frame)
                buf = b""
                while b"done\n" not in buf and b"error " not in buf:
                    chunk = u.recv(65536)
                    if not chunk:
                        break
                    buf += chunk
                c.sendall(buf)
        except OSError as e:
            c.sendall(f"error 2 azsv: push failed ({e})\n".encode())
            return
        if b"done\n" in buf:
            emu.last = "push"
            time.sleep(1)
            emu.up_restart(sw)
        return
    elif cmd == 6 and argtext.startswith("get @sv/"):
        data = ("\n".join(emu.hist) + "\n").encode()
        c.sendall(f"file {len(data)} {zlib.crc32(data) & 0xFFFFFFFF:08x}\n".encode() + data + b"\ndone\n")
        return
    elif cmd == 6 or cmd == 4 or cmd == 3 or cmd == 7:
        relay(emu, h + area + (recv_n(c, size) if size else b""), c)
        return
    else:
        out = [f"error 4 unknown command {cmd} (azsv)"]
    c.sendall("".join(l + "\n" for l in out).encode())


def serve(emu, port):
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    print(f"azsv: supervisor emulation on 127.0.0.1:{port}, game on :{emu.up.port}", flush=True)
    threading.Thread(target=emu.watchdog, daemon=True).start()
    while True:
        c, _ = srv.accept()

        def one(c=c):
            with c:
                c.settimeout(200)
                try:
                    handle(emu, c)
                except OSError:
                    pass

        threading.Thread(target=one, daemon=True).start()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--up-port", type=int, required=True, help="the game's devupdate port (the azahar-session port)")
    ap.add_argument("--port", type=int, default=5051, help="the supervisor port to answer on")
    ap.add_argument("--ip", default="127.0.0.1")
    a = ap.parse_args()
    if a.ip != "127.0.0.1":
        print("azsv: loopback only")
        return 2
    serve(Emu(a.ip, a.up_port), a.port)


if __name__ == "__main__":
    sys.exit(main())
