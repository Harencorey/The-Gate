# The Gate (Norka DSP)

A source-aware gate for live use and mixing. The full plan is in [ROADMAP.md](ROADMAP.md).

**Current stage: Phase 2.** A pro-grade gate (lookahead, sidechain, detection filters, tracking high cut)
plus the **Leveler** (evens out soft and loud hits) and **Micro Glow** (Glow's saturation and a clipper at the
output). The spectral decay, source models and instance network come next and plug into this core.

## Install

Download **TheGate-Installer.pkg** from the latest release: https://github.com/Harencorey/the-gate/releases/tag/latest
It isn't Apple-notarized yet, so the first time macOS blocks it: **System Settings > Privacy & Security > Open Anyway**
(or right-click the .pkg and choose **Open**). Rescan plug-ins and look for **Norka DSP > The Gate**.
Every past version stays downloadable at https://github.com/Harencorey/the-gate/releases.

## Controls

The window: a scrolling view on top, then **DETECT**, **ENVELOPE** and **KEY FILTERS**, then **LEVELER** and **MICRO GLOW**.
Hover any control for about 3 seconds for an explanation.

**The view** (about 3 seconds, newest on the right): the grey shape is the input, the brighter shape inside it is
what gets through, the **blue** line is the key level the detector hears, the **amber** line is the gate's gain,
green shading = gate open. The solid green line is the open threshold, the dashed one the close threshold.
Violet triangles mark hits the leveler moved (up = turned up, down = turned down).

| Control | What it does |
|---|---|
| **Threshold** | Key level that opens the gate (−80 to 0 dBFS). |
| **Hysteresis** | The gate closes only when the key falls this far below the threshold (0–20 dB), so a decaying hit doesn't chatter open and shut. Measured: a decay wobbling ±1.5 dB through the threshold opens 4 times with 0 dB, once with 6 dB. |
| **Lookahead** | Off, 2, 5, 10 or 20 ms. The gate sees the hit early and is fully open the moment the transient arrives. Changing it while playing fades out and back in over ~3 ms. Use **Off or 2 ms for live playing**; more for mixing (and 5 ms+ for the leveler). Total latency = lookahead + 1 ms (Micro Glow's filters, always there so switching Micro Glow never shifts timing); reported to the host and kept in bypass. |
| **Attack** | Opening time (0.01–50 ms, raised-cosine ramp). With Lookahead at least as long as Attack, the ramp ends exactly on the transient's first sample, so the attack is never softened. |
| **Hold** | Time the gate stays open after the key drops below the close threshold. |
| **Release** | Time to close through the whole Range (a straight line in dB). |
| **Range** | How far it closes. All the way down (−∞) = fully shut; −20 dB lets some room through. |
| **Key HPF** | High-pass on the key only (24 dB/oct, 10 Hz = Off): kick and bass bleed can't open the gate. The audio is untouched. |
| **High Cut** | Low-pass on the key only (24 dB/oct, top = Off): cymbals can't open the gate. |
| **Tracking** | The high cut follows each hit: wide open on the strike (so the stick attack opens the gate), then closing as the hit decays, so cymbal wash in the tail can't hold the gate open. 100 % = one octave down per 12 dB of decay (to 300 Hz at most). A new hit re-opens it, even a soft one right after a loud one. Works with the High Cut off too (starts from 20 kHz). |
| **Key** | Internal (the gate's own input) or Sidechain. |
| **Listen Key** | Hear exactly what the detector hears, after the key filters. |

### Leveler (on/off)

Evens out soft and loud hits, mainly for drums. Each hit's peak is measured during the lookahead, *before*
the hit reaches the output; its gain is set and glides into place just before the hit's first sample, then
holds through the whole decay. So the transient is never ramped and the tail keeps its hit's level.

| Control | What it does |
|---|---|
| **Amount** | How much of the gap to the target each hit closes. 100 % = every hit lands on the target; 50 % = halfway. |
| **Target** | The peak level hits are evened towards. Bottom = **Auto**: the running average of the last ~8 hits, so the overall level stays where it is and only the hit-to-hit differences shrink. |
| **Max** | The most any one hit is turned up or down (±0–24 dB). |

Every stroke counts as a hit, even in a roll or a flam where the gate never closes: strokes are found on the
key's slope (its high-frequency content), so a ghost note sitting just above the last hit's ringing tail is
still picked out and timed to the sample. With 5 ms lookahead the leveler judges each hit by its first
3.5 ms; with 2 ms, its first 1.25 ms; with Lookahead Off it measures the first 1 ms and the gain lands 2 ms
into the hit (so the attack is shaped a little).

### Micro Glow (on/off)

Glow's saturation at the very end, after the gate and leveler: one knob and a type.
Oversampled to ~384 kHz like Glow (8x at 44.1/48k) with **linear-phase** filters and no DC blocker, so it
adds **no phase shift** below the curve, important in a plug-in that keeps mics phase-aligned.
The output is **loudness-matched** like Glow's Auto Gain: Drive changes the color and density, not the level.

| Control | What it does |
|---|---|
| **Type** | **Tube**: Glow's tube output stage. The whole sound thickens, softer and rounder as Drive goes up (a little 2nd harmonic from Glow's measured asymmetry). **Iron**: Glow's transformer core. Only the lows (below ~126 Hz) saturate and compress; mids and highs pass clean. |
| **Drive** (center knob) | How hard it's hit, on Glow's Drive scale (0–100, default 25). Tube follows Glow's measured Drive map exactly. Iron's knob covers Glow's Drive 0–45: its harmonics peak about 10 dB into the core, and past that the lows just drop out, so the whole knob stays on the useful side. Double-click: 25. |
| **Env** (outer ring) | Glow's envelope follower, at Glow's default speed: hits that rise above the track's own ~3 s average push Drive up (right of center) or pull it back (left), 4 Drive units per dB at 100 %, always returning to exactly the knob setting. A violet dot on the knob shows where Drive is right now. Double-click: off. |

On/Off and Type changes crossfade (5 / 10 ms). Off is bit-exact. The 1 ms of latency is there on or off.

**How the detector works:** the key is summed to mono, filtered, and measured with a blend of a peak follower
(10 ms release) and a 3 ms RMS. Every hit is logged with its arrival time and level; later phases fill in pitch,
source confidence and which instance claimed it.

## Measured (offline tests, `tests/test_engine.cpp`, `tests/test_processor.cpp`)

Phase 2:
- Leveler, fixed target: every hit lands within 0.000 dB of where Amount/Max say it should (10 hits spread over 21 dB).
- Leveler Auto: 40 random hits spread 11.2 dB → 2.1 dB, overall level unchanged (−8.4 → −8.4 dBFS).
- The leveler gain is constant from the hit's first sample through its first 20 ms (2, 5, 10, 20 ms lookahead): no ramp on the transient.
- A 16-stroke roll (40 ms apart, 13 dB loud/soft): 16 hits found, leveled strokes within 0.00 dB of each other.
- Leveler gain changes and On/Off toggling: click-free. Leveler Off is bit-identical.
- Micro Glow below the curve nulls against the delayed input at −74 dB (Tube) / −78 dB (Iron), 40 Hz–18 kHz, 44.1/48/96 kHz.
- Drive 0 → 100 on a −6 dBFS tone: Tube THD −48 → −8 dB, Iron −35 → −18 dB, rising at every step; level held within 0.00 dB by the auto gain.
- Iron at Drive 75: 60 Hz THD −18 dB, 2 kHz THD −77 dB (lows only). Both types make 2nd harmonic (−49 / −40 dBc).
- Aliasing, −6 dBFS: Drive 50 −86 to −122 dBc (vs −35 to −55 with no oversampling); Drive 100 Tube −86 dBc at 2.1 kHz, −46 dBc on a pure 7 kHz tone; Iron −121 dBc everywhere.
- Envelope at +100 % moves Drive 40 → up to 90 and back; at −100 % down to 0 and back; at 0 it never moves.
- On/Off and Type switching: no step bigger than the signals' own steepness. CPU: ~2.5 % of one core per stereo instance at 48 kHz with everything on.

Phase 1:

- Range 0 dB, or fully open: output is the input delayed by the latency, **bit for bit** (44.1–192 kHz, every lookahead).
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

## Branches

Each phase is built on its own branch (`phase-2`, `phase-3`, ...) and merged into `main` when it's done;
`phase-1` is a snapshot of where Phase 1 finished. Every branch push builds and validates on a Mac and
publishes a **test build** (pre-release tagged `test-<branch>`) so it can be tried before merging.
`main` publishes the official **latest** release and a permanent version release.

## Automated builds

Every push to `main` builds on a GitHub macOS runner (`.github/workflows/build-mac.yml`), the same pipeline as Glow and
Glitch Nasty: tests, universal-binary check, **TheGate-Installer.pkg**, a test install, Apple `auval` and pluginval at
strictness 10, then the **latest** release plus a permanent version release with that commit's notes.

## Files

- `Source/GateCore.h`: the whole audio path (detector, filters, tracking, onsets, envelope, leveler, lookahead, meters). Plain C++, shared with the tests.
- `Source/MicroGlow.h`: Micro Glow (Glow's Tube and Iron, envelope follower, auto gain, oversampling). `Source/GlowDriveMap.h` is Glow's Drive map, copied verbatim; `Source/HalfbandCoeffs.h` is generated by `tools/design_halfband.py`.
- `Source/HitRecord.h`: the hit record every feature reads and writes.
- `Source/SpscFifo.h`: lock-free audio → UI queue.
- `Source/Params.h`: parameter IDs (don't rename after release: it breaks sessions).
- `Source/PluginProcessor.*`, `Source/PluginEditor.*`: JUCE wrapper and UI.
- `tests/`: engine and plug-in tests.
- The plug-in code is `Gate` (manufacturer `Nrka`): keep it forever.
