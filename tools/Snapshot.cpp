//==============================================================================
// Renders the real editor to a PNG after feeding it a simulated tom track with
// snare bleed, so UI changes can be reviewed as screenshots.
//   GateSnapshot out.png
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DrumSim.h"
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
    set (tg::pid::levelerOn, 1.0f);
    set (tg::pid::levelAmount, 80.0f);
    set (tg::pid::glowOn, 1.0f);
    set (tg::pid::glowDrive, 35.0f);
    set (tg::pid::glowEnv, 60.0f);
    set (tg::pid::specOn, 1.0f);
    set (tg::pid::specFocus, 70.0f);
    set (tg::pid::specSustain, 40.0f);
    set (tg::pid::keySource, 2.0f);          // Source key, with Learn
    set (tg::pid::threshold, -20.0f);
    p.prepareToPlay (sr, 256);
    // mode (2nd arg): "learning" = Learn still lit at the end; otherwise learn over the first half, then use it
    const bool stillLearning = argc > 2 && String (argv[2]) == "learning";

    // the simulated tom mic (tests/DrumSim.h): toms with hats, snare and crashes bleeding in
    auto ses = sim::tomMicSession (sr, 40);
    const int n = (int) ses.mix.size();
    AudioBuffer<float> sig (2, n);
    for (int i = 0; i < n; ++i) { sig.setSample (0, i, ses.mix[(size_t) i]); sig.setSample (1, i, ses.mix[(size_t) i]); }

    p.learnArmed.store (stillLearning);
    std::unique_ptr<AudioProcessorEditor> ed (p.createEditor());
    ed->setVisible (true);
    MidiBuffer midi;
    AudioBuffer<float> blk (4, 256);
    const int end = n - (int) (2.4 * sr);   // stop mid-session, so the view shows several hits
    for (int pos = 0; pos + 256 <= end; pos += 256)
    {
        blk.clear();
        for (int c = 0; c < 2; ++c) blk.copyFrom (c, 0, sig, c, pos, 256);
        if (! stillLearning) p.learnArmed.store (pos > (int) (1.0 * sr) && pos < end * 3 / 4);
        p.processBlock (blk, midi);
        // let the editor drain the meter FIFO as its timer would
        if (pos % (256 * 8) == 0)
            for (auto* c : ed->getChildren())
                if (auto* gd = dynamic_cast<GateDisplay*> (c)) gd->pull();
    }
    for (auto* c : ed->getChildren())
    {
        if (auto* gd = dynamic_cast<GateDisplay*> (c)) gd->pull();
        if (auto* sv = dynamic_cast<SpectrumView*> (c)) sv->pull();
    }

    std::fprintf (stderr, "learn: %d hits / %d bleeds seen, profile %d hits, failed %d, vetoes %d\n", p.engine.uiLearnHits.load(), p.engine.uiLearnBleeds.load(),
                  p.engine.uiProfileHits.load(), (int) p.engine.uiLearnFailed.load(), p.engine.uiVetoes.load());
    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
    File out (argc > 1 ? String (argv[1]) : String ("snapshot.png"));
    out.deleteFile();
    FileOutputStream os (out);
    PNGImageFormat().writeImageToStream (img, os);
    std::printf ("wrote %s\n", out.getFullPathName().toRawUTF8());
    return 0;
}
