#!/usr/bin/env python3
"""Decodes a THP with the port's decoder (thp_dump) and with ffmpeg and compares.

  compare.py IN.thp --dump ./thp_dump [--frames N] [--pal] [--png-dir DIR]

Video: per-frame PSNR and max absolute difference of the Y, U and V planes
against ffmpeg's decode (yuvj420p, no colour conversion). Audio: the sample
streams compared (max difference, count of differing samples). With --png-dir
the port decoder's first frames are written as PNG.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

import numpy as np


def ffprobe_size(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=width,height",
                          "-of", "csv=p=0", path], capture_output=True, text=True, check=True).stdout
    w, h = out.strip().split(",")[:2]
    return int(w), int(h)


def psnr(a, b):
    d = a.astype(np.int32) - b.astype(np.int32)
    mse = float((d * d).mean())
    return 99.0 if mse == 0 else 10 * np.log10(255.0 * 255.0 / mse)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("thp")
    ap.add_argument("--dump", required=True)
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--pal", action="store_true")
    ap.add_argument("--png-dir")
    a = ap.parse_args()
    w, h = ffprobe_size(a.thp)
    cw, ch = (w + 1) // 2, (h + 1) // 2
    fsz = w * h + 2 * cw * ch
    with tempfile.TemporaryDirectory() as td:
        pre = os.path.join(td, "o")
        r = subprocess.run([a.dump, a.thp, pre, str(a.frames)] + (["pal"] if a.pal else []), capture_output=True,
                           text=True)
        print(r.stderr.strip())
        if r.returncode:
            sys.exit(1)
        mine = np.fromfile(pre + ".yuv", dtype=np.uint8)
        ref_path = os.path.join(td, "ref.yuv")
        subprocess.run(["ffmpeg", "-v", "error", "-i", a.thp, "-frames:v", str(a.frames), "-f", "rawvideo", "-pix_fmt",
                        "yuvj420p", ref_path], check=True)
        ref = np.fromfile(ref_path, dtype=np.uint8)
        n = min(len(mine), len(ref)) // fsz
        print(f"video: {n} frames compared ({len(mine) // fsz} ours, {len(ref) // fsz} ffmpeg)")
        worst_psnr, worst_max = 99.0, 0
        for i in range(n):
            m, f = mine[i * fsz:(i + 1) * fsz], ref[i * fsz:(i + 1) * fsz]
            planes = [(0, w * h), (w * h, w * h + cw * ch), (w * h + cw * ch, fsz)]
            res = []
            for lo, hi in planes:
                res.append((psnr(m[lo:hi], f[lo:hi]), int(np.abs(m[lo:hi].astype(int) - f[lo:hi].astype(int)).max())))
            worst_psnr = min(worst_psnr, *(p for p, _ in res))
            worst_max = max(worst_max, *(x for _, x in res))
            if i < 5 or i == n - 1:
                print("  frame %2d  Y %.1f dB max %d | U %.1f dB max %d | V %.1f dB max %d" %
                      (i, res[0][0], res[0][1], res[1][0], res[1][1], res[2][0], res[2][1]))
        print(f"video worst: PSNR {worst_psnr:.2f} dB, max abs diff {worst_max}")

        ref_a = os.path.join(td, "ref.pcm")
        ra = subprocess.run(["ffmpeg", "-v", "error", "-i", a.thp, "-vn", "-f", "s16le", "-acodec", "pcm_s16le", ref_a],
                            capture_output=True, text=True)
        if ra.returncode == 0 and os.path.getsize(ref_a) and os.path.getsize(pre + ".pcm"):
            m = np.fromfile(pre + ".pcm", dtype=np.int16).reshape(-1, 2).astype(int)
            f = np.fromfile(ref_a, dtype=np.int16).reshape(-1, 2).astype(int)
            k = min(len(m), len(f))
            print(f"audio: {len(m)} stereo samples ours, {len(f)} ffmpeg")
            for name, mm in (("as written (R,L)", m[:k]), ("channels swapped", m[:k][:, ::-1])):
                d = np.abs(mm - f[:k])
                print(f"  {name}: max diff {d.max()}, differing samples {int((d > 0).sum())} of {d.size}")
        else:
            print("audio: none or ffmpeg could not decode it")
        if a.png_dir:
            os.makedirs(a.png_dir, exist_ok=True)
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuvj420p", "-s", f"{w}x{h}",
                            "-i", pre + ".yuv", "-frames:v", "4", os.path.join(a.png_dir, "frame%02d.png")], check=True)


if __name__ == "__main__":
    main()
