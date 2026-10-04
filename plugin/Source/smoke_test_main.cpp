// Loads the built VST3 bundles the way a host would and checks the basics:
//   SGTM Automix: loads, reports 0 latency, passes audio unchanged when solo, output gain works,
//                 state round-trips, editor opens.
//   Two SGTM Automix instances link up and share gain; Bypass takes one out of the group.
//   SGTM Host Probe: two instances log every processBlock call (live and non-realtime) to CSV.
//
// Usage: "SGTM Automix Smoke Test" [<SGTM Automix.vst3> [<SGTM Host Probe.vst3>]]
// With no arguments it finds both bundles in the same build directory as itself.

#include <juce_audio_processors/juce_audio_processors.h>
#include "InstanceLink.h"
#include <juce_events/juce_events.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#if ! JUCE_WINDOWS
 #include <sys/mman.h>
#endif

#if JUCE_MAC
 #include <CoreFoundation/CoreFoundation.h>
#endif

namespace
{
int failures = 0;

std::string linkNameForTest()
{
    const char* name = std::getenv ("SGTM_AUTOMIX_LINK_NAME");
    return name != nullptr ? name : "";
}

void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok)
        ++failures;
}

std::unique_ptr<juce::AudioPluginInstance> load (juce::AudioPluginFormatManager& formats,
                                                 const juce::String& path, double sr, int block)
{
    juce::OwnedArray<juce::PluginDescription> found;
    juce::VST3PluginFormat vst3;
    vst3.findAllTypesForFile (found, path);
    if (found.isEmpty())
        return nullptr;

    juce::String error;
    auto instance = formats.createPluginInstance (*found[0], sr, block, error);
    if (instance == nullptr)
        std::printf ("load error: %s\n", error.toRawUTF8());
    return instance;
}

juce::AudioBuffer<float> sine (int channels, int samples, double sr)
{
    juce::AudioBuffer<float> b (channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < samples; ++i)
            b.setSample (ch, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / sr));
    return b;
}

// Processes `input` through the plugin in blocks of `block` samples and returns the output.
juce::AudioBuffer<float> run (juce::AudioPluginInstance& p, const juce::AudioBuffer<float>& input, int block)
{
    juce::AudioBuffer<float> out (input);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < out.getNumSamples(); pos += block)
    {
        const int n = std::min (block, out.getNumSamples() - pos);
        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), out.getNumChannels(), pos, n);
        p.processBlock (view, midi);
    }
    return out;
}

float maxDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, float scaleB = 1.0f)
{
    float d = 0.0f;
    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        for (int i = 0; i < a.getNumSamples(); ++i)
            d = std::max (d, std::abs (a.getSample (ch, i) - scaleB * b.getSample (ch, i)));
    return d;
}

juce::AudioProcessorParameter* findParam (juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* param : p.getParameters())
        if (param->getName (64) == name)
            return param;
    return nullptr;
}

void testAutomix (juce::AudioPluginFormatManager& formats, const juce::String& path)
{
    std::printf ("SGTM Automix (%s)\n", path.toRawUTF8());
    const double sr = 48000.0;
    const int block = 256;

    auto p = load (formats, path, sr, block);
    check (p != nullptr, "VST3 loads");
    if (p == nullptr)
        return;

    check (p->getName() == "SGTM Automix", "name is SGTM Automix (got '" + p->getName() + "')");

    for (auto layout : { juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo() })
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (layout);
        l.outputBuses.add (layout);
        check (p->checkBusesLayoutSupported (l), "supports " + layout.getDescription() + " insert");
    }

    p->prepareToPlay (sr, block);
    check (p->getLatencySamples() == 0, "reports 0 samples latency");

    const auto input = sine (2, 48000, sr);
    auto out = run (*p, input, block);
    check (maxDiff (out, input) < 1.0e-6f, "solo instance passes audio through unchanged");

    auto* gain = findParam (*p, "Output Gain");
    check (gain != nullptr, "has Output Gain parameter");
    check (findParam (*p, "Weight") != nullptr, "has Weight parameter");

    if (gain != nullptr)
    {
        // Range -24..+12 dB: -6 dB is at (18/36) = 0.5.
        gain->setValueNotifyingHost (0.5f);
        p->releaseResources();
        p->prepareToPlay (sr, block);
        out = run (*p, input, block);
        const float expected = juce::Decibels::decibelsToGain (-6.0f);
        check (maxDiff (out, input, expected) < 1.0e-4f, "Output Gain -6 dB scales output by 0.501");

        juce::MemoryBlock state;
        p->getStateInformation (state);
        gain->setValueNotifyingHost (1.0f);
        p->setStateInformation (state.getData(), (int) state.getSize());
        check (std::abs (gain->getValue() - 0.5f) < 1.0e-3f, "state save/restore round-trips");
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditorAndMakeActive());
    check (editor != nullptr && editor->getWidth() > 0, "editor opens");
    editor.reset();

    p->releaseResources();
}

void testLinked (juce::AudioPluginFormatManager& formats, const juce::String& path)
{
    std::printf ("\nTwo linked SGTM Automix instances\n");
    const double sr = 48000.0;
    const int block = 64;

    auto a = load (formats, path, sr, block);
    auto b = load (formats, path, sr, block);
    check (a != nullptr && b != nullptr, "two instances load");
    if (a == nullptr || b == nullptr)
        return;
    a->prepareToPlay (sr, block);
    b->prepareToPlay (sr, block);

    // Equal input on both, processed in turn like a host's audio callback.
    const auto input = sine (2, block, sr);
    juce::MidiBuffer midi;
    auto runBoth = [&] (int blocks)
    {
        juce::AudioBuffer<float> outA, outB;
        for (int i = 0; i < blocks; ++i)
        {
            outA = input;
            outB = input;
            a->processBlock (outA, midi);
            b->processBlock (outB, midi);
        }
        return std::pair { outA, outB };
    };

    auto [outA, outB] = runBoth (750); // 1 s
    const float half = juce::Decibels::decibelsToGain (-3.01f);
    const float diffA = maxDiff (outA, input, half), diffB = maxDiff (outB, input, half);
    check (diffA < 0.01f && diffB < 0.01f,
           "equal channels share gain: each about -3 dB (max error " + juce::String (std::max (diffA, diffB), 4) + ")");

    auto* bypass = findParam (*a, "Bypass");
    check (bypass != nullptr && a->getBypassParameter() == bypass, "has Bypass, mapped to the host's bypass");
    if (bypass != nullptr)
    {
        bypass->setValueNotifyingHost (1.0f);
        std::tie (outA, outB) = runBoth (750);
        check (maxDiff (outA, input) < 1.0e-4f, "bypassed channel passes audio at unity");
        check (maxDiff (outB, input) < 1.0e-3f, "the other channel, now alone, is at unity");
        bypass->setValueNotifyingHost (0.0f);
    }

    // Host bypass with an Output trim set: audio must pass through unchanged, trim included.
    if (bypass != nullptr)
        if (auto* outGain = findParam (*a, "Output Gain"))
        {
            outGain->setValueNotifyingHost (0.5f); // -6 dB
            a->getBypassParameter()->setValueNotifyingHost (1.0f);
            std::tie (outA, outB) = runBoth (750);
            check (maxDiff (outA, input) == 0.0f, "host bypass passes audio unchanged, Output trim included (bit-exact)");
            a->getBypassParameter()->setValueNotifyingHost (0.0f);
            std::tie (outA, outB) = runBoth (750);
            const float shared = juce::Decibels::decibelsToGain (-3.01f) * juce::Decibels::decibelsToGain (-6.0f);
            check (maxDiff (outA, input, shared) < 0.01f, "un-bypassed, the trim and the sharing apply again");
            outGain->setValueNotifyingHost (2.0f / 3.0f); // back to 0 dB
        }

    // Track names reach the list in any order of name, layout change and prepare, and a name the
    // user typed beats the host's.
    {
        sgtm::InstanceLink reader (linkNameForTest());
        reader.join();
        auto pump = []
        {
           #if JUCE_MAC
            for (int i = 0; i < 5; ++i)
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
           #else
            juce::Thread::sleep (100);
           #endif
        };
        auto labels = [&reader]
        {
            juce::StringArray names;
            for (const auto& c : reader.getChannels())
                names.add (c.label);
            return names;
        };
        auto run = [&] (juce::AudioPluginInstance& p)
        {
            juce::AudioBuffer<float> buf (p.getTotalNumInputChannels(), block);
            buf.clear();
            p.processBlock (buf, midi);
        };

        juce::AudioPluginInstance::TrackProperties props;
        props.name = juce::String ("Vocal L");
        a->updateTrackProperties (props);
        pump();
        run (*a);
        check (labels().contains ("Vocal L"), "host track name appears in the list");

        // Mono to stereo after the name: the name stays.
        a->releaseResources();
        juce::AudioProcessor::BusesLayout stereo;
        stereo.inputBuses.add (juce::AudioChannelSet::stereo());
        stereo.outputBuses.add (juce::AudioChannelSet::stereo());
        a->setBusesLayout (stereo);
        a->prepareToPlay (sr, block);
        pump();
        run (*a);
        check (labels().contains ("Vocal L"), "the name survives a layout change to stereo");

        // Layout change first, then the name.
        b->releaseResources();
        b->setBusesLayout (stereo);
        b->prepareToPlay (sr, block);
        props.name = juce::String ("Choir");
        b->updateTrackProperties (props);
        pump();
        run (*b);
        check (labels().contains ("Choir"), "a name sent after the layout change appears");
    }

    a->releaseResources();
    b->releaseResources();
}

int countBlockLines (const juce::File& f)
{
    juce::StringArray lines;
    f.readLines (lines);
    int n = 0;
    for (auto& l : lines)
        if (l.startsWith ("block,"))
            ++n;
    return n;
}

void testProbe (juce::AudioPluginFormatManager& formats, const juce::String& path)
{
    std::printf ("\nSGTM Host Probe (%s)\n", path.toRawUTF8());
    const double sr = 48000.0;
    const int block = 128;

    auto a = load (formats, path, sr, block);
    auto b = load (formats, path, sr, block);
    check (a != nullptr && b != nullptr, "two instances load");
    if (a == nullptr || b == nullptr)
        return;

    const auto startTime = juce::Time::getCurrentTime() - juce::RelativeTime::seconds (1.0);

    for (auto* p : { a.get(), b.get() })
        p->prepareToPlay (sr, block);
    check (a->getLatencySamples() == 0, "reports 0 samples latency");

    const auto input = sine (2, 128 * 100, sr);
    run (*a, input, block);
    run (*b, input, 64);

    // A non-realtime pass with bigger blocks, like an offline bounce.
    a->releaseResources();
    a->setNonRealtime (true);
    a->prepareToPlay (sr, 1024);
    run (*a, input, 1024);

    a->releaseResources();
    b->releaseResources();
    a.reset();
    b.reset(); // destructors flush the logs

#if JUCE_MAC
    auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/Logs/SGTM Host Probe");
#else
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("SGTM Host Probe");
#endif

    juce::Array<juce::File> logs;
    for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "probe_*.csv"))
        if (f.getCreationTime() >= startTime)
            logs.add (f);

    check (logs.size() == 2, "one log file per instance (found " + juce::String (logs.size()) + " in " + dir.getFullPathName() + ")");

    juce::Array<int> counts;
    for (auto& f : logs)
        counts.add (countBlockLines (f));
    counts.sort();
    // Instance b: 12800 / 64 = 200 blocks. Instance a: 100 live + 13 non-realtime = 113 blocks.
    check (counts == juce::Array<int> { 113, 200 },
           "every processBlock call is logged (" + juce::String (counts.size() > 0 ? counts[0] : -1) + ", "
               + juce::String (counts.size() > 1 ? counts[1] : -1) + ")");

    bool sawNonRealtime = false;
    for (auto& f : logs)
        sawNonRealtime = sawNonRealtime || f.loadFileAsString().contains ("set_non_realtime");
    check (sawNonRealtime, "non-realtime switch is logged");

    for (auto& f : logs)
        std::printf ("      %s\n", f.getFullPathName().toRawUTF8());
}
} // namespace

int main (int argc, char* argv[])
{
    // Link the instances under test only with each other, never with a running session.
    const auto linkName = "/sgtm-am-smoke-" + juce::String (juce::Time::currentTimeMillis() % 1000000);
#if JUCE_WINDOWS
    _putenv_s ("SGTM_AUTOMIX_LINK_NAME", linkName.toRawUTF8());
#else
    setenv ("SGTM_AUTOMIX_LINK_NAME", linkName.toRawUTF8(), 1);
#endif

    juce::ScopedJuceInitialiser_GUI gui;
    juce::AudioPluginFormatManager formats;
    formats.addFormat (std::make_unique<juce::VST3PluginFormat>());

    auto absolute = [] (const char* arg)
    {
        return juce::File::getCurrentWorkingDirectory().getChildFile (arg).getFullPathName();
    };

    // <build>/SGTMAutomixSmokeTest_artefacts/<Config>/<this executable>
    const auto configDir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();
    const auto buildDir = configDir.getParentDirectory().getParentDirectory();
    auto bundle = [&] (const juce::String& target, const juce::String& product)
    {
        return buildDir.getChildFile (target + "_artefacts").getChildFile (configDir.getFileName())
                   .getChildFile ("VST3").getChildFile (product + ".vst3").getFullPathName();
    };

    const auto automixPath = argc > 1 ? absolute (argv[1]) : bundle ("SGTMAutomix", "SGTM Automix");
    const auto probePath = argc > 2 ? absolute (argv[2]) : bundle ("SGTMHostProbe", "SGTM Host Probe");

    testAutomix (formats, automixPath);
    testLinked (formats, automixPath);
    if (juce::File (probePath).exists())
        testProbe (formats, probePath);
    else
        std::printf ("\n(SGTM Host Probe not built; skipped)\n");

#if ! JUCE_WINDOWS
    shm_unlink (linkName.toRawUTF8());
#endif

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASS" : "SOME TESTS FAILED", failures,
                 failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
