#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "GateCore.h"
#include "Params.h"

class TheGateProcessor : public juce::AudioProcessor,
                         private juce::Timer
{
public:
    TheGateProcessor();
    ~TheGateProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    juce::AudioProcessorValueTreeState apvts;
    tg::GateEngine engine;   // the editor pops meter frames and hits from its FIFOs (UI thread only)

    bool sidechainConnected() const noexcept { return sidechainActive.load(); }

    // Learn (not a parameter: it's an action, not something to automate)
    std::atomic<bool> learnArmed { false };
    void clearLearned() noexcept { engine.clearProfileRequest.store (true); }

private:
    void timerCallback() override;   // keeps the reported latency in step with the Lookahead choice
    tg::GateSettings readSettings() const noexcept;
    int latencyForCurrentChoice() const noexcept;
    float effectiveLookaheadMs() const noexcept;
    static constexpr float kSourceMinLookaheadMs = 15.0f;
public:
    static constexpr float kStrictnessDefault = 50.0f;
private:

    struct Raw
    {
        std::atomic<float>* threshold {}, * hysteresis {}, * attack {}, * hold {}, * release {}, * range {},
                          * lookahead {}, * keySource {}, * keyHpf {}, * keyHighCut {}, * tracking {}, * listen {},
                          * levelerOn {}, * levelAmount {}, * levelTarget {}, * levelMax {},
                          * glowOn {}, * glowType {}, * glowDrive {}, * glowEnv {},
                          * specOn {}, * specFocus {}, * specSustain {}, * specTransient {}, * strictness {};
    } raw;

    void drainProfile();                      // message thread: newest learned profile from the engine
    juce::SpinLock profileLock;
    tg::ToneProfile savedProfile;             // what gets saved with the session

    double currentRate = 0.0;
    std::atomic<bool> sidechainActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TheGateProcessor)
};
