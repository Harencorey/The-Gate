//==============================================================================
// The Gate - plug-in level tests on the real processor (bus layouts, sidechain
// routing, reported latency, bypass, session save/load).
//==============================================================================

#include "PluginProcessor.h"
#include <cstdio>

static int failures = 0;
#define CHECK(cond, ...) \
    do { if (! (cond)) { ++failures; std::printf ("  FAIL: " __VA_ARGS__); std::printf ("\n"); } } while (0)

using namespace juce;

static void setParam (TheGateProcessor& p, const char* id, float value)
{
    auto* prm = p.apvts.getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static bool setLayout (TheGateProcessor& p, AudioChannelSet main, AudioChannelSet sc)
{
    AudioProcessor::BusesLayout l;
    l.inputBuses.add (main);
    l.inputBuses.add (sc);
    l.outputBuses.add (main);
    return p.setBusesLayout (l);
}

static void testLayouts()
{
    std::printf ("Bus layouts: mono and stereo, sidechain off / mono / stereo\n");
    TheGateProcessor p;
    for (auto main : { AudioChannelSet::mono(), AudioChannelSet::stereo() })
        for (auto sc : { AudioChannelSet::disabled(), AudioChannelSet::mono(), AudioChannelSet::stereo() })
            CHECK (setLayout (p, main, sc), "layout %s / %s refused", main.getDescription().toRawUTF8(), sc.getDescription().toRawUTF8());
    AudioProcessor::BusesLayout bad;
    bad.inputBuses.add (AudioChannelSet::mono());
    bad.inputBuses.add (AudioChannelSet::disabled());
    bad.outputBuses.add (AudioChannelSet::stereo());
    CHECK (! p.checkBusesLayoutSupported (bad), "mono in / stereo out should be refused");
}

static void testLatencyAndSidechain()
{
    std::printf ("Reported latency follows Lookahead; the sidechain bus keys the gate\n");
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        TheGateProcessor p;
        setLayout (p, AudioChannelSet::stereo(), AudioChannelSet::stereo());
        setParam (p, tg::pid::lookahead, 3.0f);   // 10 ms
        p.prepareToPlay (sr, 256);
        const int L = (int) std::lround (0.010 * sr) + (sr < 60000 ? 48 : 46);   // lookahead + Micro Glow's filters
        CHECK (p.getLatencySamples() == L, "sr %.0f latency %d, expected %d", sr, p.getLatencySamples(), L);

        // main = steady tone, sidechain = silence then a burst
        setParam (p, tg::pid::keySource, 1.0f);
        setParam (p, tg::pid::threshold, -40.0f);
        AudioBuffer<float> buf (4, 256);
        MidiBuffer midi;
        double before = 0, during = 0;
        for (int blk = 0; blk < 200; ++blk)
        {
            for (int i = 0; i < 256; ++i)
            {
                const double t = (blk * 256 + i) / sr;
                const float tone = 0.25f * (float) std::sin (2 * MathConstants<double>::pi * 100 * t);
                buf.setSample (0, i, tone); buf.setSample (1, i, tone);
                const float key = (blk >= 100 && blk < 110) ? 0.5f : 0.0f;
                buf.setSample (2, i, key); buf.setSample (3, i, key);
            }
            p.processBlock (buf, midi);
            for (int i = 0; i < 256; ++i)
            {
                const double v = std::abs (buf.getSample (0, i));
                if (blk > 20 && blk < 95) before = std::max (before, v);
                if (blk >= 102 && blk < 108) during = std::max (during, v);
            }
        }
        CHECK (before < 1e-6, "gate open without the sidechain key (%.4f)", before);
        CHECK (during > 0.24, "sidechain key didn't open the gate (%.4f)", during);
        CHECK (p.sidechainConnected(), "sidechain not seen as connected");
    }
}

static void testBypassLatency()
{
    std::printf ("Bypass keeps the latency\n");
    TheGateProcessor p;
    setLayout (p, AudioChannelSet::mono(), AudioChannelSet::disabled());
    p.prepareToPlay (48000, 512);
    const int L = p.getLatencySamples();
    AudioBuffer<float> buf (1, 512);
    buf.clear();
    buf.setSample (0, 10, 1.0f);
    MidiBuffer midi;
    p.processBlockBypassed (buf, midi);
    CHECK (L == 240 + 48 && buf.getSample (0, 10 + L) == 1.0f, "bypassed impulse not delayed by %d", L);
}

static void testStateRoundTrip()
{
    std::printf ("Session save/load restores every parameter\n");
    TheGateProcessor a;
    setParam (a, tg::pid::threshold, -42.5f);
    setParam (a, tg::pid::attack, 3.0f);
    setParam (a, tg::pid::range, -25.0f);
    setParam (a, tg::pid::lookahead, 4.0f);
    setParam (a, tg::pid::keyHpf, 220.0f);
    setParam (a, tg::pid::tracking, 65.0f);
    setParam (a, tg::pid::listen, 1.0f);
    setParam (a, tg::pid::levelerOn, 1.0f);
    setParam (a, tg::pid::levelAmount, 40.0f);
    setParam (a, tg::pid::levelTarget, -18.0f);
    setParam (a, tg::pid::levelMax, 9.0f);
    setParam (a, tg::pid::glowOn, 1.0f);
    setParam (a, tg::pid::glowType, 1.0f);
    setParam (a, tg::pid::glowDrive, 63.0f);
    setParam (a, tg::pid::glowEnv, -40.0f);
    setParam (a, tg::pid::specOn, 1.0f);
    setParam (a, tg::pid::specFocus, 85.0f);
    setParam (a, tg::pid::specSustain, 35.0f);
    setParam (a, tg::pid::specTransient, -30.0f);
    MemoryBlock mb;
    a.getStateInformation (mb);
    TheGateProcessor b;
    b.setStateInformation (mb.getData(), (int) mb.getSize());
    for (auto* prm : a.getParameters())
    {
        auto* ra = dynamic_cast<RangedAudioParameter*> (prm);
        auto* rb = dynamic_cast<RangedAudioParameter*> (b.apvts.getParameter (ra->getParameterID()));
        CHECK (rb != nullptr && std::abs (ra->getValue() - rb->getValue()) < 1e-6f, "%s not restored", ra->getParameterID().toRawUTF8());
    }
}

static void testPhase2Params()
{
    std::printf ("Leveler and Micro Glow controls reach the engine; latency doesn't move when Micro Glow toggles\n");
    TheGateProcessor p;
    setLayout (p, AudioChannelSet::mono(), AudioChannelSet::disabled());
    setParam (p, tg::pid::range, 0.0f);   // gate fully open
    setParam (p, tg::pid::glowOn, 1.0f);
    setParam (p, tg::pid::glowType, 0.0f);
    setParam (p, tg::pid::glowDrive, 80.0f);
    p.prepareToPlay (48000, 256);
    const int L0 = p.getLatencySamples();
    AudioBuffer<float> buf (1, 256);
    MidiBuffer midi;
    double pin = 0, pout = 0, h3 = 0;
    for (int blk = 0; blk < 800; ++blk)
    {
        for (int i = 0; i < 256; ++i) buf.setSample (0, i, 0.5f * (float) std::sin (2 * MathConstants<double>::pi * 93.75 * (blk * 256 + i) / 48000.0));
        if (blk > 400) pin += buf.getRMSLevel (0, 0, 256);
        p.processBlock (buf, midi);
        if (blk > 400) pout += buf.getRMSLevel (0, 0, 256);
        if (blk == 799)   // 3rd harmonic of the last block (256 samples = 1/2 cycle of 93.75 Hz... use correlation over the block)
        {
            double re = 0, im = 0;
            for (int i = 0; i < 256; ++i) { const double w = 2 * MathConstants<double>::pi * 281.25 * i / 48000.0; re += buf.getSample (0, i) * std::cos (w); im += buf.getSample (0, i) * std::sin (w); }
            h3 = std::sqrt (re * re + im * im) / 128.0;
        }
    }
    const double lv = Decibels::gainToDecibels (pout / pin);
    std::printf ("  Micro Glow TUBE Drive 80 on a -6 dBFS tone: level %+.2f dB, 3rd harmonic %.1f dBFS\n", lv, Decibels::gainToDecibels (h3));
    CHECK (std::abs (lv) < 0.2, "Micro Glow not loudness-matched (%+.2f dB)", lv);
    CHECK (h3 > 0.03, "Micro Glow Drive didn't saturate (3rd harmonic %.4f)", h3);
    setParam (p, tg::pid::glowOn, 0.0f);
    p.processBlock (buf, midi);
    CHECK (p.getLatencySamples() == L0, "latency changed when Micro Glow switched off");

    // Target knob bottom = Auto
    auto* t = p.apvts.getParameter (tg::pid::levelTarget);
    t->setValueNotifyingHost (0.0f);
    CHECK (t->getCurrentValueAsText() == "Auto", "Target bottom shows '%s'", t->getCurrentValueAsText().toRawUTF8());
}

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);   // so a crash still shows which test was running
    ScopedJuceInitialiser_GUI init;
    testLayouts();
    testLatencyAndSidechain();
    testBypassLatency();
    testStateRoundTrip();
    testPhase2Params();
    std::printf ("\n%s (%d failures)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
