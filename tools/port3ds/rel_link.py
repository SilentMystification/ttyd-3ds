#!/usr/bin/env python3
"""Partially link one REL's objects for the 3DS port's static RELs.

    rel_link.py --rel aaa --ld arm-none-eabi-ld --objcopy arm-none-eabi-objcopy \\
        --out build/rel_aaa.o obj1.o obj2.o ...

On the GameCube a REL is a separate module: its symbols are its own, the
game loads it fresh from the disc for every area change (code, data and
read-only data reset), and only its _prolog, _epilog and _unresolved are
called from outside. The port links every REL into the executable, so:

  1. `ld -r -d` merges the REL's objects into one object (commons
     allocated), placing:
       .text.ttyd_rel_<rel>   all code
       .data.ttyd_rel_<rel>   data and read-only data, with
                              __ttyd_rel_<rel>_data_start/_end around it
       .bss.ttyd_rel_<rel>    bss, with __ttyd_rel_<rel>_bss_start/_end
     Read-only data goes in the writable data block because on the GameCube
     it is in writable RAM and is reloaded with the REL; the port restores
     the whole block from a snapshot at each load (port3ds/sdk/os_link.c).
  2. objcopy makes every symbol local except the entry points and the bounds,
     and renames _prolog/_epilog/_unresolved to ttyd_rel_<rel>_prolog etc.,
     so 29 RELs with clashing names link into one program. References to the
     DOL stay undefined and resolve in the final link.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = """
SECTIONS
{{
    .text.ttyd_rel_{r} : {{ *(.text .text.* .glue_7 .glue_7t .vfp11_veneer .v4_bx) }}
    .data.ttyd_rel_{r} ALIGN(8) : {{
        __ttyd_rel_{r}_data_start = .;
        *(.rodata .rodata.* .data .data.*)
        . = ALIGN(8);
        __ttyd_rel_{r}_data_end = .;
    }}
    .bss.ttyd_rel_{r} ALIGN(8) (NOLOAD) : {{
        __ttyd_rel_{r}_bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(8);
        __ttyd_rel_{r}_bss_end = .;
    }}
}}
"""

ENTRY_POINTS = ("_prolog", "_epilog", "_unresolved")


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        sys.exit(f"rel_link: {' '.join(cmd[:2])} failed")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rel", required=True)
    ap.add_argument("--ld", required=True)
    ap.add_argument("--objcopy", required=True)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("objects", nargs="+")
    args = ap.parse_args()

    r = args.rel
    with tempfile.TemporaryDirectory() as tmp:
        script = Path(tmp) / "rel.ld"
        script.write_text(SCRIPT.format(r=r))
        merged = Path(tmp) / "merged.o"
        run([args.ld, "-r", "-d", "-T", str(script), "-o", str(merged), *args.objects])

        keep = [f"__ttyd_rel_{r}_{k}" for k in ("data_start", "data_end", "bss_start", "bss_end")]
        # objcopy matches --keep-global-symbol against the renamed names.
        keep += [f"ttyd_rel_{r}{e}" for e in ENTRY_POINTS]
        cmd = [args.objcopy]
        for k in keep:
            cmd += ["--keep-global-symbol", k]
        for e in ENTRY_POINTS:
            cmd += ["--redefine-sym", f"{e}=ttyd_rel_{r}{e}"]
        args.out.parent.mkdir(parents=True, exist_ok=True)
        cmd += [str(merged), str(args.out)]
        run(cmd)


if __name__ == "__main__":
    main()
