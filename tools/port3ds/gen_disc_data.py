#!/usr/bin/env python3
"""Generate port3ds' table of data the decomp compiles in but ports read from the disc.

    gen_disc_data.py --version G8ME01 OUT.h
    gen_disc_data.py --print-arena iso/G8MP01.iso     # fills ARENA_HI for a version (needs that disc once)

Ports ship no game data (docs/PORT_3DS_PLAN.md 6.1). Image arrays written
in game sources, and the memory card art the matching build extracts from
main.dol, are declared without contents on ports and filled at boot from
the user's disc image. This writes, for each, where it is in this version:
main.dol (address) or a REL (section index and offset), and its size,
read from config/<version>/symbols.txt and config/<version>/<rel>/.

Each entry becomes PORT_DISC_<key> = { location, section, offset, size }.

It also writes the GameCube arena the game sees at main() for this version
(PORT_GAME_ARENA_LO/HI, port3ds/game/os.c):
  - the low end is main.dol's end (from the symbols) plus the 64 KiB stack;
  - the high end is where the IPL puts the FST, under 0x81800000 by the
    disc's maximum FST size. It comes from the committed ARENA_HI table below,
    not from a disc: the US value was computed from the US disc (and equals the
    one measured in Dolphin); JP and PAL are unknown (None) until someone with
    that disc runs --print-arena once and fills the table. An unknown version
    builds with the US value as a stand-in and a warning
    (PORT_GAME_ARENA_HI_KNOWN 0); tools/port3ds/release.py refuses to package
    such a version (docs/RELEASE.md).
"""

import argparse
import re
import struct
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# key: (where, symbol, size of the port's array). where is "dol" or a REL
# name. A symbol may be listed larger than the array (the size of an
# unverified symbol runs to the next one, alignment padding included); the
# array's size is what is read.
ENTRIES = {
    "mariost_icon_tex": ("dol", "_mariost_icon_tex", 0x400),
    "mariost_icon_tlut": ("dol", "_mariost_icon_tlut", 0x200),
    "mariost_banner_tex": ("dol", "_mariost_banner_tex", 0x1800),
    "mapdrv_texture_0": ("dol", "texture_0", 0x100),
    "torch_obj2_texture": ("dol", "torch_obj2_texture", 0x3F0),
    "torch_obj22t_texture": ("dol", "torch_obj22t_texture", 0x3F0),
    "cubeDN": ("dol", "cubeDN", 0x1338),
    "win_texture_0": ("win", "texture_0", 0x1380),
}

STACK_SIZE = 0x10000  # the DOL's stack sits between its bss and the arena
MEM_TOP = 0x81800000
US_ARENA_HI = 0x817EE1A0  # G8ME01: MEM_TOP - the disc's max FST size (rounded up to 32), also measured in Dolphin

# The arena's high end per version: None = unknown, needs that disc once (--print-arena). Demo discs are not
# release versions and use the US stand-in.
ARENA_HI = {
    "G8ME01": US_ARENA_HI,
    "G8MJ01": 0x817EE740,  # JP: from the JP disc (gen_disc_data.py --print-arena iso/G8MJ01.iso), 2026-11-24
    "G8MP01": 0x817E8020,  # PAL: from the PAL disc (gen_disc_data.py --print-arena iso/G8MP01.iso), 2026-11-24
}


def arena_hi(version):
    """(value, known): the table's value, or the US stand-in for an unknown version."""
    v = ARENA_HI.get(version)
    return (US_ARENA_HI, False) if v is None else (v, True)

SYM = re.compile(r"^(\S+) = (\.[a-z0-9]+):0x([0-9A-Fa-f]+); // .*?size:0x([0-9A-Fa-f]+)")

# Geometry: one line per array in disc_geometry.txt (key symbol occurrence format bytes). The format says how the
# disc's big-endian bytes become the port's memory (tools/port3ds/disc_geo.py); the loader in
# port3ds/game/disc_data.c takes the number.
GEOMETRY_FILE = REPO / "tools" / "port3ds" / "disc_geometry.txt"
FORMATS = ("raw", "s16", "swap32", "n64vtx", "dl32")  # the order of disc_geo.FORMATS and of port_disc_data.h


def symbols(path):
    out = {}
    for line in path.read_text().splitlines():
        m = SYM.match(line)
        if m:
            out.setdefault(m.group(1), (m.group(2), int(m.group(3), 16), int(m.group(4), 16)))
    return out


def symbols_all(path):
    """name -> [(section, address, size), ...] in file order (statics repeat across files)."""
    out = {}
    for line in path.read_text().splitlines():
        m = SYM.match(line)
        if m:
            out.setdefault(m.group(1), []).append((m.group(2), int(m.group(3), 16), int(m.group(4), 16)))
    return out


def geometry_entries(path=GEOMETRY_FILE):
    """[(key, symbol, occurrence, format, bytes, rel)] from disc_geometry.txt (rel "" = main.dol)."""
    out = []
    for n, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        f = line.split()
        if len(f) not in (5, 6) or f[3] not in FORMATS:
            sys.exit(f"gen_disc_data: {path}:{n}: expected 'key symbol occurrence format bytes [rel]'")
        out.append((f[0], f[1], int(f[2]), f[3], int(f[4]), f[5] if len(f) == 6 else ""))
    return out


def dol_end(path):
    end = 0
    for line in path.read_text().splitlines():
        m = SYM.match(line)
        if m:
            end = max(end, int(m.group(3), 16) + int(m.group(4), 16))
    return end


def rel_sections(path):
    names = []
    in_sections = False
    for line in path.read_text().splitlines():
        if line.startswith("Sections:"):
            in_sections = True
            continue
        if in_sections:
            m = re.match(r"\s+(\.\S+)\s+type:", line)
            if not m:
                break
            names.append(m.group(1))
    return {name: i + 1 for i, name in enumerate(names)}


def disc_arena_hi(version, disc):
    """The FST address on the GameCube for this disc; exits unless it is the version's disc."""
    need = f"gen_disc_data: needs the {version} disc"
    if disc is None or not disc.is_file():
        sys.exit(f"{need} ({disc or 'no --disc given'} not found)")
    with open(disc, "rb") as f:
        head = f.read(0x440)
    if len(head) < 0x440 or struct.unpack(">I", head[0x1C:0x20])[0] != 0xC2339F3D:
        sys.exit(f"{need} ({disc} is not a GameCube disc image)")
    found = head[:6].decode("ascii", "replace")
    if found != version or head[7] != 0:
        sys.exit(f"{need} ({disc} is {found} revision {head[7]})")
    fst_max = struct.unpack(">I", head[0x42C:0x430])[0]
    return MEM_TOP - ((fst_max + 31) & ~31)


def print_arena(disc):
    """--print-arena: the table line for a disc image."""
    if not disc.is_file():
        sys.exit(f"gen_disc_data: {disc} not found")
    with open(disc, "rb") as f:
        head = f.read(0x440)
    if len(head) < 0x440 or struct.unpack(">I", head[0x1C:0x20])[0] != 0xC2339F3D:
        sys.exit(f"gen_disc_data: {disc} is not a GameCube disc image")
    version = head[:6].decode("ascii", "replace")
    if head[7] != 0:
        print(f"gen_disc_data: warning: {disc} is revision {head[7]}; the table is for revision 0")
    hi = disc_arena_hi(version, disc)
    print(f'    "{version}": 0x{hi:08X},')
    print(f"gen_disc_data: put the line above in ARENA_HI in tools/port3ds/gen_disc_data.py"
          f" (table now: {ARENA_HI.get(version)!r})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--version", default="G8ME01")
    ap.add_argument("--print-arena", type=Path, metavar="DISC", help="print the ARENA_HI table line for this disc image")
    ap.add_argument("out", type=Path, nargs="?")
    args = ap.parse_args()
    if args.print_arena:
        print_arena(args.print_arena)
        return
    if args.out is None:
        ap.error("OUT.h is required")
    cfg = REPO / "config" / args.version
    dol_syms = symbols(cfg / "symbols.txt")
    lines = [f"/* Generated by tools/port3ds/gen_disc_data.py ({args.version}). Do not edit. */",
             "#pragma once", ""]
    for key, (where, sym, want) in ENTRIES.items():
        path = cfg / "symbols.txt" if where == "dol" else cfg / where / "symbols.txt"
        syms = dol_syms if where == "dol" else symbols(path)
        if sym not in syms:
            sys.exit(f"gen_disc_data: {sym} not in {path}")
        section, addr, size = syms[sym]
        if size < want:
            sys.exit(f"gen_disc_data: {sym} is 0x{size:X} bytes in {path}, the port's array 0x{want:X}")
        if where == "dol":
            lines.append(f'#define PORT_DISC_{key} "", 0, 0x{addr:08X}u, 0x{want:X}u, 0')
        else:
            index = rel_sections(cfg / where / "splits.txt")[section]
            lines.append(f'#define PORT_DISC_{key} "{where}", {index}, 0x{addr:08X}u, 0x{want:X}u, 0')

    all_syms = symbols_all(cfg / "symbols.txt")
    for key, sym, occ, fmt, size, rel in geometry_entries():
        table = symbols_all(cfg / rel / "symbols.txt") if rel else all_syms
        found = table.get(sym, [])
        if occ >= len(found):
            sys.exit(f"gen_disc_data: {sym} occurrence {occ} not in the symbols of {rel or 'main.dol'} ({len(found)} found)")
        section, addr, sym_size = found[occ]
        if sym_size < size:
            sys.exit(f"gen_disc_data: {sym} is 0x{sym_size:X} bytes in symbols.txt, the port reads 0x{size:X}")
        if rel:
            index = rel_sections(cfg / rel / "splits.txt")[section]
            lines.append(f'#define PORT_DISC_{key} "{rel}", {index}, 0x{addr:08X}u, 0x{size:X}u, {FORMATS.index(fmt)}')
        else:
            lines.append(f'#define PORT_DISC_{key} "", 0, 0x{addr:08X}u, 0x{size:X}u, {FORMATS.index(fmt)}')

    lo = ((dol_end(cfg / "symbols.txt") + 31) & ~31) + STACK_SIZE
    hi, known = arena_hi(args.version)
    if not known:
        print(f"gen_disc_data: warning: {args.version}: the arena's high end is unknown, using the US value "
              "as a stand-in (fine for development, not for a release: gen_disc_data.py --print-arena "
              f"iso/{args.version}.iso fills the table)", file=sys.stderr)
    lines += ["", "/* The game arena at main() on the GameCube (port3ds/game/os.c). */",
              f"#define PORT_GAME_ARENA_LO 0x{lo:08X}u",
              f"#define PORT_GAME_ARENA_HI 0x{hi:08X}u",
              f"#define PORT_GAME_ARENA_HI_KNOWN {int(known)}"]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
