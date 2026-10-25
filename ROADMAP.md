# The Gate (Norka DSP): roadmap and design spec

The Gate is a source-aware gate. Instead of opening on level alone, it knows *what* it is gating
(a factory model or one you trained), uses the source's pitch, tone and timing, and works as a team
with every other instance in the session so bleed gets rejected. It is built for both live use and mixing.

The north star: a session-prep tool that hands you clean, phase-aligned, mix-ready tracks, reliably.
It ships in stages so every release is something usable in a real session.

## Decisions so far

- **Name:** The Gate (Norka DSP). Repo: `Harencorey/the-gate` (public, like Glow and Glitch Nasty).
- **Formats:** AU, VST3, Standalone (multi-input, so one app can gate a whole kit live). Mono and stereo, with an external sidechain input. ARA added in v2.
- **Modes:** live/real-time and mixing share one engine and one model library. ARA mode (v2) adds whole-file analysis and a review timeline.
- **v1 scope:** gate core, leveler (bypassable), Micro Glow clipper, spectral decay, factory models + user training with live learning, instance network, group phase alignment.
- **Later:** ARA mode, learnable spectral denoise and hum removal (live denoise only if CPU/latency allow).
- **Parked:** nothing else currently.

## Core architecture (built in from day one)

- **Lookahead, reported to the host.** The classifier and the spectral tail need a few ms to know what they are hearing. Optional longer lookahead (20-50 ms) for mixing.
- **The gate engine doesn't care where its decisions come from.** Live: the lookahead detector. ARA: a precomputed hit map. Same envelopes, same tail, same sound.
- **One "hit" record** that every feature reads and writes: time, level, pitch curve, class probabilities, confidence, which instance claimed it, original arrival time (before alignment), and distance from the expected bleed.
- **Real-time safe:** no allocations, locks, file I/O or logging on the audio thread. Learning and network traffic run off the audio thread through lock-free buffers.

## Status

- **Phase 0 + 1: done** (v0.1.3, Oct 22 2026): gate core, CI, installer.
- **Phase 2: done** (Oct 23 2026): leveler + Micro Glow. Decisions made while building:
  - Hits are every *stroke*, found on the key's slope (high-frequency content) rather than its level: sample-accurate even for ghost notes on a ringing tail, and in rolls where the gate never closes.
  - Leveler gain is set from the peak seen during the lookahead and glides in *before* the hit; 5 ms+ lookahead recommended (sees 3.5 ms of each hit).
  - Micro Glow runs at ~384 kHz like Glow, linear-phase, no DC blocker (zero phase shift), types Tube / Diode / Clip. Its filters add a constant 1 ms of latency, always there so switching it never shifts timing.
- **Phase 2 update** (Oct 23): Micro Glow is one Drive knob with Glow's envelope follower as a ring around it (concentric pot) and a Tube/Iron dropdown; loudness-matched like Glow's Auto Gain. Clip, Diode and Ceiling removed.
- **Phase 3: reworked** (Oct 25) after Corey's listen on real drums (thin; too much high-end ring; transient definition lost). New design:
  - **Transient:** each hit stays exactly the plain gate for the source's learned attack length (the >1 kHz band falling 12 dB from its peak; median of recent hits; 12-26 ms on Corey's toms, 27-31 ms snare). Transient knob: half to double.
  - **Focus:** then a 4-pole low-pass glides down from the top as the hit's body decays (up to 1 octave per dB); the highs close first, the drum's tone fades like it does on its own.
  - **Sustain:** only the learned body band (fundamental up to ~360-1200 Hz) rings on; the highs never outlast the gate. Per-partial band-passes removed.
  - Measured on Corey's 7 songs vs the plain gate (30-300 ms after each hit, Focus 60): body below 500 Hz within 0.2 dB, 500 Hz-2 kHz within 0.7 dB, wash (4-12 kHz) -1.3 to -4.7 dB on toms, -3.1 dB snare. Bit-identical to the plain gate when the gains are equal.
  - Review loop: before/after clips of real fills rendered for listening before each build.
- Phase 3 first version (Oct 23), superseded; decisions that still stand:
  - Fixed narrow bands were tested and rejected: with zero latency and no phase shift they're too leaky. Instead the tail **learns the source's partials** (resonant filters locked to them) and **the bleed** (while the gate is shut); everything that isn't a partial closes faster (Focus), the partials can ring past the gate (Sustain). Bit-identical to the plain gate when the gains are equal.
  - Stroke detection hardened against bleed: a stroke needs a sharp edge that lifts the peak 4 dB over the tail, or a 6 dB jump in the smooth level (works under cymbal wash); the gate wobbling back open on a decaying note isn't a hit. A ghost note < ~4 dB over a ringing hit merges with it (Phases 4/6 resolve those).
  - Bleed floors use minimum tracking, so a source tail heard while the gate is shut isn't learned as bleed.
  - Attack cleanup (pulling a crash out of the first 5 ms) moves to Phase 5, where it can use the trained source profile.
- Branches: one per phase (`phase-1` snapshot, `phase-2`, `phase-3`); each branch push gets a Mac test build (`test-<branch>` pre-release); `main` gets the official releases.
- **Phase 4: started** on branch `phase-4` (Oct 23). Training data: Corey's session 1 (7 songs, one kit, 16 mics) and a Rock session (different kit; isolated guitar/bass/vocal overdubs, no instrument bleed in the drum mics). Hit labels are SURE/UNSURE (`training/label_hits.py`); MIDI exports for review by ear.
  - Labelling lessons: the kick trigger is a witness, not ground truth (built-in re-attack ~45 ms in, misses kicks, false-fires on toms); the hat mic hears the snare as loud as the snare mic; floor toms ring sympathetically off the kick (Corey confirmed, Song 1 intro); toms are judged in the stick band (>1.5 kHz) against bleed ratios from mic to mic.
  - **Baseline measured on the real tracks** (exact engine, one threshold per drum for the whole session, `training/eval_pooled.py`): snare 98% of hits pass with 0.8% leak. Toms at <= 2% leak pass only 47-79% of hits (rack 1 47%, rack 2 68%, floor 1 67%, floor 2 79%); passing 90%+ costs 8-17% leak, because snare bleed in the tom mics sits where the softer tom hits are. Floor toms ringing off the kick never open the gate. Spectral decay (Focus 60) cuts tail wash 2-6 dB on toms, 5.6 dB on snare, cost 1.5-2 dB of tom body; Corey heard it as thinner. Fixed (Oct 24): a learned body band (fundamental up to ~450-800 Hz) follows the gate; tom body now within 0.2-0.5 dB of the plain gate, wash cut unchanged.
  - **Phase 4 target:** toms >= 95% passed at <= 2% leak on these sessions (held-out songs).

## Phases

### Phase 0: Foundation
- JUCE 8 / CMake project, Norka DSP manufacturer code, lock the 4-letter plug-in code before any tester sees it.
- CI the same as Glow and Glitch Nasty: universal build, engine tests, auval, pluginval at strictness 10, `.pkg` installer, `latest` plus permanent version releases.
- This spec in the repo.

### Phase 1: Gate core
- Detector: peak/RMS blend, threshold with hysteresis (separate open/close), attack, hold, release, range.
- Lookahead with correct latency reporting.
- Detection filters: HPF plus the envelope-driven high cut (open on the transient, closing through the decay to starve cymbal bleed).
- External sidechain and a Listen (key) button.
- Metering: input waveform with threshold and gate state, gain reduction.
- Tests: bit-identical when fully open, reported latency equals measured latency, click-free edges on sine bursts, identical at any block size and sample rate, no allocations on the audio thread.

### Phase 2: Leveler and Micro Glow
- **Leveler** (bypassable): recognises soft and loud hits and evens them out, mainly for drums.
- **Micro Glow:** a clipper/small saturator at the output, derived from Glow's engine.

### Phase 3: Spectral decay
- Hybrid: broadband gate on the attack (no smear), crossfading a few ms later into a per-band gate on the tail.
- Multi-resolution: 8-12 wide fast bands near the attack, fine bands deep in the tail. Smoothing across time and neighbouring bands to avoid watery artefacts.
- Bands where the source lives ring out; bands that are bleed close fast. Starts from a spectral template; gets smarter as models and training arrive. Can follow the source's pitch as it drifts.
- Cymbal on the same instant as a hit: compare the attack against the source's learned clean attack, pull down bands with energy the source never makes, and limit how far the dynamic high cut opens.
- Proof: clean tom + real crash mixed at realistic bleed levels, nulled against the clean tom.

### Phase 4: Training data and factory models
- **Data rights first:** commercial-use licences for any public dataset; settle rights before using client recordings; best option is dedicated training sessions we record (several kits, rooms, mics, soft to hard hits, deliberate bleed).
- Small classifier trained offline (Python), run in real time with RTNeural.
- Features: tone, attack shape, decay, and a pitch curve (tracked per hit, since pitch glides and shifts with velocity). Pitch is evidence, not a hard rule.
- Factory sources selectable from a menu (kick, snare, toms, bass guitar, guitar...). A Strictness control sets how sure it must be before opening.

### Phase 5: User training and live learning
- Guided two-step capture: "learn the source", then "learn the bleed", feeling like one gesture.
- Live learning while engaged: keeps running summaries (not audio), fixed small memory over hours, learns only from very confident hits, with a Lock button.
- Three layers: **Factory** (never changes), **Your library** (named profile files in a Norka DSP folder, portable and shareable, with version history and rollback), **Session learning** (saved with the session; "save to library" to keep it).
- Anything shared back to improve factory models is opt-in only; nothing leaves the machine by default.

### Phase 6: Instance network
- Every instance posts hits to a shared board. Identification is global across the session (drums, bass, guitar...).
- A confident claim from one instance lowers others' confidence in the same hit: evidence, not a veto. Simultaneous hits still open when there is more energy than the learned bleed explains.
- Learns the kit's bleed map (arrival delay and level of each source into each mic).
- Roles: close mics claim hits and gate; overheads/rooms share timing but don't vote.
- Timing comparisons use original arrival times, before alignment.
- Shared info feeds the spectral tail (bands owned by another source close in this gate's tail).
- Graceful fallback when the host runs instances in separate processes.

### Phase 7: Group phase alignment
- Alignment is per group (drum group, bass group); identification stays global.
- Uses the same timing measurements as the instance network.

### Phase 8: v1 polish and beta
- UI mocked as real screenshots before anything is final (done per phase, not only here).
- Presets, A/B, undo; CPU profiling; hit history display for live; beta to friends once it is basically finished.

## v2 and later
- **ARA mode:** whole-file analysis, a review timeline showing confident / uncertain / rejected hits with probabilities and who claimed them, click-to-correct, corrections kept as session training. Must sit first in the chain; not every host supports ARA (verify the host list when we start).
- **Denoise:** noise print learned automatically from closed-gate gaps (plus a manual "learn noise"), a dedicated hum remover, spectral denoise sharing the spectral engine. Lighter version for live only if technology allows.

## Open items
- 4-letter plug-in code (proposed: `Gate`, manufacturer `Nrka`).
- Training data sources and rights (needed before Phase 4).
