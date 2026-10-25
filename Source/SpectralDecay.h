#pragma once
//==============================================================================
// The Gate - spectral decay (Phase 3, rebuilt Oct 25 as BLEED CUT after Corey's listening).
//
// THE RULE: the gate decides everything - when it opens, when it closes, how loud. The spectral
// decay only ever takes away, from inside what the gate lets through:
//
//   output = g x  Cut_1  Cut_2 ... Cut_N        Cut_k = 1 - a_k BandPass_k,  0 <= a_k < 1
//
// (g = the gate's gain.) Each cut is a band-pass subtracted from its input: a band-pass's response
// lies on the circle through 0 and 1, so |1 - a H| <= 1 at every frequency - a cut can never boost
// anything, and chained cuts can't either. With every a_k = 0 it is exactly the plain gate; where the
// gate is shut it is exactly 0. It never opens, extends or re-triggers anything.
//
// WHAT IT TAKES AWAY: what outlasts the drum. The drum's BODY (from the bottom up to where its
// learned spectrum falls 30 dB under its peak) and each hit's ATTACK (its learned length, the
// Transient knob scales it) are never touched. Above the body there are bands (an octave each,
// from the body's top up). From the drum's own hits it learns how fast each band dies after the
// attack (the drum's highs die fast; bleed only ever makes a band look slower, so it takes the
// fast end of what it measures). In each hit's tail, a band that holds more than the drum's own
// decay predicts (+ Sensitivity dB) has bleed in it - hats, a cymbal hanging on - and is cut toward
// the drum's level (Bleed Cut: how much of the excess). A new stroke, even one the gate merges in a
// fast fill, lifts the drum's body: the prediction restarts from it.
//
// Until it has learned (3 hits, and each band's decay), it is exactly the plain gate.
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
// What the UI shows: the learned source and bleed spectra, the body band, the attack length,
// and how much each band is being cut right now.
struct SpectrumSnapshot
{
    static constexpr int kPoints = 96;
    static constexpr int kMaxBands = 6;
    static constexpr float kMinHz = 30.0f, kMaxHz = 16000.0f;
    float sourceDb[kPoints] {};
    float bleedDb[kPoints] {};
    float bodyHz = 0.0f;        // top of the body band (0 = none)
    float transientMs = 0.0f;   // learned attack length (before the Transient knob)
    int numBands = 0;
    float bandLoHz[kMaxBands] {};
    float bandCutDb[kMaxBands] {};   // current cut, dB (0 = untouched)
    int hitsLearned = 0;
    bool bleedLearned = false;
};

//------------------------------------------------------------------------------
class SpectralDecay
{
public:
    static constexpr int kMaxCh = 8;
    static constexpr int kMaxBands = SpectrumSnapshot::kMaxBands;

    struct Settings
    {
        bool  on          = false;
        float cut         = 70.0f;   // 0..100 %: how much of the bleed (the excess over the drum) comes out
        float sensitivity = 6.0f;    // dB: how far above the drum's own level a band must be to count as bleed
        float transient   = 0.0f;    // -100..+100 %: shorter / longer untouched attack than learned
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
        envCoef = std::exp (-1.0 / (0.005 * sr));
        hfEnvRel = std::exp (-1.0 / (0.005 * sr));
        hfHold = (int) std::lround (0.002 * sr);
        floorFall = std::exp (-1.0 / (0.050 * sr));
        floorRise = std::exp (-1.0 / (3.0 * sr));
        fadeStep = 1.0 / std::max (1.0, 0.010 * sr);
        sameHit = (int) std::lround (0.005 * sr);
        cutFall = std::exp (-1.0 / (0.010 * sr / kCtl));   // cuts move smoothly: no time-varying-filter artifacts
        cutRise = std::exp (-1.0 / (0.010 * sr / kCtl));
        bodyRefFall = 20.0 / (sr / kCtl);   // the re-hit reference falls 20 dB per second
        bodyRefRise = 1.0 - std::exp (-1.0 / (0.005 * sr / kCtl));
        hfSplit.setup (1000.0, sr, 0.5411961); hfSplit2.setup (1000.0, sr, 1.3065630);
        reset();
    }

    // Also forgets everything learned.
    void reset()
    {
        for (int c = 0; c < kMaxCh; ++c) for (int k = 0; k < kMaxBands; ++k) cutBp[c][k].reset();
        for (auto& f : anaBp) f.reset();
        bodyLp1.reset(); bodyLp2.reset();
        hfSplit.reset(); hfSplit2.reset(); holdH = 0;
        bodyHz = 0.0; pendingBodyHz = -1.0; numBands = 0;
        std::fill (srcPow.begin(), srcPow.end(), 0.0);
        std::fill (bleedPow.begin(), bleedPow.end(), 0.0);
        hits = bleeds = 0;
        capMode = idle; capPos = 0; capWait = -1;
        closedRun = 0; sinceBleed = bleedEvery;
        phase = tail; trLeft = 0; sinceOnset = 1 << 30; ctl = 0;
        for (int k = 0; k < kMaxBands; ++k) { r[k] = 1.0; bandPow[k] = 0.0; rampFrom[k] = 1.0; startDb[k] = 0.0; rateCount[k] = 0; rate[k] = 0.0; }
        bodyPow = 0.0; bodyRefDb = -200.0; haveStart = false; tailPos = 0; measRate = false;
        rampLeft = rampLen = 0;
        envH = peakH = preH = 0.0; floorH = -200.0; measuring = false; measPos = 0;
        learnedTrMs = 0.0; trCount = 0; trHist = {};
        mix = set.on ? 1.0 : 0.0;
    }

    void setSettings (const Settings& ns, float /*releaseMs*/, float /*rangeDb*/) noexcept
    {
        set = ns;
        cutAmt = std::clamp (set.cut * 0.01, 0.0, 1.0);
        margin = std::clamp ((double) set.sensitivity, 0.0, 24.0);
        trScale = std::pow (2.0, std::clamp (set.transient * 0.01, -1.0, 1.0));
    }

    //--------------------------------------------------------------------------
    // One output sample. x: the delayed audio (in), the gated audio (out).
    //   g:           the gate's gain at this sample (the ceiling for everything)
    //   onsetNow:    a hit's first sample is here     onsetAhead: one arrives in 'ramp' samples
    //   gateClosed:  the gate is shut (for learning the bleed)
    //   strokeNow:   it's the gate's own stroke (learn from it); otherwise a body lift (just stop cutting)
    inline void tick (float* x, int channels, float g, float /*gFloor*/, bool onsetNow, bool onsetAhead, int ramp, bool gateClosed, bool strokeNow = true) noexcept
    {
        channels = std::min (channels, numCh);
        float in[kMaxCh];
        for (int c = 0; c < channels; ++c) in[c] = x[c];

        //---------------- measuring (mono): the body and each band
        double mono = 0.0;
        for (int c = 0; c < channels; ++c) mono += x[c];
        mono /= channels;
        double hfA, hfOut; hfSplit.tick (mono, hfA); hfSplit2.tick (hfA, hfOut);
        if (numBands > 0)
        {
            double u;
            const double body = bodyLp2.tick (bodyLp1.tick (mono, u), u);
            bodyPow = body * body + (bodyPow - body * body) * envCoef;
            for (int k = 0; k < numBands; ++k) { const double v = anaBp[k].bandPass (mono); bandPow[k] = v * v + (bandPow[k] - v * v) * envCoef; }
        }

        // the attack detector's high band (for learning the attack length)
        const double aH = std::abs (hfOut);
        if (aH >= envH) { envH = aH; holdH = hfHold; }
        else if (holdH > 0) --holdH;
        else envH = aH + (envH - aH) * hfEnvRel;
        if (gateClosed) trackFloor (floorH, envH);

        //---------------- hit timing (from the gate's own stroke detection)
        const double trSamples = std::clamp (transientMs() * trScale, 1.0, 100.0) * 0.001 * sr;
        bool learnFromIt = strokeNow;
        if (onsetNow && sinceOnset < sameHit)
        {
            onsetNow = false;                             // the same hit, marked twice
            if (strokeNow && ! lastWasStroke) startLearning();   // the gate's stroke, a moment after a body lift
        }
        if (onsetNow) { sinceOnset = 0; lastWasStroke = strokeNow; } else if (sinceOnset < (1 << 30)) ++sinceOnset;
        if (onsetNow)
        {
            if (pendingBodyHz >= 0.0) applyBody();       // new bands switch in on an attack (all cuts off here)
            phase = transient; trLeft = (int) std::lround (trSamples);
            haveStart = false; measRate = false;
            if (rampLeft == 0) startRamp (1);
            if (learnFromIt) startLearning();
        }
        else if (onsetAhead && rampLeft == 0) startRamp (ramp);   // whole again before the stroke arrives
        bool untouched = false;
        if (phase == transient) { untouched = true; if (--trLeft <= 0) phase = tail; }

        //---------------- per band: is it outlasting the drum? (every kCtl samples)
        if (++ctl >= kCtl)
        {
            ctl = 0;
            const double bodyDb = pdb (bodyPow);
            const bool reHit = false;   // new strokes arrive as marks from the engine's body-lift detector (on the undelayed key)
            if (! untouched && ! haveStart && phase == tail && numBands > 0 && g >= 0.5f)
            {
                for (int k = 0; k < numBands; ++k) startDb[k] = pdb (bandPow[k]);
                haveStart = true; tailPos = 0;
                measRate = ! reHit; measRateAt = (int64_t) std::lround (0.030 * sr / kCtl);
            }
            if (haveStart) ++tailPos;
            // learn the drum's decay per band: 30 ms after the attack, on a clean stretch of tail
            if (measRate && tailPos >= measRateAt)
            {
                measRate = false;
                if (g >= 0.5f)
                    for (int k = 0; k < numBands; ++k)
                    {
                        const double fall = (startDb[k] - pdb (bandPow[k])) / (double) tailPos;   // dB per tick
                        rateHist[(size_t) k][(size_t) (rateCount[k] % kRateHist)] = fall;
                        ++rateCount[k];
                        const int nn = std::min (rateCount[k], kRateHist);
                        std::array<double, kRateHist> tmp {};
                        std::copy (rateHist[(size_t) k].begin(), rateHist[(size_t) k].begin() + nn, tmp.begin());
                        const int q = (nn * 3) / 4;                          // the fast end: bleed only slows it
                        std::nth_element (tmp.begin(), tmp.begin() + q, tmp.begin() + nn);
                        rate[k] = std::max (0.0, tmp[(size_t) q]);
                    }
            }
            const bool ready = hits >= kReadyHits && numBands > 0 && cutAmt > 0.0;
            for (int k = 0; k < numBands; ++k)
            {
                double target = 1.0;
                if (ready && haveStart && ! untouched && rampLeft == 0 && rateCount[k] >= kMinRates)
                {
                    const double expected = startDb[k] - rate[k] * (double) tailPos;   // where the drum alone would be
                    const double excess = pdb (bandPow[k]) - (expected + margin);
                    if (excess > 0.0) target = std::max (kMinR, std::pow (10.0, -cutAmt * excess / 20.0));
                }
                r[k] = target + (r[k] - target) * (target < r[k] ? cutFall : cutRise);
            }
        }
        if (rampLeft > 0)
        {
            --rampLeft;
            const double p = 1.0 - (double) rampLeft / (double) rampLen, w = 0.5 - 0.5 * std::cos (M_PI * p);
            for (int k = 0; k < numBands; ++k) r[k] = rampFrom[k] + (1.0 - rampFrom[k]) * w;
            if (rampLeft == 0) for (int k = 0; k < numBands; ++k) r[k] = 1.0;
        }
        if (untouched) for (int k = 0; k < numBands; ++k) r[k] = 1.0;

        //---------------- output: only ever less than the gate
        const bool wantOn = set.on;
        if (wantOn && mix < 1.0)        mix = std::min (1.0, mix + fadeStep);
        else if (! wantOn && mix > 0.0) mix = std::max (0.0, mix - fadeStep);
        for (int c = 0; c < channels; ++c)
        {
            // the cuts run on every sample (so switching never clicks); a cut of 0 leaves v exactly as is
            double v = x[c];
            for (int k = 0; k < numBands; ++k)
            {
                const double bp = cutBp[c][k].bandPass (v);
                const double a = 1.0 - r[k];
                if (a > 0.0) v -= a * bp;
            }
            const float plain = x[c] * g;   // exactly the gate's own output
            if (mix <= 0.0 || numBands == 0) { x[c] = plain; continue; }
            const float y = (float) v * g;
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
    double transientMs() const noexcept { return trCount > 0 ? learnedTrMs : 20.0; }   // learned attack (20 ms until learned)
    int numCutBands() const noexcept { return numBands; }
    double bandRateDbPerMs (int k) const noexcept { return k < numBands ? rate[k] * sr / kCtl / 1000.0 : 0.0; }
    int bandRatesLearned (int k) const noexcept { return k < numBands ? rateCount[k] : 0; }
    double bandLowHz (int k) const noexcept { return k < numBands ? edgeHz[k] : 0.0; }
    double bandCutDb (int k) const noexcept { return k < numBands ? 20.0 * std::log10 (std::max (r[k], 1e-6)) : 0.0; }
    double sourceDbAt (double hz) const noexcept { const size_t i = (size_t) std::lround (hz * fftSize / sr); return 10.0 * std::log10 (std::max (srcPow[std::min (i, srcPow.size() - 1)], 1e-30)); }
    double bleedDbAt (double hz) const noexcept { const size_t i = (size_t) std::lround (hz * fftSize / sr); return 10.0 * std::log10 (std::max (bleedPow[std::min (i, bleedPow.size() - 1)], 1e-30)); }

private:
    enum CapMode { idle, source, bleed };
    enum Phase { tail, transient };
    static constexpr int kCtl = 16;            // control rate: every 16 samples
    static constexpr int kReadyHits = 3;       // until this many hits are learned: the plain gate
    static constexpr int kMinRates = 5;        // hits measured per band before it may cut
    static constexpr double kMinR = 0.063;     // at most 24 dB of cut in a band (deeper notches bump when they let go)
    static constexpr int kTrHist = 15;

    static double pdb (double p) noexcept { return 10.0 * std::log10 (std::max (p, 1e-20)); }

    void startLearning() noexcept
    {
        measuring = true; measPos = 0; peakH = envH; preH = envH;
        if (capMode == source || capMode == bleed) capMode = idle;
        capWait = captureDelay;
        lastWasStroke = true;
    }

    void trackFloor (double& floorDb, double env) noexcept
    {
        const double d = 20.0 * std::log10 (std::max (env, 1e-10));
        if (floorDb < -150.0)  floorDb = d;
        else if (d < floorDb)  floorDb = d + (floorDb - d) * floorFall;
        else                   floorDb = d + (floorDb - d) * floorRise;
    }

    void startRamp (int ramp) noexcept
    {
        bool any = false;
        for (int k = 0; k < numBands; ++k) { rampFrom[k] = r[k]; any |= r[k] < 1.0; }
        if (! any) return;
        rampLen = rampLeft = std::max (1, ramp);
    }

    // The attack length: how long after the hit the attack band (> 1 kHz) takes to fall 12 dB under
    // its peak; hits buried in wash or that never settle are skipped; median of the last 15 hits.
    void learnTransient (bool gateClosed) noexcept
    {
        if (! measuring) return;
        ++measPos;
        peakH = std::max (peakH, envH);
        const double floorLin = floorH < -150.0 ? 0.0 : std::pow (10.0, floorH / 20.0);
        const int minPos = (int) (0.002 * sr), maxPos = (int) (0.050 * sr);
        bool done = false;
        if (measPos >= minPos && (envH < peakH * 0.2512 || envH < 2.0 * floorLin)) done = true;
        if (measPos >= maxPos) done = true;
        if (gateClosed) { measuring = false; return; }
        if (! done) return;
        measuring = false;
        if (peakH < 2.5 * preH) return;
        if (measPos >= maxPos) return;
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

    // The body: a low source (spectral peak below 1 kHz) keeps everything from the bottom up to where
    // its spectrum (max over a sixth of an octave) has fallen 30 dB under the peak; a high source
    // keeps everything below 500 Hz.
    void findBody() noexcept
    {
        const double binHz = sr / fftSize;
        auto dbAt = [&] (int i) { return 10.0 * std::log10 (std::max (srcPow[(size_t) i], 1e-30)); };
        const int lo = std::max (3, (int) std::ceil (40.0 / binHz)), hi = std::min (fftSize / 2 - 3, (int) (12000.0 / binHz));
        double maxDb = -400.0; int maxBin = lo;
        for (int i = lo; i <= hi; ++i) if (dbAt (i) > maxDb) { maxDb = dbAt (i); maxBin = i; }
        double top = 500.0;
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
            top = std::max (150.0, last * binHz);
        }
        pendingBodyHz = top;
        if (numBands == 0) applyBody();   // nothing to switch over from: now
    }

    void applyBody() noexcept
    {
        const double nb = pendingBodyHz;
        pendingBodyHz = -1.0;
        if (nb <= 0.0) return;
        const bool moved = numBands == 0 || std::abs (std::log2 (nb / std::max (1.0, bodyHz))) > 0.1;
        bodyHz = nb;
        if (! moved) return;
        // bands: an octave each, from a little above the body's top up to ~16 kHz (at most kMaxBands)
        double e[kMaxBands + 1]; int n = 0;
        e[n++] = std::min (1500.0, nb * 1.25);
        while (n <= kMaxBands && e[n - 1] * 2.0 <= std::min (20000.0, 0.45 * sr)) { e[n] = e[n - 1] * 2.0; ++n; }
        numBands = n - 1;
        for (int k = 0; k < numBands; ++k)
        {
            edgeHz[k] = e[k];
            centreHz[k] = std::sqrt (e[k] * e[k + 1]);
            for (int c = 0; c < numCh; ++c) { cutBp[c][k].setup (centreHz[k], sr, 1.41); cutBp[c][k].reset(); }
            anaBp[k].setup (centreHz[k], sr, 1.41); anaBp[k].reset();
            r[k] = 1.0; rateCount[k] = 0; rate[k] = 0.0; bandPow[k] = 0.0;
        }
        edgeHz[numBands] = e[numBands];
        bodyLp1.setup (nb, sr, 0.5411961); bodyLp2.setup (nb, sr, 1.3065630); bodyLp1.reset(); bodyLp2.reset();
        bodyPow = 0.0; bodyRefDb = -200.0; haveStart = false;
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
        snap.numBands = numBands;
        for (int k = 0; k < numBands; ++k) { snap.bandLoHz[k] = (float) edgeHz[k]; snap.bandCutDb[k] = (float) bandCutDb (k); }
        snap.hitsLearned = hits;
        snap.bleedLearned = bleeds > 0;
        snapshots.push (snap);
    }

    Settings set;
    double sr = 48000.0;
    int numCh = 2;

    // the bands
    Svf cutBp[kMaxCh][kMaxBands];      // the cuts (in series, on the audio)
    Svf anaBp[kMaxBands];              // the same bands, measuring the input (mono)
    Svf bodyLp1, bodyLp2;              // the body (mono), for the re-hit check
    double edgeHz[kMaxBands + 1] {}, centreHz[kMaxBands] {};
    int numBands = 0;
    double bodyHz = 0.0, pendingBodyHz = -1.0;

    // cutting
    double bandPow[kMaxBands] {}, bodyPow = 0.0, bodyRefDb = -200.0, bodyRefFall = 0.0, bodyRefRise = 0.05;
    double r[kMaxBands] { 1, 1, 1, 1, 1, 1 }, rampFrom[kMaxBands] { 1, 1, 1, 1, 1, 1 };
    double startDb[kMaxBands] {};      // each band's level as the attack ended
    bool haveStart = false;
    int64_t tailPos = 0;               // control ticks since the attack ended
    // learning each band's decay: its level at the attack's end and 30 ms later, over recent hits
    static constexpr int kRateHist = 31;
    std::array<std::array<double, kRateHist>, kMaxBands> rateHist {};
    int rateCount[kMaxBands] {};
    double rate[kMaxBands] {};         // dB per control tick the drum's band falls (the fast end)
    bool measRate = false; int64_t measRateAt = 0;
    double envCoef = 0.999, cutFall = 0.9, cutRise = 0.97, cutAmt = 0.7, margin = 6.0, trScale = 1.0;
    int ctl = 0, rampLeft = 0, rampLen = 0;

    // hit timing
    Phase phase = tail;
    int trLeft = 0, sinceOnset = 1 << 30, sameHit = 240;
    bool lastWasStroke = false;

    // attack learning
    Svf hfSplit, hfSplit2;
    int holdH = 0, hfHold = 96;
    double envH = 0.0, peakH = 0.0, preH = 0.0, floorH = -200.0, hfEnvRel = 0.99;
    bool measuring = false; int measPos = 0;
    std::array<double, kTrHist> trHist {};
    double learnedTrMs = 0.0; int trCount = 0;
    double floorFall = 0.9996, floorRise = 0.999993;
    double mix = 0.0, fadeStep = 0.002;

    // spectrum learning
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
