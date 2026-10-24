"""Listening check for the tom labels: a stereo WAV per song.
  Left  = the tom mics mixed together (what was played)
  Right = a click at every tom hit the labeler found, pitched per tom
          (rack 1 highest ... floor 2 lowest); SURE hits loud, UNSURE hits quiet
Usage: python3 check_audio.py labels.json song_dir out.wav"""
import json, sys, os
import numpy as np
from scipy.io import wavfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gatedata as gd

PITCH = {"rack1": 1760.0, "rack2": 1320.0, "floor1": 880.0, "floor2": 587.0}
lab = json.load(open(sys.argv[1])); song_dir = sys.argv[2]
toms = [t for t in PITCH if os.path.exists(f"{song_dir}/{t}.wav")]
mix = None
for t in toms:
    x, sr = gd.load(f"{song_dir}/{t}.wav")
    mix = x.astype(np.float64) if mix is None else mix + x
mix *= 0.7 / (np.abs(mix).max() + 1e-12)
clicks = np.zeros_like(mix)
n = int(0.03 * sr); tt = np.arange(n) / sr
for h in lab["hits"]:
    if h["drum"] not in PITCH: continue
    amp = 0.6 if h["sure"] else 0.12
    c = amp * np.sin(2 * np.pi * PITCH[h["drum"]] * tt) * np.exp(-tt / 0.008)
    s = h["sample"]; clicks[s: s + n] += c[: len(clicks) - s]
from scipy.signal import resample_poly
# 24 kHz keeps the files small enough to send; nothing a tom check needs lives above 12 kHz
out = np.stack([resample_poly(mix, 1, 2), np.clip(resample_poly(clicks, 1, 2), -0.95, 0.95)], 1)
wavfile.write(sys.argv[3], sr // 2, (out * 32767).astype(np.int16))
