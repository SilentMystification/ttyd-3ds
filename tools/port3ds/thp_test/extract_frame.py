#!/usr/bin/env python3
"""Cuts one video frame (the unstuffed JPEG) out of a THP file: extract_frame.py IN.thp OUT.bin [frame]"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
want = int(sys.argv[3]) if len(sys.argv) > 3 else 0
comp_off, first = struct.unpack(">I", d[0x20:0x24])[0], struct.unpack(">I", d[0x28:0x2C])[0]
ncomp = struct.unpack(">I", d[comp_off:comp_off + 4])[0]
pos, cur = first, struct.unpack(">I", d[0x18:0x1C])[0]
for i in range(want + 1):
    nxt = struct.unpack(">I", d[pos:pos + 4])[0]
    if i == want:
        size0 = struct.unpack(">I", d[pos + 8:pos + 12])[0]
        start = pos + 8 + 4 * ncomp
        open(sys.argv[2], "wb").write(d[start:start + size0])
    pos += cur
    cur = nxt
