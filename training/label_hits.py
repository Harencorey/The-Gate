"""Confident hit labels for a multitrack drum session.

Every hit is SURE or UNSURE; anything else at an event is bleed. Training and the gate tests use the
SURE set; UNSURE hits are kept for review (and later the ARA review timeline).

  kick, snare  the own mic's new level at each event falls into a bleed group and one or more hit
               groups (soft / full). A Gaussian mixture on the level (dB) finds them; the lowest group
               is bleed. Snare uses the bottom mic, which barely hears the hats. A kick trigger, when
               the session has one, is a second witness: SURE needs the mic and the trigger to agree.
  toms         RATIOS, not levels: bleed into a tom mic sits at a fixed ratio to the source's own mic
               (kit geometry), however hard that source is hit. A tom is hit when its mic is >= 12 dB
               (SURE) or >= 6 dB (UNSURE) over the loudest bleed expected from everything else heard at
               that instant: kick and snare when struck, the overheads (cymbals) and the hat mic always,
               and other toms that were hit.
  cymbals      not labelled yet (no reliable separation in these sessions; not a v1 gate target).

Usage: python3 label_hits.py <session_dir> <out_dir>      (session_dir holds song folders)
"""
import json, os, sys, warnings
import numpy as np
from sklearn.mixture import GaussianMixture
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatedata as gd
import arrivals as ar
warnings.filterwarnings("ignore")

KICK, SNARE = ["kick_in", "kick_port", "kick_out"], ["snare_bot", "snare_top"]
TOMS = ["rack1", "rack2", "floor1", "floor2"]
SURE_DB, UNSURE_DB = 12.0, 6.0
TOM_FLOOR_SURE, TOM_FLOOR_UNSURE = 24.0, 30.0   # dB below the tom's loudest hits (99.5th pct)
STICK_HZ = 1500.0


def events_for(song, sr, mics):
    allt = np.sort(np.concatenate([gd.onsets(song[m], sr)[0] for m in mics]))
    ev, cur = [], [allt[0]]
    for t in allt[1:]:
        if t - cur[0] <= int(0.006 * sr): cur.append(t)
        else: ev.append(min(cur)); cur = [t]
    ev.append(min(cur))
    return np.array(ev, dtype=np.int64)


def hit_probability(level_db):
    """P(hit) per event from a 2-4 group Gaussian mixture on the level; the lowest group is bleed."""
    x = level_db[level_db > -90].reshape(-1, 1)
    best = None
    for k in (2, 3, 4):
        g = GaussianMixture(k, n_init=3, random_state=0).fit(x)
        b = g.bic(x)
        if best is None or b < best[0]: best = (b, g)
    g = best[1]
    mu, sd = g.means_[:, 0], np.sqrt(g.covariances_.reshape(-1))
    o = np.argsort(mu)
    # bleed groups / hit groups: split at the widest gap between neighbouring groups, in spreads
    sep = [(mu[o[k + 1]] - mu[o[k]]) / (sd[o[k + 1]] + sd[o[k]]) for k in range(len(o) - 1)]
    cut = int(np.argmax(sep))
    bleed = o[: cut + 1]
    post = g.predict_proba(np.maximum(level_db, -90).reshape(-1, 1))
    p = 1 - post[:, bleed].sum(axis=1)
    p[level_db < mu[o[cut]]] = 0.0                              # never a hit below the top bleed group's centre
    groups = sorted((round(float(m), 1), round(float(np.sqrt(c.ravel()[0])), 1), round(float(w), 3))
                    for m, c, w in zip(g.means_[:, 0], g.covariances_, g.weights_))
    return p, {"groups_mean_sd_weight": groups, "hit_groups_from_dbfs": round(float(mu[o[cut + 1]]), 1)}


def label_song(path, cache=None):
    song, sr = gd.load_song(path)
    have = set(song)
    kick_mic = next(m for m in KICK if m in have)
    snare_mic = next(m for m in SNARE if m in have)
    snare_top = "snare_top" if "snare_top" in have else snare_mic
    toms = [t for t in TOMS if t in have]
    refs = [m for m in ("hat", "oh_l", "oh_r") if m in have]   # always-on bleed references (cymbals)
    mics = list(dict.fromkeys([kick_mic, snare_top, snare_mic] + toms + refs))
    if cache and os.path.exists(cache):
        z = np.load(cache, allow_pickle=True)
        if list(z["mics"]) == mics: ev, L, T = z["ev"], z["L"], z["T"]
        else: cache = None
    if not (cache and os.path.exists(cache)):
        ev = events_for(song, sr, mics)
        L, T = ar.measure([ar.hp(song[m], sr) for m in mics], ev, sr)
    I = {m: k for k, m in enumerate(mics)}
    lv = {m: gd.db(np.maximum(L[:, I[m]], 1e-9)) for m in mics}
    # toms are judged in the stick band: sympathetic ring (a floor tom ringing off the kick) lives at
    # the tom's pitch and has no stick attack; a real hit cracks well above 1.5 kHz in its own mic
    hb = [m for m in mics if m in toms or m in refs or m in (kick_mic, snare_top)]
    LH, _ = ar.measure([ar.hp(song[m], sr, STICK_HZ) for m in hb], ev, sr)
    lvh = {m: gd.db(np.maximum(LH[:, k], 1e-9)) for k, m in enumerate(hb)}
    hasH = {m: LH[:, k] > 0 for k, m in enumerate(hb)}
    hits, info = [], {}

    def add(drum, mic, e, sure, why, extra=None):
        t = T[e, I[mic]] if not np.isnan(T[e, I[mic]]) else ev[e]
        h = {"drum": drum, "mic": mic, "sample": int(round(t)), "time": round(float(t) / sr, 6),
             "level_dbfs": round(float(lv[mic][e]), 2), "sure": bool(sure), "why": why}
        if extra: h.update(extra)
        hits.append(h)

    # --- kick and snare: mixture on the own mic's level
    pk, info["kick"] = hit_probability(lv[kick_mic]); info["kick"]["mic"] = kick_mic
    ps, info["snare"] = hit_probability(lv[snare_mic]); info["snare"]["mic"] = snare_mic
    kick_hit, snare_hit = pk >= 0.5, ps >= 0.5
    kick_sure = pk >= 0.9
    if "kick_trig" in have:
        # the trigger is a witness: its sample has a second attack ~45 ms in, it misses some kicks
        # and false-fires on loud toms. SURE = mic and trigger agree.
        t, _ = gd.onsets(song["kick_trig"], sr)
        tp = gd.db(gd.peak_after(song["kick_trig"], t, sr))
        tr = t[tp >= tp.max() - 12]
        loud = ev[kick_sure] if kick_sure.sum() >= 20 else ev[kick_hit]
        off = np.array([s - loud[np.argmin(np.abs(loud - s))] for s in tr])
        lat = int(np.median(off[(off > 0) & (off < int(0.025 * sr))]))
        trig = np.array([np.any(np.abs(tr - lat - e) <= int(0.005 * sr)) for e in ev])
        info["kick"].update({"trigger_latency_ms": round(lat / sr * 1000, 2),
                             "trigger_fired_without_kick": int(np.sum(trig & ~kick_hit))})
        kick_sure = kick_sure & trig
        kwhy = ("kick mic + trigger", "kick mic only (no trigger, or not clearly above bleed)")
    else:
        kwhy = ("kick mic", "kick mic, not clearly above bleed")
    for e in np.nonzero(kick_hit)[0]: add("kick", kick_mic, e, kick_sure[e], kwhy[0] if kick_sure[e] else kwhy[1], {"p": round(float(pk[e]), 3)})
    for e in np.nonzero(snare_hit)[0]:
        add("snare", snare_top, e, ps[e] >= 0.9, "snare bottom mic" if ps[e] >= 0.9 else "snare bottom, near bleed (ghost?)", {"p": round(float(ps[e]), 3)})

    # --- toms: excess over the loudest expected bleed, from ratios
    struck = {kick_mic: kick_hit, snare_top: snare_hit}
    B = {}
    for tom in toms:
        for s, hit in struck.items():
            only = hit & ~np.any([h for k2, h in struck.items() if k2 != s], axis=0) & hasH[s] & hasH[tom]
            B[(s, tom)] = float(np.median(lvh[tom][only] - lvh[s][only])) if only.sum() >= 20 else -30.0
        quiet = ~kick_hit & ~snare_hit
        for r in refs:
            ok = quiet & hasH[r] & hasH[tom]
            B[(r, tom)] = float(np.median(lvh[tom][ok] - lvh[r][ok])) if ok.sum() >= 20 else -30.0

    def excess(tom, extra_refs=()):
        exp = np.full(len(ev), -200.0)
        for s, hit in struck.items():
            exp = np.where(hit, np.maximum(exp, lvh[s] + B[(s, tom)]), exp)
        for r in refs: exp = np.maximum(exp, np.where(hasH[r], lvh[r] + B[(r, tom)], -200.0))
        for (src, mask) in extra_refs: exp = np.where(mask, np.maximum(exp, lvh[src] + B[(src, tom)]), exp)
        return np.where(hasH[tom], lvh[tom] - exp, -200.0)

    ex = {t: excess(t) for t in toms}
    # pass 2: tom-to-tom bleed, learned from pass-1 SURE hits, then each tom judged against the others
    sure1 = {t: ex[t] >= SURE_DB for t in toms}
    for a in toms:
        for b in toms:
            if a == b: continue
            ok = sure1[a] & ~sure1[b] & hasH[b]
            B[(a, b)] = float(np.median(lvh[b][ok] - lvh[a][ok])) if ok.sum() >= 5 else -6.0
    ex = {t: excess(t, [(o, ex[o] >= UNSURE_DB) for o in toms if o != t]) for t in toms}
    # several toms over the bar at the same instant: only the one furthest over stays SURE
    exm = np.stack([ex[t] for t in toms], 1)
    lead_tom = np.argmax(exm, 1)
    for t in toms:
        top = np.percentile(lv[t][L[:, I[t]] > 0], 99.5)
        for e in np.nonzero((ex[t] >= UNSURE_DB) & (lv[t] >= top - TOM_FLOOR_UNSURE))[0]:
            sure = ex[t][e] >= SURE_DB and lv[t][e] >= top - TOM_FLOOR_SURE and toms[lead_tom[e]] == t
            add(t, t, e, sure, f"{ex[t][e]:.0f} dB over expected bleed, {lv[t][e] - top:.0f} dB re loudest", {"excess_db": round(float(ex[t][e]), 1)})
        info[t] = {"bleed_ratio_db": {s: round(B[(s, t)], 1) for s in list(struck) + refs + [o for o in toms if o != t]}}

    # one label per drum per 20 ms; SURE beats UNSURE at the same hit
    hits.sort(key=lambda h: (h["drum"], h["sample"]))
    out = []
    for h in hits:
        if out and out[-1]["drum"] == h["drum"] and h["sample"] - out[-1]["sample"] < int(0.020 * sr):
            if h["sure"] and not out[-1]["sure"]: out[-1] = h
            continue
        out.append(h)
    out.sort(key=lambda h: (h["sample"], h["drum"]))
    return {"sr": sr, "length": len(song[kick_mic]), "hits": out, "how": info}, song, sr


def summary(res):
    c = {}
    for h in res["hits"]:
        s = c.setdefault(h["drum"], [0, 0]); s[0 if h["sure"] else 1] += 1
    return {d: f"{v[0]} sure / {v[1]} unsure" for d, v in c.items()}


if __name__ == "__main__":
    root, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    sess = os.path.basename(os.path.normpath(root))
    for song_name in sorted(os.listdir(root)):
        p = os.path.join(root, song_name)
        if not os.path.isdir(p): continue
        res, _, _ = label_song(p)
        with open(os.path.join(out, f"{sess}_{song_name}.json"), "w") as f: json.dump(res, f, indent=0)
        print(f"{sess}/{song_name}: {summary(res)}", flush=True)
