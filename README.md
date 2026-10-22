# The Gate (Norka DSP)

A source-aware gate for live use and mixing. The full plan is in [ROADMAP.md](ROADMAP.md).

**Current stage: Phase 1, the gate core.** A pro-grade gate with lookahead, sidechain, detection filters
and a tracking high cut. The source models, spectral decay, leveler, Micro Glow and the instance network
come in the next phases and plug into this core.

## Install

Download **TheGate-Installer.pkg** from the latest release: https://github.com/Harencorey/the-gate/releases/tag/latest
It isn't Apple-notarized yet, so the first time macOS blocks it: **System Settings > Privacy & Security > Open Anyway**
(or right-click the .pkg and choose **Open**). Rescan plug-ins and look for **Norka DSP > The Gate**.
Every past version stays downloadable at https://github.com/Harencorey/the-gate/releases.

## Controls

The window: a scrolling view on top, then **DETECT**, **ENVELOPE** and **KEY FILTERS**.
Hover any control for about 3 seconds for an explanation.

**The view** (about 3 seconds, newest on the right): the grey shape is the input, the brighter shape inside it is
what gets through, the **blue** line is the key level the detector hears, the **amber** line is the gate's gain,
green shading = gate open. The solid green line is the open threshold, the dashed one the close threshold.

| Control | What it does |
|---|---|
| **Threshold** | Key level that opens the gate (−80 to 0 dBFS). |
| **Hysteresis** | The gate closes only when the key falls this far below the threshold (0–20 dB), so a decaying hit doesn't chatter open and shut. Measured: a decay wobbling ±1.5 dB through the threshold opens 4 times with 0 dB, once with 6 dB. |
| **Lookahead** | Off, 2, 5, 10 or 20 ms. The gate sees the hit early and is fully open the moment the transient arrives. The latency is reported to the host (and kept in bypass). Changing it while playing fades out and back in over ~3 ms. Use **Off or 2 ms for live playing**; more for mixing. |
| **Attack** | Opening time (0.01–50 ms, raised-cosine ramp). With Lookahead at least as long as Attack, the ramp ends exactly on the transient's first sample, so the attack is never softened. |
| **Hold** | Time the gate stays open after the key drops below the close threshold. |
| **Release** | Time to close through the whole Range (a straight line in dB). |
| **Range** | How far it closes. All the way down (−∞) = fully shut; −20 dB lets some room through. |
| **Key HPF** | High-pass on the key only (24 dB/oct, 10 Hz = Off): kick and bass bleed can't open the gate. The audio is untouched. |
| **High Cut** | Low-pass on the key only (24 dB/oct, top = Off): cymbals can't open the gate. |
| **Tracking** | The high cut follows each hit: wide open on the strike (so the stick attack opens the gate), then closing as the hit decays, so cymbal wash in the tail can't hold the gate open. 100 % = one octave down per 12 dB of decay (to 300 Hz at most). A new hit re-opens it, even a soft one right after a loud one. Works with the High Cut off too (starts from 20 kHz). |
| **Key** | Internal (the gate's own input) or Sidechain. |
| **Listen Key** | Hear exactly what the detector hears, after the key filters. |

**How the detector works:** the key is summed to mono, filtered, and measured with a blend of a peak follower
(10 ms release) and a 3 ms RMS. Every hit is logged with its arrival time and level; later phases fill in pitch,
source confidence and which instance claimed it.

## Measured (offline tests, `tests/test_engine.cpp`, `tests/test_processor.cpp`)

- Range 0 dB, or fully open: output is the input delayed by the lookahead, **bit for bit** (44.1–192 kHz, every lookahead).
- Reported latency = measured latency; the first 2 ms of a transient pass untouched.
- No clicks: worst sample-to-sample jump stays within what the attack/release shape allows, on a steady sine, for attack 0.1–2 ms and release 5–500 ms.
- Hold adds exactly the set time; release reaches the range floor in the set time (±2 ms); the floor lands on the Range setting.
- Identical output for any block size (1 sample, 512, random), with every feature on.
- No heap allocations on the audio thread; stays finite through long silence and denormal-level input.
- Plug-in: mono and stereo, sidechain off/mono/stereo; the sidechain keys the gate; bypass keeps the latency; sessions restore every parameter.

## Build it yourself (Mac)

```bash
bash build_mac.sh                   # downloads JUCE 8.0.15 automatically
# or: bash build_mac.sh ~/path/to/JUCE
```

Needs the Xcode command line tools and CMake (`brew install cmake`). Builds a universal **AU, VST3 and Standalone**,
installs the plug-ins into `~/Library/Audio/Plug-Ins/`, and runs the tests. Logic: `auval -v aufx Gate Nrka`.

## Automated builds

Every push to `main` builds on a GitHub macOS runner (`.github/workflows/build-mac.yml`), the same pipeline as Glow and
Glitch Nasty: tests, universal-binary check, **TheGate-Installer.pkg**, a test install, Apple `auval` and pluginval at
strictness 10, then the **latest** release plus a permanent version release with that commit's notes.

## Files

- `Source/GateCore.h`: the whole audio path (detector, filters, tracking, envelope, lookahead, meters). Plain C++, shared with the tests.
- `Source/HitRecord.h`: the hit record every feature reads and writes.
- `Source/SpscFifo.h`: lock-free audio → UI queue.
- `Source/Params.h`: parameter IDs (don't rename after release: it breaks sessions).
- `Source/PluginProcessor.*`, `Source/PluginEditor.*`: JUCE wrapper and UI.
- `tests/`: engine and plug-in tests.
- The plug-in code is `Gate` (manufacturer `Nrka`): keep it forever.
