#pragma once

#include "PluginProcessor.h"

namespace sgtm
{

// Vertical bar meter with a decaying display value. A reduction meter fills from the top as the
// value drops below maxDb and shows dB ticks.
class LevelMeter final : public juce::Component
{
public:
    LevelMeter (juce::String labelText, float minDb, float maxDb, juce::Colour barColour,
                bool isReductionMeter = false);

    void setLevelDb (float db);
    void paint (juce::Graphics&) override;

private:
    juce::String label;
    float minDb, maxDb;
    juce::Colour colour;
    bool reduction;
    float displayDb;
};

// Every linked channel, one row each: name, input level, automix gain and weight. This
// instance's row is highlighted. Reads the shared block on the message thread only.
class ChannelList final : public juce::Component
{
public:
    static constexpr int rowHeight = 24;
    static constexpr int headerHeight = 18;

    void setChannels (std::vector<LinkedChannelInfo> newChannels);
    void paint (juce::Graphics&) override;

private:
    std::vector<LinkedChannelInfo> channels;
};

class AutomixEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AutomixEditor (AutomixProcessor&);
    ~AutomixEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    AutomixProcessor& processor;
    juce::Image logoImage;

    LevelMeter inputMeter { "IN", -60.0f, 0.0f, juce::Colour (0xff4caf50) };
    LevelMeter gainMeter { "GAIN", -20.0f, 0.0f, juce::Colour (0xffffb300), true };
    LevelMeter outputMeter { "OUT", -60.0f, 0.0f, juce::Colour (0xff42a5f5) };

    juce::Slider weightSlider, outputGainSlider;
    juce::Label weightLabel, outputGainLabel;
    SliderAttachment weightAttachment, outputGainAttachment;

    juce::ToggleButton bypassButton { "Bypass" };
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;
    juce::ToggleButton allOnButton { "Automix on (all channels)" };

    juce::Label nameEditor;
    juce::Label linkStatus;
    ChannelList channelList;
    juce::Viewport channelViewport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomixEditor)
};

} // namespace sgtm
