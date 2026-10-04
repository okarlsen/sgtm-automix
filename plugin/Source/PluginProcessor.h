#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "AutomixEngine.h"
#include "InstanceLink.h"

#include <atomic>

namespace sgtm
{

class AutomixProcessor final : public juce::AudioProcessor, private juce::AsyncUpdater
{
public:
    AutomixProcessor();
    ~AutomixProcessor() override;

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
    void updateTrackProperties (const TrackProperties& properties) override;

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }

    // The host's bypass maps to this channel's Bypass: unity gain, out of the group.
    juce::AudioProcessorParameter* getBypassParameter() const override;

    // Meter values, written by the audio thread, read by the editor.
    struct Meters
    {
        std::atomic<float> inputDb { -120.0f };
        std::atomic<float> automixGainDb { 0.0f };
        std::atomic<float> outputDb { -120.0f };
        std::atomic<int> numPeers { 0 };
    };
    const Meters& getMeters() const noexcept { return meters; }

    // Instance link, for the editor (message thread).
    bool isLinkAvailable() const noexcept { return link.isAvailable() && link.isJoined(); }

    // The all-channels switch, shared by every linked instance (not saved with the session).
    bool isAutomixOnForAll() const noexcept { return link.isAutomixOn(); }
    bool isBypassed() const noexcept { return bypass->load() >= 0.5f; }
    void setAutomixOnForAll (bool on) noexcept { link.setAutomixOn (on); }
    std::vector<LinkedChannelInfo> getLinkedChannels() const { return link.getChannels(); }

    // Channel name shown on every linked instance: the user's name if set, else the host's track
    // name. Message thread.
    juce::String getChannelName() const { return userLabel; }
    void setChannelName (const juce::String& name);
    juce::String getDisplayedLabel() const;

    static constexpr const char* weightId = "weight";
    static constexpr const char* outputGainId = "outputGain";
    static constexpr const char* bypassId = "bypass";

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* weightDb = nullptr;
    std::atomic<float>* outputGainDb = nullptr;
    std::atomic<float>* bypass = nullptr;

    static std::string linkName();
    void handleAsyncUpdate() override;
    void publishLabel();

    AutomixChannel engine;
    InstanceLink link;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> outputGain;

    // Absolute position of the next block. Offline, the host timeline position is used when the
    // host gives one, so all instances agree on hop boundaries; live, each instance counts its own
    // (live peers are matched by newest value, not by position).
    int64_t samplePosition = 0;

    juce::String userLabel;
    juce::CriticalSection trackNameLock;
    juce::String trackName;

    Meters meters;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomixProcessor)
};

} // namespace sgtm
