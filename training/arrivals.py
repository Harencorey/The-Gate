"""Precise per-mic arrival time and new level at each event."""
import numpy as np
from scipy.signal import butter, sosfilt


def hp(x, sr, hz=30.0):
    return sosfilt(butter(2, hz, "high", fs=sr, output="sos"), x).astype(np.float32)


def measure(xs, ev, sr, pre=(15.0, 3.0), win=(3.0, 12.0), frac=0.3):
    """For each event and mic: new peak level (peak in window minus what was ringing before) and the
    arrival = first sample in the window above pre + frac*(new peak). Returns L, T (T in samples, nan if none)."""
    a0, a1 = int(pre[0] * 1e-3 * sr), int(pre[1] * 1e-3 * sr)
    w0, w1 = int(win[0] * 1e-3 * sr), int(win[1] * 1e-3 * sr)
    L = np.zeros((len(ev), len(xs))); T = np.full((len(ev), len(xs)), np.nan)
    for m, x in enumerate(xs):
        ax = np.abs(x)
        for e, s in enumerate(ev):
            b = ax[max(0, s - a0): max(1, s - a1)].max()
            seg = ax[max(0, s - w0): s + w1]
            p = seg.max()
            if p <= b * 1.12: continue          # nothing new (< 1 dB)
            L[e, m] = p - b
            if p < 2.0 * b: continue            # level counts, but the timing isn't trustworthy (< 6 dB new)
            thr = b + frac * (p - b)
            i = int(np.argmax(seg > thr))
            # sub-sample: interpolate the crossing
            if i > 0:
                y0, y1 = seg[i - 1], seg[i]
                T[e, m] = max(0, s - w0) + i - 1 + (thr - y0) / max(y1 - y0, 1e-12)
            else:
                T[e, m] = max(0, s - w0)
    return L, T
