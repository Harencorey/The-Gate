"""Loading and basic analysis of multitrack drum sessions for The Gate's training data.

Layout (see the README in this folder):  <root>/<session>/<song>/<mic>.wav
All mics of a song are the same length and start together (sample-aligned).
"""
import os
import numpy as np
from scipy.io import wavfile

CLOSE_MICS = ["kick_in", "kick_port", "snare_top", "snare_top_ldc", "snare_bot", "rack1", "rack2", "floor1", "floor2", "hat", "ride"]
AMBIENT = ["oh_l", "oh_r", "room_l", "room_r"]
NOT_A_MIC = ["kick_trig"]          # replaced kick sample: timing reference only
DRUM_OF = {"kick_in": "kick", "kick_port": "kick", "snare_top": "snare", "snare_top_ldc": "snare", "snare_bot": "snare",
           "rack1": "rack1", "rack2": "rack2", "floor1": "floor1", "floor2": "floor2", "hat": "hat", "ride": "ride"}


def load(path):
    """Mono float32 in [-1, 1) and the sample rate."""
    sr, x = wavfile.read(path)
    if x.dtype == np.int32:   x = x.astype(np.float32) / 2147483648.0     # 24-bit arrives left-justified in int32
    elif x.dtype == np.int16: x = x.astype(np.float32) / 32768.0
    else:                     x = x.astype(np.float32)
    if x.ndim > 1: x = x.mean(axis=1)
    return x, sr


def songs(root):
    for session in sorted(os.listdir(root)):
        sp = os.path.join(root, session)
        if not os.path.isdir(sp): continue
        for song in sorted(os.listdir(sp)):
            if os.path.isdir(os.path.join(sp, song)):
                yield session, song, os.path.join(sp, song)


def load_song(path, mics=None):
    out, sr = {}, None
    for f in sorted(os.listdir(path)):
        m = f[:-4]
        if f.endswith(".wav") and (mics is None or m in mics):
            out[m], sr = load(os.path.join(path, f))
    return out, sr


def db(v):
    return 20.0 * np.log10(np.maximum(np.abs(v), 1e-12))


# ---------------------------------------------------------------------------------------------
# Onsets (offline, non-causal: for labelling, not the plug-in's detector)
from scipy.ndimage import maximum_filter1d, minimum_filter1d
from scipy.signal import lfilter, butter, sosfilt


def envelope(x, sr, smooth_ms=3.0):
    """Peak envelope: 1 ms running max, smoothed (linear amplitude)."""
    sos = butter(2, 30.0, "highpass", fs=sr, output="sos")      # rumble / DC out
    a = np.abs(sosfilt(sos, x.astype(np.float64)))
    m = maximum_filter1d(a, size=max(1, int(0.001 * sr)), origin=0)
    k = np.exp(-1.0 / (smooth_ms * 0.001 * sr))
    return lfilter([1 - k], [1, -k], m)


def onsets(x, sr, rise_db=9.0, floor_db=-55.0, min_gap_ms=25.0):
    """Strike times (samples): the envelope rises rise_db over its minimum of the previous 30 ms.
    Each onset is moved back to where the rise starts (first sample above 10 % of the local peak)."""
    env = envelope(x, sr)
    edb = 20 * np.log10(env + 1e-12)
    look = int(0.030 * sr)
    prevmin = minimum_filter1d(edb, size=look, origin=look // 2 - 1)       # min over the previous 30 ms
    cand = (edb - prevmin > rise_db) & (edb > floor_db)
    idx = np.flatnonzero(cand & ~np.r_[False, cand[:-1]])                   # first sample of each run
    out, last = [], -10**12
    gap = int(min_gap_ms * 0.001 * sr)
    w = int(0.004 * sr)
    for i in idx:
        if i - last < gap: continue
        seg = env[i : i + w]
        pk = seg.max()
        j = i - int(0.003 * sr)
        j = max(j, 0)
        a = np.abs(x[j : i + w])
        first = np.flatnonzero(a > 0.1 * pk)
        t = j + (first[0] if len(first) else 0)
        out.append(t); last = i
    return np.array(out, dtype=np.int64), env


def peak_after(x, t, sr, win_ms=12.0, pre_ms=1.0):
    """Peak |x| in [t - pre, t + win] for each t."""
    a, b = int(pre_ms * 0.001 * sr), int(win_ms * 0.001 * sr)
    ax = np.abs(x)
    return np.array([ax[max(0, s - a) : s + b].max() for s in t])
