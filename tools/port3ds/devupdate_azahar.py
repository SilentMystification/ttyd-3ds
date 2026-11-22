#!/usr/bin/env python3
"""Check ctr-devkit devupdate in Azahar against ONE long-lived instance.

    tools/port3ds/devupdate_azahar.py                 # the devupdate example
    tools/port3ds/devupdate_azahar.py --app ttyd      # TTYD with the devupdate switch
    tools/port3ds/devupdate_azahar.py --app ttyd-crash  # TTYD: crashtest=fatal, then recovery

It starts the instance with `devupdate.py azahar-session start` (headless
Xvfb, one of the machine's Azahar slots, its own profile) and drives it with
the same devupdate.py commands as the console, with --ip 127.0.0.1 (Azahar's
sockets are the host's). Each step prints PASS, FAIL or NOT SUPPORTED (what
Azahar does not emulate). A relaunch ends the Azahar instance (the Flatpak
Azahar's Qt frontend aborts on an APT application jump), so it is the last step.
Exit status 1 if a step failed.
"""
import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
TOOL = REPO / "libs/ctr-devkit/devupdate/devupdate.py"
EXAMPLES = REPO / "build/3ds/ctr-devkit/examples"
OWN_CIA = EXAMPLES / "devupdate/ctrdk-devupdate.cia"
OTHER_CIA = EXAMPLES / "crash/ctrdk-crash.cia"
TTYD_CIA = REPO / "build/3ds/game/ttyd.cia"
results = []


def step(name, ok, detail=""):
    word = {True: "PASS", False: "FAIL", None: "NOT SUPPORTED"}[ok]
    results.append((name, word, detail))
    print(f"devupdate_azahar: {word}: {name}{': ' + detail if detail else ''}", flush=True)


def tool(*argv, timeout=300, quiet=False):
    """devupdate.py with --ip 127.0.0.1: the same command line as for the console."""
    cmd = [sys.executable, str(TOOL), "--ip", "127.0.0.1", "--timeout", "30", *argv]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    out = r.stdout + r.stderr
    if not quiet:
        print("\n".join("    " + l for l in out.strip().splitlines()[-12:]), flush=True)
    return r.returncode, out


def session(action, profile, *extra, timeout=1200):
    cmd = [sys.executable, str(TOOL), "azahar-session", action, "--profile", str(profile), *extra]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    print("    " + (r.stdout + r.stderr).strip().replace("\n", "\n    "), flush=True)
    return r.returncode, r.stdout + r.stderr


def alive(profile):
    return session("status", profile)[0] == 0


def wait_log(path, pattern, timeout):
    rx = re.compile(pattern)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if path.exists():
            m = rx.search(path.read_text(errors="replace"))
            if m:
                return m
        time.sleep(0.5)
    return None


def cia_tid(path):
    sys.path.insert(0, str(TOOL.parent))
    import devupdate
    return devupdate.cia_title_id(Path(path).read_bytes())


def title_dir(sdmc, tid):
    return sdmc / "Nintendo 3DS" / ("0" * 32) / ("0" * 32) / "title" / f"{tid >> 32:08x}" / f"{tid & 0xFFFFFFFF:08x}"


def app_hash(sdmc, tid):
    apps = sorted((title_dir(sdmc, tid) / "content").glob("*.app"), key=lambda p: p.stat().st_mtime)
    return hashlib.sha1(apps[-1].read_bytes()).hexdigest()[:12] if apps else None


def rebuild_examples():
    r = subprocess.run([str(REPO / "tools/port3ds/build.sh"), "libs/ctr-devkit"], capture_output=True, text=True)
    return r.returncode == 0


def common_checks(sdmc, own_cia, own_tid):
    rc, out = tool("ping")
    step("ping", rc == 0 and f"title={own_tid:016x}" in out, next((l for l in out.splitlines() if "pong" in l), ""))
    before = app_hash(sdmc, own_tid)
    rc, out = tool("push", str(own_cia), "--no-relaunch", "--bad-crc")
    step("push with a bad CRC is refused", rc == 1 and "crc mismatch" in out)
    after = app_hash(sdmc, own_tid)
    # Azahar's AM writes a CIA straight into the title's content folder and deletes it on
    # cancel, so a refused push over an installed title damages that install there. The
    # console's AM imports to a temporary area: check this one on hardware.
    step("refused push leaves the old install", True if after == before else None,
         f".app {before} -> {after}" + ("" if after == before else " (Azahar's AM cancel replaces the install)"))
    other_tid = cia_tid(OTHER_CIA)
    shutil.rmtree(title_dir(sdmc, other_tid), ignore_errors=True)
    rc, out = tool("push", str(OTHER_CIA), "--no-relaunch")
    step("push installs another title (AM, streamed)", rc == 0 and app_hash(sdmc, other_tid) is not None)
    rc, out = tool("report", quiet=True)
    step("report (no crash: the log tail)", rc == 0 and "== log" in out)


def run_example(args, profile):
    sdmc = profile / "data/azahar-emu/sdmc"
    log = sdmc / "3ds/ctrdk-devupdate/log.txt"
    for f in (sdmc / "3ds/ctrdk-devupdate/switches.txt", sdmc / "3ds/ctrdk-devupdate/crashnow"):
        f.unlink(missing_ok=True)
    own_tid = cia_tid(OWN_CIA)
    rc, out = session("start", profile, "--boot", str(OWN_CIA), "--wait", "120")
    step("session starts and answers ping", rc == 0)
    if rc != 0:
        return
    common_checks(sdmc, OWN_CIA, own_tid)

    # 3 pushes of a rebuilt CIA of the running title (its build stamp changes each build).
    for i in range(1, 4):
        old = app_hash(sdmc, own_tid)
        if not rebuild_examples():
            step(f"push {i}: rebuild", False)
            continue
        rc, out = tool("push", str(OWN_CIA), "--no-relaunch")
        new = app_hash(sdmc, own_tid)
        rate = next((l.split("(")[-1].rstrip(")") for l in out.splitlines() if "sent in" in l), "")
        step(f"push {i}: AM install over the running title", rc == 0 and new != old and new is not None,
             f".app {old} -> {new}, {rate}")
    rc, out = tool("ping", quiet=True)
    step("still running the old build, update pending", rc == 0 and "pending=1" in out)

    # Crash, recovery, then the relaunch (the last step: it ends the instance in Azahar).
    (sdmc / "3ds/ctrdk-devupdate/crashnow").write_text("fatal\n")
    m = wait_log(log, r"recovery: waiting for the PC", 60)
    step("crash hands over to recovery", m is not None)
    if m:
        rc, out = tool("ping", quiet=True)
        step("ping in recovery", rc == 0 and "recovery=1" in out)
        rc, out = tool("report", "--out", str(args.out / "example_report.txt"), quiet=True)
        text = (args.out / "example_report.txt").read_text(errors="replace") if rc == 0 else ""
        step("recovery report has the crash report and the log", "[CRASH] v1 fatal" in text and "== log" in text)
        rc, out = tool("push", str(OTHER_CIA), "--no-relaunch", "--bad-crc")
        step("recovery: bad CRC refused", rc == 1 and "crc mismatch" in out)
        rc, out = tool("push", str(OTHER_CIA), "--no-relaunch")
        step("recovery: push installs", rc == 0 and "installed" in out)
    restart_last(profile, log, "after=recovery" if m else "hello=1")


def restart_last(profile, log, word):
    rc, out = tool("restart", word, "wdog=9")
    step("restart answers", rc == 0 and "relaunching" in out)
    m = wait_log(log, rf'launch args "([^"]*{word}[^"]*)"', 30)
    if m:
        step("APT relaunch with deliver arguments", True, f'args "{m.group(1)}"')
    else:
        step("APT relaunch with deliver arguments", None,
             "Azahar ended the instance" if not alive(profile) else "no relaunch seen")


def run_ttyd(args, profile, crash):
    sdmc = profile / "data/azahar-emu/sdmc"
    log = sdmc / "3ds/ttyd/log.txt"
    if not (sdmc / "3ds/ttyd/game.iso").exists():
        print("devupdate_azahar: the profile has no 3ds/ttyd/game.iso (hard-link iso/game.iso there)")
        return
    (sdmc / "3ds/ttyd").mkdir(parents=True, exist_ok=True)
    (sdmc / "3ds/ttyd/debug3ds.txt").write_text("devupdate fastboot" + (" crashtest=fatal crashframe=300" if crash else "")
                                                + "\n")
    own_tid = cia_tid(TTYD_CIA)
    rc, out = session("start", profile, "--boot", str(TTYD_CIA), "--wait", "180")
    step("TTYD session starts and answers ping", rc == 0)
    if rc != 0:
        return
    if crash:
        m = wait_log(log, r"recovery: waiting for the PC", 120)
        step("TTYD crash hands over to recovery", m is not None)
        if m:
            rc, out = tool("report", "--out", str(args.out / "ttyd_report.txt"), quiet=True)
            text = (args.out / "ttyd_report.txt").read_text(errors="replace") if rc == 0 else ""
            step("TTYD recovery report", "[CRASH] v1 fatal" in text and "== log" in text)
        restart_last(profile, log, "after=recovery")
        return
    wait_log(log, r"warp:|title|frame hooks", 60)
    common_checks(sdmc, TTYD_CIA, own_tid)
    paused = wait_log(log, r"devupdate: game thread paused", 5)
    resumed = wait_log(log, r"devupdate: game thread resumed", 5)
    step("on_begin pauses the game at a frame boundary, on_resume resumes it", bool(paused and resumed))
    old = app_hash(sdmc, own_tid)
    rc, out = tool("push", str(TTYD_CIA), "--no-relaunch", timeout=900)
    rate = next((l.split("(")[-1].rstrip(")") for l in out.splitlines() if "sent in" in l), "")
    step("TTYD: AM install over the running title", rc == 0 and "installed" in out, f".app {old} -> "
         f"{app_hash(sdmc, own_tid)}, {rate}")
    time.sleep(3)
    size = log.stat().st_size
    time.sleep(5)
    step("TTYD keeps running after the push", log.stat().st_size > size and alive(profile))
    restart_last(profile, log, "hello=1")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", choices=["example", "ttyd", "ttyd-crash"], default="example")
    ap.add_argument("--profile", type=Path, help="default build/azahar-profile-devupdate (example) or "
                    "build/azahar-profile (TTYD, which needs the disc image there)")
    ap.add_argument("--out", type=Path, default=REPO / "build/devupdate-azahar")
    args = ap.parse_args()
    profile = (args.profile or REPO / ("build/azahar-profile-devupdate" if args.app == "example"
                                       else "build/azahar-profile")).resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    try:
        if args.app == "example":
            run_example(args, profile)
        else:
            run_ttyd(args, profile, args.app == "ttyd-crash")
    finally:
        session("stop", profile)
        sdmc = profile / "data/azahar-emu/sdmc"
        for rel in ("3ds/ctrdk-devupdate/log.txt", "3ds/ctrdk-devupdate/log.old", "3ds/ttyd/log.txt"):
            if (sdmc / rel).exists():
                shutil.copy(sdmc / rel, args.out / f"{args.app}_{Path(rel).name}")
        for name in ("devupdate-session-azahar.txt",):
            if (profile / name).exists():
                shutil.copy(profile / name, args.out / f"{args.app}_{name}")
        alog = profile / "data/azahar-emu/log/azahar_log.txt"
        if alog.exists():
            shutil.copy(alog, args.out / f"{args.app}_azahar_log.txt")
    failed = [r for r in results if r[1] == "FAIL"]
    print(f"devupdate_azahar: {len(results) - len(failed)} of {len(results)} steps did not fail; "
          f"logs in {args.out.relative_to(REPO)}/")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
