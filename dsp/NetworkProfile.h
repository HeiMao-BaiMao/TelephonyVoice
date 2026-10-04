#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace TelephonyDSP {

enum class NetworkLossModel { Independent, GilbertElliott, ErrorPattern };
enum class JitterDistribution { Uniform, Gamma, Weibull, Pareto };

// k/h are the probabilities of SUCCESS in Good/Bad, respectively. The
// transition is made after emitting each packet's loss decision.
struct GilbertElliottParameters {
    double p = 0.01, r = 0.5, k = 1.0, h = 0.0;
    static GilbertElliottParameters fromLossAndBurst(double loss, double meanBurst);
    double stationaryLoss() const;
};

// Independent knobs; no implicit coupling to an aggregate degradation slider.
// Delay values are milliseconds. Positive distributions are mean-centred and
// their scale is jitterAmplitudeMs; this is NOT their standard deviation.
struct NetworkProfile {
    double bandwidthNarrowing = 0.0;
    double jitterAmplitudeMs = 0.0;
    double burstLengthMean = 2.0;
    double lossRate = 0.0, lossRateBoost = 0.0;
    int opusFecPercent = 0;
    double opusPlaybackDelayMs = 60.0;
    bool filterCascadeEnabled = true;
    NetworkLossModel lossModel = NetworkLossModel::Independent;
    GilbertElliottParameters gilbert;
    JitterDistribution jitterDistribution = JitterDistribution::Uniform;
    double jitterShape = 2.0;
    double jitterAutocorrelation = 0.0;
    double baseDelayMs = 0.0;
    bool allowEarlyArrivals = true;
    double duplicateProbability = 0.0, duplicateDelayMs = 1.0;
    double lateProbability = 0.0, lateExtraDelayMs = 200.0;
    double maxDelayMs = 60000.0;
    std::size_t maxQueuedPackets = 512;
    NetworkProfile sanitized() const;
};

struct NetworkPacket {
    // Extended internal sequence, not the wrapping 16-bit wire sequence.
    uint64_t sequence = 0;
    uint32_t timestamp = 0;
    double sentTimeMs = 0.0, arrivalTimeMs = 0.0;
    bool duplicate = false;
    std::vector<uint8_t> bytes;
};

struct NetworkCounters {
    uint64_t submitted = 0, lost = 0, duplicated = 0;
    uint64_t delivered = 0, reordered = 0, overflow = 0;
};

// No sockets, wall clock or global RNG. Reset repeats the same realization.
// Not thread safe. The bounded packet queue owns encoded bytes; reserve codec
// buffers separately if the caller needs a strictly allocation-free callback.
class NetworkSimulator {
public:
    explicit NetworkSimulator(const NetworkProfile& = {}, uint32_t seed = 0x54454c45u);
    void configure(const NetworkProfile&); // preserves packets, RNG and loss state
    void reset(uint32_t seed = 0x54454c45u);
    const NetworkProfile& profile() const { return config; }
    const NetworkCounters& counters() const { return counts; }
    bool submit(NetworkPacket packet, bool forceLoss = false); // false: loss or capacity rejection
    bool popArrived(double nowMs, NetworkPacket&);
    std::size_t queued() const { return pending.size(); }
    bool nextLoss();
    double nextJitterMs();
    // An actual externally supplied error pattern, 1=lost, 0=received. No
    // fabricated EP1..EP6 presets. EP1..EP6 belong to TR45.050 Annex F.2
    // (GSM chip errors/TCH-FS data), not TS26.131 packet-loss Markov presets:
    // https://www.etsi.org/deliver/etsi_tr/145000_145099/145050/19.00.00_60/tr_145050v190000p.pdf
    // TS26.131 delay/loss traces are separately attached to TS26.132. Loading
    // this simple 0/1 trace is not a parser for either reference binary format.
    bool setErrorPattern(const std::string& bits, bool repeat = true);
private:
    NetworkProfile config;
    NetworkCounters counts;
    std::mt19937 random, lossRandom, duplicateRandom, lateRandom;
    std::vector<NetworkPacket> pending;
    std::vector<bool> errorPattern;
    std::size_t patternOffset = 0;
    bool patternRepeats = true, bad = false, haveJitter = false, haveDelivered = false;
    double previousJitter = 0.0;
    uint64_t greatestDelivered = 0;
    double uniform();
    static double uniform(std::mt19937&);
    double normal();
    double gamma(double shape);
    void insert(NetworkPacket);
};

struct JitterBufferCounters {
    uint64_t accepted = 0, duplicates = 0, late = 0, missing = 0, overflow = 0;
};

// Arrival scheduling and playout are deliberately separate: collect network
// arrivals through a deadline, then call take() exactly once per frame. This
// permits reordering without treating the earliest arrival as the next frame.
class PacketJitterBuffer {
public:
    explicit PacketJitterBuffer(std::size_t capacity = 512);
    void reset(uint64_t firstSequence = 0);
    bool push(NetworkPacket);
    bool take(uint64_t sequence, double deadlineMs, NetworkPacket&);
    bool peek(uint64_t sequence, double deadlineMs, NetworkPacket&) const;
    const JitterBufferCounters& counters() const { return counts; }
    std::size_t queued() const { return packets.size(); }
private:
    std::size_t capacity;
    uint64_t nextSequence = 0;
    std::map<uint64_t, NetworkPacket> packets;
    JitterBufferCounters counts;
};
}
