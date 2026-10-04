#include "PluginEditor.h"
#include "BinaryData.h"

namespace sgtm
{

//==================================================================================================
LevelMeter::LevelMeter (juce::String labelText, float min, float max, juce::Colour barColour)
    : label (std::move (labelText)), minDb (min), maxDb (max), colour (barColour), displayDb (min)
{
}

void LevelMeter::setLevelDb (float db)
{
    // Instant rise, ~20 dB/s fall at 30 Hz refresh.
    db = juce::jlimit (minDb, maxDb, db);
    const float fallen = displayDb - 0.7f;
    const float next = std::max (db, fallen);
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
    auto filled = bar.withTop (bar.getBottom() - bar.getHeight() * proportion);
    g.setColour (colour);
    g.fillRoundedRectangle (filled, 3.0f);

    g.setColour (juce::Colours::lightgrey);
    g.setFont (12.0f);
    g.drawText (label, labelArea, juce::Justification::centred);
}

//==================================================================================================
AutomixEditor::AutomixEditor (AutomixProcessor& p)
    : AudioProcessorEditor (p),
      processor (p),
      weightAttachment (p.getParameters(), AutomixProcessor::weightId, weightSlider),
      outputGainAttachment (p.getParameters(), AutomixProcessor::outputGainId, outputGainSlider)
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

    setSize (380, 240);
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
}

} // namespace sgtm
