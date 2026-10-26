"""Pooled real-track evaluation of the engine, per drum (one threshold for the whole session, the way
a user sets it). For every threshold, over all songs of the session:
  passed   share of SURE hits where the gate is open: output within 3 dB of input over the first 10 ms
  leak     share of the time AWAY from this drum's hits (sure or unsure, -5..+400 ms) the gate is open
           (gain > -20 dB): bleed getting through
  ring     (Song 1 intro, 3-14.8 s: floor toms ringing off the kick, confirmed by Corey) time open
Then at the chosen threshold (most passed with leak <= 2%), spectral decay off vs on in the SURE hits'
tails (30-300 ms): wash = 4-12 kHz energy change, body = 40-300 Hz energy change.
Usage: python3 eval_pooled.py <RealRun> <labels_dir> <session> <data_dir> <out_json>"""
import json, os, subprocess, sys, glob
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from scipy.signal import butter, sosfilt
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatedata as gd

BIN, LABELS, SESS, DATA, OUT = sys.argv[1:6]
EXTRA = sys.argv[6:]   # extra engine settings for every run (e.g. src=1)
WORK = "/home/claude/rr/work" + ("_" + "_".join(a.replace("=", "") for a in sys.argv[6:]) if len(sys.argv) > 6 else ""); os.makedirs(WORK, exist_ok=True)
MIC = {"snare": "snare_top", "rack1": "rack1", "rack2": "rack2", "floor1": "floor1", "floor2": "floor2"}
if os.environ.get("DRUMS"): MIC = {d: m for d, m in MIC.items() if d in os.environ["DRUMS"].split(",")}
BASE = ["hold=30", "rel=250", "la=5", "hpf=50", "range=-80"]
THRS = list(range(int(os.environ.get("THR_LO", "-42")), -5, 3))
SR = 48000


def gain_db(x, y):
    """Per-ms gate gain estimate: output vs input energy in 1 ms frames (only where input is audible)."""
    n = len(x) // 48 * 48
    ex = (x[:n].astype(np.float64) ** 2).reshape(-1, 48).sum(1); ey = (y[:n].astype(np.float64) ** 2).reshape(-1, 48).sum(1)
    g = 10 * np.log10((ey + 1e-20) / (ex + 1e-20))
    g[ex < 1e-9] = np.nan          # input below -60 dBFS: gain undefined
    return g


def run(inp, tag, args):
    out = f"{WORK}/{tag}.out.f32"
    subprocess.run([BIN, inp, out, f"{WORK}/{tag}.csv", str(SR)] + BASE + EXTRA + args, check=True, capture_output=True)
    y = np.fromfile(out, np.float32); os.remove(out); os.remove(f"{WORK}/{tag}.csv")
    return y


def tails(y, starts, lo, hi):
    z = sosfilt(butter(4, [lo, hi], "band", fs=SR, output="sos"), y.astype(np.float64)) ** 2
    return float(sum(z[s + int(0.03 * SR): s + int(0.3 * SR)].sum() for s in starts))


def song_eval(args):
    song, drum = args
    lab = json.load(open(f"{LABELS}/{SESS}_{song}.json"))
    p = f"{DATA}/{song}/{MIC[drum]}.wav"
    x, sr = gd.load(p)
    sure = np.array([h["sample"] for h in lab["hits"] if h["drum"] == drum and h["sure"]], np.int64)
    anyl = np.array([h["sample"] for h in lab["hits"] if h["drum"] == drum], np.int64)
    away = np.ones(len(x) // 48, bool)
    for s in anyl: away[max(0, (s - 240) // 48): (s + int(0.4 * SR)) // 48] = False
    intro = np.zeros_like(away)
    if song == "song1" and drum in ("floor1", "floor2"): intro[3000:14800] = True
    inp = f"{WORK}/{song}_{drum}.in.f32"; x.astype(np.float32).tofile(inp)
    rows = []
    for t in THRS:
        y = run(inp, f"{song}_{drum}_{t}", [f"thr={t}"])
        g = gain_db(x, y)
        passed = sum(1 for s in sure if np.nanmean(g[s // 48: s // 48 + 10]) >= -3) if len(sure) else 0
        a = away & ~np.isnan(g)
        rows.append({"thr": t, "passed": passed, "away_ms": int(a.sum()), "open_away_ms": int((g[a] > -20).sum()),
                     "intro_open_ms": int((g[intro & ~np.isnan(g)] > -20).sum()), "intro_ms": int((intro & ~np.isnan(g)).sum())})
    return song, drum, len(sure), inp, sure, rows


songs = sorted(d for d in os.listdir(DATA) if os.path.isdir(f"{DATA}/{d}"))
jobs = [(s, d) for s in songs for d in MIC if os.path.exists(f"{DATA}/{s}/{MIC[d]}.wav")]
res = {}
with ThreadPoolExecutor(2) as ex:
    for song, drum, n, inp, sure, rows in ex.map(song_eval, jobs):
        res.setdefault(drum, []).append((song, n, inp, sure, rows))

report = {}
for drum, per in res.items():
    tot = sum(n for _, n, _, _, _ in per)
    table = []
    for k, t in enumerate(THRS):
        passed = sum(r[k]["passed"] for *_, r in per); away = sum(r[k]["away_ms"] for *_, r in per)
        oa = sum(r[k]["open_away_ms"] for *_, r in per)
        io = sum(r[k]["intro_open_ms"] for *_, r in per); im = sum(r[k]["intro_ms"] for *_, r in per)
        table.append({"thr": t, "passed_pct": round(100 * passed / max(tot, 1), 1), "leak_pct": round(100 * oa / max(away, 1), 2),
                      "ring_open_pct": round(100 * io / im, 1) if im else None})
    ok = [r for r in table if r["leak_pct"] <= 2.0]
    best = max(ok, key=lambda r: (r["passed_pct"], -r["leak_pct"])) if ok else min(table, key=lambda r: r["leak_pct"])
    # spectral decay off vs on at that threshold, pooled over songs
    w_off = w_on = b_off = b_on = 0.0
    for song, n, inp, sure, _ in per:
        if n == 0: continue
        yo = run(inp, f"{song}_{drum}_off", [f"thr={best['thr']}"])
        yn = run(inp, f"{song}_{drum}_on", [f"thr={best['thr']}", "spec=1", "focus=60", "sustain=0"])
        w_off += tails(yo, sure, 4000, 12000); w_on += tails(yn, sure, 4000, 12000)
        b_off += tails(yo, sure, 40, 300); b_on += tails(yn, sure, 40, 300)
    report[drum] = {"sure_hits": tot, "chosen": best, "table": table,
                    "spectral_wash_db": round(10 * np.log10(w_on / w_off), 2), "spectral_body_db": round(10 * np.log10(b_on / b_off), 2)}
    print(f"{drum:7s} {tot:4d} sure hits | thr {best['thr']} dBFS: passed {best['passed_pct']:5.1f}%, leak {best['leak_pct']:.2f}%"
          + (f", ring open {best['ring_open_pct']}%" if best['ring_open_pct'] is not None else "")
          + f" | spectral decay: wash {report[drum]['spectral_wash_db']:+.1f} dB, body {report[drum]['spectral_body_db']:+.1f} dB", flush=True)
    print("        sweep (thr: passed% / leak%): " + "  ".join(f"{r['thr']}: {r['passed_pct']:.0f}/{r['leak_pct']:.1f}" for r in table), flush=True)
for f_ in glob.glob(f"{WORK}/*.in.f32"): os.remove(f_)
json.dump(report, open(OUT, "w"), indent=1)
