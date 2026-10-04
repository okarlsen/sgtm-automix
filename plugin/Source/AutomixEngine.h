#pragma once

// SGTM Automix engine: the gain-sharing law and the per-channel level detector.
//
// Plain C++ with no JUCE dependency so it can be unit-tested on its own. The same code runs
// live and during offline bounces: the output depends only on the input samples, the
// parameters and the peer levels for the same sample positions, never on block size or
// wall-clock time.
//
// The law: every channel is attenuated by the number of dB its
// level sits below the level of the sum of all channels. With uncorrelated inputs the level of
// the sum is the sum of the powers, so the amplitude gain of channel i is
//
//     gain_i = sqrt(P_i / sum_j P_j)
//
// where P is a smoothed, weighted mean-square level. The gains then always add up (in power)
// to one open channel: N equal channels each get -10*log10(N) dB, a channel 20 dB above the
// rest gets ~0 dB and the rest ~-20 dB. No thresholds, no look-ahead, zero latency.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sgtm
{

//==================================================================================================
// Detector and smoothing settings. Starting points, to be tuned by ear.
struct EngineSettings
{
    // Levels are computed and exchanged once per hop. The hop grid is aligned to absolute sample
    // positions, so every instance in a session agrees on which samples belong to which hop.
    static constexpr int hopSize = 16;

    double attackMs = 5.0;
    double releaseMs = 200.0;

    // Added to every channel's power so digital silence still shares gain equally
    // (N silent channels each get -10*log10(N) dB). -100 dBFS.
    double powerFloor = 1.0e-10;
};

//==================================================================================================
// Where peer levels come from. Thread 3 implements this with the shared-memory link; until then
// a channel runs solo (no peers), which makes its automix gain exactly 1.
class PeerLevels
{
public:
    virtual ~PeerLevels() = default;

    // Called once per hop with this channel's smoothed, weighted power for that hop.
    virtual void publish (int64_t hopIndex, double power) noexcept = 0;

    // Sum of every other channel's power for the same hop.
    virtual double sumOfPeerPowers (int64_t hopIndex) noexcept = 0;
};

//==================================================================================================
// Amplitude gain for one channel, given its power and the sum of the others' powers.
inline double automixGain (double ownPower, double peerPowerSum) noexcept
{
    const double total = ownPower + peerPowerSum;
    return total > 0.0 ? std::sqrt (ownPower / total) : 1.0;
}

inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }
inline double gainToDb (double gain, double floorDb = -120.0) noexcept
{
    return gain > 0.0 ? std::max (floorDb, 20.0 * std::log10 (gain)) : floorDb;
}

//==================================================================================================
// One automix channel: one plugin instance on one vocal track (mono or stereo).
class AutomixChannel
{
public:
    void prepare (double sampleRate, const EngineSettings& newSettings = {})
    {
        settings = newSettings;
        const double hopsPerSecond = sampleRate / EngineSettings::hopSize;
        attackCoeff = std::exp (-1000.0 / (settings.attackMs * hopsPerSecond));
        releaseCoeff = std::exp (-1000.0 / (settings.releaseMs * hopsPerSecond));
        reset();
    }

    void reset() noexcept
    {
        hopAccumulator = 0.0;
        hopFill = 0;
        expectedNextSample = 0;
        smoothedPower = settings.powerFloor;
        currentGain = targetGain = 1.0;
        gainStep = 0.0;
        lastInputPower = 0.0;
    }

    // Weight scales the detector input only, never the audio (sets priority between channels).
    void setWeightDb (double weightDb) noexcept { weightPower = std::pow (10.0, weightDb / 10.0); }

    // Applies the automix gain in place. startSample is the absolute sample position of
    // buffer[..][0]; hop boundaries fall on multiples of hopSize. peers may be null (solo).
    //
    // The gain computed from hop k is ramped in across hop k+1: the gain lags by one hop
    // (16 samples), which keeps the audio path free of look-ahead and the reported latency at 0.
    void process (float* const* channels, int numChannels, int numSamples,
                  int64_t startSample, PeerLevels* peers) noexcept
    {
        if (numChannels <= 0)
            return;

        const double channelNorm = 1.0 / numChannels;
        hopFill = static_cast<int> (positiveModulo (startSample, EngineSettings::hopSize));

        // Transport jumped (locate, loop): drop the partial hop. If the jump lands mid-hop, that
        // first hop is measured over fewer samples, which the smoothing absorbs.
        if (startSample != expectedNextSample)
            hopAccumulator = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            double power = 0.0;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const double x = channels[ch][i];
                power += x * x;
            }
            hopAccumulator += power * channelNorm;

            currentGain += gainStep;
            const auto g = static_cast<float> (currentGain);
            for (int ch = 0; ch < numChannels; ++ch)
                channels[ch][i] *= g;

            if (++hopFill == EngineSettings::hopSize)
            {
                const int64_t hopIndex = floorDiv (startSample + i, EngineSettings::hopSize);
                endHop (hopIndex, peers);
            }
        }

        expectedNextSample = startSample + numSamples;
    }

    double getSmoothedPower() const noexcept { return smoothedPower; }
    double getLastInputPower() const noexcept { return lastInputPower; }
    double getCurrentGain() const noexcept { return currentGain; }

private:
    void endHop (int64_t hopIndex, PeerLevels* peers) noexcept
    {
        lastInputPower = hopAccumulator / EngineSettings::hopSize;
        const double weighted = lastInputPower * weightPower;

        // Asymmetric one-pole smoothing in the log domain: fast attack, slow release.
        const double logIn = std::log (weighted + settings.powerFloor);
        const double logState = std::log (smoothedPower);
        const double coeff = logIn > logState ? attackCoeff : releaseCoeff;
        smoothedPower = std::exp (logIn + coeff * (logState - logIn));

        double peerSum = 0.0;
        if (peers != nullptr)
        {
            peers->publish (hopIndex, smoothedPower);
            peerSum = peers->sumOfPeerPowers (hopIndex);
        }

        targetGain = automixGain (smoothedPower, peerSum);
        gainStep = (targetGain - currentGain) / EngineSettings::hopSize;

        hopAccumulator = 0.0;
        hopFill = 0;
    }

    static int64_t floorDiv (int64_t a, int64_t b) noexcept
    {
        return a >= 0 ? a / b : -((-a + b - 1) / b);
    }

    static int64_t positiveModulo (int64_t a, int64_t b) noexcept
    {
        const int64_t m = a % b;
        return m < 0 ? m + b : m;
    }

    EngineSettings settings;
    double attackCoeff = 0.0, releaseCoeff = 0.0;
    double weightPower = 1.0;

    double hopAccumulator = 0.0;
    int hopFill = 0;
    int64_t expectedNextSample = 0;

    double smoothedPower = 1.0e-10;
    double lastInputPower = 0.0;
    double currentGain = 1.0, targetGain = 1.0, gainStep = 0.0;
};

} // namespace sgtm
