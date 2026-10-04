// Unit tests for the automix engine. No framework: prints each check and exits non-zero on failure.

#include "AutomixEngine.h"
#include "InstanceLink.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <sys/wait.h>
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
        // Peers that stop processing drop out after a second; a running peer's metadata is visible.
        LinkTestName shm;
        sgtm::InstanceLink a (shm.name), b (shm.name);
        a.join();
        b.join();
        b.setLabel ("Pastor");
        const int64_t t = 5'000'000'000;
        b.beginBlock (t, false, 0, 64);
        b.setDisplay (-30.0f, -3.0f, -33.0f, 2.0f);
        a.beginBlock (t + 1000, false, 0, 64);
        const auto list = a.getChannels (t + 1000);
        const bool sawB = list.size() == 2 && list[1].label == "Pastor" && list[1].weightDb == 2.0f && ! list[1].isSelf;
        check (a.getNumPeers() == 1 && sawB, "a running peer is counted and its name and levels are visible");
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
        check (near (g[0], -4.77, 0.1), "silence: gain shared equally (-10log10(3))", g[0], -4.77);
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
