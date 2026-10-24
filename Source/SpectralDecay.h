#pragma once
//==============================================================================
// The Gate - spectral decay (Phase 3).
//
// The gate's tail, shaped by what the source actually is. Plain C++, no JUCE.
//
// LEARNING (automatic, from the track itself)
//   Source: ~85 ms of each hit's body (starting 10 ms after the hit, past the attack) is
//   analysed (FFT) and averaged over hits. Its strongest resonances - a tom's fundamental and
//   overtones, a kick's body, a snare's shell ring - become the source's PARTIALS (up to 8):
//   frequency and width, each at least 1/6 octave from the next, each a real peak (the spectrum
//   drops 9 dB on both sides within a third of an octave - noise bumps don't), within 30 dB of
//   the strongest, and at least 10 dB above the bleed.
//   Bleed: the same analysis runs while the gate is shut (everything the mic hears that isn't
//   the source), plus each partial's own bleed floor.
//   New partials are learned in the background and switched in at the next hit's attack.
//
// THE TAIL
//   The attack is exactly the plain gate (all gains equal, so the output IS x * gate gain):
//   the transient is never filtered. 5 ms after each hit the tail splits in two:
//     partials - resonant band-passes locked to the source's partials (0 dB and 0 degrees at
//                their centre). They follow the gate, and with Sustain they keep ringing past it:
//                each partial rings down as far as Sustain allows (25 % = 15 dB below its peak,
//                50 % = 30 dB, 100 % = 60 dB), never into its bleed floor (+6 dB),
//                then releases up to 4x
//                slower than the gate's Release: the decay extension.
//     the rest - everything else: cymbal wash, snare buzz, room. With Focus it closes faster
//                than the gate (at 100 %, through the whole range in 1/20 of the Release).
//   output = rest gain * x + sum over partials of (partial gain - rest gain) * w * bandpass(x)
//   (w: weights so the overlapping partial filters sum to exactly 0 dB at every partial)
//   So when every gain is equal (Focus 0, Sustain 0, or during the attack) the output is
//   exactly x * gate gain: zero latency, no phase shift, bit-identical to the plain gate.
//   When a new stroke comes in while the rest is closed, the rest ramps back up so it's fully
//   open exactly when the transient arrives (using the lookahead), like the gate's own attack.
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
struct PartialInfo { float hz = 0.0f, q = 0.0f, levelDb = -120.0f; };

// What the UI shows: the learned source and bleed spectra and the partials.
struct SpectrumSnapshot
{
    static constexpr int kPoints = 96;
    static constexpr float kMinHz = 30.0f, kMaxHz = 16000.0f;
    float sourceDb[kPoints] {};
    float bleedDb[kPoints] {};
    PartialInfo partials[8] {};
    int numPartials = 0, hitsLearned = 0;
    bool bleedLearned = false;
};

//------------------------------------------------------------------------------
class SpectralDecay
{
public:
    static constexpr int kMaxPartials = 8;
    static constexpr int kMaxCh = 8;

    struct Settings
    {
        bool  on      = false;
        float focus   = 50.0f;   // 0..100 %: how fast everything that isn't the source closes
        float sustain = 0.0f;    // 0..100 %: how much longer the source's partials ring
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
        splitLen = (int) std::lround (0.005 * sr);
        captureDelay = (int) std::lround (0.010 * sr);
        bleedSettle = (int) std::lround (0.020 * sr);
        bleedEvery = (int) std::lround (0.250 * sr);
        envRel = std::exp (-1.0 / (0.010 * sr));
        floorFall = std::exp (-1.0 / (0.050 * sr));
        floorRise = std::exp (-1.0 / (3.0 * sr));
        fadeStep = 1.0 / std::max (1.0, 0.010 * sr);
        reset();
    }

    // Also forgets everything learned.
    void reset()
    {
        for (auto& ch : bp) for (auto& f : ch) f.reset();
        for (auto& f : lp) f.reset();
        bodyHz = pendingBodyHz = 0.0;
        for (int k = 0; k < kMaxPartials; ++k) { env[k] = peak[k] = 0.0; floorLog[k] = -200.0; own[k] = 0.0; holding[k] = false; }
        numPartials = 0;
        pendingCount = -1;
        std::fill (srcPow.begin(), srcPow.end(), 0.0);
        std::fill (bleedPow.begin(), bleedPow.end(), 0.0);
        hits = bleeds = 0;
        capMode = idle; capPos = 0; capWait = -1;
        closedRun = 0; sinceBleed = bleedEvery;
        phase = tail; splitLeft = 0; sinceSplit = 0; rampLeft = rampLen = 0; rampFrom = 1.0f;
        gRest = 1.0f;
        mix = set.on ? 1.0 : 0.0;
    }

    void setSettings (const Settings& ns, float releaseMsIn, float rangeDbIn) noexcept
    {
        set = ns;
        const double rel = std::max (1.0, (double) releaseMsIn);
        const double F = std::clamp (set.focus * 0.01, 0.0, 1.0);
        span = rangeDbIn <= -80.0f ? 80.0 : std::max (0.0, -(double) rangeDbIn);
        // the rest: F x span dB per (Release x (1 - 0.95 F)), so Focus 0 = never faster than the gate
        restDbPerSample = F * span / (std::max (0.003 * sr, rel * 0.001 * sr * (1.0 - 0.95 * F)));
        const double S = std::clamp (set.sustain * 0.01, 0.0, 1.0);
        sustainBlend = std::min (1.0, S * 10.0);
        ringLimit = std::pow (10.0, -60.0 * S / 20.0);   // Sustain 25 %: ring down to -15 dB, 50 %: -30, 100 %: -60
        partialRel = std::pow (10.0, -span / (20.0 * std::max (1.0, rel * 0.001 * sr * (1.0 + 3.0 * S))));
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

        //---------------- the body band (always running): everything below the body crossover
        double lpOut[kMaxCh];
        for (int c = 0; c < channels; ++c) { double hpUnused; lpOut[c] = bodyHz > 0.0 ? lp[c].tick (x[c], hpUnused) : 0.0; }

        //---------------- partial band-passes (always running) and their envelopes
        double bpOut[kMaxPartials][kMaxCh];
        for (int k = 0; k < numPartials; ++k)
        {
            double a = 0.0;
            for (int c = 0; c < channels; ++c)
            {
                bpOut[k][c] = bp[c][k].bandPass (x[c]);
                a = std::max (a, std::abs (bpOut[k][c]));
            }
            env[k] = a > env[k] ? a : a + (env[k] - a) * envRel;
            if (gateClosed)
            {
                // minimum tracking: falls fast to the quietest level between hits, creeps up slowly,
                // so a source tail still ringing when the gate shuts can't pass itself off as bleed
                const double d = lin2db (env[k]);
                if (floorLog[k] < -150.0)  floorLog[k] = d;
                else if (d < floorLog[k])  floorLog[k] = d + (floorLog[k] - d) * floorFall;
                else                       floorLog[k] = d + (floorLog[k] - d) * floorRise;
            }
        }

        //---------------- hit timing
        if (onsetNow)
        {
            if (pendingCount >= 0) applyPending();   // new partials switch in on an attack (all gains equal here)
            phase = hold; splitLeft = splitLen;
            for (int k = 0; k < numPartials; ++k) { own[k] = 1.0; peak[k] = 0.0; holding[k] = true; }
            if (rampLeft == 0 && gRest < g) { rampFrom = gRest; rampLen = rampLeft = std::max (1, ramp); }   // no lookahead: ramp after
            if (capMode == source || capMode == bleed) capMode = idle;   // the next hit cuts the capture off
            capWait = captureDelay;
        }
        else if (onsetAhead && gRest < 1.0f && rampLeft == 0)
        {
            // a stroke is coming while the rest is closed: ramp it back up so it's fully open
            // exactly when the transient arrives (like the gate's own attack)
            rampFrom = gRest; rampLen = rampLeft = std::max (1, ramp);
            phase = hold; splitLeft = rampLen + splitLen;
        }

        //---------------- the rest's gain
        float base;
        if (phase == hold)
        {
            base = g;
            if (--splitLeft <= 0) { phase = tail; sinceSplit = 0; }
        }
        else
        {
            ++sinceSplit;
            const double restDb = -restDbPerSample * (double) sinceSplit;
            base = std::min (g, std::max (gFloor, restDb <= -span ? gFloor : (float) std::pow (10.0, restDb / 20.0)));
        }
        if (rampLeft > 0)
        {
            --rampLeft;
            const double p = 1.0 - (double) rampLeft / (double) rampLen;
            gRest = std::min (base, (float) (rampFrom + (1.0 - rampFrom) * (0.5 - 0.5 * std::cos (M_PI * p))));
        }
        else gRest = base;

        //---------------- each partial's gain: follows the gate; with Sustain it rings on
        float gk[kMaxPartials];
        for (int k = 0; k < numPartials; ++k)
        {
            peak[k] = std::max (peak[k], env[k]);
            if (phase != hold && holding[k])
            {
                const double floorLin = floorLog[k] < -150.0 ? 0.0 : db2lin (floorLog[k]);
                if (! (env[k] > std::max (2.0 * floorLin, peak[k] * ringLimit))) holding[k] = false;   // into its bleed floor, or rung as far as Sustain allows
            }
            if (! holding[k]) own[k] *= partialRel;
            const double sus = std::max ((double) g, own[k]);
            gk[k] = phase == hold ? g   // the attack is exactly the plain gate
                                  : std::max (gRest, std::max (gFloor, (float) (g + sustainBlend * (sus - g))));
        }

        //---------------- output
        const bool wantOn = set.on;
        if (wantOn && mix < 1.0)        mix = std::min (1.0, mix + fadeStep);
        else if (! wantOn && mix > 0.0) mix = std::max (0.0, mix - fadeStep);
        for (int c = 0; c < channels; ++c)
        {
            const float plain = x[c] * g;   // exactly the gate's own output
            if (mix <= 0.0) { x[c] = plain; continue; }
            // the rest closes at gRest; the source's body band follows the gate exactly like the plain
            // gate (so the drum keeps its weight); partials ring relative to whichever band they sit in
            float y = x[c] * gRest;
            if (bodyHz > 0.0) y += (g - gRest) * (float) lpOut[c];
            for (int k = 0; k < numPartials; ++k)
            {
                const float base = inBody[k] ? g : gRest;
                y += (gk[k] - base) * weight[k] * (float) bpOut[k][c];   // exactly 0 when the gains are equal
            }
            x[c] = mix >= 1.0 ? y : plain + (float) mix * (y - plain);
        }

        //---------------- learning: captures of the source (hit bodies) and the bleed (gate shut)
        learn (in, channels, gateClosed, g);
    }

    SpscFifo<SpectrumSnapshot, 4> snapshots;   // audio -> UI

    // read-only access for tests and the UI
    int numLearnedPartials() const noexcept { return numPartials; }
    float partialHz (int k) const noexcept { return k < numPartials ? partials[k].hz : 0.0f; }
    int hitsLearned() const noexcept { return hits; }
    int bleedsLearned() const noexcept { return bleeds; }
    float restGain() const noexcept { return gRest; }
    float bodyTopHz() const noexcept { return (float) bodyHz; }   // 0 = no body band (a high source)
    // the learned source / bleed spectrum at a frequency (dB, for tests and analysis)
    double sourceDbAt (double hz) const noexcept { const size_t i = (size_t) std::lround (hz * fftSize / sr); return 10.0 * std::log10 (std::max (srcPow[std::min (i, srcPow.size() - 1)], 1e-30)); }
    double bleedDbAt (double hz) const noexcept { const size_t i = (size_t) std::lround (hz * fftSize / sr); return 10.0 * std::log10 (std::max (bleedPow[std::min (i, bleedPow.size() - 1)], 1e-30)); }

private:
    enum CapMode { idle, source, bleed };
    enum Phase { tail, hold };

    static double lin2db (double v) noexcept { return 20.0 * std::log10 (std::max (v, 1e-10)); }
    static double db2lin (double d) noexcept { return std::pow (10.0, d / 20.0); }

    void learn (const float* in, int channels, bool gateClosed, float g) noexcept
    {
        float m = 0.0f;
        for (int c = 0; c < channels; ++c) m += in[c];
        m /= (float) channels;

        // source: start captureDelay after the hit, if the gate is properly open
        if (capWait >= 0 && --capWait < 0 && g >= 0.5f) { capMode = source; capPos = 0; }

        // bleed: once the gate has been shut for 20 ms, at most every 250 ms
        closedRun = gateClosed ? closedRun + 1 : 0;
        ++sinceBleed;
        if (capMode == idle && capWait < 0 && closedRun >= bleedSettle && sinceBleed >= bleedEvery)
        {
            capMode = bleed; capPos = 0;
        }
        if (capMode == bleed && ! gateClosed) capMode = idle;   // the gate opened: not bleed any more

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
            // source: average over the last few hits. bleed: biased toward the quieter captures
            // (the same minimum-tracking idea), so source tails heard while the gate was shut
            // don't count as bleed
            const double a = count == 0 ? 1.0 : (isSource ? 0.25 : (p < acc[i] ? 0.5 : 0.1));
            acc[i] = acc[i] + a * (p - acc[i]);
        }
        ++count;
        if (hits >= 2) pickPartials();
        publishSnapshot();
    }

    // The source's strongest resonances (see the header for the rules).
    void pickPartials() noexcept
    {
        const int nyq = fftSize / 2;
        const double binHz = sr / fftSize;
        auto dbAt = [&] (const std::vector<double>& v, int i) { return 10.0 * std::log10 (std::max (v[(size_t) i], 1e-30)); };
        const int lo = std::max (3, (int) std::ceil (40.0 / binHz)), hi = std::min (nyq - 3, (int) (12000.0 / binHz));
        double maxDb = -400.0;
        int maxBin = lo;
        for (int i = lo; i <= hi; ++i) if (dbAt (srcPow, i) > maxDb) { maxDb = dbAt (srcPow, i); maxBin = i; }

        // the body band: a low source (peak below 400 Hz) keeps everything from the bottom up to where
        // its spectrum (max over a sixth of an octave) has fallen 30 dB under the peak, at most 800 Hz.
        // Real drums aren't a few clean partials: the shell's broad low-mid resonance is the drum's
        // weight, and closing it with the wash made toms thin.
        pendingBodyHz = 0.0;
        if (maxBin * binHz < 400.0)
        {
            const double sixth = std::pow (2.0, 1.0 / 6.0);
            int last = maxBin;
            for (int i = maxBin; i <= std::min (hi, (int) (800.0 / binHz)); ++i)
            {
                double m = -400.0;
                for (int j = std::max (lo, (int) (i / sixth)); j <= std::min (hi, (int) std::ceil (i * sixth)); ++j) m = std::max (m, dbAt (srcPow, j));
                if (m >= maxDb - 30.0) last = i; else break;
            }
            pendingBodyHz = last * binHz;
        }

        struct Cand { int bin; double db; };
        Cand cand[64]; int nc = 0;
        const double third = std::pow (2.0, 1.0 / 3.0);
        for (int i = lo; i <= hi && nc < 64; ++i)
        {
            const double d = dbAt (srcPow, i);
            if (d < maxDb - 30.0) continue;
            if (d < dbAt (srcPow, i - 1) || d < dbAt (srcPow, i + 1) || d < dbAt (srcPow, i - 2) || d < dbAt (srcPow, i + 2)) continue;
            // a resonance, not a noise bump: the spectrum drops at least 9 dB on BOTH sides
            // within a third of an octave (averaged noise wobbles by only a few dB)
            const int a = std::max (1, (int) (i / third)), b = std::min (nyq, (int) std::ceil (i * third));
            double leftMin = d, rightMin = d;
            for (int j = a; j < i; ++j)  leftMin  = std::min (leftMin,  dbAt (srcPow, j));
            for (int j = i + 1; j <= b; ++j) rightMin = std::min (rightMin, dbAt (srcPow, j));
            if (d - leftMin < 9.0 || d - rightMin < 9.0) continue;
            if (bleeds > 0 && d - dbAt (bleedPow, i) < 10.0) continue;           // well above the bleed there
            cand[nc++] = { i, d };
        }
        std::sort (cand, cand + nc, [] (const Cand& p, const Cand& q) { return p.db > q.db; });

        int n = 0;
        PartialInfo out[kMaxPartials];
        for (int c = 0; c < nc && n < kMaxPartials; ++c)
        {
            const int i = cand[c].bin;
            // parabolic peak interpolation
            const double y0 = dbAt (srcPow, i - 1), y1 = dbAt (srcPow, i), y2 = dbAt (srcPow, i + 1);
            const double den = y0 - 2.0 * y1 + y2;
            const double off = std::abs (den) > 1e-12 ? std::clamp (0.5 * (y0 - y2) / den, -0.5, 0.5) : 0.0;
            const double hz = (i + off) * binHz;
            bool tooClose = false;
            for (int k = 0; k < n; ++k)
                if (std::abs (std::log2 (hz / out[k].hz)) < 1.0 / 6.0) tooClose = true;
            if (tooClose) continue;
            // width at -3 dB (within a third of an octave each side)
            const double lim = std::pow (2.0, 1.0 / 3.0);
            int l = i, r = i;
            while (l > (int) (i / lim) && dbAt (srcPow, l) > y1 - 3.0) --l;
            while (r < std::min (nyq, (int) (i * lim)) && dbAt (srcPow, r) > y1 - 3.0) ++r;
            const double bw = std::max (1.0, (double) (r - l)) * binHz;
            out[n++] = { (float) hz, (float) std::clamp (hz / bw, 3.0, 25.0), (float) cand[c].db };
        }
        for (int k = 0; k < n; ++k) pending[k] = out[k];
        pendingCount = n;
        if (numPartials == 0) applyPending();   // nothing playing through partials yet: switch now
    }

    void applyPending() noexcept
    {
        for (int k = 0; k < pendingCount; ++k)
        {
            const bool moved = k >= numPartials || std::abs (std::log2 (pending[k].hz / std::max (1.0f, partials[k].hz))) > 0.04;
            partials[k] = pending[k];
            for (int c = 0; c < numCh; ++c) bp[c][k].setup (partials[k].hz, sr, partials[k].q);
            if (moved) { floorLog[k] = -200.0; env[k] = 0.0; for (int c = 0; c < numCh; ++c) bp[c][k].reset(); }
        }
        numPartials = pendingCount;
        pendingCount = -1;
        const double xo = std::min (1000.0, pendingBodyHz * 1.25);   // crossover a little above the body's top
        if (pendingBodyHz > 0.0 && std::abs (std::log2 (xo / std::max (1.0, bodyHz * 1.25))) > 0.1)
            for (int c = 0; c < numCh; ++c) { lp[c].setup (xo, sr, 0.70710678); if (bodyHz <= 0.0) lp[c].reset(); }
        bodyHz = pendingBodyHz;
        for (int k = 0; k < numPartials; ++k) inBody[k] = bodyHz > 0.0 && partials[k].hz < bodyHz * 1.25;
        solveWeights();
    }

    // Neighbouring partial filters overlap, so their sum would come out a little hot. Real weights
    // w (least squares) make sum_j w_j H_j(f_k) = 1 at every partial's centre: the partials alone
    // reproduce the source at 0 dB.
    void solveWeights() noexcept
    {
        const int n = numPartials;
        double re_[kMaxPartials][kMaxPartials], im_[kMaxPartials][kMaxPartials];
        for (int k = 0; k < n; ++k)
            for (int j = 0; j < n; ++j)
            {
                // band-pass (bilinear, prewarped) at partial k's centre: (jx/Q) / (1 - x^2 + jx/Q)
                const double x = std::tan (M_PI * partials[k].hz / sr) / std::tan (M_PI * partials[j].hz / sr);
                const double q = partials[j].q, ar = 1.0 - x * x, ai = x / q;
                const double den = ar * ar + ai * ai;
                re_[k][j] = (ai * ai) / den;          // (j x/Q) * conj(ar + j ai) / den
                im_[k][j] = (ai * ar) / den;
            }
        // normal equations: M w = b, M = Re(A^H A), b = Re(A^H 1)
        double M[kMaxPartials][kMaxPartials + 1];
        for (int a = 0; a < n; ++a)
        {
            for (int b = 0; b < n; ++b)
            {
                double m = 0.0;
                for (int k = 0; k < n; ++k) m += re_[k][a] * re_[k][b] + im_[k][a] * im_[k][b];
                M[a][b] = m;
            }
            double r = 0.0;
            for (int k = 0; k < n; ++k) r += re_[k][a];
            M[a][n] = r;
        }
        for (int c = 0; c < n; ++c)   // Gaussian elimination, partial pivoting
        {
            int piv = c;
            for (int r = c + 1; r < n; ++r) if (std::abs (M[r][c]) > std::abs (M[piv][c])) piv = r;
            for (int t = 0; t <= n; ++t) std::swap (M[c][t], M[piv][t]);
            if (std::abs (M[c][c]) < 1e-12) continue;
            for (int r = 0; r < n; ++r)
                if (r != c)
                {
                    const double f = M[r][c] / M[c][c];
                    for (int t = c; t <= n; ++t) M[r][t] -= f * M[c][t];
                }
        }
        for (int k = 0; k < n; ++k)
            weight[k] = std::abs (M[k][k]) > 1e-12 ? (float) std::clamp (M[k][n] / M[k][k], 0.3, 1.5) : 1.0f;
    }

    void publishSnapshot() noexcept
    {
        SpectrumSnapshot snap;
        const double binHz = sr / fftSize;
        for (int p = 0; p < SpectrumSnapshot::kPoints; ++p)
        {
            const double f = SpectrumSnapshot::kMinHz * std::pow ((double) SpectrumSnapshot::kMaxHz / SpectrumSnapshot::kMinHz, (p + 0.5) / SpectrumSnapshot::kPoints);
            const int a = std::max (1, (int) (f / std::pow (2.0, 1.0 / 24.0) / binHz)), b = std::min (fftSize / 2, std::max (a, (int) (f * std::pow (2.0, 1.0 / 24.0) / binHz)));
            double s = 0, bl = 0;
            for (int i = a; i <= b; ++i) { s = std::max (s, srcPow[(size_t) i]); bl = std::max (bl, bleedPow[(size_t) i]); }
            snap.sourceDb[p] = (float) (10.0 * std::log10 (std::max (s, 1e-30)));
            snap.bleedDb[p]  = (float) (10.0 * std::log10 (std::max (bl, 1e-30)));
        }
        const int n = pendingCount >= 0 ? pendingCount : numPartials;
        for (int k = 0; k < n; ++k) snap.partials[k] = pendingCount >= 0 ? pending[k] : partials[k];
        snap.numPartials = n;
        snap.hitsLearned = hits;
        snap.bleedLearned = bleeds > 0;
        snapshots.push (snap);
    }

    Settings set;
    double sr = 48000.0;
    int numCh = 2;

    // partials
    Svf bp[kMaxCh][kMaxPartials];
    Svf lp[kMaxCh];                       // the body band
    double bodyHz = 0.0, pendingBodyHz = 0.0;
    bool inBody[kMaxPartials] {};
    PartialInfo partials[kMaxPartials], pending[kMaxPartials];
    int numPartials = 0, pendingCount = -1;
    float weight[kMaxPartials] { 1, 1, 1, 1, 1, 1, 1, 1 };
    double env[kMaxPartials] {}, peak[kMaxPartials] {}, floorLog[kMaxPartials] {}, own[kMaxPartials] {};
    bool holding[kMaxPartials] {};
    double envRel = 0.999, floorFall = 0.9996, floorRise = 0.999993;

    // gains
    Phase phase = tail;
    int splitLen = 240, splitLeft = 0, rampLeft = 0, rampLen = 0;
    int64_t sinceSplit = 0;
    float gRest = 1.0f, rampFrom = 1.0f;
    double span = 80.0, restDbPerSample = 0.0, partialRel = 0.999, sustainBlend = 0.0, ringLimit = 1.0;
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
