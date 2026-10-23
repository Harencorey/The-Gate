#pragma once
#include <algorithm>
#include <cmath>

namespace tg
{
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

    // band-pass with exactly 0 dB and 0 degrees at its centre (k * v1)
    inline double bandPass (double x) noexcept
    {
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return k * v1;
    }
};


//------------------------------------------------------------------------------
// Hilbert pair (Olli Niemitalo's 90-degree phase-difference all-pass network): two outputs
// 90 degrees apart from ~20 Hz to ~0.45 fs, zero latency. sqrt(a^2 + b^2) is the signal's
// instantaneous amplitude: its envelope with no ripple at the waveform's own frequency.
struct HilbertPair
{
    static constexpr double c1[4] { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
    static constexpr double c2[4] { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };
    double x1[4][2] {}, y1[4][2] {}, x2[4][2] {}, y2[4][2] {}, delayed = 0.0;

    void reset() noexcept { *this = HilbertPair(); }

    // returns the instantaneous power (a^2 + b^2)
    inline double power (double x) noexcept
    {
        double a = x, b = x;
        for (int i = 0; i < 4; ++i)
        {
            const double ya = c1[i] * c1[i] * (a + y1[i][1]) - x1[i][1];
            x1[i][1] = x1[i][0]; x1[i][0] = a; y1[i][1] = y1[i][0]; y1[i][0] = ya; a = ya;
            const double yb = c2[i] * c2[i] * (b + y2[i][1]) - x2[i][1];
            x2[i][1] = x2[i][0]; x2[i][0] = b; y2[i][1] = y2[i][0]; y2[i][0] = yb; b = yb;
        }
        const double ad = delayed;   // path 1 is taken one sample late
        delayed = a;
        return ad * ad + b * b;
    }
};

} // namespace tg
