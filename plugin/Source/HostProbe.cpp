// SGTM Host Probe
//
// A pass-through plugin that logs, per instance, every processBlock call: wall-clock time, the
// calling thread, the host timeline position, block size and the realtime/offline flag. Put one
// on each vocal track, play, record and bounce, then run tools/analyse_probe.py over the logs to
// see how the host schedules tracks against each other.
//
// The audio thread never touches the file: it pushes fixed-size records into a lock-free
// single-producer FIFO, and a background thread writes them out as CSV, one file per instance.

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#if JUCE_WINDOWS
 #include <process.h>
 #define SGTM_GETPID _getpid
#else
 #include <unistd.h>
 #define SGTM_GETPID getpid
#endif

namespace sgtm
{

namespace
{
int64_t wallClockNs()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds> (system_clock::now().time_since_epoch()).count();
}

int64_t steadyNs()
{
    using namespace std::chrono;
    return duration_cast<nanoseconds> (steady_clock::now().time_since_epoch()).count();
}

int processId()
{
    return static_cast<int> (SGTM_GETPID());
}

uint64_t currentThreadId()
{
    return static_cast<uint64_t> (reinterpret_cast<uintptr_t> (juce::Thread::getCurrentThreadId()));
}

juce::File logDirectory()
{
   #if JUCE_MAC
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
        .getChildFile ("Library/Logs/SGTM Host Probe");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("SGTM Host Probe");
   #endif
}

juce::String csvEscape (juce::String s)
{
    return "\"" + s.replace ("\"", "\"\"") + "\"";
}
} // namespace

//==================================================================================================
class ProbeProcessor final : public juce::AudioProcessor
{
public:
    ProbeProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          instanceId (juce::String::toHexString (juce::Random().nextInt64()).paddedLeft ('0', 16)),
          writer (*this)
    {
        const auto started = juce::Time::getCurrentTime();
        logFile = logDirectory().getChildFile (
            "probe_" + started.formatted ("%Y%m%d_%H%M%S") + "_pid"
            + juce::String (processId()) + "_" + instanceId + ".csv");

        juce::PluginHostType host;
        addEvent ("created", "host=" + juce::String (host.getHostDescription())
                                 + " wrapper=" + juce::String (getWrapperTypeDescription (wrapperType))
                                 + " pid=" + juce::String (processId())
                                 + " os=" + juce::SystemStats::getOperatingSystemName());
        writer.startThread (juce::Thread::Priority::low);
    }

    ~ProbeProcessor() override
    {
        addEvent ("destroyed", {});
        writer.stopThread (2000);
        writer.drain(); // flush whatever is left
    }

    //==============================================================================================
    void prepareToPlay (double sampleRate, int maxBlock) override
    {
        setLatencySamples (0);
        currentSampleRate.store (sampleRate);
        localSamples = 0;
        addEvent ("prepare", "sample_rate=" + juce::String (sampleRate)
                                 + " max_block=" + juce::String (maxBlock)
                                 + " channels_in=" + juce::String (getTotalNumInputChannels())
                                 + " channels_out=" + juce::String (getTotalNumOutputChannels())
                                 + " non_realtime=" + juce::String ((int) isNonRealtime()));
    }

    void releaseResources() override { addEvent ("release", {}); }

    void setNonRealtime (bool isNonRealtime) noexcept override
    {
        AudioProcessor::setNonRealtime (isNonRealtime);
        addEvent ("set_non_realtime", juce::String ((int) isNonRealtime));
    }

    void updateTrackProperties (const TrackProperties& properties) override
    {
        trackName = properties.name.value_or (juce::String());
        addEvent ("track", trackName);
    }

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto out = layouts.getMainOutputChannelSet();
        if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
            return false;
        return layouts.getMainInputChannelSet() == out;
    }

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        Record r;
        r.wallNs = wallClockNs();
        r.steadyNs = steadyNs();
        r.thread = currentThreadId();
        r.blockSize = buffer.getNumSamples();
        r.localSamples = localSamples;
        r.nonRealtime = isNonRealtime();

        if (auto* hostPlayHead = getPlayHead())
        {
            if (auto pos = hostPlayHead->getPosition())
            {
                r.hasPosition = true;
                r.timelineSamples = pos->getTimeInSamples().orFallback (-1);
                r.hostTimeNs = static_cast<int64_t> (pos->getHostTimeNs().orFallback (0));
                r.playing = pos->getIsPlaying();
                r.recording = pos->getIsRecording();
                r.looping = pos->getIsLooping();
            }
        }

        localSamples += buffer.getNumSamples();

        // Pass-through: just clear any output channels without an input.
        for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
            buffer.clear (ch, 0, buffer.getNumSamples());

        r.seq = nextSeq++;
        if (! fifo.push (r))
            dropped.fetch_add (1, std::memory_order_relaxed);
    }

    using AudioProcessor::processBlock;

    //==============================================================================================
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
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    //==============================================================================================
    juce::File getLogFile() const { return logFile; }
    juce::String getInstanceId() const { return instanceId; }
    juce::String getTrackName() const { return trackName; }
    uint64_t getBlocksLogged() const noexcept { return written.load(); }
    uint64_t getDropped() const noexcept { return dropped.load(); }

private:
    struct Record
    {
        uint64_t seq = 0;
        int64_t wallNs = 0, steadyNs = 0, hostTimeNs = 0;
        uint64_t thread = 0;
        int64_t timelineSamples = -1, localSamples = 0;
        int32_t blockSize = 0;
        bool hasPosition = false, playing = false, recording = false, looping = false,
             nonRealtime = false;
    };

    // Single-producer (audio thread), single-consumer (writer thread) ring of records.
    class RecordFifo
    {
    public:
        explicit RecordFifo (int capacity) : fifo (capacity), storage ((size_t) capacity) {}

        bool push (const Record& r) noexcept
        {
            const auto scope = fifo.write (1);
            if (scope.blockSize1 + scope.blockSize2 == 0)
                return false;
            storage[(size_t) (scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = r;
            return true;
        }

        template <typename Fn>
        void popAll (Fn&& fn)
        {
            const auto scope = fifo.read (fifo.getNumReady());
            for (int i = 0; i < scope.blockSize1; ++i) fn (storage[(size_t) (scope.startIndex1 + i)]);
            for (int i = 0; i < scope.blockSize2; ++i) fn (storage[(size_t) (scope.startIndex2 + i)]);
        }

    private:
        juce::AbstractFifo fifo;
        std::vector<Record> storage;
    };

    class Writer final : public juce::Thread
    {
    public:
        explicit Writer (ProbeProcessor& p) : Thread ("SGTM probe writer"), owner (p) {}

        void run() override
        {
            while (! threadShouldExit())
            {
                drain();
                wait (100);
            }
        }

        void drain()
        {
            std::lock_guard lock (drainMutex);
            juce::String text;

            if (! headerWritten)
            {
                owner.logFile.getParentDirectory().createDirectory();
                text << "# SGTM Host Probe log, instance " << owner.instanceId << "\n"
                     << "kind,seq,wall_ns,steady_ns,host_time_ns,thread,timeline_samples,"
                        "local_samples,block_size,sample_rate,non_realtime,playing,recording,"
                        "looping,has_position,dropped_total,info\n";
                headerWritten = true;
            }

            {
                std::lock_guard eventLock (owner.eventMutex);
                for (auto& e : owner.pendingEvents)
                    text << e << "\n";
                owner.pendingEvents.clear();
            }

            const double sr = owner.currentSampleRate.load();
            const auto droppedTotal = owner.dropped.load();
            uint64_t count = 0;

            owner.fifo.popAll ([&] (const Record& r)
            {
                text << "block," << (juce::int64) r.seq << "," << r.wallNs << "," << r.steadyNs << ","
                     << r.hostTimeNs << "," << juce::String::toHexString ((juce::int64) r.thread) << ","
                     << r.timelineSamples << "," << r.localSamples << "," << r.blockSize << "," << sr
                     << "," << (int) r.nonRealtime << "," << (int) r.playing << "," << (int) r.recording
                     << "," << (int) r.looping << "," << (int) r.hasPosition << ","
                     << (juce::int64) droppedTotal << ",\n";
                ++count;
            });

            if (text.isNotEmpty())
                owner.logFile.appendText (text, false, false, "\n");

            owner.written.fetch_add (count);
        }

    private:
        ProbeProcessor& owner;
        std::mutex drainMutex;
        bool headerWritten = false;
    };

    // Events from non-audio threads (creation, prepare, track name). Never called on the audio
    // thread except setNonRealtime, which hosts call outside processBlock.
    void addEvent (const juce::String& kind, const juce::String& info)
    {
        std::lock_guard lock (eventMutex);
        pendingEvents.add (kind + ",," + juce::String (wallClockNs()) + "," + juce::String (steadyNs())
                           + ",," + juce::String::toHexString ((juce::int64) currentThreadId())
                           + ",,,,,,,,,,," + csvEscape (info));
    }

    const juce::String instanceId;
    juce::File logFile;
    juce::String trackName;

    RecordFifo fifo { 1 << 16 };
    std::atomic<uint64_t> dropped { 0 }, written { 0 };
    uint64_t nextSeq = 0;
    int64_t localSamples = 0;
    std::atomic<double> currentSampleRate { 0.0 };

    std::mutex eventMutex;
    juce::StringArray pendingEvents;

    Writer writer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProbeProcessor)
};

//==================================================================================================
class ProbeEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit ProbeEditor (ProbeProcessor& p) : AudioProcessorEditor (p), probe (p)
    {
        openButton.onClick = [this] { probe.getLogFile().getParentDirectory().revealToUser(); };
        addAndMakeVisible (openButton);
        addAndMakeVisible (info);
        info.setJustificationType (juce::Justification::topLeft);
        info.setFont (juce::FontOptions (13.0f));
        setSize (440, 170);
        timerCallback();
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff2b2d31));
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        g.drawText ("SGTM Host Probe", getLocalBounds().removeFromTop (32).reduced (12, 0),
                    juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        area.removeFromTop (24);
        openButton.setBounds (area.removeFromBottom (26).removeFromLeft (140));
        info.setBounds (area);
    }

private:
    void timerCallback() override
    {
        info.setText ("Instance: " + probe.getInstanceId()
                          + "\nTrack: " + (probe.getTrackName().isEmpty() ? "(host did not say)" : probe.getTrackName())
                          + "\nBlocks logged: " + juce::String ((juce::int64) probe.getBlocksLogged())
                          + "   dropped: " + juce::String ((juce::int64) probe.getDropped())
                          + "\nLog: " + probe.getLogFile().getFullPathName(),
                      juce::dontSendNotification);
    }

    ProbeProcessor& probe;
    juce::Label info;
    juce::TextButton openButton { "Open log folder" };
};

juce::AudioProcessorEditor* ProbeProcessor::createEditor()
{
    return new ProbeEditor (*this);
}

} // namespace sgtm

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new sgtm::ProbeProcessor();
}
