"""Leave-one-song-out evaluation of the 'is it my drum?' classifier, per mic.
Reports, pooled over held-out songs: of this drum's real hits, the share accepted while letting through
at most 1% / 0.3% of the bleed events; and the same for a baseline that only knows the event's level and
its energy at the fundamental (about what the Source key knows)."""
import sys, json
import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import make_pipeline
from sklearn.metrics import roc_auc_score
import hit_dataset as hd

SESS, LAB = "/home/claude/gate-data/session01", "/home/claude/gate-data/labels"
SONGS = [f"song{i}" for i in range(1, 8)]
DRUMS = {"rack1": ("rack1", 141), "rack2": ("rack2", 105), "floor1": ("floor1", 82), "floor2": ("floor2", 59), "snare": ("snare_top", 527)}

def recall_at(scores, y, fpr):
    neg = np.sort(scores[y == 0])[::-1]
    k = int(np.floor(fpr * len(neg)))
    thr = neg[k] if k < len(neg) else -np.inf
    return np.mean(scores[y == 1] > thr)

for drum, (mic, f0) in DRUMS.items():
    if len(sys.argv) > 1 and drum not in sys.argv[1:]: continue
    per = {s: hd.build(SESS, LAB, "session01", drum, mic, [s], f0=f0) for s in SONGS}
    res = {"full": [], "lr": [], "mlp": [], "base": []}; ys = []
    for test in SONGS:
        Ftr = {k: np.concatenate([per[s][0][k] for s in SONGS if s != test]) for k in per[test][0]}
        ytr = np.concatenate([per[s][1] for s in SONGS if s != test])
        Fte, yte, _ = per[test]
        if yte.sum() == 0: continue
        prof_src, prof_bleed = Ftr["shape"][ytr == 1].mean(0), Ftr["shape"][ytr == 0].mean(0)
        lvl_ref = np.median(Ftr["lvl"][ytr == 1])
        Xtr, Xte = hd.finish(Ftr, prof_src, prof_bleed, lvl_ref), hd.finish(Fte, prof_src, prof_bleed, lvl_ref)
        w = np.where(ytr == 1, (ytr == 0).sum() / max(1, ytr.sum()), 1.0)
        hgb = HistGradientBoostingClassifier(max_iter=200, learning_rate=0.08, max_leaf_nodes=15, l2_regularization=1.0)
        hgb.fit(Xtr, ytr, sample_weight=w); res["full"].append(hgb.predict_proba(Xte)[:, 1])
        lr = make_pipeline(StandardScaler(), LogisticRegression(C=0.3, class_weight="balanced", max_iter=2000))
        lr.fit(Xtr, ytr); res["lr"].append(lr.predict_proba(Xte)[:, 1])
        mlp = make_pipeline(StandardScaler(), MLPClassifier((24,), alpha=1e-2, max_iter=600, random_state=0))
        # oversample positives for the MLP
        idx = np.r_[np.nonzero(ytr == 0)[0], np.repeat(np.nonzero(ytr == 1)[0], int(w.max()))]
        mlp.fit(Xtr[idx], ytr[idx]); res["mlp"].append(mlp.predict_proba(Xte)[:, 1])
        bcols = np.c_[Ftr["lvl"] - lvl_ref, Ftr["f0"], Ftr["atk"]], np.c_[Fte["lvl"] - lvl_ref, Fte["f0"], Fte["atk"]]
        base = make_pipeline(StandardScaler(), LogisticRegression(class_weight="balanced", max_iter=2000)).fit(bcols[0], ytr)
        res["base"].append(base.predict_proba(bcols[1])[:, 1])
        ys.append(yte)
    y = np.concatenate(ys)
    line = f"{drum:7s} {int(y.sum()):4d} hits / {int((y == 0).sum()):5d} bleed events |"
    for k, name in (("base", "level+f0+attack"), ("lr", "linear"), ("mlp", "small net"), ("full", "boosted trees")):
        s = np.concatenate(res[k])
        line += f" {name}: AUC {roc_auc_score(y, s):.3f}, hits kept @1% bleed {100*recall_at(s, y, 0.01):5.1f}% @0.3% {100*recall_at(s, y, 0.003):5.1f}% |"
    print(line, flush=True)
