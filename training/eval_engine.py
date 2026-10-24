"""Runs the plug-in engine (tools/RealRun) over real tracks and scores it against the SURE labels.

Per drum and song, sweeps the threshold and reports:
  recall      share of SURE hits the gate opens on (a gate hit within -6..+3 ms of the label)
  false opens gate hits with no label of that drum (sure or unsure) within 10 ms: bleed openings
Then, at each drum's best threshold, compares spectral decay off/on in the tails of SURE hits:
  wash        4-12 kHz energy 30-300 ms after the hit (cymbal wash in the tom mic): lower = better
  body        energy at 40-300 Hz in the same window (the drum's own ring): should stay put
Usage: python3 eval_engine.py <RealRun> <labels_dir> <out_json>
"""
import json, os, subprocess, sys, glob
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from scipy.signal import butter, sosfilt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatedata as gd

BIN, LABELS, OUT = sys.argv[1], sys.argv[2], sys.argv[3]
WORK = "/home/claude/rr/work"
DATA = {"session01": "/home/claude/gate-data/session01", "session02-rock": "/home/claude/gate-data/session02-rock"}
MIC = {"snare": "snare_top", "rack1": "rack1", "rack2": "rack2", "floor1": "floor1", "floor2": "floor2"}
BASE = ["hold=30", "rel=250", "la=5", "hpf=50", "range=-80"]
THRS = list(range(-45, -5, 3))


def run(inp, tag, args):
    out, hits = f"{WORK}/{tag}.out.f32", f"{WORK}/{tag}.csv"
    subprocess.run([BIN, inp, out, hits, "48000"] + BASE + args, check=True, capture_output=True)
    h = np.loadtxt(hits, delimiter=",", skiprows=1, ndmin=2)
    return (h[:, 0].astype(np.int64) if len(h) else np.zeros(0, np.int64)), out


def score(gate, sure, anyl, sr=48000):
    lo, hi = int(0.006 * sr), int(0.003 * sr)
    rec = sum(1 for s in sure if np.any((gate >= s - lo) & (gate <= s + hi)))
    fo = sum(1 for g in gate if not np.any(np.abs(anyl - g) <= int(0.010 * sr)))
    return rec, fo


def band_energy(y, starts, sr, lo_hz, hi_hz, a_ms=30, b_ms=300):
    sos = butter(4, [lo_hz, hi_hz], "band", fs=sr, output="sos")
    z = sosfilt(sos, y) ** 2
    a, b = int(a_ms * 1e-3 * sr), int(b_ms * 1e-3 * sr)
    return float(sum(z[s + a: s + b].sum() for s in starts))


jobs, results = [], {}
for lf in sorted(glob.glob(f"{LABELS}/*.json")):
    name = os.path.basename(lf)[:-5]
    sess, song = name.rsplit("_", 1)
    lab = json.load(open(lf))
    for drum, mic in MIC.items():
        p = f"{DATA[sess]}/{song}/{mic}.wav"
        if not os.path.exists(p): continue
        sure = np.array([h["sample"] for h in lab["hits"] if h["drum"] == drum and h["sure"]], np.int64)
        anyl = np.array([h["sample"] for h in lab["hits"] if h["drum"] == drum], np.int64)
        if len(sure) < 3: continue
        jobs.append((name, drum, p, sure, anyl))


def do(job):
    name, drum, p, sure, anyl = job
    x, sr = gd.load(p)
    inp = f"{WORK}/{name}_{drum}.in.f32"
    x.astype(np.float32).tofile(inp)
    sweep = []
    for t in THRS:
        g, _ = run(inp, f"{name}_{drum}_t{t}", [f"thr={t}"])
        rec, fo = score(g, sure, anyl)
        sweep.append({"thr": t, "recall": rec / len(sure), "false_opens": fo, "gate_hits": len(g)})
    # best: most hits found with the fewest false opens (F-measure, false opens as false positives)
    def f1(r):
        tp = r["recall"] * len(sure); fp = r["false_opens"]; fn = len(sure) - tp
        return 2 * tp / max(2 * tp + fp + fn, 1)
    best = max(sweep, key=f1)
    t = best["thr"]
    _, off = run(inp, f"{name}_{drum}_off", [f"thr={t}"])
    _, on = run(inp, f"{name}_{drum}_on", [f"thr={t}", "spec=1", "focus=60", "sustain=0"])
    yo, yn = np.fromfile(off, np.float32).astype(np.float64), np.fromfile(on, np.float32).astype(np.float64)
    wash = 10 * np.log10(band_energy(yn, sure, sr, 4000, 12000) / max(band_energy(yo, sure, sr, 4000, 12000), 1e-20))
    body = 10 * np.log10(band_energy(yn, sure, sr, 40, 300) / max(band_energy(yo, sure, sr, 40, 300), 1e-20))
    for f_ in glob.glob(f"{WORK}/{name}_{drum}*"): os.remove(f_)
    return name, drum, {"sure_hits": int(len(sure)), "sweep": sweep, "best": dict(best, f1=round(f1(best), 3)),
                        "spectral_wash_change_db": round(wash, 2), "spectral_body_change_db": round(body, 2)}


with ThreadPoolExecutor(2) as ex:
    for name, drum, r in ex.map(do, jobs):
        results.setdefault(name, {})[drum] = r
        b = r["best"]
        print(f"{name:22s} {drum:7s} sure {r['sure_hits']:4d} | best thr {b['thr']:4d} dBFS: recall {100*b['recall']:5.1f}%, "
              f"false opens {b['false_opens']:4d} (F1 {b['f1']:.2f}) | spectral: wash {r['spectral_wash_change_db']:+.1f} dB, body {r['spectral_body_change_db']:+.1f} dB", flush=True)
json.dump(results, open(OUT, "w"), indent=1)
