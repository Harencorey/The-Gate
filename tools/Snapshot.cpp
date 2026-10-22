//==============================================================================
// Renders the real editor to a PNG after feeding it a simulated tom track with
// snare bleed, so UI changes can be reviewed as screenshots.
//   GateSnapshot out.png
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <random>

using namespace juce;

static void addHit (AudioBuffer<float>& b, int at, float amp, float hz, double sr, float decayMs, unsigned seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    const int len = (int) (decayMs * 0.001 * sr * 6);
    for (int i = 0; i < len && at + i < b.getNumSamples(); ++i)
    {
        const double t = i / sr;
        const double env = std::exp (-t / (decayMs * 0.001));
        const double click = i < (int) (0.002 * sr) ? u (rng) * std::exp (-t / 0.0005) : 0.0;
        const float v = (float) (amp * (env * std::sin (2 * MathConstants<double>::pi * hz * t) + 0.5 * click));
        for (int c = 0; c < b.getNumChannels(); ++c) b.addSample (c, at + i, v);
    }
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    const double sr = 48000;
    TheGateProcessor p;
    auto set = [&] (const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); };
    set (tg::pid::threshold, -26.0f);
    set (tg::pid::hysteresis, 8.0f);
    set (tg::pid::release, 220.0f);
    set (tg::pid::keyHpf, 90.0f);
    set (tg::pid::keyHighCut, 9000.0f);
    set (tg::pid::tracking, 60.0f);
    p.prepareToPlay (sr, 256);

    // 3 s: tom hits (loud and soft) with quieter snare bleed hits in between, plus a noise floor
    const int n = (int) (sr * 3.2);
    AudioBuffer<float> sig (2, n);
    sig.clear();
    std::mt19937 rng (4);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    for (int i = 0; i < n; ++i) { const float v = 0.004f * u (rng); sig.setSample (0, i, v); sig.setSample (1, i, v); }
    const int toms[] = { 6000, 30000, 52000, 75000, 96000, 120000, 138000 };
    const float tAmp[] = { 0.8f, 0.5f, 0.85f, 0.3f, 0.7f, 0.9f, 0.45f };
    for (int k = 0; k < 7; ++k) addHit (sig, toms[k], tAmp[k], 110.0f, sr, 90.0f, (unsigned) k);
    for (int at = 18000; at < n; at += 24000) addHit (sig, at, 0.03f, 220.0f, sr, 40.0f, (unsigned) at);

    std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
    ed->setVisible (true);
    MidiBuffer midi;
    AudioBuffer<float> blk (4, 256);
    for (int pos = 0; pos + 256 <= n; pos += 256)
    {
        blk.clear();
        for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, sig, c, pos, 256);
        p.processBlock (blk, midi);
        // let the editor drain the meter FIFO as its timer would
        if (pos % (256 * 8) == 0)
            for (auto* c : ed->getChildren())
                if (auto* gd = dynamic_cast<GateDisplay*> (c)) gd->pull();
    }
    for (auto* c : ed->getChildren())
        if (auto* gd = dynamic_cast<GateDisplay*> (c)) gd->pull();

    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    File out (argc > 1 ? String (argv[1]) : String ("snapshot.png"));
    out.deleteFile();
    FileOutputStream os (out);
    PNGImageFormat().writeImageToStream (img, os);
    std::printf ("wrote %s\n", out.getFullPathName().toRawUTF8());
    return 0;
}
