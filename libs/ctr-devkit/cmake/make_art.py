#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
"""Regenerate the example apps' placeholder art in cmake/art/.

    python3 libs/ctr-devkit/cmake/make_art.py

Writes icon.png (48x48), banner.png (256x128) and banner.wav (a short
two-note chime). Plain shapes only. The outputs are checked in, so building
needs only devkitPro; run this only to change the art. Standard library only.
"""
import math
import struct
import wave
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent / "art"
BG = (0x1C, 0x3A, 0x4A)
FG = (0x6F, 0xD3, 0xC4)
ACCENT = (0xF5, 0xB9, 0x42)


def png(path, w, h, pixel):
    rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(w)) for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")
    path.write_bytes(data)


def icon(x, y):
    # A ring with a dot: a "target" mark.
    d = math.hypot(x - 23.5, y - 23.5)
    if 14 <= d <= 19:
        return FG
    if d <= 6:
        return ACCENT
    return BG


def banner(x, y):
    # Diagonal stripes framing a band.
    if 48 <= y < 80:
        return FG if (x // 8) % 2 == 0 and 56 <= y < 72 else BG
    return ACCENT if ((x + y) // 12) % 2 == 0 else BG


def chime(path):
    rate = 22050
    frames = bytearray()
    for i in range(int(rate * 0.6)):
        t = i / rate
        f = 880.0 if t < 0.25 else 1318.5
        amp = 0.3 * math.exp(-4 * (t % 0.25 if t < 0.25 else t - 0.25))
        frames += struct.pack("<h", int(32767 * amp * math.sin(2 * math.pi * f * t)))
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def main():
    OUT.mkdir(exist_ok=True)
    png(OUT / "icon.png", 48, 48, icon)
    png(OUT / "banner.png", 256, 128, banner)
    chime(OUT / "banner.wav")
    print(f"wrote {OUT}/icon.png, banner.png, banner.wav")


if __name__ == "__main__":
    main()
