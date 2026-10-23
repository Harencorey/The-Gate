#pragma once
//==============================================================================
// Synthetic drum-room material for the spectral decay tests: a tom (membrane overtones,
// pitch glide, stick click) and the bleed a tom mic hears: hi-hat, snare and crash.
// Not a substitute for real multitracks, but every component is known exactly.
//==============================================================================
#include "Svf.h"
#include <cmath>
#include <random>
#include <vector>

namespace sim
{
// tom membrane overtone ratios (circular membrane modes)
inline constexpr double kTomRatios[] = { 1.0, 1.594, 2.136, 2.296, 2.653, 2.918 };
inline constexpr double kTomAmps[]   = { 1.0, 0.55,  0.35,  0.25,  0.18,  0.12 };
inline constexpr double kTomDecay[]  = { 0.40, 0.26, 0.18, 0.15, 0.12, 0.10 };   // seconds

inline void addTom (std::vector<float>& x, size_t at, float amp, double f0, double sr, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> u (-1.0, 1.0);
    const size_t len = (size_t) (2.5 * sr);
    double ph[6] {};
    for (size_t i = 0; i < len && at + i < x.size(); ++i)
    {
        const double t = (double) i / sr;
        const double glide = 1.0 + 0.04 * std::exp (-t / 0.06);   // starts sharp, settles
        double v = 0.0;
        for (int m = 0; m < 6; ++m)
        {
            ph[m] += 2.0 * M_PI * f0 * kTomRatios[m] * glide / sr;
            v += kTomAmps[m] * std::exp (-t / kTomDecay[m]) * std::sin (ph[m]);
        }
        const double click = i < (size_t) (0.003 * sr) ? 0.6 * u (rng) * std::exp (-t / 0.0007) : 0.0;
        x[at + i] += (float) (amp * 0.22 * (v + click));
    }
}

// band-limited noise burst: attack 1 ms, exponential decay
inline void addNoise (std::vector<float>& x, size_t at, float amp, double lo, double hi, double decay, double sr, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> n (0.0, 1.0);
    tg::Svf h1, h2, l1, l2;
    h1.setup (lo, sr, 0.5412); h2.setup (lo, sr, 1.3066); l1.setup (hi, sr, 0.5412); l2.setup (hi, sr, 1.3066);
    const size_t len = (size_t) (decay * 7.0 * sr);
    for (size_t i = 0; i < len && at + i < x.size(); ++i)
    {
        const double t = (double) i / sr;
        double hp, d, v = n (rng);
        h1.tick (v, hp); h2.tick (hp, v);
        v = l1.tick (v, d); v = l2.tick (v, d);
        const double env = std::min (1.0, t / 0.001) * std::exp (-t / decay);
        x[at + i] += (float) (amp * 0.35 * env * v);
    }
}

inline void addHat   (std::vector<float>& x, size_t at, float amp, double sr, unsigned s) { addNoise (x, at, amp, 6000, 15000, 0.035, sr, s); }
inline void addCrash (std::vector<float>& x, size_t at, float amp, double sr, unsigned s) { addNoise (x, at, amp, 3000, 14000, 1.6, sr, s); }
inline void addSnare (std::vector<float>& x, size_t at, float amp, double sr, unsigned s)
{
    std::vector<float> body (x.size(), 0.0f);
    addNoise (x, at, amp, 1500, 7000, 0.12, sr, s);
    for (size_t i = 0; i < (size_t) (0.6 * sr) && at + i < x.size(); ++i)
        x[at + i] += (float) (amp * 0.5 * std::exp (-(double) i / sr / 0.08) * std::sin (2 * M_PI * 190.0 * (double) i / sr));
}

struct Session
{
    std::vector<float> mix, tomOnly, bleedOnly;
    std::vector<size_t> toms, crashedToms;   // tom hit starts; those with a crash on the same instant
};

// A tom-mic track: toms every 600 ms (varying velocity), 8th-note hats, snare on 2 and 4 (120 bpm),
// and a crash on the same instant as some tom hits. Levels are as heard in the tom mic.
inline Session tomMicSession (double sr, int numToms, double tomHz = 110.0, double crashDb = -15.0)
{
    Session s;
    const size_t n = (size_t) ((numToms * 0.6 + 3.0) * sr);
    s.mix.assign (n, 0.0f); s.tomOnly.assign (n, 0.0f); s.bleedOnly.assign (n, 0.0f);
    std::mt19937 rng (11);
    std::uniform_real_distribution<float> vel (0.6f, 1.0f);
    for (int k = 0; k < numToms; ++k)
    {
        const size_t at = (size_t) ((0.3 + 0.6 * k) * sr);
        addTom (s.tomOnly, at, vel (rng), tomHz, sr, (unsigned) k);
        s.toms.push_back (at);
        if (k % 5 == 4) { addCrash (s.bleedOnly, at, (float) std::pow (10.0, crashDb / 20.0), sr, (unsigned) (100 + k)); s.crashedToms.push_back (at); }
    }
    for (size_t at = (size_t) (0.05 * sr); at < n; at += (size_t) (0.25 * sr)) addHat (s.bleedOnly, at, 0.06f, sr, (unsigned) at);
    for (size_t at = (size_t) (0.55 * sr); at < n; at += (size_t) (1.0 * sr)) addSnare (s.bleedOnly, at, 0.035f, sr, (unsigned) (at + 7));
    for (size_t i = 0; i < n; ++i) s.mix[i] = s.tomOnly[i] + s.bleedOnly[i];
    return s;
}
} // namespace sim
