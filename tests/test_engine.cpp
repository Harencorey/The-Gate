//==============================================================================
// The Gate - offline engine tests (no JUCE, no DAW).
//
// Build & run:  cmake --build build --target GateTests && ./build/GateTests
//==============================================================================

#include "GateCore.h"

#include <atomic>
#include <cmath>
#include <cstdio>
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
            const int L = e.lookaheadSamples (laMs);
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
    const int L = (int) std::lround (0.005 * sr);
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
            CHECK (L == e.lookaheadSamples (laMs), "latency mismatch");
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
    CHECK (e.currentLatency() == e.lookaheadSamples (2.0f), "lookahead didn't switch");
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
        CHECK (hs[i].timeSamples == hs[i].arrivalSample + 240, "hit %d output time not shifted by the lookahead", (int) i);
    }
    if (hs.size() == 3) CHECK (hs[0].peakDb > hs[1].peakDb && hs[1].peakDb > hs[2].peakDb, "hit levels out of order");
    // audio between hits is gated shut, audio during a hit passes
    CHECK (std::abs (out[0][2000]) < 1e-6f, "gate open before the first key hit");
    double during = 0; for (size_t i = 4800 + 240; i < 4800 + 2400; ++i) during = std::max (during, (double) std::abs (out[0][i]));
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
    CHECK (x[100 + 480] == 1.0f, "bypassed impulse not delayed by the lookahead");
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
    std::printf ("\n%s (%d failures)\n", gFailures == 0 ? "ALL PASSED" : "FAILED", gFailures);
    return gFailures == 0 ? 0 : 1;
}
