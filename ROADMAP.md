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
