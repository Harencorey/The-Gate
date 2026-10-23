"""Writes hit labels as a MIDI file (GM drum map) to line up under the tracks in a DAW.
SURE hits velocity 110, UNSURE velocity 35 (shows as faint notes). Tempo 120 BPM, so 1 beat = 0.5 s;
drop it at the session start (bar 1) with the project tempo set to 120 or 'import tempo' on.
Usage: python3 to_midi.py labels.json out.mid"""
import json, struct, sys

NOTE = {"kick": 36, "snare": 38, "rack1": 48, "rack2": 47, "floor1": 43, "floor2": 41, "hat": 42, "ride": 51, "cymbal": 49}
PPQ, US_PER_BEAT = 960, 500000          # 120 BPM -> 1920 ticks per second


def vlq(n):
    b = [n & 0x7F]
    while n > 127:
        n >>= 7; b.insert(0, (n & 0x7F) | 0x80)
    return bytes(b)


def write(labels, path):
    evs = []
    for h in labels["hits"]:
        if h["drum"] not in NOTE: continue
        t = int(round(h["time"] * 2 * PPQ))
        v = 110 if h["sure"] else 35
        evs += [(t, 1, NOTE[h["drum"]], v), (t + 60, 0, NOTE[h["drum"]], 0)]
    evs.sort(key=lambda e: (e[0], e[1]))
    trk = bytearray(b"\x00\xff\x51\x03" + US_PER_BEAT.to_bytes(3, "big"))
    trk += b"\x00\xff\x03" + vlq(len("The Gate labels")) + b"The Gate labels"
    now = 0
    for t, on, n, v in evs:
        trk += vlq(t - now) + bytes([0x99 if on else 0x89, n, v]); now = t
    trk += b"\x00\xff\x2f\x00"
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 0, 1, PPQ) + b"MTrk" + struct.pack(">I", len(trk)) + trk)


if __name__ == "__main__":
    write(json.load(open(sys.argv[1])), sys.argv[2])
