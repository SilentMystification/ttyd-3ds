#!/usr/bin/env python3
"""List all-black (single-colour) P6 .ppm shots in a directory and count them."""
import os
import sys


def pixels(path):
    with open(path, "rb") as f:
        data = f.read()
    # header: P6 <w> <h> <maxval> then one whitespace byte, then pixels
    pos, fields = 2, []
    while len(fields) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            pos = data.index(b"\n", pos)
            continue
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    return data[pos + 1:]


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "."
    names = sorted(n for n in os.listdir(d) if n.endswith(".ppm"))
    bad = 0
    for n in names:
        p = pixels(os.path.join(d, n))
        if not p or p[:3] * (len(p) // 3) == p:
            bad += 1
            print("black/single-colour:", n, "(first pixel %s)" % p[:3].hex())
    print("%d of %d shots black" % (bad, len(names)))


if __name__ == "__main__":
    main()
