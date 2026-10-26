#pragma once
//==============================================================================
// The Gate - spectral decay (Phase 3, reworked Oct 25 after listening on real drums).
//
// The gate's tail, shaped like a real drum decays: the attack stays whole, the highs close
// first as the hit dies away, the low end carries the sustain. Plain C++, no JUCE.
//
//   1. TRANSIENT - from each hit's first sample, for as long as the source's attack lasts,
//      the output is exactly the plain gate (full bandwidth, untouched). The length is LEARNED:
//      on every hit the source's attack band (> 1 kHz) is followed until it has fallen 12 dB
//      under its peak; hits buried in wash are skipped; median of the last 15 hits, 3..50 ms. The
//      Transient knob scales it (-100 % = half, +100 % = double).
//   2. THE HIGHS CLOSE FROM THE TOP DOWN - after the transient, a 4-pole low-pass glides down
//      as the hit's body decays: cutoff = top x 2^(-k x dB the body has fallen since its peak),
//      k set by Focus (100 % = 1 octave per dB). Everything above the cutoff closes (to the
//      gate's Range). A hard hit stays bright longer, a soft one closes sooner - like the drum.
//      The cutoff never goes below the body band.
//   3. THE BODY - a low source (spectral peak < 1 kHz) has a learned body band: from the
//      bottom up to where its spectrum falls 30 dB under the peak (max 1.2 kHz). It follows the
//      gate; Sustain lets ONLY this band ring on past it: down as far as Sustain allows (25 % =
//      15 dB under its peak, 50 % = 30 dB, 100 % = 60 dB, never into its bleed floor), then
//      releasing up to 4x slower than the gate. The high end never rings longer than the gate.
//
//   output = g x + (gBody - g) x lowBand(x) + (gHigh - g) x (x - movingLowPass(x))
//   (g = the gate's gain.) So whenever the gains are equal - during the transient, or with
//   Focus 0 and Sustain 0 - the output IS x x g: bit-identical to the plain gate, no latency,
//   no phase shift. When a stroke comes in while the highs are closed, they open back up
//   (gain and cutoff, smoothly) so they're fully open exactly when the transient arrives.
//
// LEARNING (automatic, from the track): the source's spectrum (85 ms of each hit's body) and
// the bleed's (while the gate is shut), for the body band and the display; the transient
// length from every hit.
//
// Real-time safety: prepare() allocates. tick() never allocates or locks; the FFT runs on the
// audio thread once per finished capture (~40 us for 4096 points), deterministic for renders.
//==============================================================================

#include "Svf.h"
#include "SpscFifo.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tg
{

//------------------------------------------------------------------------------
// In-place iterative radix-2 complex FFT (forward). Tables built in prepare().
class Fft
{
public:
    void prepare (int n)
    {
        size = n;
        int bits = 0; while ((1 << bits) < n) ++bits;
        rev.assign ((size_t) n, 0);
        for (int i = 0; i < n; ++i)
        {
            int r = 0; for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        cosT.resize ((size_t) n / 2); sinT.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i) { cosT[(size_t) i] = std::cos (2.0 * M_PI * i / n); sinT[(size_t) i] = -std::sin (2.0 * M_PI * i / n); }
    }

    void perform (double* re, double* im) const noexcept
    {
        for (int i = 0; i < size; ++i)
        {
            const int r = rev[(size_t) i];
            if (r > i) { std::swap (re[i], re[r]); std::swap (im[i], im[r]); }
        }
        for (int len = 2; len <= size; len <<= 1)
        {
            const int half = len / 2, step = size / len;
            for (int i = 0; i < size; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const double wr = cosT[(size_t) (j * step)], wi = sinT[(size_t) (j * step)];
                    const int a = i + j, b = a + half;
                    const double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
        }
    }

private:
    int size = 0;
    std::vector<int> rev;
    std::vector<double> cosT, sinT;
};

//------------------------------------------------------------------------------
// What the UI shows: the learned source and bleed spectra, the body band and the transient.
struct SpectrumSnapshot
{
    static constexpr int kPoints = 96;
    static constexpr float kMinHz = 30.0f, kMaxHz = 16000.0f;
    float sourceDb[kPoints] {};
    float bleedDb[kPoints] {};
    float bodyHz = 0.0f;        // top of the body band (0 = none)
    float transientMs = 0.0f;   // learned attack length (before the Transient knob)
    int hitsLearned = 0;
    bool bleedLearned = false;
};

//------------------------------------------------------------------------------
class SpectralDecay
{
public:
    static constexpr int kMaxCh = 8;

    struct Settings
    {
        bool  on        = false;
        float focus     = 50.0f;   // 0..100 %: how fast the highs close after the transient
        float sustain   = 0.0f;    // 0..100 %: how much longer the low end (body band) rings
        float transient = 0.0f;    // -100..+100 %: shorter / longer than the learned attack
    };

    void prepare (double sampleRate, int maxChannels)
    {
        sr = sampleRate;
        numCh = std::clamp (maxChannels, 1, kMaxCh);
        fftSize = 1; while (fftSize < (int) std::lround (0.085 * sr)) fftSize <<= 1;
        fft.prepare (fftSize);
        capture.assign ((size_t) fftSize, 0.0f);
        window.resize ((size_t) fftSize);
        for (int i = 0; i < fftSize; ++i) window[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / fftSize);
        re.assign ((size_t) fftSize, 0.0); im.assign ((size_t) fftSize, 0.0);
        srcPow.assign ((size_t) fftSize / 2 + 1, 0.0);
        bleedPow.assign ((size_t) fftSize / 2 + 1, 0.0);
        captureDelay = (int) std::lround (0.010 * sr);
        bleedSettle = (int) std::lround (0.020 * sr);
        bleedEvery = (int) std::lround (0.250 * sr);
        bodyEnvRel = std::exp (-1.0 / (0.030 * sr));     // smooth enough for a 40 Hz body
        hfEnvRel = std::exp (-1.0 / (0.005 * sr));
        hfHold = (int) std::lround (0.002 * sr);
        floorFall = std::exp (-1.0 / (0.050 * sr));
        floorRise = std::exp (-1.0 / (3.0 * sr));
        fcSmooth = std::exp (-1.0 / (0.004 * sr));
        fadeStep = 1.0 / std::max (1.0, 0.010 * sr);
        closeLen = std::max (1, (int) std::lround (0.003 * sr));
        fTop = std::min (20000.0, 0.45 * sr);
        hfSplit.setup (1000.0, sr, 0.5411961); hfSplit2.setup (1000.0, sr, 1.3065630);   // 4-pole: the body doesn't leak in
        reset();
    }

    // Also forgets everything learned.
    void reset()
    {
        for (int c = 0; c < kMaxCh; ++c) { lp[c].reset(); mv1[c].reset(); mv2[c].reset(); }
        hfSplit.reset(); hfSplit2.reset(); holdH = 0;
        bodyHz = 0.0; pendingBodyHz = -1.0; fundHz = 0.0;
        haveBody = false;
        std::fill (srcPow.begin(), srcPow.end(), 0.0);
        std::fill (bleedPow.begin(), bleedPow.end(), 0.0);
        hits = bleeds = 0;
        capMode = idle; capPos = 0; capWait = -1;
        closedRun = 0; sinceBleed = bleedEvery;
        phase = tail; trLeft = 0; closeLeft = 0;
        rampLeft = rampLen = 0; rampFromH = 1.0; rampFromFc = fTop;
        h = 1.0; fc = fcTarget = fTop; retuneIn = 0;
        envB = peakB = 0.0; floorB = -200.0; own = 0.0; holding = false;
        envH = peakH = 0.0; floorH = -200.0; measuring = false; measPos = 0;
        learnedTrMs = 0.0; trCount = 0;
        setMoving (fTop);
        mix = set.on ? 1.0 : 0.0;
    }

    void setSettings (const Settings& ns, float releaseMsIn, float rangeDbIn) noexcept
    {
        set = ns;
        const double rel = std::max (1.0, (double) releaseMsIn);
        focusK = 1.0 * std::clamp (set.focus * 0.01, 0.0, 1.0);           // octaves per dB of body decay
        span = rangeDbIn <= -80.0f ? 80.0 : std::max (0.0, -(double) rangeDbIn);
        const double S = std::clamp (set.sustain * 0.01, 0.0, 1.0);
        sustainBlend = std::min (1.0, S * 10.0);
        ringLimit = std::pow (10.0, -60.0 * S / 20.0);   // Sustain 25 %: ring down to -15 dB, 50 %: -30, 100 %: -60
        bodyRel = std::pow (10.0, -span / (20.0 * std::max (1.0, rel * 0.001 * sr * (1.0 + 3.0 * S))));
        trScale = std::pow (2.0, std::clamp (set.transient * 0.01, -1.0, 1.0));
    }

    //--------------------------------------------------------------------------
    // One output sample. x: the delayed audio (in), the gated audio (out).
    //   g, gFloor:   the gate's gain and floor at this sample
    //   onsetNow:    a hit's first sample is here     onsetAhead: one arrives in 'ramp' samples
    //   gateClosed:  the gate is shut (for learning the bleed)
    inline void tick (float* x, int channels, float g, float gFloor, bool onsetNow, bool onsetAhead, int ramp, bool gateClosed) noexcept
    {
        channels = std::min (channels, numCh);
        float in[kMaxCh];
        for (int c = 0; c < channels; ++c) in[c] = x[c];

        //---------------- filters (always running, so switching never clicks)
        double low[kMaxCh], mov[kMaxCh];
        double bAbs = 0.0, mono = 0.0;
        for (int c = 0; c < channels; ++c)
        {
            double unused;
            low[c] = haveBody ? lp[c].tick (x[c], unused) : 0.0;
            mov[c] = mv2[c].tick (mv1[c].tick (x[c], unused), unused);
            bAbs = std::max (bAbs, std::abs (haveBody ? low[c] : (double) x[c]));
            mono += x[c];
        }
        mono /= channels;
        double hfA, hfOut; hfSplit.tick (mono, hfA); hfSplit2.tick (hfA, hfOut);

        // body envelope (drives the moving cutoff and the Sustain ring) and its bleed floor
        envB = bAbs > envB ? bAbs : bAbs + (envB - bAbs) * bodyEnvRel;
        // peak with a 2 ms hold, then a 5 ms release: follows the attack's level, not its waveform
        const double aH = std::abs (hfOut);
        if (aH >= envH) { envH = aH; holdH = hfHold; }
        else if (holdH > 0) --holdH;
        else envH = aH + (envH - aH) * hfEnvRel;
        if (gateClosed)
        {
            // minimum tracking: a source tail still ringing when the gate shuts isn't bleed
            trackFloor (floorB, envB);
            trackFloor (floorH, envH);
        }

        //---------------- hit timing
        const double trSamples = std::clamp (transientMs() * trScale, 1.0, 100.0) * 0.001 * sr;
        if (onsetNow)
        {
            if (pendingBodyHz >= 0.0) applyBody();       // a new body band switches in on an attack (gains equal)
            phase = transient; trLeft = (int) std::lround (trSamples); closeLeft = 0;
            peakB = envB; holding = true; own = 1.0;
            if (rampLeft == 0 && h < 1.0) startRamp (ramp);   // no lookahead: open right after
            measuring = true; measPos = 0; peakH = envH; preH = envH;   // learn this hit's attack length
            if (capMode == source || capMode == bleed) capMode = idle;
            capWait = captureDelay;
        }
        else if (onsetAhead && rampLeft == 0 && (h < 1.0 || fc < fTop * 0.99))
        {
            startRamp (ramp);                              // fully open exactly when the transient arrives
            phase = transient; trLeft = std::max (1, ramp) + (int) std::lround (trSamples); closeLeft = 0;
        }

        //---------------- the highs: open through the transient, then close from the top down
        bool untouched = false;   // this sample is still inside the attack window
        if (phase == transient)
        {
            peakB = std::max (peakB, envB);
            untouched = true;
            if (--trLeft <= 0) { phase = tail; closeLeft = focusK > 0.0 ? closeLen : 0; }
        }
        if (rampLeft > 0)
        {
            --rampLeft;
            const double p = 1.0 - (double) rampLeft / (double) rampLen, w = 0.5 - 0.5 * std::cos (M_PI * p);
            h = rampFromH + (1.0 - rampFromH) * w;
            fcTarget = fc = std::exp (std::log (rampFromFc) + (std::log (fTop) - std::log (rampFromFc)) * w);
            if (rampLeft == 0) { h = 1.0; fc = fcTarget = fTop; }
            retuneIn = 0;
        }
        else if (phase == tail && focusK > 0.0 && ! untouched)
        {
            if (closeLeft > 0) { --closeLeft; h = (double) closeLeft / closeLen; }   // 3 ms fade into the closed highs
            else h = 0.0;
            const double fallenDb = peakB > 1e-12 ? std::min (0.0, 20.0 * std::log10 (std::max (envB, 1e-12) / peakB)) : 0.0;
            fcTarget = std::max (fcMin(), fTop * std::pow (2.0, focusK * fallenDb));
            fc = fcTarget + (fc - fcTarget) * fcSmooth;
        }
        else if (focusK <= 0.0) { h = 1.0; fc = fcTarget = fTop; }
        if (--retuneIn <= 0) { setMoving (fc); retuneIn = 16; }

        //---------------- the body: follows the gate; with Sustain it rings on
        float gB = g;
        if (haveBody && phase == tail && ! untouched && sustainBlend > 0.0)
        {
            if (holding)
            {
                const double floorLin = floorB < -150.0 ? 0.0 : std::pow (10.0, floorB / 20.0);
                if (! (envB > std::max (2.0 * floorLin, peakB * ringLimit))) holding = false;   // into its bleed floor, or as far as Sustain allows
            }
            if (! holding) own *= bodyRel;
            gB = (float) std::max ((double) g, g + sustainBlend * (std::max ((double) g, own) - g));
        }
        const float gH = std::max (gFloor, std::min (g, (float) (g * h)));

        //---------------- output
        const bool wantOn = set.on;
        if (wantOn && mix < 1.0)        mix = std::min (1.0, mix + fadeStep);
        else if (! wantOn && mix > 0.0) mix = std::max (0.0, mix - fadeStep);
        for (int c = 0; c < channels; ++c)
        {
            const float plain = x[c] * g;   // exactly the gate's own output
            if (mix <= 0.0) { x[c] = plain; continue; }
            float y = plain;
            if (haveBody) y += (gB - g) * (float) low[c];                // exactly 0 when the gains are equal
            y += (gH - g) * (float) (x[c] - mov[c]);                    // exactly 0 when the gains are equal
            x[c] = mix >= 1.0 ? y : plain + (float) mix * (y - plain);
        }

        //---------------- learning
        learnTransient (gateClosed);
        learn (in, channels, gateClosed, g);
    }

    SpscFifo<SpectrumSnapshot, 4> snapshots;   // audio -> UI

    // read-only access for tests and the UI
    int hitsLearned() const noexcept { return hits; }
    int bleedsLearned() const noexcept { return bleeds; }
    float bodyTopHz() const noexcept { return (float) bodyHz; }          // 0 = no body band
    double fundamentalHz() const noexcept { return fundHz; }            // 0 = not learned / a high source
    double transientMs() const noexcept { return trCount > 0 ? learnedTrMs : 20.0; }   // learned attack (20 ms until learned)
    double cutoffHz() const noexcept { return fc; }
    double highGain() const noexcept { return h; }
    double sourceDbAt (double hz) const noexcept { const size_t i = (size_t) std::lround (hz * fftSize / sr); return 10.0 * std::log10 (std::max (srcPow[std::min (i, srcPow.size() - 1)], 1e-30)); }
    double bleedDbAt (double hz) const noexcept { const size_t i = (size_t) std::lround (hz * fftSize / sr); return 10.0 * std::log10 (std::max (bleedPow[std::min (i, bleedPow.size() - 1)], 1e-30)); }

private:
    enum CapMode { idle, source, bleed };
    enum Phase { tail, transient };

    void trackFloor (double& floorDb, double env) noexcept
    {
        const double d = 20.0 * std::log10 (std::max (env, 1e-10));
        if (floorDb < -150.0)  floorDb = d;
        else if (d < floorDb)  floorDb = d + (floorDb - d) * floorFall;
        else                   floorDb = d + (floorDb - d) * floorRise;
    }

    double fcMin() const noexcept { return haveBody ? std::min (1500.0, bodyHz * 1.25) : 300.0; }

    void setMoving (double f) noexcept
    {
        for (int c = 0; c < numCh; ++c) { mv1[c].setup (f, sr, 0.5411961); mv2[c].setup (f, sr, 1.3065630); }   // 4-pole Butterworth
    }

    void startRamp (int ramp) noexcept
    {
        rampFromH = h; rampFromFc = std::max (20.0, fc);
        rampLen = rampLeft = std::max (1, ramp);
    }

    // The attack length: how long after the hit the high band takes to fall 18 dB under its
    // peak (or into its bleed floor). Averaged over hits.
    void learnTransient (bool gateClosed) noexcept
    {
        if (! measuring) return;
        ++measPos;
        peakH = std::max (peakH, envH);
        const double floorLin = floorH < -150.0 ? 0.0 : std::pow (10.0, floorH / 20.0);
        const int minPos = (int) (0.002 * sr), maxPos = (int) (0.050 * sr);
        bool done = false;
        // over when the attack band has fallen 12 dB under its peak (or into its bleed floor)
        if (measPos >= minPos && (envH < peakH * 0.2512 || envH < 2.0 * floorLin)) done = true;
        if (measPos >= maxPos) done = true;
        if (gateClosed) { measuring = false; return; }     // gate shut before the attack ended: not a usable hit
        if (! done) return;
        measuring = false;
        if (peakH < 2.5 * preH) return;   // the hit barely rose out of what was already ringing (wash): can't tell
        if (measPos >= maxPos) return;    // never settled (a crash on it): not the drum's attack
        // the median of the last 15 hits: a hit with a crash on it (the high band never settles)
        // is an outlier, not the drum's attack
        trHist[(size_t) (trCount % kTrHist)] = std::clamp (measPos * 1000.0 / sr, 3.0, 50.0);
        ++trCount;
        const int n = std::min (trCount, kTrHist);
        std::array<double, kTrHist> tmp {};
        std::copy (trHist.begin(), trHist.begin() + n, tmp.begin());
        std::nth_element (tmp.begin(), tmp.begin() + n / 2, tmp.begin() + n);
        learnedTrMs = tmp[(size_t) (n / 2)];
    }

    void learn (const float* in, int channels, bool gateClosed, float g) noexcept
    {
        float m = 0.0f;
        for (int c = 0; c < channels; ++c) m += in[c];
        m /= (float) channels;

        if (capWait >= 0 && --capWait < 0 && g >= 0.5f) { capMode = source; capPos = 0; }
        closedRun = gateClosed ? closedRun + 1 : 0;
        ++sinceBleed;
        if (capMode == idle && capWait < 0 && closedRun >= bleedSettle && sinceBleed >= bleedEvery) { capMode = bleed; capPos = 0; }
        if (capMode == bleed && ! gateClosed) capMode = idle;

        if (capMode != idle)
        {
            capture[(size_t) capPos++] = m;
            if (capPos == fftSize)
            {
                const bool isSource = capMode == source;
                capMode = idle;
                if (isSource) analyse (true);
                else { sinceBleed = 0; analyse (false); }
            }
        }
    }

    void analyse (bool isSource) noexcept
    {
        for (int i = 0; i < fftSize; ++i) { re[(size_t) i] = capture[(size_t) i] * window[(size_t) i]; im[(size_t) i] = 0.0; }
        fft.perform (re.data(), im.data());
        auto& acc = isSource ? srcPow : bleedPow;
        int& count = isSource ? hits : bleeds;
        for (size_t i = 0; i <= (size_t) fftSize / 2; ++i)
        {
            const double p = re[i] * re[i] + im[i] * im[i];
            const double a = count == 0 ? 1.0 : (isSource ? 0.25 : (p < acc[i] ? 0.5 : 0.1));
            acc[i] = acc[i] + a * (p - acc[i]);
        }
        ++count;
        if (hits >= 2) findBody();
        publishSnapshot();
    }

    // The body band: a low source (spectral peak below 1 kHz) keeps everything from the bottom up
    // to where its spectrum (max over a sixth of an octave) has fallen 30 dB under the peak.
    void findBody() noexcept
    {
        const double binHz = sr / fftSize;
        auto dbAt = [&] (int i) { return 10.0 * std::log10 (std::max (srcPow[(size_t) i], 1e-30)); };
        const int lo = std::max (3, (int) std::ceil (40.0 / binHz)), hi = std::min (fftSize / 2 - 3, (int) (12000.0 / binHz));
        double maxDb = -400.0; int maxBin = lo;
        for (int i = lo; i <= hi; ++i) if (dbAt (i) > maxDb) { maxDb = dbAt (i); maxBin = i; }
        double top = 0.0;
        // the fundamental: the strongest low peak (40 Hz - 1 kHz, within 20 dB of the spectrum's peak) that
        // the bleed doesn't explain - so the kick ringing in a floor tom mic can't claim it
        fundHz = 0.0;
        {
            double best = -1e9; int bestBin = -1;
            for (int i = lo; i <= std::min (hi, (int) (1000.0 / binHz)); ++i)
            {
                const double d = dbAt (i);
                if (d < maxDb - 20.0 || d < dbAt (i - 1) || d < dbAt (i + 1)) continue;
                const double over = bleeds > 0 ? d - 10.0 * std::log10 (std::max (bleedPow[(size_t) i], 1e-30)) : d;
                if (over > best) { best = over; bestBin = i; }
            }
            if (bestBin > 0) fundHz = bestBin * binHz;
        }
        if (maxBin * binHz < 1000.0)
        {
            const double sixth = std::pow (2.0, 1.0 / 6.0);
            int last = maxBin;
            for (int i = maxBin; i <= std::min (hi, (int) (1200.0 / binHz)); ++i)
            {
                double m = -400.0;
                for (int j = std::max (lo, (int) (i / sixth)); j <= std::min (hi, (int) std::ceil (i * sixth)); ++j) m = std::max (m, dbAt (j));
                if (m >= maxDb - 30.0) last = i; else break;
            }
            top = last * binHz;
        }
        pendingBodyHz = top;
        if (! haveBody && bodyHz <= 0.0) applyBody();   // nothing to switch over from: now
    }

    void applyBody() noexcept
    {
        const double nb = pendingBodyHz;
        pendingBodyHz = -1.0;
        if (nb <= 0.0) { haveBody = false; bodyHz = 0.0; return; }
        const double xo = std::min (1500.0, nb * 1.25);   // crossover a little above the body's top
        if (! haveBody || std::abs (std::log2 (xo / std::max (1.0, bodyHz * 1.25))) > 0.1)
            for (int c = 0; c < numCh; ++c) { lp[c].setup (xo, sr, 0.70710678); if (! haveBody) lp[c].reset(); }
        bodyHz = nb; haveBody = true;
    }

    void publishSnapshot() noexcept
    {
        SpectrumSnapshot snap;
        const double binHz = sr / fftSize;
        for (int p = 0; p < SpectrumSnapshot::kPoints; ++p)
        {
            const double f = SpectrumSnapshot::kMinHz * std::pow ((double) SpectrumSnapshot::kMaxHz / SpectrumSnapshot::kMinHz, (p + 0.5) / SpectrumSnapshot::kPoints);
            const int a = std::max (1, (int) (f / std::pow (2.0, 1.0 / 24.0) / binHz)), b = std::min (fftSize / 2, std::max (a, (int) (f * std::pow (2.0, 1.0 / 24.0) / binHz)));
            double sv = 0, bl = 0;
            for (int i = a; i <= b; ++i) { sv = std::max (sv, srcPow[(size_t) i]); bl = std::max (bl, bleedPow[(size_t) i]); }
            snap.sourceDb[p] = (float) (10.0 * std::log10 (std::max (sv, 1e-30)));
            snap.bleedDb[p]  = (float) (10.0 * std::log10 (std::max (bl, 1e-30)));
        }
        snap.bodyHz = (float) (pendingBodyHz >= 0.0 ? pendingBodyHz : bodyHz);
        snap.transientMs = (float) transientMs();
        snap.hitsLearned = hits;
        snap.bleedLearned = bleeds > 0;
        snapshots.push (snap);
    }

    Settings set;
    double sr = 48000.0;
    int numCh = 2;

    // filters
    Svf lp[kMaxCh];                  // the body band
    Svf mv1[kMaxCh], mv2[kMaxCh];    // the moving low-pass (4-pole)
    Svf hfSplit, hfSplit2;           // > 1 kHz (4-pole), for learning the attack
    int holdH = 0, hfHold = 96;
    double bodyHz = 0.0, pendingBodyHz = -1.0, fundHz = 0.0;
    bool haveBody = false;

    // state
    Phase phase = tail;
    int trLeft = 0, closeLeft = 0, closeLen = 144, rampLeft = 0, rampLen = 0, retuneIn = 0;
    double h = 1.0, fc = 20000.0, fcTarget = 20000.0, fTop = 20000.0, rampFromH = 1.0, rampFromFc = 20000.0;
    double envB = 0.0, peakB = 0.0, floorB = -200.0, own = 0.0;
    bool holding = false;
    double envH = 0.0, peakH = 0.0, preH = 0.0, floorH = -200.0;
    bool measuring = false; int measPos = 0;
    static constexpr int kTrHist = 15;
    std::array<double, kTrHist> trHist {};
    double learnedTrMs = 0.0; int trCount = 0;
    double bodyEnvRel = 0.999, hfEnvRel = 0.98, floorFall = 0.9996, floorRise = 0.999993, fcSmooth = 0.995;
    double span = 80.0, focusK = 0.0, bodyRel = 0.999, sustainBlend = 0.0, ringLimit = 1.0, trScale = 1.0;
    double mix = 0.0, fadeStep = 0.002;

    // learning
    Fft fft;
    int fftSize = 4096;
    std::vector<float> capture;
    std::vector<double> window, re, im, srcPow, bleedPow;
    CapMode capMode = idle;
    int capPos = 0, capWait = -1, captureDelay = 480;
    int closedRun = 0, sinceBleed = 0, bleedSettle = 960, bleedEvery = 12000;
    int hits = 0, bleeds = 0;
};

} // namespace tg
