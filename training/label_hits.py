"""Auto-labels every drum hit in a multitrack session, from arrival time and level.

The struck drum reaches its own close mic first. Every other mic hears it later by a fixed delay
(the kit's geometry) and quieter by a roughly fixed amount: the BLEED MAP.
  1. Events: onsets on any close mic or overhead, grouped within 6 ms.
  2. Per event and mic: the NEW level (peak minus what was already ringing) and a precise arrival time.
  3. Seeds: events where one mic leads every other mic by >= 0.4 ms (one source struck alone).
     They give the bleed map: delay and level of each source in each mic.
  4. Labelling: mics in arrival order. A mic is bleed when the sources already claimed at this
     event explain it (arrival within tolerance of the learned delay and level <= expected + 9 dB);
     otherwise its own drum was struck. The overheads stand for cymbals with no close mic (crashes).

Usage: python3 label_hits.py <session_root> <out_dir>
"""
import json, os, sys, warnings
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatedata as gd
import arrivals as ar
warnings.filterwarnings("ignore")

SOURCES = [("kick", "kick_in"), ("snare", "snare_top"), ("rack1", "rack1"), ("rack2", "rack2"), ("floor1", "floor1"),
           ("floor2", "floor2"), ("hat", "hat"), ("ride", "ride"), ("cymbal", "oh_l"), ("cymbal", "oh_r")]
DRUMS = ["kick", "snare", "rack1", "rack2", "floor1", "floor2", "hat", "ride", "cymbal"]
LEAD = 0.4e-3      # seed: lead over every other mic
TOL = 0.8e-3       # arrival tolerance around the learned delay
OVER = 10 ** (9 / 20)
MIN_DB = -40.0     # quietest hit labelled, re a full hit


def events_for(song, sr):
    allt = np.sort(np.concatenate([gd.onsets(song[m], sr)[0] for _, m in SOURCES]))
    ev, cur = [], [allt[0]]
    for t in allt[1:]:
        if t - cur[0] <= int(0.006 * sr): cur.append(t)
        else: ev.append(min(cur)); cur = [t]
    ev.append(min(cur))
    return np.array(ev, dtype=np.int64)


def bleed_map(L, T, sr):
    n = L.shape[1]
    Ts = np.where(np.isnan(T), np.inf, T)
    o = np.argsort(Ts, 1)
    r = np.arange(len(T))
    lead = (Ts[r, o[:, 1]] - Ts[r, o[:, 0]]) / sr
    have2 = np.isfinite(Ts[r, o[:, 1]])
    # the leader must be a real hit in its own mic (>= -20 dB of its loud hits)
    big = np.array([np.percentile(L[L[:, s] > 0, s], 98) for s in range(n)])
    lead_lvl = L[r, o[:, 0]] / big[o[:, 0]]
    seed = have2 & (lead >= LEAD) & (lead_lvl >= 0.1)
    D = np.zeros((n, n)); R = np.eye(n); nseed = np.zeros(n, int)
    for s in range(n):
        e = seed & (o[:, 0] == s)
        nseed[s] = e.sum()
        for m in range(n):
            if m == s: continue
            ok = e & ~np.isnan(T[:, m])
            if ok.sum() >= 8:
                D[s, m] = np.median(T[ok, m] - T[ok, s]) / sr
                R[s, m] = np.median(L[ok, m] / L[ok, s])
            else:   # unknown pair: far apart, quiet
                D[s, m], R[s, m] = 4e-3, 10 ** (-30 / 20)
    ref = np.array([np.percentile(L[seed & (o[:, 0] == s), s], 90) if nseed[s] >= 8 else np.percentile(L[L[:, s] > 0, s], 98)
                    for s in range(n)])
    return D, R, ref, nseed


def label_events(L, T, ev, sr, D, R, ref):
    n = L.shape[1]
    hits = []
    for e in range(len(ev)):
        order = [m for m in np.argsort(np.where(np.isnan(T[e]), np.inf, T[e])) if not np.isnan(T[e, m])]
        own = []
        for m in order:
            exp = sum(R[s, m] * L[e, s] for s in own)
            timed = any(abs((T[e, m] - T[e, s]) / sr - D[s, m]) <= TOL for s in own)
            if own and timed and L[e, m] <= OVER * exp:
                continue                                   # bleed from what was struck
            lvl = L[e, m] / ref[m]
            if gd.db(lvl) < MIN_DB: continue
            own.append(m)
            # confidence: how far above the bleed it would have been (no claimed source -> 1)
            conf = 1.0 if exp <= 0 else float(min(1.0, 1 - exp / L[e, m]))
            hits.append({"drum": SOURCES[m][0], "mic": SOURCES[m][1], "sample": int(round(T[e, m])),
                         "time": round(float(T[e, m]) / sr, 6), "level_db": round(float(gd.db(lvl)), 2),
                         "confidence": round(max(conf, 0.0), 3)})
    hits.sort(key=lambda h: (h["sample"], h["drum"]))
    out, last = [], {}
    for h in hits:   # one label per source per 20 ms (both overheads hearing one crash = one hit)
        if h["sample"] - last.get(h["drum"], -10 ** 9) >= int(0.020 * sr):
            out.append(h); last[h["drum"]] = h["sample"]
    return out


def label_song(path):
    song, sr = gd.load_song(path)
    ev = events_for(song, sr)
    xs = [ar.hp(song[m], sr) for _, m in SOURCES]
    L, T = ar.measure(xs, ev, sr)
    D, R, ref, nseed = bleed_map(L, T, sr)
    hits = label_events(L, T, ev, sr, D, R, ref)
    names = [m for _, m in SOURCES]
    return {"sr": sr, "length": len(song["kick_in"]), "hits": hits,
            "seeds": {names[i]: int(nseed[i]) for i in range(len(names))},
            "bleed_delay_ms": {names[i]: {names[j]: round(D[i, j] * 1000, 2) for j in range(len(names))} for i in range(len(names))},
            "bleed_level_db": {names[i]: {names[j]: round(float(gd.db(R[i, j])), 1) for j in range(len(names))} for i in range(len(names))},
            "ref_dbfs": {names[i]: round(float(gd.db(ref[i])), 1) for i in range(len(names))}}, song, sr


def kick_check(result, song, sr):
    """Kick labels vs the kick trigger, an independent reference: each loud trigger should have a kick
    label 6-16 ms before it (the trigger's latency is ~11.7 ms), and vice versa."""
    t, _ = gd.onsets(song["kick_trig"], sr)
    pk = gd.db(gd.peak_after(song["kick_trig"], t, sr))
    trig = t[pk >= -25]
    keep = [True] + [trig[i] - trig[i - 1] >= int(0.050 * sr) for i in range(1, len(trig))]
    trig = trig[np.array(keep)]
    kicks = np.array([h["sample"] for h in result["hits"] if h["drum"] == "kick"])
    lo, hi = int(0.006 * sr), int(0.016 * sr)
    matched = sum(1 for s in trig if np.any((kicks >= s - hi) & (kicks <= s - lo)))
    extra = [k for k in kicks if not np.any((trig >= k + lo) & (trig <= k + hi))]
    return {"loud_triggers": len(trig), "matched": matched, "kick_labels": len(kicks), "kick_labels_without_trigger": len(extra)}


if __name__ == "__main__":
    root, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for session, name, path in gd.songs(root):
        res, song, sr = label_song(path)
        res["kick_vs_trigger"] = kick_check(res, song, sr)
        with open(os.path.join(out, f"{session}_{name}.json"), "w") as f:
            json.dump(res, f)
        counts = {d: sum(1 for h in res["hits"] if h["drum"] == d) for d in DRUMS}
        print(f"{session}/{name}: {len(res['hits'])} hits {counts}  kick vs trigger {res['kick_vs_trigger']}", flush=True)
