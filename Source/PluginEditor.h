#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

//==============================================================================
// Scrolling view: input level, key level, threshold lines, gate state and gain.
class GateDisplay : public juce::Component
{
public:
    explicit GateDisplay (TheGateProcessor& p) : proc (p) { history.resize (kFrames); }

    void pull();   // UI thread: drain the engine's meter FIFO
    void paint (juce::Graphics&) override;

private:
    static constexpr int kFrames = 1200;   // ~3 s at 48 kHz (128 samples per frame)
    TheGateProcessor& proc;
    std::vector<tg::MeterFrame> history;
    int head = 0;
};

//==============================================================================
// What the spectral decay has learned: the source's spectrum, the bleed's, the body band and the attack length.
class SpectrumView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit SpectrumView (TheGateProcessor& p) : proc (p) {}
    void pull();   // UI thread: newest snapshot from the engine
    void paint (juce::Graphics&) override;
private:
    TheGateProcessor& proc;
    tg::SpectrumSnapshot snap;
    bool have = false;
};

//==============================================================================
// A concentric pot: the main knob in the middle, a second control as a ring around it.
// The ring only takes the mouse on the ring itself, so the two never get in each other's way.
class ConcentricKnob : public juce::Component
{
public:
    ConcentricKnob (juce::AudioProcessorValueTreeState&, const char* knobId, const char* ringId,
                    const juce::String& knobName, const juce::String& ringName,
                    const juce::String& knobTip, const juce::String& ringTip);
    void setModulated (float knobValue);   // live value of the knob incl. modulation (drawn as a dot)
    void resized() override;
    void paint (juce::Graphics&) override;

    struct RingSlider : juce::Slider
    {
        bool hitTest (int x, int y) override;
    };

private:
    RingSlider ring;
    juce::Slider knob;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> knobAtt, ringAtt;
    juce::String knobName, ringName;
    juce::Rectangle<int> dial;
};

//==============================================================================
class TheGateLook : public juce::LookAndFeel_V4
{
public:
    TheGateLook();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float start, float end, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
};

//==============================================================================
class TheGateEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit TheGateEditor (TheGateProcessor&);
    ~TheGateEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;
    };
    struct Choice
    {
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> att;
    };

    void makeKnob (Knob&, const char* id, const juce::String& name, const juce::String& tip);
    void makeChoice (Choice&, const char* id, const juce::String& name, const juce::String& tip);

    TheGateProcessor& proc;
    TheGateLook look;
    GateDisplay display;

    Knob threshold, hysteresis, attack, hold, release, range, keyHpf, keyHighCut, tracking;
    Knob levelAmount, levelTarget, levelMax, specTransient, specFocus, specSustain, strictness;
    juce::TextButton learnBtn { "LEARN" }, clearBtn { "CLEAR" };
    ConcentricKnob glowDrive;
    SpectrumView spectrum;
    Choice lookahead, keySource, glowType;
    juce::TextButton listen { "LISTEN KEY" }, levelerOn { "ON" }, glowOn { "ON" }, specOn { "ON" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> listenAtt, levelerOnAtt, glowOnAtt, specOnAtt;
    juce::TooltipWindow tooltips { this, 3000 };

    juce::Rectangle<int> detectArea, envArea, keyArea, levelArea, specArea, glowArea, sourceNote, learnNote;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TheGateEditor)
};
