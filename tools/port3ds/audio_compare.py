#!/usr/bin/env python3
"""Audio captures of the 3DS port and paper-cut, and a tolerance-based comparison.

The port and paper-cut mix differently (resamplers, NDSP's own mix and its
32728 Hz output), so captures are never compared sample by sample. The
comparison aligns the two captures in time and measures:

  - per-channel RMS envelopes over time (20 ms frames), their correlation and level difference;
  - the long-term spectrum in 1/3-octave bands and the pitch (a semitone shift search over a
    fine log-frequency spectrum, and chroma correlation over time);
  - clicks (second-difference spikes far above the local level) and gaps (digital silence or a
    held sample inside loud audio), counted per minute: a bad loop point shows up as either.

    tools/port3ds/audio_compare.py scenes
    tools/port3ds/audio_compare.py capture-papercut --scene title
    tools/port3ds/audio_compare.py capture-port --scene gor_01 [--lle-audio] [--switches "ndspvoices=1"]
    tools/port3ds/audio_compare.py compare A.wav B.wav [--json out.json]
    tools/port3ds/audio_compare.py cpu --scene gor_01 [--switches "ndspvoices=1"]
    tools/port3ds/audio_compare.py loops | loopcheck | selftest

Captures go to build/audio/<source>_<scene>[_<tag>]/:

  capture-papercut  paper-cut's headless build (build/papercut-headless/ttyd-us, see
                    --help of that command) with $TTYD_AUDIO_WAV: pc.wav, 32 kHz, the
                    deterministic 60 Hz clock (TTYD_FAST). Runs in a second.
  capture-port      the port in Azahar through run_azahar.py (slot-locked, headless):
                      wavdump.wav  the game's software mix (switch wavdump=N), 32 kHz; with
                                   ndspvoices=1 it only holds what is still mixed in software;
                      azahar.wav   Azahar's sound output: every NDSP channel after the DSP
                                   (its SDL2 sink on SDL's disk driver, 44.1 kHz), recorded with
                                   emulation held at --speed percent (default 34) and the disk
                                   driver slowed to match. Where Azahar still falls behind it
                                   pads with held or silent runs; compare cuts and counts them.
                    --no-sink: wavdump only, at full speed.
  cpu               pc_sample.py --interval 200 on a scene under Azahar's GDB stub.
  loops             every sample's loop points on the disc (how many start inside a frame).
  loopcheck         the port's loop re-alignment code, compiled for this PC, on every loop.

Needs numpy. Exit status: 0 ok (compare: within tolerance), 1 outside tolerance or a failed run,
2 setup errors.
"""

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu_usage  # noqa: E402

try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None

REPO = Path(__file__).resolve().parents[1].parent
PAPERCUT_EXE = REPO / "build" / "papercut-headless" / "ttyd-us"
OUT = REPO / "build" / "audio"

# Scenes, the same on both sides. Port padscripts count game frames from boot (the map=
# warp happens around frame 600); paper-cut's input scripts start once the map is reached.
SCENES = {
    "title": {
        "desc": "boot to the title screen and its music",
        "port_switches": "",
        "port_seconds": 75,
        "pc_frames": 3600,
        "pc_env": {},
        "pc_input": None,
    },
    "gor_01": {
        "desc": "Rogueport square, story 30: walk right, jump twice, walk left, jump",
        "port_switches": "map=gor_01 story=30 padscript=wait*900,cr*90,a*3,wait*60,a*3,wait*60,cl*90,a*3,wait*120",
        "port_seconds": 75,
        "pc_frames": 1500,
        "pc_env": {"TTYD_MAP": "gor_01", "TTYD_STORY": "30"},
        # (an input script ends the run at its "end", not at TTYD_EXIT_FRAMES)
        "pc_input": "wait 60\nstick 127 0 90\npress a\nwait 60\npress a\nwait 60\nstick -128 0 90\npress a\nwait 600\nend\n",
    },
    "win_00": {
        "desc": "Boggly Woods entrance, story 30: an ambient loop that starts inside an ADPCM frame (re-aligned)",
        "port_switches": "map=win_00 story=30",
        "port_seconds": 60,
        "pc_frames": 2400,
        "pc_env": {"TTYD_MAP": "win_00", "TTYD_STORY": "30"},
        "pc_input": None,
    },
    "tou_03": {
        "desc": "Glitzville, story 30: sound effects with reverb sends set just after they start",
        "port_switches": "map=tou_03 story=30",
        "port_seconds": 60,
        "pc_frames": 2400,
        "pc_env": {"TTYD_MAP": "tou_03", "TTYD_STORY": "30"},
        "pc_input": None,
    },
    "battle": {
        "desc": "gon_02 at story 30, which starts a battle on entry: the battle music",
        "port_switches": "map=gon_02 story=30",
        "port_seconds": 90,
        "pc_frames": 2400,
        "pc_env": {"TTYD_MAP": "gon_02", "TTYD_STORY": "30"},
        "pc_input": None,
    },
}

# Tolerances for `compare` (B against reference A).
TOL = {
    "env_corr_min": 0.75,      # envelope correlation (dB envelopes, active frames; segment median)
    "level_db_max": 4.0,       # |median level difference|
    "band_rms_db_max": 4.0,    # spectral shape: RMS over bands of the level-normalised difference
    "pitch_shift_max": 0.25,   # semitones
    "chroma_cos_min": 0.75,    # per-frame chroma cosine similarity (segment median)
    "clicks_extra_per_min": 6.0,
    "gaps_extra_per_min": 2.0,
}


# ----------------------------------------------------------------------------- WAV I/O

def read_wav(path):
    with wave.open(str(path), "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width != 2:
        raise SystemExit(f"audio_compare: {path}: only 16-bit WAV is read")
    x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    x = x.reshape(-1, ch)
    if ch == 1:
        x = np.repeat(x, 2, axis=1)
    return x[:, :2], rate


def write_wav(path, x, rate):
    x = np.clip(np.round(np.asarray(x) * 32768.0), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(x.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(x.tobytes())


def raw_to_wav(raw_path, wav_path, rate=44100):
    data = np.fromfile(str(raw_path), dtype="<i2")
    data = data[: len(data) // 2 * 2].reshape(-1, 2).astype(np.float32) / 32768.0
    write_wav(wav_path, data, rate)
    return len(data)


# ----------------------------------------------------------------------------- analysis

FRAME_S = 0.02
BANDS = [25.0 * 2 ** (i / 3.0) for i in range(3, 30)]  # 1/3-octave centres, 31.5 Hz .. 12.7 kHz


def holds(x, rate, min_s=0.01):
    """Runs (start, length) of one stereo sample repeated >= min_s: Azahar's padding, or
    digital silence. Returned in samples."""
    same = np.all(x[1:] == x[:-1], axis=1)
    runs = []
    n = len(same)
    i = 0
    min_len = int(min_s * rate)
    # vectorised run detection
    d = np.diff(np.concatenate(([0], same.astype(np.int8), [0])))
    starts = np.where(d == 1)[0]
    ends = np.where(d == -1)[0]
    for s, e in zip(starts, ends):
        if e - s + 1 >= min_len:
            runs.append((int(s), int(e - s + 1)))
    del n, i
    return runs


def cut_holds(x, rate, loud_db=-45.0, zeros=False):
    """Removes runs of one held non-zero sample with loud audio on both sides: Azahar's
    padding when emulation fell behind real time (it repeats the last frame). Runs of exact
    zeros stay (zeros=False): those are gaps in the game's own output, which clicks_and_gaps
    counts. For an Azahar capture (zeros=True) they go too: Azahar's sink also leaves silence
    when it falls behind, and the game's own underruns are in its log ("underruns")."""
    runs = holds(x, rate)
    keep = np.ones(len(x), dtype=bool)
    cut = []
    w = int(0.03 * rate)
    for s, n in runs:
        if not zeros and not np.any(x[s]):
            continue
        before = x[max(0, s - w):s]
        after = x[s + n:s + n + w]
        lb = 20 * np.log10(np.sqrt(np.mean(before ** 2)) + 1e-9) if len(before) else -120
        la = 20 * np.log10(np.sqrt(np.mean(after ** 2)) + 1e-9) if len(after) else -120
        if min(lb, la) > loud_db:
            keep[s:s + n] = False
            cut.append((s, n))
    return x[keep], cut


def frames(x, rate):
    hop = FRAME_S * rate
    win = int(round(2 * hop))
    count = int((len(x) - win) / hop) if len(x) > win else 0
    idx = (np.arange(count) * hop).astype(np.int64)
    return idx, win


def features(x, rate):
    """Per 20 ms frame: L/R RMS (dB), band energies (dB, BANDS), chroma (12) and a fine
    log-frequency spectrum (1/24 octave, 55 Hz .. 7 kHz) for the pitch search."""
    idx, win = frames(x, rate)
    nfft = 1
    while nfft < win:
        nfft *= 2
    nfft = max(nfft, 8192)  # bins of 5 Hz or less: every 1/3-octave band from 31.5 Hz has some
    hann = np.hanning(win).astype(np.float32)
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    edges = [(c * 2 ** (-1 / 6), c * 2 ** (1 / 6)) for c in BANDS]
    band_bins = [np.where((freqs >= lo) & (freqs < hi))[0] for lo, hi in edges]
    fine_c = 55.0 * 2 ** (np.arange(0, 24 * 7) / 24.0)
    fine_c = fine_c[fine_c < min(7000.0, rate / 2 * 0.9)]
    fine_bin = np.clip(np.round(fine_c * nfft / rate).astype(int), 1, len(freqs) - 1)
    tonal = (freqs >= 60) & (freqs <= 4000)
    midi = np.zeros_like(freqs)
    midi[1:] = 69 + 12 * np.log2(np.maximum(freqs[1:], 1e-3) / 440.0)
    pc = np.mod(np.round(midi), 12).astype(int)

    rms = np.zeros((len(idx), 2), dtype=np.float32)
    bands = np.zeros((len(idx), len(BANDS)), dtype=np.float32)
    chroma = np.zeros((len(idx), 12), dtype=np.float32)
    fine = np.zeros((len(idx), len(fine_c)), dtype=np.float32)
    chunk = 512
    for c0 in range(0, len(idx), chunk):
        ii = idx[c0:c0 + chunk]
        seg = np.stack([x[i:i + win] for i in ii])  # (k, win, 2)
        rms[c0:c0 + len(ii)] = np.sqrt(np.mean(seg ** 2, axis=1))
        mono = seg.mean(axis=2) * hann
        spec = np.abs(np.fft.rfft(mono, nfft, axis=1)) ** 2
        for b, bins in enumerate(band_bins):
            if len(bins):
                bands[c0:c0 + len(ii), b] = spec[:, bins].sum(axis=1)
        fine[c0:c0 + len(ii)] = spec[:, fine_bin]
        t = spec[:, tonal]
        for k in range(12):
            chroma[c0:c0 + len(ii), k] = t[:, pc[tonal] == k].sum(axis=1)
    db = lambda v: 10 * np.log10(v + 1e-12)
    return {
        "rms_db": 20 * np.log10(rms + 1e-6),
        "bands": bands,
        "bands_db": db(bands),
        "chroma": chroma,
        "fine": fine,
        "fine_c": fine_c,
    }


def best_lag(a, b, max_lag):
    """Lag (frames) that best aligns envelope b onto a: b[t + lag] ~ a[t]."""
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    n = 1
    while n < len(a) + len(b):
        n *= 2
    # Pearson correlation over the overlap at every lag, from running sums (FFT cross terms).
    fa, fb = np.fft.rfft(a, n), np.fft.rfft(b, n)
    cross = np.fft.irfft(fb * np.conj(fa), n)
    ca, ca2 = np.concatenate(([0], np.cumsum(a))), np.concatenate(([0], np.cumsum(a * a)))
    cb, cb2 = np.concatenate(([0], np.cumsum(b))), np.concatenate(([0], np.cumsum(b * b)))
    best, best_lag_v = -2.0, 0
    need = min(len(a), len(b)) * 0.4
    for lag in range(-max_lag, max_lag + 1):
        lo = max(0, -lag)
        hi = min(len(a), len(b) - lag)
        m = hi - lo
        if m < need or m < 3:
            continue
        sa, sa2 = ca[hi] - ca[lo], ca2[hi] - ca2[lo]
        sb, sb2 = cb[hi + lag] - cb[lo + lag], cb2[hi + lag] - cb2[lo + lag]
        sab = cross[lag % n]  # zero padding: the circular term is exactly the overlap's
        va = sa2 - sa * sa / m
        vb = sb2 - sb * sb / m
        if va <= 1e-9 or vb <= 1e-9:
            continue
        r = (sab - sa * sb / m) / np.sqrt(va * vb)
        if r > best:
            best, best_lag_v = r, lag
    return int(best_lag_v), float(best)


def pearson(a, b):
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    if len(a) < 3 or a.std() < 1e-9 or b.std() < 1e-9:
        return float("nan")
    return float(np.corrcoef(a, b)[0, 1])


def clicks_and_gaps(x, rate):
    """Clicks: |second difference| more than 10x its local RMS (50 ms) and above -30 dBFS.
    Gaps: runs >= 10 ms of exact silence or one held sample with loud audio (-45 dB) on both
    sides. Both per minute of audio."""
    mono = x.mean(axis=1).astype(np.float64)
    d2 = np.abs(mono[2:] - 2 * mono[1:-1] + mono[:-2])
    w = int(0.05 * rate)
    k = np.ones(w) / w
    local = np.sqrt(np.convolve(d2 ** 2, k, mode="same")) + 1e-9
    spikes = np.where((d2 > 10 * local) & (d2 > 10 ** (-30 / 20)))[0]
    # one click per 5 ms
    clicks = 0
    last = -10 ** 9
    for s in spikes:
        if s - last > 0.005 * rate:
            clicks += 1
            last = s
    gaps = 0
    ww = int(0.03 * rate)
    for s, n in holds(x, rate):
        before = x[max(0, s - ww):s]
        after = x[s + n:s + n + ww]
        if len(before) and len(after):
            lb = 20 * np.log10(np.sqrt(np.mean(before ** 2)) + 1e-9)
            la = 20 * np.log10(np.sqrt(np.mean(after ** 2)) + 1e-9)
            if min(lb, la) > -45:
                gaps += 1
    minutes = len(x) / rate / 60.0
    return clicks / minutes if minutes else 0.0, gaps / minutes if minutes else 0.0


def compare(path_a, path_b, max_lag_s=40.0, cut=True, azahar_b=False):
    xa, ra = read_wav(path_a)
    xb, rb = read_wav(path_b)
    info = {"a": str(path_a), "b": str(path_b), "rate_a": ra, "rate_b": rb,
            "seconds_a": round(len(xa) / ra, 2), "seconds_b": round(len(xb) / rb, 2)}
    ca_raw, ga_raw = clicks_and_gaps(xa, ra)
    cb_raw, gb_raw = clicks_and_gaps(xb, rb)
    if cut:
        xa, cuta = cut_holds(xa, ra)
        xb, cutb = cut_holds(xb, rb, zeros=azahar_b)
        info["holds_cut_a_s"] = round(sum(n for _, n in cuta) / ra, 3)
        info["holds_cut_b_s"] = round(sum(n for _, n in cutb) / rb, 3)
    fa, fb = features(xa, ra), features(xb, rb)
    ea = fa["rms_db"].mean(axis=1)
    eb = fb["rms_db"].mean(axis=1)
    lag, peak = best_lag(np.maximum(ea, -80), np.maximum(eb, -80), int(max_lag_s / FRAME_S))
    info["lag_s"] = round(lag * FRAME_S, 3)
    lo = max(0, -lag)
    hi = min(len(ea), len(eb) - lag)
    sa = slice(lo, hi)
    sb = slice(lo + lag, hi + lag)
    active = (np.maximum(ea[sa], eb[sb]) > -50)
    info["overlap_s"] = round((hi - lo) * FRAME_S, 2)
    info["active_s"] = round(int(active.sum()) * FRAME_S, 2)
    res = {}
    for ch, name in ((0, "L"), (1, "R")):
        a = np.maximum(fa["rms_db"][sa, ch][active], -80)
        b = np.maximum(fb["rms_db"][sb, ch][active], -80)
        res[f"env_corr_{name}"] = round(pearson(a, b), 4)
        res[f"env_corr_lin_{name}"] = round(pearson(10 ** (a / 20), 10 ** (b / 20)), 4)
        res[f"level_db_{name}"] = round(float(np.median(b - a)), 2)
    # Segments: the two runs drift apart where game timing differs (loads, a slower frame rate
    # in the emulator), so each 4 s window of A is aligned on its own (within 8 s of the global
    # lag) and measured there; the medians over the windows are the verdict's numbers.
    win, hop, slack = int(4 / FRAME_S), int(2 / FRAME_S), int(8 / FRAME_S)
    seg = {"L": [], "R": [], "cos": [], "lag": [], "w": []}
    eam, ebm = np.maximum(ea, -80), np.maximum(eb, -80)
    for w0 in range(lo, hi - win + 1, hop):
        if (eam[w0:w0 + win] > -50).mean() < 0.5:
            continue
        b0 = max(0, w0 + lag - slack)
        b1 = min(len(eb), w0 + lag + win + slack)
        if b1 - b0 < win // 2:
            continue
        l2, r2 = best_lag(eam[w0:w0 + win], ebm[b0:b1], 2 * slack)
        off = b0 + l2  # B frame aligned with A frame w0
        m = min(win, len(eb) - off) if off >= 0 else 0
        if m < win // 2 or off < 0:
            continue
        aa, bb_ = slice(w0, w0 + m), slice(off, off + m)
        act = np.maximum(ea[aa], eb[bb_]) > -50
        if act.sum() < 20:
            continue
        for ch, name in ((0, "L"), (1, "R")):
            seg[name].append(pearson(np.maximum(fa["rms_db"][aa, ch][act], -80), np.maximum(fb["rms_db"][bb_, ch][act], -80)))
        ca_, cb_ = fa["chroma"][aa][act], fb["chroma"][bb_][act]
        na_ = ca_ / (np.linalg.norm(ca_, axis=1, keepdims=True) + 1e-12)
        nb_ = cb_ / (np.linalg.norm(cb_, axis=1, keepdims=True) + 1e-12)
        seg["cos"].append(float(np.mean(np.sum(na_ * nb_, axis=1))))
        seg["lag"].append((off - w0) * FRAME_S)
    def med(v):
        v = [x for x in v if x == x]
        return round(float(np.median(v)), 4) if v else float("nan")
    res["segments"] = len(seg["lag"])
    res["seg_env_corr_L"], res["seg_env_corr_R"] = med(seg["L"]), med(seg["R"])
    res["seg_env_corr_p10"] = round(float(np.percentile([x for x in seg["L"] + seg["R"] if x == x], 10)), 4) if seg["L"] else float("nan")
    res["seg_chroma_cos"] = med(seg["cos"])
    res["seg_lag_min_s"] = round(min(seg["lag"]), 2) if seg["lag"] else 0.0
    res["seg_lag_max_s"] = round(max(seg["lag"]), 2) if seg["lag"] else 0.0
    # balance: L-R difference per side
    bal_a =fa["rms_db"][sa, 0][active] - fa["rms_db"][sa, 1][active]
    bal_b = fb["rms_db"][sb, 0][active] - fb["rms_db"][sb, 1][active]
    res["balance_db_a"] = round(float(np.median(bal_a)), 2)
    res["balance_db_b"] = round(float(np.median(bal_b)), 2)
    # spectrum
    ba = 10 * np.log10(fa["bands"][sa][active].mean(axis=0) + 1e-12)
    bb = 10 * np.log10(fb["bands"][sb][active].mean(axis=0) + 1e-12)
    nyq = min(ra, rb) / 2 * 0.9
    valid = np.array([c < nyq for c in BANDS]) & (ba > ba.max() - 50) & (bb > bb.max() - 60)
    diff = (bb - ba)[valid]
    diff -= np.median(diff)
    res["band_rms_db"] = round(float(np.sqrt(np.mean(diff ** 2))), 2)
    res["band_max_db"] = round(float(np.max(np.abs(diff))), 2)
    res["band_worst_hz"] = round(float(np.array(BANDS)[valid][int(np.argmax(np.abs(diff)))]), 1)
    # pitch: shift of the long-term fine spectrum, in 1/24-octave steps
    la = np.log10(fa["fine"][sa][active].mean(axis=0) + 1e-12)
    lb = np.log10(fb["fine"][sb][active].mean(axis=0) + 1e-12)
    n = min(len(la), len(lb))
    la, lb = la[:n] - la[:n].mean(), lb[:n] - lb[:n].mean()
    best, best_s = -2.0, 0
    for s in range(-6, 7):
        if s >= 0:
            c = pearson(la[s:], lb[:n - s]) if s else pearson(la, lb)
        else:
            c = pearson(la[:n + s], lb[-s:])
        if c > best:
            best, best_s = c, s
    res["pitch_shift_semitones"] = -best_s / 2.0
    res["pitch_spectrum_corr"] = round(best, 4)
    cha = fa["chroma"][sa][active]
    chb = fb["chroma"][sb][active]
    res["chroma_corr"] = round(pearson(cha.mean(axis=0), chb.mean(axis=0)), 4)
    na = cha / (np.linalg.norm(cha, axis=1, keepdims=True) + 1e-12)
    nb = chb / (np.linalg.norm(chb, axis=1, keepdims=True) + 1e-12)
    tonal = (cha.sum(axis=1) > np.percentile(cha.sum(axis=1), 30)) if len(cha) else []
    res["chroma_frame_cos"] = round(float(np.mean(np.sum(na * nb, axis=1)[tonal])), 4) if len(cha) else float("nan")
    res["clicks_per_min_a"], res["gaps_per_min_a"] = round(ca_raw, 2), round(ga_raw, 2)
    res["clicks_per_min_b"], res["gaps_per_min_b"] = round(cb_raw, 2), round(gb_raw, 2)
    info.update(res)
    fails = []
    if not (min(res["seg_env_corr_L"], res["seg_env_corr_R"]) >= TOL["env_corr_min"]):
        fails.append("envelope")
    if max(abs(res["level_db_L"]), abs(res["level_db_R"])) > TOL["level_db_max"]:
        fails.append("level")
    if res["band_rms_db"] > TOL["band_rms_db_max"]:
        fails.append("spectrum")
    if abs(res["pitch_shift_semitones"]) > TOL["pitch_shift_max"]:
        fails.append("pitch")
    if not (res["seg_chroma_cos"] >= TOL["chroma_cos_min"]):
        fails.append("chroma")
    if cb_raw - ca_raw > TOL["clicks_extra_per_min"]:
        fails.append("clicks")
    if not azahar_b and gb_raw - ga_raw > TOL["gaps_extra_per_min"]:
        fails.append("gaps")
    info["fails"] = fails
    info["ok"] = not fails
    return info


def print_report(r):
    print(f"A {r['a']}  ({r['seconds_a']} s at {r['rate_a']} Hz)")
    print(f"B {r['b']}  ({r['seconds_b']} s at {r['rate_b']} Hz)")
    print(f"  aligned: B lags A by {r['lag_s']} s; overlap {r['overlap_s']} s, {r['active_s']} s above -50 dB"
          + (f"; held runs cut A {r.get('holds_cut_a_s', 0)} s, B {r.get('holds_cut_b_s', 0)} s" if 'holds_cut_a_s' in r else ""))
    print(f"  envelope corr  per 4 s segment (median of {r['segments']}): L {r['seg_env_corr_L']:.3f}  R {r['seg_env_corr_R']:.3f}, "
          f"10th percentile {r['seg_env_corr_p10']:.3f}; segment lags {r['seg_lag_min_s']}..{r['seg_lag_max_s']} s")
    print(f"                 whole run at one lag: L {r['env_corr_L']:.3f}  R {r['env_corr_R']:.3f}   (linear L {r['env_corr_lin_L']:.3f} R {r['env_corr_lin_R']:.3f})")
    print(f"  level B-A      L {r['level_db_L']:+.2f} dB  R {r['level_db_R']:+.2f} dB;  L-R balance A {r['balance_db_a']:+.2f} B {r['balance_db_b']:+.2f} dB")
    print(f"  spectrum       band RMS {r['band_rms_db']:.2f} dB, worst {r['band_max_db']:.2f} dB at {r['band_worst_hz']} Hz")
    print(f"  pitch          shift {r['pitch_shift_semitones']:+.2f} semitones (corr {r['pitch_spectrum_corr']:.3f}); "
          f"chroma profile {r['chroma_corr']:.3f}, per frame {r['seg_chroma_cos']:.3f} (segments)")
    print(f"  clicks/min     A {r['clicks_per_min_a']}  B {r['clicks_per_min_b']};  gaps/min A {r['gaps_per_min_a']}  B {r['gaps_per_min_b']}")
    print("  verdict:", "within tolerance" if r["ok"] else "OUTSIDE tolerance: " + ", ".join(r["fails"]))


# ----------------------------------------------------------------------------- captures

def capture_papercut(scene, out):
    if not PAPERCUT_EXE.is_file():
        print(f"audio_compare: {PAPERCUT_EXE.relative_to(REPO)} not found. Build paper-cut's headless backend:\n"
              "  PATH=/usr/lib/llvm-21/bin:$PATH cmake -S ../paper-cut -B build/papercut-headless -G Ninja \\\n"
              "    --toolchain ../paper-cut/cmake/toolchains/linux-clang.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \\\n"
              "    -DTTYD_VERSION=G8ME01 -DTTYD_BACKEND=headless\n"
              "  ninja -C build/papercut-headless ttyd-us")
        return 2
    sc = SCENES[scene]
    out.mkdir(parents=True, exist_ok=True)
    user = out / "user"
    if user.exists():
        shutil.rmtree(user)
    user.mkdir()
    (user / "settings.txt").write_text("skip_intro=1\nfullscreen=0\n")
    env = dict(os.environ, TTYD_FAST="1", TTYD_USER_DIR=str(user), TTYD_NO_LAUNCHER="1",
               TTYD_EXIT_FRAMES=str(sc["pc_frames"]), TTYD_WATCHDOG="60",
               TTYD_DISCORD_IPC=str(user / "no-discord"), TTYD_AUDIO_WAV=str(out / "pc.wav"))
    env.update(sc["pc_env"])
    if sc["pc_input"]:
        (out / "input.txt").write_text(sc["pc_input"])
        env["TTYD_INPUT"] = str(out / "input.txt")
    iso = REPO / "iso" / "game.iso"
    with open(out / "stdout.txt", "w") as so:
        try:
            r = subprocess.run([str(PAPERCUT_EXE), str(iso)], env=env, stdout=so, stderr=subprocess.STDOUT,
                               timeout=120, cwd=str(out))
        except subprocess.TimeoutExpired:
            print("audio_compare: paper-cut did not exit in 120 s")
            return 1
    print(f"audio_compare: paper-cut exit {r.returncode}; {out.relative_to(REPO)}/pc.wav")
    return 0 if r.returncode == 0 and (out / "pc.wav").is_file() else 1


def load_run_azahar():
    spec = importlib.util.spec_from_file_location("run_azahar", Path(__file__).with_name("run_azahar.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def capture_port(scene, out, build, extra_switches, lle_audio, timeout, seconds, speed=34, sink=True):
    ra = load_run_azahar()
    sc = SCENES[scene]
    seconds = seconds or sc["port_seconds"]
    profile = ra.PROFILE
    raw = profile / "azahar_audio.raw"
    if raw.exists():
        raw.unlink()
    if sink:
        hook_sink(ra, raw, speed)
    switches = " ".join(s for s in (sc["port_switches"], f"wavdump={seconds}", extra_switches) if s)
    last = out / "azahar-last"
    if timeout is None:
        # the sink holds emulation to `speed` percent, so the scene takes seconds * 100 / speed; plus boot and margin
        timeout = 45 + seconds * (100.0 / speed if sink else 1.0) * 1.3
    argv = ["run_azahar.py", "--build", str(build), "--switches", switches, "--log", "3ds/ttyd/log.txt",
            "--until", "audio: wrote audio.wav", *emu_usage.time_args(timeout), "--last", str(last)]
    if sink:
        argv.append("--realtime")
    if lle_audio:
        argv.append("--lle-audio")
    out.mkdir(parents=True, exist_ok=True)
    (out / "switches.txt").write_text(switches + "\n")
    saved = sys.argv
    sys.argv = argv
    try:
        code = ra.main()
    except SystemExit as e:
        code = e.code if isinstance(e.code, int) else 1
    finally:
        sys.argv = saved
    _, _, sdmc = ra.profile_dirs()
    for name, dest in (("audio.wav", "wavdump.wav"),):
        wd = sdmc / "3ds" / "ttyd" / name
        if wd.is_file():
            shutil.copy(wd, out / dest)
            wd.unlink()
    if raw.is_file() and raw.stat().st_size > 0:
        n = raw_to_wav(raw, out / "azahar.wav")
        raw.unlink()
        print(f"audio_compare: Azahar output {n / 44100:.1f} s -> {(out / 'azahar.wav').relative_to(REPO)}")
    elif sink:
        print("audio_compare: no Azahar output file (SDL disk driver not used?)")
    return code or 0


def hook_sink(ra, raw, speed):
    """--sink: Azahar's sound output itself, recorded: its SDL2 sink (output_type 4) on SDL's disk
    driver writes it to a file. The sink takes samples at a fixed pace, and Azahar pads (or drops)
    when emulation is off that pace, so emulation is held at `speed` percent (frame limiter) and
    the disk driver slowed to match (SDL_AUDIO_DISK_TIMESCALE), so that emulation slower than real time keeps up.
    Even so Azahar falls behind at times: compare cuts the held runs (and counts them)."""
    ra.QT_CONFIG = ra.QT_CONFIG.replace("frame_limit={frame_limit}", f"frame_limit={speed}").replace(
        "audio_emulation={audio}\n",
        "audio_emulation={audio}\noutput_type=4\noutput_type\\default=false\n"
        # (Azahar hands its "auto" to SDL as a device name; this is the disk driver's one device)
        "output_device=System audio playback device\noutput_device\\default=false\n")
    base_cmd = ra.flatpak_cmd

    def flatpak_cmd(args):
        cmd = base_cmd(args)
        # (the Flatpak's SDL2 is sdl2-compat over SDL3: SDL3's names, and SDL2's for an older runtime)
        envs = ["--env=SDL_AUDIODRIVER=disk", "--env=SDL_AUDIO_DRIVER=disk",
                f"--env=SDL_DISKAUDIOFILE={raw}", f"--env=SDL_AUDIO_DISK_OUTPUT_FILE={raw}",
                f"--env=SDL_AUDIO_DISK_TIMESCALE={100.0 / speed:.4f}"]
        if os.environ.get("AUDIO_COMPARE_SDL_DEBUG"):
            envs.append("--env=SDL_LOGGING=*=debug")
        return cmd[:2] + envs + cmd[2:]
    ra.flatpak_cmd = flatpak_cmd


# ----------------------------------------------------------------------------- CPU

def cpu_profile(scene, build, extra_switches, out, duration, start):
    """The audio thread's busy share: the scene in Azahar under its GDB stub, sampled by
    pc_sample.py --interval 200 once the game log matches `start`."""
    import signal
    import time
    sc = SCENES[scene]
    # wdog: the sampler's stops (about 150 ms each) slow the game's frames past the 10 s watchdog
    switches = " ".join(s for s in (sc["port_switches"], "wdog=900", extra_switches) if s)
    out.mkdir(parents=True, exist_ok=True)
    ra = load_run_azahar()
    log = ra.PROFILE / "data" / "azahar-emu" / "sdmc" / "3ds" / "ttyd" / "log.txt"
    if log.exists():
        log.unlink()
    here = Path(__file__).resolve().parent
    runner = subprocess.Popen([sys.executable, str(here / "run_azahar.py"), "--build", str(build), "--switches", switches,
                               "--gdb", *emu_usage.time_args(duration + 900), "--allow-hw-faults",
                               "--last", str(out / "azahar-last")])
    try:
        cmd = [sys.executable, str(here / "pc_sample.py"), "--elf", str(build.with_suffix(".elf")),
               "--duration", str(duration), "--interval", "200", "--connect-timeout", "600",
               "--tag-log", str(log), "--start-after", start, "--start-timeout", "900",
               "--json", str(out / "pc_sample.json")]
        with open(out / "pc_sample.txt", "w") as so:
            r = subprocess.run(cmd, stdout=so, stderr=subprocess.STDOUT, timeout=duration + 2400)
        print(f"audio_compare: pc_sample exit {r.returncode}; {(out / 'pc_sample.txt').relative_to(REPO)}")
        return r.returncode
    finally:
        runner.send_signal(signal.SIGTERM)
        try:
            runner.wait(timeout=60)
        except subprocess.TimeoutExpired:
            runner.kill()


# ----------------------------------------------------------------------------- loop survey

def disc_files(iso):
    """{path: (offset, size)} from a GameCube disc image's FST."""
    import struct
    with open(iso, "rb") as f:
        f.seek(0x424)
        fst_off, fst_size = struct.unpack(">II", f.read(8))
        f.seek(fst_off)
        fst = f.read(fst_size)
    count = struct.unpack_from(">I", fst, 8)[0]
    names = fst[count * 12:]
    out = {}
    def name_at(o):
        return names[o:names.index(b"\0", o)].decode("ascii", "replace")
    # walk: directory entries carry the index of their next sibling
    stack = [("", count)]
    i = 1
    while i < count:
        while stack and i >= stack[-1][1]:
            stack.pop()
        word, a, b = struct.unpack_from(">III", fst, i * 12)
        name = name_at(word & 0xFFFFFF)
        path = (stack[-1][0] if stack else "") + "/" + name
        if word >> 24:
            stack.append((path, b))
        else:
            out[path] = (a, b)
        i += 1
    return out


def loop_survey(iso):
    """Every sample of the disc's MusyX sample directories: how many loop, and how many of
    those have a loop start (or end + 1) off a 14-sample DSP-ADPCM frame boundary."""
    import struct
    files = disc_files(iso)
    sdirs = sorted(p for p in files if p.lower().endswith(".sdir"))
    total = loops = start_off = end_off = both_ok = pcm = 0
    lengths = []
    examples = []
    with open(iso, "rb") as f:
        for p in sdirs:
            off, size = files[p]
            f.seek(off)
            d = f.read(size)
            n = 0
            while n * 0x20 + 2 <= len(d) and struct.unpack_from(">H", d, n * 0x20)[0] != 0xFFFF:
                sid, _ref, _o, _a, info, length, loop_off, loop_len, _x = struct.unpack_from(">HHIIIIIII", d, n * 0x20)
                comp = length >> 24
                nsmp = length & 0xFFFFFF
                total += 1
                if comp not in (0, 1, 4, 5):
                    pcm += 1
                elif loop_len:
                    loops += 1
                    s_ok = loop_off % 14 == 0
                    e_ok = (loop_off + loop_len) % 14 == 0
                    start_off += not s_ok
                    end_off += not e_ok
                    both_ok += s_ok and e_ok
                    lengths.append(loop_len)
                    if not s_ok and len(examples) < 8:
                        examples.append(f"{p} id {sid}: loop {loop_off}+{loop_len} of {nsmp} (start {loop_off % 14} into a frame)")
                n += 1
    print(f"{len(sdirs)} sample directories, {total} samples ({pcm} PCM, the rest DSP-ADPCM)")
    print(f"  looping ADPCM samples: {loops}; loop start off a frame boundary: {start_off}; "
          f"loop end+1 off a boundary: {end_off}; both on boundaries: {both_ok}")
    if lengths:
        ls = sorted(lengths)
        print(f"  loop lengths: min {ls[0]}, median {ls[len(ls) // 2]}, max {ls[-1]} samples; "
              f"shorter than one frame: {sum(1 for v in ls if v < 14)}")
    for e in examples:
        print("   ", e)
    return 0


LOOPCHECK_C = r'''
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* MusyX's software path (hw_pcmixer.c fetch1): decode to the end, jump to the
 * loop with the loop history (loopType 0). */
static void ref_play(const uint8_t* d, const uint16_t* c, uint32_t end, uint32_t ls, int16_t ly1, int16_t ly2,
                     uint32_t n, int16_t* out) {
    int32_t y1 = 0, y2 = 0;
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) {
        int16_t v;
        adpcm_decode(d, c, s, 1, y1, y2, &v);
        out[i] = v;
        y2 = y1;
        y1 = v;
        if (s == end - 1) {
            s = ls;
            y1 = ly1;
            y2 = ly2;
        } else {
            s++;
        }
    }
}

/* NDSP's model: buffer A [0, nls) then B [nls, nend) repeated, B's history
 * reloaded at each pass (adpcm_dirty). */
static void ndsp_play(const CacheEntry* e, const uint16_t* c, uint32_t n, int16_t* out) {
    int32_t y1 = 0, y2 = 0;
    uint32_t s = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s == e->loop_start) {
            y1 = e->loop_yn1;
            y2 = e->loop_yn2;
        }
        int16_t v;
        adpcm_decode(e->lin, c, s, 1, y1, y2, &v);
        out[i] = v;
        y2 = y1;
        y1 = v;
        s = (s == e->end - 1) ? e->loop_start : s + 1;
    }
}

int main(int argc, char** argv) {
    FILE* f = fopen(argv[1], "rb");
    long sdir_off = atol(argv[2]), sdir_len = atol(argv[3]), samp_off = atol(argv[4]), samp_len = atol(argv[5]);
    uint8_t* sdir = malloc(sdir_len);
    uint8_t* samp = malloc(samp_len + 64);
    fseek(f, sdir_off, SEEK_SET);
    fread(sdir, 1, sdir_len, f);
    fseek(f, samp_off, SEEK_SET);
    fread(samp, 1, samp_len, f);
    memset(samp + samp_len, 0, 64);
    int tested = 0, realigned = 0, bad = 0, ps_mismatch = 0;
    double worst_snr = 1e9, worst_jump = 0;
    for (int i = 0; be16(sdir + i * 32) != 0xFFFF; i++) {
        const uint8_t* e = sdir + i * 32;
        uint32_t off = be32(e + 4), len = be32(e + 0x10), loop = be32(e + 0x14), loopLen = be32(e + 0x18), x = be32(e + 0x1C);
        if ((len >> 24) != 0 || loopLen == 0 || x == 0) {
            continue;
        }
        const uint8_t* info = sdir + x;
        uint16_t coefs[16];
        for (int k = 0; k < 16; k++) {
            coefs[k] = be16(info + 8 + k * 2);
        }
        uint8_t loopPS = info[3];
        int16_t loopY0 = (int16_t)be16(info + 4), loopY1 = (int16_t)be16(info + 6);
        uint32_t end = loop + loopLen;
        const uint8_t* data = samp + off;
        if (loopPS != data[(loop / 14) * 8]) {
            ps_mismatch++;
        }
        CacheEntry ce;
        memset(&ce, 0, sizeof(ce));
        uint32_t frames = (end + 13) / 14 + 2;
        ce.lin = calloc(frames, 8);
        memcpy(ce.lin, data, ((end + 13) / 14) * 8);
        ce.end = end;
        ce.loop_start = loop;
        ce.looping = 1;
        ce.loop_ps = loopPS;
        ce.loop_yn1 = loopY1;
        ce.loop_yn2 = loopY0;
        int ok = realign_loop(&ce, coefs);
        tested++;
        if (!ok) {
            bad++;
            free(ce.lin);
            continue;
        }
        realigned += ce.realigned;
        uint32_t n = end + 3 * loopLen;
        int16_t* a = malloc(n * 2);
        int16_t* b = malloc(n * 2);
        ref_play(data, coefs, end, loop, loopY1, loopY0, n, a);
        ndsp_play(&ce, coefs, n, b);
        double sig = 0, err = 0, maxd = 0;
        uint32_t at = 0;
        for (uint32_t k = 0; k < n; k++) {
            double d = (double)a[k] - b[k];
            sig += (double)a[k] * a[k];
            err += d * d;
            if (fabs(d) > maxd) { maxd = fabs(d); at = k; }
        }
        double snr = err > 0 ? 10 * log10(sig / err) : 200;
        if (snr < worst_snr) worst_snr = snr;
        if (maxd > worst_jump) worst_jump = maxd;
        if (snr < 40 || maxd > 1500 || argc > 6)
            printf("id %u loop %u+%u (%u into a frame): SNR %.1f dB, max diff %.0f at %u (%s %u, end-1 at %u; ref %d new %d)\n",
                   be16(e), loop, loopLen, loop % 14, snr, maxd, at, at < end ? "first pass" : "loop pass offset",
                   at < end ? at : (at - end) % loopLen, loopLen - 1, a[at], b[at]);
        free(a);
        free(b);
        free(ce.lin);
    }
    printf("%d looping ADPCM samples: %d re-aligned, %d refused; loopPS differs from the frame header in %d; "
           "worst SNR over 3 loop passes %.1f dB, worst sample difference %.0f\n",
           tested, realigned, bad, ps_mismatch, worst_snr, worst_jump);
    return 0;
}
'''


def loop_check(iso):
    """The loop re-alignment of port3ds/plat/audio.c (adpcm_decode, adpcm_encode_frame,
    realign_loop, compiled for this PC with gcc) on every looping sample of the disc: the
    re-aligned copy played as NDSP plays it (buffer A, then B with its history reloaded each
    pass) against MusyX's software path, over the first pass and three loop passes."""
    import tempfile
    plat = (REPO / "port3ds" / "plat" / "audio.c").read_text()

    def grab(start, end):
        i = plat.index(start)
        return plat[i:plat.index(end, i)]
    code = ("#include <math.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n" +
            grab("typedef struct CacheEntry {", "} CacheEntry;") + "} CacheEntry;\n" +
            grab("static inline int16_t clamp16", "static void cache_free") + LOOPCHECK_C)
    files = disc_files(iso)
    sdir = files["/sound/proj/pmario.sdir"]
    samp = files["/sound/proj/pmario.samp"]
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "loopcheck.c", Path(tmp) / "loopcheck"
        c.write_text(code)
        r = subprocess.run(["gcc", "-O2", "-w", "-o", str(exe), str(c), "-lm"])
        if r.returncode != 0:
            return 2
        return subprocess.run([str(exe), str(iso), str(sdir[0]), str(sdir[1]), str(samp[0]), str(samp[1])]).returncode


# ----------------------------------------------------------------------------- self test

def selftest():
    rate = 32000
    t = np.arange(rate * 20) / rate
    rng = np.random.default_rng(7)
    seg = np.floor(t * 4).astype(int)
    env = rng.uniform(0.1, 1.0, seg.max() + 1)[seg] * np.exp(-8 * (t * 4 % 1))
    notes = 220 * 2 ** (rng.integers(0, 12, seg.max() + 1)[seg] / 12.0)
    phase = 2 * np.pi * np.cumsum(notes) / rate
    sig = 0.3 * env * np.sin(phase)
    a = np.stack([sig, 0.8 * sig], axis=1)
    tmp = OUT / "selftest"
    tmp.mkdir(parents=True, exist_ok=True)
    write_wav(tmp / "a.wav", a, rate)
    # B: 1.3 s later, at 32728 Hz, -1 dB, a little noise: within tolerance
    rb = 32728
    tb = np.arange(int(rb * 21)) / rb - 1.3
    b = np.stack([np.interp(tb, t, a[:, 0], left=0, right=0), np.interp(tb, t, a[:, 1], left=0, right=0)], axis=1)
    b = b * 10 ** (-1 / 20) + np.random.default_rng(1).normal(0, 1e-4, b.shape)
    write_wav(tmp / "b.wav", b, rb)
    r = compare(tmp / "a.wav", tmp / "b.wav")
    ok1 = r["ok"] and abs(r["lag_s"] - 1.3) < 0.05
    # C: a semitone up, with gaps: pitch and gaps flagged
    c = np.stack([np.interp(t * 2 ** (1 / 12), t, a[:, 0]), np.interp(t * 2 ** (1 / 12), t, a[:, 1])], axis=1)
    for k in range(5):  # 25 ms of zeros just after five note starts
        at = int(rate * (2 + 3 * k + 0.01) / 2 ** (1 / 12))
        c[at:at + 800] = 0
    write_wav(tmp / "c.wav", c, rate)
    r2 = compare(tmp / "a.wav", tmp / "c.wav", cut=False)
    ok2 = "pitch" in r2["fails"] and r2["gaps_per_min_b"] > r2["gaps_per_min_a"]
    print_report(r)
    print_report(r2)
    print("selftest:", "ok" if ok1 and ok2 else "FAILED")
    return 0 if ok1 and ok2 else 1


def main():
    if np is None:
        print("audio_compare: needs numpy")
        return 2
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scenes")
    p = sub.add_parser("capture-papercut")
    p.add_argument("--scene", required=True, choices=sorted(SCENES))
    p.add_argument("--out", type=Path)
    p = sub.add_parser("capture-port")
    p.add_argument("--scene", required=True, choices=sorted(SCENES))
    p.add_argument("--build", type=Path, default=REPO / "build" / "3ds" / "game" / "ttyd.cia")
    p.add_argument("--switches", default="", help="more switches (ndspvoices=1, ...)")
    p.add_argument("--tag", default="", help="suffix of the output folder")
    p.add_argument("--lle-audio", action="store_true")
    p.add_argument("--seconds", type=int, default=0, help="wavdump length (default per scene)")
    p.add_argument("--timeout", type=float, help="Azahar run limit in seconds (default: from the scene length; over 60 s is a --long run)")
    p.add_argument("--no-sink", action="store_true", help="only the game's software mix (wavdump), at full speed")
    p.add_argument("--speed", type=int, default=34,
                   help="emulation speed in percent while Azahar's output is recorded (default 34)")
    p.add_argument("--out", type=Path)
    p = sub.add_parser("compare")
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p.add_argument("--json", type=Path)
    p.add_argument("--max-lag", type=float, default=40.0)
    p.add_argument("--no-cut", action="store_true", help="keep held-sample runs")
    p.add_argument("--azahar", action="store_true",
                   help="B is an Azahar capture: its silent and held runs inside loud audio are the sink's "
                        "padding (cut, and not counted as gaps)")
    sub.add_parser("selftest")
    p = sub.add_parser("cpu", help="pc_sample.py profile of a scene (audio thread busy share)")
    p.add_argument("--scene", required=True, choices=sorted(SCENES))
    p.add_argument("--build", type=Path, default=REPO / "build" / "3ds" / "game" / "ttyd.cia")
    p.add_argument("--switches", default="")
    p.add_argument("--tag", default="")
    p.add_argument("--duration", type=float, default=60)
    p.add_argument("--start", default="padscript: done|warp: ",
                   help="regex in the game log that starts the sampling")
    p = sub.add_parser("loops", help="loop points of every MusyX sample on the disc")
    p.add_argument("--iso", type=Path, default=REPO / "iso" / "game.iso")
    p = sub.add_parser("loopcheck", help="the port's loop re-alignment checked on every looping sample (needs gcc)")
    p.add_argument("--iso", type=Path, default=REPO / "iso" / "game.iso")
    a = ap.parse_args()
    if a.cmd == "loops":
        return loop_survey(a.iso)
    if a.cmd == "loopcheck":
        return loop_check(a.iso)
    if a.cmd == "cpu":
        tag = ("_" + a.tag) if a.tag else ""
        return cpu_profile(a.scene, a.build.resolve(), a.switches, OUT / f"cpu_{a.scene}{tag}", a.duration, a.start)
    if a.cmd == "scenes":
        for k, v in SCENES.items():
            print(f"{k:8} {v['desc']}\n         port: {v['port_switches'] or '(boot)'}\n         paper-cut: {v['pc_env'] or '(boot)'}")
        return 0
    if a.cmd == "capture-papercut":
        return capture_papercut(a.scene, a.out or OUT / f"pc_{a.scene}")
    if a.cmd == "capture-port":
        tag = ("_" + a.tag) if a.tag else ""
        return capture_port(a.scene, a.out or OUT / f"port_{a.scene}{tag}", a.build.resolve(), a.switches,
                            a.lle_audio, a.timeout, a.seconds, a.speed, not a.no_sink)
    if a.cmd == "compare":
        r = compare(a.a, a.b, a.max_lag, cut=not a.no_cut, azahar_b=a.azahar or a.b.name == "azahar.wav")
        print_report(r)
        if a.json:
            a.json.write_text(json.dumps(r, indent=1))
        return 0 if r["ok"] else 1
    if a.cmd == "selftest":
        return selftest()
    return 2


if __name__ == "__main__":
    sys.exit(main())
