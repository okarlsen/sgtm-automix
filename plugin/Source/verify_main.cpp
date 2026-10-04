// Unit tests for the automix engine. No framework: prints each check and exits non-zero on failure.

#include "AutomixEngine.h"
#include "InstanceLink.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <map>
#include <random>
#include <vector>

namespace
{
int failures = 0;

void check (bool ok, const char* what, double got = 0.0, double want = 0.0)
{
    std::printf ("%s  %s", ok ? "PASS" : "FAIL", what);
    if (got != 0.0 || want != 0.0)
        std::printf ("  (got %.4f, want %.4f)", got, want);
    std::printf ("\n");
    if (! ok)
        ++failures;
}

bool near (double a, double b, double tol) { return std::abs (a - b) <= tol; }

// In-memory stand-in for the instance link. Channels are processed one after another, so
// when a peer has not published a hop yet the newest value it did publish is used (the live rule).
struct FakeLink
{
    std::map<int64_t, std::vector<double>> powers; // hop -> power per channel (-1 = not yet)
    std::vector<double> newest;
    int numChannels = 0;

    struct Endpoint final : sgtm::PeerLevels
    {
        FakeLink* link = nullptr;
        int index = 0;

        void publish (int64_t hop, double power) noexcept override
        {
            auto& v = link->powers[hop];
            v.resize ((size_t) link->numChannels, -1.0);
            v[(size_t) index] = power;
            link->newest.resize ((size_t) link->numChannels, 0.0);
            link->newest[(size_t) index] = power;
        }

        double sumOfPeerPowers (int64_t hop) noexcept override
        {
            double sum = 0.0;
            const auto& v = link->powers[hop];
            for (int i = 0; i < link->numChannels; ++i)
                if (i != index)
                    sum += (size_t) i < v.size() && v[(size_t) i] >= 0.0 ? v[(size_t) i]
                                                                        : link->newest[(size_t) i];
            return sum;
        }
    };
};

struct LinkTestName
{
    std::string name = "/sgtm-am-test-" + std::to_string ((long) getpid());
    LinkTestName() { sgtm::InstanceLink::unlinkShared (name); }
    ~LinkTestName() { sgtm::InstanceLink::unlinkShared (name); }
};

std::vector<float> noise (int n, double rmsDb, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> dist (0.0f, (float) sgtm::dbToGain (rmsDb));
    std::vector<float> v ((size_t) n);
    for (auto& x : v)
        x = dist (rng);
    return v;
}

// Runs N mono channels in lockstep through a FakeLink and returns each channel's final gain in dB.
std::vector<double> runGroup (const std::vector<double>& levelsDb, const std::vector<double>& weightsDb = {})
{
    const int n = (int) levelsDb.size();
    const double sr = 48000.0;
    const int total = 48000; // 1 s, long enough for the 200 ms release to settle

    FakeLink link;
    link.numChannels = n;
    link.newest.assign ((size_t) n, 0.0);
    std::vector<FakeLink::Endpoint> ends ((size_t) n);
    std::vector<sgtm::AutomixChannel> chans ((size_t) n);
    std::vector<std::vector<float>> audio;

    for (int i = 0; i < n; ++i)
    {
        ends[(size_t) i].link = &link;
        ends[(size_t) i].index = i;
        chans[(size_t) i].prepare (sr);
        chans[(size_t) i].setWeightDb (weightsDb.empty() ? 0.0 : weightsDb[(size_t) i]);
        audio.push_back (noise (total, levelsDb[(size_t) i], 1234u + (unsigned) i));
    }

    // Lockstep in hop-sized pieces so every channel publishes a hop before any reads it.
    for (int pos = 0; pos < total; pos += sgtm::EngineSettings::hopSize)
        for (int i = 0; i < n; ++i)
        {
            float* p = audio[(size_t) i].data() + pos;
            chans[(size_t) i].process (&p, 1, sgtm::EngineSettings::hopSize, pos, &ends[(size_t) i]);
        }

    std::vector<double> gains;
    for (auto& c : chans)
        gains.push_back (sgtm::gainToDb (c.getCurrentGain()));
    return gains;
}

//==================================================================================================
// Instance link tests.

constexpr double linkRate = 48000.0;
constexpr int64_t nsPerSample = 1'000'000'000 / 48000;

struct LinkedChannel
{
    std::unique_ptr<sgtm::InstanceLink> link;
    sgtm::AutomixChannel engine;
    std::vector<float> audio;
};

std::vector<LinkedChannel> makeLinkedChannels (const std::string& name, const std::vector<double>& levelsDb,
                                              int total, const std::vector<double>& weightsDb = {},
                                              const std::vector<int>& groups = {})
{
    std::vector<LinkedChannel> chans (levelsDb.size());
    for (size_t i = 0; i < chans.size(); ++i)
    {
        chans[i].link = std::make_unique<sgtm::InstanceLink> (name);
        chans[i].link->prepare (linkRate);
        chans[i].link->setGroup (groups.empty() ? 0 : groups[i], true);
        chans[i].link->join();
        chans[i].engine.prepare (linkRate);
        chans[i].engine.setWeightDb (weightsDb.empty() ? 0.0 : weightsDb[i]);
        chans[i].audio = noise (total, levelsDb[i], 1234u + (unsigned) i);
    }
    return chans;
}

void processBlock (LinkedChannel& c, int64_t start, int n, bool offline, int64_t nowNs, int64_t offset = 0)
{
    float* p = c.audio.data() + (start - offset);
    c.engine.setBypassed (c.engine.isBypassed()); // the plugin sets bypass every block
    c.link->beginBlock (nowNs, offline, start, n);
    c.engine.process (&p, 1, n, start, c.link.get());
}

// Live: channels take turns block by block in one thread, like a host's audio callback.
std::vector<double> runLiveGroup (const std::string& name, const std::vector<double>& levelsDb,
                                  const std::vector<double>& weightsDb = {}, int block = 64,
                                  const std::vector<int>& groups = {})
{
    const int total = 48000;
    auto chans = makeLinkedChannels (name, levelsDb, total, weightsDb, groups);
    for (int pos = 0; pos + block <= total; pos += block)
        for (auto& c : chans)
            processBlock (c, pos, block, false, 1'000'000'000 + pos * nsPerSample);

    std::vector<double> gains;
    for (auto& c : chans)
        gains.push_back (sgtm::gainToDb (c.engine.getCurrentGain()));
    return gains;
}

// Exact reference for offline rendering: every channel sees every peer's power for the same hop.
// A channel's smoothed power depends only on its own input, so record it solo first.
struct OracleLink final : sgtm::PeerLevels
{
    const std::vector<std::vector<double>>* powers = nullptr;
    const std::vector<int>* groups = nullptr;
    size_t self = 0;
    int64_t firstHop = 0;
    void publish (int64_t, double) noexcept override {}
    double sumOfPeerPowers (int64_t hop) noexcept override
    {
        double sum = 0.0;
        for (size_t i = 0; i < powers->size(); ++i)
            if (i != self && (groups->empty() || (*groups)[i] == (*groups)[self]))
                sum += (*powers)[i][(size_t) (hop - firstHop)];
        return sum;
    }
};

std::vector<std::vector<float>> referenceRender (const std::vector<double>& levelsDb, int total, int64_t offset,
                                                 const std::vector<int>& groups = {})
{
    const size_t n = levelsDb.size();
    std::vector<std::vector<double>> powers (n);
    std::vector<std::vector<float>> out (n);
    const int64_t firstHop = offset / sgtm::EngineSettings::hopSize;

    for (size_t i = 0; i < n; ++i)
    {
        struct Recorder final : sgtm::PeerLevels
        {
            std::vector<double>* v = nullptr;
            void publish (int64_t, double p) noexcept override { v->push_back (p); }
            double sumOfPeerPowers (int64_t) noexcept override { return 0.0; }
        } rec;
        rec.v = &powers[i];
        sgtm::AutomixChannel c;
        c.prepare (linkRate);
        auto a = noise (total, levelsDb[i], 1234u + (unsigned) i);
        float* p = a.data();
        c.process (&p, 1, total, offset, &rec);
    }

    for (size_t i = 0; i < n; ++i)
    {
        OracleLink oracle;
        oracle.powers = &powers;
        oracle.groups = &groups;
        oracle.self = i;
        oracle.firstHop = firstHop;
        sgtm::AutomixChannel c;
        c.prepare (linkRate);
        out[i] = noise (total, levelsDb[i], 1234u + (unsigned) i);
        float* p = out[i].data();
        c.process (&p, 1, total, offset, &oracle);
    }
    return out;
}

double maxDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    double d = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        d = std::max (d, (double) std::abs (a[i] - b[i]));
    return d;
}

// Offline bounce: each channel renders on its own thread with its own block sizes and jitter,
// starting at a timeline position that is not on the hop grid.
void runOfflineThreads (std::vector<LinkedChannel>& chans, int total, int64_t offset, unsigned seed)
{
    std::vector<std::thread> threads;
    for (size_t i = 0; i < chans.size(); ++i)
        threads.emplace_back ([&, i]
        {
            std::mt19937 rng (seed + (unsigned) i);
            std::uniform_int_distribution<int> blockDist (1, 1500);
            std::uniform_int_distribution<int> jitter (0, 9);
            int64_t pos = offset;
            while (pos < offset + total)
            {
                const int n = (int) std::min<int64_t> (blockDist (rng), offset + total - pos);
                processBlock (chans[i], pos, n, true, sgtm::InstanceLink::steadyNowNs(), offset);
                pos += n;
                if (jitter (rng) == 0)
                    std::this_thread::sleep_for (std::chrono::microseconds (200));
            }
        });
    for (auto& t : threads)
        t.join();
}

void linkTests()
{
    std::printf ("\nInstance link\n");

    {
        LinkTestName shm;
        sgtm::InstanceLink link (shm.name);
        check (link.isAvailable() && link.join(), "shared block opens and a slot is claimed");

        sgtm::AutomixChannel c;
        c.prepare (linkRate);
        auto input = noise (20000, -20.0, 3u);
        auto out = input;
        for (int pos = 0; pos < 20000; pos += 100)
        {
            float* p = out.data() + pos;
            link.beginBlock (1'000'000'000 + pos * nsPerSample, false, pos, 100);
            c.process (&p, 1, 100, pos, &link);
        }
        check (out == input && link.getNumPeers() == 0, "lone linked instance is bit-exact pass-through");
    }

    {
        // A block left behind by a build with another layout must not stop this build linking.
        const auto name = sgtm::InstanceLink::defaultName();
        check (name.find (std::to_string (sizeof (sgtm::linkdetail::Shared))) != std::string::npos && name.size() <= 31,
               "the shared block's name carries the layout, so older builds' blocks are never reused");

        LinkTestName shm;
        {
            const int fd = shm_open (shm.name.c_str(), O_RDWR | O_CREAT, 0600);
            ftruncate (fd, (off_t) sizeof (sgtm::linkdetail::Shared));
            auto* old = static_cast<uint32_t*> (mmap (nullptr, 8, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
            old[0] = sgtm::linkdetail::magic;
            old[1] = 12345; // another layout
            munmap (old, 8);
            close (fd);
        }
        sgtm::InstanceLink stale (shm.name);
        check (! stale.isAvailable(), "a block with another layout is refused, not misread");
    }

    {
        LinkTestName shm;
        auto g = runLiveGroup (shm.name, { -40, -40, -40, -40 });
        for (int i = 0; i < 4; ++i)
            check (near (g[(size_t) i], -6.02, 0.5), "live, 4 equal mics: each about -6 dB", g[(size_t) i], -6.02);
    }
    {
        LinkTestName shm;
        auto g = runLiveGroup (shm.name, { -20, -40, -40, -40 }, {}, 512);
        check (near (g[0], -0.13, 0.5), "live, 1 mic 20 dB louder: about 0 dB (512 blocks)", g[0], -0.13);
        check (near (g[1], -20.1, 1.0), "live, the others: about -20 dB", g[1], -20.1);
    }
    {
        LinkTestName shm;
        auto g = runLiveGroup (shm.name, { -40, -40 }, { 6.0, 0.0 });
        check (near (g[0], -0.97, 0.5), "live, weight +6 dB raises that channel's share", g[0], -0.97);
        check (near (g[1], -6.99, 0.5), "live, and lowers the other's", g[1], -6.99);
    }

    {
        LinkTestName shm;
        const std::vector<double> levels { -20, -30, -40, -26 };
        const int total = 96000;
        const int64_t offset = 4803; // not on the hop grid
        auto reference = referenceRender (levels, total, offset);

        auto chans = makeLinkedChannels (shm.name, levels, total);

        // The host's engine has been running live before the bounce; then it resets the plugins.
        for (int pos = 0; pos < 4800; pos += 480)
            for (auto& c : chans)
            {
                auto scratch = noise (480, -30.0, 5u);
                float* p = scratch.data();
                c.link->beginBlock (sgtm::InstanceLink::steadyNowNs(), false, pos, 480);
                c.engine.process (&p, 1, 480, pos, c.link.get());
            }
        for (auto& c : chans)
            c.engine.prepare (linkRate);

        runOfflineThreads (chans, total, offset, 99u);
        double worst = 0.0;
        int64_t timeouts = 0;
        for (size_t i = 0; i < chans.size(); ++i)
        {
            worst = std::max (worst, maxDiff (chans[i].audio, reference[i]));
            timeouts += chans[i].link->getWaitTimeouts();
        }
        check (worst == 0.0 && timeouts == 0,
               "offline, 4 threads, random blocks and timing: identical to exact reference", worst, 0.0);

        // Second bounce over the same range: must not pick up values from the first pass.
        for (size_t i = 0; i < chans.size(); ++i)
            chans[i].audio = noise (total, levels[i], 1234u + (unsigned) i);
        for (auto& c : chans)
            c.engine.prepare (linkRate);
        runOfflineThreads (chans, total, offset, 7u);
        worst = 0.0;
        for (size_t i = 0; i < chans.size(); ++i)
            worst = std::max (worst, maxDiff (chans[i].audio, reference[i]));
        check (worst == 0.0, "offline, second bounce of the same range: identical again", worst, 0.0);
    }

    {
        // A host that renders tracks one after another on a single thread: must not stall, and
        // still converge (peers are then one block behind, like live).
        LinkTestName shm;
        const std::vector<double> levels { -40, -40, -40, -40 };
        const int total = 480000; // 10 s
        auto chans = makeLinkedChannels (shm.name, levels, total);
        const auto t0 = std::chrono::steady_clock::now();
        for (int pos = 0; pos + 512 <= total; pos += 512)
            for (auto& c : chans)
                processBlock (c, pos, 512, true, sgtm::InstanceLink::steadyNowNs());
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        check (ms < 2000.0, "offline, tracks rendered serially on one thread: no stall (ms for 10 s x 4)", ms, 2000.0);
        check (near (sgtm::gainToDb (chans[0].engine.getCurrentGain()), -6.02, 0.5),
               "offline serial: gains still about -6 dB", sgtm::gainToDb (chans[0].engine.getCurrentGain()), -6.02);
    }

    {
        LinkTestName shm;
        auto g = runLiveGroup (shm.name, { -40, -40, -40, -40 }, {}, 64, { 0, 0, 1, 1 });
        check (near (g[0], -3.01, 0.5) && near (g[1], -3.01, 0.5), "groups: two equal mics in A, about -3 dB each",
               g[0], -3.01);
        check (near (g[2], -3.01, 0.5) && near (g[3], -3.01, 0.5), "and two in B, independently -3 dB each",
               g[2], -3.01);
    }
    {
        LinkTestName shm;
        auto g = runLiveGroup (shm.name, { -40, -40, -40 }, {}, 64, { 0, 0, 2 });
        check (near (g[2], 0.0, 1e-9), "a lone channel in group C stays at unity", g[2], 0.0);
        check (near (g[0], -3.01, 0.5), "while the two in A share", g[0], -3.01);
    }
    {
        // Moving a channel from B to A while playing: gains change smoothly, then 3 share.
        LinkTestName shm;
        auto chans = makeLinkedChannels (shm.name, { -40, -40, -40 }, 96000, {}, { 0, 0, 1 });
        double biggestStep = 0.0;
        std::vector<double> previous (3, 1.0);
        for (int pos = 0; pos + 64 <= 96000; pos += 64)
        {
            if (pos == 48000)
                chans[2].link->setGroup (0);
            for (auto& c : chans)
                processBlock (c, pos, 64, false, 1'000'000'000 + pos * nsPerSample);
            for (size_t i = 0; i < 3; ++i)
            {
                const double gain = chans[i].engine.getCurrentGain();
                if (pos > 48000)
                    biggestStep = std::max (biggestStep, std::abs (gain - previous[i]));
                previous[i] = gain;
            }
        }
        const double g2 = sgtm::gainToDb (chans[2].engine.getCurrentGain());
        check (near (g2, -4.77, 0.5) && chans[2].link->getNumPeers() == 2, "moved channel ends sharing with A's two",
               g2, -4.77);
        // An instant switch would jump about 0.42 in one block; the 20 ms fade spreads it over ~15.
        check (biggestStep < 0.1, "moving between groups fades (largest gain change per 64 samples)", biggestStep, 0.1);
    }
    {
        // Offline bounce with two groups at once, matching an exact per-group reference.
        LinkTestName shm;
        const std::vector<double> levels { -20, -30, -40, -26 };
        const std::vector<int> groups { 0, 1, 0, 1 };
        const int total = 48000;
        auto reference = referenceRender (levels, total, 0, groups);
        auto chans = makeLinkedChannels (shm.name, levels, total, {}, groups);
        for (auto& c : chans)
            c.link->beginBlock (sgtm::InstanceLink::steadyNowNs(), false, -64, 64);
        runOfflineThreads (chans, total, 0, 31u);
        double worst = 0.0;
        for (size_t i = 0; i < chans.size(); ++i)
            worst = std::max (worst, maxDiff (chans[i].audio, reference[i]));
        check (worst == 0.0, "offline with groups A and B: identical to exact per-group reference", worst, 0.0);
    }

    std::printf ("\nMono and stereo together\n");
    {
        // The same voice on a mono track and on a stereo track (both sides) takes the same share:
        // the detector uses the mean power over the channels, so stereo gets no hidden +3 dB.
        LinkTestName shm;
        auto chans = makeLinkedChannels (shm.name, { -40, -40 }, 48000);
        std::vector<float> right = chans[1].audio;
        int64_t pos = 0;
        for (; pos + 64 <= 48000; pos += 64)
        {
            processBlock (chans[0], pos, 64, false, 1'000'000'000 + pos * nsPerSample);
            float* p[2] = { chans[1].audio.data() + pos, right.data() + pos };
            chans[1].engine.setBypassed (false);
            chans[1].link->beginBlock (1'000'000'000 + pos * nsPerSample, false, pos, 64);
            chans[1].engine.process (p, 2, 64, pos, chans[1].link.get());
        }
        const double gm = sgtm::gainToDb (chans[0].engine.getCurrentGain());
        const double gs = sgtm::gainToDb (chans[1].engine.getCurrentGain());
        check (near (gm, -3.01, 0.5) && near (gs, -3.01, 0.5), "equal voices on a mono and a stereo channel: -3 dB each",
               gs, gm);

        // The stereo channel's track turns mono (same voice): its share does not jump.
        double biggestStep = 0.0, previous = chans[1].engine.getCurrentGain();
        auto more0 = noise (48000, -40.0, 900u), more1 = noise (48000, -40.0, 901u);
        for (int k = 0; k + 64 <= 48000; k += 64, pos += 64)
        {
            float* a = more0.data() + k;
            chans[0].link->beginBlock (1'000'000'000 + pos * nsPerSample, false, pos, 64);
            chans[0].engine.process (&a, 1, 64, pos, chans[0].link.get());
            float* b = more1.data() + k;
            chans[1].link->beginBlock (1'000'000'000 + pos * nsPerSample, false, pos, 64);
            chans[1].engine.process (&b, 1, 64, pos, chans[1].link.get());
            biggestStep = std::max (biggestStep, std::abs (chans[1].engine.getCurrentGain() - previous));
            previous = chans[1].engine.getCurrentGain();
        }
        check (biggestStep < 0.05 && near (sgtm::gainToDb (previous), -3.01, 0.5),
               "a channel switching from stereo to mono keeps its share without a jump", biggestStep, 0.05);
    }

    std::printf ("\nFade-up\n");
    {
        // Ten open mics at room tone; one starts talking at -20 dBFS. With the 15 ms attack it
        // should reach within 1 dB of full gain in roughly 7 ms (about 3 ms with the old 5 ms).
        LinkTestName shm;
        const int total = 48000 * 3, onset = 48000 * 2;
        auto chans = makeLinkedChannels (shm.name, std::vector<double> (10, -71.4), total);
        auto talk = noise (total - onset, -20.0, 4242u);
        std::copy (talk.begin(), talk.end(), chans[0].audio.begin() + onset);
        double reachedMs = -1.0;
        for (int pos = 0; pos + 64 <= total; pos += 64)
        {
            for (auto& c : chans)
                processBlock (c, pos, 64, false, 1'000'000'000 + pos * nsPerSample);
            if (reachedMs < 0.0 && pos >= onset && sgtm::gainToDb (chans[0].engine.getCurrentGain()) >= -1.0)
                reachedMs = (pos + 64 - onset) / 48.0;
        }
        check (reachedMs > 4.0 && reachedMs < 12.0, "a lone talker reaches within 1 dB of full gain (ms)", reachedMs, 7.0);
    }

    std::printf ("\nSignal presence\n");
    {
        LinkTestName shm;
        auto g = runLiveGroup (shm.name, std::vector<double> (10, -71.4));
        check (near (g[0], -10.0, 0.6) && near (g[9], -10.0, 0.6),
               "10 open mics at room tone (-71.4 dBFS): each about -10 dB", g[0], -10.0);
    }
    {
        LinkTestName shm;
        auto g = runLiveGroup (shm.name, { -40, -40, -110 });
        check (near (g[0], -3.01, 0.5) && near (g[1], -3.01, 0.5),
               "a channel with its fader down does not dilute the others (-3 dB, not -4.8)", g[0], -3.01);
        check (near (g[2], 0.0, 1e-9), "and sits at unity itself", g[2], 0.0);
    }
    {
        // A channel wakes up: fader comes up from silence. Smooth, then shares with the others.
        LinkTestName shm;
        auto chans = makeLinkedChannels (shm.name, { -40, -40, -40 }, 96000);
        for (int i = 0; i < 48000; ++i)
            chans[2].audio[(size_t) i] = 0.0f;
        double biggestStep = 0.0, previous = 1.0;
        for (int pos = 0; pos + 64 <= 96000; pos += 64)
        {
            for (auto& c : chans)
                processBlock (c, pos, 64, false, 1'000'000'000 + pos * nsPerSample);
            const double gain = chans[0].engine.getCurrentGain();
            if (pos >= 48000)
                biggestStep = std::max (biggestStep, std::abs (gain - previous));
            previous = gain;
            if (pos == 47936)
                check (! chans[2].engine.isPresent() && near (sgtm::gainToDb (gain), -3.01, 0.5),
                       "while silent, the other two share as a pair", sgtm::gainToDb (gain), -3.01);
        }
        check (chans[2].engine.isPresent() && near (sgtm::gainToDb (chans[2].engine.getCurrentGain()), -4.77, 0.6),
               "after waking up, all three share", sgtm::gainToDb (chans[2].engine.getCurrentGain()), -4.77);
        check (biggestStep < 0.05, "the others fade down as it joins (largest change per 64 samples)", biggestStep, 0.05);
    }
    {
        // Hysteresis: a dip to between the thresholds keeps the channel in; a drop below the lower
        // threshold takes it out only after the 1 s hold.
        sgtm::AutomixChannel c;
        c.prepare (linkRate);
        auto run = [&] (double db, int samples)
        {
            auto a = noise (samples, db, 77u);
            for (int pos = 0; pos + 64 <= samples; pos += 64)
            {
                float* p = a.data() + pos;
                c.process (&p, 1, 64, 0, nullptr);
            }
        };
        run (-60.0, 24000);
        const bool inAtFirst = c.isPresent();
        run (-78.0, 96000);
        const bool stillInBetween = c.isPresent();
        run (-95.0, 24000);
        const bool inDuringHold = c.isPresent();
        run (-95.0, 72000);
        check (inAtFirst && stillInBetween, "a level between the two thresholds keeps the channel in");
        check (inDuringHold && ! c.isPresent(), "below the lower threshold it leaves after the 1 s hold");
    }

    {
        // Bypass on one of two channels: it fades to unity and out of the group, so the other
        // channel is alone and also goes to unity. No step larger than the fade allows.
        LinkTestName shm;
        auto chans = makeLinkedChannels (shm.name, { -30, -30 }, 96000);
        double biggestStep = 0.0, previous = 1.0;
        for (int pos = 0; pos + 64 <= 96000; pos += 64)
        {
            if (pos == 48000)
                chans[0].engine.setBypassed (true);
            for (auto& c : chans)
                processBlock (c, pos, 64, false, 1'000'000'000 + pos * nsPerSample);
            const double g = chans[0].engine.getCurrentGain();
            if (pos > 48000)
                biggestStep = std::max (biggestStep, std::abs (g - previous));
            previous = g;
        }
        check (near (sgtm::gainToDb (chans[0].engine.getCurrentGain()), 0.0, 1e-6), "bypassed channel ends at unity",
               sgtm::gainToDb (chans[0].engine.getCurrentGain()), 0.0);
        check (near (sgtm::gainToDb (chans[1].engine.getCurrentGain()), 0.0, 0.1),
               "the other channel is then alone: unity", sgtm::gainToDb (chans[1].engine.getCurrentGain()), 0.0);
        check (biggestStep < 0.05, "bypass fades in (largest gain change per 64-sample block)", biggestStep, 0.05);
    }

    {
        // The all-channels switch is shared, and a new session starts with the automix on.
        LinkTestName shm;
        sgtm::InstanceLink a (shm.name), b (shm.name);
        a.join();
        b.join();
        a.setAutomixOn (false);
        check (! b.isAutomixOn(), "switching the automix off on one instance switches it off on all");
        a.leave();
        b.leave();
        sgtm::InstanceLink c (shm.name);
        c.join();
        check (c.isAutomixOn(), "a new session starts with the automix on");
    }

    {
        // A slot whose previous owner died mid-way through writing its name (odd sequence) is
        // reset when the slot is taken again, so the new owner's name can be read.
        LinkTestName shm;
        auto a = std::make_unique<sgtm::InstanceLink> (shm.name);
        a->join();
        {
            const int fd = shm_open (shm.name.c_str(), O_RDWR, 0600);
            auto* shared = static_cast<sgtm::linkdetail::Shared*> (
                mmap (nullptr, sizeof (sgtm::linkdetail::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
            close (fd);
            shared->slots[a->getSlotIndex()].labelSeq.store (31097); // stuck mid-write
            munmap (shared, sizeof (sgtm::linkdetail::Shared));
        }
        a.reset();
        sgtm::InstanceLink b (shm.name), reader (shm.name);
        b.join();
        reader.join();
        b.setLabel ("Vocal L");
        b.beginBlock (sgtm::InstanceLink::steadyNowNs(), false, 0, 64);
        bool found = false;
        for (const auto& c : reader.getChannels())
            found = found || c.label == "Vocal L";
        check (found, "a name half-written by a crashed owner does not hide the next owner's name");
    }

    {
        // Peers that stop processing drop out after a second; a running peer's metadata is visible.
        LinkTestName shm;
        sgtm::InstanceLink a (shm.name), b (shm.name);
        a.join();
        b.join();
        b.setLabel ("Pastor");
        const int64_t t = 5'000'000'000;
        b.beginBlock (t, false, 0, 64);
        b.setDisplay (-30.0f, -3.0f, -33.0f, 2.0f);
        b.setInputSides (-28.0f, -34.0f, 2);
        a.beginBlock (t + 1000, false, 0, 64);
        const auto list = a.getChannels (t + 1000);
        const bool sawB = list.size() == 2 && list[1].label == "Pastor" && list[1].weightDb == 2.0f && ! list[1].isSelf
                          && list[1].stereo && list[1].inputLeftDb == -28.0f && list[1].inputRightDb == -34.0f
                          && ! list[0].stereo;
        check (a.getNumPeers() == 1 && sawB, "a running peer is counted and its name, levels and stereo input sides are visible");
        a.beginBlock (t + 1'500'000'000, false, 64, 64);
        const auto later = a.getChannels (t + 1'500'000'000);
        check (a.getNumPeers() == 0, "a peer that stops processing drops out of the sharing after 1 s");
        check (later.size() == 2 && later[1].idle && ! later[0].idle,
               "and is still listed, as idle, while its instance is loaded");
        b.leave();
        a.beginBlock (t + 1'500'001'000, false, 128, 64);
        check (a.getNumPeers() == 0 && a.getChannels (t + 1'500'001'000).size() == 1, "a peer that leaves is gone");
    }

    {
        // Another process joins and publishes, then dies without leaving (a crash).
        LinkTestName shm;
        const pid_t child = fork();
        if (child == 0)
        {
            sgtm::InstanceLink link (shm.name);
            link.join();
            link.setLabel ("Other process");
            for (int i = 0; i < 400; ++i)
            {
                link.beginBlock (sgtm::InstanceLink::steadyNowNs(), false, i * 64, 64);
                link.publish (i * 4, 0.01);
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            _exit (0); // no leave(): the slot stays claimed, as after a crash
        }

        sgtm::InstanceLink a (shm.name);
        a.join();
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        a.beginBlock (sgtm::InstanceLink::steadyNowNs(), false, 0, 64);
        const double peerPower = a.sumOfPeerPowers (0);
        check (a.getNumPeers() == 1 && near (peerPower, 0.01, 1e-12), "a peer in another process is seen");
        int status = 0;
        waitpid (child, &status, 0);

        std::this_thread::sleep_for (std::chrono::milliseconds (1100));
        a.beginBlock (sgtm::InstanceLink::steadyNowNs(), false, 64, 64);
        check (a.getNumPeers() == 0 && a.getChannels().size() == 1, "a crashed peer drops out and is not listed");

        std::this_thread::sleep_for (std::chrono::milliseconds (1000));
        std::vector<std::unique_ptr<sgtm::InstanceLink>> more;
        int joined = 0;
        for (int i = 0; i < 63; ++i)
        {
            more.push_back (std::make_unique<sgtm::InstanceLink> (shm.name));
            joined += more.back()->join() ? 1 : 0;
        }
        check (joined == 63, "the crashed process's slot is reclaimed (64 slots all usable)", joined, 63);
        sgtm::InstanceLink extra (shm.name);
        check (! extra.join(), "a 65th instance runs solo instead of failing");
    }
}
} // namespace

int main()
{
    std::printf ("Gain law\n");
    check (near (sgtm::automixGain (1.0, 0.0), 1.0, 1e-12), "solo channel gets unity gain");
    check (near (sgtm::gainToDb (sgtm::automixGain (1.0, 3.0)), -6.0206, 1e-3),
           "4 equal channels: -6 dB each", sgtm::gainToDb (sgtm::automixGain (1.0, 3.0)), -6.0206);

    std::printf ("\nGroups through the engine (1 s of noise per channel)\n");
    {
        auto g = runGroup ({ -40, -40, -40, -40 });
        for (int i = 0; i < 4; ++i)
            check (near (g[(size_t) i], -6.02, 0.5), "4 equal mics: each about -6 dB", g[(size_t) i], -6.02);
    }
    {
        auto g = runGroup ({ -20, -40, -40, -40 });
        check (near (g[0], -0.13, 0.5), "1 mic 20 dB louder: about 0 dB", g[0], -0.13);
        check (near (g[1], -20.1, 1.0), "the others: about -20 dB", g[1], -20.1);
    }
    {
        auto g = runGroup ({ -20, -20, -40, -40 });
        check (near (g[0], -3.0, 0.5), "2 mics equal, 20 dB above: each about -3 dB", g[0], -3.0);
        check (near (g[2], -23.0, 1.0), "the rest: about -23 dB", g[2], -23.0);
    }
    {
        auto g = runGroup ({ -40, -40 }, { 6.0, 0.0 });
        // Weight +6 dB = 4x power share: 10*log10(4/5) = -0.97 dB, 10*log10(1/5) = -6.99 dB.
        check (near (g[0], -0.97, 0.5), "weight +6 dB raises that channel's share", g[0], -0.97);
        check (near (g[1], -6.99, 0.5), "and lowers the other's", g[1], -6.99);
    }
    {
        auto g = runGroup ({ -200, -200, -200 }); // effectively digital silence
        check (near (g[0], 0.0, 1e-9), "digital silence: no signal, so no share taken and unity gain", g[0], 0.0);
    }

    std::printf ("\nStereo\n");
    {
        // Left only, 0.5 amplitude square wave: left about -6 dB, right silent, combined 3 dB below left.
        std::vector<float> left (480), right (480, 0.0f);
        for (size_t i = 0; i < left.size(); ++i)
            left[i] = (i / 24) % 2 == 0 ? 0.5f : -0.5f;
        const float* ch[2] = { left.data(), right.data() };
        const auto stereo = sgtm::measureLevels (ch, 2, 480);
        check (near (stereo.channelDb[0], -6.02, 0.01) && stereo.channelDb[1] <= -119.0f,
               "stereo meters: left and right measured separately", stereo.channelDb[0], -6.02);
        check (near (stereo.combinedDb, -9.03, 0.01), "combined level is the mean power of both sides",
               stereo.combinedDb, -9.03);
        const auto mono = sgtm::measureLevels (ch, 1, 480);
        check (near (mono.channelDb[0], -6.02, 0.01) && near (mono.combinedDb, -6.02, 0.01), "mono: one level");
    }
    {
        // One linked gain for both sides, from the mean power of both: a stereo channel with
        // signal on the left only gets the same gain as a mono channel carrying that mean power,
        // and both sides are turned down alike, so the image does not shift.
        struct OnePeer final : sgtm::PeerLevels
        {
            double power = std::pow (10.0, -23.0 / 10.0);
            void publish (int64_t, double) noexcept override {}
            double sumOfPeerPowers (int64_t) noexcept override { return power; }
        } peer;

        sgtm::AutomixChannel stereoChannel, monoChannel;
        stereoChannel.prepare (48000.0);
        monoChannel.prepare (48000.0);
        auto l = noise (48000, -20.0, 5u);
        std::vector<float> r (48000, 0.0f);
        std::vector<float> m (l.size());
        for (size_t i = 0; i < l.size(); ++i)
            m[i] = l[i] * (float) std::sqrt (0.5); // same mean power as the stereo pair
        for (int pos = 0; pos + 64 <= 48000; pos += 64)
        {
            float* p[2] = { l.data() + pos, r.data() + pos };
            stereoChannel.process (p, 2, 64, pos, &peer);
            float* q = m.data() + pos;
            monoChannel.process (&q, 1, 64, pos, &peer);
        }
        const double gs = sgtm::gainToDb (stereoChannel.getCurrentGain());
        const double gm = sgtm::gainToDb (monoChannel.getCurrentGain());
        check (near (gs, gm, 0.05), "stereo detection is linked: left-only stereo shares like mono at its mean power", gs, gm);
    }

    std::printf ("\nSolo pass-through and determinism\n");
    {
        const int total = 10000;
        auto input = noise (total, -20.0, 7u);

        auto runWithBlocks = [&] (std::vector<int> blockSizes)
        {
            sgtm::AutomixChannel c;
            c.prepare (44100.0);
            auto out = input;
            int pos = 0, k = 0;
            while (pos < total)
            {
                const int n = std::min (blockSizes[(size_t) (k++ % (int) blockSizes.size())], total - pos);
                float* p = out.data() + pos;
                c.process (&p, 1, n, pos, nullptr);
                pos += n;
            }
            return out;
        };

        auto a = runWithBlocks ({ 512 });
        auto b = runWithBlocks ({ 1, 7, 64, 333, 2048, 13 });

        bool identical = a == b, unity = true;
        for (int i = 0; i < total; ++i)
            unity = unity && a[(size_t) i] == input[(size_t) i];

        check (unity, "solo instance is bit-exact pass-through");
        check (identical, "output does not depend on block size");
    }

    linkTests();

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASS" : "SOME TESTS FAILED", failures,
                 failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
