#pragma once
//==============================================================================
// De-bleed: takes cymbal wash out of the gate's tail. It only ever turns the highs DOWN.
//
// A drum's own highs (the stick, the head's overtones) die away at least as fast as its body.
// Cymbal wash doesn't: it rings on for seconds. So after each hit's attack (which passes untouched),
// the highs (above ~2.5 kHz) are allowed to decay only as slowly as the drum's body does - faster
// as DE-BLEED goes up. Whatever is left above that line is bleed, and it's pulled down.
//   - no cymbal: the highs already fall with the body, nothing is touched
//   - cymbal in the tail: the part of the highs that outlasts the drum is turned down
// The gate decides everything; this never opens anything or holds anything open.
//
// Signal: y = low + gH * (x - low), where low is a 4th-order low-pass at the split frequency.
// gH = 1 gives back x exactly (bit-identical), so 0 % is a true bypass.
// Timing: runs on the output timeline. 'onsetNow' = a hit's first sample is here; 'onsetAhead' = one
// lands in 'ramp' samples (lookahead), so the highs are fully back BEFORE the next transient.
// Real-time: no allocation after prepare().
//==============================================================================

#include "Svf.h"
#include <algorithm>
#include <cmath>

namespace tg
{

class DeBleed
{
public:
    static constexpr int kMaxCh = 8;
    static constexpr double kSplitHz = 2500.0;
    static constexpr double kSlopeDbPerSec = 200.0;   // at 100 %: the highs fall at least 20 dB per 100 ms after the attack

    void prepare (double sampleRate, int channels) noexcept
    {
        sr = sampleRate;
        numCh = std::clamp (channels, 1, kMaxCh);
        for (int c = 0; c < kMaxCh; ++c) { lp1[c].setup (kSplitHz, sr, 0.5411961); lp2[c].setup (kSplitHz, sr, 1.3065630); }
        dl1.setup (kSplitHz, sr, 0.5411961); dl2.setup (kSplitHz, sr, 1.3065630);
        relB = std::exp (-1.0 / (0.030 * sr));    // body envelope: Hilbert amplitude, 30 ms release (no ripple)
        relH = std::exp (-1.0 / (0.010 * sr));    // highs: peak, 2 ms hold, 10 ms release
        holdLen = (int) std::lround (0.002 * sr);
        downCoef = std::exp (-1.0 / (0.003 * sr)); // gH goes down over ~3 ms (no zipper), up fast
        upCoef = std::exp (-1.0 / (0.0005 * sr));

        reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < kMaxCh; ++c) { lp1[c].reset(); lp2[c].reset(); }
        dl1.reset(); dl2.reset(); hil.reset();
        envB = envH = peakB = peakH = 0.0; holdH = 0;
        gH = 1.0; inAttack = false; attackLeft = 0; haveHit = false; rampLeft = 0;
    }

    // amount 0..100 (%), attackMs: how long each hit's attack passes untouched
    void setSettings (float amountPct, float attackMs) noexcept
    {
        amt = std::clamp ((double) amountPct, 0.0, 100.0) * 0.01;
        rate = 1.0 + 3.0 * amt;                         // the highs may fall 1x .. 4x as fast (in dB) as the body
        slopePerSample = std::pow (10.0, -kSlopeDbPerSec * amt / 20.0 / sr);   // ... and also fall with time
        gMin = std::pow (10.0, -40.0 * amt / 20.0);     // never more than 40 dB down
        attackLen = std::max (1, (int) std::lround (std::max (1.0f, attackMs) * 0.001 * sr));
    }

    // dry: the delayed input (for listening); io: the gated audio, processed in place
    // protectNow: a stroke whose attack must pass, but that isn't a new reference (a fill's next stroke)
    inline void tick (const float* dry, float* io, int channels, bool onsetNow, bool protectNow, bool onsetAhead, int ramp) noexcept
    {
        channels = std::min (channels, numCh);
        //---------------- listen: body and highs of the ungated input
        double m = 0.0;
        for (int c = 0; c < channels; ++c) m += dry[c];
        m /= channels;
        double unused;
        const double lo = dl2.tick (dl1.tick (m, unused), unused), hi = m - lo;
        const double aB = std::sqrt (hil.power (lo));
        envB = aB > envB ? aB : aB + (envB - aB) * relB;
        const double aH = std::abs (hi);
        if (aH >= envH) { envH = aH; holdH = holdLen; }
        else if (holdH > 0) --holdH;
        else envH = aH + (envH - aH) * relH;

        //---------------- each hit: its attack passes; its peaks set the reference for the tail
        if (onsetNow)
        {
            inAttack = true; attackLeft = attackLen; haveHit = true; timeFall = 1.0; protectOnly = false;
            peakB = envB; peakH = envH;
            rampLeft = 0; gH = 1.0;
        }
        else if (protectNow)
        {
            inAttack = true; attackLeft = attackLen; protectOnly = true;
            rampLeft = 0; gH = 1.0;
        }
        else if (onsetAhead && rampLeft == 0 && gH < 1.0)
        {
            rampLeft = rampLen = std::max (1, ramp); rampFrom = gH;   // fully back before the next transient
        }
        if (inAttack)
        {
            if (! protectOnly) { peakB = std::max (peakB, envB); peakH = std::max (peakH, envH); }
            if (--attackLeft <= 0) inAttack = false;
        }

        double target = 1.0;
        if (amt > 0.0 && haveHit && ! inAttack && peakB > 1e-9 && peakH > 1e-9 && envH > 1e-9)
        {
            // where the highs would be if they fell 'rate' times as fast as the body (in dB)
            const double bodyRatio = std::clamp (envB / peakB, 1e-6, 1.0);
            timeFall *= slopePerSample;
            const double allowed = peakH * std::pow (bodyRatio, rate) * timeFall;
            target = std::clamp (allowed / envH, gMin, 1.0);
        }
        if (rampLeft > 0)
        {
            --rampLeft;
            const double p = 1.0 - (double) rampLeft / rampLen;
            gH = rampFrom + (1.0 - rampFrom) * (0.5 - 0.5 * std::cos (M_PI * p));
        }
        else
        {
            gH = target < gH ? target + (gH - target) * downCoef : target + (gH - target) * upCoef;
            if (gH > 1.0 - 1e-9) gH = 1.0;
        }

        //---------------- apply: low + gH * high (exactly x when gH == 1)
        if (amt <= 0.0) return;
        for (int c = 0; c < channels; ++c)
        {
            const double x = io[c];
            const double l = lp2[c].tick (lp1[c].tick (x, unused), unused);
            if (gH < 1.0) io[c] = (float) (l + gH * (x - l));
        }
    }

    double highGainDb() const noexcept { return 20.0 * std::log10 (std::max (gH, 1e-6)); }   // for the UI / tests

private:
    double sr = 48000.0;
    int numCh = 2;
    Svf lp1[kMaxCh], lp2[kMaxCh], dl1, dl2;
    HilbertPair hil;
    double relB = 0.999, relH = 0.99, downCoef = 0.99, upCoef = 0.9;
    int holdLen = 96, holdH = 0;
    double envB = 0.0, envH = 0.0, peakB = 0.0, peakH = 0.0, gH = 1.0, rampFrom = 1.0;
    double amt = 0.0, rate = 1.0, gMin = 1.0, slopePerSample = 1.0, timeFall = 1.0;
    int attackLen = 480, attackLeft = 0, rampLeft = 0, rampLen = 1;
    bool inAttack = false, haveHit = false, protectOnly = false;
};

} // namespace tg
