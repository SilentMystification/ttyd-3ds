#!/usr/bin/env python3
"""Build-time check of a PICA200 vertex shader's output discipline.

    check_vsh.py SHADER.v.pica [--stamp FILE]

The PICA200 hangs on a vertex shader that writes an output register
partially or more than once (both seen on hardware by the earlier 3DS ports;
Azahar does not care). This parses the picasso source and fails when, in the
main procedure:

  - an output (`.out` name or o0-o15) is written other than exactly once;
  - a write to an output has a component mask other than none or .xyzw;
  - an output is written inside a branch or loop (ifu/ifc/for ... .end), or
    in any procedure other than main;
  - the bool uniform b15 is used (it does not work on hardware), or more than
    15 bool uniforms are declared.

Every `.out` must be written. With --stamp, writes FILE on success (for the
build's dependency tracking).
"""
import re
import sys
from pathlib import Path


def fail(path, line, msg):
    sys.stderr.write(f"{path}:{line}: check_vsh: {msg}\n")
    sys.exit(1)


def main():
    args = sys.argv[1:]
    stamp = None
    if "--stamp" in args:
        i = args.index("--stamp")
        stamp = args[i + 1]
        del args[i:i + 2]
    if len(args) != 1:
        sys.stderr.write(__doc__)
        return 2
    path = Path(args[0])
    outs = {}      # name -> register index or None
    out_regs = set()
    bools = 0
    writes = {}    # output key -> list of line numbers
    proc = None
    depth = 0
    for n, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        if re.search(r"\bb15\b", line):
            fail(path, n, "bool uniform b15 is used (it does not work on hardware)")
        tok = line.split()
        head = tok[0]
        if head == ".out":
            if len(tok) < 2:
                fail(path, n, "malformed .out")
            m = re.match(r"o(\d+)$", tok[1])
            name = tok[1].split(".")[0]
            outs[name] = int(m.group(1)) if m else None
            continue
        if head == ".bool":
            bools += len([t for t in re.split(r"[ ,]+", line[len(".bool"):]) if t])
            continue
        if head == ".proc":
            proc = tok[1] if len(tok) > 1 else ""
            depth = 0
            continue
        if head in ("ifu", "ifc", "for"):
            depth += 1
            continue
        if head == ".else":
            continue
        if head == ".end":
            if depth > 0:
                depth -= 1
            else:
                proc = None
            continue
        if head.startswith("."):
            continue
        # an instruction: the destination is the first operand
        ops = line[len(head):].strip()
        if not ops:
            continue
        dst = ops.split(",")[0].strip()
        base, _, mask = dst.partition(".")
        is_out = base in outs or re.fullmatch(r"o\d+", base) is not None
        if not is_out:
            continue
        key = base
        if proc != "main":
            fail(path, n, f"output {base} written outside main")
        if depth != 0:
            fail(path, n, f"output {base} written inside a branch or loop")
        if mask not in ("", "xyzw"):
            fail(path, n, f"partial write .{mask} to output {base}")
        writes.setdefault(key, []).append(n)
    if bools > 15:
        fail(path, 0, f"{bools} bool uniforms (b15 must stay unused)")
    for name in outs:
        lines = writes.get(name, [])
        if len(lines) != 1:
            fail(path, lines[1] if len(lines) > 1 else 0,
                 f"output {name} written {len(lines)} times (must be exactly once)")
    for key, lines in writes.items():
        if len(lines) != 1:
            fail(path, lines[1], f"output {key} written {len(lines)} times (must be exactly once)")
    print(f"check_vsh: {path.name}: {len(outs)} outputs, each written once in full")
    if stamp:
        Path(stamp).write_text("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
