#!/usr/bin/env python3
"""Generate the byte-swap tables for disc formats from the port's own debug info.

    gen_swap.py probe --out swap_probe.c
    gen_swap.py tables --readelf READELF --obj swap_probe.o --out-c port_swap_gen.c --out-h port_swap_gen.h

Disc files are big-endian. Pointers are 32 bits on the 3DS as on the
GameCube and every struct has the GameCube layout (check_layout.py), so a
loaded file is converted by swapping each 2-, 4- and 8-byte field in place.
Which fields those are comes from the compiler, not from hand-written lists:

  1. `probe` writes a C file that includes each format's headers and names
     each struct the format's loader converts.
  2. CMake compiles it with the game's flags and full debug info.
  3. `tables` reads the object's DWARF (readelf --debug-dump=info) and
     writes, for each struct, its runs of 2/4/8-byte units (offset, width,
     count). One-byte fields are left alone. Pointer fields are 4-byte units
     (offsets in the file before the game relocates them).

Bitfields are refused: their bit order differs between the two machines, so
they need a loader of their own. A union is accepted when every member gives
the same units; otherwise it is refused and the format's loader must handle
the field itself (listed in OVERRIDES as raw bytes).

The format loaders in port3ds/game/formats/ walk each file's pointer graph
(after paper-cut's port/bedecode/*.spec) and swap the structs they reach with
these tables.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

# format: (headers, structs)
FORMATS = {
    "tpl": (["sdk/texPalette.h"], ["TEXPalette", "TEXDescriptor", "TEXHeader", "CLUTHeader"]),
    "anim": (["driver/animdrv.h"],
             ["AnimationPoseData", "AnimPoseShape", "AnimDrawCallEntry", "AnimTexMtx", "AnimPoseTexEntry",
              "AnimTexBind", "AnimPoseShapeDraw", "AnimPoseGroup", "AnimTableEntry", "AnimData", "AnimKeyframe",
              "AnimTexDelta", "AnimValueKey"]),
    "font": (["manager/fontmgr.h"],
             ["BinaryFontHeader", "BinaryFontBlock", "BinaryFontInfo", "BinaryFontGlyph", "BinaryFontMap",
              "BinaryFontWidth"]),
    "map": (["driver/mapdrv.h"],
            ["MapFileHeader", "MapFileChunk", "MapFileInfo", "MapFileJoint", "MapFileJointPart", "MapJointDrawMode",
             "MapMaterial", "MapMatSampler", "MapTexInfo", "MapTexTable", "MapMaterialName", "MapMaterialNameTable",
             "MapFileLightTable", "MapFileLight", "MapFileFog", "MapVcdTable", "MapVtxRec", "MapRawDL",
             "MapFileMesh", "MapAnimHeader"]),
}

# (struct, member path) -> "raw": bytes left as they are by the generic swap
# (the format loader converts them itself, or they are byte data).
OVERRIDES = {
    # isPackedDisplay picks the member: four-byte pointers or {offset, size} pairs (formats/map.c).
    ("MapFileMesh", "displayLists"): "raw",
}

DIE = re.compile(r"^\s*<(\d+)><([0-9a-f]+)>: Abbrev Number: (\d+)(?: \((DW_TAG_\w+)\))?")
ATTR = re.compile(r"^\s*<[0-9a-f]+>\s+(DW_AT_\w+)\s*:\s*(.*)$")


class Die:
    __slots__ = ("off", "tag", "attrs", "children", "parent")

    def __init__(self, off, tag, parent):
        self.off, self.tag, self.attrs, self.children, self.parent = off, tag, {}, [], parent

    def name(self):
        v = self.attrs.get("DW_AT_name")
        if v is None:
            return None
        m = re.match(r"\(indirect (?:line )?string, offset: 0x[0-9a-f]+\): (.*)$", v)
        return m.group(1).strip() if m else v.strip()

    def ref(self, attr="DW_AT_type"):
        v = self.attrs.get(attr)
        if v is None:
            return None
        m = re.match(r"<0x([0-9a-f]+)>", v)
        return int(m.group(1), 16) if m else None

    def num(self, attr):
        v = self.attrs.get(attr)
        if v is None:
            return None
        m = re.match(r"(-?(?:0x[0-9a-f]+|\d+))", v.strip())
        if not m:
            raise SystemExit(f"gen_swap: cannot read {attr} = {v!r} at <{self.off:x}>")
        return int(m.group(1), 0)


def parse(text):
    dies = {}
    stack = []
    top = []
    cur = None
    for line in text.splitlines():
        m = DIE.match(line)
        if m:
            depth, off, abbrev, tag = int(m.group(1)), int(m.group(2), 16), int(m.group(3)), m.group(4)
            del stack[depth:]
            if abbrev == 0:
                cur = None
                continue
            parent = stack[-1] if stack else None
            cur = Die(off, tag, parent)
            dies[off] = cur
            if parent is not None:
                parent.children.append(cur)
            if depth == 1:
                top.append(cur)
            stack.append(cur)
            continue
        m = ATTR.match(line)
        if m and cur is not None:
            cur.attrs[m.group(1)] = m.group(2)
    return dies, top


class Flattener:
    def __init__(self, dies):
        self.dies = dies

    def resolve(self, die):
        while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"):
            r = die.ref()
            die = self.dies.get(r) if r is not None else None
        return die

    def size(self, die):
        die = self.resolve(die)
        if die is None:
            return 0
        if die.tag == "DW_TAG_array_type":
            return self.size(self.dies[die.ref()]) * self.count(die)
        return die.num("DW_AT_byte_size") or 0

    def count(self, arr):
        n = 1
        for sub in arr.children:
            if sub.tag != "DW_TAG_subrange_type":
                continue
            if "DW_AT_count" in sub.attrs:
                n *= sub.num("DW_AT_count")
            elif "DW_AT_upper_bound" in sub.attrs:
                n *= sub.num("DW_AT_upper_bound") + 1
            else:
                n *= 0  # flexible array member: no storage in the struct
        return n

    def units(self, die, base, path, owner):
        """[(offset, width)] of the 2/4/8-byte scalars in a value of type die."""
        if (owner, path) in OVERRIDES:
            return []
        die = self.resolve(die)
        if die is None:
            return []
        tag = die.tag
        if tag in ("DW_TAG_base_type", "DW_TAG_enumeration_type", "DW_TAG_pointer_type"):
            size = die.num("DW_AT_byte_size")
            if size in (2, 4, 8):
                return [(base, size)]
            if size == 1:
                return []
            raise SystemExit(f"gen_swap: {owner}.{path}: a {size}-byte scalar")
        if tag == "DW_TAG_array_type":
            elem = self.dies[die.ref()]
            esize = self.size(elem)
            one = self.units(elem, 0, path + "[]", owner)
            out = []
            for i in range(self.count(die)):
                out += [(base + i * esize + o, w) for o, w in one]
            return out
        if tag == "DW_TAG_structure_type":
            out = []
            for m in die.children:
                if m.tag != "DW_TAG_member":
                    continue
                name = m.name() or "?"
                sub = f"{path}.{name}" if path else name
                if "DW_AT_bit_size" in m.attrs:
                    raise SystemExit(f"gen_swap: {owner}.{sub} is a bitfield; convert it in the format loader "
                                     "(and list it in OVERRIDES)")
                loc = m.num("DW_AT_data_member_location") or 0
                out += self.units(self.dies[m.ref()], base + loc, sub, owner)
            return out
        if tag == "DW_TAG_union_type":
            variants = []
            for m in die.children:
                if m.tag == "DW_TAG_member":
                    name = m.name() or "?"
                    sub = f"{path}.{name}" if path else name
                    variants.append(self.units(self.dies[m.ref()], base, sub, owner))
            if variants and all(v == variants[0] for v in variants):
                return variants[0]
            raise SystemExit(f"gen_swap: {owner}.{path} is a union whose members differ; list it in OVERRIDES "
                             "and convert it in the format loader")
        raise SystemExit(f"gen_swap: {owner}.{path}: unsupported {tag}")


def runs(units):
    units = sorted(set(units))
    out = []
    for off, w in units:
        if out and out[-1][1] == w and out[-1][0] + out[-1][2] * w == off:
            out[-1][2] += 1
        else:
            out.append([off, w, 1])
    for (a, wa, na), (b, _, _) in zip(out, out[1:]):
        if a + wa * na > b:
            raise SystemExit(f"gen_swap: overlapping units at {a:#x} and {b:#x}")
    return out


def find_type(top, dies, name):
    for d in top:
        if d.tag == "DW_TAG_typedef" and d.name() == name:
            return d
    for d in top:
        if d.tag == "DW_TAG_structure_type" and d.name() == name:
            return d
    raise SystemExit(f"gen_swap: type {name} not in the probe's debug info")


def cmd_probe(args):
    lines = ["/* Generated by tools/port3ds/gen_swap.py: names the structs of the disc formats so their",
             " * layout is in this object's debug info. Not linked. */"]
    headers = []
    for hs, _ in FORMATS.values():
        for h in hs:
            if h not in headers:
                headers.append(h)
    lines += [f'#include "{h}"' for h in headers]
    for fmt, (_, structs) in FORMATS.items():
        for s in structs:
            lines.append(f"{s}* port_swap_probe_{fmt}_{s};")
    Path(args.out).write_text("\n".join(lines) + "\n")


def cmd_tables(args):
    text = subprocess.run([args.readelf, "--debug-dump=info", args.obj], capture_output=True, text=True,
                          check=True).stdout
    dies, top = parse(text)
    fl = Flattener(dies)
    c = ["/* Generated by tools/port3ds/gen_swap.py from the port's debug info. Do not edit. */",
         '#include "port3ds/port_swap.h"']
    headers = []
    for hs, _ in FORMATS.values():
        headers += [h for h in hs if h not in headers]
    if not args.host:
        c += [f'#include "{h}"' for h in headers]
    names = []
    h = ["/* Generated by tools/port3ds/gen_swap.py. Do not edit. */", "#pragma once",
         '#include "port3ds/port_swap.h"']
    done = set()
    for fmt, (_, structs) in FORMATS.items():
        for s in structs:
            if s in done:
                continue
            done.add(s)
            d = find_type(top, dies, s)
            size = fl.size(d)
            rs = runs(fl.units(d, 0, "", s))
            if not args.host:
                c.append(f"_Static_assert(sizeof({s}) == {size}, \"{s}: layout changed since the probe\");")
            names.append(s)
            c.append(f"static const PortSwapRun runs_{s}[] = {{")
            c += [f"    {{{o}, {w}, {n}}}," for o, w, n in rs] or ["    {0, 0, 0},"]
            c.append("};")
            c.append(f"const PortSwapType port_swap_type_{s} = {{\"{s}\", {size}, {len(rs)}, runs_{s}}};")
            h.append(f"extern const PortSwapType port_swap_type_{s};")
    if args.host:
        # Host decode tool (tools/port3ds/host_decode): the layouts come from a 32-bit probe object, so there are no
        # game headers to check against; every table is listed for the swap round-trip test.
        c.append("const PortSwapType* const port_swap_all_types[] = {")
        c += [f"    &port_swap_type_{n}," for n in names]
        c.append("};")
        c.append(f"const unsigned port_swap_all_count = {len(names)};")
        h.append("extern const PortSwapType* const port_swap_all_types[];")
        h.append("extern const unsigned port_swap_all_count;")
    Path(args.out_c).write_text("\n".join(c) + "\n")
    Path(args.out_h).write_text("\n".join(h) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("probe")
    p.add_argument("--out", required=True)
    t = sub.add_parser("tables")
    t.add_argument("--readelf", required=True)
    t.add_argument("--obj", required=True)
    t.add_argument("--out-c", required=True)
    t.add_argument("--out-h", required=True)
    t.add_argument("--host", action="store_true",
                   help="tables for the host decode tool: no game headers, no layout asserts, plus a list of all tables")
    args = ap.parse_args()
    {"probe": cmd_probe, "tables": cmd_tables}[args.cmd](args)


if __name__ == "__main__":
    main()
