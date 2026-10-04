#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace sgtm
{

namespace
{
float blockRmsDb (const juce::AudioBuffer<float>& buffer, int numChannels, int numSamples)
{
    if (numChannels == 0 || numSamples == 0)
        return -120.0f;

    double sum = 0.0;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = buffer.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            sum += static_cast<double> (data[i]) * data[i];
    }
    return static_cast<float> (gainToDb (std::sqrt (sum / (numChannels * numSamples))));
}
} // namespace

AutomixProcessor::AutomixProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "SGTMAutomix", createParameterLayout())
{
    weightDb = parameters.getRawParameterValue (weightId);
    outputGainDb = parameters.getRawParameterValue (outputGainId);
}

juce::AudioProcessorValueTreeState::ParameterLayout AutomixProcessor::createParameterLayout()
{
    auto dbAttributes = juce::AudioParameterFloatAttributes().withLabel ("dB");

    return {
        std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { weightId, 1 }, "Weight",
            juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f, dbAttributes),
        std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { outputGainId, 1 }, "Output Gain",
            juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f, dbAttributes),
    };
}

bool AutomixProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Insert on a mono or stereo vocal track, same layout in and out.
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void AutomixProcessor::prepareToPlay (double sampleRate, int)
{
    setLatencySamples (0);
    engine.prepare (sampleRate);
    engine.setWeightDb (weightDb->load());
    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputGainDb->load()));
    samplePosition = 0;
}

void AutomixProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int ch = getTotalNumInputChannels(); ch < numChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    meters.inputDb.store (blockRmsDb (buffer, numChannels, numSamples), std::memory_order_relaxed);

    engine.setWeightDb (weightDb->load());
    engine.process (buffer.getArrayOfWritePointers(), numChannels, numSamples, samplePosition,
                    /* peers: instance link arrives in thread 3 */ nullptr);
    samplePosition += numSamples;

    outputGain.setTargetValue (juce::Decibels::decibelsToGain (outputGainDb->load()));
    if (outputGain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outputGain.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[i] *= g;
        }
    }
    else
    {
        buffer.applyGain (outputGain.getTargetValue());
    }

    meters.automixGainDb.store (static_cast<float> (gainToDb (engine.getCurrentGain())),
                                std::memory_order_relaxed);
    meters.outputDb.store (blockRmsDb (buffer, numChannels, numSamples), std::memory_order_relaxed);
}

void AutomixProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = parameters.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void AutomixProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
            parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* AutomixProcessor::createEditor()
{
    return new AutomixEditor (*this);
}

} // namespace sgtm

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sgtm::AutomixProcessor();
}
