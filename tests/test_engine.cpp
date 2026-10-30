//==============================================================================
// The Gate - offline engine tests (no JUCE, no DAW).
//
// Build & run:  cmake --build build --target GateTests && ./build/GateTests
//==============================================================================

#include "GateCore.h"
#include "DrumSim.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <functional>
#include <new>
#include <random>
#include <vector>

//------------------------------------------------------------------------------
// Allocation spy: counts heap allocations while the audio path runs.
static std::atomic<bool> gWatchAllocs { false };
static std::atomic<int>  gAllocCount { 0 };

void* operator new (std::size_t n)
{
    if (gWatchAllocs.load()) ++gAllocCount;
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

static int gFailures = 0;
#define CHECK(cond, ...) \
    do { if (! (cond)) { ++gFailures; std::printf ("  FAIL: " __VA_ARGS__); std::printf ("\n"); } } while (0)

using Buf = std::vector<std::vector<float>>;

static Buf makeBuf (int ch, size_t n) { return Buf ((size_t) ch, std::vector<float> (n, 0.0f)); }

// Runs the engine over the whole buffer in blocks. blockSize 0 = random 1..1024.
static Buf run (const Buf& in, const tg::GateSettings& s, double sr, int blockSize = 512,
                const Buf* key = nullptr, unsigned seed = 1, tg::GateEngine* keep = nullptr,
                bool watchAllocs = false)
{
    tg::GateEngine local;
    tg::GateEngine& e = keep ? *keep : local;
    const int ch = (int) in.size();
    e.prepare (sr, ch);
    e.setSettings (s);
    e.reset();
    Buf out = in;
    std::mt19937 rng (seed);
    std::uniform_int_distribution<int> dist (1, 1024);
    const size_t n = in[0].size();
    std::vector<float*> io ((size_t) ch);
    std::vector<const float*> kp (key ? key->size() : 0);
    if (watchAllocs) { gAllocCount = 0; gWatchAllocs = true; }
    for (size_t pos = 0; pos < n;)
    {
        const int bs = (int) std::min<size_t> (n - pos, (size_t) (blockSize > 0 ? blockSize : dist (rng)));
        for (int c = 0; c < ch; ++c) io[(size_t) c] = out[(size_t) c].data() + pos;
        for (size_t c = 0; c < kp.size(); ++c) kp[c] = (*key)[c].data() + pos;
        e.setSettings (s);
        e.process (io.data(), ch, key ? kp.data() : nullptr, (int) kp.size(), bs);
        tg::MeterFrame f; while (e.meters.pop (f)) {}
        pos += (size_t) bs;
    }
    gWatchAllocs = false;
    return out;
}

static float dbToLin (float db) { return std::pow (10.0f, db / 20.0f); }

// A drum-ish hit: noisy click + decaying tone
static void addHit (std::vector<float>& x, size_t at, float amp, float hz, double sr, float decayMs, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    const size_t len = (size_t) (decayMs * 0.001 * sr * 6);
    for (size_t i = 0; i < len && at + i < x.size(); ++i)
    {
        const double t = (double) i / sr;
        const double env = std::exp (-t / (decayMs * 0.001));
        const double click = i < (size_t) (0.002 * sr) ? u (rng) * std::exp (-t / 0.0005) : 0.0;
        x[at + i] += (float) (amp * (env * std::sin (2 * M_PI * hz * t) + 0.5 * click));
    }
}

//------------------------------------------------------------------------------
static void testRangeZeroIsTransparent()
{
    std::printf ("Range 0 dB: output = input delayed by the lookahead, bit for bit\n");
    for (double sr : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (float laMs : { 0.0f, 2.0f, 5.0f, 10.0f, 20.0f })
        {
            const size_t n = (size_t) sr / 2;
            Buf in = makeBuf (2, n);
            std::mt19937 rng (7);
            std::uniform_real_distribution<float> u (-1.0f, 1.0f);
            for (auto& c : in) for (auto& v : c) v = u (rng) * 0.5f;
            tg::GateSettings s; s.rangeDb = 0.0f; s.lookaheadMs = laMs; s.thresholdDb = -20.0f;
            Buf out = run (in, s, sr, 0);
            tg::GateEngine e; e.prepare (sr, 2);
            const int L = e.latencyFor (laMs);
            bool same = true;
            for (int c = 0; c < 2; ++c)
                for (size_t i = (size_t) L; i < n; ++i)
                    same &= out[(size_t) c][i] == in[(size_t) c][i - (size_t) L];
            CHECK (same, "sr %.0f lookahead %.0f ms not bit-identical", sr, laMs);
        }
}

static void testOpenIsBitIdentical()
{
    std::printf ("Fully open on a loud signal: bit-identical to the delayed input\n");
    const double sr = 48000;
    const size_t n = 48000;
    Buf in = makeBuf (2, n);
    for (size_t i = 0; i < n; ++i) { in[0][i] = 0.5f * (float) std::sin (2 * M_PI * 220 * i / sr); in[1][i] = 0.3f * (float) std::sin (2 * M_PI * 330 * i / sr); }
    tg::GateSettings s; s.thresholdDb = -30.0f;
    Buf out = run (in, s, sr, 0);
    tg::GateEngine e0; e0.prepare (sr, 2);
    const int L = e0.latencyFor (5.0f);
    bool same = true;
    for (int c = 0; c < 2; ++c)
        for (size_t i = 4800; i < n; ++i) same &= out[(size_t) c][i] == in[(size_t) c][i - (size_t) L];
    CHECK (same, "open gate changed the audio");
}

static void testLatencyAndTransientIntact()
{
    std::printf ("Reported latency = measured latency; the transient passes at full level\n");
    for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (float laMs : { 2.0f, 5.0f, 10.0f, 20.0f })
        {
            const size_t n = (size_t) (sr * 0.5);
            Buf in = makeBuf (1, n);
            const size_t at = (size_t) (sr * 0.2) + 3;
            addHit (in[0], at, 0.8f, 150.0f, sr, 80.0f, 3);
            tg::GateSettings s; s.lookaheadMs = laMs; s.attackMs = 0.5f; s.thresholdDb = -30.0f;
            tg::GateEngine e;
            Buf out = run (in, s, sr, 0, nullptr, 2, &e);
            const int L = e.currentLatency();
            CHECK (L == e.lookaheadSamples (laMs) + e.glowLatency(), "latency mismatch");
            CHECK (e.glowLatency() == (sr < 60000 ? 48 : 46), "Micro Glow latency %d at %.0f", e.glowLatency(), sr);
            size_t firstIn = 0, firstOut = 0;
            while (firstIn < n && in[0][firstIn] == 0.0f) ++firstIn;
            while (firstOut < n && std::abs (out[0][firstOut]) < 1e-9f) ++firstOut;
            CHECK (firstOut == firstIn + (size_t) L, "sr %.0f la %.0f: hit came out %d samples late (expected %d)",
                   sr, laMs, (int) (firstOut - firstIn), L);
            // the first 2 ms of the transient must be untouched (gate already fully open)
            bool intact = true;
            for (size_t i = 0; i < (size_t) (0.002 * sr); ++i)
                intact &= out[0][firstIn + (size_t) L + i] == in[0][firstIn + i];
            CHECK (intact, "sr %.0f la %.0f: transient was ramped", sr, laMs);
        }
}

static void testClickFree()
{
    std::printf ("No clicks: open/close on a steady sine never jumps more than the envelope allows\n");
    const double sr = 48000;
    const size_t n = (size_t) sr * 2;
    const float A = 0.5f, hz = 200.0f;
    Buf in = makeBuf (1, n), key = makeBuf (1, n);
    for (size_t i = 0; i < n; ++i) in[0][i] = A * (float) std::sin (2 * M_PI * hz * i / sr);
    for (size_t b = 0; b < 8; ++b) for (size_t i = 0; i < 4000; ++i) key[0][b * 11000 + 2000 + i] = 0.5f;   // key bursts
    for (float att : { 0.1f, 0.5f, 2.0f })
        for (float rel : { 5.0f, 50.0f, 500.0f })
        {
            tg::GateSettings s; s.externalKey = true; s.attackMs = att; s.releaseMs = rel; s.holdMs = 0.0f; s.thresholdDb = -20.0f;
            Buf out = run (in, s, sr, 0, &key);
            const double attN = std::max (1.0, att * 0.001 * sr);
            const double bound = A * (2 * M_PI * hz / sr + M_PI / (2 * attN)) * 1.05 + 1e-6;
            double worst = 0.0;
            for (size_t i = 1; i < n; ++i) worst = std::max (worst, (double) std::abs (out[0][i] - out[0][i - 1]));
            CHECK (worst <= bound, "attack %.1f release %.0f: jump %.4f > bound %.4f", att, rel, worst, bound);
        }
}

static void testLookaheadChangeIsClickFree()
{
    std::printf ("Changing the lookahead while playing fades instead of clicking\n");
    const double sr = 48000;
    tg::GateEngine e; e.prepare (sr, 1);
    tg::GateSettings s; s.rangeDb = 0.0f; s.lookaheadMs = 5.0f;
    e.setSettings (s); e.reset();
    std::vector<float> x (256 * 188);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * (float) std::sin (2 * M_PI * 100 * i / sr);
    double worst = 0; float prev = 0;
    for (size_t pos = 0; pos < x.size(); pos += 256)
    {
        if (pos == 12800) { s.lookaheadMs = 20.0f; e.setSettings (s); }
        if (pos == 32000) { s.lookaheadMs = 2.0f;  e.setSettings (s); }
        float* p = x.data() + pos;
        e.process (&p, 1, nullptr, 0, 256);
        for (int i = 0; i < 256; ++i) { if (pos + (size_t) i > 0) worst = std::max (worst, (double) std::abs (p[i] - prev)); prev = p[i]; }
    }
    const double bound = 0.5 * (2 * M_PI * 100 / sr + 1.0 / tg::GateEngine::kLookaheadFade) * 1.05;
    CHECK (worst <= bound, "jump %.4f > %.4f", worst, bound);
    CHECK (e.currentLatency() == e.latencyFor (2.0f), "lookahead didn't switch");
}

static Buf makeDrumTrack (double sr, size_t n, unsigned seed)
{
    Buf in = makeBuf (2, n);
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> amp (0.05f, 0.9f), noise (-1.0f, 1.0f);
    for (size_t at = 1000; at + 20000 < n; at += 9000 + (size_t) (rng() % 7000))
    {
        addHit (in[0], at, amp (rng), 120.0f + (float) (rng() % 80), sr, 60.0f, (unsigned) at);
    }
    for (size_t i = 0; i < n; ++i) { in[0][i] += 0.01f * noise (rng); in[1][i] = 0.8f * in[0][i]; }   // bleed floor
    return in;
}

static void testBlockSizeIndependence()
{
    std::printf ("Identical output for any block size (every feature on)\n");
    const double sr = 48000;
    const size_t n = (size_t) sr * 3;
    Buf in = makeDrumTrack (sr, n, 11);
    Buf key = makeDrumTrack (sr, n, 12);
    for (int variant = 0; variant < 4; ++variant)
    {
        tg::GateSettings s;
        s.keyHpfHz = 80.0f; s.keyHighCutHz = 8000.0f; s.trackingPct = 70.0f; s.rangeDb = -40.0f; s.holdMs = 15.0f;
        s.externalKey = variant & 1; s.listen = variant == 2; s.lookaheadMs = variant == 3 ? 0.0f : 10.0f;
        s.levelerOn = true; s.levelTarget = variant == 1 ? -10.0f : tg::GateSettings::kAutoTarget;
        s.glow.on = variant != 2; s.glow.type = variant % 2; s.glow.drive = 60.0f; s.glow.envAmount = variant == 1 ? -70.0f : 80.0f;
        s.spectral.on = variant != 3; s.spectral.focus = 70.0f; s.spectral.sustain = variant == 0 ? 80.0f : 30.0f;
        Buf a = run (in, s, sr, 512, &key);
        Buf b = run (in, s, sr, 0, &key, 99);
        Buf c = run (in, s, sr, 1, &key);
        bool same = true;
        for (int ch = 0; ch < 2; ++ch) for (size_t i = 0; i < n; ++i)
            same &= a[(size_t) ch][i] == b[(size_t) ch][i] && a[(size_t) ch][i] == c[(size_t) ch][i];
        CHECK (same, "variant %d differs with block size", variant);
    }
}

static void testNoAllocations()
{
    std::printf ("No heap allocations on the audio path\n");
    const double sr = 96000;
    const size_t n = (size_t) sr * 2;
    Buf in = makeDrumTrack (sr, n, 21), key = makeDrumTrack (sr, n, 22);
    tg::GateSettings s; s.keyHpfHz = 60.0f; s.trackingPct = 100.0f; s.externalKey = true;
    s.levelerOn = true; s.glow.on = true; s.glow.drive = 80.0f; s.glow.envAmount = 60.0f; s.glow.type = tg::MicroGlow::iron;
    s.spectral.on = true; s.spectral.focus = 90.0f; s.spectral.sustain = 60.0f;
    tg::GateEngine e;
    e.prepare (sr, 2);
    run (in, s, sr, 0, &key, 5, &e, true);
    CHECK (gAllocCount == 0, "%d allocations", gAllocCount.load());
}

static int countOpens (tg::GateEngine& e) { int k = 0; tg::Hit h; while (e.hits.pop (h)) ++k; return k; }

static void testHysteresis()
{
    std::printf ("Hysteresis: a wobbling decay closes once instead of chattering\n");
    const double sr = 48000;
    const size_t n = (size_t) sr * 2;
    Buf in = makeBuf (1, n);
    for (size_t i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const double levelDb = -20.0 - 15.0 * t + 1.5 * std::sin (2 * M_PI * 6 * t);   // slides down through -30 with a wobble
        in[0][i] = (float) (std::pow (10.0, levelDb / 20.0) * std::sin (2 * M_PI * 300 * t));
    }
    tg::GateSettings s; s.thresholdDb = -30.0f; s.holdMs = 0.0f;
    s.hysteresisDb = 0.0f;
    tg::GateEngine a; run (in, s, sr, 512, nullptr, 1, &a);
    s.hysteresisDb = 6.0f;
    tg::GateEngine b; run (in, s, sr, 512, nullptr, 1, &b);
    const int chatter = countOpens (a), clean = countOpens (b);
    std::printf ("  opens without hysteresis: %d, with 6 dB: %d\n", chatter, clean);
    CHECK (clean == 1, "6 dB hysteresis opened %d times", clean);
    CHECK (chatter > 1, "test signal didn't chatter without hysteresis");
}

static void testHoldReleaseRange()
{
    std::printf ("Hold, release time and range land where the knobs say\n");
    const double sr = 48000;
    const size_t n = (size_t) sr;
    Buf in = makeBuf (1, n), key = makeBuf (1, n);
    for (size_t i = 0; i < n; ++i) in[0][i] = 0.5f;          // DC audio: output = 0.5 * gain
    for (size_t i = 1000; i < 10000; ++i) key[0][i] = 0.5f;  // key on for 9000 samples
    auto closeAt = [&] (const Buf& out) { size_t k = 10000; while (k < n && out[0][k] >= 0.5f) ++k; return k; };
    for (float range : { -80.0f, -40.0f, -20.0f })
    {
        tg::GateSettings s; s.externalKey = true; s.holdMs = 0.0f; s.releaseMs = 100.0f; s.rangeDb = range;
        s.lookaheadMs = 0.0f; s.attackMs = 0.01f;
        const size_t noHold = closeAt (run (in, s, sr, 0, &key));
        s.holdMs = 50.0f;
        Buf out = run (in, s, sr, 0, &key);
        const size_t c = closeAt (out);
        const double holdMs = (double) (c - noHold) * 1000.0 / sr;
        const float floorLin = range <= -80.0f ? 0.0f : 0.5f * dbToLin (range);
        size_t floorAt = c;
        while (floorAt < n && out[0][floorAt] > floorLin + 1e-6f) ++floorAt;
        const double relMs = (double) (floorAt - c) * 1000.0 / sr;
        std::printf ("  range %.0f: hold added %.2f ms, released in %.1f ms, floor %.4f (detector let go %.1f ms after the key)\n",
                     range, holdMs, relMs, out[0][n - 1], (noHold - 10000) * 1000.0 / sr);
        CHECK (std::abs (holdMs - 50.0) < 0.1, "hold %.2f ms", holdMs);
        CHECK (std::abs (relMs - 100.0) < 2.0, "release %.1f ms", relMs);
        CHECK (std::abs (out[0][n - 1] - floorLin) < 1e-5f, "range floor %.5f vs %.5f", out[0][n - 1], floorLin);
        CHECK (std::abs (out[0][500] - floorLin) < 1e-6f, "not closed before the key");
    }
}

static void testDetectionHpf()
{
    std::printf ("Key HPF keeps low bleed from opening the gate\n");
    const double sr = 48000;
    const size_t n = (size_t) sr;
    Buf in = makeBuf (1, n);
    for (size_t i = 0; i < n; ++i) in[0][i] = 0.3f * (float) std::sin (2 * M_PI * 50 * i / sr);   // -10 dBFS of 50 Hz
    tg::GateSettings s; s.thresholdDb = -30.0f;
    tg::GateEngine a; run (in, s, sr, 512, nullptr, 1, &a);
    s.keyHpfHz = 300.0f;
    tg::GateEngine b; run (in, s, sr, 512, nullptr, 1, &b);
    CHECK (countOpens (a) == 1, "no-HPF gate didn't open on 50 Hz");
    CHECK (countOpens (b) == 0, "HPF at 300 Hz still opened on 50 Hz");
}

static void testTrackingHighCut()
{
    std::printf ("Tracking high cut: open on the strike, closing as the hit decays, re-opening on a soft hit\n");
    const double sr = 48000;
    tg::GateEngine e; e.prepare (sr, 1);
    tg::GateSettings s; s.keyHighCutHz = 12000.0f; s.trackingPct = 100.0f;
    e.setSettings (s); e.reset();
    std::vector<float> x ((size_t) sr * 2);
    addHit (x, 4800, 0.9f, 2000.0f, sr, 40.0f, 1);      // loud hit decaying ~0.22 dB/ms
    addHit (x, 48000, 0.06f, 2000.0f, sr, 40.0f, 2);    // soft hit 23 dB quieter, 1 s later
    double atStrike = 0, after = 0, softStrike = 0;
    for (size_t pos = 0; pos < x.size(); pos += 16)
    {
        float* p = x.data() + pos;
        e.process (&p, 1, nullptr, 0, 16);
        if (pos == 4800 + 160) atStrike = e.currentHighCutHz();          // 3 ms in
        if (pos == 4800 + 4800) after = e.currentHighCutHz();            // 100 ms in: ~22 dB down
        if (pos == 48000 + 160) softStrike = e.currentHighCutHz();
        tg::MeterFrame f; while (e.meters.pop (f)) {}
    }
    std::printf ("  cutoff at the strike %.0f Hz, 100 ms later %.0f Hz, at the soft hit %.0f Hz\n", atStrike, after, softStrike);
    CHECK (atStrike > 10000.0, "cutoff not open on the strike (%.0f)", atStrike);
    CHECK (after < 4000.0, "cutoff didn't close as the hit decayed (%.0f)", after);
    CHECK (softStrike > 10000.0, "soft hit after a loud one didn't re-open the high cut (%.0f)", softStrike);
}

static void testHitsAndExternalKey()
{
    std::printf ("Every hit is reported once with the right time and level; external key drives the gate\n");
    const double sr = 48000;
    const size_t n = (size_t) sr * 2;
    Buf in = makeBuf (1, n), key = makeBuf (1, n);
    for (size_t i = 0; i < n; ++i) in[0][i] = 0.25f * (float) std::sin (2 * M_PI * 80 * i / sr);   // steady audio
    const size_t starts[] = { 4800, 30000, 60000 };
    const float amps[] = { 0.9f, 0.3f, 0.1f };
    for (int h = 0; h < 3; ++h) addHit (key[0], starts[h], amps[h], 200.0f, sr, 30.0f, (unsigned) h);
    tg::GateSettings s; s.externalKey = true; s.thresholdDb = -40.0f; s.lookaheadMs = 5.0f;
    tg::GateEngine e;
    Buf out = run (in, s, sr, 0, &key, 3, &e);
    std::vector<tg::Hit> hs; tg::Hit h; while (e.hits.pop (h)) hs.push_back (h);
    CHECK (hs.size() == 3, "%d hits reported", (int) hs.size());
    for (size_t i = 0; i < std::min<size_t> (3, hs.size()); ++i)
    {
        const long dt = (long) hs[i].arrivalSample - (long) starts[i];
        std::printf ("  hit %d: arrived %+ld samples from the strike, peak %.1f dBFS, output time %llu\n", (int) i, dt, hs[i].peakDb, (unsigned long long) hs[i].timeSamples);
        CHECK (dt >= 0 && dt < 48, "hit %d detected %ld samples off", (int) i, dt);
        CHECK (hs[i].timeSamples == hs[i].arrivalSample + (uint64_t) e.currentLatency(), "hit %d output time not shifted by the latency", (int) i);
    }
    if (hs.size() == 3) CHECK (hs[0].peakDb > hs[1].peakDb && hs[1].peakDb > hs[2].peakDb, "hit levels out of order");
    // audio between hits is gated shut, audio during a hit passes
    CHECK (std::abs (out[0][2000]) < 1e-6f, "gate open before the first key hit");
    const size_t L = (size_t) e.currentLatency();
    double during = 0; for (size_t i = 4800 + L; i < 4800 + 2400; ++i) during = std::max (during, (double) std::abs (out[0][i]));
    CHECK (during > 0.24, "gate didn't open on the key hit");
}

static void testBypassKeepsLatency()
{
    std::printf ("Bypass keeps the same latency\n");
    const double sr = 48000;
    tg::GateEngine e; e.prepare (sr, 1);
    tg::GateSettings s; s.lookaheadMs = 10.0f; e.setSettings (s); e.reset();
    std::vector<float> x (2048, 0.0f); x[100] = 1.0f;
    float* p = x.data();
    e.processBypassed (&p, 1, 2048);
    CHECK (x[100 + (size_t) e.currentLatency()] == 1.0f, "bypassed impulse not delayed by the latency");
}

static void testListenAndStability()
{
    std::printf ("Listen plays the filtered key; long silence and tiny signals stay finite\n");
    const double sr = 44100;
    const size_t n = (size_t) sr * 4;
    Buf in = makeBuf (1, n);
    for (size_t i = 0; i < (size_t) sr; ++i) in[0][i] = 0.5f * (float) std::sin (2 * M_PI * 1000 * i / sr);
    for (size_t i = (size_t) sr * 2; i < n; ++i) in[0][i] = 1e-30f;   // denormal territory
    tg::GateSettings s; s.listen = true; s.keyHpfHz = 2000.0f;
    Buf out = run (in, s, sr, 0);
    double pk = 0; bool finite = true;
    for (size_t i = 4410; i < (size_t) sr; ++i) pk = std::max (pk, (double) std::abs (out[0][i]));
    for (float v : out[0]) finite &= std::isfinite (v);
    std::printf ("  1 kHz through a 2 kHz key HPF (24 dB/oct): %.1f dB\n", 20 * std::log10 (pk / 0.5));
    CHECK (pk < 0.5 * 0.1 && pk > 0.5 * 0.01, "listen level %.4f not consistent with the HPF", pk);
    CHECK (finite, "non-finite output");
}


//==============================================================================
// Phase 2: leveler
//==============================================================================

// A hit-only track: hits with the given peak levels (dBFS), 250 ms apart. Returns the start samples.
static std::vector<size_t> makeHits (std::vector<float>& x, double sr, const std::vector<float>& peaksDb, size_t spacing, float decayMs = 60.0f)
{
    std::vector<size_t> starts;
    size_t at = 2400;
    for (float pdb : peaksDb)
    {
        std::vector<float> one ((size_t) (decayMs * 0.001 * sr * 6), 0.0f);
        addHit (one, 0, 1.0f, 110.0f, sr, decayMs, (unsigned) at);
        float pk = 0; for (float v : one) pk = std::max (pk, std::abs (v));
        const float scale = dbToLin (pdb) / pk;
        for (size_t i = 0; i < one.size() && at + i < x.size(); ++i) x[at + i] += one[i] * scale;
        starts.push_back (at);
        at += spacing;
    }
    return starts;
}

static float peakDbIn (const std::vector<float>& x, size_t from, size_t to)
{
    float pk = 0; for (size_t i = from; i < std::min (to, x.size()); ++i) pk = std::max (pk, std::abs (x[i]));
    return 20.0f * std::log10 (std::max (pk, 1e-9f));
}

static tg::GateSettings levelerSettings()
{
    tg::GateSettings s;
    s.thresholdDb = -40.0f; s.holdMs = 20.0f; s.releaseMs = 100.0f; s.lookaheadMs = 5.0f;
    s.levelerOn = true; s.levelAmount = 100.0f; s.levelTarget = -12.0f; s.levelMaxDb = 24.0f;
    return s;
}

static void testLevelerFixedTarget()
{
    std::printf ("Leveler: every hit lands on the target; Amount and Max scale and limit the change\n");
    const double sr = 48000;
    const std::vector<float> peaks { -6, -16, -4, -15, -20, -9, -18, -12, -5, -19 };
    Buf in = makeBuf (1, 12000 * peaks.size() + 24000);
    const auto starts = makeHits (in[0], sr, peaks, 12000);

    for (float amount : { 100.0f, 50.0f })
        for (float maxDb : { 24.0f, 6.0f })
        {
            tg::GateSettings s = levelerSettings(); s.levelAmount = amount; s.levelMaxDb = maxDb;
            tg::GateEngine e;
            Buf out = run (in, s, sr, 0, nullptr, 3, &e);
            const size_t L = (size_t) e.currentLatency();
            double worst = 0;
            for (size_t k = 0; k < starts.size(); ++k)
            {
                const float inPk  = peakDbIn (in[0], starts[k], starts[k] + 480);
                const float outPk = peakDbIn (out[0], starts[k] + L, starts[k] + L + 480);
                const float want  = inPk + std::clamp (amount * 0.01f * (-12.0f - inPk), -maxDb, maxDb);
                worst = std::max (worst, (double) std::abs (outPk - want));
            }
            std::printf ("  amount %3.0f %%, max %2.0f dB: worst hit %.3f dB from where it should land\n", amount, maxDb, worst);
            CHECK (worst < 0.05, "amount %.0f max %.0f: off by %.3f dB", amount, maxDb, worst);
        }
}

static void testLevelerAuto()
{
    std::printf ("Leveler Auto: evens hits around their own average (no target to set)\n");
    const double sr = 48000;
    std::vector<float> peaks;
    std::mt19937 rng (5);
    std::uniform_real_distribution<float> u (-14.0f, -2.0f);
    for (int k = 0; k < 40; ++k) peaks.push_back (u (rng));
    Buf in = makeBuf (1, 12000 * peaks.size() + 24000);
    const auto starts = makeHits (in[0], sr, peaks, 12000);
    tg::GateSettings s = levelerSettings(); s.levelTarget = tg::GateSettings::kAutoTarget;
    tg::GateEngine e;
    Buf out = run (in, s, sr, 0, nullptr, 3, &e);
    const size_t L = (size_t) e.currentLatency();
    auto spread = [&] (const std::vector<float>& x, size_t off)
    {
        float lo = 1e9f, hi = -1e9f, sum = 0;
        for (size_t k = 8; k < starts.size(); ++k)
        {
            const float p = peakDbIn (x, starts[k] + off, starts[k] + off + 480);
            lo = std::min (lo, p); hi = std::max (hi, p); sum += p;
        }
        return std::make_pair (hi - lo, sum / (float) (starts.size() - 8));
    };
    const auto [inSpread, inMean] = spread (in[0], 0);
    const auto [outSpread, outMean] = spread (out[0], L);
    std::printf ("  hit-to-hit spread %.1f dB -> %.1f dB, average level %.1f -> %.1f dBFS\n", inSpread, outSpread, inMean, outMean);
    CHECK (outSpread < 0.4f * inSpread, "spread only went from %.1f to %.1f dB", inSpread, outSpread);
    CHECK (std::abs (outMean - inMean) < 1.5f, "Auto moved the overall level by %.1f dB", outMean - inMean);
}

static void testLevelerTransientUntouched()
{
    std::printf ("Leveler: the gain is in place before the hit, so the transient is never ramped\n");
    std::printf ("  (it judges each hit by what it sees before committing: lookahead - glide - 0.5 ms)\n");
    const double sr = 48000;
    for (float laMs : { 2.0f, 5.0f, 10.0f, 20.0f })
    {
        Buf in = makeBuf (1, 48000);
        // loud, soft, loud; 250 ms apart, so each tail has died away under the next hit
        const auto starts = makeHits (in[0], sr, { -3.0f, -17.0f, -6.0f }, 12000);
        tg::GateSettings s = levelerSettings(); s.lookaheadMs = laMs;
        tg::GateEngine e;
        Buf out = run (in, s, sr, 0, nullptr, 7, &e);
        const size_t L = (size_t) e.currentLatency();
        for (size_t k = 1; k < 3; ++k)
        {
            // over the hit's first 20 ms, output / input must be one constant gain
            double gMin = 1e9, gMax = 0;
            for (size_t i = starts[k]; i < starts[k] + 960; ++i)
                if (std::abs (in[0][i]) > 1e-3f)
                {
                    const double gk = out[0][i + L] / in[0][i];
                    gMin = std::min (gMin, gk); gMax = std::max (gMax, gk);
                }
            const int la = (int) std::lround (laMs * 48), G = std::max (std::min (la / 4, 48), (int) (0.4 * la)), P = std::min (24, la / 8);
            const size_t W = (size_t) (la - G - P);
            const double expect = std::pow (10.0, (-12.0 - peakDbIn (in[0], starts[k], starts[k] + W)) / 20.0);
            if (k == 1) std::printf ("  lookahead %2.0f ms: sees the first %.2f ms of each hit\n", laMs, W / 48.0);
            CHECK (gMax / gMin < 1.0001, "lookahead %.0f hit %d: gain moved %.4f dB during the hit", laMs, (int) k, 20 * std::log10 (gMax / gMin));
            CHECK (std::abs (20 * std::log10 (gMin / expect)) < 0.05, "lookahead %.0f hit %d: gain %.2f dB, expected %.2f", laMs, (int) k, 20 * std::log10 (gMin), 20 * std::log10 (expect));
        }
    }
}

static void testLevelerRolls()
{
    std::printf ("Leveler: every stroke of a roll is a new hit, even though the gate never closes\n");
    const double sr = 48000;
    Buf in = makeBuf (1, 48000);
    std::vector<size_t> starts;
    for (int k = 0; k < 16; ++k)
    {
        const size_t at = 2400 + (size_t) k * 1920;   // 40 ms apart, loud/soft alternating
        addHit (in[0], at, k % 2 ? 0.15f : 0.7f, 180.0f, sr, 12.0f, (unsigned) k);
        starts.push_back (at);
    }
    tg::GateSettings s = levelerSettings(); s.holdMs = 200.0f;
    tg::GateEngine e;
    Buf out = run (in, s, sr, 0, nullptr, 9, &e);
    int n = 0; tg::Hit h; while (e.hits.pop (h)) ++n;
    const size_t L = (size_t) e.currentLatency();
    double lo = 1e9, hi = -1e9;
    for (size_t k = 1; k < starts.size(); ++k)
    {
        const double p = peakDbIn (out[0], starts[k] + L, starts[k] + L + 240);
        lo = std::min (lo, p); hi = std::max (hi, p);
    }
    std::printf ("  %d strokes -> %d hits; leveled strokes within %.2f dB of each other (input: %.1f dB apart)\n",
                 (int) starts.size(), n, hi - lo, 20 * std::log10 (0.7 / 0.15));
    CHECK (n == (int) starts.size(), "%d hits for %d strokes", n, (int) starts.size());
    CHECK (hi - lo < 0.5, "strokes still %.2f dB apart", hi - lo);
}

static void testLevelerNoLookahead()
{
    std::printf ("Leveler with Lookahead Off still levels (gain lands 2 ms into the hit)\n");
    const double sr = 48000;
    Buf in = makeBuf (1, 48000);
    const auto starts = makeHits (in[0], sr, { -3.0f, -17.0f }, 12000);
    tg::GateSettings s = levelerSettings(); s.lookaheadMs = 0.0f;
    tg::GateEngine e;
    Buf out = run (in, s, sr, 0, nullptr, 2, &e);
    const size_t L = (size_t) e.currentLatency(), i = starts[1] + 240;   // 5 ms into the soft hit
    const double gotDb = 20 * std::log10 (std::abs (out[0][i + L] / in[0][i]));
    const double wantDb = -12.0 - peakDbIn (in[0], starts[1], starts[1] + 48);
    std::printf ("  5 ms into the soft hit: %+.2f dB (aimed %+.2f from the first 1 ms)\n", gotDb, wantDb);
    CHECK (std::abs (gotDb - wantDb) < 0.05, "gain %.2f dB, expected %.2f", gotDb, wantDb);
}

static void testLevelerLeavesBleedAlone()
{
    std::printf ("Leveler with the threshold all the way down: bleed far under the hits isn't pulled up, gain moves smoothly\n");
    const double sr = 48000;
    std::vector<float> peaks;
    for (int k = 0; k < 16; ++k) { peaks.push_back (k % 4 == 1 ? -14.0f : -6.0f); peaks.push_back (-32.0f); }   // hits, with bleed between
    Buf in = makeBuf (1, 6000 * peaks.size() + 24000);
    const auto starts = makeHits (in[0], sr, peaks, 6000);
    tg::GateSettings s = levelerSettings(); s.thresholdDb = -80.0f; s.hysteresisDb = 0.0f; s.levelTarget = tg::GateSettings::kAutoTarget;
    s.levelMaxDb = 21.9f; s.lookaheadMs = 10.0f;
    tg::GateEngine e;
    Buf out = run (in, s, sr, 0, nullptr, 4, &e);
    const size_t L = (size_t) e.currentLatency();
    double worstBleed = -1e9;
    for (size_t k = 5; k < starts.size(); k += 2)
        worstBleed = std::max (worstBleed, (double) (peakDbIn (out[0], starts[k] + L, starts[k] + L + 480) - peakDbIn (in[0], starts[k], starts[k] + 480)));
    std::printf ("  bleed events turned up by at most %+.1f dB (100 %% Amount, Max 21.9 dB would pull them up ~20 dB)\n", worstBleed);
    CHECK (worstBleed < 6.0, "bleed pulled up %+.1f dB", worstBleed);
}

static void testLevelerClickFreeAndOff()
{
    std::printf ("Leveler: gain changes and On/Off are click-free; Off is bit-identical\n");
    const double sr = 48000;
    const size_t n = (size_t) sr * 3;
    Buf in = makeBuf (1, n), key = makeBuf (1, n);
    for (size_t i = 0; i < n; ++i) in[0][i] = 0.5f * (float) std::sin (2 * M_PI * 100 * i / sr);
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> u (0.05f, 0.9f);
    for (size_t at = 1000; at + 3000 < n; at += 6000) addHit (key[0], at, u (rng), 200.0f, sr, 20.0f, (unsigned) at);
    tg::GateSettings s = levelerSettings(); s.externalKey = true; s.rangeDb = 0.0f; s.levelMaxDb = 12.0f; s.levelTarget = -6.0f;
    tg::GateEngine e; e.prepare (sr, 1); e.setSettings (s); e.reset();
    std::vector<float> x = in[0];
    double worst = 0; float prev = 0;
    for (size_t pos = 0; pos < n; pos += 256)
    {
        s.levelerOn = ((pos / 12000) % 2) == 0;
        e.setSettings (s);
        float* p = x.data() + pos; const float* kp = key[0].data() + pos;
        e.process (&p, 1, &kp, 1, (int) std::min<size_t> (256, n - pos));
        tg::MeterFrame f; while (e.meters.pop (f)) {}
        for (size_t i = 0; i < 256 && pos + i < n; ++i) { if (pos + i > 0) worst = std::max (worst, (double) std::abs (p[i] - prev)); prev = p[i]; }
    }
    // 12 dB over a 1 ms glide on a 0.5 sine: ~0.5 * (4 - 1) / 48 per sample, plus the sine's own slope
    const double bound = 0.5 * 2 * M_PI * 100 / sr * 4.0 + 0.5 * (3.98 - 1.0) / 48.0;
    std::printf ("  worst sample-to-sample jump %.4f (bound %.4f)\n", worst, bound);
    CHECK (worst <= bound, "jump %.4f", worst);

    tg::GateSettings a = levelerSettings(); a.levelerOn = false;
    tg::GateSettings b = a; b.levelAmount = 0.0f; b.levelMaxDb = 0.0f;
    Buf drums = makeDrumTrack (sr, n, 4);
    Buf oa = run (drums, a, sr, 0), ob = run (drums, b, sr, 0);
    CHECK (oa == ob, "leveler Off isn't bit-identical");
}

//==============================================================================
// Phase 2: Micro Glow
//==============================================================================

static double goertzelDb (const std::vector<float>& x, size_t from, size_t len, double f, double sr)
{
    // windowed (Blackman-Harris) single-bin DFT, amplitude in dBFS
    const double w = 2 * M_PI * f / sr;
    double re = 0, im = 0, wsum = 0;
    for (size_t i = 0; i < len; ++i)
    {
        const double t = (double) i / (double) (len - 1);
        const double win = 0.35875 - 0.48829 * std::cos (2 * M_PI * t) + 0.14128 * std::cos (4 * M_PI * t) - 0.01168 * std::cos (6 * M_PI * t);
        re += win * x[from + i] * std::cos (w * (double) i);
        im -= win * x[from + i] * std::sin (w * (double) i);
        wsum += win;
    }
    return 20 * std::log10 (std::max (2 * std::sqrt (re * re + im * im) / wsum, 1e-12));
}

static tg::GateSettings glowSettings (int type, float drive, float env = 0.0f)
{
    tg::GateSettings s; s.rangeDb = 0.0f;   // gate fully open: only Micro Glow acts
    s.glow.on = true; s.glow.type = type; s.glow.drive = drive; s.glow.envAmount = env;
    return s;
}

static const char* glowName (int t) { return t == tg::MicroGlow::tube ? "TUBE" : "IRON"; }

static Buf tone (double f, float amp, double sr, size_t n)
{
    Buf b = makeBuf (1, n);
    for (size_t i = 0; i < n; ++i) b[0][i] = amp * (float) std::sin (2 * M_PI * f * (double) i / sr);
    return b;
}

// harmonics 2..8 relative to the fundamental, in dB
static double thdDb (const std::vector<float>& x, double f0, double sr)
{
    const double h1 = std::pow (10.0, goertzelDb (x, 32768, 32768, f0, sr) / 20.0);
    double p = 0;
    for (int k = 2; k <= 8 && k * f0 < sr / 2; ++k) p += std::pow (std::pow (10.0, goertzelDb (x, 32768, 32768, k * f0, sr) / 20.0), 2.0);
    return 10 * std::log10 (p / (h1 * h1) + 1e-30);
}

static double rmsDb (const std::vector<float>& x, size_t from, size_t to)
{
    double p = 0; for (size_t i = from; i < to; ++i) p += (double) x[i] * x[i];
    return 10 * std::log10 (p / (double) (to - from) + 1e-30);
}

static void testGlowTransparentBelowCurve()
{
    std::printf ("Micro Glow below the curve: linear-phase and flat (null vs the delayed input)\n");
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        const size_t n = (size_t) sr * 2;
        Buf in = makeBuf (1, n);
        const double freqs[] = { 40, 100, 440, 1000, 3000, 8000, 12000, 16000, 18000 };
        for (size_t i = 0; i < n; ++i)
            for (double f : freqs) in[0][i] += 0.003f * (float) std::sin (2 * M_PI * f * (double) i / sr + f);
        for (int t = 0; t < 2; ++t)
        {
            tg::GateEngine e;
            Buf out = run (in, glowSettings (t, 0.0f), sr, 0, nullptr, 1, &e);   // -50 dBFS tones: nowhere near the curve
            const size_t L = (size_t) e.currentLatency();
            double num = 0, den = 0;
            for (size_t i = n / 2; i < n; ++i) { const double d = out[0][i] - in[0][i - L]; num += d * d; den += (double) in[0][i - L] * in[0][i - L]; }
            const double nullDb = 10 * std::log10 (num / den);
            std::printf ("  %.0f kHz %s: null %.1f dB (40 Hz - 18 kHz, %dx oversampling)\n", sr / 1000, glowName (t), nullDb, sr < 60000 ? 8 : 4);
            CHECK (nullDb < -60.0, "sr %.0f %s null only %.1f dB", sr, glowName (t), nullDb);
        }
    }
}

static void testGlowDriveAndAutoGain()
{
    std::printf ("Micro Glow Drive: more harmonics as it goes up, loudness held by the auto gain\n");
    const double sr = 48000;
    const size_t n = 96000;
    Buf in = tone (80.0, 0.5f, sr, n);   // -6 dBFS, 80 Hz (inside IRON's flux band)
    const double inRms = rmsDb (in[0], n / 2, n);
    for (int t = 0; t < 2; ++t)
    {
        double prev = -300;
        std::printf ("  %s THD at Drive 0/25/50/75/100:", glowName (t));
        for (float d : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
        {
            tg::GateEngine e;
            Buf o = run (in, glowSettings (t, d), sr, 0, nullptr, 1, &e);
            const double thd = thdDb (o[0], 80.0, sr), lv = rmsDb (o[0], n / 2, n) - inRms;
            std::printf (" %.1f dB (level %+.2f)", thd, lv);
            CHECK (thd > prev, "%s THD didn't rise at Drive %.0f", glowName (t), d);
            CHECK (std::abs (lv) < 0.1, "%s level moved %.2f dB at Drive %.0f", glowName (t), lv, d);
            prev = thd;
        }
        std::printf ("\n");
    }
}

static void testIronLowsOnly()
{
    std::printf ("IRON saturates the lows; mids and highs pass clean (TUBE colors everything)\n");
    const double sr = 48000;
    const size_t n = 96000;
    for (int t = 0; t < 2; ++t)
    {
        const double lo = thdDb (run (tone (60.0, 0.5f, sr, n), glowSettings (t, 75.0f), sr, 0)[0], 60.0, sr);
        const double hi = thdDb (run (tone (2000.0, 0.5f, sr, n), glowSettings (t, 75.0f), sr, 0)[0], 2000.0, sr);
        std::printf ("  %s at Drive 75, -6 dBFS: 60 Hz THD %.1f dB, 2 kHz THD %.1f dB\n", glowName (t), lo, hi);
        if (t == tg::MicroGlow::iron) CHECK (hi < lo - 20.0, "IRON 2 kHz THD %.1f not well below 60 Hz %.1f", hi, lo);
    }
}

static void testGlowEvenHarmonics()
{
    std::printf ("Micro Glow harmonics: both types are asymmetric (2nd harmonic) like Glow\n");
    const double sr = 48000;
    for (int t = 0; t < 2; ++t)
    {
        Buf o = run (tone (93.75, 0.5f, sr, 98304), glowSettings (t, 75.0f), sr, 0);
        const double h1 = goertzelDb (o[0], 32768, 32768, 93.75, sr);
        const double h2 = goertzelDb (o[0], 32768, 32768, 187.5, sr) - h1, h3 = goertzelDb (o[0], 32768, 32768, 281.25, sr) - h1;
        std::printf ("  %s at Drive 75: 2nd %.1f dBc, 3rd %.1f dBc\n", glowName (t), h2, h3);
        CHECK (h2 > -60.0, "%s 2nd harmonic only %.1f dBc", glowName (t), h2);
    }
}

static void testGlowAliasing()
{
    std::printf ("Micro Glow aliasing, -6 dBFS tones (that don't divide 48 kHz, so aliases can't hide on harmonics)\n");
    const double sr = 48000;
    const size_t n = 98304;
    auto worstAlias = [&] (const std::vector<float>& x, double f0)
    {
        const double ref = goertzelDb (x, 32768, 32768, f0, sr);
        double w = -300;
        for (double f = 100; f < 20000; f += 13)
        {
            const double r = std::fmod (f, f0);
            if (r < 40 || r > f0 - 40) continue;   // skip the harmonics themselves
            w = std::max (w, goertzelDb (x, 32768, 32768, f, sr) - ref);
        }
        return w;
    };
    tg::MicroGlow mg; mg.prepare (sr, 1);
    for (float drive : { 50.0f, 100.0f })
        for (double f0 : { 2113.0, 6997.0 })
        {
            Buf in = tone (f0, 0.5f, sr, n);
            std::vector<float> naive (n);   // the TUBE curve straight at 48 kHz
            for (size_t i = 0; i < n; ++i) naive[i] = mg.tubeCurve (in[0][i], drive);
            const double tube = worstAlias (run (in, glowSettings (tg::MicroGlow::tube, drive), sr, 0)[0], f0);
            const double iron = worstAlias (run (in, glowSettings (tg::MicroGlow::iron, drive), sr, 0)[0], f0);
            const double raw = worstAlias (naive, f0);
            std::printf ("  Drive %3.0f, %4.0f Hz: TUBE %6.1f dBc (no oversampling %5.1f), IRON %6.1f dBc\n", drive, f0, tube, raw, iron);
            if (f0 < 3000) CHECK (tube < (drive < 75 ? -90.0 : -80.0), "TUBE drive %.0f %.0f Hz alias %.1f dBc", drive, f0, tube);
            if (drive < 75) CHECK (tube < raw - 30.0, "TUBE drive %.0f %.0f Hz: oversampling only gained %.1f dB", drive, f0, raw - tube);
            CHECK (iron < -90.0, "IRON drive %.0f %.0f Hz alias %.1f dBc", drive, f0, iron);
        }
}

static void testGlowEnvelope()
{
    std::printf ("Micro Glow envelope (Glow's follower): hits above the track's average push Drive away from the knob and it comes back\n");
    const double sr = 48000;
    std::vector<float> peaks;
    std::mt19937 rng (8);
    std::uniform_real_distribution<float> u (-24.0f, -3.0f);
    for (int k = 0; k < 30; ++k) peaks.push_back (u (rng));
    Buf in = makeBuf (1, 12000 * peaks.size() + 24000);
    makeHits (in[0], sr, peaks, 12000);
    for (float env : { 0.0f, 100.0f, -100.0f })
    {
        tg::GateSettings s = glowSettings (tg::MicroGlow::tube, 40.0f, env);
        tg::GateEngine e; e.prepare (sr, 1); e.setSettings (s); e.reset();
        std::vector<float> x = in[0];
        float lo = 1e9f, hi = -1e9f;
        for (size_t pos = 0; pos + 256 <= x.size(); pos += 256)
        {
            float* p = x.data() + pos;
            e.process (&p, 1, nullptr, 0, 256);
            tg::MeterFrame f; while (e.meters.pop (f)) {}
            if (pos > 48000) { lo = std::min (lo, e.glowModulatedDrive()); hi = std::max (hi, e.glowModulatedDrive()); }
        }
        std::printf ("  amount %+4.0f %%: Drive moved between %.1f and %.1f (knob 40)\n", env, lo, hi);
        if (env > 0)      { CHECK (hi > 55.0 && lo >= 39.99f, "env + range %.1f..%.1f", lo, hi); }
        else if (env < 0) { CHECK (lo < 25.0 && hi <= 40.01f, "env - range %.1f..%.1f", lo, hi); }
        else              { CHECK (lo == 40.0f && hi == 40.0f, "env 0 moved Drive %.1f..%.1f", lo, hi); }
    }
}

static void testGlowSwitchingClickFree()
{
    std::printf ("Micro Glow: On/Off and type changes crossfade (no clicks beyond the waveforms' own steepness)\n");
    const double sr = 48000;
    const size_t n = (size_t) sr * 2;
    std::vector<float> src (n);
    for (size_t i = 0; i < n; ++i) src[i] = 0.7f * (float) std::sin (2 * M_PI * 100 * i / sr);
    auto worstJump = [&] (bool toggle, int fixedType, bool fixedOn)
    {
        std::vector<float> x = src;
        tg::GateSettings s = glowSettings (fixedType, 90.0f);
        s.glow.on = fixedOn;
        tg::GateEngine e; e.prepare (sr, 1); e.setSettings (s); e.reset();
        double worst = 0; float prev = 0;
        for (size_t pos = 0; pos < n; pos += 128)
        {
            if (toggle)
            {
                const size_t step = pos / 4800;   // every 100 ms
                s.glow.on = (step % 3) != 0;
                s.glow.type = (int) ((step / 2) % 2);
                e.setSettings (s);
            }
            float* p = x.data() + pos;
            e.process (&p, 1, nullptr, 0, 128);
            tg::MeterFrame f; while (e.meters.pop (f)) {}
            for (int i = 0; i < 128; ++i) { if (pos + (size_t) i > 4800) worst = std::max (worst, (double) std::abs (p[i] - prev)); prev = p[i]; }
        }
        return worst;
    };
    // steepest the steady signals ever get (dry, TUBE, IRON), then the switching run
    const double steady = std::max ({ worstJump (false, 0, false), worstJump (false, 0, true), worstJump (false, 1, true) });
    const double switching = worstJump (true, 0, true);
    // a crossfade between two signals at most ~1.4 apart over 240 samples adds ~0.006 per sample
    std::printf ("  steepest steady signal %.4f per sample; while switching %.4f\n", steady, switching);
    CHECK (switching <= steady + 1.4 / 240.0 + 1e-4, "switching jump %.4f vs steady %.4f", switching, steady);
}


//==============================================================================
// Phase 3: spectral decay (on the simulated tom mic, tests/DrumSim.h)
//==============================================================================

// energy (dB) of x in a band, summed over windows after each hit (output timeline: + off)
static double bandEnergyDb (const std::vector<float>& x, const std::vector<size_t>& at, size_t off, double lo, double hi, double fromMs, double toMs, double sr)
{
    tg::Svf h1, h2, l1, l2; h1.setup (lo, sr, 0.5412); h2.setup (lo, sr, 1.3066); l1.setup (hi, sr, 0.5412); l2.setup (hi, sr, 1.3066);
    std::vector<double> y (x.size());
    for (size_t i = 0; i < x.size(); ++i) { double hp, d, v = x[i]; h1.tick (v, hp); h2.tick (hp, v); v = l1.tick (v, d); v = l2.tick (v, d); y[i] = v; }
    double e = 0;
    for (size_t a : at)
        for (size_t i = a + off + (size_t) (fromMs * sr / 1000); i < std::min (x.size(), a + off + (size_t) (toMs * sr / 1000)); ++i) e += y[i] * y[i];
    return 10 * std::log10 (e + 1e-30);
}

static tg::GateSettings tomGate()
{
    tg::GateSettings s; s.thresholdDb = -24; s.hysteresisDb = 6; s.holdMs = 30; s.releaseMs = 250; s.lookaheadMs = 5;
    return s;
}

static void testSpectralNeutral()
{
    std::printf ("Spectral decay: Focus 0 + Sustain 0 is bit-identical to the plain gate; each hit's attack always is\n");
    const double sr = 48000;
    auto ses = sim::tomMicSession (sr, 12);
    Buf in = makeBuf (1, ses.mix.size()); in[0] = ses.mix;
    tg::GateEngine eOff; Buf off = run (in, tomGate(), sr, 0, nullptr, 3, &eOff);
    tg::GateSettings s = tomGate(); s.spectral.on = true; s.spectral.focus = 0; s.spectral.sustain = 0;
    Buf neutral = run (in, s, sr, 0, nullptr, 3);
    CHECK (neutral == off, "Focus 0 / Sustain 0 not bit-identical");
    s.spectral.focus = 100; s.spectral.sustain = 100;
    tg::GateEngine e; Buf on = run (in, s, sr, 0, nullptr, 3, &e);
    const size_t tr = (size_t) std::floor (e.spectralDecay().transientMs() * sr / 1000.0);
    std::vector<uint64_t> starts; tg::Hit h; while (e.hits.pop (h)) starts.push_back (h.timeSamples);
    bool same = true; size_t diff = 0;
    for (size_t k = 3; k < starts.size(); ++k)   // once the attack length is learned
        for (size_t i = starts[k]; i < starts[k] + tr && i < on[0].size(); ++i) { same &= on[0][i] == off[0][i]; diff += on[0][i] != off[0][i]; }
    bool changed = false; for (size_t i = 0; i < on[0].size(); ++i) changed |= on[0][i] != off[0][i];
    std::printf ("  %zu hits: the attack (%.1f ms, learned) identical to the plain gate (%zu samples differ); the tails differ: %s\n",
                 starts.size(), e.spectralDecay().transientMs(), diff, changed ? "yes" : "no");
    CHECK (same, "attack differs from the plain gate");
    CHECK (changed, "Focus 100 changed nothing");
}

static void testSpectralLearns()
{
    std::printf ("Spectral decay learns the tom's body band from the bleed-laden track, and the attack length from clean hits\n");
    const double sr = 48000;
    auto ses = sim::tomMicSession (sr, 20);
    Buf in = makeBuf (1, ses.mix.size()); in[0] = ses.mix;
    tg::GateSettings s = tomGate(); s.spectral.on = true;
    tg::GateEngine e; run (in, s, sr, 0, nullptr, 3, &e);
    auto& sp = e.spectralDecay();
    int nh = 0, extra = 0; tg::Hit h;
    while (e.hits.pop (h))
    {
        ++nh;
        long best = 1L << 40; for (auto t : ses.toms) best = std::min (best, std::labs ((long) h.arrivalSample - (long) t));
        if (best > 48) ++extra;
    }
    // the same toms with no bleed: the attack (the stick click, ~3 ms) can be measured on every hit
    Buf clean = makeBuf (1, ses.tomOnly.size()); clean[0] = ses.tomOnly;
    tg::GateEngine ec; run (clean, s, sr, 0, nullptr, 3, &ec);
    std::printf ("  %d hits for %zu toms (%d not a tom); body band up to %.0f Hz (the tom's modes: 110-321 Hz)\n"
                 "  attack: %.1f ms with the hats and crashes on top (hits buried in wash are skipped), %.1f ms on the clean toms (the click: ~3 ms, + the detector's 2 ms hold)\n",
                 nh, ses.toms.size(), extra, sp.bodyTopHz(), sp.transientMs(), ec.spectralDecay().transientMs());
    CHECK (nh == (int) ses.toms.size() && extra == 0, "%d hits, %d not toms", nh, extra);
    CHECK (sp.bodyTopHz() >= 300 && sp.bodyTopHz() <= 1200, "body band top %.0f Hz", sp.bodyTopHz());
    CHECK (ec.spectralDecay().transientMs() >= 3.0 && ec.spectralDecay().transientMs() <= 12.0, "clean attack %.1f ms", ec.spectralDecay().transientMs());
    CHECK (sp.transientMs() >= 3.0 && sp.transientMs() <= 25.0, "attack %.1f ms", sp.transientMs());
}

static void testSpectralCleansTail()
{
    std::printf ("Spectral decay: cymbal and hat wash closes in the tail, the tom body stays (vs the plain gate, 20-400 ms)\n");
    const double sr = 48000;
    auto ses = sim::tomMicSession (sr, 30);
    std::vector<size_t> plain, crashed (ses.crashedToms.begin() + 1, ses.crashedToms.end());
    for (size_t k = 4; k < ses.toms.size(); ++k)
        if (std::find (ses.crashedToms.begin(), ses.crashedToms.end(), ses.toms[k]) == ses.crashedToms.end()) plain.push_back (ses.toms[k]);
    Buf in = makeBuf (1, ses.mix.size()); in[0] = ses.mix;
    tg::GateEngine e0; Buf off = run (in, tomGate(), sr, 256, nullptr, 1, &e0);
    const size_t L = (size_t) e0.currentLatency();
    for (float focus : { 30.0f, 60.0f, 100.0f })
    {
        tg::GateSettings s = tomGate(); s.spectral.on = true; s.spectral.focus = focus;
        Buf o = run (in, s, sr, 256);
        for (int w = 0; w < 2; ++w)
        {
            const auto& at = w ? crashed : plain;
            const double hf = bandEnergyDb (o[0], at, L, 4000, 16000, 20, 400, sr) - bandEnergyDb (off[0], at, L, 4000, 16000, 20, 400, sr);
            const double body = bandEnergyDb (o[0], at, L, 60, 400, 20, 400, sr) - bandEnergyDb (off[0], at, L, 60, 400, 20, 400, sr);
            std::printf ("  Focus %3.0f, %-24s: cymbal/hats (>4 kHz) %6.1f dB, tom body (60-400 Hz) %+.1f dB\n", focus, w ? "toms with a crash on them" : "toms", hf, body);
            CHECK (hf < (focus >= 100 ? -14.0 : focus >= 60 ? -10.0 : -4.0), "Focus %.0f: wash only %.1f dB down", focus, hf);
            CHECK (std::abs (body) < 0.5, "Focus %.0f: tom body moved %.1f dB", focus, body);
        }
    }
}

static void testSpectralSustain()
{
    std::printf ("Spectral decay Sustain: the tom's low end rings on past the gate; the highs and the wash don't\n");
    const double sr = 48000;
    const int nT = 14; const size_t n = (size_t) ((nT * 1.5 + 2) * sr);
    std::vector<float> tom (n, 0.0f), bleed (n, 0.0f);
    std::vector<size_t> at;
    for (int k = 0; k < nT; ++k) { const size_t a = (size_t) ((0.3 + 1.5 * k) * sr); sim::addTom (tom, a, 0.9f, 110, sr, (unsigned) k); if (k >= 4) at.push_back (a); }
    for (size_t a = (size_t) (0.05 * sr); a < n; a += (size_t) (0.25 * sr)) sim::addHat (bleed, a, 0.06f, sr, (unsigned) a);
    Buf mix = makeBuf (1, n), tomOnly = makeBuf (1, n);
    for (size_t i = 0; i < n; ++i) { mix[0][i] = tom[i] + bleed[i]; tomOnly[0][i] = tom[i]; }
    tg::GateSettings base; base.thresholdDb = -18; base.hysteresisDb = 4; base.holdMs = 0; base.releaseMs = 60; base.lookaheadMs = 5;
    tg::GateEngine e0; Buf off = run (mix, base, sr, 256, nullptr, 1, &e0);
    const size_t L = (size_t) e0.currentLatency();
    tg::GateSettings open = base; open.rangeDb = 0;
    Buf natural = run (tomOnly, open, sr, 256);   // the tom's own ring, never gated
    const double n1 = bandEnergyDb (natural[0], at, L, 60, 400, 300, 900, sr), n2 = bandEnergyDb (natural[0], at, L, 60, 400, 700, 1400, sr);
    const double hiPlain = bandEnergyDb (off[0], at, L, 1000, 16000, 100, 900, sr);
    Buf bleedOnly = makeBuf (1, n); bleedOnly[0] = bleed;
    const double rawWash = bandEnergyDb (bleedOnly[0], at, 0, 4000, 16000, 300, 1400, sr);   // the hats as the mic hears them
    double prev = -1e9;
    for (float sus : { 0.0f, 25.0f, 50.0f, 75.0f, 100.0f })
    {
        tg::GateSettings s = base; s.spectral.on = true; s.spectral.focus = 80; s.spectral.sustain = sus;
        Buf o = run (mix, s, sr, 256);
        const double b1 = bandEnergyDb (o[0], at, L, 60, 400, 300, 900, sr) - n1;
        const double b2 = std::max (-99.0, bandEnergyDb (o[0], at, L, 60, 400, 700, 1400, sr) - n2);
        const double hf = bandEnergyDb (o[0], at, L, 4000, 16000, 300, 1400, sr) - rawWash;
        const double hi = bandEnergyDb (o[0], at, L, 1000, 16000, 100, 900, sr) - hiPlain;
        std::printf ("  Sustain %3.0f: low end vs its natural decay %+.1f dB (300-900 ms), %+.1f dB (700-1400 ms); highs (>1 kHz) vs the plain gate %+.1f dB; hat wash vs the raw hats %.1f dB\n", sus, b1, b2, hi, hf);
        CHECK (b1 >= prev - 0.05, "Sustain %.0f rang less than a lower setting", sus);
        CHECK (hf < -40.0, "Sustain %.0f let the wash through (%.1f dB vs raw)", sus, hf);
        CHECK (hi < 0.5, "Sustain %.0f made the highs ring longer than the gate (%+.1f dB)", sus, hi);
        if (sus >= 100) CHECK (b1 > -1.5 && b2 > -3.0, "Sustain 100 ring %.1f / %.1f dB", b1, b2);
        if (sus == 0)   CHECK (b2 < -60.0, "Sustain 0 still rang (%.1f dB)", b2);
        prev = b1;
    }
}

static void testSpectralTransientKnob()
{
    std::printf ("Spectral decay Transient knob: the untouched attack window follows it (half .. double the learned length)\n");
    const double sr = 48000;
    auto ses = sim::tomMicSession (sr, 12);
    Buf in = makeBuf (1, ses.mix.size()); in[0] = ses.mix;
    Buf off = run (in, tomGate(), sr, 256);
    double prevMs = 0.0;
    for (float t : { -100.0f, 0.0f, 100.0f })
    {
        tg::GateSettings s = tomGate(); s.spectral.on = true; s.spectral.focus = 100; s.spectral.transient = t;
        tg::GateEngine e; Buf o = run (in, s, sr, 256, nullptr, 1, &e);
        std::vector<uint64_t> starts; tg::Hit h; while (e.hits.pop (h)) starts.push_back (h.timeSamples);
        // how long each hit stays identical to the plain gate (median over the later hits)
        std::vector<double> ms;
        for (size_t k = 4; k < starts.size(); ++k)
        {
            size_t i = starts[k];
            while (i < o[0].size() && o[0][i] == off[0][i]) ++i;
            ms.push_back ((i - starts[k]) * 1000.0 / sr);
        }
        std::sort (ms.begin(), ms.end());
        const double med = ms[ms.size() / 2];
        std::printf ("  Transient %+4.0f %%: attack untouched for %.1f ms (x %.2f of the attack length in effect)\n", t, med, std::pow (2.0, t / 100.0));
        CHECK (med >= 1.5 && med <= 100.0, "untouched for %.1f ms", med);
        if (prevMs > 0) CHECK (med > 1.6 * prevMs && med < 2.5 * prevMs, "Transient %.0f: %.1f ms vs %.1f at the setting below", t, med, prevMs);
        prevMs = med;
    }
}

static void testSpectralRestRampClean()
{
    std::printf ("Spectral decay: when a stroke comes in, the closed highs open back up like the gate's attack (no clicks)\n");
    const double sr = 48000;
    const size_t n = (size_t) sr * 3;
    Buf in = makeBuf (1, n), key = makeBuf (1, n);
    std::mt19937 rng (2);
    std::normal_distribution<float> g (0.0f, 1.0f);
    for (size_t i = 0; i < n; ++i) in[0][i] = 0.3f * (float) std::sin (2 * M_PI * 3100 * i / sr) + 0.05f * g (rng);   // steady high content
    for (size_t a = 2400; a + 4800 < n; a += 7200) addHit (key[0], a, 0.8f, 150.0f, sr, 20.0f, (unsigned) a);          // strokes via the sidechain
    auto worstJump = [&] (const Buf& o) { double w = 0; for (size_t i = 4800; i < n; ++i) w = std::max (w, (double) std::abs (o[0][i] - o[0][i - 1])); return w; };
    tg::GateSettings s; s.externalKey = true; s.thresholdDb = -30; s.holdMs = 100; s.releaseMs = 300; s.rangeDb = -40;
    const double plain = worstJump (run (in, s, sr, 0, &key));
    s.spectral.on = true; s.spectral.focus = 100;
    const double spec = worstJump (run (in, s, sr, 0, &key));
    std::printf ("  worst sample-to-sample step: plain gate %.4f, spectral (Focus 100) %.4f\n", plain, spec);
    CHECK (spec <= plain * 1.1 + 1e-4, "spectral step %.4f vs plain %.4f", spec, plain);
}

static void testSourceKey()
{
    std::printf ("Source key: a cymbal can't open it (it does open the plain key); toms pass from their first sample\n");
    const double sr = 48000;
    const size_t n = (size_t) (28 * sr);
    std::vector<float> mix (n, 0.0f);
    std::vector<size_t> toms, crashes;
    for (int k = 0; k < 16; ++k) { const size_t a = (size_t) ((0.5 + 1.0 * k) * sr); sim::addTom (mix, a, 0.8f, 95, sr, (unsigned) k); toms.push_back (a); }
    for (int k = 0; k < 8; ++k) { const size_t a = (size_t) ((17.5 + 1.25 * k) * sr); sim::addCrash (mix, a, 0.5f, sr, (unsigned) (40 + k)); crashes.push_back (a); }   // on their own, after the toms
    Buf in = makeBuf (1, n); in[0] = mix;
    tg::GateSettings s; s.thresholdDb = -30; s.hysteresisDb = 6; s.holdMs = 20; s.releaseMs = 150; s.lookaheadMs = 15;
    auto passedDb = [&] (const Buf& o, size_t at, size_t L, double fromMs, double toMs) {   // out vs in energy over a window after an event
        double ei = 0, eo = 0;
        for (size_t i = at + (size_t) (fromMs * sr / 1000); i < at + (size_t) (toMs * sr / 1000); ++i) { ei += (double) in[0][i] * in[0][i]; eo += (double) o[0][i + L] * o[0][i + L]; }
        return 10 * std::log10 ((eo + 1e-30) / (ei + 1e-30));
    };
    tg::GateEngine eP; Buf plain = run (in, s, sr, 256, nullptr, 1, &eP);
    s.sourceKey = true;
    tg::GateEngine eS; Buf src = run (in, s, sr, 256, nullptr, 1, &eS);
    const size_t L = (size_t) eS.currentLatency();
    int crashPlain = 0, crashSrc = 0; double worstTom = 0.0;
    for (auto a : crashes) { crashPlain += passedDb (plain, a, L, 0, 30) > -20; crashSrc += passedDb (src, a, L, 0, 30) > -20; }
    for (size_t k = 6; k < toms.size(); ++k) worstTom = std::min (worstTom, passedDb (src, toms[k], L, 0, 10));
    std::printf ("  learned fundamental %.0f Hz (the tom: 95 Hz); crashes getting through: plain key %d of %zu, Source key %d; toms' first 10 ms at worst %+.2f dB vs the input\n",
                 eS.sourceKeyHz(), crashPlain, crashes.size(), crashSrc, worstTom);
    CHECK (std::abs (eS.sourceKeyHz() / 95.0 - 1.0) < 0.06, "fundamental %.0f Hz", eS.sourceKeyHz());
    CHECK (crashPlain >= 6, "the test crash should open the plain key (%d)", crashPlain);
    CHECK (crashSrc == 0, "%d crashes opened the Source key", crashSrc);
    CHECK (worstTom > -0.5, "a tom's attack lost %.2f dB", worstTom);
}

static void testLearn()
{
    std::printf ("Learn: taught on toms + soft snare bleed, it takes back loud snare bleed that opens the Source key; toms untouched\n");
    const double sr = 48000;
    const size_t n = (size_t) (60 * sr);
    std::vector<float> mix (n, 0.0f);
    std::vector<size_t> toms, snares;
    for (int k = 0; k < 11; ++k)   // learn passage (from 5 s): toms with soft snare bleed between them
    {
        const size_t a = (size_t) ((0.5 + 1.5 * k) * sr);
        sim::addTom (mix, a, 0.8f, 95, sr, (unsigned) k);
        for (int j = 0; j < 2; ++j) sim::addSnare (mix, a + (size_t) ((0.75 + 0.35 * j) * sr), 0.3f, sr, (unsigned) (100 + 2 * k + j));
    }
    for (int k = 0; k < 20; ++k)   // then: toms, and snare bleed loud enough to open the Source key on its own
    {
        const size_t a = (size_t) ((18.5 + 2.0 * k) * sr);
        sim::addTom (mix, a, (k % 3) == 0 ? 0.4f : 0.8f, 95, sr, (unsigned) (200 + k)); toms.push_back (a);
        const size_t b = a + (size_t) (1.2 * sr);
        sim::addSnare (mix, b, 3.0f, sr, (unsigned) (300 + k)); snares.push_back (b);
    }
    tg::GateSettings s; s.thresholdDb = -30; s.hysteresisDb = 6; s.holdMs = 20; s.releaseMs = 150; s.lookaheadMs = 15; s.sourceKey = true;
    auto go = [&] (bool teach, tg::GateEngine& e, bool watch)
    {
        Buf out = makeBuf (1, n); out[0] = mix;
        e.prepare (sr, 1); e.setSettings (s); e.reset();
        if (watch) { gAllocCount = 0; gWatchAllocs = true; }
        for (size_t pos = 0; pos < n; pos += 256)
        {
            const int bs = (int) std::min<size_t> (256, n - pos);
            float* io[1] = { out[0].data() + pos };
            tg::GateSettings b = s; b.learn = teach && pos >= (size_t) (5 * sr) && pos < (size_t) (16.5 * sr);
            e.setSettings (b);
            e.process (io, 1, nullptr, 0, bs);
            tg::MeterFrame f; while (e.meters.pop (f)) {}
            tg::Hit h; while (e.hits.pop (h)) {}
        }
        gWatchAllocs = false;
        return out;
    };
    auto passedDb = [&] (const Buf& o, size_t at, size_t L, double fromMs, double toMs) {
        double ei = 0, eo = 0;
        for (size_t i = at + (size_t) (fromMs * sr / 1000); i < at + (size_t) (toMs * sr / 1000); ++i) { ei += (double) mix[i] * mix[i]; eo += (double) o[0][i + L] * o[0][i + L]; }
        return 10 * std::log10 ((eo + 1e-30) / (ei + 1e-30));
    };
    tg::GateEngine e0, e1;
    Buf plain = go (false, e0, false);
    Buf taught = go (true, e1, true);
    const long allocs = gAllocCount;
    const size_t L = (size_t) e1.currentLatency();
    int snPlain = 0, snTaught = 0, tomsLost = 0; double worstTom = 0.0;
    // count only snares the gate had to OPEN for (some arrive while a loud tom's tail still holds it open)
    int snFresh = 0;
    for (auto b : snares)
    {
        if (passedDb (plain, b - (size_t) (0.015 * sr), L, 0, 10) > -20) continue;
        ++snFresh; snPlain += passedDb (plain, b, L, 0, 30) > -20; snTaught += passedDb (taught, b, L, 0, 30) > -20;
    }
    for (auto a : toms) { const double d = passedDb (taught, a, L, 0, 10); worstTom = std::min (worstTom, d); tomsLost += d < -3; }
    std::printf ("  learned from %d hits (%d seen) / %d bleed events; loud snares getting through: Source key %d of %zu, with Learn %d (taken back %d); "
                 "toms lost %d of %zu, worst first 10 ms %+.2f dB; allocations %ld\n",
                 e1.uiProfileHits.load(), e1.uiLearnHits.load(), e1.uiLearnBleeds.load(), snPlain, (size_t) snFresh, snTaught, e1.uiVetoes.load(), tomsLost, toms.size(), worstTom, allocs);
    CHECK (e1.uiProfileHits.load() >= 6, "learned from only %d hits", e1.uiProfileHits.load());
    CHECK (snFresh >= 10 && snPlain == snFresh, "the test snare should open the Source key (%d of %d)", snPlain, snFresh);
    CHECK (snTaught == 0, "%d loud snares still got through with Learn", snTaught);
    CHECK (tomsLost == 0 && worstTom > -0.5, "a tom was hurt: %d lost, worst %+.2f dB", tomsLost, worstTom);
    CHECK (allocs == 0, "%ld allocations on the audio thread", allocs);
}

static void testDeBleed()
{
    std::printf ("De-bleed: crash wash comes out of the tom tails; attacks and the body untouched; 0 %% is bit-identical\n");
    const double sr = 48000;
    auto ses = sim::tomMicSession (sr, 30, 110.0, -12.0);
    Buf in = makeBuf (1, ses.mix.size()); in[0] = ses.mix;
    tg::GateSettings s = tomGate(); s.lookaheadMs = 15;
    tg::GateEngine e0; Buf off = run (in, s, sr, 256, nullptr, 1, &e0);
    const size_t L = (size_t) e0.currentLatency();
    s.deBleed = 0.0f;
    tg::GateEngine eZ; Buf zero = run (in, s, sr, 333, nullptr, 2, &eZ);
    bool same = true;
    for (size_t i = 0; i < off[0].size(); ++i) same = same && off[0][i] == zero[0][i];
    CHECK (same, "De-bleed 0 %% isn't bit-identical to the plain gate");
    std::vector<size_t> plain;   // tom hits with no crash on them (and none still ringing from a crash before)
    for (size_t k = 0; k < ses.toms.size(); ++k)
        if (std::find (ses.crashedToms.begin(), ses.crashedToms.end(), ses.toms[k]) == ses.crashedToms.end()) plain.push_back (ses.toms[k]);
    for (float amt : { 50.0f, 100.0f })
    {
        s.deBleed = amt;
        tg::GateEngine e; Buf o = run (in, s, sr, 256, nullptr, 3, &e, amt == 100.0f);
        const long allocs = gAllocCount;
        const double wash = bandEnergyDb (o[0], ses.crashedToms, L, 4000, 14000, 60, 500, sr) - bandEnergyDb (off[0], ses.crashedToms, L, 4000, 14000, 60, 500, sr);
        const double body = bandEnergyDb (o[0], ses.toms, L, 60, 400, 20, 500, sr) - bandEnergyDb (off[0], ses.toms, L, 60, 400, 20, 500, sr);
        double worstAtt = 0.0;
        for (double lo : { 60.0, 2000.0, 6000.0 })
        {
            const double hi = lo == 60.0 ? 400.0 : lo * 2.5;
            worstAtt = std::max (worstAtt, std::abs (bandEnergyDb (o[0], ses.toms, L, lo, hi, 0, 8, sr) - bandEnergyDb (off[0], ses.toms, L, lo, hi, 0, 8, sr)));
        }
        std::printf ("  %3.0f %%: crash wash in the tails %+.1f dB, body %+.2f dB, attacks (first 8 ms, 3 bands) worst %.3f dB%s\n",
                     amt, wash, body, worstAtt, amt == 100.0f ? (" , allocations " + std::to_string (allocs)).c_str() : "");
        CHECK (wash < (amt == 100.0f ? -12.0 : -6.0), "%.0f %%: crash wash only %+.1f dB", amt, wash);
        CHECK (std::abs (body) < 0.2, "%.0f %%: the body moved %+.2f dB", amt, body);
        CHECK (worstAtt < 0.1, "%.0f %%: an attack changed %.3f dB", amt, worstAtt);
        if (amt == 100.0f) CHECK (allocs == 0, "%ld allocations", allocs);
    }
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    testRangeZeroIsTransparent();
    testOpenIsBitIdentical();
    testLatencyAndTransientIntact();
    testClickFree();
    testLookaheadChangeIsClickFree();
    testBlockSizeIndependence();
    testNoAllocations();
    testHysteresis();
    testHoldReleaseRange();
    testDetectionHpf();
    testTrackingHighCut();
    testHitsAndExternalKey();
    testBypassKeepsLatency();
    testListenAndStability();
    testLevelerFixedTarget();
    testLevelerAuto();
    testLevelerTransientUntouched();
    testLevelerRolls();
    testLevelerNoLookahead();
    testLevelerLeavesBleedAlone();
    testLevelerClickFreeAndOff();
    testGlowTransparentBelowCurve();
    testGlowDriveAndAutoGain();
    testIronLowsOnly();
    testGlowEvenHarmonics();
    testGlowAliasing();
    testGlowEnvelope();
    testGlowSwitchingClickFree();
    testSpectralNeutral();
    testSpectralLearns();
    testSpectralCleansTail();
    testSpectralSustain();
    testSpectralTransientKnob();
    testSpectralRestRampClean();
    testSourceKey();
    testLearn();
    testDeBleed();
    std::printf ("\n%s (%d failures)\n", gFailures == 0 ? "ALL PASSED" : "FAILED", gFailures);
    return gFailures == 0 ? 0 : 1;
}
