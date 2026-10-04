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
// rest gets ~0 dB and the rest ~-20 dB. No look-ahead, zero latency.
//
// A channel only takes part while it carries signal. Inserted post-fader, a channel whose fader is
// down (or whose source is unplugged) must not take a share from the others, so below a presence
// threshold it leaves the group, contributes nothing and sits at unity gain. The threshold sits
// well below an open microphone's room tone, so open but quiet mics still count.

#include <algorithm>
#include <array>
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

    // 15 ms (was 5 ms): a slightly softer fade-up on word starts. A lone talker among ten open
    // mics reaches within 1 dB of full gain in about 7 ms (about 3 ms with 5 ms).
    double attackMs = 15.0;
    double releaseMs = 200.0;

    // Bypass fades the channel to unity gain, and its share out of the group, over this time.
    // Joining or leaving because of signal presence uses the same fade.
    double bypassFadeMs = 20.0;

    // The detector listens to the voice band only: its copy of the signal goes through a band-pass
    // (2nd-order high-pass and low-pass, IIR, no look-ahead), so rumble, handling noise and hiss
    // take no share. The audio itself is never filtered. Set detectorVoiceBand to false to revert
    // to the full-band detector.
    bool detectorVoiceBand = true;
    double detectorHighPassHz = 150.0;
    double detectorLowPassHz = 5000.0;

    // Signal presence, on the unweighted detector level: present from presenceOnDb up, absent
    // after staying below presenceOffDb for presenceHoldMs.
    double presenceOnDb = -75.0;
    double presenceOffDb = -81.0;
    double presenceHoldMs = 1000.0;

    // Added to every channel's power so digital silence still shares gain equally
    // (N silent channels each get -10*log10(N) dB). -100 dBFS.
    double powerFloor = 1.0e-10;
};

//==================================================================================================
// Where peer levels come from: InstanceLink (InstanceLink.h) in the plugin, fakes in the tests.
// Without peers a channel runs solo, which makes its automix gain exactly 1.
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
// Block RMS levels for the meters: each of the first two channels on its own (left and right on a
// stereo track), and all channels together.
struct BlockLevels
{
    float channelDb[2] { -120.0f, -120.0f };
    float combinedDb = -120.0f;
};

inline BlockLevels measureLevels (const float* const* channels, int numChannels, int numSamples) noexcept
{
    BlockLevels levels;
    if (numChannels <= 0 || numSamples <= 0)
        return levels;

    double total = 0.0;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i)
            sum += static_cast<double> (channels[ch][i]) * channels[ch][i];
        total += sum;
        if (ch < 2)
            levels.channelDb[ch] = static_cast<float> (gainToDb (std::sqrt (sum / numSamples)));
    }
    levels.combinedDb = static_cast<float> (gainToDb (std::sqrt (total / (numChannels * numSamples))));
    return levels;
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
        bypassStep = std::min (1.0, 1000.0 / (settings.bypassFadeMs * hopsPerSecond));
        presenceOnPower = std::pow (10.0, settings.presenceOnDb / 10.0);
        presenceOffPower = std::pow (10.0, settings.presenceOffDb / 10.0);
        presenceHoldHops = std::max (1, (int) std::lround (settings.presenceHoldMs * hopsPerSecond / 1000.0));
        highPass = Biquad::butterworth (sampleRate, settings.detectorHighPassHz, true);
        lowPass = Biquad::butterworth (sampleRate, std::min (settings.detectorLowPassHz, 0.45 * sampleRate), false);
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : filterState)
            s = {};
        hopAccumulator = 0.0;
        hopFill = 0;
        expectedNextSample = 0;
        smoothedPower = settings.powerFloor;
        currentGain = targetGain = 1.0;
        gainStep = 0.0;
        lastInputPower = 0.0;
        bypassMix = bypassTarget;
        present = false;
        presentMix = 0.0;
        holdHopsLeft = 0;
    }

    // Bypassed: unity gain, and nothing contributed to the group's sum, so the other channels
    // share as if this one were not there. Changes fade over bypassFadeMs unless immediate.
    void setBypassed (bool shouldBypass, bool immediate = false) noexcept
    {
        bypassTarget = shouldBypass ? 1.0 : 0.0;
        if (immediate)
            bypassMix = bypassTarget;
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
        // first hop is measured over fewer samples, which the smoothing absorbs. A gain ramp still
        // running is re-sized to end on its target with that shorter hop; left as it was, it would
        // run past the target (even below zero) by the samples the jump added.
        if (startSample != expectedNextSample)
        {
            hopAccumulator = 0.0;
            gainStep = (targetGain - currentGain) / (EngineSettings::hopSize - hopFill);
        }

        for (int i = 0; i < numSamples; ++i)
        {
            double power = 0.0;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                double x = channels[ch][i];
                if (settings.detectorVoiceBand && ch < maxFilteredChannels)
                {
                    auto& s = filterState[(size_t) ch];
                    x = lowPass.process (highPass.process (x, s.hp), s.lp);
                }
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

    static constexpr int maxFilteredChannels = 8;
    double getLastInputPower() const noexcept { return lastInputPower; }
    double getCurrentGain() const noexcept { return currentGain; }
    bool isBypassed() const noexcept { return bypassTarget > 0.5; }
    bool isPresent() const noexcept { return present; }

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

        updatePresence (smoothedPower / weightPower);

        bypassMix = stepTowards (bypassMix, bypassTarget);
        presentMix = stepTowards (presentMix, present ? 1.0 : 0.0);

        // How much this channel takes part: 0 when bypassed or without signal (unity gain,
        // nothing shared), 1 when fully in, faded in between.
        const double active = (1.0 - bypassMix) * presentMix;

        double peerSum = 0.0;
        if (peers != nullptr)
        {
            peers->publish (hopIndex, smoothedPower * active);
            peerSum = peers->sumOfPeerPowers (hopIndex);
        }

        targetGain = (1.0 - active) + active * automixGain (smoothedPower, peerSum);
        gainStep = (targetGain - currentGain) / EngineSettings::hopSize;

        hopAccumulator = 0.0;
        hopFill = 0;
    }

    void updatePresence (double level) noexcept
    {
        if (level >= presenceOnPower)
        {
            present = true;
            holdHopsLeft = presenceHoldHops;
        }
        else if (present && level < presenceOffPower)
        {
            if (--holdHopsLeft <= 0)
                present = false;
        }
        else if (present)
        {
            holdHopsLeft = presenceHoldHops;
        }
    }

    double stepTowards (double value, double target) const noexcept
    {
        return value < target ? std::min (target, value + bypassStep) : std::max (target, value - bypassStep);
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

    // Transposed direct form II biquad (RBJ Butterworth, Q = 0.7071), coefficients normalised.
    struct Biquad
    {
        struct State { double z1 = 0.0, z2 = 0.0; };
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

        static Biquad butterworth (double sampleRate, double cornerHz, bool isHighPass) noexcept
        {
            const double w = 2.0 * 3.14159265358979323846 * cornerHz / sampleRate;
            const double alpha = std::sin (w) / (2.0 * 0.70710678118654752440);
            const double c = std::cos (w), a0 = 1.0 + alpha;
            Biquad q;
            q.b0 = (isHighPass ? (1.0 + c) : (1.0 - c)) / 2.0 / a0;
            q.b1 = (isHighPass ? -(1.0 + c) : (1.0 - c)) / a0;
            q.b2 = q.b0;
            q.a1 = -2.0 * c / a0;
            q.a2 = (1.0 - alpha) / a0;
            return q;
        }

        double process (double x, State& s) const noexcept
        {
            const double y = b0 * x + s.z1;
            s.z1 = b1 * x - a1 * y + s.z2;
            s.z2 = b2 * x - a2 * y;
            return y;
        }
    };

    struct ChannelFilterState { Biquad::State hp, lp; };

    EngineSettings settings;
    Biquad highPass, lowPass;
    std::array<ChannelFilterState, maxFilteredChannels> filterState {};
    double attackCoeff = 0.0, releaseCoeff = 0.0;
    double weightPower = 1.0;

    double hopAccumulator = 0.0;
    int hopFill = 0;
    int64_t expectedNextSample = 0;

    double smoothedPower = 1.0e-10;
    double lastInputPower = 0.0;
    double currentGain = 1.0, targetGain = 1.0, gainStep = 0.0;
    double bypassTarget = 0.0, bypassMix = 0.0, bypassStep = 1.0;

    double presenceOnPower = 0.0, presenceOffPower = 0.0;
    int presenceHoldHops = 1, holdHopsLeft = 0;
    bool present = false;
    double presentMix = 0.0;
};

} // namespace sgtm
