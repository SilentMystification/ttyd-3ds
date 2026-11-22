#!/usr/bin/env python3
"""Builds a THP 1.1 movie for testing the port's THP decoder.

No THP file ships on the US disc, so this makes one: ffmpeg encodes baseline
4:2:0 JPEG frames of a synthetic picture, the 0xFF00 byte stuffing is removed
(THP stores the entropy data unstuffed), and a stereo 4-bit ADPCM track is
encoded here with the SDK's decoder arithmetic. ffmpeg's own THP demuxer and
decoders (video: mjpeg, audio: adpcm_thp) are then an independent reference.

  make_thp.py OUT.thp [--size WxH] [--frames N] [--fps F] [--source SRC]
"""
import argparse
import math
import os
import random
import struct
import subprocess
import tempfile

COEFS = [(0, 0), (2048, 0), (0, 2048), (1024, 1024), (4096, -2048), (3584, -1536), (3072, -1024), (4608, -2560)]


def unstuff(jpeg: bytes) -> bytes:
    """Removes FF00 stuffing from the scan data (everything after SOS's header)."""
    i = 2
    while True:
        assert jpeg[i] == 0xFF
        m = jpeg[i + 1]
        n = struct.unpack(">H", jpeg[i + 2 : i + 4])[0]
        i += 2 + n
        if m == 0xDA:
            break
    head, scan = jpeg[:i], jpeg[i:]
    return head + scan.replace(b"\xff\x00", b"\xff")


def adpcm_encode(samples, coefs):
    """Greedy 14-sample frames: per frame the predictor and scale with the least error."""
    out = bytearray()
    yn1 = yn2 = 0
    for base in range(0, len(samples), 14):
        blk = samples[base : base + 14]
        blk = blk + [0] * (14 - len(blk))
        best = None
        for p, (c0, c1) in enumerate(coefs):
            for sc in range(0, 12):
                a1, a2, err, nibs = yn1, yn2, 0, []
                for s in blk:
                    pred = c1 * a2 + c0 * a1
                    # solve for nibble: acc = pred + nib * 2^sc * 2048, out = (acc*32 + 0x8000) >> 16
                    base_out = pred * 32
                    step = (1 << sc) * 2048 * 32
                    nib = round(((s << 16) - base_out) / step)
                    nib = max(-8, min(7, nib))
                    acc = base_out + nib * step
                    o = max(-32768, min(32767, (acc + 0x8000) >> 16))
                    err += (o - s) ** 2
                    nibs.append(nib & 15)
                    a2, a1 = a1, o
                if best is None or err < best[0]:
                    best = (err, p, sc, nibs, a1, a2)
        _, p, sc, nibs, yn1, yn2 = best
        out.append(p << 4 | sc)
        for k in range(0, 14, 2):
            out.append(nibs[k] << 4 | nibs[k + 1])
    return bytes(out)


def audio_frame(left, right, coefs):
    ld = adpcm_encode(left, coefs)
    rd = adpcm_encode(right, coefs)
    cf = b"".join(struct.pack(">hh", *c) for c in coefs)
    hdr = struct.pack(">II", len(ld), len(left)) + cf + cf + struct.pack(">hhhh", 0, 0, 0, 0)
    assert len(hdr) == 80
    return hdr + ld + rd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--size", default="640x480")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--source", default="testsrc2")
    ap.add_argument("--quality", type=int, default=3)
    ap.add_argument("--rate", type=int, default=32000)
    ap.add_argument("--noise", type=int, default=6, help="temporal noise strength (0: none; a smaller, more usual frame)")
    ap.add_argument("--no-audio", action="store_true")
    a = ap.parse_args()
    w, h = map(int, a.size.split("x"))
    with tempfile.TemporaryDirectory() as td:
        src = a.source
        if "=" not in src:
            src = f"{src}=s={w}x{h}:r={a.fps}"
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", src, "-vf", (f"noise=alls={a.noise}:allf=t," if a.noise else "") + "format=yuvj420p",
                        "-c:v", "mjpeg", "-q:v", str(a.quality), "-frames:v", str(a.frames),
                        os.path.join(td, "f%04d.jpg")], check=True)
        frames = []
        for i in range(a.frames):
            with open(os.path.join(td, "f%04d.jpg" % (i + 1)), "rb") as f:
                frames.append(unstuff(f.read()))
    spf = int(round(a.rate / a.fps / 14)) * 14  # whole ADPCM frames, as ffmpeg's adpcm_thp requires
    random.seed(1)
    n = spf * a.frames
    left = [int(9000 * math.sin(2 * math.pi * 440 * t / a.rate) + 3000 * math.sin(2 * math.pi * 97 * t / a.rate)
                + random.randint(-300, 300)) for t in range(n)]
    right = [int(8000 * math.sin(2 * math.pi * 660 * t / a.rate) + random.randint(-300, 300)) for t in range(n)]
    ncomp = 1 if a.no_audio else 2
    comps = []
    for i in range(a.frames):
        v = frames[i]
        v += b"\0" * (-len(v) % 4)
        parts = [v]
        if not a.no_audio:
            af = audio_frame(left[i * spf : (i + 1) * spf], right[i * spf : (i + 1) * spf], COEFS)
            af += b"\0" * (-len(af) % 4)
            parts.append(af)
        comps.append(parts)
    # frame: nextFrameSize, prevFrameSize, comp sizes, data (padded to 32 bytes like the real files)
    raw = []
    for i, parts in enumerate(comps):
        body = b"".join(parts)
        hdr_len = 8 + 4 * ncomp
        total = hdr_len + len(body)
        total += -total % 32
        raw.append([total, parts])
    out_frames = []
    for i, (total, parts) in enumerate(raw):
        nxt = raw[i + 1][0] if i + 1 < len(raw) else raw[0][0]
        prev = raw[i - 1][0] if i > 0 else raw[-1][0]
        body = struct.pack(">II", nxt, prev) + b"".join(struct.pack(">I", len(p)) for p in parts) + b"".join(parts)
        body += b"\0" * (total - len(body))
        out_frames.append(body)
    comp_off = 0x30
    comp = struct.pack(">I", ncomp) + bytes([0, 1][:ncomp]) + b"\xff" * (16 - ncomp)
    comp += struct.pack(">III", w, h, 1)
    if not a.no_audio:
        comp += struct.pack(">IIII", 2, a.rate, n, 1)
    offs_off = comp_off + len(comp)
    offs = struct.pack(">I", 0) * 0
    data_off = offs_off + len(offs)
    data_off += -data_off % 32
    movie = b"".join(out_frames)
    sizes_max = max(len(f) for f in out_frames)
    first_off = data_off
    last_off = data_off + len(movie) - len(out_frames[-1])
    hdr = b"THP\0" + struct.pack(">II", 0x11000, sizes_max) + struct.pack(">I", spf if not a.no_audio else 0)
    hdr += struct.pack(">f", a.fps) + struct.pack(">III", a.frames, len(out_frames[0]), len(movie))
    hdr += struct.pack(">IIII", comp_off, offs_off, first_off, last_off)
    assert len(hdr) == 0x30
    blob = hdr + comp
    blob += b"\0" * (data_off - len(blob))
    blob += movie
    with open(a.out, "wb") as f:
        f.write(blob)


if __name__ == "__main__":
    main()
