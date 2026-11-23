#!/usr/bin/env python3
"""Fail the build when the executable image ends past 0x01000000.

    check_image_end.py <nm> <ttyd.elf>

PORT_IS_ADDR (port3ds/include/port_addr_impl.h) takes every word between
0x00100000 and __end__ for a pointer. Sound ids carry flag bits from
0x01000000 up (PaperSoundIdBits), so an image that reaches 0x01000000 turns
those ids into "pointers" and the game panics at boot (plat/boot.c checks the
same limit). Big port-side tables belong on the heap (game/gx/gx_bigmem.h),
not in .bss or .data.
"""

import subprocess
import sys

LIMIT = 0x01000000


def main():
    nm, elf = sys.argv[1], sys.argv[2]
    out = subprocess.run([nm, elf], capture_output=True, text=True, check=True).stdout
    end = None
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == "__end__":
            end = int(parts[0], 16)
    if end is None:
        sys.exit("check_image_end: no __end__ in " + elf)
    left = LIMIT - end
    if left < 0:
        sys.exit(f"error: image ends at {end:#010x}, limit {LIMIT:#010x} (PORT_IS_ADDR, sound-id flag bits): "
                 f"{-left} bytes over. Move large .bss/.data tables to the heap (port3ds/game/gx/gx_bigmem.h).")
    print(f"check_image_end: image ends at {end:#010x}, {left} bytes below the {LIMIT:#010x} limit")


if __name__ == "__main__":
    main()
