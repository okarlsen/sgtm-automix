#pragma once

#include "PluginProcessor.h"

namespace sgtm
{

// Level meters (IN, OUT and the input bars in the channel list) run from 0 down to -80 dBFS, so
// the quiet region around the no-signal threshold (-75 dBFS) is visible.
constexpr float levelRangeDb = 80.0f;

// Vertical bar meter with a decaying display value, optionally with dB ticks and dimmed.
class LevelMeter final : public juce::Component
{
public:
    LevelMeter (juce::String labelText, float minDb, float maxDb, juce::Colour barColour,
                bool showTicks = false);

    void setLevelDb (float db);
    // Two bars side by side (left, right) when stereo, one bar otherwise.
    void setLevelsDb (float leftDb, float rightDb, bool stereo);
    void setDimmed (bool shouldBeDimmed);
    void paint (juce::Graphics&) override;

private:
    juce::String label;
    float minDb, maxDb;
    juce::Colour colour;
    bool ticks;
    bool dimmed = false;
    bool isStereo = false;
    float displayDb[2];
};

// Every linked channel, one row each: name, input level, automix gain and weight. This
// instance's row is highlighted. Reads the shared block on the message thread only.
class ChannelList final : public juce::Component
{
public:
    static constexpr int rowHeight = 24;
    static constexpr int headerHeight = 18;

    void setChannels (std::vector<LinkedChannelInfo> newChannels, bool automixOn);
    void paint (juce::Graphics&) override;

private:
    std::vector<LinkedChannelInfo> channels;
    bool allOn = true;
};

// Group selector menu: each entry (A, B, C) drawn in its group's colour.
class GroupLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                            const juce::Colour* textColour) override;
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
    void updateGroupColours();
    void showHelpDialog();

    static constexpr int helpButtonSize = 22;
    juce::TextButton helpButton { "?" };

    static constexpr int minWidth = 460, minHeight = 450;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    AutomixProcessor& processor;
    juce::Image logoImage;

    LevelMeter inputMeter { "IN", -levelRangeDb, 0.0f, juce::Colour (0xff4caf50) };
    LevelMeter gainMeter { "GAIN", -15.0f, 0.0f, juce::Colour (0xffffb300), true };
    LevelMeter outputMeter { "OUT", -levelRangeDb, 0.0f, juce::Colour (0xff42a5f5) };

    juce::Slider weightSlider, outputGainSlider;
    juce::Label weightLabel, outputGainLabel;
    SliderAttachment weightAttachment, outputGainAttachment;

    juce::ToggleButton bypassButton { "Bypass" };
    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;
    juce::ToggleButton allOnButton { "Automix on (all channels)" };
    GroupLookAndFeel groupLookAndFeel;
    juce::ComboBox groupBox;
    int shownGroup = -1;
    juce::Label groupLabel;
    juce::AudioProcessorValueTreeState::ComboBoxAttachment groupAttachment;

    juce::Label nameEditor;
    juce::Label linkStatus;
    ChannelList channelList;
    juce::Viewport channelViewport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutomixEditor)
};

} // namespace sgtm
