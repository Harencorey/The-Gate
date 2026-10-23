"""Hit labelling by fingerprints: each source has a fingerprint (its energy in every mic of the kit).
Every event is split into a non-negative mix of fingerprints (energies add for simultaneous hits), and
the fingerprints are re-learned from events one source clearly owns. Uses ALL mics (snare bottom, kick
port, rooms, overheads), which separates sources that look alike in their own mics (snare vs hat)."""
import numpy as np
from scipy.optimize import nnls


def event_energy(xs, ev, sr, win=(1.0, 15.0), pre=(30.0, 3.0)):
    """New energy each mic gains at each event: energy in the window minus what the pre-window's
    level would have put there. Returns E (events x mics), and each mic's arrival (first sample above
    25% of the window's peak excess), T."""
    w0, w1 = int(win[0] * 1e-3 * sr), int(win[1] * 1e-3 * sr)
    p0, p1 = int(pre[0] * 1e-3 * sr), int(pre[1] * 1e-3 * sr)
    E = np.zeros((len(ev), len(xs))); T = np.full((len(ev), len(xs)), np.nan)
    for m, x in enumerate(xs):
        x2 = x.astype(np.float64) ** 2
        c = np.concatenate([[0.0], np.cumsum(x2)])
        a, b = np.clip(ev - w0, 0, len(x)), np.clip(ev + w1, 0, len(x))
        pa, pb = np.clip(ev - p0, 0, len(x)), np.clip(ev - p1, 0, len(x))
        rate = (c[pb] - c[pa]) / np.maximum(pb - pa, 1)
        E[:, m] = np.maximum(c[b] - c[a] - rate * (b - a), 0.0)
        ax = np.abs(x)
        for e, s in enumerate(ev):
            if E[e, m] <= 0: continue
            seg = ax[a[e]: b[e]]
            base = np.sqrt(rate[e]) * 1.5
            pk = seg.max()
            if pk < 2 * base: continue
            T[e, m] = a[e] + int(np.argmax(seg > base + 0.25 * (pk - base)))
    return E, T


def learn(E, own, iters=6, share=0.85):
    """own[s] = list of mic indices that belong to source s. Returns fingerprints F (sources x mics),
    each scaled so its own mics sum to 1, and per-mic scale used for weighting."""
    nm = E.shape[1]
    scale = np.array([np.percentile(E[E[:, m] > 0, m], 99) if (E[:, m] > 0).any() else 1.0 for m in range(nm)])
    En = E / scale
    S = len(own)
    F = np.full((S, nm), 0.02)
    for s, ms in enumerate(own): F[s, ms] = 1.0 / len(ms)
    for _ in range(iters):
        A = np.array([nnls(F.T, en)[0] if en.sum() > 0 else np.zeros(S) for en in En])
        fit = A[:, :, None] * F[None, :, :]                    # events x sources x mics
        tot = fit.sum(axis=(1, 2)) + 1e-12
        sh = fit.sum(axis=2) / tot[:, None]
        newF = F.copy()
        for s, ms in enumerate(own):
            clean = (sh[:, s] >= share) & (A[:, s] > 0)
            if clean.sum() < 8: continue
            prof = En[clean] / (En[clean][:, ms].sum(1, keepdims=True) + 1e-12)
            newF[s] = np.median(prof, axis=0)
            newF[s, ms] = np.median(prof[:, ms], axis=0)
            newF[s] = np.maximum(newF[s], 1e-4)
        F = newF
    A = np.array([nnls(F.T, en)[0] if en.sum() > 0 else np.zeros(S) for en in En])
    return F, A, En, scale
