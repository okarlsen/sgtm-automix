#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace sgtm
{

AutomixProcessor::AutomixProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "SGTMAutomix", createParameterLayout()),
      link (linkName())
{
    weightDb = parameters.getRawParameterValue (weightId);
    outputGainDb = parameters.getRawParameterValue (outputGainId);
    bypass = parameters.getRawParameterValue (bypassId);
    groupParam = parameters.getRawParameterValue (groupId);
    link.join();
    numChannelsChanged();
}

AutomixProcessor::~AutomixProcessor()
{
    cancelPendingUpdate();
    link.leave();
}

// SGTM_AUTOMIX_LINK_NAME overrides the shared block, so tests never join a running session.
std::string AutomixProcessor::linkName()
{
    if (const char* name = std::getenv ("SGTM_AUTOMIX_LINK_NAME"))
        return name;
    return InstanceLink::defaultName();
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
        std::make_unique<juce::AudioParameterBool> (juce::ParameterID { bypassId, 1 }, "Bypass", false),
        // Gain is shared only among channels in the same group.
        std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { groupId, 1 }, "Group",
                                                      juce::StringArray { "A", "B", "C" }, 0),
    };
}

juce::AudioProcessorParameter* AutomixProcessor::getBypassParameter() const
{
    return parameters.getParameter (bypassId);
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
    engine.setBypassed (bypass->load() >= 0.5f || ! link.isAutomixOn(), true);
    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (bypass->load() >= 0.5f ? 1.0f
                                                                : juce::Decibels::decibelsToGain (outputGainDb->load()));
    samplePosition = 0;
    link.prepare (sampleRate);
    link.setGroup (getGroup(), true);
    link.join();
    triggerAsyncUpdate(); // republish the name from the message thread (hosts may call this from others)
}

void AutomixProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numChannels = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    for (int ch = getTotalNumInputChannels(); ch < numChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    const auto in = measureLevels (buffer.getArrayOfReadPointers(), numChannels, numSamples);
    const float inputDb = in.combinedDb;
    meters.numChannels.store (numChannels, std::memory_order_relaxed);
    meters.inputDb.store (inputDb, std::memory_order_relaxed);
    meters.inputLeftDb.store (in.channelDb[0], std::memory_order_relaxed);
    meters.inputRightDb.store (in.channelDb[1], std::memory_order_relaxed);

    // Offline, take the block's position from the host timeline so every instance agrees on hop
    // numbers. Live, keep counting: peers are matched by their newest value.
    const bool offline = isNonRealtime();
    int64_t startSample = samplePosition;
    if (offline)
        if (auto* playHead = getPlayHead())
            if (auto position = playHead->getPosition())
                if (auto time = position->getTimeInSamples())
                    startSample = *time;

    link.setGroup (getGroup());
    link.beginBlock (InstanceLink::steadyNowNs(), offline, startSample, numSamples);
    meters.numPeers.store (link.getNumPeers(), std::memory_order_relaxed);

    const float weight = weightDb->load();
    const bool channelBypassed = bypass->load() >= 0.5f;
    engine.setWeightDb (weight);
    engine.setBypassed (channelBypassed || ! link.isAutomixOn());
    engine.process (buffer.getArrayOfWritePointers(), numChannels, numSamples, startSample,
                    link.isJoined() ? &link : nullptr);
    samplePosition = startSample + numSamples;

    // Bypassed (by the host or with the Bypass button, which are the same parameter), the audio
    // passes unchanged: the Output trim fades to unity along with the automix gain.
    outputGain.setTargetValue (channelBypassed ? 1.0f : juce::Decibels::decibelsToGain (outputGainDb->load()));
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

    const auto automixDb = static_cast<float> (gainToDb (engine.getCurrentGain()));
    const auto out = measureLevels (buffer.getArrayOfReadPointers(), numChannels, numSamples);
    const float outputDb = out.combinedDb;
    meters.outputLeftDb.store (out.channelDb[0], std::memory_order_relaxed);
    meters.outputRightDb.store (out.channelDb[1], std::memory_order_relaxed);
    meters.automixGainDb.store (automixDb, std::memory_order_relaxed);
    meters.outputDb.store (outputDb, std::memory_order_relaxed);
    link.setDisplay (inputDb, automixDb, outputDb, weight, channelBypassed, engine.isPresent());
    link.setInputSides (in.channelDb[0], in.channelDb[1], numChannels);
}

void AutomixProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("channelName", getChannelName(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void AutomixProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (parameters.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);
            {
                const juce::ScopedLock lock (trackNameLock);
                userLabel = state.getProperty ("channelName").toString();
            }
            state.removeProperty ("channelName", nullptr);
            parameters.replaceState (state);
            triggerAsyncUpdate();
        }
}

// The track's layout is known as soon as the host sets it, before any audio is processed, so the
// meters and the channel list show mono or stereo correctly from the moment a session opens.
void AutomixProcessor::numChannelsChanged()
{
    const int channels = std::max (1, getTotalNumOutputChannels()); // as processBlock counts them
    meters.numChannels.store (channels, std::memory_order_relaxed);
    link.setInputSides (-120.0f, -120.0f, channels);
}

// Hosts may call this from any thread; the link is updated on the message thread.
void AutomixProcessor::updateTrackProperties (const TrackProperties& properties)
{
    {
        const juce::ScopedLock lock (trackNameLock);
        trackName = properties.name.value_or (juce::String());
    }
    triggerAsyncUpdate();
}

void AutomixProcessor::setChannelName (const juce::String& name)
{
    {
        const juce::ScopedLock lock (trackNameLock);
        userLabel = name.trim();
    }
    publishLabel();
}

juce::String AutomixProcessor::getChannelName() const
{
    const juce::ScopedLock lock (trackNameLock);
    return userLabel;
}

// The user's name wins over the host's track name; either can arrive in any order.
juce::String AutomixProcessor::getDisplayedLabel() const
{
    const juce::ScopedLock lock (trackNameLock);
    return userLabel.isNotEmpty() ? userLabel : trackName;
}

void AutomixProcessor::handleAsyncUpdate() { publishLabel(); }

void AutomixProcessor::publishLabel() { link.setLabel (getDisplayedLabel().toStdString()); }

juce::AudioProcessorEditor* AutomixProcessor::createEditor()
{
    return new AutomixEditor (*this);
}

} // namespace sgtm

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sgtm::AutomixProcessor();
}
