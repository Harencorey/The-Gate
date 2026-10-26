"""Session learning test: can the gate teach itself 'my drum vs bleed' from ONE mic, with no labels?

Per song and drum (the way the plugin would see it: learn while the track plays, then gate it):
  1. every event the gate would consider (same events and ground truth as hit_dataset / classifier_eval)
  2. SELF-labels, from this mic only:
       confident hits  = level within LOUD_DB of the loud cluster AND stick attack >= ATK_DB
       confident bleed = level more than QUIET_DB under the hit median
                         OR (level under the hit median by BLEEDATK_DB and no stick attack)
  3. a model fitted to those self-labels, scored on ALL events:
       'centroid'  correlation of the event's band shape with the hit vs bleed mean shapes
       'gauss'     diagonal-Gaussian log-likelihood ratio over shape/new/f0/attack
       'lda'       shrunk shared-covariance linear discriminant (a weight vector: one dot product per event)
Reported like classifier_eval: of the real hits, % kept while letting through <=1% / 0.3% of bleed.
Level is left OUT of the learned score on purpose: the gate's threshold already handles level, the learned
check must add something the threshold doesn't know.
Usage: python3 session_eval.py [drum ...]   (env SESSION=rock to run the Rock session)"""
import os, sys, json
import numpy as np
from sklearn.metrics import roc_auc_score
import hit_dataset as hd

LAB = "/home/claude/gate-data/labels"
if os.environ.get("SESSION") == "rock":
    SESS, SNAME, SONGS = "/home/claude/gate-data/session02-rock", "session02-rock", ["song1"]
else:
    SESS, SNAME, SONGS = "/home/claude/gate-data/session01", "session01", [f"song{i}" for i in range(1, 8)]
DRUMS = {"rack1": ("rack1", 141), "rack2": ("rack2", 105), "floor1": ("floor1", 82), "floor2": ("floor2", 59), "snare": ("snare_top", 527)}
LOUD_DB, ATK_DB, QUIET_DB, BLEEDATK_DB = float(os.environ.get("LOUD_DB", 10)), 6.0, 24.0, 12.0


def recall_at(scores, y, fpr):
    neg = np.sort(scores[y == 0])[::-1]
    k = int(np.floor(fpr * len(neg)))
    thr = neg[k] if k < len(neg) else -np.inf
    return np.mean(scores[y == 1] > thr)


TAUGHT = int(os.environ.get("TAUGHT", 0))

def self_labels(F, y=None):
    lvl, atk = F["lvl"], F["atk"]
    if TAUGHT and y is not None:
        # 'Learn' button: the user points the plugin at a few clean hits (here: the song's first TAUGHT real hits)
        hit = np.zeros(len(y), bool); hit[np.nonzero(y == 1)[0][:TAUGHT]] = True
        med = np.median(lvl[hit])
        bleed = (lvl < med - QUIET_DB) | ((lvl < med - BLEEDATK_DB) & (atk < 3.0))
        return hit, bleed & ~hit
    # pick hits by level IN THE FUNDAMENTAL BAND, not overall level: on a tom mic the snare/kick bleed is
    # often louder overall, but only the tom puts that much energy at the tom's own fundamental
    fab = lvl + F["f0"]
    top = np.percentile(fab, 99.5)
    hit = (fab >= top - LOUD_DB) & (atk >= ATK_DB) & (F["f0"] >= np.median(F["f0"]))
    med = np.median(lvl[hit]) if hit.sum() else top
    bleed = (lvl < med - QUIET_DB) | ((lvl < med - BLEEDATK_DB) & (atk < 3.0))
    return hit, bleed & ~hit


def feats(F):
    return np.hstack([F["shape"], F["new"], F["f0"][:, None], F["atk"][:, None]])


def corr(a, b):
    a = a - a.mean(1, keepdims=True); b = b - b.mean()
    return (a @ b) / (np.linalg.norm(a, axis=1) * np.linalg.norm(b) + 1e-9)


def models(F, hit, bleed):
    X = feats(F)
    out = {"centroid": corr(F["shape"], F["shape"][hit].mean(0)) - corr(F["shape"], F["shape"][bleed].mean(0))}
    mu1, mu0 = X[hit].mean(0), X[bleed].mean(0)
    v1, v0 = X[hit].var(0) + 1.0, X[bleed].var(0) + 1.0
    out["gauss"] = (-0.5 * ((X - mu1) ** 2 / v1 + np.log(v1)) + 0.5 * ((X - mu0) ** 2 / v0 + np.log(v0))).sum(1)
    Xc = np.vstack([X[hit] - mu1, X[bleed] - mu0])
    S = np.cov(Xc.T); lam = 0.3
    S = (1 - lam) * S + lam * np.diag(np.diag(S)) + 1e-3 * np.eye(len(S))
    w = np.linalg.solve(S, mu1 - mu0)
    out["lda"] = X @ w
    return out


for drum, (mic, f0) in DRUMS.items():
    if len(sys.argv) > 1 and drum not in sys.argv[1:]: continue
    res, ys, base, info = {"centroid": [], "gauss": [], "lda": []}, [], [], []
    for song in SONGS:
        try:
            F, y, _ = hd.build(SESS, LAB, SNAME, drum, mic, [song], f0=f0)
        except (ValueError, IndexError, FileNotFoundError):
            continue
        if y.sum() == 0: continue
        hit, bleed = self_labels(F, y)
        if hit.sum() < 4 or bleed.sum() < 10: info.append(f"{song}: too few self-labels"); continue
        # how clean were the self-labels? (uses ground truth only to REPORT, not to learn)
        known = y >= 0
        info.append(f"{song}: {hit.sum()} self-hits ({100*np.mean(y[hit]==1):.0f}% real), {bleed.sum()} self-bleed ({100*np.mean(y[bleed]==0):.0f}% real)")
        for k, s in models(F, hit, bleed).items(): res[k].append(s)
        # baseline: what the Source key already knows (f0 band + attack) as a simple sum, same footing
        base.append(F["f0"] + 0.5 * F["atk"])
        ys.append(y)
    if not ys: print(f"{drum}: no data"); continue
    y = np.concatenate(ys)
    print(f"\n{drum}  ({int(y.sum())} hits / {int((y==0).sum())} bleed events)")
    for l in info: print("   ", l)
    # each song gets its own threshold (the plugin learns per track), so recall is per song, weighted by hits
    def pooled(lst, fpr):
        return sum(recall_at(s, yy, fpr) * yy.sum() for s, yy in zip(lst, ys)) / y.sum()
    for name, lst in [("f0+attack (Source key)", base)] + list(res.items()):
        auc = np.mean([roc_auc_score(yy, s) for s, yy in zip(lst, ys) if 0 < yy.sum() < len(yy)])
        print(f"   {name:24s} AUC {auc:.3f}  kept @1% bleed {100*pooled(lst, .01):5.1f}%  @0.3% {100*pooled(lst, .003):5.1f}%", flush=True)


# ---- what it does inside the gate: Source key alone vs Source key + learned check ----------------------
# Source key passes an event if its fundamental-band level is over the threshold and it has a stick attack.
# For each song, the user sets the threshold (and, with learning on, the Strictness) for the least bleed that
# still keeps >= KEEP of the real hits. Count the bleed events that get through.
def best_bleed(y, fab, atk, learned, keep):
    ok = atk >= ATK_DB
    need = int(np.ceil(keep * y.sum()))
    best = None
    lt = [None] if learned is None else list(np.unique(np.percentile(learned[y == 1], np.arange(0, 51, 2))))
    for l in lt:
        m = ok if l is None else ok & (learned >= l)
        hits = np.sort(fab[m & (y == 1)])[::-1]
        if len(hits) < need: continue
        thr = hits[need - 1]
        b = int(np.sum(m & (y == 0) & (fab >= thr)))
        best = b if best is None else min(best, b)
    return best

if os.environ.get("GATE", "1") == "1":
    KEEP = float(os.environ.get("KEEP", 0.9))
    print(f"\nInside the gate: bleed events let through while keeping {KEEP:.0%} of the real hits (per song, summed)")
    for drum, (mic, f0) in DRUMS.items():
        if len(sys.argv) > 1 and drum not in sys.argv[1:]: continue
        tot = {"src": 0, "src+lda": 0, "src+gauss": 0}; nh = nb = 0; skipped = 0
        for song in SONGS:
            try: F, y, _ = hd.build(SESS, LAB, SNAME, drum, mic, [song], f0=f0)
            except (ValueError, IndexError, FileNotFoundError): continue
            if y.sum() < 3: continue
            hit, bleed = self_labels(F, y)
            fab = F["lvl"] + F["f0"]
            a = best_bleed(y, fab, F["atk"], None, KEEP)
            if a is None: skipped += 1; continue   # the attack check alone already drops >10% of hits here
            if hit.sum() < 4 or bleed.sum() < 10:
                M = {"lda": None, "gauss": None}; skipped += 1
            else:
                M = models(F, hit, bleed)
            for k in ("lda", "gauss"):
                b = best_bleed(y, fab, F["atk"], M[k], KEEP) if M[k] is not None else a
                tot["src+" + k] += a if b is None else b
            tot["src"] += a if a is not None else 0
            nh += int(y.sum()); nb += int((y == 0).sum())
        print(f"  {drum:7s} {nh:5d} hits / {nb:5d} bleed | Source key {tot['src']:4d} | +learned (lda) {tot['src+lda']:4d} | +learned (gauss) {tot['src+gauss']:4d}" + (f"  ({skipped} songs skipped/unlearned)" if skipped else ""), flush=True)
