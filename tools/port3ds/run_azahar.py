#!/usr/bin/env python3
"""Run a 3DS build in Azahar and decide pass or fail from its logs.

    tools/port3ds/run_azahar.py --build build/3ds/probe/ttyd_probe.cia \\
        --switches "autoexit" --until "PROBE DONE" --log 3ds/ttyd/probe.txt

What it does:
  1. Uses an isolated Azahar profile in build/azahar-profile/ (its own
     config, NAND and SD card), so your personal Azahar settings are never
     touched. The profile is New 3DS mode, no frame limit, no vsync, and has
     the GDB stub on --gdb-port when --gdb is given.
  2. Writes --switches to sdmc:/3ds/ttyd/debug3ds.txt.
  3. A .3dsx is booted directly. A .cia is installed with `azahar -i` and
     its installed content (.app) is booted, so the ExHeader from the CIA
     (QTM mapping, 804 MHz, core 2) applies.
  4. Tails the game log (sdmc-relative --log). Stops on the --until regex
     (pass; --frames N is --until for "game frame N reached"), a "[CRASH]"
     line, --stall seconds (default 20) without log growth, --timeout, or the
     emulator exiting by itself. A run that times out with an --until that
     never matched prints the last log lines.
  5. Always stops Azahar, then copies the game log and Azahar's own log to
     build/azahar-last/ and fails on Azahar lines that would fault on
     hardware (unmapped memory access, unhandled exceptions).

Exit status: 0 pass, 1 fail, 2 usage or setup error.
"""

import argparse
import atexit
import fcntl
import os
import re
import resource
import shutil
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_usage  # noqa: E402

APP_ID = "org.azahar_emu.Azahar"
REPO = Path(__file__).resolve().parents[2]
DEFAULT_PROFILE = REPO / "build" / "azahar-profile"
PROFILE = DEFAULT_PROFILE
LAST = REPO / "build" / "azahar-last"

# Azahar log lines that mean the guest did something real hardware punishes.
HW_FAULT_PATTERNS = [
    re.compile(r"Unmapped|unmapped (read|write)", re.IGNORECASE),
    re.compile(r"Exception Type|Prefetch Abort|Data Abort|Undefined instruction", re.IGNORECASE),
    re.compile(r"svcBreak|Break\(\) called", re.IGNORECASE),
]

QT_CONFIG = """[System]
is_new_3ds=true
is_new_3ds\\default=false
region_value=1
region_value\\default=false
lle_applets=false
lle_applets\\default=false

[Renderer]
frame_limit={frame_limit}
frame_limit\\default=false
use_vsync={vsync}
use_vsync\\default=false
graphics_api={api}
graphics_api\\default=false
async_shader_compilation=false
async_shader_compilation\\default=false

[Audio]
enable_audio_stretching=false
enable_audio_stretching\\default=false
audio_emulation={audio}
audio_emulation\\default=false

[Debugging]
use_gdbstub={gdb}
use_gdbstub\\default=false
gdbstub_port={gdb_port}
gdbstub_port\\default=false

[Miscellaneous]
log_filter=*:Info
log_filter\\default=true

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


_slot_fd = None
_usage = {"start": time.time(), "slot": None, "switches": "", "result": "no result"}


def _log_usage():
    # Every run ends here (pass, fail, usage error, signal): one line in the machine-wide log.
    now = time.monotonic()
    slot = _usage["slot"]
    if slot is None:  # never got a slot: all waiting
        waited, ran = now - _usage["t0"], 0.0
    else:
        waited, ran = slot - _usage["t0"], now - slot
    emu_usage.record("azahar", REPO, waited, ran, _usage["result"], _usage["switches"], _usage["start"])


_usage["t0"] = time.monotonic()
atexit.register(_log_usage)


def acquire_slot():
    """Machine-wide limit on Azahar instances: one flock'd slot file per instance, held until this
    process ends (the OS drops the lock if it dies). AZAHAR_MAX (default 3) is the number of slots."""
    global _slot_fd
    n = int(os.environ.get("AZAHAR_MAX", "3"))
    run = os.environ.get("XDG_RUNTIME_DIR")
    d = Path(run) / "ttyd-azahar" if run and os.access(run, os.W_OK) else Path(f"/tmp/ttyd-azahar-{os.getuid()}")
    d.mkdir(parents=True, exist_ok=True)
    told = False
    while True:
        used = 0
        for i in range(n):
            fd = os.open(d / f"azahar-slot-{i}", os.O_RDWR | os.O_CREAT, 0o666)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                os.ftruncate(fd, 0)
                os.write(fd, f"{os.getpid()}\n".encode())
                _slot_fd = fd
                _usage["slot"] = time.monotonic()
                return
            except OSError:
                os.close(fd)
                used += 1
        if not told:
            print(f"run_azahar: waiting for a free Azahar slot ({used} in use)", flush=True)
            told = True
        time.sleep(5)


def emulator_preexec():
    """Runs in the child before exec: no core dump (a crashed emulator would be dumped by systemd-coredump,
    costing minutes of CPU and disk) and a lower priority, so builds and the desktop get the cores first."""
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.nice(10)


def fail(msg, code=1):
    print(f"run_azahar: FAIL: {msg}")
    _usage["result"] = f"FAIL: {msg}"
    sys.exit(code)


def profile_dirs():
    config = PROFILE / "config"
    data = PROFILE / "data"
    return config, data, data / "azahar-emu" / "sdmc"


def write_profile(gdb, gdb_port, lle_audio=False, realtime=False, api=1):
    config, data, sdmc = profile_dirs()
    (config / "azahar-emu").mkdir(parents=True, exist_ok=True)
    sdmc.mkdir(parents=True, exist_ok=True)
    cfg = QT_CONFIG.format(gdb="true" if gdb else "false", gdb_port=gdb_port, audio=2 if lle_audio else 0,
                           frame_limit=100 if realtime else 0, vsync="true" if realtime else "false", api=api)
    (config / "azahar-emu" / "qt-config.ini").write_text(cfg)
    return sdmc


_xvfb = None
DISPLAY_NUM = None  # set when running headless


def start_xvfb():
    """A private virtual display for this run, so no Azahar window appears. Stopped at exit."""
    global _xvfb, DISPLAY_NUM
    r, w = os.pipe()
    _xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(w), "-screen", "0", "1280x720x24", "-nolisten", "tcp", "-ac"],
                             pass_fds=(w,), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                             start_new_session=True)
    os.close(w)
    num = b""
    while not num.endswith(b"\n"):
        chunk = os.read(r, 16)
        if not chunk:
            fail("Xvfb did not start (use --show to use the real display)", 2)
        num += chunk
    os.close(r)
    DISPLAY_NUM = int(num)
    atexit.register(stop_xvfb)


def stop_xvfb():
    if _xvfb is not None and _xvfb.poll() is None:
        _xvfb.terminate()
        try:
            _xvfb.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _xvfb.kill()


def run_env():
    env = dict(os.environ)
    if DISPLAY_NUM is not None:
        env["DISPLAY"] = f":{DISPLAY_NUM}"
        env.pop("WAYLAND_DISPLAY", None)
    return env


def flatpak_cmd(args):
    # Flatpak overrides XDG_* (even with --env), so a shell inside the
    # sandbox re-exports them before starting Azahar.
    config, data, _ = profile_dirs()
    return [
        "flatpak", "run",
        f"--filesystem={PROFILE}",
    ] + ([f"--env=DISPLAY=:{DISPLAY_NUM}", "--env=WAYLAND_DISPLAY=", "--env=QT_QPA_PLATFORM=xcb", "--nosocket=wayland",
          "--env=LIBGL_ALWAYS_SOFTWARE=1"] if DISPLAY_NUM is not None else []) + [
        "--command=sh",
        APP_ID,
        "-c", 'export XDG_CONFIG_HOME="$1" XDG_DATA_HOME="$2"; shift 2; exec azahar "$@"',
        "sh", str(config), str(data),
    ] + args


def session_pids(sid):
    """Processes in the session this run started (Azahar is launched with start_new_session)."""
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


def stop_azahar(proc):
    """Stops only the Azahar this run started (its session), never other instances: no `flatpak kill`."""
    for sig in (signal.SIGTERM, signal.SIGKILL):
        pids = session_pids(proc.pid)
        for pid in pids:
            try:
                os.kill(pid, sig)
            except OSError:
                pass
        try:
            proc.wait(timeout=5)
            if not session_pids(proc.pid):
                return
        except subprocess.TimeoutExpired:
            pass


def cia_title_id(cia):
    """Reads the title ID from the CIA's ticket (follows the CIA header layout)."""
    data = cia.read_bytes()
    header_size, _type, _ver, cert_size, ticket_size = struct.unpack_from("<IHHII", data, 0)

    def align(x):
        return (x + 63) & ~63

    ticket = align(header_size) + align(cert_size)
    sig_type = struct.unpack_from(">I", data, ticket)[0]
    sig_len = {0x10003: 0x200 + 0x3C, 0x10004: 0x100 + 0x3C, 0x10005: 0x3C + 0x40}.get(sig_type)
    if sig_len is None:
        raise SystemExit(f"run_azahar: unknown ticket signature type 0x{sig_type:x}")
    body = ticket + 4 + sig_len
    return struct.unpack_from(">Q", data, body + 0x9C)[0]


def install_cia(cia, sdmc):
    print(f"run_azahar: installing {cia.name}")
    r = subprocess.run(flatpak_cmd(["-i", str(cia)]), env=run_env(), capture_output=True, text=True, timeout=120,
                       preexec_fn=emulator_preexec)
    if r.returncode != 0 or "Installed CIA successfully" not in r.stdout + r.stderr:
        print(r.stdout[-2000:], r.stderr[-2000:])
        fail("CIA install failed", 2)
    tid = cia_title_id(cia)
    high, low = tid >> 32, tid & 0xFFFFFFFF
    content = (sdmc / "Nintendo 3DS" / ("0" * 32) / ("0" * 32) / "title" / f"{high:08x}" / f"{low:08x}" / "content")
    apps = sorted(content.glob("*.app"), key=lambda p: p.stat().st_mtime)
    if not apps:
        fail(f"no installed .app under {content}", 2)
    return apps[-1]


def azahar_log_path():
    _, data, _ = profile_dirs()
    return data / "azahar-emu" / "log" / "azahar_log.txt"


def main():
    ap = argparse.ArgumentParser(description="Run a 3DS build in Azahar with pass/fail detection.")
    ap.add_argument("--build", required=True, type=Path, help=".3dsx or .cia to run")
    ap.add_argument("--switches", default="",
                    help="words written to sdmc:/3ds/ttyd/debug3ds.txt (fastboot is added unless nofastboot is given)")
    ap.add_argument("--log", default="3ds/ttyd/latest.log", help="game log path relative to the SD card root")
    ap.add_argument("--until", help="regex in the game log that means success")
    ap.add_argument("--frames", type=int, metavar="N", default=None,
                    help="pass when the game reaches game frame N (the game logs 'frame N reached' via the framemark "
                    "switch); use this instead of a hand-written --until for runs to a frame")
    ap.add_argument("--timeout", type=float, default=None,
                    help="seconds the emulator may run once it has a slot (default 60; more needs --long)")
    ap.add_argument("--long", type=float, metavar="SECONDS", default=None,
                    help="a run that genuinely needs more than 60 s: the limit in seconds (max 1800)")
    ap.add_argument("--stall", type=float, default=20.0,
                    help="fail (FAIL: stall) after this many seconds without game log growth (default 20; 0 = off)")
    ap.add_argument("--gdb", action="store_true", help="enable Azahar's GDB stub; the guest waits for a debugger")
    ap.add_argument("--gdb-port", type=int, default=24689)
    ap.add_argument("--realtime", action="store_true",
                    help="frame limiter at 100%% and vsync on, for watching and listening (default: as fast as it goes)")
    ap.add_argument("--renderer", choices=["software", "opengl", "vulkan"], default="opengl")
    ap.add_argument("--lle-audio", action="store_true",
                    help="run the real DSP firmware (sdmc:/3ds/dspfirm.cdc in the profile) instead of Azahar's HLE DSP")
    ap.add_argument("--expect-crash", action="store_true",
                    help="a [CRASH] line does not end the run (crash tests: wait for --until)")
    ap.add_argument("--profile", type=Path, help="Azahar profile folder (config, NAND, SD card); default build/azahar-profile. "
                    "A different folder lets a second instance run beside another; only that profile's Azahar is stopped")
    ap.add_argument("--last", type=Path, help="folder for this run's logs; default build/azahar-last")
    ap.add_argument("--show", action="store_true", help="show the Azahar window on the real display (default: a private Xvfb)")
    ap.add_argument("--allow-hw-faults", action="store_true", help="do not fail on Azahar fault lines")
    args = ap.parse_args()
    global PROFILE, LAST
    if args.profile:
        PROFILE = args.profile.resolve()
    if args.last:
        LAST = args.last.resolve()

    args.timeout, err = emu_usage.resolve_timeout("run_azahar", args.timeout, args.long)
    if err:
        fail(err, 2)
    if args.frames is not None:
        if args.until:
            fail("--frames and --until are two ways to say when a run passes: use one", 2)
        if args.frames < 1:
            fail("--frames takes a game frame number of 1 or more", 2)
        args.until = rf"\bframe {args.frames} reached\b"
    _usage["switches"] = args.switches
    build = args.build.resolve()
    # A background job starts with SIGINT ignored, and SIGTERM would skip the finally blocks:
    # turn both into an exception so the Azahar this run started always goes down with it.
    def _on_signal(signum, _frame):
        _usage["result"] = f"killed: signal {signum}"
        raise KeyboardInterrupt(f"signal {signum}")
    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)
    acquire_slot()
    if not args.show:
        start_xvfb()
    if not build.is_file():
        fail(f"{build} not found", 2)
    if not str(build).startswith(str(Path.home())):
        fail("the build must be under the home directory (Azahar's Flatpak cannot see /tmp)", 2)

    sdmc = write_profile(args.gdb, args.gdb_port, args.lle_audio, args.realtime,
                         {"software": 0, "opengl": 1, "vulkan": 2}[args.renderer])

    ttyd_dir = sdmc / "3ds" / "ttyd"
    ttyd_dir.mkdir(parents=True, exist_ok=True)
    sw = args.switches.split()
    if args.frames is not None:
        sw = [w for w in sw if not w.startswith("framemark=")] + [f"framemark={args.frames}"]
    if "nofastboot" in sw:
        sw.remove("nofastboot")
    elif "fastboot" not in sw:
        sw.append("fastboot")  # presses A through the boot screens
    (ttyd_dir / "debug3ds.txt").write_text(" ".join(sw) + "\n")
    game_log = sdmc / args.log
    if game_log.exists():
        game_log.unlink()
    alog = azahar_log_path()
    if alog.exists():
        alog.unlink()

    if build.suffix == ".cia":
        boot = install_cia(build, sdmc)
    elif build.suffix == ".3dsx":
        boot = build
    else:
        fail("--build must be a .3dsx or .cia", 2)

    LAST.mkdir(parents=True, exist_ok=True)
    stdout_path = LAST / "azahar_stdout.txt"
    print(f"run_azahar: booting {boot}")
    with open(stdout_path, "w") as so:
        proc = subprocess.Popen(flatpak_cmd([str(boot)]), env=run_env(), stdout=so, stderr=subprocess.STDOUT,
                                start_new_session=True, preexec_fn=emulator_preexec)
        atexit.register(lambda p=proc: stop_azahar(p))

    until = re.compile(args.until) if args.until else None
    # Periodic lines the game writes even when it is stuck: they do not count as log growth for --stall.
    idle_lines = re.compile(r"^.*(?:audio: peak|memory now:).*\n?", re.M)
    verdict, reason = None, None
    start = time.monotonic()
    last_size, last_growth = -1, start
    try:
        while verdict is None:
            time.sleep(0.25)
            now = time.monotonic()
            text = ""
            if game_log.exists():
                text = game_log.read_text(errors="replace")
                size = len(idle_lines.sub("", text))
                if size != last_size:
                    last_size, last_growth = size, now
            if "[CRASH]" in text and not args.expect_crash:
                verdict, reason = False, "[CRASH] line in game log"
            elif until is not None and until.search(text):
                verdict, reason = True, f"matched {args.until!r}"
            elif proc.poll() is not None:
                verdict, reason = (until is None), f"Azahar exited (status {proc.returncode})"
            elif args.stall and now - last_growth > args.stall:
                verdict, reason = False, f"stall: no log growth for {args.stall:.0f}s"
            elif now - start > args.timeout:
                verdict, reason = False, f"timeout after {args.timeout:.0f}s"
    finally:
        stop_azahar(proc)

    if game_log.exists():
        shutil.copy(game_log, LAST / "game_log.txt")
        if verdict is False and until is not None and reason.startswith(("timeout", "stall")):
            lines = game_log.read_text(errors="replace").splitlines()
            if not any(until.search(line) for line in lines):
                # A run that ran to the limit with a pattern that never matched: show why (wrong pattern, or a
                # line that only appears with a switch).
                print("run_azahar: --until never matched; last log lines:")
                for line in lines[-10:]:
                    print(f"  {line}")
    faults = []
    if alog.exists():
        shutil.copy(alog, LAST / "azahar_log.txt")
        for line in alog.read_text(errors="replace").splitlines():
            if any(p.search(line) for p in HW_FAULT_PATTERNS):
                faults.append(line)
    if faults and not args.allow_hw_faults:
        verdict = False
        reason = f"{len(faults)} hardware-fault line(s) in Azahar log, first: {faults[0].strip()}"

    print(f"run_azahar: logs in {LAST.relative_to(REPO)}/")
    if verdict:
        print(f"run_azahar: PASS: {reason}")
        _usage["result"] = f"PASS: {reason}"
        return 0
    fail(reason)


if __name__ == "__main__":
    # Every exit gets a result for the usage log: a usage error, an exception or a SystemExit with a message
    # that did not go through fail() would otherwise be logged as "no result".
    try:
        sys.exit(main())
    except SystemExit as e:
        if _usage["result"] == "no result":
            if e.code == 2:
                _usage["result"] = "usage error"
            elif isinstance(e.code, str):
                _usage["result"] = f"aborted: {e.code}"
            else:
                _usage["result"] = f"aborted: exit {e.code}"
        raise
    except BaseException as e:  # KeyboardInterrupt from a signal already set "killed: signal N"
        if _usage["result"] == "no result":
            _usage["result"] = f"aborted: {type(e).__name__}: {e}"
        raise
