#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

//==============================================================================
static NormalisableRange<float> skewed (float lo, float hi, float centre)
{
    NormalisableRange<float> r (lo, hi);
    r.setSkewForCentre (centre);
    return r;
}

AudioProcessorValueTreeState::ParameterLayout TheGateProcessor::createLayout()
{
    using P = AudioParameterFloat;
    using A = AudioParameterFloatAttributes;
    std::vector<std::unique_ptr<RangedAudioParameter>> ps;

    auto dB  = A().withLabel ("dB").withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; });
    auto ms  = A().withLabel ("ms").withStringFromValueFunction ([] (float v, int)
                   { return v < 10.0f ? String (v, 2) + " ms" : v < 100.0f ? String (v, 1) + " ms" : String (roundToInt (v)) + " ms"; });
    auto hz  = [] (float off, bool offIsLow)
    {
        return A().withLabel ("Hz").withStringFromValueFunction ([off, offIsLow] (float v, int)
        {
            if (offIsLow ? v <= off : v >= off) return String ("Off");
            return v >= 1000.0f ? String (v / 1000.0f, 1) + " kHz" : String (roundToInt (v)) + " Hz";
        });
    };

    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::threshold, 1 },  "Threshold",  NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -30.0f, dB));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::hysteresis, 1 }, "Hysteresis", NormalisableRange<float> (0.0f, 20.0f, 0.1f), 6.0f, dB));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::attack, 1 },     "Attack",     skewed (0.01f, 50.0f, 2.0f), 0.5f, ms));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::hold, 1 },       "Hold",       skewed (0.0f, 500.0f, 50.0f), 20.0f, ms));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::release, 1 },    "Release",    skewed (5.0f, 2000.0f, 150.0f), 150.0f, ms));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::range, 1 },      "Range",      NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -80.0f,
                                       A().withLabel ("dB").withStringFromValueFunction ([] (float v, int)
                                           { return v <= -80.0f ? String (CharPointer_UTF8 ("-\xe2\x88\x9e dB")) : String (v, 1) + " dB"; })));
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { tg::pid::lookahead, 1 }, "Lookahead",
                                       StringArray { "Off", "2 ms", "5 ms", "10 ms", "20 ms" }, tg::kLookaheadDefault));
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { tg::pid::keySource, 1 }, "Key",
                                       StringArray { "Internal", "Sidechain" }, 0));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::keyHpf, 1 },     "Key HPF",      skewed (tg::kHpfOffHz, 2000.0f, 150.0f), tg::kHpfOffHz, hz (tg::kHpfOffHz, true)));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::keyHighCut, 1 }, "Key High Cut", skewed (1000.0f, tg::kHighCutOffHz, 6000.0f), tg::kHighCutOffHz, hz (tg::kHighCutOffHz, false)));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::tracking, 1 },   "Tracking",     NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
                                       A().withLabel ("%").withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; })));
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { tg::pid::listen, 1 }, "Listen", false));

    // Leveler
    auto pct = A().withLabel ("%").withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; });
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { tg::pid::levelerOn, 1 }, "Leveler", false));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::levelAmount, 1 }, "Level Amount", NormalisableRange<float> (0.0f, 100.0f, 0.1f), 75.0f, pct));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::levelTarget, 1 }, "Level Target", NormalisableRange<float> (tg::kTargetAutoDb, 0.0f, 0.1f), tg::kTargetAutoDb,
                                       A().withLabel ("dB").withStringFromValueFunction ([] (float v, int)
                                           { return v <= tg::kTargetAutoDb + 0.05f ? String ("Auto") : String (v, 1) + " dB"; })));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::levelMax, 1 }, "Level Max", NormalisableRange<float> (0.0f, 24.0f, 0.1f), 12.0f,
                                       A().withLabel ("dB").withStringFromValueFunction ([] (float v, int) { return String (CharPointer_UTF8 ("\xc2\xb1")) + String (v, 1) + " dB"; })));

    // Micro Glow
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { tg::pid::glowOn, 1 }, "Micro Glow", false));
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { tg::pid::glowType, 1 }, "Glow Type", StringArray { "Tube", "Diode", "Clip" }, 0));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::glowDrive, 1 }, "Glow Drive", NormalisableRange<float> (0.0f, 24.0f, 0.1f), 0.0f,
                                       A().withLabel ("dB").withStringFromValueFunction ([] (float v, int) { return "+" + String (v, 1) + " dB"; })));
    ps.push_back (std::make_unique<P> (ParameterID { tg::pid::glowCeiling, 1 }, "Glow Ceiling", NormalisableRange<float> (-24.0f, 0.0f, 0.1f), 0.0f, dB));
    return { ps.begin(), ps.end() };
}

//==============================================================================
TheGateProcessor::TheGateProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "TheGate", createLayout())
{
    raw.threshold  = apvts.getRawParameterValue (tg::pid::threshold);
    raw.hysteresis = apvts.getRawParameterValue (tg::pid::hysteresis);
    raw.attack     = apvts.getRawParameterValue (tg::pid::attack);
    raw.hold       = apvts.getRawParameterValue (tg::pid::hold);
    raw.release    = apvts.getRawParameterValue (tg::pid::release);
    raw.range      = apvts.getRawParameterValue (tg::pid::range);
    raw.lookahead  = apvts.getRawParameterValue (tg::pid::lookahead);
    raw.keySource  = apvts.getRawParameterValue (tg::pid::keySource);
    raw.keyHpf     = apvts.getRawParameterValue (tg::pid::keyHpf);
    raw.keyHighCut = apvts.getRawParameterValue (tg::pid::keyHighCut);
    raw.tracking   = apvts.getRawParameterValue (tg::pid::tracking);
    raw.listen     = apvts.getRawParameterValue (tg::pid::listen);
    raw.levelerOn   = apvts.getRawParameterValue (tg::pid::levelerOn);
    raw.levelAmount = apvts.getRawParameterValue (tg::pid::levelAmount);
    raw.levelTarget = apvts.getRawParameterValue (tg::pid::levelTarget);
    raw.levelMax    = apvts.getRawParameterValue (tg::pid::levelMax);
    raw.glowOn      = apvts.getRawParameterValue (tg::pid::glowOn);
    raw.glowType    = apvts.getRawParameterValue (tg::pid::glowType);
    raw.glowDrive   = apvts.getRawParameterValue (tg::pid::glowDrive);
    raw.glowCeiling = apvts.getRawParameterValue (tg::pid::glowCeiling);

    engine.prepare (48000.0, 2);
    startTimerHz (10);
}

TheGateProcessor::~TheGateProcessor() { stopTimer(); }

bool TheGateProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out != AudioChannelSet::mono() && out != AudioChannelSet::stereo()) return false;
    if (in != out) return false;
    if (layouts.inputBuses.size() > 1)
    {
        const auto sc = layouts.getChannelSet (true, 1);
        if (! sc.isDisabled() && sc != AudioChannelSet::mono() && sc != AudioChannelSet::stereo()) return false;
    }
    return true;
}

tg::GateSettings TheGateProcessor::readSettings() const noexcept
{
    tg::GateSettings s;
    s.thresholdDb  = raw.threshold->load();
    s.hysteresisDb = raw.hysteresis->load();
    s.attackMs     = raw.attack->load();
    s.holdMs       = raw.hold->load();
    s.releaseMs    = raw.release->load();
    s.rangeDb      = raw.range->load();
    s.lookaheadMs  = tg::kLookaheadMs[jlimit (0, 4, (int) raw.lookahead->load())];
    s.externalKey  = raw.keySource->load() > 0.5f;
    const float hpf = raw.keyHpf->load(), hc = raw.keyHighCut->load();
    s.keyHpfHz     = hpf <= tg::kHpfOffHz ? 0.0f : hpf;
    s.keyHighCutHz = hc >= tg::kHighCutOffHz ? 0.0f : hc;
    s.trackingPct  = raw.tracking->load();
    s.listen       = raw.listen->load() > 0.5f;
    s.levelerOn    = raw.levelerOn->load() > 0.5f;
    s.levelAmount  = raw.levelAmount->load();
    const float tgt = raw.levelTarget->load();
    s.levelTarget  = tgt <= tg::kTargetAutoDb + 0.05f ? tg::GateSettings::kAutoTarget : tgt;
    s.levelMaxDb   = raw.levelMax->load();
    s.glow.on        = raw.glowOn->load() > 0.5f;
    s.glow.type      = jlimit (0, 2, (int) raw.glowType->load());
    s.glow.driveDb   = raw.glowDrive->load();
    s.glow.ceilingDb = raw.glowCeiling->load();
    return s;
}

int TheGateProcessor::latencyForCurrentChoice() const noexcept
{
    return engine.latencyFor (tg::kLookaheadMs[jlimit (0, 4, (int) raw.lookahead->load())]);
}

void TheGateProcessor::prepareToPlay (double sampleRate, int)
{
    currentRate = sampleRate;
    engine.prepare (sampleRate, 2);
    engine.setSettings (readSettings());
    engine.reset();
    setLatencySamples (engine.currentLatency());
}

void TheGateProcessor::timerCallback()
{
    // Message thread: report a new latency to the host after the Lookahead choice changes.
    if (currentRate > 0.0)
    {
        const int want = latencyForCurrentChoice();
        if (want != getLatencySamples())
            setLatencySamples (want);
    }
}

void TheGateProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;

    auto main = getBusBuffer (buffer, false, 0);
    const int numCh = main.getNumChannels();
    const int n = buffer.getNumSamples();

    // The sidechain view must outlive engine.process(): its channel-pointer array lives
    // inside this AudioBuffer object, so it can't be a temporary inside the if-block.
    AudioBuffer<float> sc;
    const float* const* scPtrs = nullptr;
    int scCh = 0;
    if (getBusCount (true) > 1)
    {
        auto* scBus = getBus (true, 1);
        if (scBus != nullptr && scBus->isEnabled())
        {
            sc = getBusBuffer (buffer, true, 1);
            scCh = sc.getNumChannels();
            scPtrs = scCh > 0 ? sc.getArrayOfReadPointers() : nullptr;
        }
    }
    sidechainActive.store (scCh > 0);

    engine.setSettings (readSettings());
    engine.process (main.getArrayOfWritePointers(), numCh, scPtrs, scCh, n);

    // Output only uses the main bus channels; silence anything else the host handed us.
    for (int c = numCh; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, n);
}

void TheGateProcessor::processBlockBypassed (AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    auto main = getBusBuffer (buffer, false, 0);
    engine.processBypassed (main.getArrayOfWritePointers(), main.getNumChannels(), buffer.getNumSamples());
    for (int c = main.getNumChannels(); c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, buffer.getNumSamples());
}

//==============================================================================
void TheGateProcessor::getStateInformation (MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("version", JucePlugin_VersionString, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TheGateProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (ValueTree::fromXml (*xml));
}

AudioProcessorEditor* TheGateProcessor::createEditor() { return new TheGateEditor (*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TheGateProcessor(); }
