#pragma once
//==============================================================================
// The Gate - core gate engine (Phases 1-2). Plain C++, no JUCE, so the offline
// tests run the exact code the plug-in runs.
//
// Signal flow:
//
//   key (input or sidechain) -> mono -> key HPF (24 dB/oct) -> key high cut (24 dB/oct,
//        cutoff tracks each hit's decay) -> detector (peak/RMS blend) -> open/close with
//        hysteresis -> state delay (so the attack ramp ends exactly on the transient)
//        -> gain envelope (attack / hold / release / range)
//
//   audio -> lookahead delay -> x gate gain (or the spectral decay) x leveler gain -> Micro Glow -> out
//                                                    (Listen: the filtered key, Micro Glow skipped)
//
// Hits: a hit starts when the gate opens, or - while it's open - with each new stroke (a roll,
// a flam): a sharp edge in the key that also lifts its peak 4 dB over the ringing tail, or a
// 6 dB jump in its level (which still works under cymbal wash). A hi-hat tick riding on a tom's
// tail isn't a stroke; neither is a ghost note buried less than ~4 dB above a still-ringing hit
// (Phases 4 and 6 sort those out by what the sound is and which mic claims it).
// Every hit gets a Hit record.
//
// Leveler: each hit's audio peak is measured during the lookahead, before the hit reaches
// the output, and a gain for that hit is set so the hit lands nearer the target. The gain
// glides into place just BEFORE the hit's first sample and holds through its whole decay,
// so the transient itself is never ramped. Needs lookahead; with Lookahead Off it measures
// the first 1 ms and glides over the next 1 ms (the attack gets shaped a little).
//
// Real-time safety: prepare() allocates; process() never allocates, locks or blocks.
// Block-size independence: everything runs per sample, and filter coefficients update
// on a fixed 16-sample grid counted from reset(), not from block starts.
//==============================================================================

#include "HitRecord.h"
#include "SpscFifo.h"
#include "MicroGlow.h"
#include "Svf.h"
#include "SpectralDecay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tg
{

//------------------------------------------------------------------------------
struct GateSettings
{
    float thresholdDb  = -30.0f;   // open threshold (key level, dBFS)
    float hysteresisDb = 6.0f;     // close threshold sits this far below the open threshold
    float attackMs     = 0.5f;
    float holdMs       = 20.0f;
    float releaseMs    = 150.0f;   // time to travel the whole range
    float rangeDb      = -80.0f;   // how far it closes; -80 or lower = fully shut
    float lookaheadMs  = 5.0f;     // 0 .. kMaxLookaheadMs
    float keyHpfHz     = 0.0f;     // 0 = off
    float keyHighCutHz = 0.0f;     // 0 = off
    float trackingPct  = 0.0f;     // 0..100: how far the high cut closes as each hit decays
    bool  externalKey  = false;    // key from the sidechain input
    bool  sourceKey    = false;    // Source key: the level at the drum's learned fundamental, opening only on a stick attack
    float sourceAttackDb = 6.0f;   // Source key: how far the high band must jump for a stick attack (0 = no check); 6 dB measured best
    bool  listen       = false;    // output the filtered key (what the detector hears)

    // Leveler
    bool  levelerOn    = false;
    float levelAmount  = 75.0f;    // 0..100 %: how much of the gap to the target each hit closes
    float levelTarget  = kAutoTarget; // dBFS, or kAutoTarget = the running average of recent hits
    float levelMaxDb   = 12.0f;    // the most any hit is turned up or down

    // Micro Glow
    MicroGlow::Settings glow;

    // Spectral decay
    SpectralDecay::Settings spectral;

    static constexpr float kAutoTarget = -100.0f;
};

// One display frame per kFrameSamples of output, for the UI's scrolling view.
struct MeterFrame
{
    float inPeak  = 0.0f;   // input audio peak (aligned with the output)
    float keyDb   = -120.0f;// detector level (aligned with the output)
    float gainDb  = 0.0f;   // gate gain (0 = open)
    float levelDb = 0.0f;   // leveler gain
    bool  open    = false;  // detector state at this point
    bool  hit     = false;  // a hit's first sample is in this frame
    float hitChangeDb = 0.0f; // the leveler change for that hit
};

// 4th-order Butterworth = two SVFs with these Qs
static constexpr double kButterQ1 = 0.5411961, kButterQ2 = 1.3065630;

//------------------------------------------------------------------------------
class GateEngine
{
public:
    static constexpr double kMaxLookaheadMs = 20.0;
    static constexpr int    kCoeffEvery     = 16;    // samples between filter retunes
    static constexpr int    kFrameSamples   = 128;   // samples per UI meter frame
    static constexpr int    kLookaheadFade  = 64;    // samples to fade out/in when lookahead changes
    static constexpr float  kFullyShutDb    = -80.0f;

    void prepare (double sampleRate, int maxChannels)
    {
        sr = sampleRate;
        numCh = std::clamp (maxChannels, 1, kMaxChannels);
        const size_t maxLa = (size_t) std::ceil (kMaxLookaheadMs * 0.001 * sr) + 1;
        ringSize = 1;
        while (ringSize < maxLa + 2) ringSize <<= 1;
        ringMask = ringSize - 1;
        audioRing.assign ((size_t) numCh * ringSize, 0.0f);
        keyRing.assign (ringSize, 0.0f);
        levelRing.assign (ringSize, 0.0f);
        stateRing.assign (ringSize, 0);
        onsetRing.assign (ringSize, 0);
        keyAbsRing.assign (ringSize, 0.0f);
        glow.prepare (sr, numCh);
        atkHp1.setup (1000.0, sr, kButterQ1); atkHp2.setup (1000.0, sr, kButterQ2);
        atkRel = std::exp (-1.0 / (0.005 * sr)); atkRise = 1.0 - std::exp (-1.0 / (0.030 * sr));
        atkHoldLen = (int) std::lround (0.002 * sr); attackWindow = (int) std::lround (0.010 * sr);
        spectral.prepare (sr, numCh);
        reset();
    }

    void reset()
    {
        std::fill (audioRing.begin(), audioRing.end(), 0.0f);
        std::fill (keyRing.begin(), keyRing.end(), 0.0f);
        std::fill (levelRing.begin(), levelRing.end(), 0.0f);
        std::fill (stateRing.begin(), stateRing.end(), (uint8_t) 0);
        std::fill (onsetRing.begin(), onsetRing.end(), (uint8_t) 0);
        spectral.reset();
        atkHp1.reset(); atkHp2.reset(); srcBp1.reset(); srcBp2.reset(); atkEnv = atkValley = srcPeak = srcRms = 0.0; atkHoldLeft = 0; haveAttack = false; lastAttack = 0; srcF0 = 0.0;
        writePos = 0;
        clock = 0;
        for (auto* f : { &hpf1, &hpf2, &lpf1, &lpf2 }) f->reset();
        peakEnv = rmsPow = 0.0;
        trackFast = trackSlow = trackPeak = 0.0;
        detectorOpen = false;
        g = gFloor = gFloorTarget = dbToGain (s.rangeDb);
        attacking = false;
        holdLeft = 0;
        la = laTarget = lookaheadSamples (s.lookaheadMs);
        laFade = 0;
        laFadeGain = 1.0f;
        hpfHzSm = s.keyHpfHz > 0 ? s.keyHpfHz : 0.0;
        hcStartSm = highCutStart();
        frameCount = 0;
        frame = {};
        onsetPeak = onsetValley = 0.0;
        lastOnset = riseStart = 0;
        prevKf = slopeEnv = prevSlopeEnv = 0.0;
        candActive = false; candT0 = 0;
        jumpPow = prevJumpEnv = lvlPeak = lvlValley = 0.0; levelRiseStart = lastEdge = lastJump = closedAt = 0;
        std::fill (keyAbsRing.begin(), keyAbsRing.end(), 0.0f);
        haveOnset = false;
        numPending = 0;
        lvlGain = lvlFrom = lvlTo = 1.0;
        lvlGlideLeft = 0;
        lvlUser = 1.0;
        lvlAvgDb = 0.0;
        lvlHaveAvg = false;
        lvlMarkAt = UINT64_MAX;
        glow.reset();
        // (the FIFOs are drained by their consumer; clearing them here would race the UI thread)
        retune();
        // settle the smoothers so a fresh start is exact
        gFloor = gFloorTarget;
        g = gFloor;
    }

    // Call at the start of each block (cheap; no allocation).
    void setSettings (const GateSettings& ns) noexcept
    {
        s = ns;
        thrOpen  = dbToGain (s.thresholdDb);
        atkJump  = dbToGain (std::max (1.0f, s.sourceAttackDb));
        thrClose = dbToGain (s.thresholdDb - std::max (0.0f, s.hysteresisDb));
        attackSamples = std::max (1.0, std::ceil (s.attackMs * 0.001 * sr - 1e-9));   // whole samples, so the ramp ends exactly on the transient
        holdSamples   = (int) std::lround (std::max (0.0f, s.holdMs) * 0.001 * sr);
        const double span = s.rangeDb <= kFullyShutDb ? -kFullyShutDb : -std::min (0.0f, s.rangeDb);
        const double relSamples = std::max (1.0, s.releaseMs * 0.001 * sr);
        releaseCoef = std::pow (10.0, -span / (20.0 * relSamples));
        gFloorTarget = dbToGain (s.rangeDb);
        laTarget = lookaheadSamples (s.lookaheadMs);
        glow.setSettings (s.glow);
        spectral.setSettings (s.spectral, s.releaseMs, s.rangeDb);
    }

    int lookaheadSamples (float ms) const noexcept
    {
        return (int) std::lround (std::clamp ((double) ms, 0.0, kMaxLookaheadMs) * 0.001 * sr);
    }
    // total latency = lookahead + Micro Glow's oversampling filters (constant, on or off)
    int currentLatency() const noexcept { return la + glow.latencySamples(); }
    int latencyFor (float lookaheadMs) const noexcept { return lookaheadSamples (lookaheadMs) + glow.latencySamples(); }
    int glowLatency() const noexcept { return glow.latencySamples(); }

    //--------------------------------------------------------------------------
    // io: numChannels audio buffers, processed in place.
    // key: sidechain buffers (used only when settings.externalKey and keyChannels > 0).
    void process (float* const* io, int numChannels, const float* const* key, int keyChannels, int numSamples) noexcept
    {
        numChannels = std::min (numChannels, numCh);
        const bool useExt = s.externalKey && key != nullptr && keyChannels > 0;
        const int  delayForState = std::max (0, la - (int) attackSamples);
        const double peakRel  = std::exp (-1.0 / (0.010 * sr));
        const double rmsCoef  = std::exp (-1.0 / (0.003 * sr));
        const double fastRel  = std::exp (-1.0 / (0.005 * sr));
        const double slowCoef = std::exp (-1.0 / (0.005 * sr));
        const int hitWindowSamples = std::max (1, (int) std::lround (0.010 * sr));
        const uint64_t minGap = (uint64_t) std::lround (0.015 * sr);
        const uint64_t maxBackdate = (uint64_t) std::lround (0.003 * sr);
        const int afterLen = std::max (1, (int) std::lround (0.001 * sr)), beforeLen = (int) std::lround (0.010 * sr);
        const uint64_t reopenGap = (uint64_t) std::lround (0.050 * sr), recentLen = (uint64_t) std::lround (0.005 * sr);
        const double jumpCoef = std::exp (-1.0 / (0.008 * sr));
        const double slopeRel = std::exp (-1.0 / (0.005 * sr));
        const int oneMs = std::max (1, (int) std::lround (0.001 * sr));
        // Leveler timing (see the header): measure for W samples, glide for G, finish P before the hit
        const int G = la > 0 ? std::clamp (la / 4, 1, oneMs) : oneMs;
        const int P = la > 0 ? std::min (oneMs / 2, la / 8) : 0;
        const int W = la > 0 ? std::max (1, la - G - P) : oneMs;
        const double lvlStepOff = 1.0 / std::max (1.0, 0.010 * sr);   // 10 ms on/off glide
        float glowFrame[kMaxChannels];

        for (int i = 0; i < numSamples; ++i)
        {
            if ((clock % kCoeffEvery) == 0)
                retune();

            //---------------- key -> mono -> filters
            double k = 0.0;
            if (useExt)
            {
                for (int c = 0; c < keyChannels; ++c) k += key[c][i];
                k /= keyChannels;
            }
            else
            {
                for (int c = 0; c < numChannels; ++c) k += io[c][i];
                k /= std::max (1, numChannels);
            }

            double hp = k, dummy;
            if (hpfOn)
            {
                hpf1.tick (k, hp);
                double h2;
                hpf2.tick (hp, h2);
                hp = h2;
            }

            // tracking envelope runs on the HPF'd key (before the high cut, so it can't chase itself)
            {
                const double a = std::abs (hp);
                trackFast = a > trackFast ? a : a + (trackFast - a) * fastRel;
                trackSlow = trackFast + (trackSlow - trackFast) * slowCoef;
                if (trackFast > 2.0 * trackSlow + 1e-9)   // onset: a new hit resets the reference, even a soft one
                    trackPeak = trackFast;
                else
                    trackPeak = std::max (trackPeak, trackFast);
            }

            //---------------- Source key: the drum's fundamental, confirmed by a stick attack
            // Cymbals and hats put next to nothing at a tom's fundamental, so they can't push it over the
            // threshold; a drum ringing in sympathy (a floor tom off the kick) does reach it, but has no
            // stick crack - so the gate only OPENS when the fundamental crosses the threshold within
            // 10 ms of a stick attack (the key's high band jumping 12 dB). Closing follows the
            // fundamental alone. Until the fundamental is learned it uses the normal key level, with
            // the same stick-attack check.
            double srcBand = 0.0;
            {
                double a1, hk;
                atkHp1.tick (k, a1); atkHp2.tick (a1, hk);
                const double ah = std::abs (hk);
                if (ah >= atkEnv) { atkEnv = ah; atkHoldLeft = atkHoldLen; }
                else if (atkHoldLeft > 0) --atkHoldLeft;
                else atkEnv = ah + (atkEnv - ah) * atkRel;
                if (atkEnv < atkValley) atkValley = atkEnv;
                else atkValley += (atkEnv - atkValley) * atkRise;
                if (atkEnv > atkJump * atkValley && atkEnv > 1e-5) { lastAttack = clock; haveAttack = true; atkValley = atkEnv; }
                if ((clock % 4096) == 0)
                {
                    const double f0 = spectral.fundamentalHz();
                    if (f0 > 0.0 && std::abs (f0 - srcF0) > 0.5) { srcF0 = f0; srcBp1.setup (f0, sr, 2.0); srcBp2.setup (f0, sr, 2.0); }
                }
                if (srcF0 > 0.0) srcBand = srcBp2.bandPass (srcBp1.bandPass (hp));
            }

            double kf = hp;
            if (lpfOn)
            {
                kf = lpf1.tick (hp, dummy);
                kf = lpf2.tick (kf, dummy);
            }

            //---------------- detector
            const double a = std::abs (kf);
            peakEnv = a > peakEnv ? a : a + (peakEnv - a) * peakRel;
            rmsPow  = kf * kf + (rmsPow - kf * kf) * rmsCoef;
            double level = 0.5 * (peakEnv + 1.41421356 * std::sqrt (rmsPow));
            if (s.sourceKey && srcF0 > 0.0)
            {
                const double sa = std::abs (srcBand);
                srcPeak = sa > srcPeak ? sa : sa + (srcPeak - sa) * peakRel;
                srcRms  = srcBand * srcBand + (srcRms - srcBand * srcBand) * rmsCoef;
                level = 0.5 * (srcPeak + 1.41421356 * std::sqrt (srcRms));
            }
            const bool attackRecent = haveAttack && clock - lastAttack <= (uint64_t) attackWindow;

            const bool wasOpen = detectorOpen;
            if (! detectorOpen && level >= thrOpen && (! s.sourceKey || s.sourceAttackDb <= 0.0f || attackRecent)) detectorOpen = true;
            else if (detectorOpen && level < thrClose)    detectorOpen = false;
            if (s.sourceKey && srcF0 > 0.0 && detectorOpen && ! wasOpen)
            {
                // the fundamental takes a few ms to build up: open from where the hit really began (the
                // stick attack, or the start of the key's rise), as far back as the lookahead still holds
                const uint64_t began = std::min (clock, haveAttack && clock - lastAttack <= (uint64_t) attackWindow ? lastAttack : std::min (riseStart, levelRiseStart));
                const uint64_t room = (uint64_t) std::max (0, delayForState - 1);
                const uint64_t from = std::max (began >= (uint64_t) oneMs ? began - (uint64_t) oneMs : 0, clock >= room ? clock - room : 0);
                for (uint64_t t = from; t < clock; ++t) stateRing[(size_t) (t & (uint64_t) ringMask)] = 1;
            }

            //---------------- onsets: the gate opening, or a fresh stroke while it's open
            // Strokes are found on the key's slope (its first difference: "high-frequency
            // content"), not its level. A stick attack is a sharp broadband edge; the ringing tail
            // of the last hit is smooth, so even a ghost note 9 dB over a tom's tail stands out
            // clearly and gets timed to the sample. riseStart = the last sample before the slope
            // envelope started climbing: where the stroke really begins.
            {
                const double d = std::abs (kf - prevKf);
                prevKf = kf;
                slopeEnv = d > slopeEnv ? d : d + (slopeEnv - d) * slopeRel;
            }
            if (slopeEnv <= prevSlopeEnv) riseStart = clock;
            prevSlopeEnv = slopeEnv;
            // a smooth level for spotting strokes by their jump: 8 ms RMS (ripples < 0.5 dB on a
            // 110 Hz tom, where the peak follower ripples ~8 dB every cycle)
            jumpPow = kf * kf + (jumpPow - kf * kf) * jumpCoef;
            const double jumpEnv = std::sqrt (jumpPow);
            if (jumpEnv <= prevJumpEnv) levelRiseStart = clock;
            prevJumpEnv = jumpEnv;
            // edges and jumps are tracked all the time (also while the gate is shut)
            const bool edge = slopeEnv > 4.0 * onsetValley && onsetValley < 0.25 * onsetPeak;
            const bool jump = jumpEnv > 2.0 * lvlValley && lvlValley < 0.5 * lvlPeak;
            if (edge) lastEdge = clock;
            if (jump) lastJump = clock;
            if (! detectorOpen && wasOpen) closedAt = clock;

            // The gate opening is a hit - unless it's the same note wobbling back over the
            // threshold: reopening within 50 ms of closing, with no edge or jump in the last 5 ms
            // (close overtones beating in a decaying tom make its level wobble by several dB).
            bool onset = detectorOpen && ! wasOpen
                         && (! haveOnset || clock - closedAt >= reopenGap
                             || clock - lastEdge <= recentLen || clock - lastJump <= recentLen);
            // backdated start: where the rise began (at most 3 ms, and never before the last hit)
            auto backdate = [&] (uint64_t rs)
            {
                uint64_t t = std::max (rs + 1 <= clock ? rs + 1 : clock, clock >= maxBackdate ? clock - maxBackdate : 0);
                return haveOnset ? std::max (t, lastOnset + 1) : t;
            };
            uint64_t t0 = backdate (riseStart);
            if (detectorOpen && wasOpen)
            {
                // A new stroke while the gate is open shows up as a sharp edge (the key's slope
                // jumps 12 dB) or, under cymbal wash that hides the edge, as a 6 dB jump in the
                // smooth level. An edge is only a CANDIDATE: the biggest swing in the 1 ms after it
                // must be 4 dB above the biggest in the 10 ms before (at least a full cycle of the
                // ringing tail). A real stroke does; a hi-hat tick riding on a tom's ringing tail
                // has the edge but hardly raises the peak.
                // (Test against the valley/peak so far, THEN update them: a stroke louder than the
                // last one would otherwise reset the valley before its own jump is seen.)
                if (jump && (! haveOnset || clock - lastOnset >= minGap))
                {
                    onset = true;   // a 6 dB jump in the smooth level is a stroke by itself
                    t0 = candActive ? std::min (candT0, backdate (levelRiseStart)) : backdate (levelRiseStart);
                    candActive = false;
                }
                else if (! candActive && edge && (! haveOnset || clock - lastOnset >= minGap))
                {
                    candActive = true; candT0 = t0;
                    onsetPeak = onsetValley = slopeEnv;
                }
                if (candActive && ! onset && clock + 1 >= candT0 + (uint64_t) afterLen)
                {
                    // biggest swing in [t0, t0 + 1 ms) vs in [t0 - 10 ms, t0), from the delay line
                    double after = std::abs (kf), before = 0.0;
                    for (uint64_t c0 = candT0; c0 < clock; ++c0) after = std::max (after, (double) keyAbsRing[(size_t) c0 & ringMask]);
                    const uint64_t b0 = candT0 >= (uint64_t) beforeLen ? candT0 - (uint64_t) beforeLen : 0;
                    for (uint64_t c0 = b0; c0 < candT0; ++c0) before = std::max (before, (double) keyAbsRing[(size_t) c0 & ringMask]);
                    if (after >= 1.585 * before) { onset = true; t0 = candT0; }
                    candActive = false;
                }
            }
            else candActive = false;
            if (slopeEnv > onsetPeak)          { onsetPeak = slopeEnv; onsetValley = slopeEnv; }
            else if (slopeEnv < onsetValley)   onsetValley = slopeEnv;
            if (jumpEnv > lvlPeak)             { lvlPeak = jumpEnv; lvlValley = jumpEnv; }
            else if (jumpEnv < lvlValley)      lvlValley = jumpEnv;
            keyAbsRing[writePos] = (float) std::abs (kf);
            if (onset)
            {
                onsetPeak = onsetValley = slopeEnv;
                lvlPeak = lvlValley = jumpEnv;
                lastOnset = t0;
                haveOnset = true;
                // mark where the hit lands on the output timeline (or now, if that's already passed)
                const uint64_t outNow = clock >= (uint64_t) la ? clock - (uint64_t) la : 0;
                onsetRing[(size_t) (std::max (t0, outNow) & (uint64_t) ringMask)] = 1;
                if (numPending < kMaxPending)
                {
                    auto& pd = pending[numPending++];
                    pd = {};
                    pd.hit.arrivalSample = t0;
                    pd.hit.timeSamples = t0 + (uint64_t) la + (uint64_t) glow.latencySamples();
                    pd.start = t0;
                    pd.decideAt = t0 + (uint64_t) W;
                    pd.reportAt = t0 + (uint64_t) std::max (W, hitWindowSamples);
                    pd.landsAt = t0 + (uint64_t) la;
                    // the samples from t0 up to now are already in the delay line
                    for (uint64_t c0 = t0; c0 < clock; ++c0)
                    {
                        const size_t r = (writePos + ringSize - (size_t) (clock - c0)) & ringMask;
                        for (int c = 0; c < numChannels; ++c)
                            pd.audioPeak = std::max (pd.audioPeak, (double) std::abs (audioRing[(size_t) c * ringSize + r]));
                        pd.keyPeak = std::max (pd.keyPeak, (double) levelRing[r]);
                    }
                }
            }

            //---------------- pending hits: measure, then decide the leveler gain, then report
            for (int h = 0; h < numPending;)
            {
                auto& pd = pending[h];
                if (clock < pd.start + (uint64_t) hitWindowSamples) pd.keyPeak = std::max (pd.keyPeak, level);
                if (! pd.decided)
                {
                    for (int c = 0; c < numChannels; ++c) pd.audioPeak = std::max (pd.audioPeak, (double) std::abs (io[c][i]));
                    if (clock + 1 >= pd.decideAt)
                    {
                        pd.decided = true;
                        decideLevel (pd, G);
                    }
                }
                if (pd.decided && clock + 1 >= pd.reportAt)
                {
                    pd.hit.peakDb = (float) gainToDb (pd.keyPeak);
                    hits.push (pd.hit);
                    pending[h] = pending[--numPending];
                    continue;
                }
                ++h;
            }

            //---------------- delay lines
            const size_t w = writePos;
            for (int c = 0; c < numChannels; ++c)
                audioRing[(size_t) c * ringSize + w] = io[c][i];
            keyRing[w]   = (float) kf;
            levelRing[w] = (float) level;
            stateRing[w] = detectorOpen ? 1 : 0;

            const size_t rAudio = (w + ringSize - (size_t) la) & ringMask;
            const size_t rState = (w + ringSize - std::min ((size_t) delayForState, ringSize - 1)) & ringMask;
            const bool want = stateRing[rState] != 0;

            //---------------- gain envelope
            gFloor = gFloorTarget + (gFloor - gFloorTarget) * floorCoef;
            if (std::abs (gFloor - gFloorTarget) < 1e-7) gFloor = gFloorTarget;

            if (want)
            {
                holdLeft = holdSamples;
                if (! attacking && g < 1.0)
                {
                    attacking = true;
                    attackFrom = g;
                    attackPhase = 0.0;
                }
            }
            else if (holdLeft > 0)
            {
                --holdLeft;
            }
            else
            {
                attacking = false;
                g *= releaseCoef;
                if (g < 1e-4) g = 0.0;   // -80 dB: snap to silence
            }

            if (attacking)
            {
                attackPhase += 1.0 / attackSamples;
                if (attackPhase >= 1.0) { g = 1.0; attacking = false; }
                else g = attackFrom + (1.0 - attackFrom) * (0.5 - 0.5 * std::cos (M_PI * attackPhase));
            }
            g = std::max (g, gFloor);

            //---------------- lookahead change: fade out, switch, fade in
            if (laFade == 0 && laTarget != la) laFade = 2 * kLookaheadFade;
            if (laFade > 0)
            {
                --laFade;
                if (laFade == kLookaheadFade) la = laTarget;
                const int p = laFade >= kLookaheadFade ? laFade - kLookaheadFade : kLookaheadFade - laFade;
                laFadeGain = (float) p / (float) kLookaheadFade;
            }
            else laFadeGain = 1.0f;

            //---------------- leveler gain: hit gain (scheduled by decideLevel) x on/off glide
            if (lvlGlideLeft > 0)
            {
                --lvlGlideLeft;
                lvlGain = lvlGlideLeft == 0 ? lvlTo : lvlTo + (lvlFrom - lvlTo) * (double) lvlGlideLeft / (double) lvlGlideLen;
            }
            const double userT = s.levelerOn ? 1.0 : 0.0;
            if (lvlUser < userT)      lvlUser = std::min (userT, lvlUser + lvlStepOff);
            else if (lvlUser > userT) lvlUser = std::max (userT, lvlUser - lvlStepOff);
            const double lg = lvlUser >= 1.0 ? lvlGain : (lvlUser <= 0.0 ? 1.0 : 1.0 + lvlUser * (lvlGain - 1.0));

            //---------------- output: the gate (or the spectral decay) x leveler
            const float gf = (float) g * laFadeGain;
            const float lf = (float) lg;
            const bool onsetNow = onsetRing[rAudio] != 0;
            onsetRing[rAudio] = 0;
            const int attackN = (int) attackSamples;
            const int peek = la > 0 ? std::min (attackN, la) : 0;
            const bool onsetAhead = peek > 0 && onsetRing[(rAudio + (size_t) peek) & ringMask] != 0;
            const bool gateClosed = stateRing[rAudio] == 0 && g <= gFloor + 1e-9;
            float gated[kMaxChannels];
            for (int c = 0; c < numChannels; ++c) gated[c] = audioRing[(size_t) c * ringSize + rAudio];
            // x * gate gain when the spectral decay is off (bit-exact), its shaped tail when on
            spectral.tick (gated, numChannels, gf, (float) gFloor * laFadeGain, onsetNow, onsetAhead, attackN, gateClosed);
            if (s.listen)
            {
                const float kOut = keyRing[rAudio] * laFadeGain;
                for (int c = 0; c < numChannels; ++c) glowFrame[c] = kOut;
            }
            else
            {
                for (int c = 0; c < numChannels; ++c)
                    glowFrame[c] = gated[c] * lf;   // gated = x * 1.0f is exact: bit-identical when fully open
            }
            glow.tick (glowFrame, numChannels, s.listen);
            for (int c = 0; c < numChannels; ++c) io[c][i] = glowFrame[c];

            //---------------- meters (aligned with the output)
            float inPk = 0.0f;
            for (int c = 0; c < numChannels; ++c) inPk = std::max (inPk, std::abs (audioRing[(size_t) c * ringSize + rAudio]));
            frame.inPeak = std::max (frame.inPeak, inPk);
            frame.keyDb  = std::max (frame.keyDb, (float) gainToDb (levelRing[rAudio]));
            frame.gainDb = std::min (frame.gainDb, (float) gainToDb (g));
            frame.levelDb = (float) gainToDb (lg);
            frame.open   = frame.open || stateRing[rAudio] != 0;
            if (clock == lvlMarkAt) { frame.hit = true; frame.hitChangeDb = lvlMarkDb; }
            if (++frameCount == kFrameSamples)
            {
                meters.push (frame);
                frame = {};
                frameCount = 0;
            }

            writePos = (w + 1) & ringMask;
            ++clock;
        }
    }

    // Bypass keeps the same latency, so toggling it never shifts the track in time.
    // (Micro Glow is forced to its dry path, which is the matching pure delay.)
    void processBypassed (float* const* io, int numChannels, int numSamples) noexcept
    {
        numChannels = std::min (numChannels, numCh);
        float fr[kMaxChannels];
        for (int i = 0; i < numSamples; ++i)
        {
            const size_t w = writePos;
            for (int c = 0; c < numChannels; ++c) audioRing[(size_t) c * ringSize + w] = io[c][i];
            const size_t r = (w + ringSize - (size_t) la) & ringMask;
            for (int c = 0; c < numChannels; ++c) fr[c] = audioRing[(size_t) c * ringSize + r];
            glow.tick (fr, numChannels, true);
            for (int c = 0; c < numChannels; ++c) io[c][i] = fr[c];
            writePos = (w + 1) & ringMask;
            ++clock;
        }
    }

    static constexpr int kMaxChannels = 8;

    SpscFifo<MeterFrame, 1024> meters;   // audio -> UI
    SpscFifo<Hit, 256>         hits;     // audio -> UI / later phases

    // read-only access for tests
    double currentGain() const noexcept { return g; }
    double currentHighCutHz() const noexcept { return lpfOn ? hcNow : 0.0; }
    double currentLevelerGain() const noexcept { return lvlGain; }
    float glowModulatedDrive() const noexcept { return glow.modulatedDrive(); }
    float glowAutoGainDb() const noexcept { return glow.autoGainDb(); }
    SpectralDecay& spectralDecay() noexcept { return spectral; }   // learned partials, UI snapshots

private:
    static double dbToGain (double db) noexcept { return db <= kFullyShutDb ? 0.0 : std::pow (10.0, db / 20.0); }
    static double gainToDb (double gn) noexcept { return gn <= 1e-6 ? -120.0 : 20.0 * std::log10 (gn); }

    double highCutStart() const noexcept { return s.keyHighCutHz > 0 ? s.keyHighCutHz : 20000.0; }

    struct Pending
    {
        Hit hit;
        uint64_t start = 0, decideAt = 0, reportAt = 0, landsAt = 0;
        double keyPeak = 0.0, audioPeak = 0.0;
        bool decided = false;
    };

    // Sets this hit's leveler gain; it glides in over G samples and is fully there P samples
    // before the hit's first sample comes out (landsAt).
    void decideLevel (Pending& pd, int G) noexcept
    {
        const double peakDb = gainToDb (std::max (pd.audioPeak, 1e-6));
        pd.hit.audioPeakDb = (float) peakDb;
        const bool autoT = s.levelTarget <= GateSettings::kAutoTarget + 1.0f;
        const double target = autoT ? (lvlHaveAvg ? lvlAvgDb : peakDb) : (double) s.levelTarget;
        const double maxDb = std::clamp ((double) s.levelMaxDb, 0.0, 48.0);
        const double changeDb = std::clamp ((s.levelAmount * 0.01) * (target - peakDb), -maxDb, maxDb);
        // the running average of hit peaks (what Auto aims at), ~8 hits long
        lvlAvgDb = lvlHaveAvg ? lvlAvgDb + (peakDb - lvlAvgDb) / 8.0 : peakDb;
        lvlHaveAvg = true;

        pd.hit.levelChangeDb = s.levelerOn ? (float) changeDb : 0.0f;
        lvlFrom = lvlGain;
        lvlTo = std::pow (10.0, changeDb / 20.0);
        lvlGlideLen = lvlGlideLeft = std::max (1, G);
        lvlMarkAt = pd.landsAt;
        lvlMarkDb = pd.hit.levelChangeDb;
    }

    // Runs every kCoeffEvery samples, on a grid counted from reset().
    void retune() noexcept
    {
        const double sm = 0.15;   // ~2 ms glide at 48 kHz with 16-sample ticks
        // HPF
        if (s.keyHpfHz > 0)
        {
            if (! hpfOn) { hpfHzSm = s.keyHpfHz; hpf1.reset(); hpf2.reset(); }
            hpfHzSm *= std::pow (s.keyHpfHz / hpfHzSm, sm);
            hpf1.setup (hpfHzSm, sr, kButterQ1);
            hpf2.setup (hpfHzSm, sr, kButterQ2);
            hpfOn = true;
        }
        else hpfOn = false;

        // High cut, with tracking: one octave down per 12 dB the hit has decayed (at 100 %)
        const bool wantLpf = s.keyHighCutHz > 0 || s.trackingPct > 0;
        if (wantLpf)
        {
            const double start = highCutStart();
            if (! lpfOn) { hcStartSm = start; lpf1.reset(); lpf2.reset(); }
            hcStartSm *= std::pow (start / hcStartSm, sm);
            double oct = 0.0;
            if (s.trackingPct > 0 && trackPeak > 1e-7)
            {
                const double decayDb = std::clamp (20.0 * std::log10 (std::max (trackFast, 1e-9) / trackPeak), -72.0, 0.0);
                oct = std::min (5.0, (s.trackingPct * 0.01) * (-decayDb) / 12.0);
            }
            hcNow = std::max (300.0, hcStartSm * std::pow (2.0, -oct));
            lpf1.setup (hcNow, sr, kButterQ1);
            lpf2.setup (hcNow, sr, kButterQ2);
            lpfOn = true;
        }
        else lpfOn = false;

        // range glide (~20 ms), applied per sample
        floorCoef = std::exp (-1.0 / (0.020 * sr));
    }

    GateSettings s;
    double sr = 48000.0;
    int numCh = 2;

    std::vector<float>   audioRing, keyRing, levelRing;
    std::vector<uint8_t> stateRing, onsetRing;
    // Source key
    Svf atkHp1, atkHp2, srcBp1, srcBp2;
    double atkJump = 4.0, atkEnv = 0.0, atkValley = 0.0, atkRel = 0.999, atkRise = 0.001, srcPeak = 0.0, srcRms = 0.0, srcF0 = 0.0;
    int atkHoldLeft = 0, atkHoldLen = 96, attackWindow = 480;
    uint64_t lastAttack = 0; bool haveAttack = false;
public:
    double sourceKeyHz() const noexcept { return srcF0; }   // the Source key's fundamental (0 = not learned yet)
private:
    size_t ringSize = 1, ringMask = 0, writePos = 0;
    uint64_t clock = 0;

    Svf hpf1, hpf2, lpf1, lpf2;
    bool hpfOn = false, lpfOn = false;
    double hpfHzSm = 0.0, hcStartSm = 20000.0, hcNow = 20000.0;

    double peakEnv = 0.0, rmsPow = 0.0;
    double trackFast = 0.0, trackSlow = 0.0, trackPeak = 0.0;
    double thrOpen = 0.03, thrClose = 0.015;
    bool detectorOpen = false;

    // onsets and pending hits
    static constexpr int kMaxPending = 8;
    Pending pending[kMaxPending];
    int numPending = 0;
    double onsetPeak = 0.0, onsetValley = 0.0;
    uint64_t lastOnset = 0, riseStart = 0;
    double prevKf = 0.0, slopeEnv = 0.0, prevSlopeEnv = 0.0;
    double jumpPow = 0.0, prevJumpEnv = 0.0, lvlPeak = 0.0, lvlValley = 0.0;
    uint64_t levelRiseStart = 0, lastEdge = 0, lastJump = 0, closedAt = 0;
    std::vector<float> keyAbsRing;   // |key| per sample (for confirming strokes)
    bool candActive = false;
    uint64_t candT0 = 0;
    bool haveOnset = false;

    // leveler
    double lvlGain = 1.0, lvlFrom = 1.0, lvlTo = 1.0, lvlUser = 1.0, lvlAvgDb = 0.0;
    int lvlGlideLeft = 0, lvlGlideLen = 1;
    bool lvlHaveAvg = false;
    uint64_t lvlMarkAt = UINT64_MAX;
    float lvlMarkDb = 0.0f;

    MicroGlow glow;
    SpectralDecay spectral;

    double g = 0.0, gFloor = 0.0, gFloorTarget = 0.0, floorCoef = 0.0;
    double attackSamples = 24.0, attackFrom = 0.0, attackPhase = 0.0, releaseCoef = 0.999;
    bool attacking = false;
    int holdSamples = 0, holdLeft = 0;

    int la = 0, laTarget = 0, laFade = 0;
    float laFadeGain = 1.0f;

    MeterFrame frame;
    int frameCount = 0;
};

} // namespace tg
