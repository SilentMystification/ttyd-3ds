#!/usr/bin/env python3
"""Find where the game hangs: run it in Azahar under the GDB stub, stop it a
few seconds after a log line appears, and print every thread's PC and LR with
their functions (addr2line on the ELF).

    tools/port3ds/hang_probe.py --switches "map=las_03 story=30" --after "OSLink las" --wait 6

The game's watchdog ends the process after 10 s without a frame, so --wait
must stay below that.
"""
import argparse
import re
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdbrsp import RSP  # noqa: E402
import emu_usage  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
LOG = ROOT / "build/azahar-profile/data/azahar-emu/sdmc/3ds/ttyd/log.txt"


def symbolize(elf, addrs):
    out = subprocess.run(["addr2line", "-f", "-i", "-e", str(elf)] + [f"0x{a:x}" for a in addrs],
                         capture_output=True, text=True).stdout.splitlines()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", type=Path, default=ROOT / "build/3ds/game/ttyd.cia")
    ap.add_argument("--elf", type=Path, default=ROOT / "build/3ds/game/ttyd.elf")
    ap.add_argument("--switches", required=True)
    ap.add_argument("--after", required=True, help="regex in the game log; the stop comes --wait seconds later")
    ap.add_argument("--wait", type=float, default=6.0)
    ap.add_argument("--timeout", type=float, default=120.0, help="seconds; over 60 is a --long run")
    ap.add_argument("--port", type=int, default=24689)
    ap.add_argument("--peek", action="append", default=[], help="ADDR:SIZE (hex address) read at each sample")
    ap.add_argument("--samples", type=int, default=1, help="stops, 0.5 s apart; threads in a system call are left out after the first")
    ap.add_argument("--interval", type=float, default=0.5, help="seconds the game runs between two samples")
    ap.add_argument("--stack-all", action="store_true", help="scan every thread's stack, not only the first thread's")
    ap.add_argument("--stack-words", type=int, default=24, help="code addresses kept from the stack scan")
    args = ap.parse_args()

    if LOG.exists():
        LOG.unlink()
    runner = subprocess.Popen([sys.executable, str(ROOT / "tools/port3ds/run_azahar.py"), "--build", str(args.build),
                               "--switches", args.switches, "--gdb", "--gdb-port", str(args.port),
                               *emu_usage.time_args(args.timeout), "--allow-hw-faults"], cwd=ROOT)
    try:
        rsp = RSP("127.0.0.1", args.port, connect_timeout=120)
        rsp.stop_reason()
        rsp.send("c")
        deadline = time.monotonic() + args.timeout
        pattern = re.compile(args.after)
        while time.monotonic() < deadline:
            text = LOG.read_text(errors="replace") if LOG.exists() else ""
            if pattern.search(text):
                break
            time.sleep(0.5)
        else:
            print("hang_probe: the log line never came")
            return 1
        time.sleep(args.wait)
        for sample in range(args.samples):
            rsp.interrupt()
            threads = rsp.thread_ids()
            print(f"-- sample {sample + 1}")
            for spec in args.peek:
                addr, size = spec.split(":")
                print(f"    peek {addr}: {rsp.read(int(addr, 16), int(size)).hex()}")
            for t in threads:
                words = rsp.core_registers(t)
                pc, lr, sp = words[15], words[14], words[13]
                sym = symbolize(args.elf, [pc, lr])
                if sample > 0 and t != threads[0] and any("svc.s" in line for line in sym[:2]):
                    continue
                print(f"thread {t:x}: pc {pc:08x} lr {lr:08x} sp {sp:08x}")
                print("    r0-r12 " + " ".join(f"{w:08x}" for w in words[:13]))
                if t == threads[0] or args.stack_all:
                    # the thread's stack: words that point into the code, innermost first
                    stack = rsp.read(sp, 1024)
                    code = [int.from_bytes(stack[i:i + 4], "little") for i in range(0, len(stack), 4)]
                    code = [w for w in code if 0x100000 <= w < 0x500000][:args.stack_words]
                    if code:
                        names = subprocess.run(["addr2line", "-f", "-s", "-e", str(args.elf)] +
                                               [f"0x{w:x}" for w in code], capture_output=True, text=True).stdout.split()
                        print("    stack " + " ".join(names[0::2]))
                for line in sym:
                    print(f"    {line}")
            if sample + 1 < args.samples:
                rsp.send("c")
                time.sleep(args.interval)
        rsp.close()
        return 0
    finally:
        runner.send_signal(signal.SIGINT)  # run_azahar.py stops Azahar in its finally block
        try:
            runner.wait(timeout=30)
        except subprocess.TimeoutExpired:
            runner.kill()


if __name__ == "__main__":
    sys.exit(main())
