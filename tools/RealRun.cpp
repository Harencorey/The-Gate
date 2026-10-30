//==============================================================================
// Runs the exact plug-in engine (GateCore.h) over a real track, offline.
//   RealRun in.f32 out.f32 hits.csv sr key=value...
// in/out: raw mono float32. hits.csv: arrivalSample,timeSamples,peakDb per detected hit.
// Settings: thr hyst att hold rel range la hpf hc track spec focus sustain lev (see GateSettings).
// Output is aligned with the input (the reported latency is removed).
//==============================================================================
#include "GateCore.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 5) { std::fprintf (stderr, "usage: RealRun in.f32 out.f32 hits.csv sr [key=value...]\n"); return 1; }
    const double sr = std::atof (argv[4]);
    tg::GateSettings s;
    s.rangeDb = -80.0f;
    double learnFrom = -1.0, learnTo = -1.0;   // learn=a-b: Learn armed from a to b seconds
    std::string gainOut;                        // gainout=path: the leveler gain per sample (float32), aligned with the output   // learn=a-b: Learn armed from a to b seconds
    for (int a = 5; a < argc; ++a)
    {
        std::string kv = argv[a];
        auto eq = kv.find ('=');
        if (eq == std::string::npos) continue;
        const std::string k = kv.substr (0, eq);
        const float v = (float) std::atof (kv.c_str() + eq + 1);
        if (k == "gainout") { gainOut = kv.substr (eq + 1); continue; }
        if (k == "learn") { std::sscanf (kv.c_str() + eq + 1, "%lf-%lf", &learnFrom, &learnTo); continue; }
        if      (k == "thr")     s.thresholdDb = v;
        else if (k == "hyst")    s.hysteresisDb = v;
        else if (k == "att")     s.attackMs = v;
        else if (k == "hold")    s.holdMs = v;
        else if (k == "rel")     s.releaseMs = v;
        else if (k == "range")   s.rangeDb = v;
        else if (k == "la")      s.lookaheadMs = v;
        else if (k == "hpf")     s.keyHpfHz = v;
        else if (k == "hc")      s.keyHighCutHz = v;
        else if (k == "track")   s.trackingPct = v;
        else if (k == "spec")    s.spectral.on = v > 0.5f;
        else if (k == "focus")   s.spectral.focus = v;
        else if (k == "sustain") s.spectral.sustain = v;
        else if (k == "transient") s.spectral.transient = v;
        else if (k == "lev")     s.levelerOn = v > 0.5f;
        else if (k == "src")     s.sourceKey = v > 0.5f;
        else if (k == "srcatk")  s.sourceAttackDb = v;
        else if (k == "strict")  s.strictness = v;
        else if (k == "lamt")    s.levelAmount = v;
        else if (k == "lmax")    s.levelMaxDb = v;
        else if (k == "ltgt")    s.levelTarget = v;
        else if (k == "glow")    s.glow.on = v > 0.5f;
        else if (k == "gdrive")  s.glow.drive = v;
        else if (k == "genv")    s.glow.envAmount = v;
        else { std::fprintf (stderr, "unknown setting %s\n", k.c_str()); return 1; }
    }
    FILE* f = std::fopen (argv[1], "rb");
    if (! f) { std::perror (argv[1]); return 1; }
    std::fseek (f, 0, SEEK_END);
    const long n = std::ftell (f) / (long) sizeof (float);
    std::fseek (f, 0, SEEK_SET);
    std::vector<float> x ((size_t) n);
    if (std::fread (x.data(), sizeof (float), (size_t) n, f) != (size_t) n) return 1;
    std::fclose (f);

    auto eng = std::make_unique<tg::GateEngine>();
    eng->prepare (sr, 1);
    eng->setSettings (s);
    eng->reset();
    const int L = eng->currentLatency();
    const int B = gainOut.empty() ? 256 : 1;
    std::vector<float> lg;
    if (! gainOut.empty()) lg.reserve ((size_t) n + (size_t) L);
    std::vector<float> y ((size_t) n + (size_t) L + B, 0.0f), buf (B);
    FILE* hf = std::fopen (argv[3], "w");
    std::fprintf (hf, "arrival,time,peakDb,audioPeakDb,levelChangeDb\n");
    for (long p = 0; p < n + L; p += B)
    {
        const int len = (int) std::min<long> (B, n + L - p);
        for (int i = 0; i < len; ++i) buf[(size_t) i] = p + i < n ? x[(size_t) (p + i)] : 0.0f;
        float* io[1] = { buf.data() };
        s.learn = learnFrom >= 0.0 && p >= (long) (learnFrom * sr) && p < (long) (learnTo * sr);
        eng->setSettings (s);
        eng->process (io, 1, nullptr, 0, len);
        std::memcpy (y.data() + p, buf.data(), sizeof (float) * (size_t) len);
        if (! gainOut.empty()) lg.push_back ((float) eng->currentLevelerGain());
        tg::Hit h;
        while (eng->hits.pop (h)) std::fprintf (hf, "%llu,%llu,%.2f,%.2f,%.2f\n", (unsigned long long) h.arrivalSample, (unsigned long long) h.timeSamples, h.peakDb, h.audioPeakDb, h.levelChangeDb);
        tg::MeterFrame mf;
        while (eng->meters.pop (mf)) {}
    }
    std::fclose (hf);
    if (! gainOut.empty())
    {
        FILE* gf = std::fopen (gainOut.c_str(), "wb");
        std::fwrite (lg.data() + L, sizeof (float), (size_t) n, gf);
        std::fclose (gf);
    }
    FILE* of = std::fopen (argv[2], "wb");
    std::fwrite (y.data() + L, sizeof (float), (size_t) n, of);
    std::fclose (of);
    std::fprintf (stderr, "latency %d samples, %ld samples processed, source key at %.1f Hz\n", L, n, eng->sourceKeyHz());
    if (learnFrom >= 0.0)
        std::fprintf (stderr, "learn: profile from %d hits (%s), %d bleed events seen; %d openings taken back\n",
                      eng->uiProfileHits.load(), eng->uiLearnFailed.load() ? "FAILED: not enough" : "ok", eng->uiLearnBleeds.load(), eng->uiVetoes.load());
    auto& sd = eng->spectralDecay();
    std::fprintf (stderr, "spectral: %d hits learned, %d bleed captures, body band to %.0f Hz, attack %.1f ms\n", sd.hitsLearned(), sd.bleedsLearned(), sd.bodyTopHz(), sd.transientMs());
    if (s.spectral.on)
    {
        std::fprintf (stderr, "spectrum (Hz: source dB / bleed dB):");
        for (double hz : { 50.0, 63.0, 80.0, 100.0, 125.0, 160.0, 200.0, 250.0, 315.0, 400.0, 630.0, 1000.0, 2000.0, 4000.0, 8000.0 })
            std::fprintf (stderr, " %.0f: %.0f/%.0f", hz, sd.sourceDbAt (hz), sd.bleedDbAt (hz));
        std::fprintf (stderr, "\n");
    }
    return 0;
}
