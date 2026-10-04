// Unit tests for the automix engine. No framework: prints each check and exits non-zero on failure.

#include "AutomixEngine.h"

#include <cstdio>
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

// In-memory stand-in for the thread-3 instance link. Channels are processed one after another, so
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

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL TESTS PASS" : "SOME TESTS FAILED", failures,
                 failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
