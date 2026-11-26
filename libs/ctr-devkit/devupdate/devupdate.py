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
    devupdate.py files put a.tpad b.tpad [--dest sub] | get remote [-o local] | ls [dir] | rm path...
    devupdate.py log [--prev | --crash] [-o FILE]
    devupdate.py queue add manifest.json [files...] | list | clear | start | status | pull DIR
    devupdate.py supervisor ping | status | terminate | launch [SWITCHES] | push CIA [--switches ...]
                 | files get|ls PATH [-o FILE] | log [--prev|--crash|--sv] | watch N | reboot [--hard|--dry]
                 | exit | self-update SUPERVISOR.CIA   [--sv-port 5051]
    devupdate.py loop manifest.json [--run NAME]      the unattended runner (supervisor first, 5050 fallback)

The console runs an app with devupdate_start() (or the bootstrap 3dsx).
--ip defaults to $N3DS_IP, --port to 5050. Development use only.

push streams the CIA with its CRC32; the console installs it through AM as it
arrives and commits the install only if the CRC matches, so a bad transfer
leaves the old install as it was. Then it relaunches the title with the
switches as APT deliver arguments (--no-relaunch: install only).

bootstrap sends the bootstrap 3dsx with 3dslink to the Homebrew Launcher
(which must be waiting: press Y), waits for it to answer, pushes the CIA and
has it jump into the installed title.

files works in one folder the app chose (e.g. sdmc:/3ds/<app>/queue/), with a CRC32 check.
A path starting "@/" is in the app's read-only folder instead (get and ls only):
`files get @/latest.log`, `files ls @/`. log prints the game's latest.log tail.

queue runs a list of test runs on the console without anyone there. The manifest
is JSON, a list or {"defaults": {...}, "entries": [...]}; an entry is
  {"name": "gor_01", "switches": "map=gor_01 story=30", "padfile": "walk.tpad",
   "until": "regex on the app's log", "frames": 0, "timeout_s": 300,
   "shots": 0, "collect": ["shots"]}
(padfile: a local file, uploaded with the queue; until: a small regex, no groups or
counts; shots N needs --shots-switches, e.g. "shots={n} shotmax=40"; collect: app
paths moved to out/<run>_<N>_<name>/ when the entry ends). `queue start` relaunches
the title entry by entry; each entry ends on its until pattern, its frame count, or
its timeout, a crash is recorded and the queue goes on. `queue pull DIR` fetches
results.txt and the collected files.

Exit status: 0 done, 1 the console reported an error, 2 no connection or
usage error.
"""
import argparse
import json
import os
import re
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
CMD = {"ping": 1, "push": 2, "restart": 3, "report": 4, "launch": 5, "files": 6, "queue": 7,
       # 0x100 and up: the supervisor's own commands (libs/ctr-devkit/supervisor/, include/ctrdk/supervisor.h)
       "sv_status": 0x101, "sv_terminate": 0x102, "sv_launch": 0x103, "sv_watch": 0x104, "sv_reboot": 0x105,
       "sv_exit": 0x106}
SV_TITLE_ID = 0x0004013000FD8A02  # CTRDK_SV_TITLE_ID: a push of this title is the supervisor's self-update
SV_PORT = 5051
FLAG_RELAUNCH, FLAG_LAUNCH, FLAG_VERSION, FLAG_STAGE = 1, 2, 4, 8
VERSION_MAX = 47  # the console keeps at most this many characters of the incoming version text
# Console errors after which a direct install is worth retrying through the SD card (--stage):
# crc mismatch, AM start, connection closed, receive timeout, AM write, AM commit.
STAGE_RETRY_CODES = {5, 13, 14, 15, 17, 18}
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


def version_text(args, cia_path):
    """The incoming build's version text: --version-text, else the first line of the sidecar
    next to the CIA (<name>.version, written by the build), else none (the console shows "?")."""
    text = getattr(args, "version_text", None)
    if text is None:
        side = Path(cia_path).with_suffix(".version")
        text = side.read_text().splitlines()[0].strip() if side.exists() and side.read_text().strip() else ""
    return text.encode()[:VERSION_MAX]


def request_prefix(cmd, flags, switches, vt, size=0, crc=0, tid=0, token=""):
    """Header plus argument area, exactly as sent: with a version text the argument area is
    one length byte, the text, then the switch text, and the FLAG_VERSION bit is set."""
    sw = switches
    if vt:
        sw = bytes([len(vt)]) + vt + sw
        flags |= FLAG_VERSION
    return header(cmd, size, crc, sw, tid, flags, token) + sw


def push(args, cia_path, flags, switches, bad_crc=False, stage=False):
    data = Path(cia_path).read_bytes()
    tid = cia_title_id(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    if bad_crc:
        crc ^= 0xDEADBEEF
    sw = switch_bytes(switches)
    vt = version_text(args, cia_path)
    if stage:
        flags |= FLAG_STAGE
    print(f"devupdate: pushing {cia_path} ({len(data)} bytes, title {tid:016x}, crc {crc:08x}) to {args.ip}:{args.port}"
          + (" via the SD card (--stage)" if stage else ""))
    link = Link(args)
    with link.connect() as s:
        s.sendall(request_prefix("push", flags, sw, vt, len(data), crc, tid, args.token))
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
                if not stage and getattr(args, "break_after", 0) and off >= args.break_after * 1024:
                    # test aid: a network that breaks mid-stream (the console must cancel and resume)
                    s.shutdown(socket.SHUT_RDWR)
                    t.join(5)
                    return "error 14 connection closed by this tool (--break-after)", tid
                s.sendall(view[off:off + 65536])
        except OSError as e:
            t.join(5)
            return result.get("last") or f"error send failed: {e}", tid
        t.join(args.timeout + 60)
        el = time.monotonic() - start
        print(f"devupdate: sent in {el:.1f} s ({len(data) / max(el, 1e-6) / 1024:.0f} KiB/s)")
        return result.get("last", "error no answer"), tid


def ping_line(args, timeout=5):
    """The console's pong line, or None if it does not answer."""
    got = []
    link = Link(args)
    try:
        with link.connect(timeout) as s:
            s.sendall(header("ping", token=args.token))
            last = link.lines(s, lambda line, *_: got.append(line))
    except OSError:
        return None
    if last != "done":
        return None
    return next((line for line in got if line.startswith("pong ")), None)


def fetch_report(args):
    """The crash report and log tail as one text, and the last reply line."""
    link = Link(args)
    out = []

    def on_line(line, s, buf):
        parts = line.split()
        if len(parts) == 2 and parts[0] in ("report", "log") and parts[1].isdigit():
            n = int(parts[1])
            while len(buf) < n:
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
            out.append(f"== {parts[0]} ({n} bytes)\n{buf[:n].decode(errors='replace')}")
            return buf[n:]
        return None

    with link.connect() as s:
        s.sendall(header("report", token=args.token))
        last = link.lines(s, on_line)
    return "\n".join(out), last


def preflight_report(args):
    """Before an install or a relaunch: if the console is in recovery mode (it crashed
    and waits there), save its crash report under build/devupdate-last/ and print the path."""
    if getattr(args, "no_report", False):
        return
    pong = ping_line(args)
    if pong is None or "recovery=1" not in pong.split():
        return
    text, last = fetch_report(args)
    if last != "done" or not text:
        print(f"devupdate: the console is in recovery mode; no crash report ({last})")
        return
    out = HERE.parents[2] / "build/devupdate-last"
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"crash-{time.strftime('%Y%m%d-%H%M%S')}.txt"
    path.write_text(text)
    print(f"devupdate: the console is in recovery mode; crash report saved to {path}")


def error_code(last):
    m = re.match(r"error (\d+)", last or "")
    return int(m.group(1)) if m else None


def push_with_fallback(args, cia, flags, switches):
    """A direct install (the CIA streams into AM as it arrives); if the console reports a failure of
    that kind (AM write or commit, a broken connection, a CRC mismatch), the same CIA goes through the
    SD card (--stage) once. The console cancels each failed install and resumes the game, so the retry
    needs nothing else. --stage starts staged; --no-stage-fallback and --bad-crc never retry."""
    stage = getattr(args, "stage", False)
    last, tid = push(args, cia, flags, switches, args.bad_crc, stage)
    if (last != "done" and not stage and not args.bad_crc and not getattr(args, "no_stage_fallback", False)
            and (error_code(last) in STAGE_RETRY_CODES or last.startswith("error timeout")
                 or last.startswith("error connection closed") or last.startswith("error send failed"))):
        print(f"devupdate: the direct install failed ({last}); retrying through the SD card (--stage)")
        time.sleep(4)  # the console shows the error for 3 s before it resumes the game
        last, tid = push(args, cia, flags, switches, args.bad_crc, True)
    return last, tid


def cmd_push(args):
    preflight_report(args)
    flags = 0 if args.no_relaunch else FLAG_RELAUNCH
    if args.launch:
        flags = FLAG_LAUNCH
    last, tid = push_with_fallback(args, args.cia, flags, args.switches)
    status = finish(last)
    if status == 0 and args.wait and not args.no_relaunch:
        print(f"devupdate: waiting up to {args.wait} s for the title to answer again")
        time.sleep(2)
        pong = wait_ping(args, args.wait)
        print(f"devupdate: {pong}" if pong else "devupdate: no answer after the relaunch")
        status = 0 if pong else 1
    return status


def cmd_restart(args):
    preflight_report(args)
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
    last, tid = push(args, args.cia, FLAG_LAUNCH, args.switches, False, getattr(args, "stage", False))
    return finish(last)


# ---- files and queue ----

NAME_RE = re.compile(r"^[A-Za-z0-9._-]{1,40}$")
REL_RE = re.compile(r"^[A-Za-z0-9._+-]+(/[A-Za-z0-9._+-]+)*$")
QUEUE_FIELDS = ("name", "switches", "padfile", "until", "frames", "timeout_s", "collect")


def request(args, cmd, argtext="", payload=b"", on_line=None, timeout=None):
    """One files/queue request: the reply lines (all of them) and the last one ("done" or "error ...")."""
    a = argtext.encode()
    if len(a) > MAX_ARGS:
        raise SystemExit("devupdate: argument too long")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    link = Link(args)
    lines = []

    def collect(line, s, buf):
        lines.append(line)
        return on_line(line, s, buf) if on_line else None

    with link.connect(timeout) as s:
        s.sendall(header(cmd, len(payload), crc, a, token=args.token) + a)
        if payload:
            try:
                s.sendall(payload)
            except OSError:
                pass  # the console refused early: its error line is waiting
        last = link.lines(s, collect)
    return last, lines


def files_put(args, local, remote):
    data = Path(local).read_bytes()
    if not REL_RE.match(remote) or ".." in remote.split("/"):
        raise SystemExit(f"devupdate: bad remote name {remote!r} (letters, digits, . _ - + and /)")
    last, _ = request(args, "files", f"put {remote}", data)
    return last, len(data)


def files_get(args, remote):
    """The file's bytes (CRC checked) or raises RuntimeError."""
    got = {}

    def on_line(line, s, buf):
        p = line.split()
        if len(p) == 3 and p[0] == "file" and p[1].isdigit():
            n = int(p[1])
            while len(buf) < n + 1:
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
            got["data"], got["crc"] = buf[:n], int(p[2], 16)
            return buf[n:]
        return None

    last, _ = request(args, "files", f"get {remote}", on_line=on_line)
    if last != "done" or "data" not in got:
        raise RuntimeError(last)
    if zlib.crc32(got["data"]) & 0xFFFFFFFF != got["crc"]:
        raise RuntimeError("crc mismatch on download")
    return got["data"]


def files_ls(args, remote=""):
    """[(is_dir, size, name)] or raises RuntimeError."""
    last, lines = request(args, "files", f"ls {remote}".rstrip())
    if last != "done":
        raise RuntimeError(last)
    out = []
    for line in lines:
        p = line.split(" ", 3)
        if len(p) == 4 and p[0] == "entry":
            out.append((p[1] == "d", int(p[2]), p[3]))
    return out


def cmd_files(args):
    act = args.action
    if act == "put":
        if not args.paths:
            raise SystemExit("devupdate: files put <file>... [--dest dir]")
        status = 0
        for p in args.paths:
            remote = (args.dest.rstrip("/") + "/" if args.dest else "") + Path(p).name
            last, n = files_put(args, p, remote)
            print(f"devupdate: put {p} -> {remote} ({n} bytes): {last}")
            status |= 0 if last == "done" else 1
        return status
    if act == "get":
        if not args.paths:
            raise SystemExit("devupdate: files get <remote> [-o file]")
        try:
            data = files_get(args, args.paths[0])
        except RuntimeError as e:
            print(f"devupdate: {e}")
            return 1
        out = Path(args.out or Path(args.paths[0]).name)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        print(f"devupdate: {args.paths[0]} -> {out} ({len(data)} bytes)")
        return 0
    if act == "ls":
        try:
            for is_dir, size, name in sorted(files_ls(args, args.paths[0] if args.paths else "")):
                print(f"{'d' if is_dir else '-'} {size:>9} {name}")
        except RuntimeError as e:
            print(f"devupdate: {e}")
            return 1
        return 0
    status = 0
    for p in args.paths:
        last, _ = request(args, "files", f"rm {p}")
        print(f"devupdate: rm {p}: {last}")
        status |= 0 if last == "done" else 1
    return status


def cmd_log(args):
    """The game's latest.log (or previous.log, the last run's with --prev): the last 200 lines, or the whole file with -o."""
    remote = "@/crash.txt" if args.crash else "@/previous.log" if args.prev else "@/latest.log"
    try:
        data = files_get(args, remote)
    except RuntimeError as e:
        print(f"devupdate: {remote}: {e}")
        return 1
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        print(f"devupdate: {remote} -> {out} ({len(data)} bytes)")
        return 0
    for line in data.decode(errors="replace").splitlines()[-200:]:
        print(line)
    return 0


def queue_lines(entries, shots_switches=None, shots_collect="shots", defaults=None):
    """Entry dicts -> (queue.txt text, local files to upload as {name: path})."""
    defaults = defaults or {}
    uploads, rows = {}, []
    for i, raw in enumerate(entries, 1):
        e = {**defaults, **raw}
        name = str(e.get("name", ""))
        if not NAME_RE.match(name):
            raise SystemExit(f"devupdate: entry {i}: bad name {name!r} (letters, digits, . _ -, up to 40)")
        switches = str(e.get("switches", "")).strip()
        shots = int(e.get("shots", 0) or 0)
        collect = list(e.get("collect", []) or [])
        if shots:
            if not shots_switches:
                raise SystemExit(f"devupdate: entry {name}: shots needs --shots-switches (the app's switches for a screenshot every N frames)")
            switches = (switches + " " + shots_switches.format(n=shots)).strip()
            if shots_collect and shots_collect not in collect:
                collect.append(shots_collect)
        pad = str(e.get("padfile", "") or "")
        if pad:
            local = Path(pad)
            if local.is_file():
                uploads[local.name] = local
                pad = local.name
            elif not REL_RE.match(pad):
                raise SystemExit(f"devupdate: entry {name}: padfile {pad!r} is not a file here or a plain name")
        until = str(e.get("until", "") or "")
        if any(ch in until for ch in "(){}"):
            raise SystemExit(f"devupdate: entry {name}: until may not use groups or counts ( ) {{ }} (the console's matcher is small)")
        try:
            re.compile(until)
        except re.error as err:
            raise SystemExit(f"devupdate: entry {name}: bad until pattern: {err}")
        fields = [name, switches, pad, until, str(int(e.get("frames", 0) or 0)), str(int(e.get("timeout_s", 0) or 0)),
                  ",".join(collect)]
        for f in fields:
            if "\t" in f or "\n" in f:
                raise SystemExit(f"devupdate: entry {name}: tabs and newlines are not allowed")
        rows.append("\t".join(fields))
    return "\n".join(rows) + "\n", uploads


def queue_add_entries(args, entries, shots_switches=None, shots_collect="shots", defaults=None, extra_files=()):
    """Uploads the padfiles and adds the entries. Returns 0 or 1."""
    text, uploads = queue_lines(entries, shots_switches, shots_collect, defaults)
    for p in extra_files:
        uploads[Path(p).name] = Path(p)
    for name, path in uploads.items():
        last, n = files_put(args, path, name)
        print(f"devupdate: uploaded {name} ({n} bytes): {last}")
        if last != "done":
            return 1
    last, lines = request(args, "queue", "add", text.encode())
    print(f"devupdate: queue add: {next((l for l in lines if l.startswith('queued')), last)}")
    return 0 if last == "done" else 1


def load_manifest(path):
    doc = json.loads(Path(path).read_text())
    if isinstance(doc, list):
        doc = {"entries": doc}
    entries, defaults = doc["entries"], doc.get("defaults", {})
    base = Path(path).resolve().parent
    for e in entries:  # a padfile path in the manifest is relative to the manifest
        p = e.get("padfile")
        if p and not Path(p).is_absolute() and (base / p).is_file():
            e["padfile"] = str(base / p)
    return entries, defaults, doc


def queue_status(args, timeout=6):
    """{'state':..., 'index':..., ...} from `queue status`, or None if the console does not answer."""
    try:
        a = argparse.Namespace(**vars(args))
        a.timeout = timeout
        last, lines = request(a, "queue", "status", timeout=timeout)
    except (OSError, socket.timeout):
        return None
    for line in lines:
        if line.startswith("queue "):
            return dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
    return None


def parse_results(text):
    rows = []
    for line in text.splitlines():
        f = line.split("\t")
        if len(f) >= 6:
            rows.append({"index": int(f[0]), "name": f[1], "result": f[2], "frames": int(f[3]), "ms": int(f[4]),
                         "outdir": f[5], "detail": f[6] if len(f) > 6 else ""})
    return rows


def queue_pull(args, dest):
    """results.txt and every collected file under out/ into dest. Returns the parsed rows."""
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    try:
        text = files_get(args, "results.txt").decode(errors="replace")
    except RuntimeError as e:
        print(f"devupdate: no results.txt ({e})")
        return []
    (dest / "results.txt").write_text(text)
    rows = parse_results(text)
    for row in rows:
        row["files"] = []
        stack = [(f"out/{row['outdir']}", dest / row["outdir"])]
        while stack:
            remote, local = stack.pop()
            try:
                items = files_ls(args, remote)
            except (RuntimeError, OSError):
                continue
            for is_dir, size, name in items:
                if is_dir:
                    stack.append((f"{remote}/{name}", local / name))
                    continue
                try:
                    data = files_get(args, f"{remote}/{name}")
                except (RuntimeError, OSError) as e:
                    print(f"devupdate: {remote}/{name}: {e}")
                    continue
                local.mkdir(parents=True, exist_ok=True)
                (local / name).write_bytes(data)
                row["files"].append(str((local / name).relative_to(dest)))
    return rows


def cmd_queue(args):
    act = args.action
    if act == "add":
        if not args.paths:
            raise SystemExit("devupdate: queue add <manifest.json> [files...]")
        entries, defaults, doc = load_manifest(args.paths[0])
        return queue_add_entries(args, entries, args.shots_switches or doc.get("shots_switches"),
                                 args.shots_collect or doc.get("shots_collect", "shots"), defaults, args.paths[1:])
    if act == "list":
        last, lines = request(args, "queue", "list")
        for line in lines:
            if line.startswith("q "):
                num, _, rest = line[2:].partition("\t")
                f = rest.split("\t")
                if len(f) == len(QUEUE_FIELDS):
                    d = dict(zip(QUEUE_FIELDS, f))
                    print(f"{num:>3} {d['name']:<24} {d['switches']}" + (f" [pad {d['padfile']}]" if d["padfile"] else "") +
                          (f" [until /{d['until']}/]" if d["until"] else "") + (f" [{d['frames']} frames]" if d["frames"] != "0" else "") +
                          f" [timeout {d['timeout_s']} s]" + (f" [collect {d['collect']}]" if d["collect"] else ""))
            elif line and line != "done":
                print(f"console: {line}")
        return 0 if last == "done" else 1
    if act == "status":
        st = queue_status(args, args.timeout)
        print("devupdate: queue " + (" ".join(f"{k}={v}" for k, v in st.items()) if st else "(no answer)"))
        return 0 if st else 2
    if act == "pull":
        rows = queue_pull(args, args.paths[0] if args.paths else "queue-results")
        for r in rows:
            print(f"{r['index']:>3} {r['name']:<24} {r['result']:<8} {r['frames']:>6} frames {r['ms'] / 1000:7.1f} s  {len(r['files'])} files  {r['detail']}")
        return 0
    if act == "start":
        preflight_report(args)
    last, lines = request(args, "queue", act)  # clear, start
    for line in lines:
        if line != "done":
            print(f"console: {line}")
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


SLOT_INDEX = None  # the Azahar slot the last acquire_slot took (TTYD's run_azahar.py scheme), else None


def devupdate_port(index):
    """A slot's devupdate port in the TTYD repository: 5050 + 10 + slot index (run_azahar.devupdate_port)."""
    return 5050 + 10 + index


def set_debug_port(sdmc, port):
    """Points the game's devupdateport switch (sdmc:/3ds/ttyd/debug3ds.txt, written by the caller) at port."""
    path = sdmc / "3ds/ttyd/debug3ds.txt"
    words = path.read_text().split() if path.is_file() else ["devupdate"]
    words = [w for w in words if not w.startswith("devupdateport=")] + [f"devupdateport={port}"]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(" ".join(words) + "\n")


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
                # a session is never reaped; --slot-wait bounds the wait
                if not run_azahar.acquire_slot(kind="session", wait_s=wait_s):
                    return None
                global SLOT_INDEX
                SLOT_INDEX = run_azahar._slot_index
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


EMU_MARKER = "3ds/ctrdk-emulate-relaunch"
EMU_ARGS = "3ds/ctrdk-launch-args.txt"


def az_spawn(profile, target, display, slot):
    """Starts Azahar on the .app/.3dsx; the slot's fd stays taken as long as it runs."""
    env = dict(os.environ)
    if display is not None:
        env["DISPLAY"] = f":{display}"
        env.pop("WAYLAND_DISPLAY", None)
        env["QT_QPA_PLATFORM"] = "xcb"
    out = open(profile / "devupdate-session-azahar.txt", "a")
    return subprocess.Popen(az_cmd(profile, [str(target)], x11=display is not None), stdout=out,
                            stderr=subprocess.STDOUT, env=env, start_new_session=True, pass_fds=(slot,))


def az_supervise(args):
    """Emulates the APT relaunch Azahar cannot do. The library (when EMU_MARKER exists) writes the next
    launch's deliver arguments to EMU_ARGS right before it ends the process; this loop sees that, ends the
    instance if it is still around, and starts the title again, which reads (and deletes) the file."""
    profile, _, _, sdmc, state_file = az_paths(args.profile)
    argsfile = sdmc / EMU_ARGS
    pending, since = False, 0.0
    print(f"supervisor {os.getpid()} up", flush=True)
    while True:
        state = json.loads(state_file.read_text()) if state_file.is_file() else None
        if not state or state.get("supervisor") != os.getpid():
            print("supervisor: session stopped, leaving", flush=True)
            return 0
        if pending:
            if not argsfile.exists() or time.monotonic() - since > 180:
                pending = False
        elif argsfile.exists():
            end = time.monotonic() + 8
            while session_pids(state["sid"]) and time.monotonic() < end:
                time.sleep(0.25)
            kill_session(state["sid"])
            proc = az_spawn(profile, state["boot"], state.get("display"), args.slot_fd)
            state["sid"] = proc.pid
            state["relaunches"] = state.get("relaunches", 0) + 1
            state_file.write_text(json.dumps(state))
            print(f"supervisor: relaunch {state['relaunches']}: Azahar sid {proc.pid} "
                  f"({argsfile.read_text(errors='replace')[:100]!r})", flush=True)
            pending, since = True, time.monotonic()
        time.sleep(0.5)


def cmd_azahar_session(args):
    profile, config, data, sdmc, state_file = az_paths(args.profile)
    if args.action == "supervise":
        return az_supervise(args)
    state = json.loads(state_file.read_text()) if state_file.is_file() else None
    alive = bool(state and (session_pids(state["sid"]) or (state.get("supervisor") and session_pids(state["supervisor"]))))
    if args.action == "status":
        print(f"devupdate: azahar-session {'running' if alive else 'not running'}" +
              (f" (sid {state['sid']}, display :{state.get('display')}, relaunches {state.get('relaunches', 0)})" if alive else ""))
        return 0 if alive else 1
    if args.action == "stop":
        if state:
            if state.get("supervisor"):
                kill_session(state["supervisor"])
            kill_session(state["sid"])
            if state.get("xvfb_sid"):
                kill_session(state["xvfb_sid"])
            state_file.unlink(missing_ok=True)
        (sdmc / EMU_MARKER).unlink(missing_ok=True)
        (sdmc / EMU_ARGS).unlink(missing_ok=True)
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
    # Each session listens on its own port (its slot's), unless --port says otherwise: one agent's push
    # cannot reach another agent's instance.
    if not args.port_given and SLOT_INDEX is not None:
        args.port = devupdate_port(SLOT_INDEX)
    set_debug_port(sdmc, args.port)
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
    (profile / "devupdate-session-azahar.txt").write_text("")
    if args.supervise:
        # Azahar cannot do the APT relaunch (a restart, a queue's next entry): the library, told by this marker,
        # leaves the deliver arguments in a file, and the supervisor starts the title again.
        (sdmc / "3ds").mkdir(parents=True, exist_ok=True)
        (sdmc / EMU_MARKER).write_text("1\n")
        (sdmc / EMU_ARGS).unlink(missing_ok=True)
    # The slot's fd goes to Azahar: the slot stays taken exactly as long as this instance runs.
    proc = az_spawn(profile, target, display, slot)
    state = {"sid": proc.pid, "xvfb_sid": xvfb_sid, "display": display, "boot": str(target), "port": args.port}
    if args.supervise:
        sup = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "azahar-session", "supervise",
                                "--profile", str(profile), "--slot-fd", str(slot)],
                               stdout=open(profile / "devupdate-supervisor.txt", "w"), stderr=subprocess.STDOUT,
                               start_new_session=True, pass_fds=(slot,))
        state["supervisor"] = sup.pid
    state_file.write_text(json.dumps(state))
    os.close(slot)
    print(f"devupdate: azahar-session started on port {args.port} (sid {proc.pid}{f', Xvfb :{display}' if display else ''}"
          f"{', supervised' if args.supervise else ''}), booted {target.name}")
    if args.wait:
        args.ip = args.ip or "127.0.0.1"
        pong = wait_ping(args, args.wait)
        if pong is None:
            print(f"devupdate: no devupdate answer on {args.ip}:{args.port} after {args.wait} s (session left running)")
            return 1
        print(f"devupdate: {pong}")
    return 0


def sv_args(args, timeout=None):
    """A copy of args whose port is the supervisor's (Link, request, push and files_get use args.port)."""
    a = argparse.Namespace(**vars(args))
    a.port = getattr(args, "sv_port", SV_PORT)
    if timeout is not None:
        a.timeout = timeout
    return a


def sv_request(args, cmd, argtext="", timeout=None):
    """One supervisor request on --sv-port; returns (last line, the other lines). Raises OSError."""
    a = sv_args(args, timeout)
    got = []
    sw = argtext.encode()
    if len(sw) > MAX_ARGS:
        raise SystemExit("devupdate: argument too long")
    with socket.create_connection((a.ip, a.port), timeout=a.timeout) as s:
        s.settimeout(a.timeout)
        s.sendall(header(cmd, args=sw, token=a.token) + sw)
        last = Link(a).lines(s, lambda line, *_: got.append(line))
    return last, got


def sv_try(args, cmd, argtext="", timeout=10):
    """sv_request that never raises: ("error no connection: ...", []) when the supervisor does not answer."""
    try:
        return sv_request(args, cmd, argtext, timeout)
    except OSError as e:
        return f"error no connection: {e}", []


def sv_status(args, timeout=10):
    """The supervisor's status as a dict (status key=value words, the heartbeat line split), or None."""
    try:
        last, lines = sv_request(args, "sv_status", timeout=timeout)
    except OSError:
        return None
    if last != "done":
        return None
    st = {}
    for line in lines:
        if not line.startswith("status "):
            continue
        body = line[7:]
        if body.startswith(("last=", "switches=")):
            k, _, v = body.partition("=")
            st[k] = v
            continue
        for word in body.split():
            if "=" in word:
                k, _, v = word.partition("=")
                st.setdefault(k, v)
    return st


def sv_ping(args, tries=3, timeout=10):
    """The supervisor's pong line, or None after `tries` failed attempts."""
    for i in range(tries):
        try:
            last, lines = sv_request(args, "ping", timeout=timeout)
            pong = next((l for l in lines if l.startswith("pong supervisor")), None)
            if last == "done" and pong:
                return pong
        except OSError:
            pass
        if i + 1 < tries:
            time.sleep(2)
    return None


def cmd_supervisor(args):
    """The supervisor system module (libs/ctr-devkit/supervisor/). It answers on its own port (5051)
    whatever the game does: hung, crashed, ended, not running."""
    act = args.action
    rest = args.rest or []
    try:
        if act in ("ping", "status", "terminate", "exit"):
            cmd = {"ping": "ping", "status": "sv_status", "terminate": "sv_terminate", "exit": "sv_exit"}[act]
            argtext = f"gpureset={args.gpureset}" if act == "terminate" and args.gpureset is not None else ""
            last, lines = sv_request(args, cmd, argtext, timeout=max(args.timeout, 20))
        elif act == "launch":
            last, lines = sv_request(args, "sv_launch", " ".join(rest) or args.switches, timeout=max(args.timeout, 30))
        elif act == "watch":
            if len(rest) != 1 or not rest[0].isdigit():
                raise SystemExit("devupdate: supervisor watch N (seconds; 0 = off)")
            last, lines = sv_request(args, "sv_watch", rest[0])
        elif act == "reboot":
            word = "dry" if args.dry else "hard" if args.hard else ""
            last, lines = sv_request(args, "sv_reboot", word)
        elif act in ("push", "self-update"):
            if len(rest) != 1:
                raise SystemExit(f"devupdate: supervisor {act} <file.cia>")
            tid = cia_title_id(Path(rest[0]).read_bytes())
            if (act == "self-update") != (tid == SV_TITLE_ID):
                raise SystemExit(f"devupdate: {rest[0]} has title {tid:016x}: "
                                 + ("not the supervisor's" if act == "self-update" else "the supervisor's (use self-update)"))
            a = sv_args(args, max(args.timeout, 60))
            last, _ = push(a, rest[0], 0, args.switches)
            return finish(last)
        elif act in ("files", "log"):
            return sv_files(args, act, rest)
        else:
            raise SystemExit(f"devupdate: unknown supervisor action {act}")
    except OSError as e:
        print(f"devupdate: supervisor: no answer from {args.ip}:{args.sv_port}: {e}")
        return 2
    for line in lines:
        print(f"supervisor: {line}")
    return finish(last)


def sv_files(args, act, rest):
    """supervisor files get|ls PATH, supervisor log: read-only, served by the supervisor ("@sv/..." is
    its own folder, anything else the app's folder)."""
    a = sv_args(args)
    if act == "log":
        remote = "@sv/supervisor.log" if args.sv else "@/crash.txt" if args.crash else "@/previous.log" if args.prev else "@/latest.log"
        rest = ["get", remote]
    if not rest or rest[0] not in ("get", "ls"):
        raise SystemExit("devupdate: supervisor files get <path> [-o FILE] | ls [path]  (read-only)")
    try:
        if rest[0] == "ls":
            for is_dir, size, name in sorted(files_ls(a, rest[1] if len(rest) > 1 else "")):
                print(f"{'d' if is_dir else '-'} {size:>9} {name}")
            return 0
        data = files_get(a, rest[1])
    except RuntimeError as e:
        print(f"devupdate: supervisor: {e}")
        return 1
    if args.out:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
        print(f"devupdate: {rest[1]} -> {out} ({len(data)} bytes)")
    elif act == "log":
        for line in data.decode(errors="replace").splitlines()[-200:]:
            print(line)
    else:
        out = Path(Path(rest[1]).name)
        out.write_bytes(data)
        print(f"devupdate: {rest[1]} -> {out} ({len(data)} bytes)")
    return 0


# ---- loop: the unattended runner (docs/CTR_DEVKIT_PLAN.md "Supervisor", PC side) ----

LOOP_DEFAULTS = {"cia": "", "switches": "", "until": "", "frames": 0, "timeout_s": 300, "watch": 0,
                 "collect": [], "push_timeout_s": 120, "start_timeout_s": 60, "reboot_wait_s": 90, "poll_s": 5,
                 "log_poll_s": 10, "fallback_settle_s": 15}


class SupervisorGone(Exception):
    pass


def loop_log(path, text):
    line = f"{time.strftime('%H:%M:%S')} {text}"
    print(f"loop: {text}", flush=True)
    with open(path, "a") as f:
        f.write(line + "\n")


def loop_push(args, e, log):
    """Step 2: push through the supervisor (retry once), then through devupdate (5050, with its --stage
    fallback). Returns (ok, how)."""
    a = sv_args(args, e["push_timeout_s"])
    for attempt in (1, 2):
        try:
            last, _ = push(a, e["cia"], 0, e["switches"])
        except OSError as err:
            last = f"error no connection: {err}"
        log(f"push via supervisor (try {attempt}): {last}")
        if last == "done":
            return True, "supervisor"
        time.sleep(3)
    d = argparse.Namespace(**vars(args))
    d.timeout = e["push_timeout_s"]
    d.bad_crc = False
    d.stage = False
    try:
        last, _ = push_with_fallback(d, e["cia"], FLAG_RELAUNCH, e["switches"])
    except OSError as err:
        last = f"error no connection: {err}"
    log(f"push via devupdate {args.ip}:{args.port}: {last}")
    return last == "done", "devupdate"


def loop_wait_start(args, e, log):
    """Step 3: the first heartbeat after the launch. terminate + launch once, then reboot. Returns
    (result, status) with result "started" or "no start"."""
    def wait(seconds):
        end = time.monotonic() + seconds
        st = None
        while time.monotonic() < end:
            st = sv_status(args)
            if st is not None and int(st.get("since_launch", "0") or 0) > 0:
                return True, st
            time.sleep(min(2, e["poll_s"]))
        return False, st

    ok, st = wait(e["start_timeout_s"])
    if ok:
        return "started", st
    log(f"no heartbeat {e['start_timeout_s']} s after the launch: terminate + launch")
    try:
        last, lines = sv_request(args, "sv_launch", e["switches"], timeout=30)
        log(f"launch: {last} ({'; '.join(lines)})")
    except OSError as err:
        log(f"launch: no answer: {err}")
    ok, st = wait(e["start_timeout_s"])
    if ok:
        return "started", st
    log("still no heartbeat: reboot (NS_RebootToTitle; the game's ensure() starts the supervisor again)")
    try:
        last, _ = sv_request(args, "sv_reboot", "", timeout=10)
        log(f"reboot: {last}")
    except OSError as err:
        log(f"reboot: {err}")
    time.sleep(10)
    if sv_ping(args, tries=max(1, e["reboot_wait_s"] // 12), timeout=10) is None:
        raise SupervisorGone("no answer after the reboot")
    ok, st = wait(e["start_timeout_s"])
    return ("started" if ok else "no start"), st


def loop_fetch_log(args):
    """latest.log through the supervisor, else devupdate; b"" if neither answers."""
    for a in (sv_args(args, 20), args):
        try:
            return files_get(a, "@/latest.log")
        except (RuntimeError, OSError):
            continue
    return b""


def loop_run(args, e, log):
    """Step 4: until the pattern (latest.log), the frame count (heartbeat), the timeout, or the game's end."""
    pat = re.compile(e["until"]) if e["until"] else None
    end = time.monotonic() + e["timeout_s"]
    next_log = 0.0
    misses = 0
    while time.monotonic() < end:
        st = sv_status(args)
        if st is None:
            misses += 1
            if misses >= 3 and sv_ping(args) is None:
                raise SupervisorGone("status and ping stopped answering during the run")
        else:
            misses = 0
            frame = int(st.get("frame", "0") or 0)
            if e["frames"] and frame >= e["frames"]:
                return "frames", f"frame {frame}"
            flags = int(st.get("flags", "0") or 0)
            if st.get("game_state") == "stopped":
                return "ended", "the game is not running"
            if flags & 4:
                return "crash", "recovery flag (crash screen)"
        if pat is not None and time.monotonic() >= next_log:
            next_log = time.monotonic() + e["log_poll_s"]
            text = loop_fetch_log(args).decode(errors="replace")
            m = pat.search(text)
            if m:
                return "until", m.group(0)[:120]
        time.sleep(e["poll_s"])
    return "timeout", f"{e['timeout_s']} s"


def loop_collect(args, e, outdir, log):
    """Step 5: latest.log, crash.txt, gpuhang/* and the entry's collect paths, supervisor first."""
    outdir.mkdir(parents=True, exist_ok=True)
    got = []
    wanted = ["latest.log", "crash.txt"] + list(e.get("collect") or [])
    srcs = [sv_args(args, 30), args]
    for remote in wanted:
        for a in srcs:
            try:
                data = files_get(a, f"@/{remote}")
            except (RuntimeError, OSError):
                continue
            dest = outdir / remote
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
            got.append(remote)
            break
    for a in srcs:
        try:
            items = files_ls(a, "@/gpuhang")
        except (RuntimeError, OSError):
            continue
        for is_dir, _, name in items:
            if is_dir:
                continue
            try:
                (outdir / "gpuhang").mkdir(exist_ok=True)
                (outdir / "gpuhang" / name).write_bytes(files_get(a, f"@/gpuhang/{name}"))
                got.append(f"gpuhang/{name}")
            except (RuntimeError, OSError):
                pass
        break
    log(f"collected {len(got)} files: {' '.join(got) or 'none'}")
    return got


def cmd_loop(args):
    """devupdate.py loop manifest.json: per entry ping, push, first heartbeat, run, collect, one row in
    build/loop/<run>/results.tsv. Exit 0 at the end, 2 when the supervisor stops answering."""
    doc = json.loads(Path(args.manifest).read_text())
    if isinstance(doc, list):
        doc = {"entries": doc}
    base = Path(args.manifest).resolve().parent
    defaults = {**LOOP_DEFAULTS, **doc.get("defaults", {})}
    entries = []
    for i, raw in enumerate(doc["entries"], 1):
        e = {**defaults, **raw}
        e.setdefault("name", f"entry{i}")
        if not NAME_RE.match(str(e["name"])):
            raise SystemExit(f"devupdate: loop entry {i}: bad name {e['name']!r}")
        if e["cia"] and not Path(e["cia"]).is_absolute() and (base / e["cia"]).is_file():
            e["cia"] = str(base / e["cia"])
        if e["cia"] and not Path(e["cia"]).is_file():
            raise SystemExit(f"devupdate: loop entry {e['name']}: no file {e['cia']}")
        if e["until"]:
            re.compile(e["until"])
        entries.append(e)
    run = args.run or time.strftime("%Y%m%d-%H%M%S")
    root = Path(args.out) if args.out else HERE.parents[2] / "build/loop"
    rundir = root / run
    rundir.mkdir(parents=True, exist_ok=True)
    logpath = rundir / "loop.log"
    results = rundir / "results.tsv"
    if not results.exists():
        results.write_text("index\tname\tresult\tdetail\tpush\tstart_s\trun_s\tfiles\n")
    print(f"devupdate: loop {len(entries)} entries -> {rundir}")
    rows = []
    current, last_status = None, None
    try:
        for i, e in enumerate(entries, 1):
            current = f"{i} {e['name']}"

            def log(text, _i=i, _n=e["name"]):
                loop_log(logpath, f"[{_i} {_n}] {text}")

            t0 = time.monotonic()
            pong = sv_ping(args, tries=3, timeout=10)  # step 1
            if pong is None:
                raise SupervisorGone("no ping answer (3 tries, 10 s each)")
            log(pong)
            how = "none"
            result, detail = "", ""
            if e["cia"]:  # step 2
                ok, how = loop_push(args, e, log)
                if not ok:
                    result, detail = "push failed", how
                elif how == "devupdate":
                    time.sleep(e["fallback_settle_s"])  # this push did not reset the supervisor's heartbeat count
            else:
                last, lines = sv_try(args, "sv_launch", e["switches"], 30)
                log(f"launch: {last}")
                how = "launch"
                if last != "done":
                    result, detail = "launch failed", last
            t_start, t_run = 0.0, 0.0
            if not result:
                if e["watch"]:
                    last, _ = sv_try(args, "sv_watch", str(int(e["watch"])))
                    log(f"watch {e['watch']}: {last}")
                res, last_status = loop_wait_start(args, e, log)  # step 3
                t_start = time.monotonic() - t0
                if res != "started":
                    result, detail = res, "no heartbeat"
                else:
                    t1 = time.monotonic()
                    result, detail = loop_run(args, e, log)  # step 4
                    t_run = time.monotonic() - t1
            if e["watch"]:
                sv_try(args, "sv_watch", "0")
            files =loop_collect(args, e, rundir / f"{i:02d}_{e['name']}", log)  # step 5
            row = [str(i), e["name"], result, detail.replace("\t", " "), how, f"{t_start:.1f}", f"{t_run:.1f}",
                   ",".join(files)]
            with open(results, "a") as f:  # step 6
                f.write("\t".join(row) + "\n")
            rows.append(row)
            log(f"result {result}: {detail}")
            last_status = sv_status(args) or last_status
    except SupervisorGone as err:
        loop_log(logpath, f"STOP: the supervisor stopped answering ({err}) during entry {current}")
        loop_log(logpath, f"last status: {json.dumps(last_status) if last_status else 'none'}")
        print(f"devupdate: loop stopped at entry {current}: supervisor gone; report in {logpath}")
        for r in rows:
            print("  " + "  ".join(r[:4]))
        return 2
    print(f"devupdate: loop done, {len(rows)} entries ({results})")
    for r in rows:
        print("  " + "  ".join(r[:4]))
    return 0


def cmd_selftest(args):
    """Builds devupdate/selftest_host.c (the device's decoder, proto.c) with gcc and feeds it the
    requests this tool sends: version text of length 0, 1, 14, 47 and (raw) 255, with and without
    switches, with --bad-crc, with and without --stage. The decoded header, version, switches and the
    offset where the payload starts must equal what was meant, so the CIA bytes that follow are not shifted."""
    import tempfile
    exe = Path(tempfile.mkdtemp(prefix="cdu-selftest-")) / "selftest_host"
    cc = subprocess.run(["gcc", "-O1", "-Wall", "-Werror", "-o", str(exe), str(HERE / "selftest_host.c"),
                         str(HERE / "proto.c")], capture_output=True, text=True)
    if cc.returncode != 0:
        print(cc.stderr)
        print("devupdate selftest: cannot build the host decoder")
        return 2
    cia = bytes(range(256)) * 40  # a stand-in payload: framing only, no CIA parsing
    crc_ok = zlib.crc32(cia) & 0xFFFFFFFF
    fails = 0
    count = 0

    def decode(pkt):
        r = subprocess.run([str(exe)], input=pkt, capture_output=True)
        return dict(line.split("=", 1) for line in r.stdout.decode().splitlines() if "=" in line)

    def check(name, pkt, cmd, flags_expect, vt, sw, crc, payload_follows=True):
        nonlocal fails, count
        count += 1
        d = decode(pkt)
        got = {"cmd": d.get("cmd"), "flags": d.get("flags"), "crc": d.get("crc"),
               "version": bytes.fromhex(d.get("version_hex", "")), "switches": bytes.fromhex(d.get("switches_hex", "")),
               "payload_offset": d.get("payload_offset"), "malformed": d.get("malformed")}
        want = {"cmd": str(CMD[cmd]), "flags": str(flags_expect), "crc": f"{crc:08x}", "version": vt[:VERSION_MAX],
                "switches": sw, "payload_offset": str(len(pkt) - len(cia) if payload_follows else len(pkt)), "malformed": "0"}
        ok = got == want
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: flags={d.get('flags')} version={got['version']!r} switches={got['switches']!r} "
              f"payload@{d.get('payload_offset')}")
        if not ok:
            print(f"       want {want}\n       got  {got}")

    print("devupdate selftest: the PC's push packets through the device's decoder (proto.c, host build)")
    for vlen in (0, 1, 14, 47):
        for sw in (b"", b"map=gor_01 wdog=9"):
            for bad in (False, True):
                for stage in (False, True):
                    vt = b"#465 0123abc12"[:14] if vlen == 14 else b"V" * vlen
                    crc = crc_ok ^ 0xDEADBEEF if bad else crc_ok
                    flags = FLAG_RELAUNCH | (FLAG_STAGE if stage else 0)
                    pkt = request_prefix("push", flags, sw, vt, len(cia), crc, 0x0004000000ABCD00, "") + cia
                    check(f"push vt={vlen}{' sw' if sw else ''}{' bad-crc' if bad else ''}{' stage' if stage else ''}",
                          pkt, "push", flags | (FLAG_VERSION if vt else 0), vt, sw, crc)
    # What the PC tool really sends for a 255-character version text: it clamps to 47 itself.
    class A:
        version_text = "X" * 255
    vt = version_text(A, "unused.cia")
    pkt = request_prefix("push", FLAG_RELAUNCH, b"a=1", vt, len(cia), crc_ok, 1, "") + cia
    check("PC tool with a 255-character version text (clamped to 47)", pkt, "push", FLAG_RELAUNCH | FLAG_VERSION, vt, b"a=1", crc_ok)
    # A length byte of 255 in the packet (another client): the device consumes all 255 and keeps 47.
    for sw in (b"", b"a=1"):
        raw = bytes([255]) + b"X" * 255 + sw
        pkt = header("push", len(cia), crc_ok, raw, 1, FLAG_RELAUNCH | FLAG_VERSION) + raw + cia
        count += 1
        d = decode(pkt)
        ok = (bytes.fromhex(d.get("version_hex", "")) == b"X" * VERSION_MAX and bytes.fromhex(d.get("switches_hex", "")) == sw
              and d.get("payload_offset") == str(64 + len(raw)))
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} raw version length byte 255{' sw' if sw else ''}: keeps {VERSION_MAX}, switches {sw!r}, "
              f"payload@{d.get('payload_offset')}")
    # restart with a version text too: the decoder must cope with it on any command.
    pkt = request_prefix("restart", 0, b"hello=1", b"#1 abc")
    check("restart with a version text", pkt, "restart", FLAG_VERSION, b"#1 abc", b"hello=1", 0, False)
    # A malformed area: the length byte promises more than the area holds: kept as switch text, flagged.
    count += 1
    area = b"\x20abc"
    d = decode(header("push", 1, 1, area, 1, FLAG_VERSION) + area)
    ok = d.get("malformed") == "1" and d.get("switches_hex") == area.hex()
    fails += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} malformed version area kept as switch text, flagged malformed={d.get('malformed')}")

    # The supervisor's requests through the same decoder: command numbers and argument text.
    print("devupdate selftest: supervisor requests (proto.c, host build)")
    for name, argtext in (("ping", ""), ("sv_status", ""), ("sv_terminate", "gpureset=1"), ("sv_launch", "map=gor_01 wdog=9"),
                          ("sv_watch", "45"), ("sv_reboot", "dry"), ("sv_exit", ""), ("files", "get @sv/supervisor.log")):
        a = argtext.encode()
        pkt = header(name, args=a) + a
        check(f"{name} {argtext!r}", pkt, name, 0, b"", a, 0, False)
    pkt = request_prefix("push", 0, b"map=x", b"#9 abc", len(cia), crc_ok, SV_TITLE_ID, "") + cia
    check("supervisor push with a version text (self-update title)", pkt, "push", FLAG_VERSION, b"#9 abc", b"map=x", crc_ok)

    f2, c2 = selftest_supervisor_policy(exe.parent)
    f3, c3 = selftest_loop()
    fails, count = fails + f2 + f3, count + c2 + c3
    print(f"devupdate selftest: {count - fails} of {count} passed")
    return 1 if fails else 0


def selftest_supervisor_policy(tmp):
    """supervisor/policy.c (the console's watchdog policy and text helpers) built with gcc and run
    through scripted cases. Returns (fails, count)."""
    src = HERE.parent / "supervisor"
    exe = Path(tmp) / "sv_selftest"
    cc = subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(src / "selftest_host.c"),
                         str(src / "policy.c")], capture_output=True, text=True)
    print("devupdate selftest: supervisor policy (supervisor/policy.c, host build)")
    if cc.returncode != 0:
        print(cc.stderr)
        print("  FAIL cannot build supervisor/selftest_host.c")
        return 1, 1

    def run(script):
        r = subprocess.run([str(exe)], input="\n".join(script) + "\n", capture_output=True, text=True)
        return r.stdout.splitlines()

    cases = [
        # (name, script, the expected last line)
        ("off: no action however stale", ["init 2", "tick 1 999999"], "tick=0 relaunches=0 watch=0"),
        ("armed, fresh heartbeats: none", ["init 2", "arm 10 0", "hb 1 0 1000", "hb 2 0 9000", "tick 1 15000"],
         "tick=0 relaunches=0 watch=10"),
        ("armed, frame frozen 10 s: relaunch 1", ["init 2", "arm 10 0", "hb 5 0 1000", "hb 5 0 6000", "tick 1 11000"],
         "tick=1 relaunches=1 watch=10"),
        ("no heartbeat at all after the launch (hung at frame 0): relaunch",
         ["init 2", "arm 10 0", "launched 0", "tick 1 10000"], "tick=1 relaunches=1 watch=10"),
        ("game stopped: none (the PC decides)", ["init 2", "arm 10 0", "tick 0 50000"], "tick=0 relaunches=0 watch=10"),
        ("state unknown (emulator) counts as running", ["init 2", "arm 10 0", "tick -1 10000"],
         "tick=1 relaunches=1 watch=10"),
        ("paused: never hung", ["init 2", "arm 10 0", "hb 5 1 1000", "tick 1 60000", "tick 1 65000"],
         "tick=0 relaunches=0 watch=10"),
        ("recovery: never hung", ["init 2", "arm 10 0", "hb 5 4 1000", "tick 1 60000"], "tick=0 relaunches=0 watch=10"),
        ("loading: 4x the limit (39 s: none)", ["init 2", "arm 10 0", "hb 5 2 0", "tick 1 39000"],
         "tick=0 relaunches=0 watch=10"),
        ("loading: 4x the limit (40 s: relaunch)", ["init 2", "arm 10 0", "hb 5 2 0", "tick 1 40000"],
         "tick=1 relaunches=1 watch=10"),
        ("two relaunches, then it gives up and disarms",
         ["init 2", "arm 10 0", "tick 1 10000", "launched 10000", "tick 1 20000", "launched 20000", "tick 1 30000"],
         "tick=2 relaunches=2 watch=0"),
        ("a push gives two relaunches again",
         ["init 2", "arm 10 0", "tick 1 10000", "launched 10000", "tick 1 20000", "pushed 20000", "tick 1 30000"],
         "tick=1 relaunches=1 watch=10"),
        ("max_relaunches 0: gives up at once", ["init 0", "arm 5 0", "tick 1 5000"], "tick=2 relaunches=0 watch=0"),
        ("a resumed heartbeat after a pause restarts the timer",
         ["init 2", "arm 10 0", "hb 5 1 1000", "tick 1 50000", "hb 5 0 50000", "tick 1 59000"], "tick=0 relaunches=0 watch=10"),
        ("kv int", ["kv gpureset=1 x=2|gpureset|9"], "kv=1"),
        ("kv hex and missing", ["kv a=0x10|a|0"], "kv=16"),
        ("kv missing -> default", ["kv watch=3|gpureset|2"], "kv=2"),
        ("kv bad value -> default", ["kv gpureset=x|gpureset|2"], "kv=2"),
        ("kv key prefix is not a match", ["kv xgpureset=1|gpureset|2"], "kv=2"),
        ("word", ["word reboot hard|hard"], "word=1"),
        ("word is not a prefix", ["word hardly|hard"], "word=0"),
        ("launch args with switches", ["largs map=gor_01 wdog=9|200"], "largs=45 ctrdk:map=gor_01 wdog=9 ctrdk_from=supervisor"),
        ("launch args without switches", ["largs |200"], "largs=27 ctrdk:ctrdk_from=supervisor"),
        ("launch args that do not fit", ["largs abcdef|20"], "largs=-1 "),
        ("rel ok", ["rel gpuhang/dump_1.bin"], "rel=1"),
        ("rel ..", ["rel ../ttyd/latest.log"], "rel=0"),
        ("rel absolute", ["rel /3ds/x"], "rel=0"),
        ("rel //", ["rel a//b"], "rel=0"),
        ("rel bad character", ["rel a b"], "rel=0"),
    ]
    fails = 0
    for name, script, want in cases:
        out = run(script)
        got = out[-1] if out else "(no output)"
        ok = got == want
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + ("" if ok else f": want {want!r}, got {got!r}"))
    return fails, len(cases)


class MockConsole:
    """A stand-in for the supervisor (or for devupdate on 5050) in `selftest`: the same header, the
    same reply lines. The game is modelled as: after a push or launch, each status call is one more
    heartbeat, 100 frames each; latest.log says "boot done" from the second status on."""

    def __init__(self, kind="supervisor", push_error=None):
        self.kind, self.push_error = kind, push_error
        self.sock = socket.socket()
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(4)
        self.sock.settimeout(0.2)
        self.port = self.sock.getsockname()[1]
        self.since = 0
        self.launched = False
        self.alive = True
        self.calls = []
        self.files = {"crash.txt": b"[CRASH] v1 none\n"}
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def stop(self):
        self.alive = False
        self.thread.join(2)
        self.sock.close()

    def serve(self):
        while self.alive:
            try:
                c, _ = self.sock.accept()
            except OSError:
                continue
            with c:
                c.settimeout(5)
                try:
                    self.handle(c)
                except OSError:
                    pass

    def recv_n(self, c, n):
        b = b""
        while len(b) < n:
            chunk = c.recv(n - len(b))
            if not chunk:
                raise OSError("closed")
            b += chunk
        return b

    def handle(self, c):
        h = self.recv_n(c, 64)
        _m, _v, cmd, flags, size, crc, alen, tid, _t = struct.unpack("<4sHHIIIIQ32s", h)
        area = self.recv_n(c, alen) if alen else b""
        if flags & FLAG_VERSION and area:
            area = area[1 + area[0]:]
        argtext = area.decode(errors="replace")
        self.calls.append((cmd, argtext))
        out = []
        if cmd == 1:
            out = [f"pong {self.kind} version=mock state=running", "done"]
        elif cmd == 0x101:
            if self.launched:
                self.since += 1
            out = ["status game_state=running",
                   f"status heartbeat={self.since} frame={self.since * 100} flags=0 age_ms=0 stale_ms=0 since_launch={self.since}",
                   "done"]
            if self.since >= 2:
                self.files["latest.log"] = b"[I] boot\n[I] boot done map=gor_01\n"
        elif cmd == 2:
            data = self.recv_n(c, size)
            if self.push_error:
                out = [self.push_error]
            elif zlib.crc32(data) & 0xFFFFFFFF != crc:
                out = ["error 5 crc mismatch"]
            else:
                self.launched, self.since = True, 0
                out = [f"installed {tid:016x}", "done"]
        elif cmd in (0x102, 0x104, 0x105, 0x106):
            out = ["ok", "done"]
        elif cmd == 0x103:
            self.launched, self.since = True, 0
            out = ["launch rc=00000000", "done"]
        elif cmd == 6:
            op, _, path = argtext.partition(" ")
            rel = path[2:] if path.startswith("@/") else path
            if op == "get" and rel in self.files:
                d = self.files[rel]
                c.sendall(f"file {len(d)} {zlib.crc32(d) & 0xFFFFFFFF:08x}\n".encode() + d + b"\n")
                out = ["done"]
            elif op == "ls" and rel == "gpuhang":
                out = ["error 21 no such folder gpuhang"]
            else:
                out = [f"error 21 no such file {rel}"]
        else:
            out = [f"error 4 unknown command {cmd}"]
        c.sendall("".join(l + "\n" for l in out).encode())


def selftest_loop():
    """`loop` against mock consoles on localhost: a normal entry (push, first heartbeat, until pattern,
    collect), an entry ended by its frame count, the fallback to devupdate when the supervisor refuses
    the push, and the stop with exit 2 when the supervisor stops answering. Returns (fails, count)."""
    import tempfile
    print("devupdate selftest: loop runner against mock consoles (localhost)")
    tmp = Path(tempfile.mkdtemp(prefix="cdu-loop-"))
    # A fake CIA with a TMD the title-ID walk accepts: header 0x20 bytes, no cert/ticket, TMD sig 0x10004.
    hdr = struct.pack("<IHHIIII", 0x20, 0, 0, 0, 0, 0x200, 0) + b"\0" * 8
    tmd = struct.pack(">I", 0x10004) + b"\0" * (0x100 + 0x3C) + b"\0" * 0x4C + struct.pack(">Q", 0x000400000FD88600)
    cia = tmp / "game.cia"
    cia.write_bytes(hdr.ljust(64, b"\0") + tmd + b"\0" * 1024)
    fast = {"poll_s": 0.05, "log_poll_s": 0, "start_timeout_s": 3, "timeout_s": 4, "push_timeout_s": 10,
            "fallback_settle_s": 0, "reboot_wait_s": 12}
    fails, count = 0, 0

    def run(name, manifest, sv, du, want_code, want_results, extra=None):
        nonlocal fails, count
        count += 1
        mpath = tmp / f"{name}.json"
        mpath.write_text(json.dumps(manifest))
        a = argparse.Namespace(manifest=str(mpath), run=name, out=str(tmp / "runs"), ip="127.0.0.1",
                               port=du.port if du else 1, sv_port=sv.port, token="", timeout=5, version_text="")
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = cmd_loop(a)
        rows = (tmp / "runs" / name / "results.tsv").read_text().splitlines()[1:]
        got = [r.split("\t")[2] for r in rows]
        ok = code == want_code and got == want_results and (extra is None or extra(tmp / "runs" / name))
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: exit {code}, results {got}" +
              ("" if ok else f" (want exit {want_code}, {want_results})\n{buf.getvalue()[-1500:]}"))

    sv = MockConsole()
    run("until-and-frames", {"defaults": fast, "entries": [
        {"name": "boot", "cia": str(cia), "switches": "map=gor_01", "until": r"boot done", "collect": ["latest.log"]},
        {"name": "frames", "switches": "map=gor_02", "frames": 300},
    ]}, sv, None, 0, ["until", "frames"],
        extra=lambda d: (d / "01_boot" / "latest.log").is_file() and (d / "01_boot" / "crash.txt").is_file())
    pushes = [c for c in sv.calls if c[0] == 2]
    count += 1
    ok = len(pushes) == 1 and pushes[0][1] == "map=gor_01" and (0x103, "map=gor_02") in sv.calls
    fails += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} the push carried the switches; the CIA-less entry launched with its own")
    sv.stop()

    sv = MockConsole(push_error="error 17 AM write failed: stage=am-write at=0 rc=c8a044dc")
    du = MockConsole(kind="devupdate")
    du.launched = False
    # devupdate's push does not reset the supervisor mock's heartbeat: start the mock "game" as launched.
    sv.launched = True
    run("fallback-to-5050", {"defaults": fast, "entries": [
        {"name": "fb", "cia": str(cia), "until": r"boot done"}]}, sv, du, 0, ["until"])
    count += 1
    ok = sum(1 for c in sv.calls if c[0] == 2) == 2 and sum(1 for c in du.calls if c[0] == 2) == 1
    fails += not ok
    print(f"  {'ok  ' if ok else 'FAIL'} supervisor push tried twice, then devupdate once")
    sv.stop()
    du.stop()

    sv = MockConsole()
    port = sv.port
    sv.stop()  # nothing listens: the runner must stop at entry 1 with exit 2
    dead = argparse.Namespace(port=port)
    run("supervisor-gone", {"defaults": fast, "entries": [{"name": "x", "cia": str(cia)}]}, dead, None, 2, [],
        extra=lambda d: "STOP: the supervisor stopped answering" in (d / "loop.log").read_text())
    return fails, count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ip", default=os.environ.get("N3DS_IP"), help="console address (default $N3DS_IP)")
    ap.add_argument("--port", type=int, default=None,
                    help="TCP port (default $DEVUPDATE_PORT or 5050; azahar-session start picks its slot's port)")
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
    p.add_argument("--version-text", help="the build's version text shown on the console (default: the CIA's .version sidecar)")
    p.add_argument("--stage", action="store_true",
                   help="write the CIA to the SD card first, check it there, then install from the file "
                        "(a slow or broken network cannot break AM's stream); a failed direct install retries this way by itself")
    p.add_argument("--break-after", type=int, default=0, help=argparse.SUPPRESS)  # test: break a direct push after N KiB
    p.add_argument("--no-stage-fallback", action="store_true", help="do not retry with --stage after a failed direct install")
    p.add_argument("--bad-crc", action="store_true", help="send a wrong CRC (test: the install must be refused)")
    p.add_argument("--no-report", action="store_true", help="do not save the crash report first (when the console is in recovery)")
    sub.add_parser("selftest", help="check the request framing against a host build of the device's decoder (no console)")
    p = sub.add_parser("restart", help="relaunch with switches")
    p.add_argument("switches", nargs="*")
    p.add_argument("--no-report", action="store_true", help="do not save the crash report first (when the console is in recovery)")
    p = sub.add_parser("report", help="the crash report and the log tail")
    p.add_argument("--out")
    p = sub.add_parser("launch", help="jump into an installed title")
    p.add_argument("title_id", help="hex, e.g. 000400000FD88600")
    p.add_argument("--switches", default="")
    p = sub.add_parser("files", help="put/get/ls/rm in the app's files folder on the console")
    p.add_argument("action", choices=["put", "get", "ls", "rm"])
    p.add_argument("paths", nargs="*")
    p.add_argument("--dest", default="", help="put: a folder under the files folder")
    p.add_argument("-o", "--out", help="get: the local file")
    p = sub.add_parser("log", help="the game's latest.log (read-only folder): the last 200 lines")
    p.add_argument("--prev", action="store_true", help="previous.log, the last run's log")
    p.add_argument("--crash", action="store_true", help="crash.txt, the crash handler's report (the same folder)")
    p.add_argument("-o", "--out", help="write the whole file here instead of printing the tail")
    p = sub.add_parser("queue", help="a list of test runs the console works through unattended")
    p.add_argument("action", choices=["add", "list", "clear", "start", "status", "pull"])
    p.add_argument("--no-report", action="store_true", help="start: do not save the crash report first (when the console is in recovery)")
    p.add_argument("paths", nargs="*", help="add: manifest.json and more files to upload; pull: the folder")
    p.add_argument("--shots-switches", help='add: switches for an entry\'s "shots": N, e.g. "shots={n} shotmax=40"')
    p.add_argument("--shots-collect", help="add: the app path the screenshots are collected from (default shots)")
    p = sub.add_parser("bootstrap", help="3dslink the bootstrap, push the CIA, jump into it")
    p.add_argument("--cia", required=True)
    p.add_argument("--boot", default=str(HERE.parents[2] / "build/3ds/ctr-devkit/examples/devupdate/devupdate-boot.3dsx"))
    p.add_argument("--switches", default="")
    p.add_argument("--boot-wait", type=float, default=30, help="seconds to wait for the bootstrap to answer")
    p.add_argument("--no-send", action="store_true", help="the bootstrap already runs")
    sv_port_default = int(os.environ.get("CTRDK_SV_PORT", SV_PORT))
    p = sub.add_parser("supervisor", help="the supervisor system module on --sv-port (5051)")
    p.add_argument("action", choices=["ping", "status", "terminate", "launch", "push", "files", "log", "watch",
                                      "reboot", "exit", "self-update"])
    p.add_argument("rest", nargs="*", help="launch: switches; push/self-update: the CIA; files: get|ls PATH; watch: N")
    p.add_argument("--switches", default="", help="push, launch: the switches for the launch")
    p.add_argument("--gpureset", type=int, choices=[0, 1, 2], help="terminate: the GPU reset level for this call")
    p.add_argument("--hard", action="store_true", help="reboot: APT_HardwareResetAsync only (comes back at the HOME Menu)")
    p.add_argument("--dry", action="store_true", help="reboot: only say what it would call")
    p.add_argument("--prev", action="store_true", help="log: previous.log")
    p.add_argument("--crash", action="store_true", help="log: crash.txt")
    p.add_argument("--sv", action="store_true", help="log: the supervisor's own log")
    p.add_argument("-o", "--out", help="files get, log: write the whole file here")
    p.add_argument("--version-text", help=argparse.SUPPRESS)
    p.add_argument("--sv-port", type=int, default=sv_port_default,
                   help="the supervisor's TCP port (default $CTRDK_SV_PORT or 5051)")
    p = sub.add_parser("loop", help="the unattended runner: a manifest of push/run/collect steps (supervisor first)")
    p.add_argument("manifest", help='JSON: {"defaults": {...}, "entries": [{"name", "cia", "switches", "until", "frames", '
                                    '"timeout_s", "watch", "collect"}]}')
    p.add_argument("--run", help="the run's folder name under build/loop/ (default: the time)")
    p.add_argument("--out", help="the folder for runs (default build/loop)")
    p.add_argument("--sv-port", type=int, default=sv_port_default)
    p = sub.add_parser("azahar-session", help="one long-lived headless Azahar for the same commands (--ip 127.0.0.1)")
    p.add_argument("action", choices=["start", "stop", "status", "supervise"])
    p.add_argument("--supervise", action="store_true",
                   help="start: emulate the APT relaunch (restart, queue entries) by starting the title again")
    p.add_argument("--slot-fd", type=int, help=argparse.SUPPRESS)
    p.add_argument("--profile", required=True, help="Azahar profile folder (config, NAND, SD card), under $HOME")
    p.add_argument("--boot", help="start: the .cia (installed, then its .app booted) or .3dsx")
    p.add_argument("--wait", type=float, default=120, help="start: wait this long for a devupdate ping (0 = no wait)")
    p.add_argument("--window", action="store_true", help="start: on this display instead of a headless Xvfb")
    p.add_argument("--slot-wait", type=float, default=900, help="start: wait this long for one of the 3 Azahar slots")
    args = ap.parse_args()
    args.port_given = args.port is not None
    if args.port is None:
        args.port = int(os.environ.get("DEVUPDATE_PORT", "5050"))
    if args.cmd == "azahar-session":
        return cmd_azahar_session(args)
    if args.cmd == "selftest":
        return cmd_selftest(args)
    if not args.ip:
        ap.error("give --ip or set N3DS_IP (Azahar: --ip 127.0.0.1)")
    try:
        return {"ping": cmd_ping, "push": cmd_push, "restart": cmd_restart, "report": cmd_report,
                "launch": cmd_launch, "bootstrap": cmd_bootstrap, "files": cmd_files, "queue": cmd_queue,
                "log": cmd_log, "supervisor": cmd_supervisor, "loop": cmd_loop}[args.cmd](args)
    except OSError as e:
        print(f"devupdate: no connection to {args.ip}:{args.port}: {e}")
        return 2


if __name__ == "__main__":
    sys.exit(main())
