#pragma once

// SGTM Automix instance link: lets every SGTM Automix instance on this computer see the others'
// levels, so the gain-sharing law can run across all of them.
//
// Plain C++ (POSIX shared memory, no JUCE) so it can be tested on its own. Instances meet in one
// named shared-memory block, so it also works when a host runs plugins in separate processes.
// Each instance owns one slot: it publishes its smoothed power once per hop into a ring indexed
// by absolute hop number, plus its newest value, a heartbeat and a few display values.
//
// Live (realtime) processing never waits, locks, allocates or makes syscalls: a channel uses the
// newest value each peer has published. Hosts process tracks one after another within a buffer
// cycle, so a peer's value is at most one buffer old.
//
// Offline (bounce) processing is position-exact where the host allows it. A channel reads each
// peer's value for the same hop. If a peer has not reached that hop yet it waits briefly, within a
// small budget per block. When a peer keeps it waiting past the budget (typically a host that
// renders tracks one after another on one thread), it stops waiting for that peer for an
// exponentially growing number of blocks and uses the peer's newest value instead.
//
// Peers whose heartbeat is older than a second drop out. A slot whose owner process has died is
// reclaimed by the next instance that needs one.

#include "AutomixEngine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
 #define SGTM_LINK_POSIX 1
 #include <cerrno>
 #include <fcntl.h>
 #include <signal.h>
 #include <sys/mman.h>
 #include <sys/stat.h>
 #include <unistd.h>
#else
 #define SGTM_LINK_POSIX 0
#endif

namespace sgtm
{

namespace linkdetail
{
constexpr uint32_t magic = 0x53414d31; // layout tag; bump with the version in the name
constexpr int maxSlots = 64;
constexpr int ringHops = 8192;          // ~2.7 s at 48 kHz with 16-sample hops
constexpr int labelBytes = 48;

static_assert (std::atomic<int64_t>::is_always_lock_free, "needs lock-free 64-bit atomics");
static_assert (std::atomic<uint64_t>::is_always_lock_free, "needs lock-free 64-bit atomics");

inline uint64_t toBits (double d) noexcept { uint64_t u; std::memcpy (&u, &d, 8); return u; }
inline double fromBits (uint64_t u) noexcept { double d; std::memcpy (&d, &u, 8); return d; }
inline uint32_t toBits (float f) noexcept { uint32_t u; std::memcpy (&u, &f, 4); return u; }
inline float fromBits32 (uint32_t u) noexcept { float f; std::memcpy (&f, &u, 4); return f; }

// Ring entries store the hop as a tag; 0 means empty (INT64_MIN is never a real hop).
inline int64_t tagFor (int64_t hop) noexcept { return hop ^ INT64_MIN; }

struct Entry
{
    std::atomic<int64_t> tag;
    std::atomic<uint64_t> powerBits;
};

struct alignas (64) Slot
{
    std::atomic<uint64_t> owner;       // 0 = free
    std::atomic<int32_t> pid;
    std::atomic<uint32_t> offline;
    std::atomic<int64_t> heartbeatNs;  // steady clock; 0 = not running
    std::atomic<int64_t> runStartNs;   // when the current contiguous run of blocks began
    std::atomic<int64_t> latestHop;
    std::atomic<uint64_t> latestPowerBits;

    // Display values, written once per block by the owner's audio thread.
    std::atomic<uint32_t> inputDbBits, gainDbBits, outputDbBits, weightDbBits;
    std::atomic<uint32_t> bypassed;

    // Label, written by the owner's message thread. Even seq = stable.
    std::atomic<uint32_t> labelSeq;
    std::atomic<uint64_t> labelWords[labelBytes / 8];

    Entry ring[ringHops];
};

struct Shared
{
    std::atomic<uint32_t> magic;
    std::atomic<uint32_t> layoutSize;
    std::atomic<uint32_t> automixOff; // the all-channels switch: 1 = every channel at unity
    Slot slots[maxSlots];
};
} // namespace linkdetail

//==================================================================================================
// What the editor shows for one channel.
struct LinkedChannelInfo
{
    int slot = -1;
    bool isSelf = false;
    std::string label;
    float inputDb = -120.0f, gainDb = 0.0f, outputDb = -120.0f, weightDb = 0.0f;
    bool bypassed = false;
    bool idle = false; // loaded but not processing (for example Logic with the transport stopped)
};

//==================================================================================================
struct InstanceLinkSettings
{
    int64_t aliveNs = 1'000'000'000;       // peers silent for longer drop out
    int64_t reclaimNs = 2'000'000'000;     // a dead owner's slot can be reused after this
    int64_t waitBudgetNs = 5'000'000;      // offline: total wait per block
    int64_t runMarginNs = 250'000'000;     // offline: a peer run that began this much earlier is a previous pass
    int maxBackoffShift = 9;               // offline: skip a slow peer for up to 512 blocks
};

class InstanceLink final : public PeerLevels
{
public:
    using Settings = InstanceLinkSettings;

    static int64_t steadyNowNs() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds> (
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // One block per user, versioned, so a layout change never meets an older build's block.
    static std::string defaultName()
    {
#if SGTM_LINK_POSIX
        return "/sgtm-automix-1-" + std::to_string ((unsigned long) getuid());
#else
        return {};
#endif
    }

    // Removes a named block (tests only; a live block is left in place).
    static void unlinkShared (const std::string& name)
    {
#if SGTM_LINK_POSIX
        shm_unlink (name.c_str());
#else
        (void) name;
#endif
    }

    explicit InstanceLink (const std::string& name = defaultName(), Settings s = {})
        : settings (s)
    {
        shared = openShared (name);
        token = makeToken();
    }

    ~InstanceLink() override
    {
        leave();
#if SGTM_LINK_POSIX
        if (shared != nullptr)
            munmap (shared, sizeof (linkdetail::Shared));
#endif
    }

    InstanceLink (const InstanceLink&) = delete;
    InstanceLink& operator= (const InstanceLink&) = delete;

    // False when the shared block could not be opened (sandboxed host, unsupported platform):
    // the channel then runs solo.
    bool isAvailable() const noexcept { return shared != nullptr; }
    bool isJoined() const noexcept { return self != nullptr; }
    int getSlotIndex() const noexcept { return selfIndex; }

    //==============================================================================================
    // Message thread.

    // Claims a slot. Safe to call repeatedly.
    bool join (int64_t nowNs = steadyNowNs())
    {
        if (shared == nullptr || self != nullptr)
            return self != nullptr;

        for (int pass = 0; pass < 2; ++pass)
        {
            for (int i = 0; i < linkdetail::maxSlots; ++i)
            {
                auto& s = shared->slots[i];
                uint64_t current = s.owner.load (std::memory_order_acquire);

                if (current != 0)
                {
                    // Second pass: take over slots whose owner process is gone.
                    if (pass == 0 || ! isReclaimable (s, nowNs))
                        continue;
                }

                if (s.owner.compare_exchange_strong (current, token, std::memory_order_acq_rel))
                {
                    initialiseSlot (s);
                    self = &s;
                    selfIndex = i;
                    publishLabel();

                    // The all-channels switch lives as long as the shared block (until restart),
                    // so a new session never starts with the automix off from an earlier one.
                    if (! anyOtherAlive (nowNs))
                        shared->automixOff.store (0, std::memory_order_release);
                    return true;
                }
            }
        }
        return false;
    }

    void leave() noexcept
    {
        if (self == nullptr)
            return;
        self->heartbeatNs.store (0, std::memory_order_release);
        uint64_t mine = token;
        self->owner.compare_exchange_strong (mine, 0, std::memory_order_acq_rel);
        self = nullptr;
        selfIndex = -1;
    }

    // The all-channels switch: when off, every linked channel runs at unity gain.
    void setAutomixOn (bool on) noexcept
    {
        if (shared != nullptr)
            shared->automixOff.store (on ? 0u : 1u, std::memory_order_release);
    }

    bool isAutomixOn() const noexcept
    {
        return shared == nullptr || shared->automixOff.load (std::memory_order_acquire) == 0;
    }

    void setLabel (const std::string& newLabel)
    {
        label = newLabel.substr (0, linkdetail::labelBytes - 1);
        publishLabel();
    }

    // Every loaded channel, this one included, in slot order: processing ones, and idle ones whose
    // process is still running (hosts such as Logic stop processing tracks while stopped).
    // Allocates and makes syscalls; message thread only.
    std::vector<LinkedChannelInfo> getChannels (int64_t nowNs = steadyNowNs()) const
    {
        std::vector<LinkedChannelInfo> result;
        if (shared == nullptr)
            return result;

        for (int i = 0; i < linkdetail::maxSlots; ++i)
        {
            const auto& s = shared->slots[i];
            const bool isSelf = &s == self;
            const bool processing = isAlive (s, nowNs);
            if (! isSelf && ! processing && ! isLoaded (s))
                continue;

            LinkedChannelInfo info;
            info.slot = i;
            info.isSelf = isSelf;
            info.idle = ! processing;
            info.label = readLabel (s);
            if (info.label.empty())
                info.label = "Channel " + std::to_string (i + 1);
            info.inputDb = linkdetail::fromBits32 (s.inputDbBits.load (std::memory_order_relaxed));
            info.gainDb = linkdetail::fromBits32 (s.gainDbBits.load (std::memory_order_relaxed));
            info.outputDb = linkdetail::fromBits32 (s.outputDbBits.load (std::memory_order_relaxed));
            info.weightDb = linkdetail::fromBits32 (s.weightDbBits.load (std::memory_order_relaxed));
            info.bypassed = s.bypassed.load (std::memory_order_relaxed) != 0;
            result.push_back (std::move (info));
        }
        return result;
    }

    //==============================================================================================
    // Audio thread. No locks, allocation or syscalls; waits only when offline, and boundedly.

    // Call before processing a block that starts at absolute sample startSample.
    void beginBlock (int64_t nowNs, bool isOffline, int64_t startSample, int numSamples) noexcept
    {
        numPeers = 0;
        offline = isOffline;
        waitDeadlineNs = 0;
        ++blockCounter;

        if (self == nullptr)
            return;

        if (startSample != expectedNextSample || ! running)
        {
            // New contiguous run (start, locate, loop, or a second bounce over the same range).
            self->latestHop.store (INT64_MIN, std::memory_order_release);
            self->runStartNs.store (nowNs, std::memory_order_release);
            myRunStartNs = nowNs;
            running = true;
        }
        expectedNextSample = startSample + numSamples;

        self->offline.store (isOffline ? 1u : 0u, std::memory_order_relaxed);
        self->heartbeatNs.store (nowNs, std::memory_order_release);

        for (int i = 0; i < linkdetail::maxSlots; ++i)
        {
            auto& s = shared->slots[i];
            if (&s == self || ! isAlive (s, nowNs))
                continue;

            const uint64_t owner = s.owner.load (std::memory_order_relaxed);
            auto& state = peerState[(size_t) i];
            if (state.owner != owner)
                state = { owner, 0, 0 };

            auto& p = peers[(size_t) numPeers++];
            p.slot = &s;
            p.state = &state;
            // Offline, every peer is read by position. A peer that is still running live (the
            // host has not switched it to the bounce yet) or is on a previous pass is waited for.
            p.exact = isOffline;
        }
    }

    // Display values for this block.
    void setDisplay (float inputDb, float gainDb, float outputDb, float weightDb, bool bypassed = false) noexcept
    {
        if (self == nullptr)
            return;
        self->bypassed.store (bypassed ? 1u : 0u, std::memory_order_relaxed);
        self->inputDbBits.store (linkdetail::toBits (inputDb), std::memory_order_relaxed);
        self->gainDbBits.store (linkdetail::toBits (gainDb), std::memory_order_relaxed);
        self->outputDbBits.store (linkdetail::toBits (outputDb), std::memory_order_relaxed);
        self->weightDbBits.store (linkdetail::toBits (weightDb), std::memory_order_relaxed);
    }

    // Peers counted by the last beginBlock (this channel excluded).
    int getNumPeers() const noexcept { return numPeers; }

    // Offline waits that ran out of budget (diagnostics and tests).
    int64_t getWaitTimeouts() const noexcept { return waitTimeouts; }

    void publish (int64_t hopIndex, double power) noexcept override
    {
        if (self == nullptr)
            return;

        // Per-entry sequence: invalidate, write, then tag, so a reader never pairs a tag with
        // another hop's value.
        auto& e = self->ring[ringIndex (hopIndex)];
        e.tag.store (0, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        e.powerBits.store (linkdetail::toBits (power), std::memory_order_relaxed);
        e.tag.store (linkdetail::tagFor (hopIndex), std::memory_order_release);

        self->latestPowerBits.store (linkdetail::toBits (power), std::memory_order_relaxed);
        self->latestHop.store (hopIndex, std::memory_order_release);
    }

    double sumOfPeerPowers (int64_t hopIndex) noexcept override
    {
        double sum = 0.0;
        for (int i = 0; i < numPeers; ++i)
        {
            auto& p = peers[(size_t) i];
            sum += p.exact ? exactPower (p, hopIndex) : latestPower (*p.slot);
        }
        return sum;
    }

private:
    struct PeerState
    {
        uint64_t owner = 0;
        int strikes = 0;
        int64_t skipUntilBlock = 0;
    };

    struct Peer
    {
        linkdetail::Slot* slot = nullptr;
        PeerState* state = nullptr;
        bool exact = false;
    };

    static size_t ringIndex (int64_t hop) noexcept
    {
        const int64_t m = hop % linkdetail::ringHops;
        return (size_t) (m < 0 ? m + linkdetail::ringHops : m);
    }

    static double latestPower (const linkdetail::Slot& s) noexcept
    {
        return linkdetail::fromBits (s.latestPowerBits.load (std::memory_order_relaxed));
    }

    static bool readEntry (const linkdetail::Slot& s, int64_t hop, double& power) noexcept
    {
        const auto& e = s.ring[ringIndex (hop)];
        const int64_t want = linkdetail::tagFor (hop);
        if (e.tag.load (std::memory_order_acquire) != want)
            return false;
        power = linkdetail::fromBits (e.powerBits.load (std::memory_order_relaxed));
        std::atomic_thread_fence (std::memory_order_acquire);
        return e.tag.load (std::memory_order_relaxed) == want;
    }

    double exactPower (Peer& p, int64_t hop) noexcept
    {
        auto& s = *p.slot;
        auto& st = *p.state;

        if (! hasReached (s, hop))
        {
            if (st.skipUntilBlock > blockCounter || ! waitFor (s, hop))
            {
                if (st.skipUntilBlock <= blockCounter)
                {
                    st.strikes = std::min (st.strikes + 1, settings.maxBackoffShift);
                    st.skipUntilBlock = blockCounter + (int64_t (1) << st.strikes);
                    ++waitTimeouts;
                }
                return latestPower (s);
            }
        }

        double power;
        return readEntry (s, hop, power) ? power : latestPower (s);
    }

    // The peer is rendering offline in the same pass as this channel and has published hop.
    bool hasReached (const linkdetail::Slot& s, int64_t hop) const noexcept
    {
        return s.offline.load (std::memory_order_acquire) != 0
               && s.runStartNs.load (std::memory_order_acquire) >= myRunStartNs - settings.runMarginNs
               && s.latestHop.load (std::memory_order_acquire) >= hop;
    }

    bool waitFor (const linkdetail::Slot& s, int64_t hop) noexcept
    {
        for (;;)
        {
            if (hasReached (s, hop))
                return true;
            const int64_t now = steadyNowNs();
            if (waitDeadlineNs == 0)
                waitDeadlineNs = now + settings.waitBudgetNs;
            if (now >= waitDeadlineNs)
                return false;
            std::this_thread::yield();
        }
    }

    bool isAlive (const linkdetail::Slot& s, int64_t nowNs) const noexcept
    {
        if (s.owner.load (std::memory_order_acquire) == 0)
            return false;
        const int64_t hb = s.heartbeatNs.load (std::memory_order_acquire);
        return hb != 0 && nowNs - hb < settings.aliveNs;
    }

    bool anyOtherAlive (int64_t nowNs) const noexcept
    {
        for (const auto& s : shared->slots)
            if (&s != self && isAlive (s, nowNs))
                return true;
        return false;
    }

    // Claimed by a process that is still running (an instance that exists but may not process).
    static bool isLoaded (const linkdetail::Slot& s) noexcept
    {
        if (s.owner.load (std::memory_order_acquire) == 0)
            return false;
#if SGTM_LINK_POSIX
        const pid_t pid = (pid_t) s.pid.load (std::memory_order_relaxed);
        return pid > 0 && (kill (pid, 0) == 0 || errno == EPERM);
#else
        return true;
#endif
    }

    bool isReclaimable (const linkdetail::Slot& s, int64_t nowNs) const noexcept
    {
        const int64_t hb = s.heartbeatNs.load (std::memory_order_acquire);
        if (hb != 0 && nowNs - hb < settings.reclaimNs)
            return false;
#if SGTM_LINK_POSIX
        const pid_t pid = (pid_t) s.pid.load (std::memory_order_relaxed);
        return pid <= 0 || (kill (pid, 0) != 0 && errno == ESRCH);
#else
        return false;
#endif
    }

    void initialiseSlot (linkdetail::Slot& s) noexcept
    {
#if SGTM_LINK_POSIX
        s.pid.store ((int32_t) getpid(), std::memory_order_relaxed);
#endif
        s.heartbeatNs.store (0, std::memory_order_relaxed);
        s.offline.store (0, std::memory_order_relaxed);
        s.runStartNs.store (0, std::memory_order_relaxed);
        s.latestHop.store (INT64_MIN, std::memory_order_relaxed);
        s.latestPowerBits.store (linkdetail::toBits (EngineSettings {}.powerFloor), std::memory_order_relaxed);
        s.bypassed.store (0, std::memory_order_relaxed);
        for (auto& e : s.ring)
            e.tag.store (0, std::memory_order_relaxed);
        std::atomic_thread_fence (std::memory_order_release);
        running = false;
    }

    void publishLabel() noexcept
    {
        if (self == nullptr)
            return;
        char buf[linkdetail::labelBytes] = {};
        std::memcpy (buf, label.data(), std::min (label.size(), sizeof (buf) - 1));

        self->labelSeq.fetch_add (1, std::memory_order_acq_rel);
        for (int w = 0; w < linkdetail::labelBytes / 8; ++w)
        {
            uint64_t word;
            std::memcpy (&word, buf + w * 8, 8);
            self->labelWords[w].store (word, std::memory_order_relaxed);
        }
        self->labelSeq.fetch_add (1, std::memory_order_release);
    }

    static std::string readLabel (const linkdetail::Slot& s)
    {
        char buf[linkdetail::labelBytes + 1] = {};
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const uint32_t before = s.labelSeq.load (std::memory_order_acquire);
            if ((before & 1u) != 0)
                continue;
            for (int w = 0; w < linkdetail::labelBytes / 8; ++w)
            {
                const uint64_t word = s.labelWords[w].load (std::memory_order_relaxed);
                std::memcpy (buf + w * 8, &word, 8);
            }
            std::atomic_thread_fence (std::memory_order_acquire);
            if (s.labelSeq.load (std::memory_order_relaxed) == before)
                return std::string (buf);
        }
        return {};
    }

    static uint64_t makeToken() noexcept
    {
        static std::atomic<uint64_t> counter { 0 };
        uint64_t t = (uint64_t) steadyNowNs() * 0x9e3779b97f4a7c15ull;
#if SGTM_LINK_POSIX
        t ^= (uint64_t) getpid() << 40;
#endif
        t ^= counter.fetch_add (1) * 0xbf58476d1ce4e5b9ull;
        return t == 0 ? 1 : t;
    }

    static linkdetail::Shared* openShared (const std::string& name) noexcept
    {
#if SGTM_LINK_POSIX
        if (name.empty())
            return nullptr;

        const int fd = shm_open (name.c_str(), O_RDWR | O_CREAT, 0600);
        if (fd < 0)
            return nullptr;

        const auto size = (off_t) sizeof (linkdetail::Shared);
        struct stat st {};
        if (fstat (fd, &st) == 0 && st.st_size == 0)
            (void) ftruncate (fd, size); // may lose a race with another process; checked below

        if (fstat (fd, &st) != 0 || st.st_size < size)
        {
            close (fd);
            return nullptr;
        }

        void* mem = mmap (nullptr, sizeof (linkdetail::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close (fd);
        if (mem == MAP_FAILED)
            return nullptr;

        // A new block is all zeros, which is a valid empty state; stamp it once.
        auto* shared = static_cast<linkdetail::Shared*> (mem);
        constexpr auto layoutSize = (uint32_t) sizeof (linkdetail::Shared);
        uint32_t expected = 0;
        if (shared->magic.compare_exchange_strong (expected, linkdetail::magic))
            shared->layoutSize.store (layoutSize, std::memory_order_release);
        else if (expected != linkdetail::magic
                 || (shared->layoutSize.load (std::memory_order_acquire) != layoutSize
                     && shared->layoutSize.load (std::memory_order_acquire) != 0))
        {
            munmap (mem, sizeof (linkdetail::Shared));
            return nullptr;
        }
        return shared;
#else
        (void) name;
        return nullptr;
#endif
    }

    Settings settings;
    linkdetail::Shared* shared = nullptr;
    linkdetail::Slot* self = nullptr;
    int selfIndex = -1;
    uint64_t token = 0;
    std::string label;

    // Audio-thread state.
    bool offline = false, running = false;
    int64_t expectedNextSample = 0, myRunStartNs = 0, waitDeadlineNs = 0, blockCounter = 0;
    int64_t waitTimeouts = 0;
    int numPeers = 0;
    std::array<Peer, linkdetail::maxSlots> peers {};
    std::array<PeerState, linkdetail::maxSlots> peerState {};
};

} // namespace sgtm
