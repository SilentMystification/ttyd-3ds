#!/usr/bin/env python3
"""Find event scripts whose last command is not a terminator.

An event script is an array of EvtWord: commands of (param_count << 16 | opcode)
followed by param_count words. A script must end in END_SCRIPT, END_EVENT or an
unconditional GOTO. One that does not runs on into whatever the compiler placed
after it. On the GameCube that was often the next script (so the game worked by
accident); on the 3DS the data layout differs.

Usage: tools/port3ds/evt_fallthrough.py [--elf build/3ds/game/ttyd.elf] [--json OUT]

Script names and source locations come from the EVT_BEGIN(name) and
`EvtWord name[] = {` definitions in src/ and rels/. Each is looked up in the
port ELF's symbol table (object symbols, with their sizes) and its words are
walked as commands. The GameCube successor is the next symbol by address in
config/G8ME01 (symbols.txt of the module); the 3DS successor is the next symbol
by address in the ELF. Python 3, standard library only.
"""

import argparse
import bisect
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OP_END_SCRIPT, OP_END_EVENT, OP_GOTO = 1, 2, 4


def load_opcodes():
    text = (ROOT / "include/manager/evtmgr.h").read_text()
    return {int(v) for v in re.findall(r"OPCODE_\w+\s*=\s*(\d+)", text)}


def find_sources():
    """{name: [(path, line)]} for every script definition in src/ and rels/."""
    out = {}
    pat = re.compile(r"^\s*(?:static\s+)?(?:EVT_BEGIN\((\w+)\)|EvtWord\s+(\w+)\[\]\s*=\s*\{)")
    for base in ("src", "rels"):
        for p in sorted((ROOT / base).rglob("*.c")):
            for i, line in enumerate(p.read_text(errors="replace").splitlines(), 1):
                m = pat.match(line)
                if m:
                    out.setdefault(m.group(1) or m.group(2), []).append((str(p.relative_to(ROOT)), i))
    return out


def load_elf(path):
    d = Path(path).read_bytes()
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    es, n = struct.unpack_from("<HH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * es) for i in range(n)]
    syms = []
    for s in secs:
        if s[1] != 2:
            continue
        so = secs[s[6]][4]
        cur_file = ""
        for o in range(s[4], s[4] + s[5], 16):
            nm, v, sz, info, _oth, shndx = struct.unpack_from("<IIIBBH", d, o)
            kind = info & 15
            name = d[so + nm:d.index(b"\0", so + nm)].decode("ascii", "replace")
            if kind == 4:
                cur_file = name
            elif kind == 1 and name and shndx < n:
                syms.append((v, sz, name, cur_file, shndx))
    return d, secs, sorted(syms)


def sec_words(d, secs, shndx, addr, size):
    sh = secs[shndx]
    if sh[1] == 8 or addr < sh[3] or addr + size > sh[3] + sh[5]:
        return None  # NOBITS or out of range
    off = sh[4] + addr - sh[3]
    return struct.unpack_from("<%di" % (size // 4), d, off)


def walk(words, opcodes):
    """Returns (ok, last_op): ok when the words parse as commands to the exact end."""
    i, last = 0, None
    while i < len(words):
        w = words[i] & 0xFFFFFFFF
        op, cnt = w & 0xFFFF, w >> 16
        if op not in opcodes or i + 1 + cnt > len(words):
            return False, None
        if op != 0:  # 0 is NEXT, also the zero padding after END_SCRIPT
            last = op
        i += 1 + cnt
    return True, last


def gc_symbols(module):
    """[(section, addr, size, name)] from config/G8ME01 for a module ('' = the main DOL)."""
    p = ROOT / "config/G8ME01" / module / "symbols.txt" if module else ROOT / "config/G8ME01/symbols.txt"
    out = []
    if not p.exists():
        return out
    pat = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // (.*)$")
    for line in p.read_text().splitlines():
        m = pat.match(line)
        if not m:
            continue
        sz = re.search(r"size:0x([0-9A-Fa-f]+)", m.group(4))
        out.append((m.group(2), int(m.group(3), 16), int(sz.group(1), 16) if sz else 0, m.group(1)))
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", default=str(ROOT / "build/3ds/game/ttyd.elf"))
    ap.add_argument("--json", help="also write the hits as JSON")
    args = ap.parse_args()

    opcodes = load_opcodes()
    sources = find_sources()
    d, secs, syms = load_elf(args.elf)
    addrs = [s[0] for s in syms]

    scripts, hits = 0, []
    for idx, (addr, size, name, cfile, shndx) in enumerate(syms):
        if name not in sources or size < 4 or size % 4:
            continue
        words = sec_words(d, secs, shndx, addr, size)
        if words is None:
            continue
        ok, last = walk(words, opcodes)
        if not ok:
            continue
        scripts += 1
        if last in (OP_END_SCRIPT, OP_END_EVENT, OP_GOTO):
            continue
        cands = [s for s in sources[name] if Path(s[0]).name == cfile] or sources[name]
        src = cands[0]
        # 3DS successor: next symbol with a different address
        j = idx + 1
        while j < len(syms) and syms[j][0] == addr:
            j += 1
        succ3 = syms[j][2] if j < len(syms) else "?"
        # GameCube successor: next symbol in the module's symbols.txt
        m = re.match(r"rels/(\w+)/", src[0])
        gc = gc_symbols(m.group(1) if m else "")
        cur = [g for g in gc if g[3] == name]
        succ_gc, is_script = "?", "n/a"
        if cur:
            g = cur[0]
            nxt = [x for x in gc if x[0] == g[0] and x[1] >= g[1] + g[2] and x[1] != g[1]]
            if nxt:
                nx = min(nxt, key=lambda x: x[1])
                succ_gc = nx[3] + ("" if nx[1] == g[1] + g[2] else " (+gap)")
                is_script = "yes" if nx[3] in sources else "NO"
        hits.append({"script": name, "source": "%s:%d" % src, "words": size // 4,
                     "last_op": last, "gc_successor": succ_gc, "gc_successor_is_script": is_script,
                     "n3ds_successor": succ3})

    print("scripts scanned: %d, without a terminator: %d" % (scripts, len(hits)))
    if hits:
        cols = ["script", "source", "gc_successor", "n3ds_successor", "gc_successor_is_script"]
        rows = [[str(h[c]) for c in cols] for h in hits]
        w = [max(len(c), *(len(r[i]) for r in rows)) for i, c in enumerate(cols)]
        print("  ".join(c.ljust(w[i]) for i, c in enumerate(cols)))
        for r in rows:
            print("  ".join(v.ljust(w[i]) for i, v in enumerate(r)))
    if args.json:
        Path(args.json).write_text(json.dumps(hits, indent=1))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
