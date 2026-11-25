#!/usr/bin/env python3
"""Prove that the geometry the port reads from the disc equals what it used to compile in (byte equality).

    check_geometry.py --elf OLD.elf [--iso iso/game.iso] [--version G8ME01]

OLD.elf is a port executable built BEFORE the geometry arrays were taken out of the sources (their initialisers still
compiled in; for example the build of the commit before "geometry from the disc"). For each line of
tools/port3ds/disc_geometry.txt this reads the array from the disc image the way the port's loader does (main.dol by
address, then the format's byte swap, tools/port3ds/disc_geo.py) and looks for a symbol of that name in OLD.elf with
the same bytes. The u32 display lists (format dl32) were byte-swapped by a constructor at start-up; the swap is
applied to the old bytes first (disc_geo.compiled_to_memory). A shorter read than the array (shadow_vtx) needs the
rest of the old array to be zero, which is what the port's zeroed array holds.

Exit status 0 only when every entry is equal. Prints the counts and every mismatch. Needs the disc of the version
(the repository holds no game data, so this is a developer check, not part of the build).
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import disc_geo  # noqa: E402
import gen_disc_data  # noqa: E402

NM = "/opt/devkitpro/devkitARM/bin/arm-none-eabi-nm"


def elf_loads(path):
    d = Path(path).read_bytes()
    if d[:4] != b"\x7fELF" or d[4] != 1:
        sys.exit(f"check_geometry: {path} is not a 32-bit ELF")
    phoff, = struct.unpack_from("<I", d, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", d, 0x2A)
    loads = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from("<8I", d, phoff + i * phentsize)
        if p_type == 1:
            loads.append((p_vaddr, p_off, p_filesz))
    return d, loads


def elf_symbols(nm, path):
    """name (without a '.N' suffix) -> [(address, size)] for initialised data symbols."""
    out = subprocess.run([nm, "-S", "-n", "--defined-only", str(path)], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "dDrR":
            syms.setdefault(p[3].split(".")[0], []).append((int(p[0], 16), int(p[1], 16)))
    return syms


def elf_read(d, loads, addr, size):
    for vaddr, off, filesz in loads:
        if vaddr <= addr and addr + size <= vaddr + filesz:
            return d[off + addr - vaddr: off + addr - vaddr + size]
    return None  # not in the file (zero-initialised)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--elf", required=True, type=Path, help="port ELF built before the arrays left the sources")
    ap.add_argument("--iso", type=Path, default=gen_disc_data.REPO / "iso" / "game.iso")
    ap.add_argument("--version", default="G8ME01")
    ap.add_argument("--nm", default=NM)
    args = ap.parse_args()

    cfg = gen_disc_data.REPO / "config" / args.version
    all_syms = gen_disc_data.symbols_all(cfg / "symbols.txt")
    dol = disc_geo.Dol(args.iso)
    if dol.game_id != args.version:
        sys.exit(f"check_geometry: {args.iso} is {dol.game_id}, not {args.version}")
    d, loads = elf_loads(args.elf)
    elf_syms = elf_symbols(args.nm, args.elf)

    entries = gen_disc_data.geometry_entries()
    equal = 0
    bad = []
    absent = []  # unused statics the linker dropped: nothing was compiled in, nothing to compare
    used = set()  # ELF (address) already matched: statics of one name repeat
    for key, sym, occ, fmt, size, rel in entries:
        if rel:
            section, addr, _ = gen_disc_data.symbols_all(cfg / rel / "symbols.txt")[sym][occ]
            index = gen_disc_data.rel_sections(cfg / rel / "splits.txt")[section]
            disc = disc_geo.swap(dol.read_rel_section(rel, index, addr, size), fmt)
        else:
            addr = all_syms[sym][occ][1]
            disc = disc_geo.swap(dol.read(addr, size), fmt)
        found = None
        why = "symbol not in the ELF"
        for ea, es in elf_syms.get(sym, []):
            if ea in used:
                continue
            if es < size:
                why = f"ELF array is {es} bytes, entry {size}"
                continue
            raw = elf_read(d, loads, ea, es)
            if raw is None:
                why = "ELF array has no file contents"
                continue
            mem = disc_geo.compiled_to_memory(raw[:size], fmt)
            tail_zero = not any(raw[size:])
            if mem == disc and tail_zero:
                found = ea
                break
            why = "bytes differ" if mem != disc else "bytes after the read are not zero"
        if found is None and why == "symbol not in the ELF":
            absent.append(key)
        elif found is None:
            bad.append((key, sym, fmt, size, why))
        else:
            used.add(found)
            equal += 1

    by_fmt = {}
    for _, _, _, fmt, _, _ in entries:
        by_fmt[fmt] = by_fmt.get(fmt, 0) + 1
    print(f"check_geometry: {len(entries)} entries ({', '.join(f'{k} {v}' for k, v in sorted(by_fmt.items()))}), "
          f"{equal} equal, {len(bad)} mismatched, {len(absent)} not in the ELF (unused, dropped by the linker)")
    for key in absent:
        print(f"  not in the ELF: {key}")
    for key, sym, fmt, size, why in bad:
        print(f"  MISMATCH {key} ({sym}, {fmt}, {size} bytes): {why}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
