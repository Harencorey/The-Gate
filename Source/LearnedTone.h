#pragma once
//==============================================================================
// Learn: the user teaches the gate what its drum sounds like, from a passage they pick.
//
// While Learn is armed, every time the gate opens (Source key) the first 12 ms of that hit are
// measured and kept as "this is my drum"; every stick attack that does NOT open the gate is kept as
// "this is bleed". When Learn is switched off, the two sets become a profile.
// After that, every time the gate opens, the same 12 ms are measured and scored against the
// profile; if it sounds more like the bleed than the drum (by the Strictness setting), the opening
// is taken back before it reaches the output (the lookahead still holds it). The learned check can
// only REMOVE openings the gate made; it never opens the gate.
//
// What is measured (the same as training/session_eval.py, which measured this design):
//   shape  24 log-spaced band levels (40 Hz - 16 kHz) of the first 12 ms, minus their mean
//   new    those bands vs the 12 ms before (what is new in this event)
//   f0     energy at the drum's fundamental vs the whole event
//   attack how far the >1 kHz band jumps vs the 20 ms before
// Overall level is left out on purpose: the threshold already decides level.
// Model: a diagonal Gaussian per class; score = log-likelihood ratio, rescaled so the learned
// bleed averages 0 and the learned hits average 1.
//
// Real-time: prepare() allocates; everything else is allocation-free.
//==============================================================================

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace tg
{

struct ToneProfile
{
    static constexpr int kBands = 24;
    static constexpr int kFeatures = 2 * kBands + 2;
    bool  valid = false;
    int   hits = 0, bleeds = 0;
    float f0Hz = 0.0f;
    float muHit[kFeatures] {}, varHit[kFeatures] {}, muBleed[kFeatures] {}, varBleed[kFeatures] {};
    float scoreBleed = 0.0f, scoreHit = 1.0f;   // mean raw scores of the learned sets (for rescaling)
};

class ToneLearner
{
public:
    static constexpr int NF = ToneProfile::kFeatures, NB = ToneProfile::kBands;
    static constexpr int kMaxHits = 64, kMaxBleeds = 256;
    static constexpr int kMinHits = 4, kMinBleeds = 10;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        n12 = (int) std::lround (0.012 * sr);
        nfft = 1;
        while (nfft < n12) nfft <<= 1;
        win.resize ((size_t) n12);
        for (int i = 0; i < n12; ++i) win[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * M_PI * i / (n12 - 1));
        buf.assign ((size_t) nfft, {});
        tw.resize ((size_t) nfft / 2);
        for (int i = 0; i < nfft / 2; ++i) tw[(size_t) i] = std::polar (1.0, -2.0 * M_PI * i / nfft);
        bandLo.resize (NB); bandHi.resize (NB);
        for (int b = 0; b < NB; ++b)
        {
            const double lo = 40.0 * std::pow (16000.0 / 40.0, (double) b / NB), hi = 40.0 * std::pow (16000.0 / 40.0, (double) (b + 1) / NB);
            bandLo[(size_t) b] = (int) std::ceil (lo * nfft / sr - 1e-9);
            bandHi[(size_t) b] = (int) std::ceil (hi * nfft / sr - 1e-9);   // bins in [lo, hi)
        }
        hitF.assign ((size_t) kMaxHits * NF, 0.0f);
        bleedF.assign ((size_t) kMaxBleeds * NF, 0.0f);
        clearLearning();
    }

    int window() const noexcept { return n12; }
    void clearLearning() noexcept { nHits = nBleeds = 0; }
    int learnedHits() const noexcept { return nHits; }
    int learnedBleeds() const noexcept { return nBleeds; }

    // Measure an event starting at absolute sample e, using n samples (n <= window()).
    // raw/src/hi: rings of the mono key, its fundamental band and its >1 kHz band, indexed t & mask.
    void measure (const float* raw, const float* src, const float* hi, size_t mask, uint64_t e, int n, float* out) noexcept
    {
        n = std::clamp (n, 8, n12);
        double b[NB], bp[NB];
        bands (raw, mask, e, n, b);
        bands (raw, mask, e - (uint64_t) n12, n12, bp);
        double mean = 0.0;
        for (int k = 0; k < NB; ++k) mean += b[k];
        mean /= NB;
        for (int k = 0; k < NB; ++k) { out[k] = (float) (b[k] - mean); out[NB + k] = (float) (b[k] - bp[k]); }
        double tot = 0.0, f0e = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const size_t r = (size_t) ((e + (uint64_t) i) & mask);
            tot += (double) raw[r] * raw[r]; f0e += (double) src[r] * src[r];
        }
        out[2 * NB] = (float) (10.0 * std::log10 (f0e + 1e-12) - 10.0 * std::log10 (tot + 1e-12));
        const int oneMs = (int) std::lround (0.001 * sr), pre = (int) std::lround (0.020 * sr);
        double hNow = 0.0, hPre = 0.0;
        for (int i = -oneMs; i < n; ++i) hNow = std::max (hNow, (double) std::abs (hi[(size_t) ((e + (uint64_t) (int64_t) i) & mask)]));
        for (int i = -pre; i < -oneMs; ++i) hPre = std::max (hPre, (double) std::abs (hi[(size_t) ((e + (uint64_t) (int64_t) i) & mask)]));
        out[2 * NB + 1] = (float) (20.0 * std::log10 ((hNow + 1e-9) / (hPre + 1e-9)));
    }

    void addHit (const float* f) noexcept   { if (nHits < kMaxHits) std::copy (f, f + NF, &hitF[(size_t) nHits++ * NF]); }
    void addBleed (const float* f) noexcept
    {
        if (nBleeds < kMaxBleeds) std::copy (f, f + NF, &bleedF[(size_t) nBleeds++ * NF]);
        else std::copy (f, f + NF, &bleedF[(size_t) (bleedRot++ % kMaxBleeds) * NF]);   // keep the newest
    }

    // Turn what was learned into a profile. Returns false (and leaves p alone) if there isn't enough.
    bool finish (ToneProfile& p, float f0Hz) const noexcept
    {
        if (nHits < kMinHits || nBleeds < kMinBleeds) return false;
        ToneProfile q;
        stats (hitF.data(), nHits, q.muHit, q.varHit);
        stats (bleedF.data(), nBleeds, q.muBleed, q.varBleed);
        q.valid = true; q.hits = nHits; q.bleeds = nBleeds; q.f0Hz = f0Hz;
        double sh = 0.0, sb = 0.0;
        for (int i = 0; i < nHits; ++i) sh += rawScore (q, &hitF[(size_t) i * NF]);
        for (int i = 0; i < nBleeds; ++i) sb += rawScore (q, &bleedF[(size_t) i * NF]);
        q.scoreHit = (float) (sh / nHits); q.scoreBleed = (float) (sb / nBleeds);
        if (! (q.scoreHit > q.scoreBleed + 1e-3f)) return false;   // can't tell them apart
        p = q;
        return true;
    }

    // 0 = sounds like the learned bleed on average, 1 = like the learned hits on average
    static double score (const ToneProfile& p, const float* f) noexcept
    {
        return (rawScore (p, f) - p.scoreBleed) / std::max (1e-3f, p.scoreHit - p.scoreBleed);
    }

private:
    static double rawScore (const ToneProfile& p, const float* f) noexcept
    {
        double s = 0.0;
        for (int k = 0; k < NF; ++k)
        {
            // each feature's say is capped (8 standard deviations), so one odd measurement can't outvote the rest
            const double d1 = f[k] - p.muHit[k], d0 = f[k] - p.muBleed[k];
            s += -0.5 * (std::min (64.0, d1 * d1 / p.varHit[k]) + std::log (p.varHit[k])) + 0.5 * (std::min (64.0, d0 * d0 / p.varBleed[k]) + std::log (p.varBleed[k]));
        }
        return s;
    }

    static void stats (const float* F, int n, float* mu, float* var) noexcept
    {
        for (int k = 0; k < NF; ++k)
        {
            double m = 0.0, v = 0.0;
            for (int i = 0; i < n; ++i) m += F[(size_t) i * NF + k];
            m /= n;
            for (int i = 0; i < n; ++i) { const double d = F[(size_t) i * NF + k] - m; v += d * d; }
            mu[k] = (float) m; var[k] = (float) (v / n + 1.0);   // +1 dB^2: no feature gets infinite trust
        }
    }

    void bands (const float* raw, size_t mask, uint64_t e, int n, double* out) noexcept
    {
        std::fill (buf.begin(), buf.end(), std::complex<double> {});
        // the window is always the full 12 ms Hann; a shorter event uses its first part
        for (int i = 0; i < n; ++i) buf[(size_t) i] = raw[(size_t) ((e + (uint64_t) i) & mask)] * win[(size_t) i];
        fft();
        for (int b = 0; b < NB; ++b)
        {
            double s = 0.0;
            for (int i = bandLo[(size_t) b]; i < bandHi[(size_t) b] && i <= nfft / 2; ++i) s += std::norm (buf[(size_t) i]);
            out[b] = std::max (-100.0, 10.0 * std::log10 (s + 1e-12));   // floor: digital silence before a hit isn't "infinitely new"
        }
    }

    void fft() noexcept   // in-place iterative radix-2
    {
        for (int i = 1, j = 0; i < nfft; ++i)
        {
            int bit = nfft >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap (buf[(size_t) i], buf[(size_t) j]);
        }
        for (int len = 2; len <= nfft; len <<= 1)
        {
            const int step = nfft / len;
            for (int i = 0; i < nfft; i += len)
                for (int k = 0; k < len / 2; ++k)
                {
                    const auto u = buf[(size_t) (i + k)], v = buf[(size_t) (i + k + len / 2)] * tw[(size_t) (k * step)];
                    buf[(size_t) (i + k)] = u + v; buf[(size_t) (i + k + len / 2)] = u - v;
                }
        }
    }

    double sr = 48000.0;
    int n12 = 576, nfft = 1024;
    std::vector<double> win;
    std::vector<std::complex<double>> buf, tw;
    std::vector<int> bandLo, bandHi;
    std::vector<float> hitF, bleedF;
    int nHits = 0, nBleeds = 0;
    unsigned bleedRot = 0;
};

} // namespace tg
