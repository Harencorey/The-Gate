#pragma once
//==============================================================================
// The Gate - Micro Glow: the output saturator (Phase 2).
//
// A light, single-stage cousin of Glow, using Glow's own pieces:
//   TUBE - Glow's tube output stage. Drive 0..100 follows Glow's measured Drive map exactly
//          (GlowDriveMap.h): how hard the stage is hit, its knee hardness, asymmetry and
//          even-order bend all move together, from barely-there to fat and soft.
//   IRON - Glow's IRON core: a transformer whose core saturates on flux (the signal below
//          ~126 Hz), so the lows thicken and compress while the mids and highs pass clean.
//          The knob covers Glow's Drive 0..45 for IRON: its harmonics peak about 10 dB into the
//          core, and past that the core is fully saturated and the lows just drop out (it turns
//          into a high-pass), so the top of the knob stays on the useful side of that peak.
//   ENV  - Glow's envelope follower (at Glow's default Speed): a peak follower compared with
//          the material's own ~3 s average. Hits that rise above the average push Drive up
//          (positive amount) or pull it back (negative): 4 Drive units per dB at 100 %,
//          capped at +/-50, always returning to exactly the knob setting.
//
//   in -> [oversampled to ~384 kHz: x Drive gain -> curve -> / Drive gain] -> auto gain -> out
//
// Auto gain (like Glow's): the output's loudness is matched to the input's, so Drive changes the
// color and density, not the level, and Micro Glow never needs an output trim. Both are
// measured as mean square over ~1 s while there's signal (held through the gaps between hits),
// and re-matched quickly (~100 ms) for a moment after the Drive knob or the type changes.
// Below the curve the signal passes at exactly its own level.
//
// Oversampling runs the curve at ~384 kHz like Glow: 8x at 44.1/48k, 4x at 88.2/96k, 2x at
// 176.4/192k, through linear-phase equiripple halfband FIR stages (HalfbandCoeffs.h): no phase
// shift from the oversampling, which matters in a plug-in whose job is keeping mics
// phase-aligned. There's no DC blocker either (so no low-end phase shift at all): every curve
// passes through zero, and a gated track returns to silence between hits, so the asymmetric
// curves can't build up DC - the small offset they add exists only while a hit is sounding.
// IRON's flux filter doesn't shift phase either: it's added and subtracted (x - flux + sat(flux)).
// The filters always run, and the Off path is a pure delay of the same length, so the latency
// never changes when Micro Glow is switched on or off (no shift in time, no host latency
// change mid-song). On/Off and type changes crossfade (5 ms / 10 ms). Off is bit-exact.
//
// Real-time safety: prepare() allocates (the knee tables are built once and shared by every
// instance), tick() never does. Block-size independence: per-sample processing, and the
// envelope updates on a fixed 64-sample grid counted from reset().
//==============================================================================

#include "HalfbandCoeffs.h"
#include "GlowDriveMap.h"

#include <algorithm>
#include <array>
#include <atomic>
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
    enum Type { tube = 0, iron, numTypes };

    struct Settings
    {
        bool  on        = false;
        int   type      = tube;
        float drive     = 25.0f;   // 0..100, Glow's Drive scale
        float envAmount = 0.0f;    // -100..+100 %: the envelope follower moves Drive with the dynamics
    };

    void prepare (double sampleRate, int maxChannels)
    {
        sr = sampleRate;
        numCh = std::clamp (maxChannels, 1, kMaxCh);
        factor = sr < 60000.0 ? 8 : (sr < 120000.0 ? 4 : 2);
        latency = latencyFor (factor);
        knees = &kneeCache();   // built once, shared (first call allocates ~0.8 MB, message thread)

        chans.assign ((size_t) numCh, Channel {});
        for (auto& c : chans)
        {
            c.upA.init (hb::stageA); c.downA.init (hb::stageA);
            c.upB.init (hb::stageB); c.downB.init (hb::stageB);
            c.upC.init (hb::stageC); c.downC.init (hb::stageC);
            c.dry.assign ((size_t) std::max (1, latency), 0.0f);
        }
        const double fsOs = sr * factor;
        ironA = 1.0 - std::exp (-2.0 * M_PI * 40.0 * std::sqrt (10.0) / fsOs);   // Glow IRON at Density 50: 126 Hz
        driveCoef = std::exp (-1.0 / (0.050 * sr));    // 50 ms Drive glide (as Glow)
        offCoef   = std::exp (-1.0 / (0.002 * sr));    // 2 ms: smooths the follower's 64-sample steps
        envAtk = std::exp (-1.0 / (0.00775 * sr));     // Glow's follower at Speed 50: 7.75 ms attack
        envRel = std::exp (-1.0 / (0.1936 * sr));      //                                 194 ms release
        envAvgCoef = std::exp (-(double) kEnvChunk / (3.0 * sr));   // ~3 s average
        envAmtCoef = std::exp (-(double) kEnvChunk / (0.050 * sr));
        fadeOnStep  = 1.0 / std::max (1.0, 0.005 * sr);
        typeFadeLen = std::max (1, (int) std::lround (0.010 * sr));
        agSlow = std::exp (-1.0 / (1.0 * sr));
        agFast = std::exp (-1.0 / (0.100 * sr));
        agFastLen = (int) std::lround (0.300 * sr);
        reset();
    }

    void reset()
    {
        for (auto& c : chans)
        {
            c.upA.reset(); c.downA.reset(); c.upB.reset(); c.downB.reset(); c.upC.reset(); c.downC.reset();
            std::fill (c.dry.begin(), c.dry.end(), 0.0f);
            c.dryPos = 0;
            c.ironLp = 0.0;
        }
        mix = set.on ? 1.0 : 0.0;
        curType = prevType = set.type;
        typeFade = 0;
        driveSm = set.drive;
        envLin = 0.0; envAvgDb = -100.0; envOffset = envOffsetSm = 0.0; envAmtSm = set.envAmount;
        chunkPos = 0;
        modDrive.store ((float) driveSm);
        pinAvg = poutAvg = 0.0;
        agFastLeft = 0;
        lastKnobDrive = set.drive;
    }

    void setSettings (const Settings& ns) noexcept
    {
        const int t = std::clamp (ns.type, 0, (int) numTypes - 1);
        if (t != curType && typeFade == 0)
        {
            prevType = curType;
            curType = t;
            typeFade = typeFadeLen;
            agFastLeft = agFastLen;
        }
        if (std::abs (ns.drive - lastKnobDrive) > 0.5f)   // the knob moved: re-match quickly
        {
            lastKnobDrive = ns.drive;
            agFastLeft = agFastLen;
        }
        set = ns;
    }

    int latencySamples() const noexcept { return latency; }
    int oversamplingFactor() const noexcept { return factor; }
    float modulatedDrive() const noexcept { return modDrive.load (std::memory_order_relaxed); }   // UI: Drive incl. envelope

    // each stage's round trip (up + down) in base samples: (taps - 1) / oversampling factor at that stage
    static constexpr int latencyFor (int osFactor) noexcept
    {
        const int a = ((int) std::size (hb::stageA) - 1) / 2;
        const int b = ((int) std::size (hb::stageB) - 1) / 4;
        const int c = ((int) std::size (hb::stageC) - 1) / 8;
        return osFactor >= 8 ? a + b + c : (osFactor >= 4 ? a + b : a);
    }

    // One sample for every channel, in place. forceDry: output the delayed input (Listen, bypass).
    inline void tick (float* x, int channels, bool forceDry = false) noexcept
    {
        channels = std::min (channels, numCh);

        //---------------- envelope follower (Glow's): on the input, stereo-linked
        {
            double a = 0.0;
            for (int ch = 0; ch < channels; ++ch) a = std::max (a, (double) std::abs (x[ch]));
            envLin = a > envLin ? envAtk * envLin + (1.0 - envAtk) * a : envRel * envLin + (1.0 - envRel) * a;
            if (++chunkPos == kEnvChunk)
            {
                chunkPos = 0;
                updateEnvOffset();
            }
            envOffsetSm = envOffset + (envOffsetSm - envOffset) * offCoef;
        }

        //---------------- Drive (knob glide + envelope) -> Glow's stage at that Drive
        driveSm = set.drive + (driveSm - set.drive) * driveCoef;
        if (std::abs (driveSm - set.drive) < 1e-6) driveSm = set.drive;
        const float d = (float) std::clamp (driveSm + envOffsetSm, 0.0, 100.0);
        if (++modCount >= 256) { modCount = 0; modDrive.store (d, std::memory_order_relaxed); }
        const glowmap::Shape sh = glowmap::at (d);
        StageParams sp;
        sp.gain = std::pow (10.0f, sh.preDb / 20.0f);
        sp.invGain = 1.0f / sp.gain;
        sp.ironGain = std::pow (10.0f, glowmap::at (d * kIronDriveScale).preDb / 20.0f);
        sp.ironInv = 1.0f / sp.ironGain;
        sp.knee = &(*knees)[(size_t) std::lround (d)];
        sp.asym = sh.asym;
        sp.k2 = sh.k2;

        const bool wantOn = set.on && ! forceDry;
        if (wantOn && mix < 1.0)        mix = std::min (1.0, mix + fadeOnStep);
        else if (! wantOn && mix > 0.0) mix = std::max (0.0, mix - fadeOnStep);

        float wetOut[kMaxCh], dryOut[kMaxCh];
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
            float a[2], b[4], o[8];
            c.upA.process (in, a[0], a[1]);
            if (factor == 8)
            {
                for (int j = 0; j < 2; ++j) c.upB.process (a[j], b[2 * j], b[2 * j + 1]);
                for (int j = 0; j < 4; ++j) c.upC.process (b[j], o[2 * j], o[2 * j + 1]);
                for (float& v : o) v = shape (c, v, sp, typeMix);
                for (int j = 0; j < 4; ++j) b[j] = c.downC.process (o[2 * j], o[2 * j + 1]);
                for (int j = 0; j < 2; ++j) a[j] = c.downB.process (b[2 * j], b[2 * j + 1]);
            }
            else if (factor == 4)
            {
                for (int j = 0; j < 2; ++j) c.upB.process (a[j], b[2 * j], b[2 * j + 1]);
                for (float& v : b) v = shape (c, v, sp, typeMix);
                for (int j = 0; j < 2; ++j) a[j] = c.downB.process (b[2 * j], b[2 * j + 1]);
            }
            else
            {
                for (float& v : a) v = shape (c, v, sp, typeMix);
            }
            wetOut[ch] = c.downA.process (a[0], a[1]);
            dryOut[ch] = dry;
        }

        //---------------- auto gain: match the output's loudness to the input's
        double pin = 0.0, pout = 0.0;
        for (int ch = 0; ch < channels; ++ch) { pin += (double) dryOut[ch] * dryOut[ch]; pout += (double) wetOut[ch] * wetOut[ch]; }
        if (pin > 1e-8 * channels)   // only while there's signal (-80 dBFS): holds through the gaps between hits
        {
            const double k = agFastLeft > 0 ? agFast : agSlow;
            pinAvg  = pin  + (pinAvg  - pin)  * k;
            poutAvg = pout + (poutAvg - pout) * k;
        }
        if (agFastLeft > 0) --agFastLeft;
        const float comp = (pinAvg > 1e-12 && poutAvg > 1e-12)
                               ? (float) std::clamp (std::sqrt (pinAvg / poutAvg), 0.01, 100.0)   // +/-40 dB
                               : 1.0f;

        for (int ch = 0; ch < channels; ++ch)
        {
            const float y = wetOut[ch] * comp, dry = dryOut[ch];
            if (mix <= 0.0)      x[ch] = dry;   // exact: Off is bit-identical to the delayed input
            else if (mix >= 1.0) x[ch] = y;
            else                 x[ch] = dry + (float) mix * (y - dry);
        }
        lastComp = comp;
    }

    float autoGainDb() const noexcept { return 20.0f * std::log10 (lastComp); }   // for tests

    static constexpr int kMaxCh = 8;

    // The static curves at a given Drive (no oversampling, no IRON memory) - for tests and plots.
    float tubeCurve (float v, float drive) const noexcept
    {
        const glowmap::Shape sh = glowmap::at (drive);
        const float gn = std::pow (10.0f, sh.preDb / 20.0f);
        return tubeStage (v * gn, (*knees)[(size_t) std::lround (drive)], sh.asym, sh.k2) / gn;
    }

private:
    static constexpr int kEnvChunk = 64;
    static constexpr float kIronAsym = 0.05f;   // Glow's IRON core asymmetry
    static constexpr float kIronDriveScale = 0.45f;   // IRON's knob = Glow Drive 0..45 (see the header)

    struct Channel
    {
        HalfbandUp upA, upB, upC;
        HalfbandDown downA, downB, downC;
        std::vector<float> dry;
        size_t dryPos = 0;
        double ironLp = 0.0;   // IRON: core flux (low-passed signal, before the Drive gain)
    };
    struct StageParams { float gain = 1, invGain = 1, ironGain = 1, ironInv = 1, asym = 0, k2 = 0; const KneeTable* knee = nullptr; };

    // Glow's knee tables for Drive 0..100 (the output stage's knee hardness at each Drive).
    static const std::array<KneeTable, 101>& kneeCache()
    {
        static const std::array<KneeTable, 101> cache = []
        {
            std::array<KneeTable, 101> c;
            for (int d = 0; d <= 100; ++d) c[(size_t) d].build (glowmap::table[d].sharp);
            return c;
        }();
        return cache;
    }

    // Glow's tube stage: bounded even-order bend, then an asymmetric soft clip (ceilings 1 -/+ asym).
    static inline float tubeStage (float u, const KneeTable& knee, float asym, float k2) noexcept
    {
        const float uu = u * u;
        const float v = u + k2 * uu / std::sqrt (1.0f + uu * uu);
        const float cl = v >= 0.0f ? 1.0f - asym : 1.0f + asym;
        const float r = v / cl;
        const float ar = std::abs (r);
        if (ar < 1.0e-9f) return v;
        if (ar <= 1.0f) return cl * r * knee (ar);
        return cl * (r > 0.0f ? 1.0f : -1.0f) * knee (1.0f / ar);
    }

    // Glow's IRON curve: soft r / sqrt(1 + r^2) with asymmetric ceilings.
    static inline float soft2 (float v, float asym) noexcept
    {
        const float c = v >= 0.0f ? 1.0f - asym : 1.0f + asym;
        const float r = v / c;
        return c * r / std::sqrt (1.0f + r * r);
    }

    inline float shapeOne (int t, Channel& c, float v, const StageParams& sp) const noexcept
    {
        if (t == iron)
        {
            // transformer: x - flux + sat(flux); the clean part passes untouched
            const float xg = v * sp.ironGain, phi = (float) c.ironLp * sp.ironGain;
            return (xg - phi + soft2 (phi, kIronAsym)) * sp.ironInv;
        }
        return tubeStage (v * sp.gain, *sp.knee, sp.asym, sp.k2) * sp.invGain;
    }

    inline float shape (Channel& c, float v, const StageParams& sp, float typeMix) const noexcept
    {
        c.ironLp += ironA * ((double) v - c.ironLp);   // runs whichever type is on: a switch is already settled
        const float a = shapeOne (curType, c, v, sp);
        if (typeMix >= 1.0f) return a;
        const float b = shapeOne (prevType, c, v, sp);
        return b + typeMix * (a - b);
    }

    // Glow's updateEnvOffset(): once per 64 samples, relative to the material's own average.
    void updateEnvOffset() noexcept
    {
        envAmtSm = set.envAmount + (envAmtSm - set.envAmount) * envAmtCoef;
        if (std::abs (envAmtSm - set.envAmount) < 1e-4) envAmtSm = set.envAmount;
        const double envDb = envLin > 1e-5 ? 20.0 * std::log10 (envLin) : -100.0;
        if (envDb > -60.0)   // average only while there's signal (holds through silence and gaps between hits)
            envAvgDb = envAvgDb < -90.0 ? envDb : envAvgCoef * envAvgDb + (1.0 - envAvgCoef) * envDb;
        const double rise = std::max (0.0, envDb - envAvgDb);
        envOffset = (std::abs (envAmtSm) < 0.01 || envAvgDb < -90.0 || envDb < -60.0) ? 0.0
                    : std::clamp (envAmtSm / 100.0 * 4.0 * rise, -50.0, 50.0);
    }

    Settings set;
    double sr = 48000.0;
    int numCh = 2, factor = 8, latency = 0;
    std::vector<Channel> chans;
    const std::array<KneeTable, 101>* knees = nullptr;
    double ironA = 0.002;
    double driveSm = 25.0, driveCoef = 0.999, offCoef = 0.99;
    double envLin = 0.0, envAvgDb = -100.0, envOffset = 0.0, envOffsetSm = 0.0, envAmtSm = 0.0;
    double envAtk = 0.99, envRel = 0.999, envAvgCoef = 0.999, envAmtCoef = 0.9;
    int chunkPos = 0, modCount = 0;
    std::atomic<float> modDrive { 25.0f };
    double mix = 0.0, fadeOnStep = 0.004;
    double pinAvg = 0.0, poutAvg = 0.0, agSlow = 0.99998, agFast = 0.9998;
    int agFastLeft = 0, agFastLen = 14400;
    float lastKnobDrive = 25.0f, lastComp = 1.0f;
    int curType = tube, prevType = tube, typeFade = 0, typeFadeLen = 480;
};

} // namespace tg
