import sys; sys.path.insert(0, "/home/claude/the-gate/training")
import hit_dataset as hd, numpy as np
from sklearn.neural_network import MLPClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import make_pipeline
from sklearn.metrics import roc_auc_score
LAB = "/home/claude/gate-data/labels"
S1 = ("/home/claude/gate-data/session01", "session01", [f"song{i}" for i in range(1, 8)])
RK = ("/home/claude/gate-data/session02-rock", "session02-rock", ["song1"])
TOMS1 = {"rack1": 141, "rack2": 105, "floor1": 82, "floor2": 59}
def xy(sess, drum, f0):
    F, y, _ = hd.build(sess[0], LAB, sess[1], drum, drum, sess[2], f0=f0)
    ps, pb, lr = F["shape"][y == 1].mean(0), F["shape"][y == 0].mean(0), np.median(F["lvl"][y == 1])
    X = hd.finish(F, ps, pb, lr)
    # profile-relative only (drop the absolute band shapes, which are kit-specific)
    return X[:, 2 * hd.NB:], y
Xs, ys = zip(*[xy(S1, d, f) for d, f in TOMS1.items()])
Xtr, ytr = np.vstack(Xs), np.concatenate(ys)
idx = np.r_[np.nonzero(ytr == 0)[0], np.repeat(np.nonzero(ytr == 1)[0], 50)]
net = make_pipeline(StandardScaler(), MLPClassifier((16,), alpha=1e-2, max_iter=800, random_state=0)).fit(Xtr[idx], ytr[idx])
lin = make_pipeline(StandardScaler(), LogisticRegression(class_weight="balanced", max_iter=3000)).fit(Xtr, ytr)
# Rock kit: its own fundamentals, learned from its own hits (what the plug-in would learn)
import json, gatedata as gd
for d in ("rack1", "floor1", "floor2"):
    lab = json.load(open(f"{LAB}/session02-rock_song1.json"))
    x, _ = gd.load(f"{RK[0]}/song1/{d}.wav")
    sure = [h["sample"] for h in lab["hits"] if h["drum"] == d and h["sure"]]
    S = sum(np.abs(np.fft.rfft(x[s + 480: s + 480 + 8192] * np.hanning(8192))) ** 2 for s in sure if s + 8672 < len(x))
    f = np.fft.rfftfreq(8192, 1 / 48000); band = (f > 40) & (f < 400)
    f0 = float(f[band][np.argmax(S[band])])
    X, y = xy(RK, d, f0)
    for name, m in (("small net", net), ("linear", lin)):
        s = m.predict_proba(X)[:, 1]
        neg = np.sort(s[y == 0])[::-1]
        k1, k3 = int(0.01 * len(neg)), int(0.003 * len(neg))
        print(f"Rock {d} (f0 {f0:.0f} Hz, {int(y.sum())} hits / {int((y==0).sum())} bleed) {name}: AUC {roc_auc_score(y, s):.3f}, hits kept @1% bleed {100*np.mean(s[y==1] > neg[k1]):.0f}%, @0.3% {100*np.mean(s[y==1] > neg[k3]):.0f}%")
