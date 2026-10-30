"""De-bleed on the real tracks: how much cymbal wash comes out of the toms' tails, and what it costs the drum.

For every SURE tom hit (Source key at the pooled-eval threshold), gate off vs De-bleed at each amount:
  attack     the first 10 ms, all frequencies (must be untouched: 0.0 dB)
  body       40-300 Hz, 30-300 ms after the hit (the drum's tone: should stay ~0 dB)
  wash       4-12 kHz, 30-300 ms after the hit (cymbal wash in the tail: should come down)
Hits are split by how much cymbal is in the room at that moment (the overheads' 4-12 kHz over the same
30-300 ms, re this song's median): 'cymbal-heavy' = top third, 'quiet cymbals' = bottom third. On quiet
ones the highs left are mostly the tom's own, so less should come off there.
Usage: python3 debleed_eval.py <RealRun> [amounts...]   (env DRUMS=rack1,... SONGS=song1,...)"""
import json, os, subprocess, sys
import numpy as np
from scipy.signal import butter, sosfilt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatedata as gd

RR = sys.argv[1]
AMTS = [float(a) for a in sys.argv[2:]] or [50.0, 100.0]
SESS, LAB, SR = "/home/claude/gate-data/session01", "/home/claude/gate-data/labels", 48000
THR = {"rack1": -33, "rack2": -33, "floor1": -33, "floor2": -42}
if os.environ.get("DRUMS"): THR = {d: t for d, t in THR.items() if d in os.environ["DRUMS"].split(",")}
SONGS = os.environ.get("SONGS", ",".join(f"song{i}" for i in range(1, 8))).split(",")
BASE = [*os.environ.get("EXTRA", "").split(), "hold=30", "rel=250", "la=15", "hpf=50", "range=-80", "src=1", "srcatk=6"]
WORK = "/home/claude/rr/db"; os.makedirs(WORK, exist_ok=True)


def band(x, lo, hi):
    return sosfilt(butter(4, [lo, hi], "band", fs=SR, output="sos"), x.astype(np.float64)) ** 2


def run(inp, args):
    subprocess.run([RR, inp, f"{WORK}/o.f32", f"{WORK}/h.csv", str(SR)] + BASE + args, check=True, capture_output=True)
    return np.fromfile(f"{WORK}/o.f32", np.float32)


for drum, thr in THR.items():
    rows = {a: [] for a in AMTS}
    for song in SONGS:
        p = f"{SESS}/{song}/{drum}.wav"
        if not os.path.exists(p): continue
        lab = json.load(open(f"{LAB}/session01_{song}.json"))
        sure = [h["sample"] for h in lab["hits"] if h["drum"] == drum and h["sure"]]
        if not sure: continue
        x, _ = gd.load(p); inp = f"{WORK}/in.f32"; x.astype(np.float32).tofile(inp)
        oh = sum(gd.load(f"{SESS}/{song}/{m}.wav")[0].astype(np.float64) for m in ("oh_l", "oh_r"))
        ohw = band(oh, 4000, 12000)
        cym = np.array([ohw[s + 1440: s + 14400].sum() for s in sure])
        y0 = run(inp, [f"thr={thr}"])
        w0, b0 = band(y0, 4000, 12000), band(y0, 40, 300)
        for a in AMTS:
            y = run(inp, [f"thr={thr}", f"debleed={a}"])
            w, b = band(y, 4000, 12000), band(y, 40, 300)
            for k, s in enumerate(sure):
                if s + 14400 > len(y): continue
                # only hits the gate lets through (a missed hit's "attack" is silence either way)
                if (y0[s: s + 480].astype(np.float64) ** 2).sum() < 0.5 * (x[s: s + 480].astype(np.float64) ** 2).sum(): continue
                att = 10 * np.log10((y[s: s + 480].astype(np.float64) ** 2).sum() / ((y0[s: s + 480].astype(np.float64) ** 2).sum() + 1e-20) + 1e-20)
                ws = 10 * np.log10((w[s + 1440: s + 14400].sum() + 1e-20) / (w0[s + 1440: s + 14400].sum() + 1e-20))
                # the stick: 2-12 kHz over the hit's first 10 ms (must be untouched too)
                hs = band(y[s - 480: s + 960], 2000, 12000)[480:960].sum(); hs0 = band(y0[s - 480: s + 960], 2000, 12000)[480:960].sum()
                att = min(att, 10 * np.log10((hs + 1e-20) / (hs0 + 1e-20)))
                bs = 10 * np.log10((b[s + 1440: s + 14400].sum() + 1e-20) / (b0[s + 1440: s + 14400].sum() + 1e-20))
                rows[a].append((att, bs, ws, np.log10(cym[k] + 1e-20) - np.log10(np.median(cym) + 1e-20)))
    for a in AMTS:
        r = np.array(rows[a])
        if not len(r): continue
        hi, lo = r[:, 3] >= np.percentile(r[:, 3], 67), r[:, 3] <= np.percentile(r[:, 3], 33)
        print(f"{drum:7s} De-bleed {a:3.0f}% | {len(r):3d} hits | attack {np.mean(r[:, 0]):+.2f} dB (worst {r[:, 0].min():+.2f})"
              f" | body {np.mean(r[:, 1]):+.2f} dB | wash: cymbal-heavy {np.mean(r[hi, 2]):+.1f} dB, quiet cymbals {np.mean(r[lo, 2]):+.1f} dB", flush=True)
