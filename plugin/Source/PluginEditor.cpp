#include "PluginEditor.h"
#include "BinaryData.h"

namespace sgtm
{

//==================================================================================================
namespace
{
// Gain scale shared by the GAIN meter and the channel list: the gain a channel lets through, full
// at 0 dB and empty at -15 dB, linear in dB. A channel talking alone reads full; two equal channels
// read -3 dB; ten equal idle channels read -10 dB, two thirds down.
constexpr float gainRangeDb = 15.0f;
constexpr float gainTicksDb[] = { 0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -15.0f };

juce::Colour groupColour (int group)
{
    const juce::Colour colours[] = { juce::Colour (0xff4fc3f7), juce::Colour (0xffba68c8), juce::Colour (0xff81c784) };
    return colours[juce::jlimit (0, 2, group)];
}

// The combo box needs its items before the parameter attachment is made.
juce::ComboBox& withGroupItems (juce::ComboBox& box)
{
    box.addItemList ({ "A", "B", "C" }, 1);
    return box;
}

float gainProportion (float gainDb) { return juce::jlimit (0.0f, 1.0f, (gainDb + gainRangeDb) / gainRangeDb); }
} // namespace

LevelMeter::LevelMeter (juce::String labelText, float min, float max, juce::Colour barColour, bool showTicks)
    : label (std::move (labelText)), minDb (min), maxDb (max), colour (barColour), ticks (showTicks), displayDb (min)
{
}

void LevelMeter::setDimmed (bool shouldBeDimmed)
{
    if (dimmed != shouldBeDimmed)
    {
        dimmed = shouldBeDimmed;
        repaint();
    }
}

void LevelMeter::setLevelDb (float db)
{
    // Instant rise, ~20 dB/s fall at 30 Hz refresh.
    db = juce::jlimit (minDb, maxDb, db);
    const float next = std::max (db, displayDb - 0.7f);
    if (! juce::approximatelyEqual (next, displayDb))
    {
        displayDb = next;
        repaint();
    }
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();
    auto labelArea = area.removeFromBottom (18);
    auto bar = area.reduced (4, 2).toFloat();

    g.setColour (juce::Colour (0xff202124));
    g.fillRoundedRectangle (bar, 3.0f);

    const float proportion = (displayDb - minDb) / (maxDb - minDb);
    g.setColour (dimmed ? colour.withAlpha (0.3f) : colour);
    g.fillRoundedRectangle (bar.withTop (bar.getBottom() - bar.getHeight() * proportion), 3.0f);

    if (ticks)
    {
        g.setFont (9.0f);
        for (float tick : gainTicksDb)
        {
            const float y = bar.getY() + bar.getHeight() * (maxDb - tick) / (maxDb - minDb);
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawHorizontalLine ((int) std::round (y), bar.getX(), bar.getX() + 6.0f);
            g.setColour (juce::Colours::lightgrey);
            auto text = juce::Rectangle<float> (bar.getX() + 7.0f, y - 6.0f, bar.getWidth() - 8.0f, 12.0f)
                            .constrainedWithin (bar);
            g.drawText (juce::String ((int) tick), text, juce::Justification::centredLeft);
        }
    }

    g.setColour (juce::Colours::lightgrey);
    g.setFont (12.0f);
    g.drawText (label, labelArea, juce::Justification::centred);
}

//==================================================================================================
void ChannelList::setChannels (std::vector<LinkedChannelInfo> newChannels, bool automixOn)
{
    auto same = [] (const LinkedChannelInfo& a, const LinkedChannelInfo& b)
    {
        // Compare at display resolution so an idle list does not repaint.
        auto q = [] (float db) { return (int) std::lround (db * 2.0f); };
        return a.slot == b.slot && a.group == b.group && a.isSelf == b.isSelf && a.label == b.label && q (a.inputDb) == q (b.inputDb)
               && q (a.gainDb) == q (b.gainDb) && q (a.weightDb) == q (b.weightDb) && a.bypassed == b.bypassed && a.present == b.present && a.idle == b.idle;
    };

    if (automixOn == allOn && newChannels.size() == channels.size()
        && std::equal (newChannels.begin(), newChannels.end(), channels.begin(), same))
        return;

    channels = std::move (newChannels);
    allOn = automixOn;
    setSize (getWidth(), headerHeight + std::max (1, (int) channels.size()) * rowHeight);
    repaint();
}

void ChannelList::paint (juce::Graphics& g)
{
    auto drawBar = [&g] (juce::Rectangle<float> r, float proportion, juce::Colour colour)
    {
        g.setColour (juce::Colour (0xff202124));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (colour);
        g.fillRoundedRectangle (r.withWidth (r.getWidth() * juce::jlimit (0.0f, 1.0f, proportion)), 2.0f);
    };

    // Column headings, laid out like the rows below.
    {
        auto row = juce::Rectangle<int> (0, 0, getWidth(), headerHeight).reduced (6, 0);
        g.setColour (juce::Colours::grey);
        g.setFont (11.0f);
        g.drawText ("Grp", row.removeFromLeft (26), juce::Justification::centredLeft);
        g.drawText ("Channel", row.removeFromLeft (100), juce::Justification::centredLeft);
        g.drawText ("Weight", row.removeFromRight (64), juce::Justification::centredRight);
        g.drawText ("Gain dB", row.removeFromRight (56), juce::Justification::centredRight);
        row.removeFromRight (6);
        g.drawText ("Input", row.removeFromLeft (row.getWidth() / 2), juce::Justification::centredLeft);
        g.drawText ("Gain", row.withTrimmedLeft (4), juce::Justification::centredLeft);
    }

    if (channels.empty())
    {
        g.setColour (juce::Colours::grey);
        g.setFont (12.0f);
        g.drawText ("No channels running", getLocalBounds().withTrimmedTop (headerHeight), juce::Justification::centred);
        return;
    }

    int y = headerHeight;
    int previousGroup = -1;
    for (const auto& c : channels)
    {
        if (previousGroup >= 0 && c.group != previousGroup)
        {
            g.setColour (juce::Colours::white.withAlpha (0.15f));
            g.drawHorizontalLine (y, 0.0f, (float) getWidth());
        }
        previousGroup = c.group;

        auto row = juce::Rectangle<int> (0, y, getWidth(), rowHeight).reduced (0, 2);
        y += rowHeight;

        if (c.isSelf)
        {
            g.setColour (juce::Colour (0xff3d4048));
            g.fillRoundedRectangle (row.toFloat(), 3.0f);
        }

        row.reduce (6, 0);
        g.setColour (c.isSelf ? juce::Colours::white : juce::Colours::lightgrey);
        g.setFont (juce::FontOptions (12.0f, c.isSelf ? juce::Font::bold : juce::Font::plain));
        g.setColour (groupColour (c.group));
        g.drawText (AutomixProcessor::groupName (c.group), row.removeFromLeft (26), juce::Justification::centredLeft);
        g.setColour (c.isSelf ? juce::Colours::white : juce::Colours::lightgrey);
        g.drawText (c.label, row.removeFromLeft (100), juce::Justification::centredLeft, true);

        if (c.idle)
        {
            g.setColour (juce::Colours::grey);
            g.drawText ("idle: host is not processing this track", row, juce::Justification::centredLeft, true);
            continue;
        }

        auto weightArea = row.removeFromRight (64);
        g.setColour (c.bypassed ? juce::Colour (0xffef5350) : ! c.present ? juce::Colour (0xff90a4ae) : juce::Colours::grey);
        g.drawText (c.bypassed     ? juce::String ("BYPASS")
                    : ! c.present ? juce::String ("NO SIGNAL")
                                  : juce::String (c.weightDb, 1) + " dB",
                    weightArea, juce::Justification::centredRight);

        auto gainText = row.removeFromRight (56);
        g.setColour (juce::Colour (0xffffb300));
        g.drawText (juce::String (c.gainDb > -0.05f ? 0.0f : c.gainDb, 1), gainText,
                    juce::Justification::centredRight);

        row.removeFromRight (6);
        const auto bars = row.toFloat().reduced (0, 4);
        const auto half = bars.getWidth() / 2.0f - 2.0f;
        drawBar (bars.withWidth (half), (c.inputDb + 60.0f) / 60.0f, juce::Colour (0xff4caf50));
        const auto gainBar = bars.withTrimmedLeft (half + 4.0f);
        const auto gainColour = juce::Colour (0xffffb300);
        drawBar (gainBar, gainProportion (c.gainDb),
                 c.bypassed || ! c.present || ! allOn ? gainColour.withAlpha (0.3f) : gainColour);
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        for (float tick : gainTicksDb)
            if (tick < 0.0f && tick > -gainRangeDb)
                g.drawVerticalLine ((int) std::round (gainBar.getX() + gainBar.getWidth() * gainProportion (tick)),
                                    gainBar.getY(), gainBar.getBottom());
    }
}

//==================================================================================================
AutomixEditor::AutomixEditor (AutomixProcessor& p)
    : AudioProcessorEditor (p),
      processor (p),
      weightAttachment (p.getParameters(), AutomixProcessor::weightId, weightSlider),
      outputGainAttachment (p.getParameters(), AutomixProcessor::outputGainId, outputGainSlider),
      bypassAttachment (p.getParameters(), AutomixProcessor::bypassId, bypassButton),
      groupAttachment (p.getParameters(), AutomixProcessor::groupId, withGroupItems (groupBox))
{
    logoImage = juce::ImageCache::getFromMemory (Assets::sgtm_logo_png, Assets::sgtm_logo_pngSize);

    for (auto* meter : { &inputMeter, &gainMeter, &outputMeter })
        addAndMakeVisible (meter);

    auto setUpKnob = [this] (juce::Slider& slider, juce::Label& label, const juce::String& text)
    {
        slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 18);
        slider.setTextValueSuffix (" dB");
        addAndMakeVisible (slider);

        label.setText (text, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (label);
    };

    setUpKnob (weightSlider, weightLabel, "Weight");
    setUpKnob (outputGainSlider, outputGainLabel, "Output");

    // This channel's name, shown on every linked instance. Empty = the host's track name.
    nameEditor.setEditable (true);
    nameEditor.setText (p.getDisplayedLabel(), juce::dontSendNotification);
    nameEditor.setTooltip ("Channel name (click to edit)");
    nameEditor.setColour (juce::Label::backgroundColourId, juce::Colour (0xff202124));
    nameEditor.setColour (juce::Label::textColourId, juce::Colours::white);
    nameEditor.onTextChange = [this]
    {
        processor.setChannelName (nameEditor.getText());
        nameEditor.setText (processor.getDisplayedLabel(), juce::dontSendNotification);
    };
    addAndMakeVisible (nameEditor);

    bypassButton.setTooltip ("This channel at unity gain, out of the gain sharing");
    addAndMakeVisible (bypassButton);

    groupLabel.setText ("Group", juce::dontSendNotification);
    groupLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (groupLabel);
    groupBox.setTooltip ("Gain is shared only with channels in the same group");
    addAndMakeVisible (groupBox);

    allOnButton.setTooltip ("Turns the automix on or off on every linked channel at once, for A/B comparison");
    allOnButton.setToggleState (p.isAutomixOnForAll(), juce::dontSendNotification);
    allOnButton.onClick = [this] { processor.setAutomixOnForAll (allOnButton.getToggleState()); };
    addAndMakeVisible (allOnButton);

    linkStatus.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
    linkStatus.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (linkStatus);

    channelViewport.setViewedComponent (&channelList, false);
    channelViewport.setScrollBarsShown (true, false);
    addAndMakeVisible (channelViewport);

    setSize (460, 450);
    startTimerHz (30);
}

AutomixEditor::~AutomixEditor()
{
    stopTimer();
}

void AutomixEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff2b2d31));

    auto header = getLocalBounds().removeFromTop (34).reduced (12, 6);
    if (logoImage.isValid())
    {
        auto logoArea = header.removeFromLeft (header.getHeight() * logoImage.getWidth() / logoImage.getHeight());
        g.drawImage (logoImage, logoArea.toFloat(), juce::RectanglePlacement::centred);
        header.removeFromLeft (8);
    }

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
    g.drawText ("Automix", header, juce::Justification::centredLeft);

    // Version in the title bar, so a screenshot or bug report says which build is running.
    g.setColour (juce::Colours::grey);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("v" JucePlugin_VersionString, header, juce::Justification::centredRight);
}

void AutomixEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (28);

    auto listArea = area.removeFromBottom (200);
    auto switchRow = listArea.removeFromTop (26);
    bypassButton.setBounds (switchRow.removeFromLeft (90));
    groupBox.setBounds (switchRow.removeFromRight (56).reduced (0, 1));
    groupLabel.setBounds (switchRow.removeFromRight (48));
    allOnButton.setBounds (switchRow);
    auto statusRow = listArea.removeFromTop (24);
    nameEditor.setBounds (statusRow.removeFromLeft (150).reduced (0, 2));
    statusRow.removeFromLeft (8);
    linkStatus.setBounds (statusRow);
    listArea.removeFromTop (4);
    channelViewport.setBounds (listArea);
    channelList.setSize (listArea.getWidth() - channelViewport.getScrollBarThickness(),
                         channelList.getHeight());
    area.removeFromBottom (8);

    auto meters = area.removeFromLeft (150);
    const int meterWidth = meters.getWidth() / 3;
    inputMeter.setBounds (meters.removeFromLeft (meterWidth));
    gainMeter.setBounds (meters.removeFromLeft (meterWidth));
    outputMeter.setBounds (meters);

    area.removeFromLeft (12);
    const int knobWidth = area.getWidth() / 2;
    for (auto [slider, label] : { std::pair { &weightSlider, &weightLabel },
                                  std::pair { &outputGainSlider, &outputGainLabel } })
    {
        auto column = area.removeFromLeft (knobWidth);
        label->setBounds (column.removeFromTop (20));
        slider->setBounds (column.reduced (4));
    }
}

void AutomixEditor::timerCallback()
{
    const auto& m = processor.getMeters();
    inputMeter.setLevelDb (m.inputDb.load (std::memory_order_relaxed));
    gainMeter.setLevelDb (m.automixGainDb.load (std::memory_order_relaxed));
    outputMeter.setLevelDb (m.outputDb.load (std::memory_order_relaxed));

    auto channels = processor.getLinkedChannels();
    std::stable_sort (channels.begin(), channels.end(),
                      [] (const auto& a, const auto& b) { return a.group < b.group; });
    const int ownGroup = processor.getGroup();
    int idle = 0;
    bool selfIdle = false;
    for (const auto& c : channels)
    {
        idle += (! c.isSelf && c.idle && c.group == ownGroup) ? 1 : 0;
        selfIdle = selfIdle || (c.isSelf && c.idle);
    }

    juce::String status;
    if (! processor.isLinkAvailable())
        status = "Link unavailable (" + processor.getLinkUnavailableReason() + "): running solo";
    else
    {
        // Channels in this group that are processing, and how many of them take part (signal
        // present, not bypassed).
        int inGroup = 0, active = 0;
        for (const auto& c : channels)
            if (c.group == ownGroup && ! c.idle)
            {
                ++inGroup;
                active += (c.present && ! c.bypassed) ? 1 : 0;
            }
        status = "Group " + AutomixProcessor::groupName (ownGroup) + ": " + juce::String (active) + " active of "
                 + juce::String (inGroup) + " channel" + (inGroup == 1 ? "" : "s");
        if (idle > 0)
            status << ", " << idle << " idle";
        if (selfIdle)
            status = "Idle: the host is not processing this track";
    }
    if (! processor.isAutomixOnForAll())
        status = "Automix OFF on all channels";
    if (allOnButton.getToggleState() != processor.isAutomixOnForAll())
        allOnButton.setToggleState (processor.isAutomixOnForAll(), juce::dontSendNotification);
    if (linkStatus.getText() != status)
        linkStatus.setText (status, juce::dontSendNotification);

    if (! nameEditor.isBeingEdited() && nameEditor.getText() != processor.getDisplayedLabel())
        nameEditor.setText (processor.getDisplayedLabel(), juce::dontSendNotification);

    channelList.setChannels (channels, processor.isAutomixOnForAll());
    gainMeter.setDimmed (processor.isBypassed() || ! processor.isAutomixOnForAll());
}

} // namespace sgtm
