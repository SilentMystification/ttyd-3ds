#!/usr/bin/env python3
"""Lists the clicks audio_compare's detector finds in a recording, with their times (in the
signal after an Azahar capture's held runs are cut), and the distance of each to the nearest
cut: a click at a cut is the instrument's, not the game's.

  tools/port3ds/audio_clicks.py build/audio/port_gor_01/azahar.wav [--azahar] [--json F]
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import audio_compare as ac  # noqa: E402


def find(path, azahar):
    x, rate = ac.read_wav(path)
    x, cuts = ac.cut_holds(x, rate, zeros=azahar)
    cut_pos = []  # each cut's position in the cut signal
    removed = 0
    for s, n in cuts:
        cut_pos.append(s - removed)
        removed += n
    mono = x.mean(axis=1).astype(np.float64)
    d2 = np.abs(mono[2:] - 2 * mono[1:-1] + mono[:-2])
    w = int(0.05 * rate)
    local = np.sqrt(np.convolve(d2 ** 2, np.ones(w) / w, mode="same")) + 1e-9
    spikes = np.where((d2 > 10 * local) & (d2 > 10 ** (-30 / 20)))[0]
    clicks, last = [], -10 ** 9
    for s in spikes:
        if s - last > 0.005 * rate:
            clicks.append(int(s) + 1)
            last = s
    return rate, len(x), clicks, cut_pos, cuts


def parse_log(path):
    """[EV] lines of a game log (switch ndspevlog=1): (kind, ch, t, now, a, b), frames at 32 kHz."""
    import re
    rows = []
    for l in open(path, errors="replace"):
        m = re.search(r"\[EV\] (\w) ch(\d+) t (\d+) now (\d+) (\d+) (\d+)", l)
        if m:
            rows.append((m.group(1), int(m.group(2)), *map(int, m.groups()[2:])))
    return rows


def onset(path_wav, azahar=True):
    """Positive RMS rise in dB per 5 ms frame (of the cut signal): where something starts."""
    x, rate = ac.read_wav(path_wav)
    x, _ = ac.cut_holds(x, rate, zeros=azahar)
    m = x.mean(axis=1)
    h = int(0.005 * rate)
    n = len(m) // h
    e = 10 * np.log10(np.add.reduceat(m[:n * h] ** 2, np.arange(0, n * h, h)) / h + 1e-10)
    return np.maximum(0, e[1:] - e[:-1]), h / rate


def align(rows, wav):
    """Offset (s) of the recording against the game's output clock: the shift that puts the voice
    starts (S) on the recording's onsets."""
    o, step = onset(wav)
    st = np.array([r[2] / 32000.0 for r in rows if r[0] == "S"])
    best = (-1, 0)
    for off in np.arange(-5, 60, step):
        idx = np.round((st + off) / step).astype(int)
        idx = idx[(idx >= 1) & (idx < len(o) - 2)]
        sc = sum(o[i - 1:i + 3].max() for i in idx)
        if sc > best[0]:
            best = (sc, off)
    return best[1], best[0], len(st)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav", type=Path)
    ap.add_argument("--azahar", action="store_true")
    ap.add_argument("--json", type=Path)
    ap.add_argument("--log", type=Path, help="game log with [EV] lines: correlate the clicks with them")
    ap.add_argument("-q", action="store_true", help="summary only")
    a = ap.parse_args()
    rate, n, clicks, cut_pos, cuts = find(a.wav, a.azahar or a.wav.name == "azahar.wav")
    mins = n / rate / 60
    print(f"{a.wav}: {n / rate:.1f} s after cuts, {len(cuts)} cuts ({sum(c[1] for c in cuts) / rate:.1f} s), "
          f"{len(clicks)} clicks = {len(clicks) / mins:.1f}/min")
    near = 0
    crows = []
    for c in clicks:
        dist = min((abs(c - p) for p in cut_pos), default=10 ** 9) / rate * 1000
        near += dist < 5
        crows.append((c / rate, dist))
        if not a.q:
            print(f"  t={c / rate:8.3f} s  nearest cut {dist:8.1f} ms")
    print(f"clicks within 5 ms of a cut: {near}")
    if a.log:
        rows = parse_log(a.log)
        off, sc, ns = align(rows, a.wav)
        print(f"{len(rows)} events, {ns} voice starts; recording = game clock + {off:.3f} s (onset score {sc:.0f})")
        ev = [(r[2] / 32000.0 + off, r) for r in rows if r[0] != "N"]
        stat = {}
        print("click time | events within 25 ms (kind ch dt_ms)")
        for c, _d in crows:
            nr = [(r[0], r[1], (tt - c) * 1000) for tt, r in ev if abs(tt - c) < 0.025]
            for k, ch, dt in nr:
                stat[k] = stat.get(k, 0) + 1
            print(f"  {c:8.3f}  " + ", ".join(f"{k}{ch}{dt:+.0f}" for k, ch, dt in nr))
        print("events within 25 ms of a click, by kind:", stat)
        for k in sorted({r[0] for r in rows}):
            print(f"  {k}: {sum(1 for r in rows if r[0] == k)} total")
    if a.json:
        a.json.write_text(json.dumps({"clicks": [r[0] for r in crows], "cuts": [p / rate for p in cut_pos]}))


if __name__ == "__main__":
    main()
