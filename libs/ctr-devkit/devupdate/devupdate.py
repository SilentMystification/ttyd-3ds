#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
"""PC side of ctr-devkit devupdate: install and relaunch a running 3DS title.

    devupdate.py ping
    devupdate.py push build/app.cia [--switches "map=x wdog=5"] [--no-relaunch] [--wait 60]
    devupdate.py restart "map=x wdog=5"
    devupdate.py report [--out report.txt]
    devupdate.py launch 000400000FD88600 [--switches ...]
    devupdate.py bootstrap --cia build/app.cia [--boot devupdate-boot.3dsx] [--switches ...]

The console runs an app with devupdate_start() (or the bootstrap 3dsx).
--ip defaults to $N3DS_IP, --port to 5050. Development use only.

push streams the CIA with its CRC32; the console installs it through AM as it
arrives and commits the install only if the CRC matches, so a bad transfer
leaves the old install as it was. Then it relaunches the title with the
switches as APT deliver arguments (--no-relaunch: install only).

bootstrap sends the bootstrap 3dsx with 3dslink to the Homebrew Launcher
(which must be waiting: press Y), waits for it to answer, pushes the CIA and
has it jump into the installed title.

Exit status: 0 done, 1 the console reported an error, 2 no connection or
usage error.
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

MAGIC = b"CDU1"
VERSION = 1
CMD = {"ping": 1, "push": 2, "restart": 3, "report": 4, "launch": 5}
FLAG_RELAUNCH, FLAG_LAUNCH = 1, 2
MAX_ARGS = 0x2F0
THREEDSLINK = ["/opt/devkitpro/tools/bin/3dslink", "3dslink"]
HERE = Path(__file__).resolve().parent


def cia_title_id(data):
    """The title ID from the CIA's TMD (CIA header, then cert chain, ticket, TMD, each 64-aligned)."""
    hdr, _type, _ver, cert, tik, tmd = struct.unpack_from("<IHHIII", data, 0)

    def align(x):
        return (x + 63) & ~63

    off = align(hdr) + align(cert) + align(tik)
    sig = struct.unpack_from(">I", data, off)[0]
    # signature, then padding to 0x40 (the sizes include it)
    sig_len = {0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}.get(sig)
    if sig_len is None:
        raise ValueError(f"unknown TMD signature type 0x{sig:x}")
    body = off + 4 + sig_len
    return struct.unpack_from(">Q", data, body + 0x4C)[0]


def header(cmd, size=0, crc=0, args=b"", title_id=0, flags=0, token=""):
    tok = token.encode()[:31].ljust(32, b"\0")
    return struct.pack("<4sHHIIIIQ32s", MAGIC, VERSION, CMD[cmd], flags, size, crc, len(args), title_id, tok)


class Link:
    """One request: send a header (and payload), read reply lines until done or error."""

    def __init__(self, args):
        self.args = args

    def connect(self, timeout=None):
        s = socket.create_connection((self.args.ip, self.args.port), timeout=timeout or self.args.timeout)
        s.settimeout(self.args.timeout)
        return s

    def lines(self, s, on_line):
        """Reads lines until "done" or "error ..."; returns that last line."""
        buf = b""
        while True:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                return "error timeout waiting for the console"
            if not chunk:
                return "error connection closed by the console"
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode(errors="replace")
                if line == "done" or line.startswith("error "):
                    return line
                payload = on_line(line, s, buf)
                if payload is not None:
                    buf = payload


def print_line(line, s, buf):
    if line:
        print(f"console: {line}", flush=True)


def switch_bytes(text):
    b = (text or "").encode()
    if len(b) > MAX_ARGS:
        raise SystemExit(f"devupdate: switches longer than {MAX_ARGS} bytes")
    return b


def finish(last):
    if last == "done":
        print("devupdate: done")
        return 0
    print(f"devupdate: {last}")
    return 1


def cmd_ping(args):
    link = Link(args)
    with link.connect() as s:
        s.sendall(header("ping", token=args.token))
        return finish(link.lines(s, print_line))


def wait_ping(args, seconds):
    """Polls ping until the console answers; returns the pong line or None."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            with socket.create_connection((args.ip, args.port), timeout=3) as s:
                s.settimeout(5)
                s.sendall(header("ping", token=args.token))
                got = []
                last = Link(args).lines(s, lambda line, *_: got.append(line))
                if last == "done" and got:
                    return got[0]
        except OSError:
            pass
        time.sleep(1)
    return None


def push(args, cia_path, flags, switches, bad_crc=False):
    data = Path(cia_path).read_bytes()
    tid = cia_title_id(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    if bad_crc:
        crc ^= 0xDEADBEEF
    sw = switch_bytes(switches)
    print(f"devupdate: pushing {cia_path} ({len(data)} bytes, title {tid:016x}, crc {crc:08x}) to {args.ip}:{args.port}")
    link = Link(args)
    with link.connect() as s:
        s.sendall(header("push", len(data), crc, sw, tid, flags, args.token) + sw)
        result = {}

        def reader():
            result["last"] = link.lines(s, print_line)

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        start = time.monotonic()
        view = memoryview(data)
        try:
            for off in range(0, len(data), 65536):
                if not t.is_alive():
                    break  # the console answered early (an error)
                s.sendall(view[off:off + 65536])
        except OSError as e:
            t.join(5)
            return result.get("last") or f"error send failed: {e}", tid
        t.join(args.timeout + 60)
        el = time.monotonic() - start
        print(f"devupdate: sent in {el:.1f} s ({len(data) / max(el, 1e-6) / 1024:.0f} KiB/s)")
        return result.get("last", "error no answer"), tid


def cmd_push(args):
    flags = 0 if args.no_relaunch else FLAG_RELAUNCH
    if args.launch:
        flags = FLAG_LAUNCH
    last, tid = push(args, args.cia, flags, args.switches, args.bad_crc)
    status = finish(last)
    if status == 0 and args.wait and not args.no_relaunch:
        print(f"devupdate: waiting up to {args.wait} s for the title to answer again")
        time.sleep(2)
        pong = wait_ping(args, args.wait)
        print(f"devupdate: {pong}" if pong else "devupdate: no answer after the relaunch")
        status = 0 if pong else 1
    return status


def cmd_restart(args):
    sw = switch_bytes(" ".join(args.switches))
    link = Link(args)
    with link.connect() as s:
        s.sendall(header("restart", args=sw, token=args.token) + sw)
        return finish(link.lines(s, print_line))


def cmd_launch(args):
    sw = switch_bytes(args.switches)
    link = Link(args)
    with link.connect() as s:
        s.sendall(header("launch", args=sw, title_id=int(args.title_id, 16), token=args.token) + sw)
        return finish(link.lines(s, print_line))


def cmd_report(args):
    link = Link(args)
    out = []

    def on_line(line, s, buf):
        # "report N" / "log N" are followed by N bytes of text
        parts = line.split()
        if len(parts) == 2 and parts[0] in ("report", "log") and parts[1].isdigit():
            n = int(parts[1])
            while len(buf) < n:
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
            text = buf[:n].decode(errors="replace")
            out.append(f"== {parts[0]} ({n} bytes)\n{text}")
            return buf[n:]
        print_line(line, s, buf)
        return None

    with link.connect() as s:
        s.sendall(header("report", token=args.token))
        last = link.lines(s, on_line)
    text = "\n".join(out)
    if args.out:
        Path(args.out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.out).write_text(text)
        print(f"devupdate: report saved to {args.out}")
    else:
        print(text)
    return finish(last)


def find_3dslink():
    for c in THREEDSLINK:
        if Path(c).is_file() or subprocess.run(["which", c], capture_output=True).returncode == 0:
            return c
    raise SystemExit("devupdate: 3dslink not found (install devkitPro's 3ds-tools)")


def cmd_bootstrap(args):
    boot = Path(args.boot)
    if not boot.is_file():
        raise SystemExit(f"devupdate: {boot} not found (build libs/ctr-devkit/examples/devupdate)")
    pong = None if args.no_send else wait_ping(args, 1)
    if pong is None:
        if not args.no_send:
            print(f"devupdate: sending {boot} with 3dslink (the Homebrew Launcher must be waiting: press Y)")
            r = subprocess.run([find_3dslink(), "-a", args.ip, str(boot)], timeout=120)
            if r.returncode != 0:
                print("devupdate: 3dslink failed")
                return 2
        pong = wait_ping(args, args.boot_wait)
        if pong is None:
            print(f"devupdate: the bootstrap did not answer on {args.ip}:{args.port}")
            return 2
    print(f"devupdate: bootstrap up: {pong}")
    last, tid = push(args, args.cia, FLAG_LAUNCH, args.switches)
    return finish(last)


# ---- azahar-session: one long-lived headless Azahar to run the same commands against ----

AZAHAR_APP = "org.azahar_emu.Azahar"
AZAHAR_CONFIG = """[System]
is_new_3ds=true
is_new_3ds\\default=false
lle_applets=false
lle_applets\\default=false

[Renderer]
frame_limit=100
frame_limit\\default=false
graphics_api=1
graphics_api\\default=false

[UI]
firstStart=false
firstStart\\default=false
confirmClose=false
confirmClose\\default=false
pauseWhenInBackground=false
pauseWhenInBackground\\default=false
check_for_update_on_start=false
check_for_update_on_start\\default=false
"""


def az_paths(profile):
    profile = Path(profile).resolve()
    return profile, profile / "config", profile / "data", profile / "data/azahar-emu/sdmc", profile / "devupdate-session.json"


def az_cmd(profile, args, x11=False):
    profile, config, data, _, _ = az_paths(profile)
    # x11: a headless Xvfb. Under a Wayland session Flatpak does not grant the
    # app's fallback X11 socket, so ask for it. No --die-with-parent: the
    # session outlives the "start" command; "stop" ends it.
    extra = ["--socket=x11", "--nosocket=wayland"] if x11 else []
    return ["flatpak", "run", *extra, f"--filesystem={profile}", "--command=sh", AZAHAR_APP, "-c",
            'export XDG_CONFIG_HOME="$1" XDG_DATA_HOME="$2"; shift 2; exec azahar "$@"', "sh", str(config),
            str(data)] + args


def session_pids(sid):
    pids = []
    for d in Path("/proc").iterdir():
        if d.name.isdigit():
            try:
                stat = (d / "stat").read_text()
                if int(stat[stat.rindex(")") + 2:].split()[3]) == sid:
                    pids.append(int(d.name))
            except (OSError, ValueError):
                pass
    return pids


def acquire_slot(wait_s):
    """One of the machine-wide Azahar slots (flock'd files, AZAHAR_MAX, default 3). In the TTYD
    repository this is tools/port3ds/run_azahar.py's acquire_slot; elsewhere the same scheme.
    Returns the locked fd: whoever holds it open holds the slot (the session's Azahar inherits it)."""
    tools = HERE.parents[2] / "tools/port3ds"
    if (tools / "run_azahar.py").is_file():
        sys.path.insert(0, str(tools))
        try:
            import run_azahar
            if hasattr(run_azahar, "acquire_slot"):
                run_azahar.acquire_slot()
                return run_azahar._slot_fd
        finally:
            sys.path.pop(0)
    import fcntl
    n = int(os.environ.get("AZAHAR_MAX", "3"))
    run = os.environ.get("XDG_RUNTIME_DIR")
    d = Path(run) / "ttyd-azahar" if run and os.access(run, os.W_OK) else Path(f"/tmp/ttyd-azahar-{os.getuid()}")
    d.mkdir(parents=True, exist_ok=True)
    end = time.monotonic() + wait_s
    while True:
        for i in range(n):
            fd = os.open(d / f"azahar-slot-{i}", os.O_RDWR | os.O_CREAT, 0o666)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                os.ftruncate(fd, 0)
                os.write(fd, f"{os.getpid()}\n".encode())
                return fd
            except OSError:
                os.close(fd)
        if time.monotonic() > end:
            return None
        print("devupdate: waiting for a free Azahar slot", flush=True)
        time.sleep(15)


def kill_session(sid):
    import signal
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for pid in session_pids(sid):
            try:
                os.kill(pid, sig)
            except OSError:
                pass
        for _ in range(20):
            if not session_pids(sid):
                return
            time.sleep(0.25)


def free_display():
    for n in range(90, 140):
        if not Path(f"/tmp/.X11-unix/X{n}").exists() and not Path(f"/tmp/.X{n}-lock").exists():
            return n
    raise SystemExit("devupdate: no free X display for Xvfb")


def installed_app(sdmc, tid):
    content = (sdmc / "Nintendo 3DS" / ("0" * 32) / ("0" * 32) / "title" / f"{tid >> 32:08x}" /
               f"{tid & 0xFFFFFFFF:08x}" / "content")
    apps = sorted(content.glob("*.app"), key=lambda p: p.stat().st_mtime)
    return apps[-1] if apps else None


def cmd_azahar_session(args):
    import json
    profile, config, data, sdmc, state_file = az_paths(args.profile)
    state = json.loads(state_file.read_text()) if state_file.is_file() else None
    alive = bool(state and session_pids(state["sid"]))
    if args.action == "status":
        print(f"devupdate: azahar-session {'running' if alive else 'not running'}" +
              (f" (sid {state['sid']}, display :{state.get('display')})" if alive else ""))
        return 0 if alive else 1
    if args.action == "stop":
        if state:
            kill_session(state["sid"])
            if state.get("xvfb_sid"):
                kill_session(state["xvfb_sid"])
            state_file.unlink(missing_ok=True)
        print("devupdate: azahar-session stopped" if alive else "devupdate: azahar-session was not running")
        return 0
    # start
    if alive:
        print(f"devupdate: azahar-session already running (sid {state['sid']})")
        return 0
    if not args.boot:
        raise SystemExit("devupdate: azahar-session start needs --boot <app.cia|app.3dsx>")
    boot = Path(args.boot).resolve()
    if not str(boot).startswith(str(Path.home())) or not str(profile).startswith(str(Path.home())):
        raise SystemExit("devupdate: the profile and the build must be under the home directory (Flatpak)")
    (config / "azahar-emu").mkdir(parents=True, exist_ok=True)
    sdmc.mkdir(parents=True, exist_ok=True)
    ini = config / "azahar-emu/qt-config.ini"
    if not ini.exists():
        ini.write_text(AZAHAR_CONFIG)
    slot = acquire_slot(args.slot_wait)
    if slot is None:
        print(f"devupdate: no free Azahar slot after {args.slot_wait} s")
        return 2
    if boot.suffix == ".cia":
        r = subprocess.run(az_cmd(profile, ["-i", str(boot)]), capture_output=True, text=True, timeout=180)
        if "Installed CIA successfully" not in r.stdout + r.stderr:
            print((r.stdout + r.stderr)[-1500:])
            return 2
        target = installed_app(sdmc, cia_title_id(boot.read_bytes()))
        if target is None:
            raise SystemExit("devupdate: no installed .app after the CIA install")
    else:
        target = boot
    env = dict(os.environ)
    xvfb_sid, display = None, None
    if not args.window and subprocess.run(["which", "Xvfb"], capture_output=True).returncode == 0:
        display = free_display()
        xv = subprocess.Popen(["Xvfb", f":{display}", "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        xvfb_sid = xv.pid
        for _ in range(40):
            if Path(f"/tmp/.X11-unix/X{display}").exists():
                break
            time.sleep(0.1)
        env["DISPLAY"] = f":{display}"
        env.pop("WAYLAND_DISPLAY", None)
        env["QT_QPA_PLATFORM"] = "xcb"
    out = open(profile / "devupdate-session-azahar.txt", "w")
    # The slot's fd goes to Azahar: the slot stays taken exactly as long as this instance runs.
    proc = subprocess.Popen(az_cmd(profile, [str(target)], x11=display is not None), stdout=out,
                            stderr=subprocess.STDOUT, env=env,
                            start_new_session=True, pass_fds=(slot,))
    os.close(slot)
    state = {"sid": proc.pid, "xvfb_sid": xvfb_sid, "display": display, "boot": str(target)}
    state_file.write_text(json.dumps(state))
    print(f"devupdate: azahar-session started (sid {proc.pid}{f', Xvfb :{display}' if display else ''}), "
          f"booted {target.name}")
    if args.wait:
        args.ip = args.ip or "127.0.0.1"
        pong = wait_ping(args, args.wait)
        if pong is None:
            print(f"devupdate: no devupdate answer on {args.ip}:{args.port} after {args.wait} s (session left running)")
            return 1
        print(f"devupdate: {pong}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("N3DS_IP"), help="console address (default $N3DS_IP)")
    ap.add_argument("--port", type=int, default=5050)
    ap.add_argument("--token", default="", help="shared secret, if the app set one")
    ap.add_argument("--timeout", type=float, default=30.0, help="seconds without an answer before giving up")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping", help="title ID, version, free space")
    p = sub.add_parser("push", help="install a CIA and relaunch")
    p.add_argument("cia")
    p.add_argument("--switches", default="", help="deliver arguments for the relaunch")
    p.add_argument("--no-relaunch", action="store_true", help="install only")
    p.add_argument("--launch", action="store_true", help="jump to the pushed title (for another title)")
    p.add_argument("--wait", type=float, default=0, help="after the relaunch, wait this long for a ping answer")
    p.add_argument("--bad-crc", action="store_true", help="send a wrong CRC (test: the install must be refused)")
    p = sub.add_parser("restart", help="relaunch with switches")
    p.add_argument("switches", nargs="*")
    p = sub.add_parser("report", help="the crash report and the log tail")
    p.add_argument("--out")
    p = sub.add_parser("launch", help="jump into an installed title")
    p.add_argument("title_id", help="hex, e.g. 000400000FD88600")
    p.add_argument("--switches", default="")
    p = sub.add_parser("bootstrap", help="3dslink the bootstrap, push the CIA, jump into it")
    p.add_argument("--cia", required=True)
    p.add_argument("--boot", default=str(HERE.parents[2] / "build/3ds/ctr-devkit/examples/devupdate/devupdate-boot.3dsx"))
    p.add_argument("--switches", default="")
    p.add_argument("--boot-wait", type=float, default=30, help="seconds to wait for the bootstrap to answer")
    p.add_argument("--no-send", action="store_true", help="the bootstrap already runs")
    p = sub.add_parser("azahar-session", help="one long-lived headless Azahar for the same commands (--ip 127.0.0.1)")
    p.add_argument("action", choices=["start", "stop", "status"])
    p.add_argument("--profile", required=True, help="Azahar profile folder (config, NAND, SD card), under $HOME")
    p.add_argument("--boot", help="start: the .cia (installed, then its .app booted) or .3dsx")
    p.add_argument("--wait", type=float, default=120, help="start: wait this long for a devupdate ping (0 = no wait)")
    p.add_argument("--window", action="store_true", help="start: on this display instead of a headless Xvfb")
    p.add_argument("--slot-wait", type=float, default=900, help="start: wait this long for one of the 3 Azahar slots")
    args = ap.parse_args()
    if args.cmd == "azahar-session":
        return cmd_azahar_session(args)
    if not args.ip:
        ap.error("give --ip or set N3DS_IP (Azahar: --ip 127.0.0.1)")
    try:
        return {"ping": cmd_ping, "push": cmd_push, "restart": cmd_restart, "report": cmd_report,
                "launch": cmd_launch, "bootstrap": cmd_bootstrap}[args.cmd](args)
    except OSError as e:
        print(f"devupdate: no connection to {args.ip}:{args.port}: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
