#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "AutomixEngine.h"

#include <atomic>

namespace sgtm
{

class AutomixProcessor final : public juce::AudioProcessor
{
public:
    AutomixProcessor();

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

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

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }

    // Meter values, written by the audio thread, read by the editor.
    struct Meters
    {
        std::atomic<float> inputDb { -120.0f };
        std::atomic<float> automixGainDb { 0.0f };
        std::atomic<float> outputDb { -120.0f };
    };
    const Meters& getMeters() const noexcept { return meters; }

    static constexpr const char* weightId = "weight";
    static constexpr const char* outputGainId = "outputGain";

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* weightDb = nullptr;
    std::atomic<float>* outputGainDb = nullptr;

    AutomixChannel engine;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outputGain;

    // Absolute position of the next block. Thread 3 replaces this with the host timeline
    // position so all instances agree on hop boundaries; for now each instance counts its own.
    int64_t samplePosition = 0;

    Meters meters;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomixProcessor)
};

} // namespace sgtm
