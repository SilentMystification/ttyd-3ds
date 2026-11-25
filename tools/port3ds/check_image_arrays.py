#!/usr/bin/env python3
"""Fail the build when game image data is compiled into the port's executable.

    check_image_arrays.py <nm> <ttyd.elf>

Ports ship no game data (docs/RELEASE.md, docs/PORT_3DS_PLAN.md 6.1). The decomp's image arrays (textures, palettes,
display lists written as arrays) are declared without contents under TARGET_PORT and filled at boot from the player's
disc image (include/port_disc_data.h, tools/port3ds/gen_disc_data.py), so their symbols sit in .bss. This reads the
ELF's symbol table and fails for a symbol that is initialised data (.data or .rodata) and
  - is one of the names in KNOWN_IMAGE_ARRAYS, or
  - has an image-like name (texture, tex, tlut, banner, cubeDN) and is DATA_MIN bytes or more, or
  - has a geometry name (vertex arrays, display lists: _vtx, _v, vertex_tbl, _dl_<n>, _dl) and is GEOMETRY_MIN
    bytes or more. Pointer tables (diamond_7_dl_1_tbl) end in _tbl and do not match.

ALLOWED is the list of image arrays that may stay compiled in: (symbol, size). It is empty and stays empty; a new
image array gets a PORT_DISC_DATA entry instead (gen_disc_data.py).
"""

import re
import subprocess
import sys

ALLOWED = []  # (symbol, size): none

# Arrays that used to be compiled in; each is filled from the disc now.
KNOWN_IMAGE_ARRAYS = ["texture_0", "torch_obj2_texture", "torch_obj22t_texture", "cubeDN", "_mariost_icon_tex",
                      "_mariost_icon_tlut", "_mariost_banner_tex", "ach_01_v", "Ncube_inazuma_v", "but_02_v"]
IMAGE_NAME = re.compile(r"(texture|_tex$|_tex_|tex_\d|tlut|banner|cubeDN)", re.I)
DATA_MIN = 256
GEOMETRY_NAME = re.compile(r"(_vtx\d*|_v|vertex_tbl|_dl|_dl_(\d+_)*\d+)$|^vtx\d*$|^(cube|cylinder)_dl_")
GEOMETRY_MIN = 64


def main():
    nm, elf = sys.argv[1], sys.argv[2]
    out = subprocess.run([nm, "-S", "--defined-only", elf], capture_output=True, text=True, check=True).stdout
    bad = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) != 4:
            continue
        _, size, kind, name = parts
        size = int(size, 16)
        if kind not in "dDrR" or (name, size) in ALLOWED:
            continue
        # a static array's name carries a ".<n>" suffix when the compiler made it local
        base = name.split(".")[0]
        if (base in KNOWN_IMAGE_ARRAYS or (size >= DATA_MIN and IMAGE_NAME.search(base))
                or (size >= GEOMETRY_MIN and GEOMETRY_NAME.search(base))):
            bad.append((name, size, kind))
    if bad:
        sys.exit("error: image arrays and model geometry compiled into the executable (docs/RELEASE.md): "
                 + ", ".join(f"{n} ({s} bytes, {k})" for n, s, k in bad)
                 + "\nDeclare them without contents under TARGET_PORT and register them with PORT_DISC_DATA "
                 "(include/port_disc_data.h, tools/port3ds/gen_disc_data.py ENTRIES, geometry: "
                 "tools/port3ds/disc_geometry.txt).")
    print(f"check_image_arrays: no image arrays compiled in ({len(ALLOWED)} allowed)")


if __name__ == "__main__":
    main()
