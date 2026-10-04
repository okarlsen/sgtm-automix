#pragma once

#include "PluginProcessor.h"

namespace sgtm
{

// Vertical bar meter with a decaying display value.
class LevelMeter final : public juce::Component
{
public:
    LevelMeter (juce::String labelText, float minDb, float maxDb, juce::Colour barColour);

    void setLevelDb (float db);
    void paint (juce::Graphics&) override;

private:
    juce::String label;
    float minDb, maxDb;
    juce::Colour colour;
    float displayDb;
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
    LevelMeter gainMeter { "GAIN", -40.0f, 0.0f, juce::Colour (0xffffb300) };
    LevelMeter outputMeter { "OUT", -60.0f, 0.0f, juce::Colour (0xff42a5f5) };

    juce::Slider weightSlider, outputGainSlider;
    juce::Label weightLabel, outputGainLabel;
    SliderAttachment weightAttachment, outputGainAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomixEditor)
};

} // namespace sgtm
