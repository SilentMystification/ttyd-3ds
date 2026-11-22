#!/usr/bin/env python3
"""Resolve the addresses of a crash report to functions and file:line.

    tools/port3ds/crash_report.py build/azahar-last/game_log.txt
    tools/port3ds/crash_report.py path/to/crash.txt --elf build/3ds/game/ttyd.elf
    tools/port3ds/crash_report.py --ftp 192.168.1.50        # pulls /3ds/ttyd/crash.txt and log.txt

The input is a crash.txt (every crash, appended), a game log, or the two files
from the console over FTP (ftpd on port 5000). Each report is a run of
"[CRASH]" lines. The report's build string ("#171 2681d5eb") picks the ELF
that tools/port3ds/build.sh archived as build/elfs/171_2681d5eb.elf (--elf
overrides it). Resolved are the pc= and lr= values (of the failing thread and
of every thread in a watchdog dump) and every word of the "stack" lines (the
stack scan's candidate return addresses). Uses devkitARM's addr2line when it
is on PATH or in /opt/devkitpro/devkitARM/bin; otherwise the ELF's symbol
table, which gives the function only.
"""
import argparse
import bisect
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import elfsyms  # noqa: E402

HEAD = re.compile(r"^\[CRASH\] (data abort|prefetch abort|undefined instruction|VFP|exception|fatal|svcBreak|watchdog)")
BUILD = re.compile(r"build #(\d+) ([0-9a-f]{6,})")
KV = re.compile(r"\b(pc|lr)=([0-9a-f]{8})\b")
HEX = re.compile(r"\b[0-9a-f]{8}\b")
CODE_LO, CODE_HI = 0x00100000, 0x02000000


def split_reports(text):
    """[(lines)] one list of [CRASH] lines per report."""
    reports, cur = [], None
    for line in text.splitlines():
        line = re.sub(r"^#\d+ ", "", line)  # the log's line counter
        if not line.startswith("[CRASH]"):
            continue
        if HEAD.match(line):
            cur = []
            reports.append(cur)
        if cur is None:
            cur = []
            reports.append(cur)
        cur.append(line)
    return reports


def addresses(lines):
    """[(label, addr)] in order, deduplicated per label."""
    out, seen = [], set()
    for line in lines:
        found = []
        if line.startswith("[CRASH] stack ") and not line.startswith("[CRASH] stack scan"):
            if re.match(r"\[CRASH\] stack( [0-9a-f]{8})+\s*$", line):
                found = [("stack", int(h, 16)) for h in HEX.findall(line[len("[CRASH] stack"):])]
        else:
            m = re.search(r"thread[= ](\S+?)(?: \(stuck\))?:? ", line)
            who = m.group(1) if m else ""
            found = [(f"{k}{' ' + who if who else ''}", int(v, 16)) for k, v in KV.findall(line)]
        for label, a in found:
            if CODE_LO <= a < CODE_HI and (label, a) not in seen:
                seen.add((label, a))
                out.append((label, a))
    return out


def find_addr2line():
    for cand in ("arm-none-eabi-addr2line", "/opt/devkitpro/devkitARM/bin/arm-none-eabi-addr2line"):
        p = shutil.which(cand) or (cand if Path(cand).is_file() else None)
        if p:
            return p
    return None


class Resolver:
    def __init__(self, elf):
        self.elf = elf
        self.a2l = find_addr2line()
        self.syms = None
        if not self.a2l:
            s = sorted(elfsyms.load_symbols(elf).values(), key=lambda x: x.addr)
            self.syms = ([x.addr for x in s], s)

    def resolve(self, addrs):
        """{addr: 'function  file:line'}"""
        if not addrs:
            return {}
        if self.a2l:
            # return addresses point after the call: look up the call itself
            q = [f"{a - 1 if a > 4 else a:x}" for a in addrs]
            res = subprocess.run([self.a2l, "-e", str(self.elf), "-f", "-C", "-a"] + q, capture_output=True,
                                 text=True)
            lines = res.stdout.splitlines()
            out = {}
            for i, a in enumerate(addrs):
                try:
                    fn, loc = lines[i * 3 + 1], lines[i * 3 + 2]
                except IndexError:
                    out[a] = "?"
                    continue
                loc = re.sub(r"^.*?/work/", "", loc)
                out[a] = f"{fn}  {loc}"
            return out
        keys, syms = self.syms
        out = {}
        for a in addrs:
            i = bisect.bisect_right(keys, a) - 1
            s = syms[i] if i >= 0 else None
            out[a] = f"{s.name}+{a - s.addr:#x}" if s and (s.size == 0 or a < s.addr + s.size) else "?"
        return out


def pull_ftp(ip, port, dest):
    from ftplib import FTP
    dest.mkdir(parents=True, exist_ok=True)
    got = []
    ftp = FTP()
    ftp.connect(ip, port, timeout=15)
    ftp.login()
    for name in ("crash.txt", "log.txt"):
        path = dest / name
        try:
            with open(path, "wb") as f:
                ftp.retrbinary(f"RETR /3ds/ttyd/{name}", f.write)
            got.append(path)
        except Exception as e:  # missing file is normal (no crash yet)
            print(f"crash_report: /3ds/ttyd/{name}: {e}", file=sys.stderr)
            path.unlink(missing_ok=True)
    ftp.quit()
    return got


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file", nargs="*", type=Path, help="crash.txt or a log file")
    ap.add_argument("--ftp", metavar="IP", help="pull /3ds/ttyd/crash.txt and log.txt from the console")
    ap.add_argument("--ftp-port", type=int, default=5000)
    ap.add_argument("--elf", type=Path, help="ELF to use instead of build/elfs/<build>.elf")
    ap.add_argument("--elfdir", type=Path, default=ROOT / "build/elfs")
    ap.add_argument("--last", action="store_true", help="only the last report of each file")
    args = ap.parse_args()

    files = list(args.file)
    if args.ftp:
        files += pull_ftp(args.ftp, args.ftp_port, ROOT / "build/crash-last")
    if not files:
        ap.error("give a file or --ftp <ip>")

    status = 0
    for f in files:
        reports = split_reports(f.read_text(errors="replace"))
        if args.last:
            reports = reports[-1:]
        print(f"== {f}: {len(reports)} crash report(s)")
        for n, rep in enumerate(reports, 1):
            m = next((BUILD.search(l) for l in rep if BUILD.search(l)), None)
            print(f"\n-- report {n}: {rep[0][len('[CRASH] '):][:140]}")
            elf = args.elf
            if elf is None and m:
                elf = args.elfdir / f"{m.group(1)}_{m.group(2)[:8]}.elf"
            if elf is None or not elf.is_file():
                print(f"   no ELF for build {m.group(0) if m else '(no build string)'}: expected {elf}; use --elf")
                status = 1
                continue
            if m:
                print(f"   build #{m.group(1)} {m.group(2)}  ({elf})")
            addrs = addresses(rep)
            res = Resolver(elf).resolve(sorted({a for _, a in addrs}))
            for label, a in addrs:
                print(f"   {a:08x}  {label:<14} {res.get(a, '?')}")
    return status


if __name__ == "__main__":
    sys.exit(main())
