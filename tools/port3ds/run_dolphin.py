#!/usr/bin/env python3
"""Run the GameCube game in Dolphin headless and read game memory by symbol.

    tools/port3ds/run_dolphin.py --frames 600 --read gp:4 --deref gp:0x100 --json out.json

Boots iso/game.iso (or --dol with the ISO as the disc) with no window, no
video output and the GDB stub enabled, in an isolated Dolphin user folder
(build/dolphin-profile/), so your own Dolphin settings are untouched.

Frames are counted with a breakpoint on marioStMain, which the game calls
once per frame, so "--frames 600" means after 600 game updates. Symbols come
from the decomp's ELF (build/<version>/MarioSt.elf). The decomp builds a
byte-identical DOL, so its symbols are valid for the retail disc too.

  --read NAME[:SIZE]     read SIZE bytes (default: the symbol's size) at NAME
  --deref NAME:SIZE      read the 4-byte pointer at NAME, then SIZE bytes at
                         the address it holds (for example gp -> GlobalWork)
  --addr 0xADDR:SIZE     read raw memory
  --break FUNC           after --frames, run to the next call of FUNC

The GameCube debug DOL (configure.py --gcdebug; docs/PORT_3DS_TEST_INFRA.md):

  --gcdebug              boot build/gcdebug/<version>/ (its DOL and RELs, copied
                         into an extracted disc in build/gcdebug-disc/, made
                         once with dolphin-tool) with its own symbols
  --switches TEXT        the test switches, written to gcdbg_switches through
                         the GDB stub before the game runs
  --read-log             read the test log buffer (gcdbg_log) at the end into
                         the JSON's "log" and build/dolphin-last/gcdbg_log.txt
  --disc NAME            the disc folder build/NAME/ (a hard-linked copy of
                         build/gcdebug-disc/ with its own DOL and RELs)
  --shots DIR            screenshots: stop at every tst_shot_point (the test
                         switches shotat=, fxshots=, btlshots=), read the
                         frame's XFB from memory and save DIR/fNNNNN.png
                         (608x480, the EFB size), until --break (or the
                         timeout). Renders with Vulkan on the headless
                         platform (no window, no display needed), single core,
                         XFB copies written to memory

Screenshots: Dolphin's Null backend draws nothing, so --shots uses the
Vulkan backend with -p headless (no surface; it renders offscreen). XFB
copies go to emulated memory (GFX.Hacks.XFBToTextureEnable=False), and at
tst_shot_point(F), in the frame hook of game frame F after DEMODoneRender's
draw sync, DemoCurrentBuffer holds the picture finished at F: YUYV 4:2:2,
fbWidth x xfbHeight = 608x480 in the US render modes, the size of the EFB
(the copy is 1:1, no scaling; PAL's 50 Hz mode stretches 480 EFB lines to
568, --xfb-size 608x568). The copy's vertical deflicker filter is off
(GFX.Hacks.DisableCopyFilter, Dolphin's default), as on the port, whose
shots are the EFB itself. It is converted to RGB with the inverse of the
GameCube's RGB to YCbCr copy (BT.601, studio range), so a colour differs
from the EFB's by the 4:2:2 chroma averaging and rounding only.

Results are printed as hex and, with --json, written as
{"frame": N, "reads": {label: {"addr": A, "hex": "..."}}}.
Exit status: 0 ok, 1 failure, 2 usage or setup error.
"""

import argparse
import fcntl
import json
import os
import resource
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from elfsyms import load_symbols  # noqa: E402
from gdbrsp import RSP, RSPError  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
PROFILE = REPO / "build" / "dolphin-profile"
LAST = REPO / "build" / "dolphin-last"


def die(msg, code=1):
    print(f"run_dolphin: FAIL: {msg}")
    sys.exit(code)


def emulator_preexec():
    """Runs in the child before exec: no core dump (a crashed emulator would be dumped by systemd-coredump,
    costing minutes of CPU and disk) and a lower priority, so builds and the desktop get the cores first."""
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.nice(10)


_slot_fd = None


def acquire_slot():
    """Machine-wide limit on Dolphin instances: one flock'd slot file per instance, held until this process ends
    (the OS drops the lock if it dies). DOLPHIN_MAX (default 2) is the number of slots. Same scheme as run_azahar."""
    global _slot_fd
    n = int(os.environ.get("DOLPHIN_MAX", "2"))
    run = os.environ.get("XDG_RUNTIME_DIR")
    d = Path(run) / "ttyd-dolphin" if run and os.access(run, os.W_OK) else Path(f"/tmp/ttyd-dolphin-{os.getuid()}")
    d.mkdir(parents=True, exist_ok=True)
    told = False
    while True:
        used = 0
        for i in range(n):
            fd = os.open(d / f"dolphin-slot-{i}", os.O_RDWR | os.O_CREAT, 0o666)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                os.ftruncate(fd, 0)
                os.write(fd, f"{os.getpid()}\n".encode())
                _slot_fd = fd
                return
            except OSError:
                os.close(fd)
                used += 1
        if not told:
            print(f"run_dolphin: waiting for a free Dolphin slot ({used} in use)", flush=True)
            told = True
        time.sleep(5)


def parse_size(text):
    return int(text, 0)


def prepare_gcdebug_disc(version, iso, name):
    """The debug DOL needs its own RELs (the disc's are relocated against the retail DOL's addresses), so it boots
    from an extracted disc (build/gcdebug-disc/, made from the image once) with sys/main.dol and files/rel/ replaced
    by the debug build's. With --disc NAME the run gets its own folder build/NAME/, hard links to the extracted
    files plus its own DOL and RELs, so runs of different builds can go side by side. Returns the main.dol path."""
    base = REPO / "build" / "gcdebug-disc"
    disc = REPO / "build" / name
    out = REPO / "build" / "gcdebug" / version
    dol = out / "MarioSt.dol"
    if not dol.is_file():
        die(f"{dol} not found; build it (python3 configure.py --version {version} --gcdebug && "
            "ninja -f build_gcdebug.ninja)", 2)
    if not (base / "sys" / "boot.bin").is_file():
        base.mkdir(parents=True, exist_ok=True)
        print(f"run_dolphin: extracting {iso} to {base.relative_to(REPO)}/ (once)")
        r = subprocess.run(["dolphin-tool", "extract", "-i", str(iso), "-o", str(base), "-q"])
        if r.returncode != 0:
            die("dolphin-tool extract failed", 2)
    if disc != base and not (disc / "sys" / "boot.bin").is_file():
        for src in base.rglob("*"):
            dst = disc / src.relative_to(base)
            if src.is_dir():
                dst.mkdir(parents=True, exist_ok=True)
            elif not dst.exists():
                os.link(src, dst)

    def replace(src, dst):
        # a new file, never a write through a hard link into the shared extracted disc
        if dst.exists():
            dst.unlink()
        shutil.copyfile(src, dst)

    replace(dol, disc / "sys" / "main.dol")
    rels = 0
    for rel in (disc / "files" / "rel").glob("*.rel"):
        built = out / rel.stem / f"{rel.stem}.rel"
        if not built.is_file():
            die(f"{built} not found (the debug build makes every REL)", 2)
        replace(built, rel)
        rels += 1
    print(f"run_dolphin: debug DOL and {rels} RELs in {disc.relative_to(REPO)}/")
    return disc / "sys" / "main.dol"


def xfb_to_png(data, w, h, path):
    """A GameCube XFB (YUYV 4:2:2: Y0 Cb Y1 Cr per pair of pixels) to an RGB PNG."""
    import numpy as np
    from PIL import Image
    a = np.frombuffer(data, np.uint8).reshape(h, w // 2, 4).astype(np.float32)
    y = np.stack([a[..., 0], a[..., 2]], axis=-1).reshape(h, w) - 16.0
    u = np.repeat(a[..., 1], 2, axis=1) - 128.0
    v = np.repeat(a[..., 3], 2, axis=1) - 128.0
    rgb = np.stack([1.164 * y + 1.596 * v, 1.164 * y - 0.392 * u - 0.813 * v, 1.164 * y + 2.017 * u], axis=-1)
    Image.fromarray(np.clip(rgb + 0.5, 0, 255).astype(np.uint8), "RGB").save(path)


def bulk_read(rsp, addr, size, chunk=0x400, depth=16):
    """A large memory read: 'm' requests pipelined `depth` deep and the replies parsed a buffer at a time
    (RSP.read parses byte by byte, about 10 s for an XFB)."""
    reqs = [(a, min(chunk, addr + size - a)) for a in range(addr, addr + size, chunk)]
    out = bytearray()
    buf = bytearray(rsp.buf)
    rsp.buf = b""
    sent = 0
    for i in range(len(reqs)):
        while sent < len(reqs) and sent < i + depth:
            a, n = reqs[sent]
            rsp.send(f"m{a:x},{n:x}")
            sent += 1
        while True:
            start = buf.find(b"$")
            end = buf.find(b"#", start + 1) if start >= 0 else -1
            if end >= 0 and len(buf) >= end + 3:
                break
            data = rsp.sock.recv(65536)
            if not data:
                raise RSPError("GDB stub closed the connection")
            buf += data
        body = bytes(buf[start + 1:end])
        del buf[:end + 3]
        rsp.sock.sendall(b"+")
        if len(body) != reqs[i][1] * 2:
            raise RSPError(f"read at 0x{reqs[i][0]:x}: reply {body[:8]!r}")
        out += bytes.fromhex(body.decode("latin-1"))
    rsp.buf = bytes(buf)
    return bytes(out)


def stop_regs(rsp):
    """(pc, r3) at a stop, read with 'p' (GDB's PowerPC numbering: r3 is 3, the PC 64)."""
    pc, r3 = rsp.command("p40"), rsp.command("p3")
    if len(pc) < 8 or len(r3) < 8:
        die(f"unexpected register replies {pc!r} {r3!r}")
    return int(pc[:8], 16), int(r3[:8], 16)


def shot_loop(rsp, syms, args, start):
    """Stops at every tst_shot_point(frame) and saves the frame's XFB, until --break (or the timeout)."""
    w, h = (int(v) for v in args.xfb_size.lower().split("x"))
    args.shots.mkdir(parents=True, exist_ok=True)
    for name in ("tst_shot_point", "DemoCurrentBuffer"):
        if name not in syms:
            die(f"{name} is not in the ELF (an older debug build?)", 2)
    point = syms["tst_shot_point"].addr
    end = None
    if args.brk:
        if args.brk not in syms:
            die(f"unknown symbol {args.brk}", 2)
        end = syms[args.brk].addr
        rsp.set_breakpoint(end)
    rsp.set_breakpoint(point)
    saved = []
    while True:
        if time.monotonic() - start > args.timeout:
            die(f"timeout with {len(saved)} shots")
        rsp.cont()
        pc, r3 = stop_regs(rsp)
        if pc == point:
            buf = struct.unpack(">I", rsp.read(syms["DemoCurrentBuffer"].addr, 4))[0]
            path = args.shots / f"f{r3:05d}.png"
            xfb_to_png(bulk_read(rsp, buf, w * h * 2), w, h, path)
            saved.append(r3)
            print(f"run_dolphin: shot f={r3} -> {path}", flush=True)
        elif pc == end:
            break
        else:
            die(f"stopped at 0x{pc:08X}, neither tst_shot_point nor {args.brk}")
    rsp.clear_breakpoint(point)
    if end is not None:
        rsp.clear_breakpoint(end)
    return saved


def main():
    ap = argparse.ArgumentParser(description="Run TTYD in headless Dolphin and read memory by symbol.")
    ap.add_argument("--version", default="G8ME01")
    ap.add_argument("--iso", type=Path, default=REPO / "iso" / "game.iso")
    ap.add_argument("--dol", type=Path, help="boot this DOL with --iso as the disc")
    ap.add_argument("--elf", type=Path, help="symbol source (default build/<version>/MarioSt.elf)")
    ap.add_argument("--frames", type=int, default=0, help="game frames (marioStMain calls) to run first")
    ap.add_argument("--break", dest="brk", help="then run to the next call of this function")
    ap.add_argument("--read", action="append", default=[])
    ap.add_argument("--deref", action="append", default=[])
    ap.add_argument("--addr", action="append", default=[])
    ap.add_argument("--json", type=Path)
    ap.add_argument("--port", type=int, default=55555)
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--gcdebug", action="store_true", help="boot the debug DOL (configure.py --gcdebug)")
    ap.add_argument("--switches", help="test switches for the debug DOL")
    ap.add_argument("--read-log", action="store_true", help="read the debug DOL's test log at the end")
    ap.add_argument("--profile", type=Path, help="Dolphin user folder (default build/dolphin-profile)")
    ap.add_argument("--disc", default="gcdebug-disc",
                    help="with --gcdebug: the disc folder under build/ (another name: a hard-linked copy, so two runs "
                    "of different debug builds can go at once)")
    ap.add_argument("--shots", type=Path, help="with --gcdebug: save the XFB at every tst_shot_point as DIR/fNNNNN.png")
    ap.add_argument("--xfb-size", default="608x480", help="with --shots: fbWidth x xfbHeight (default 608x480)")
    ap.add_argument("--video", help="video backend (default Null; Vulkan with --shots)")
    args = ap.parse_args()
    global PROFILE
    if args.profile:
        PROFILE = args.profile.resolve()
    if (args.switches is not None or args.read_log or args.shots) and not args.gcdebug:
        die("--switches, --read-log and --shots need --gcdebug", 2)

    elf = args.elf or REPO / "build" / args.version / "MarioSt.elf"
    if args.gcdebug:
        elf = args.elf or REPO / "build" / "gcdebug" / args.version / "MarioSt.elf"
        args.dol = prepare_gcdebug_disc(args.version, args.iso, args.disc)
    if not elf.is_file():
        die(f"{elf} not found; build the decomp first (python configure.py --version {args.version} && ninja)", 2)
    if not args.iso.is_file():
        die(f"{args.iso} not found", 2)
    syms = load_symbols(elf)

    PROFILE.mkdir(parents=True, exist_ok=True)
    LAST.mkdir(parents=True, exist_ok=True)
    video = args.video or ("Vulkan" if args.shots else "Null")
    cmd = ["dolphin-emu-nogui", "-u", str(PROFILE), "-p", "headless", "-v", video,
           "-C", f"Dolphin.General.GDBPort={args.port}",
           "-C", "Dolphin.Core.CPUCore=1",
           # The JIT only honours breakpoints with debugging enabled.
           "-C", "Dolphin.Interface.DebugModeEnabled=True",
           "-C", "Dolphin.Core.EmulationSpeed=0",
           "-C", "Dolphin.Interface.ConfirmStop=False",
           "-C", "Dolphin.Analytics.PermissionAsked=True"]
    shots_ini = PROFILE / "GameSettings" / f"{args.version}.ini"
    if args.shots:
        # XFB copies to emulated memory, no copy filter, native resolution, and single core so the GPU has
        # finished the frame's copy by the time the CPU passes its draw sync. The game's own INI layer sits above
        # the command line, so the hacks also go into this profile's user game INI (the top game layer).
        shots_ini.parent.mkdir(parents=True, exist_ok=True)
        shots_ini.write_text("[Video_Hacks]\nXFBToTextureEnable = False\nImmediateXFBEnable = False\n"
                             "DeferEFBCopies = False\nDisableCopyFilter = True\n"
                             "[Core]\nCPUThread = False\n")
        cmd += ["-C", "GFX.Hacks.XFBToTextureEnable=False",
                "-C", "GFX.Hacks.ImmediateXFBEnable=False",
                "-C", "GFX.Hacks.DeferEFBCopies=False",
                "-C", "GFX.Hacks.DisableCopyFilter=True",
                "-C", "GFX.Settings.InternalResolution=1",
                "-C", "Dolphin.Core.CPUThread=False"]
    elif shots_ini.exists():
        shots_ini.unlink()
    if args.gcdebug:
        cmd += ["-e", str(args.dol.resolve())]  # sys/main.dol of an extracted disc: Dolphin boots the folder
    elif args.dol:
        cmd += ["-C", f"Dolphin.Core.DefaultISO={args.iso.resolve()}", "-e", str(args.dol.resolve())]
    else:
        cmd += ["-e", str(args.iso.resolve())]

    acquire_slot()
    log = open(LAST / "dolphin_stdout.txt", "w")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True,
                            preexec_fn=emulator_preexec)
    rsp = None
    result = {"frame": 0, "reads": {}}
    start = time.monotonic()
    try:
        rsp = RSP("127.0.0.1", args.port, connect_timeout=30.0, timeout=args.timeout)
        rsp.stop_reason()
        if args.switches is not None:
            text = args.switches.encode() + b"\0"
            if len(text) > syms["gcdbg_switches"].size:
                die("--switches is longer than gcdbg_switches", 2)
            rsp.write(syms["gcdbg_switches"].addr, text)

        if args.frames > 0:
            bp = syms["marioStMain"].addr
            rsp.set_breakpoint(bp)
            for frame in range(args.frames):
                if time.monotonic() - start > args.timeout:
                    die(f"timeout after {frame} frames")
                rsp.cont()
            rsp.clear_breakpoint(bp)
            result["frame"] = args.frames
        if args.shots:
            result["shots"] = shot_loop(rsp, syms, args, start)
            if args.brk:
                result["break"] = args.brk
        elif args.brk:
            if args.brk not in syms:
                die(f"unknown symbol {args.brk}", 2)
            rsp.set_breakpoint(syms[args.brk].addr)
            rsp.cont()
            rsp.clear_breakpoint(syms[args.brk].addr)
            result["break"] = args.brk

        def record(label, addr, size):
            data = rsp.read(addr, size)
            result["reads"][label] = {"addr": addr, "hex": data.hex()}
            print(f"{label} @ 0x{addr:08X} ({size} bytes): {data[:64].hex()}{'...' if size > 64 else ''}")

        for spec in args.read:
            name, _, size = spec.partition(":")
            if name not in syms:
                die(f"unknown symbol {name}", 2)
            record(name, syms[name].addr, parse_size(size) if size else syms[name].size)
        for spec in args.deref:
            name, _, size = spec.partition(":")
            if name not in syms or not size:
                die(f"--deref needs a known symbol and a size: {spec}", 2)
            ptr = struct.unpack(">I", rsp.read(syms[name].addr, 4))[0]
            record(f"*{name}", ptr, parse_size(size))
        for spec in args.addr:
            addr, _, size = spec.partition(":")
            record(addr, int(addr, 0), parse_size(size or "4"))
        if args.read_log:
            n = struct.unpack(">I", rsp.read(syms["gcdbg_log_len"].addr, 4))[0]
            lost = struct.unpack(">I", rsp.read(syms["gcdbg_log_lost"].addr, 4))[0]
            text = rsp.read(syms["gcdbg_log"].addr, n).decode("latin-1") if n else ""
            result["log"] = text
            result["log_lost"] = lost
            (LAST / "gcdbg_log.txt").write_text(text)
            print(f"gcdbg_log: {n} bytes, {text.count(chr(10))} lines, {lost} lines lost (buffer full)")
    except RSPError as e:
        die(str(e))
    finally:
        if rsp is not None:
            rsp.close()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        log.close()

    if args.json:
        args.json.write_text(json.dumps(result, indent=1))
    print(f"run_dolphin: OK ({result['frame']} frames, {time.monotonic() - start:.1f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
