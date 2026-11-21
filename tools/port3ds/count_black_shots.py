#!/usr/bin/env python3
"""Usage: count_black_shots.py DIR [--from N]

List all-black (single-colour) P6 .ppm shots in a directory and count them."""
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
    args = sys.argv[1:]
    first = 0  # --from N: skip shots numbered below N (boot, logo and title fades)
    if "--from" in args:
        i = args.index("--from")
        first = int(args[i + 1])
        del args[i:i + 2]
    d = args[0] if args else "."
    names = sorted(n for n in os.listdir(d) if n.endswith(".ppm") and int("".join(c for c in n if c.isdigit())) >= first)
    bad = 0
    for n in names:
        p = pixels(os.path.join(d, n))
        if not p or p[:3] * (len(p) // 3) == p:
            bad += 1
            print("black/single-colour:", n, "(first pixel %s)" % p[:3].hex())
    print("%d of %d shots black" % (bad, len(names)))


if __name__ == "__main__":
    main()
