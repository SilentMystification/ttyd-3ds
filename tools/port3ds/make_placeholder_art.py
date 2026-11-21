#!/usr/bin/env python3
"""Generate the placeholder CIA banner image, icon and banner sound.

The art is original and contains no game assets: two lines of block text on
a plain background, and a short two-note sine chime. Usage:

    make_placeholder_art.py --title "TTYD 3DS" --subtitle "PORT" --out-dir DIR

writes DIR/banner.png (256x128), DIR/icon.png (48x48) and DIR/banner.wav.
Only the standard library is used, so it runs on every build host.
"""

import argparse
import math
import struct
import wave
import zlib
from pathlib import Path

# 5x7 bitmap font for the characters the titles use.
FONT = {
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    " ": ["00000"] * 7,
}

BACKGROUND = (0x2B, 0x1E, 0x4A)
FOREGROUND = (0xF2, 0xD0, 0x5C)
SUBTITLE = (0xE8, 0xE8, 0xF0)


def text_width(text, scale):
    return len(text) * 6 * scale - scale


def draw_text(pixels, width, height, text, scale, y, color):
    x0 = (width - text_width(text, scale)) // 2
    for i, ch in enumerate(text.upper()):
        glyph = FONT.get(ch)
        if glyph is None:
            raise SystemExit(f"make_placeholder_art: no glyph for {ch!r}")
        for gy, row in enumerate(glyph):
            for gx, bit in enumerate(row):
                if bit != "1":
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        px = x0 + (i * 6 + gx) * scale + sx
                        py = y + gy * scale + sy
                        if 0 <= px < width and 0 <= py < height:
                            pixels[py][px] = color


def fit_scale(text, width, max_scale):
    scale = max_scale
    while scale > 1 and text_width(text, scale) > width - 4:
        scale -= 1
    return scale


def make_image(width, height, title, subtitle):
    pixels = [[BACKGROUND] * width for _ in range(height)]
    t_scale = fit_scale(title, width, max(1, height // 16))
    s_scale = fit_scale(subtitle, width, max(1, t_scale // 2))
    total = 7 * t_scale + (3 * s_scale + 7 * s_scale if subtitle else 0)
    y = (height - total) // 2
    draw_text(pixels, width, height, title, t_scale, y, FOREGROUND)
    if subtitle:
        draw_text(pixels, width, height, subtitle, s_scale, y + 7 * t_scale + 3 * s_scale, SUBTITLE)
    return pixels


def write_png(path, pixels):
    height = len(pixels)
    width = len(pixels[0])
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.write_bytes(png)


def write_chime(path):
    rate = 32000
    notes = [(659.25, 0.18), (987.77, 0.42)]
    frames = bytearray()
    for freq, dur in notes:
        n = int(rate * dur)
        for i in range(n):
            env = math.exp(-4.0 * i / n)
            s = int(9000 * env * math.sin(2 * math.pi * freq * i / rate))
            frames += struct.pack("<hh", s, s)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--title", required=True)
    ap.add_argument("--subtitle", default="")
    ap.add_argument("--out-dir", required=True, type=Path)
    args = ap.parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    write_png(args.out_dir / "banner.png", make_image(256, 128, args.title, args.subtitle))
    write_png(args.out_dir / "icon.png", make_image(48, 48, args.title.split()[0], ""))
    write_chime(args.out_dir / "banner.wav")


if __name__ == "__main__":
    main()
