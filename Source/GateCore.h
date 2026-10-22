#pragma once
//==============================================================================
// The Gate - core gate engine (Phase 1). Plain C++, no JUCE, so the offline
// tests run the exact code the plug-in runs.
//
// Signal flow:
//
//   key (input or sidechain) -> mono -> key HPF (24 dB/oct) -> key high cut (24 dB/oct,
//        cutoff tracks each hit's decay) -> detector (peak/RMS blend) -> open/close with
//        hysteresis -> state delay (so the attack ramp ends exactly on the transient)
//        -> gain envelope (attack / hold / release / range)
//
//   audio -> lookahead delay -> x gain -> out          (Listen: the filtered key instead)
//
// Real-time safety: prepare() allocates; process() never allocates, locks or blocks.
// Block-size independence: everything runs per sample, and filter coefficients update
// on a fixed 16-sample grid counted from reset(), not from block starts.
//==============================================================================

#include "HitRecord.h"
#include "SpscFifo.h"

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
    bool  listen       = false;    // output the filtered key (what the detector hears)
};

// One display frame per kFrameSamples of output, for the UI's scrolling view.
struct MeterFrame
{
    float inPeak  = 0.0f;   // input audio peak (aligned with the output)
    float keyDb   = -120.0f;// detector level (aligned with the output)
    float gainDb  = 0.0f;   // gate gain (0 = open)
    bool  open    = false;  // detector state at this point
};

//------------------------------------------------------------------------------
// Zavalishin/Simper TPT state-variable filter. Cheap to retune every few samples,
// which is what the tracking high cut needs.
struct Svf
{
    double ic1 = 0.0, ic2 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, k = 1.414;

    void setup (double fc, double fs, double q) noexcept
    {
        const double g = std::tan (M_PI * std::min (fc, 0.49 * fs) / fs);
        k = 1.0 / q;
        a1 = 1.0 / (1.0 + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() noexcept { ic1 = ic2 = 0.0; }

    // returns lowpass; highpass via the out-param
    inline double tick (double x, double& hp) noexcept
    {
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        hp = x - k * v1 - v2;
        return v2;
    }
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
        numCh = std::max (1, maxChannels);
        const size_t maxLa = (size_t) std::ceil (kMaxLookaheadMs * 0.001 * sr) + 1;
        ringSize = 1;
        while (ringSize < maxLa + 2) ringSize <<= 1;
        ringMask = ringSize - 1;
        audioRing.assign ((size_t) numCh * ringSize, 0.0f);
        keyRing.assign (ringSize, 0.0f);
        levelRing.assign (ringSize, 0.0f);
        stateRing.assign (ringSize, 0);
        reset();
    }

    void reset()
    {
        std::fill (audioRing.begin(), audioRing.end(), 0.0f);
        std::fill (keyRing.begin(), keyRing.end(), 0.0f);
        std::fill (levelRing.begin(), levelRing.end(), 0.0f);
        std::fill (stateRing.begin(), stateRing.end(), (uint8_t) 0);
        writePos = 0;
        clock = 0;
        for (auto* f : { &hpf1, &hpf2, &lpf1, &lpf2 }) f->reset();
        peakEnv = rmsPow = 0.0;
        trackFast = trackSlow = trackPeak = 0.0;
        detectorOpen = false;
        hitWindow = 0;
        hitPeak = 0.0;
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
        thrClose = dbToGain (s.thresholdDb - std::max (0.0f, s.hysteresisDb));
        attackSamples = std::max (1.0, std::ceil (s.attackMs * 0.001 * sr - 1e-9));   // whole samples, so the ramp ends exactly on the transient
        holdSamples   = (int) std::lround (std::max (0.0f, s.holdMs) * 0.001 * sr);
        const double span = s.rangeDb <= kFullyShutDb ? -kFullyShutDb : -std::min (0.0f, s.rangeDb);
        const double relSamples = std::max (1.0, s.releaseMs * 0.001 * sr);
        releaseCoef = std::pow (10.0, -span / (20.0 * relSamples));
        gFloorTarget = dbToGain (s.rangeDb);
        laTarget = lookaheadSamples (s.lookaheadMs);
    }

    int lookaheadSamples (float ms) const noexcept
    {
        return (int) std::lround (std::clamp ((double) ms, 0.0, kMaxLookaheadMs) * 0.001 * sr);
    }
    int currentLatency() const noexcept { return la; }

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
            const double level = 0.5 * (peakEnv + 1.41421356 * std::sqrt (rmsPow));

            const bool wasOpen = detectorOpen;
            if (! detectorOpen && level >= thrOpen)       detectorOpen = true;
            else if (detectorOpen && level < thrClose)    detectorOpen = false;

            if (detectorOpen && ! wasOpen)
            {
                hitWindow = hitWindowSamples;
                hitPeak = 0.0;
                pendingHit = {};
                pendingHit.arrivalSample = clock;
                pendingHit.timeSamples = clock + (uint64_t) la;
            }
            if (hitWindow > 0)
            {
                hitPeak = std::max (hitPeak, level);
                if (--hitWindow == 0)
                {
                    pendingHit.peakDb = (float) gainToDb (hitPeak);
                    hits.push (pendingHit);
                }
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

            //---------------- output
            const float gf = (float) g * laFadeGain;
            if (s.listen)
            {
                const float kOut = keyRing[rAudio] * laFadeGain;
                for (int c = 0; c < numChannels; ++c) io[c][i] = kOut;
            }
            else
            {
                for (int c = 0; c < numChannels; ++c)
                {
                    const float x = audioRing[(size_t) c * ringSize + rAudio];
                    io[c][i] = x * gf;   // x * 1.0f is exact: bit-identical when fully open
                }
            }

            //---------------- meters (aligned with the output)
            float inPk = 0.0f;
            for (int c = 0; c < numChannels; ++c) inPk = std::max (inPk, std::abs (audioRing[(size_t) c * ringSize + rAudio]));
            frame.inPeak = std::max (frame.inPeak, inPk);
            frame.keyDb  = std::max (frame.keyDb, (float) gainToDb (levelRing[rAudio]));
            frame.gainDb = std::min (frame.gainDb, (float) gainToDb (g));
            frame.open   = frame.open || stateRing[rAudio] != 0;
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
    void processBypassed (float* const* io, int numChannels, int numSamples) noexcept
    {
        numChannels = std::min (numChannels, numCh);
        for (int i = 0; i < numSamples; ++i)
        {
            const size_t w = writePos;
            for (int c = 0; c < numChannels; ++c) audioRing[(size_t) c * ringSize + w] = io[c][i];
            const size_t r = (w + ringSize - (size_t) la) & ringMask;
            for (int c = 0; c < numChannels; ++c) io[c][i] = audioRing[(size_t) c * ringSize + r];
            writePos = (w + 1) & ringMask;
            ++clock;
        }
    }

    SpscFifo<MeterFrame, 1024> meters;   // audio -> UI
    SpscFifo<Hit, 256>         hits;     // audio -> UI / later phases

    // read-only access for tests
    double currentGain() const noexcept { return g; }
    double currentHighCutHz() const noexcept { return lpfOn ? hcNow : 0.0; }

private:
    static double dbToGain (double db) noexcept { return db <= kFullyShutDb ? 0.0 : std::pow (10.0, db / 20.0); }
    static double gainToDb (double gn) noexcept { return gn <= 1e-6 ? -120.0 : 20.0 * std::log10 (gn); }

    double highCutStart() const noexcept { return s.keyHighCutHz > 0 ? s.keyHighCutHz : 20000.0; }

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
    std::vector<uint8_t> stateRing;
    size_t ringSize = 1, ringMask = 0, writePos = 0;
    uint64_t clock = 0;

    Svf hpf1, hpf2, lpf1, lpf2;
    bool hpfOn = false, lpfOn = false;
    double hpfHzSm = 0.0, hcStartSm = 20000.0, hcNow = 20000.0;

    double peakEnv = 0.0, rmsPow = 0.0;
    double trackFast = 0.0, trackSlow = 0.0, trackPeak = 0.0;
    double thrOpen = 0.03, thrClose = 0.015;
    bool detectorOpen = false;

    int hitWindow = 0;
    double hitPeak = 0.0;
    Hit pendingHit;

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
