#pragma once
//==============================================================================
// The Gate - Micro Glow: the output saturator / clipper (Phase 2).
//
// A light, single-stage cousin of Glow. The curves come from Glow itself:
//   TUBE  - Glow's tube output stage at its default Drive: soft knee (hardness 18.2) with the
//           measured even-order bend and asymmetry, so it adds a little 2nd harmonic.
//   DIODE - Glow's DIODE core at Density 50: a much softer knee (4.75) with 8 % asymmetry.
//   CLIP  - a near-hard, symmetric clipper (knee 60): odd harmonics only, peaks shaved flat.
//
//   in -> x Drive -> [oversampled to ~384 kHz: curve with its ceiling at Ceiling] -> out
//
// Every curve has a slope of exactly 1 for small signals, so below the knee the only thing
// Drive does is turn it up; nothing is coloured until the peaks reach the Ceiling.
//
// Oversampling runs the curve at ~384 kHz like Glow: 8x at 44.1/48k, 4x at 88.2/96k, 2x at
// 176.4/192k, through linear-phase equiripple halfband FIR stages (HalfbandCoeffs.h): no phase
// shift from the oversampling, which matters in a plug-in whose job is keeping mics
// phase-aligned. There's no DC blocker either (so no low-end phase shift at all): every curve
// passes through zero, and a gated track returns to silence between hits, so the asymmetric
// curves can't build up DC - the small offset they add exists only while a hit is sounding.
// The filters always run, and the Off path is a pure delay of the same length, so the latency
// never changes when Micro Glow is switched on or off (no shift in time, no host latency
// change mid-song).
// On/Off and type changes crossfade (5 ms / 10 ms). Off is bit-exact (the delayed input).
//
// Real-time safety: prepare() allocates, tick() never does.
//==============================================================================

#include "HalfbandCoeffs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace tg
{

//------------------------------------------------------------------------------
// Glow's knee function h(b) = (1 + b^s)^(-1/s) on [0, 1], tabulated (Catmull-Rom), as in Glow.
class KneeTable
{
public:
    void build (float s) noexcept
    {
        for (int k = 0; k < N + 4; ++k)
        {
            const double b = std::abs ((double) (k - 1) / N);
            t[(size_t) k] = (float) std::pow (1.0 + std::pow (b, (double) s), -1.0 / (double) s);
        }
    }

    inline float operator() (float b) const noexcept   // b in [0, 1]
    {
        const float pos = b * (float) N;
        const int i = std::clamp ((int) pos, 0, N - 1);
        const float f = pos - (float) i;
        const float y0 = t[(size_t) i], y1 = t[(size_t) i + 1], y2 = t[(size_t) i + 2], y3 = t[(size_t) i + 3];
        return y1 + 0.5f * f * (y2 - y0 + f * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + f * (3.0f * (y1 - y2) + y3 - y0)));
    }

private:
    static constexpr int N = 2048;
    std::array<float, N + 4> t {};
};

//------------------------------------------------------------------------------
// Polyphase halfband FIR, 2x up. Push one sample, get two (gain 2 makes up for the zero-stuffing).
class HalfbandUp
{
public:
    template <size_t N> void init (const double (&h)[N])
    {
        for (int p = 0; p < 2; ++p)
        {
            taps[p].clear();
            for (size_t k = (size_t) p; k < N; k += 2)
                if (std::abs (h[k]) > 0.0) taps[p].push_back ({ (int) ((k - (size_t) p) / 2), (float) (2.0 * h[k]) });
        }
        size = 1;
        while (size < N / 2 + 2) size <<= 1;
        hist.assign (2 * size, 0.0f);
        reset();
    }
    void reset() noexcept { std::fill (hist.begin(), hist.end(), 0.0f); pos = 0; }

    inline void process (float x, float& y0, float& y1) noexcept
    {
        pos = (pos + size - 1) & (size - 1);   // newest sample at the lowest index
        hist[pos] = hist[pos + size] = x;
        const float* hp = hist.data() + pos;
        float a = 0.0f, b = 0.0f;
        for (const auto& t : taps[0]) a += t.c * hp[t.j];
        for (const auto& t : taps[1]) b += t.c * hp[t.j];
        y0 = a; y1 = b;
    }

private:
    struct Tap { int j; float c; };
    std::vector<Tap> taps[2];
    std::vector<float> hist;
    size_t size = 1, pos = 0;
};

// Polyphase halfband FIR, 2x down. Push two samples, get one.
class HalfbandDown
{
public:
    template <size_t N> void init (const double (&h)[N])
    {
        taps.clear();
        for (size_t k = 0; k < N; ++k)
            if (std::abs (h[k]) > 0.0) taps.push_back ({ (int) k, (float) h[k] });
        size = 1;
        while (size < N + 2) size <<= 1;
        hist.assign (2 * size, 0.0f);
        reset();
    }
    void reset() noexcept { std::fill (hist.begin(), hist.end(), 0.0f); pos = 0; }

    inline float process (float x0, float x1) noexcept
    {
        push (x0);
        const float* hp = hist.data() + pos;
        float y = 0.0f;
        for (const auto& t : taps) y += t.c * hp[t.j];
        push (x1);
        return y;
    }

private:
    inline void push (float x) noexcept
    {
        pos = (pos + size - 1) & (size - 1);
        hist[pos] = hist[pos + size] = x;
    }
    struct Tap { int j; float c; };
    std::vector<Tap> taps;
    std::vector<float> hist;
    size_t size = 1, pos = 0;
};

//------------------------------------------------------------------------------
class MicroGlow
{
public:
    enum Type { tube = 0, diode, clip, numTypes };

    struct Settings
    {
        bool  on        = false;
        int   type      = tube;
        float driveDb   = 0.0f;   // 0 .. +24
        float ceilingDb = 0.0f;   // -24 .. 0 dBFS
    };

    void prepare (double sampleRate, int maxChannels)
    {
        sr = sampleRate;
        numCh = std::max (1, maxChannels);
        factor = sr < 60000.0 ? 8 : (sr < 120000.0 ? 4 : 2);
        latency = latencyFor (factor);

        chans.assign ((size_t) numCh, Channel {});
        for (auto& c : chans)
        {
            c.upA.init (hb::stageA); c.downA.init (hb::stageA);
            c.upB.init (hb::stageB); c.downB.init (hb::stageB);
            c.upC.init (hb::stageC); c.downC.init (hb::stageC);
            c.dry.assign ((size_t) std::max (1, latency), 0.0f);
        }
        knees[tube].build (kSharp[tube]);
        knees[diode].build (kSharp[diode]);
        knees[clip].build (kSharp[clip]);
        smCoef = std::exp (-1.0 / (0.020 * sr));            // 20 ms parameter glide
        fadeOnStep  = 1.0 / std::max (1.0, 0.005 * sr);       // 5 ms on/off crossfade
        typeFadeLen = std::max (1, (int) std::lround (0.010 * sr));
        reset();
    }

    void reset()
    {
        for (auto& c : chans)
        {
            c.upA.reset(); c.downA.reset(); c.upB.reset(); c.downB.reset(); c.upC.reset(); c.downC.reset();
            std::fill (c.dry.begin(), c.dry.end(), 0.0f);
            c.dryPos = 0;
        }
        mix = set.on ? 1.0 : 0.0;
        curType = prevType = set.type;
        typeFade = 0;
        driveSm = dbToLin (set.driveDb);
        ceilSm  = dbToLin (set.ceilingDb);
    }

    void setSettings (const Settings& ns) noexcept
    {
        if (ns.type != curType && typeFade == 0)
        {
            prevType = curType;
            curType = std::clamp (ns.type, 0, (int) numTypes - 1);
            typeFade = typeFadeLen;
        }
        set = ns;
    }

    int latencySamples() const noexcept { return latency; }
    int oversamplingFactor() const noexcept { return factor; }
    // each stage's round trip (up + down) in base samples: (taps - 1) / oversampling factor at that stage
    static constexpr int latencyFor (int osFactor) noexcept
    {
        const int a = ((int) std::size (hb::stageA) - 1) / 2;
        const int b = ((int) std::size (hb::stageB) - 1) / 4;
        const int c = ((int) std::size (hb::stageC) - 1) / 8;
        return osFactor >= 8 ? a + b + c : (osFactor >= 4 ? a + b : a);
    }

    // One sample for every channel, in place. forceDry: output the delayed input (Listen).
    inline void tick (float* x, int channels, bool forceDry = false) noexcept
    {
        channels = std::min (channels, numCh);

        // parameter glides (per sample, so the result never depends on the block size)
        const double driveT = dbToLin (set.driveDb), ceilT = dbToLin (set.ceilingDb);
        driveSm = driveT + (driveSm - driveT) * smCoef;
        ceilSm  = ceilT  + (ceilSm  - ceilT)  * smCoef;
        if (std::abs (driveSm - driveT) < 1e-9) driveSm = driveT;
        if (std::abs (ceilSm - ceilT) < 1e-9)   ceilSm = ceilT;

        const bool wantOn = set.on && ! forceDry;
        if (wantOn && mix < 1.0)       mix = std::min (1.0, mix + fadeOnStep);
        else if (! wantOn && mix > 0.0) mix = std::max (0.0, mix - fadeOnStep);

        float typeMix = 1.0f;   // weight of curType (the rest is prevType)
        if (typeFade > 0)
        {
            typeMix = 1.0f - (float) typeFade / (float) typeFadeLen;
            --typeFade;
        }

        for (int ch = 0; ch < channels; ++ch)
        {
            auto& c = chans[(size_t) ch];
            const float in = x[ch];

            // pure delay of the same length (the Off path, and the crossfade partner)
            float dry = in;
            if (latency > 0)
            {
                dry = c.dry[c.dryPos];
                c.dry[c.dryPos] = in;
                c.dryPos = (c.dryPos + 1) % (size_t) latency;
            }

            // the saturated path (always runs: keeps the filters settled for clean switching)
            const float drive = (float) driveSm, ceil = (float) ceilSm;
            float a[2], b[4], o[8];
            c.upA.process (in * drive, a[0], a[1]);
            if (factor == 8)
            {
                for (int j = 0; j < 2; ++j) c.upB.process (a[j], b[2 * j], b[2 * j + 1]);
                for (int j = 0; j < 4; ++j) c.upC.process (b[j], o[2 * j], o[2 * j + 1]);
                for (float& v : o) v = shape (v, ceil, typeMix);
                for (int j = 0; j < 4; ++j) b[j] = c.downC.process (o[2 * j], o[2 * j + 1]);
                for (int j = 0; j < 2; ++j) a[j] = c.downB.process (b[2 * j], b[2 * j + 1]);
            }
            else if (factor == 4)
            {
                for (int j = 0; j < 2; ++j) c.upB.process (a[j], b[2 * j], b[2 * j + 1]);
                for (float& v : b) v = shape (v, ceil, typeMix);
                for (int j = 0; j < 2; ++j) a[j] = c.downB.process (b[2 * j], b[2 * j + 1]);
            }
            else
            {
                for (float& v : a) v = shape (v, ceil, typeMix);
            }
            const float wet = c.downA.process (a[0], a[1]);

            const float y = wet;
            if (mix <= 0.0)      x[ch] = dry;   // exact: Off is bit-identical to the delayed input
            else if (mix >= 1.0) x[ch] = y;
            else                 x[ch] = dry + (float) mix * (y - dry);
        }
    }

    // The static curve (no oversampling) - for tests and plots.
    float curve (int type, float v, float ceiling) const noexcept { return shapeOne (type, v, ceiling); }

private:
    // Knee hardness, asymmetry and even-order bend of each type (from Glow, see the header).
    static constexpr float kSharp[numTypes] { 18.17177f, 4.75f, 60.0f };
    static constexpr float kAsym[numTypes]  { -0.021324f, 0.08f, 0.0f };
    static constexpr float kBend[numTypes]  { 0.047269f, 0.0f, 0.0f };

    inline float shape (float v, float ceiling, float typeMix) const noexcept
    {
        const float a = shapeOne (curType, v, ceiling);
        if (typeMix >= 1.0f) return a;
        return shapeOne (prevType, v, ceiling) + typeMix * (a - shapeOne (prevType, v, ceiling));
    }

    // Glow's stage: bounded even-order bend, then an asymmetric soft clip. Scaled so the
    // louder side's ceiling lands exactly on 'ceiling' and the small-signal slope stays 1.
    inline float shapeOne (int t, float v, float ceiling) const noexcept
    {
        const float k = 1.0f + std::abs (kAsym[t]);
        float u = v * k / ceiling;
        if (kBend[t] > 0.0f)
        {
            const float uu = u * u;
            u += kBend[t] * uu / std::sqrt (1.0f + uu * uu);
        }
        const float cl = u >= 0.0f ? 1.0f - kAsym[t] : 1.0f + kAsym[t];
        const float r = u / cl;
        const float ar = std::abs (r);
        float y;
        if (ar < 1.0e-9f)  y = u;
        else if (ar <= 1.0f) y = cl * r * knees[t] (ar);
        else               y = cl * (r > 0.0f ? 1.0f : -1.0f) * knees[t] (1.0f / ar);
        return y * ceiling / k;
    }

    static double dbToLin (double db) noexcept { return std::pow (10.0, db / 20.0); }

    struct Channel
    {
        HalfbandUp upA, upB, upC;
        HalfbandDown downA, downB, downC;
        std::vector<float> dry;
        size_t dryPos = 0;
    };

    Settings set;
    double sr = 48000.0;
    int numCh = 2, factor = 8, latency = 0;
    std::vector<Channel> chans;
    KneeTable knees[numTypes];
    double smCoef = 0.999, driveSm = 1.0, ceilSm = 1.0;
    double mix = 0.0, fadeOnStep = 0.004;
    int curType = tube, prevType = tube, typeFade = 0, typeFadeLen = 480;
};

} // namespace tg
