#!/usr/bin/env python3
"""Generate the port's Shift-JIS ROM font (what OSInitFont/OSLoadFont serve for OS_FONT_ENCODE_SJIS).

    gen_sjis_font.py [--out build/fonts/sjis_font.bin] [--font FILE.otf] [--preview PNG]

The GameCube's IPL font is Nintendo's and is neither on the disc nor
redistributable. This renders the same glyph set from Noto Sans CJK JP
(SIL Open Font License 1.1, port3ds/third_party/noto_sans_cjk/LICENSE),
downloaded once into build/fonts/ (pinned URL, SHA-256 checked, untracked),
in the layout the SDK's OSFont.c reads:

  - 24x24 cells, 21 columns x 21 rows per 512x512 sheet, 9 sheets (3969 cells,
    glyph indices 0..3760 are used), so OSGetFontTexel / OSGetFontTexture do the
    same arithmetic as on the console;
  - glyph index order = the SDK's GetFontCode: Zenkaku2Code (JIS rows 0x81..0x87,
    read from libs/dolsdk2004/src/os/OSFont.c), 0x20C.. for the half-width
    0x20..0x7E and 0xA1..0xDF, 0x2BE + (lead-0x88)*188 + (trail-0x40) for the
    level 1 kanji 0x889F..0x9872;
  - 2 bits per pixel (4 grey levels, the c0..c3 = 0, 0x55, 0xAA, 0xFF colour
    table), 8x8 tiles of 16 bytes, as OSLoadFont's sheet; OSInitFont expands it
    to I4 at run time (port3ds/game/os_font.c).

Output (little endian), embedded by port3ds/CMakeLists.txt as sjis_font_bin (an embedded binary of 600 KB made the
game stop at boot in Azahar, aptWaitForWakeUp, so the sheets are stored deflated, 40% of their size):
  0   "SJF2"
  4   u32 header size H (the OSFontHeader and the width table, 0xEE4)
  8   u32 sheet bytes S (2 bits per pixel, 0x90000)
  12  u32 Zenkaku2Code entry count Z
  16  u32 zlib stream size P
  20  Z x u16 Zenkaku2Code
  ..  P bytes: the zlib stream of the header + widths (H bytes) and the sheet (S bytes), the OSLoadFont layout
  The file is padded with zeros to 32 bytes.
The output is deterministic.
"""

import argparse
import hashlib
import re
import struct
import sys
import urllib.request
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FONT_URL = "https://github.com/notofonts/noto-cjk/raw/Sans2.004/Sans/OTF/Japanese/NotoSansCJKjp-Regular.otf"
FONT_SHA256 = "68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5"
FONT_FILE = REPO / "build/fonts/NotoSansCJKjp-Regular.otf"
OUT = REPO / "build/fonts/sjis_font.bin"
OSFONT_C = REPO / "libs/dolsdk2004/src/os/OSFont.c"

CELL = 24
SHEET = 512
COLS = SHEET // CELL  # 21
ROWS = SHEET // CELL  # 21
SHEETS = 9
CELLS = COLS * ROWS * SHEETS  # 3969
WIDTH_ENTRIES = 3764  # 0xEE4 - 48: the 2-bit font is 0x90EE4 bytes, as romfont.c allocates
HEADER_BYTES = 48 + WIDTH_ENTRIES  # 0xEE4
SHEET_BYTES = SHEETS * SHEET * SHEET // 4  # 0x90000
RENDER_PX = 22
BASELINE = 19


def fetch_font(path):
    if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == FONT_SHA256:
        return
    print(f"gen_sjis_font: downloading {FONT_URL}", file=sys.stderr)
    path.parent.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(FONT_URL, timeout=300) as r:
        data = r.read()
    if hashlib.sha256(data).hexdigest() != FONT_SHA256:
        sys.exit("gen_sjis_font: the downloaded font does not match its pinned SHA-256")
    path.write_bytes(data)


def zenkaku_table():
    text = OSFONT_C.read_text()
    m = re.search(r"Zenkaku2Code\[\]\s*=\s*\{(.*?)\};", text, re.S)
    return [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]


def sjis_char(code):
    """Unicode text of a Shift-JIS code (single byte uses JIS X 0201), or None."""
    if code < 0x100:
        if 0x20 <= code <= 0x7E:
            return {0x5C: "¥", 0x7E: "‾"}.get(code, chr(code))
        if 0xA1 <= code <= 0xDF:
            return chr(0xFF61 + code - 0xA1)
        return None
    try:
        return bytes([code >> 8, code & 0xFF]).decode("cp932")
    except UnicodeDecodeError:
        return None


def glyph_map(zen):
    """glyph index -> SJIS code, in the order of OSFont.c's GetFontCode."""
    g = {}
    for i, idx in enumerate(zen):
        if idx == 0 and i != 0:
            continue
        lead = 0x81 + i // 188
        j = i % 188
        trail = j + 0x40 + (1 if j >= 0x3F else 0)
        g[idx] = lead << 8 | trail
    # the 0x85xx row of the table aliases 0x20D.. to the half-width glyphs (and 30 extras after them): the half-width
    # codes name those glyphs, so they go last
    for c in range(0x20, 0x7F):
        g[0x20C + c - 0x20] = c
    for c in range(0xA1, 0xE0):
        g[0x26B + c - 0xA1] = c
    for lead in range(0x88, 0x99):
        for j in range(188):
            trail = j + 0x40 + (1 if j >= 0x3F else 0)
            code = lead << 8 | trail
            if 0x889F <= code <= 0x9872:
                g[0x2BE + (lead - 0x88) * 188 + j] = code
    return g


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=OUT)
    ap.add_argument("--font", type=Path, default=FONT_FILE)
    ap.add_argument("--preview", type=Path, help="write the sheets as a PNG contact sheet")
    args = ap.parse_args()

    from PIL import Image, ImageDraw, ImageFont

    fetch_font(args.font)
    font = ImageFont.truetype(str(args.font), RENDER_PX)
    zen = zenkaku_table()
    gmap = glyph_map(zen)
    top = max(gmap)
    assert top < WIDTH_ENTRIES and top < CELLS, top

    sheet_img = [Image.new("L", (SHEET, SHEET), 0) for _ in range(SHEETS)]
    widths = bytearray(WIDTH_ENTRIES)
    drawn = 0
    for g in range(WIDTH_ENTRIES):
        code = gmap.get(g)
        ch = sjis_char(code) if code is not None else None
        half = code is not None and code < 0x100
        if ch is None:
            widths[g] = 12 if g >= 0x20C else CELL
            continue
        adv = font.getlength(ch)
        widths[g] = max(4, min(CELL, round(adv) + (1 if half else 2))) if half else CELL
        if ch == " ":
            continue
        sheet, n = divmod(g, COLS * ROWS)
        row, col = divmod(n, COLS)
        cell = Image.new("L", (CELL, CELL), 0)
        d = ImageDraw.Draw(cell)
        bbox = font.getbbox(ch, anchor="ls")
        if half:
            x = 1 - min(0, bbox[0])
        else:  # centre in the full-width cell
            x = (CELL - (bbox[2] - bbox[0])) // 2 - bbox[0]
        d.text((x, BASELINE), ch, fill=255, font=font, anchor="ls")
        sheet_img[sheet].paste(cell, (col * CELL, row * CELL))
        drawn += 1

    sheet = bytearray(SHEET_BYTES)
    for s, img in enumerate(sheet_img):
        px = img.load()
        base = s * SHEET * SHEET // 4
        for y in range(SHEET):
            for x in range(SHEET):
                v = (px[x, y] * 3 + 127) // 255
                if v:
                    p = base + (SHEET // 8) * 16 * (y // 8) + (x // 8) * 16 + (y % 8) * 2 + (x % 8) // 4
                    sheet[p] |= v << (6 - (x % 4) * 2)

    # OSFontHeader, native little endian on the port
    h = bytearray(48)
    struct.pack_into("<HHHH", h, 0, 1, 0, top, 0x20C)  # fontType 1 (SJIS), firstChar, lastChar, invalChar
    struct.pack_into("<HHHH", h, 8, 20, 4, CELL, 2)  # ascent, descent, width, leading
    struct.pack_into("<HH", h, 16, CELL, CELL)  # cellWidth, cellHeight
    struct.pack_into("<I", h, 20, SHEET * SHEET // 2)  # sheetSize (I4 bytes of one sheet)
    struct.pack_into("<HHHHHH", h, 24, 4, COLS, ROWS, SHEET, SHEET, 48)  # I4, column, row, w, h, widthTable
    struct.pack_into("<II", h, 36, HEADER_BYTES, SHEETS * SHEET * SHEET // 2)  # sheetImage, sheetFullSize
    h[44:48] = bytes([0x00, 0x55, 0xAA, 0xFF])
    assert len(h) + len(widths) == HEADER_BYTES
    packed = zlib.compress(bytes(h) + bytes(widths) + bytes(sheet), 9)
    blob = b"SJF2" + struct.pack("<IIII", HEADER_BYTES, SHEET_BYTES, len(zen), len(packed))
    blob += b"".join(struct.pack("<H", z) for z in zen) + packed
    blob += b"\0" * (-len(blob) % 32)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f"gen_sjis_font: {drawn} glyphs ({top + 1} indices), {len(blob)} bytes -> {args.out}")

    if args.preview:
        sheet0 = Image.new("L", (SHEET * 3, SHEET * 3), 0)
        for i, img in enumerate(sheet_img):
            sheet0.paste(img, ((i % 3) * SHEET, (i // 3) * SHEET))
        sheet0.point(lambda v: 255 - v).save(args.preview)


if __name__ == "__main__":
    main()
