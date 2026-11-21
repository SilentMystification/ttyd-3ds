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
     (pass), a "[CRASH]" line, --stall seconds without log growth,
     --timeout, or the emulator exiting by itself.
  5. Always stops Azahar, then copies the game log and Azahar's own log to
     build/azahar-last/ and fails on Azahar lines that would fault on
     hardware (unmapped memory access, unhandled exceptions).

Exit status: 0 pass, 1 fail, 2 usage or setup error.
"""

import argparse
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

APP_ID = "org.azahar_emu.Azahar"
REPO = Path(__file__).resolve().parents[2]
PROFILE = REPO / "build" / "azahar-profile"
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


def fail(msg, code=1):
    print(f"run_azahar: FAIL: {msg}")
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


def flatpak_cmd(args):
    # Flatpak overrides XDG_* (even with --env), so a shell inside the
    # sandbox re-exports them before starting Azahar.
    config, data, _ = profile_dirs()
    return [
        "flatpak", "run",
        f"--filesystem={PROFILE}",
        "--command=sh",
        APP_ID,
        "-c", 'export XDG_CONFIG_HOME="$1" XDG_DATA_HOME="$2"; shift 2; exec azahar "$@"',
        "sh", str(config), str(data),
    ] + args


def stop_azahar(proc):
    subprocess.run(["flatpak", "kill", APP_ID], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


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
    r = subprocess.run(flatpak_cmd(["-i", str(cia)]), capture_output=True, text=True, timeout=120)
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
    ap.add_argument("--switches", default="", help="words written to sdmc:/3ds/ttyd/debug3ds.txt")
    ap.add_argument("--log", default="3ds/ttyd/log.txt", help="game log path relative to the SD card root")
    ap.add_argument("--until", help="regex in the game log that means success")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--stall", type=float, default=0.0, help="fail after this many seconds without log growth (0 = off)")
    ap.add_argument("--gdb", action="store_true", help="enable Azahar's GDB stub; the guest waits for a debugger")
    ap.add_argument("--gdb-port", type=int, default=24689)
    ap.add_argument("--realtime", action="store_true",
                    help="frame limiter at 100%% and vsync on, for watching and listening (default: as fast as it goes)")
    ap.add_argument("--renderer", choices=["software", "opengl", "vulkan"], default="opengl")
    ap.add_argument("--lle-audio", action="store_true",
                    help="run the real DSP firmware (sdmc:/3ds/dspfirm.cdc in the profile) instead of Azahar's HLE DSP")
    ap.add_argument("--allow-hw-faults", action="store_true", help="do not fail on Azahar fault lines")
    args = ap.parse_args()

    build = args.build.resolve()
    if not build.is_file():
        fail(f"{build} not found", 2)
    if not str(build).startswith(str(Path.home())):
        fail("the build must be under the home directory (Azahar's Flatpak cannot see /tmp)", 2)

    subprocess.run(["flatpak", "kill", APP_ID], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sdmc = write_profile(args.gdb, args.gdb_port, args.lle_audio, args.realtime,
                         {"software": 0, "opengl": 1, "vulkan": 2}[args.renderer])

    ttyd_dir = sdmc / "3ds" / "ttyd"
    ttyd_dir.mkdir(parents=True, exist_ok=True)
    (ttyd_dir / "debug3ds.txt").write_text(args.switches + "\n")
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
        proc = subprocess.Popen(flatpak_cmd([str(boot)]), stdout=so, stderr=subprocess.STDOUT,
                                start_new_session=True)

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
            if "[CRASH]" in text:
                verdict, reason = False, "[CRASH] line in game log"
            elif until is not None and until.search(text):
                verdict, reason = True, f"matched {args.until!r}"
            elif proc.poll() is not None:
                verdict, reason = (until is None), f"Azahar exited (status {proc.returncode})"
            elif args.stall and now - last_growth > args.stall:
                verdict, reason = False, f"no log growth for {args.stall:.0f}s"
            elif now - start > args.timeout:
                verdict, reason = False, f"timeout after {args.timeout:.0f}s"
    finally:
        stop_azahar(proc)

    if game_log.exists():
        shutil.copy(game_log, LAST / "game_log.txt")
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
        return 0
    fail(reason)


if __name__ == "__main__":
    sys.exit(main())
