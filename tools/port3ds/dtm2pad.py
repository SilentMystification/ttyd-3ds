#!/usr/bin/env python3
"""Convert a Dolphin DTM movie to a TPAD file (switch padfile=), or dump a TPAD file.

    tools/port3ds/dtm2pad.py dtm2pad MOVIE.dtm OUT.tpad
    tools/port3ds/dtm2pad.py pad2txt FILE.tpad          # one line per frame
    tools/port3ds/dtm2pad.py selftest                   # synthetic DTM round trip

TPAD (port3ds/plat/input.c), little-endian:
  header, 16 bytes: magic "TPAD", version u32 (1), frame count u32, flags u32
                    (bit0 = recorded under determ)
  per frame, 8 bytes: buttons u16, stickX s8, stickY s8, substickX s8,
                      substickY s8, triggerL u8, triggerR u8
Buttons are the GameCube PADStatus bits (PAD_BUTTON_LEFT 0x0001, RIGHT 0x0002,
DOWN 0x0004, UP 0x0008, Z 0x0010, R 0x0020, L 0x0040, A 0x0100, B 0x0200,
X 0x0400, Y 0x0800, START 0x1000).

DTM layout, from Dolphin's Source/Core/Core/Movie.h (struct DTMHeader, packed,
256 bytes, and struct ControllerState, 8 bytes), checked against the master
branch on 2026-11-22:
  0x00 filetype "DTM\\x1A"          0x04 gameID[6]          0x0A bWii (bool)
  0x0B controllers (bits 0-3 = GC ports 1-4, bits 4-7 = Wiimotes 1-4)
  0x0C bFromSaveState             0x0D frameCount u64
  0x15 inputCount u64 (the number of input frames)  ... header ends at 0x100
  then, per input frame, 8 bytes for every enabled GC controller in port order.
ControllerState bytes: byte 0 bit fields Start(bit 0) A B X Y Z DPadUp DPadDown;
byte 1 bit fields DPadLeft DPadRight L R disc reset is_connected get_origin;
then TriggerL, TriggerR, AnalogStickX, AnalogStickY, CStickX, CStickY as u8
(sticks 0..255, 128 centred). Only controller 1 is used here; a Wii movie, or
a Wiimote bit in the controller mask, is refused (its frames have another size).
"""

import struct
import sys
from pathlib import Path

MAGIC = b"TPAD"
VERSION = 1
HEADER = 16
REC = 8
DTM_HEADER = 256

# DTM byte 0 bits, byte 1 bits -> PADStatus button bits
DTM_BUTTONS = [
    (0, 0, 0x1000),  # Start
    (0, 1, 0x0100),  # A
    (0, 2, 0x0200),  # B
    (0, 3, 0x0400),  # X
    (0, 4, 0x0800),  # Y
    (0, 5, 0x0010),  # Z
    (0, 6, 0x0008),  # Up
    (0, 7, 0x0004),  # Down
    (1, 0, 0x0001),  # Left
    (1, 1, 0x0002),  # Right
    (1, 2, 0x0040),  # L
    (1, 3, 0x0020),  # R
]
BUTTON_NAMES = [(0x1000, "Start"), (0x0100, "A"), (0x0200, "B"), (0x0400, "X"), (0x0800, "Y"), (0x0010, "Z"),
                (0x0008, "Up"), (0x0004, "Down"), (0x0001, "Left"), (0x0002, "Right"), (0x0040, "L"), (0x0020, "R")]


def die(msg):
    print(f"dtm2pad: {msg}", file=sys.stderr)
    sys.exit(1)


def s8(v):
    return v - 256 if v > 127 else v


def convert_frame(rec):
    """One 8-byte DTM ControllerState -> one 8-byte TPAD record."""
    buttons = 0
    for byte, bit, mask in DTM_BUTTONS:
        if rec[byte] >> bit & 1:
            buttons |= mask
    tl, tr, sx, sy, cx, cy = rec[2:8]
    return struct.pack("<HbbbbBB", buttons, s8((sx - 128) & 255), s8((sy - 128) & 255),
                       s8((cx - 128) & 255), s8((cy - 128) & 255), tl, tr)


def dtm_to_tpad(data):
    if len(data) < DTM_HEADER or data[:4] != b"DTM\x1a":
        die("not a DTM file (magic DTM 1A missing)")
    is_wii = data[0x0A]
    controllers = data[0x0B]
    if is_wii or controllers & 0xF0:
        die("a Wii movie or one with Wiimotes: only GameCube movies are supported")
    if not controllers & 1:
        die("controller 1 is not enabled in this movie")
    stride = REC * bin(controllers & 0x0F).count("1")
    (input_count,) = struct.unpack_from("<Q", data, 0x15)
    avail = (len(data) - DTM_HEADER) // stride
    if input_count > avail:
        print(f"dtm2pad: header says {input_count} input frames, the file holds {avail}; using {avail}",
              file=sys.stderr)
        input_count = avail
    out = bytearray(struct.pack("<4sIII", MAGIC, VERSION, input_count, 0))
    for i in range(input_count):
        off = DTM_HEADER + i * stride
        out += convert_frame(data[off:off + REC])  # controller 1 comes first
    return bytes(out)


def parse_tpad(data):
    if len(data) < HEADER or data[:4] != MAGIC:
        die("not a TPAD file")
    _, version, count, flags = struct.unpack_from("<4sIII", data)
    if version != VERSION:
        die(f"TPAD version {version}, expected {VERSION}")
    count = min(count, (len(data) - HEADER) // REC)
    return flags, [struct.unpack_from("<HbbbbBB", data, HEADER + i * REC) for i in range(count)]


def pad2txt(data, out=sys.stdout):
    flags, recs = parse_tpad(data)
    print(f"TPAD v{VERSION} frames={len(recs)} flags={flags:#x}{' (determ)' if flags & 1 else ''}", file=out)
    for i, (b, sx, sy, cx, cy, tl, tr) in enumerate(recs):
        names = "+".join(n for m, n in BUTTON_NAMES if b & m) or "-"
        print(f"{i:6d} btn={b:04x} {names:<12} stick={sx},{sy} cstick={cx},{cy} trig={tl},{tr}", file=out)


def selftest():
    """A synthetic DTM with two controllers enabled -> TPAD -> text, compared with what was put in."""
    import io
    frames = [  # (byte0, byte1, TL, TR, X, Y, CX, CY) for controller 1
        (0x00, 0x00, 0, 0, 128, 128, 128, 128),
        (0x02, 0x00, 0, 0, 128, 128, 128, 128),  # A
        (0x01 | 0x04, 0x04, 0, 255, 255, 1, 128, 128),  # Start+B+L
        (0xC0, 0x03, 0, 0, 0, 255, 200, 56),  # Up+Down+Left+Right
        (0x28, 0x08, 255, 255, 128, 128, 128, 128),  # X+Z+R
    ]
    hdr = bytearray(DTM_HEADER)
    hdr[:4] = b"DTM\x1a"
    hdr[0x0B] = 0b0011  # controllers 1 and 2
    struct.pack_into("<Q", hdr, 0x15, len(frames))
    body = b""
    for f in frames:
        body += bytes((f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]))
        body += bytes(range(8))  # controller 2's data must be skipped
    tpad = dtm_to_tpad(bytes(hdr) + body)
    flags, recs = parse_tpad(tpad)
    assert flags == 0 and len(recs) == len(frames) and len(tpad) == HEADER + REC * len(frames)
    expect = [
        (0x0000, 0, 0, 0, 0, 0, 0),
        (0x0100, 0, 0, 0, 0, 0, 0),
        (0x1000 | 0x0200 | 0x0040, 127, -127, 0, 0, 0, 255),
        (0x0008 | 0x0004 | 0x0001 | 0x0002, -128, 127, 72, -72, 0, 0),
        (0x0400 | 0x0010 | 0x0020, 0, 0, 0, 0, 255, 255),
    ]
    # byte0 of frame 3 above is 0xC0 (Up+Down); byte1 0x03 (Left+Right)
    assert recs == expect, f"{recs}\n{expect}"
    buf = io.StringIO()
    pad2txt(tpad, buf)
    print(buf.getvalue(), end="")
    print("selftest: ok")


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "dtm2pad":
        out = dtm_to_tpad(Path(a[1]).read_bytes())
        Path(a[2]).write_bytes(out)
        print(f"dtm2pad: {(len(out) - HEADER) // REC} frames -> {a[2]}")
    elif len(a) == 2 and a[0] == "pad2txt":
        pad2txt(Path(a[1]).read_bytes())
    elif a == ["selftest"]:
        selftest()
    else:
        print(__doc__.split("\n\n")[0], file=sys.stderr)
        sys.exit(2)


if __name__ == "__main__":
    main()
