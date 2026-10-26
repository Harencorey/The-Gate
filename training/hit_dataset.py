"""Classifier dataset: every event a gate on this mic would consider, labelled 'my drum' or 'bleed'.

Events = onsets in the mic (gatedata.onsets) within 36 dB of the mic's loud hits.
  positive: a SURE label of this drum within 5 ms
  negative: no label of this drum (sure or unsure) within 30 ms
  (unsure-only events are left out: we don't know)
Features (only what the gate can know within its lookahead: the first 12 ms):
  shape    24 log-spaced band levels (40 Hz - 16 kHz) of the event's first 12 ms, minus their mean
  new      the same bands: the event's first 12 ms vs the 20 ms before it (how much is new)
  level    the event's level re the median of this drum's own hits (the learned source level)
  f0       the energy at the drum's fundamental re the whole event (the Source key's band)
  attack   the >1 kHz band's jump over the 10 ms before (the stick-attack check), and its rise time
  prof     correlation of 'shape' with this drum's mean hit shape and with its mean bleed shape
           (the 'learned profile' - estimated from the training songs only)
Usage (module): build(session_dir, labels_dir, drum, mic, songs) -> X (dict of arrays), y, meta
"""
import json, os
import numpy as np
from scipy.signal import butter, sosfilt
import gatedata as gd

SR = 48000
NB = 24
EDGES = np.geomspace(40, 16000, NB + 1)
N12, PRE = int(0.012 * SR), int(0.020 * SR)
NFFT = 1024
FREQS = np.fft.rfftfreq(NFFT, 1 / SR)
BIN_OF = [np.nonzero((FREQS >= EDGES[b]) & (FREQS < EDGES[b + 1]))[0] for b in range(NB)]
WIN = np.hanning(N12)


def band_levels(seg):
    sp = np.abs(np.fft.rfft(seg * WIN[: len(seg)], NFFT)) ** 2
    return np.array([10 * np.log10(sp[i].sum() + 1e-12) if len(i) else -120.0 for i in BIN_OF])


def events(x, labels_own_sure, labels_own_any):
    t, _ = gd.onsets(x, SR)
    if len(t) == 0: return [], []
    pk = gd.db(gd.peak_after(x, t, SR))
    loud = np.percentile(pk, 98)
    keep = t[pk >= loud - 36]
    ys, ts = [], []
    s_sure, s_any = np.array(labels_own_sure), np.array(labels_own_any)
    for e in keep:
        if e < PRE + 10 or e + N12 >= len(x): continue
        if len(s_sure) and np.min(np.abs(s_sure - e)) <= int(0.005 * SR): ys.append(1); ts.append(e)
        elif not len(s_any) or np.min(np.abs(s_any - e)) > int(0.030 * SR): ys.append(0); ts.append(e)
    return np.array(ts), np.array(ys)


def raw_features(x, ts, f0):
    hp = sosfilt(butter(4, 1000, "high", fs=SR, output="sos"), x)
    bp = sosfilt(butter(2, [f0 / 1.26, f0 * 1.26], "band", fs=SR, output="sos"), x) if f0 else None
    out = {"shape": [], "new": [], "lvl": [], "f0": [], "atk": [], "rise": []}
    for e in ts:
        seg, pre = x[e: e + N12].astype(np.float64), x[e - PRE: e].astype(np.float64)
        b = band_levels(seg)
        out["shape"].append(b - b.mean())
        bpre = band_levels(pre[-N12:])
        out["new"].append(b - bpre)
        tot = 10 * np.log10(np.sum(seg ** 2) + 1e-12)
        out["lvl"].append(tot)
        out["f0"].append((10 * np.log10(np.sum(bp[e: e + N12] ** 2) + 1e-12) - tot) if bp is not None else 0.0)
        h, hpre = np.abs(hp[e - 48: e + N12]), np.abs(hp[e - PRE: e - 48])
        out["atk"].append(20 * np.log10((h.max() + 1e-9) / (hpre.max() + 1e-9)))
        out["rise"].append(np.argmax(h) / SR * 1000)
    return {k: np.array(v) for k, v in out.items()}


def build(session_dir, labels_dir, sess, drum, mic, songs, f0=None):
    feats, ys, meta = [], [], []
    for song in songs:
        p = f"{session_dir}/{song}/{mic}.wav"
        if not os.path.exists(p): continue
        lab = json.load(open(f"{labels_dir}/{sess}_{song}.json"))
        sure = [h["sample"] for h in lab["hits"] if h["drum"] == drum and h["sure"]]
        anyl = [h["sample"] for h in lab["hits"] if h["drum"] == drum]
        x, _ = gd.load(p)
        ts, y = events(x, sure, anyl)
        if len(ts) == 0: continue
        F = raw_features(x, ts, f0)
        # what else was struck (for the error analysis): other drums' sure labels within 10 ms
        others = []
        for e in ts:
            near = [h["drum"] for h in lab["hits"] if h["drum"] != drum and h["sure"] and abs(h["sample"] - e) <= int(0.010 * SR)]
            others.append(",".join(sorted(set(near))) or "-")
        feats.append(F); ys.append(y); meta += [(song, int(e), o) for e, o in zip(ts, others)]
    F = {k: np.concatenate([f[k] for f in feats]) for k in feats[0]}
    return F, np.concatenate(ys), meta


def finish(F, prof_src, prof_bleed, lvl_ref):
    """Feature matrix with profile-relative features (profiles/levels from TRAINING data only)."""
    sh = F["shape"]
    def corr(a, b): a = a - a.mean(1, keepdims=True); b = b - b.mean(); return (a @ b) / (np.linalg.norm(a, axis=1) * np.linalg.norm(b) + 1e-9)
    cols = [sh, F["new"], (F["lvl"] - lvl_ref)[:, None], F["f0"][:, None], F["atk"][:, None], F["rise"][:, None],
            corr(sh, prof_src)[:, None], corr(sh, prof_bleed)[:, None]]
    return np.hstack(cols)
