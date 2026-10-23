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
    Knob levelAmount, levelTarget, levelMax, glowDrive, glowCeiling;
    Choice lookahead, keySource, glowType;
    juce::TextButton listen { "LISTEN KEY" }, levelerOn { "ON" }, glowOn { "ON" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> listenAtt, levelerOnAtt, glowOnAtt;
    juce::TooltipWindow tooltips { this, 3000 };

    juce::Rectangle<int> detectArea, envArea, keyArea, levelArea, glowArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TheGateEditor)
};
