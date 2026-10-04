#include "dsp/NetworkProfile.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace TelephonyDSP {
namespace {
double finiteClamp(double x, double lo, double hi, double fallback = 0.0) {
    return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
}
}

GilbertElliottParameters GilbertElliottParameters::fromLossAndBurst(double loss, double meanBurst) {
    loss = finiteClamp(loss, 0, 1);
    meanBurst = finiteClamp(meanBurst, 1, 1e6, 2);
    if (loss <= 0) return {0, 1, 1, 0};
    if (loss >= 1) return {1, 0, 0, 0};
    // A two-state all-good/all-bad model cannot combine a very high loss rate
    // with a short burst. Keep the requested loss, increasing burst if needed.
    const double r = std::min(1.0 / meanBurst, (1.0 - loss) / loss);
    return {loss * r / (1 - loss), r, 1, 0};
}
double GilbertElliottParameters::stationaryLoss() const {
    const double pp = finiteClamp(p, 0, 1), rr = finiteClamp(r, 0, 1);
    const double kk = finiteClamp(k, 0, 1, 1), hh = finiteClamp(h, 0, 1);
    return pp + rr > 0 ? (rr * (1 - kk) + pp * (1 - hh)) / (pp + rr) : 1 - kk;
}
NetworkProfile NetworkProfile::sanitized() const {
    NetworkProfile v = *this;
    v.bandwidthNarrowing = finiteClamp(v.bandwidthNarrowing, 0, 1);
    v.jitterAmplitudeMs = finiteClamp(v.jitterAmplitudeMs, 0, 60000);
    v.burstLengthMean = finiteClamp(v.burstLengthMean, 1, 1e6, 2);
    v.lossRate = finiteClamp(v.lossRate, 0, 1);
    v.lossRateBoost = finiteClamp(v.lossRateBoost, 0, 1);
    v.opusFecPercent = std::clamp(v.opusFecPercent, 0, 100);
    v.opusPlaybackDelayMs = finiteClamp(v.opusPlaybackDelayMs, 0, 60000, 60);
    v.gilbert.p = finiteClamp(v.gilbert.p, 0, 1);
    v.gilbert.r = finiteClamp(v.gilbert.r, 0, 1);
    v.gilbert.k = finiteClamp(v.gilbert.k, 0, 1, 1);
    v.gilbert.h = finiteClamp(v.gilbert.h, 0, 1);
    // Pareto needs shape > 2 for finite variance, required by AR(1).
    v.jitterShape = finiteClamp(v.jitterShape,
        v.jitterDistribution == JitterDistribution::Pareto ? 2.01 : 0.1, 100, 3);
    v.jitterAutocorrelation = finiteClamp(v.jitterAutocorrelation, -0.99, 0.99);
    v.baseDelayMs = finiteClamp(v.baseDelayMs, 0, 60000);
    v.duplicateProbability = finiteClamp(v.duplicateProbability, 0, 1);
    v.duplicateDelayMs = finiteClamp(v.duplicateDelayMs, 0, 60000, 1);
    v.lateProbability = finiteClamp(v.lateProbability, 0, 1);
    v.lateExtraDelayMs = finiteClamp(v.lateExtraDelayMs, 0, 60000, 200);
    v.maxDelayMs = finiteClamp(v.maxDelayMs, 0, 60000, 60000);
    v.maxQueuedPackets = std::clamp<std::size_t>(v.maxQueuedPackets, 1, 65536);
    return v;
}
NetworkSimulator::NetworkSimulator(const NetworkProfile& p, uint32_t seed) : config(p.sanitized()), random(seed) {
    pending.reserve(config.maxQueuedPackets);
    reset(seed);
}
void NetworkSimulator::configure(const NetworkProfile& p) {
    config = p.sanitized();
    pending.reserve(config.maxQueuedPackets);
    while (pending.size() > config.maxQueuedPackets) { pending.pop_back(); ++counts.overflow; }
}
void NetworkSimulator::reset(uint32_t seed) {
    random.seed(seed); lossRandom.seed(seed ^ 0xa511e9b3u);
    duplicateRandom.seed(seed ^ 0x63d83595u); lateRandom.seed(seed ^ 0x9e3779b9u);
    pending.clear(); counts = {}; bad = false;
    previousJitter = 0; haveJitter = false; haveDelivered = false;
    greatestDelivered = 0; patternOffset = 0;
}
double NetworkSimulator::uniform() {
    return uniform(random);
}
double NetworkSimulator::uniform(std::mt19937& generator) {
    // Open interval: safe for log/inverse-CDF even for mt19937 endpoints.
    return (static_cast<double>(generator()) + 0.5) / 4294967296.0;
}
double NetworkSimulator::normal() {
    return std::sqrt(-2 * std::log(uniform())) * std::cos(6.2831853071795864769 * uniform());
}
double NetworkSimulator::gamma(double shape) {
    if (shape < 1) return gamma(shape + 1) * std::pow(uniform(), 1 / shape);
    const double d = shape - 1.0 / 3.0, c = 1 / std::sqrt(9 * d);
    for (;;) {
        const double x = normal();
        const double root = 1 + c * x;
        if (root <= 0) continue;
        const double v = root * root * root, u = uniform();
        if (u < 1 - 0.0331 * x * x * x * x ||
            std::log(u) < 0.5 * x * x + d * (1 - v + std::log(v))) return d * v;
    }
}
bool NetworkSimulator::setErrorPattern(const std::string& bits, bool repeat) {
    std::vector<bool> parsed;
    for (char c : bits) {
        if (c == '0' || c == '1') parsed.push_back(c == '1');
        else if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    if (parsed.empty()) return false;
    errorPattern = std::move(parsed); patternOffset = 0; patternRepeats = repeat;
    return true;
}
bool NetworkSimulator::nextLoss() {
    bool lost = false;
    switch (config.lossModel) {
    case NetworkLossModel::GilbertElliott: {
        lost = uniform(lossRandom) >= (bad ? config.gilbert.h : config.gilbert.k);
        const double transition = uniform(lossRandom);
        if (bad) { if (transition < config.gilbert.r) bad = false; }
        else if (transition < config.gilbert.p) bad = true;
        break;
    }
    case NetworkLossModel::ErrorPattern:
        if (patternOffset < errorPattern.size()) {
            lost = errorPattern[patternOffset++];
            if (patternOffset == errorPattern.size() && patternRepeats) patternOffset = 0;
        }
        break;
    default: lost = uniform(lossRandom) < config.lossRate; break;
    }
    // An independent boost cannot reduce the configured base/model loss.
    if (uniform(lossRandom) < config.lossRateBoost) lost = true;
    return lost;
}
double NetworkSimulator::nextJitterMs() {
    if (config.jitterAmplitudeMs == 0) return 0;
    double innovation = 0;
    const double shape = config.jitterShape;
    switch (config.jitterDistribution) {
    case JitterDistribution::Gamma: innovation = gamma(shape) / shape - 1; break;
    case JitterDistribution::Weibull:
        innovation = std::pow(-std::log(uniform()), 1 / shape) / std::tgamma(1 + 1 / shape) - 1; break;
    case JitterDistribution::Pareto:
        innovation = std::pow(uniform(), -1 / shape) * (shape - 1) / shape - 1; break;
    default: innovation = 2 * uniform() - 1; break;
    }
    const double rho = config.jitterAutocorrelation;
    // AR(1) with variance-preserving innovations. At rho != 0 the marginal
    // law is the filtered law, not the original Gamma/Weibull/Pareto law.
    const double value = haveJitter ? rho * previousJitter + std::sqrt(1 - rho * rho) * innovation : innovation;
    previousJitter = value; haveJitter = true;
    return std::clamp(value * config.jitterAmplitudeMs, -config.maxDelayMs, config.maxDelayMs);
}
void NetworkSimulator::insert(NetworkPacket p) {
    const auto pos = std::upper_bound(pending.begin(), pending.end(), p,
        [](const NetworkPacket& a, const NetworkPacket& b) {
            if (a.arrivalTimeMs != b.arrivalTimeMs) return a.arrivalTimeMs < b.arrivalTimeMs;
            return a.sequence < b.sequence;
        });
    pending.insert(pos, std::move(p));
}
bool NetworkSimulator::submit(NetworkPacket packet, bool forceLoss) {
    ++counts.submitted;
    if (!std::isfinite(packet.sentTimeMs)) { ++counts.lost; return false; }
    const bool lost = nextLoss();
    double jitter = nextJitterMs();
    if (lost || forceLoss) { ++counts.lost; return false; }
    if (!config.allowEarlyArrivals) jitter = std::max(0.0, jitter);
    double delay = config.baseDelayMs + jitter;
    if (uniform(lateRandom) < config.lateProbability) delay += config.lateExtraDelayMs;
    // Early relative to nominal arrival is allowed; time travel before send is not.
    packet.arrivalTimeMs = packet.sentTimeMs + std::clamp(delay, 0.0, config.maxDelayMs);
    packet.duplicate = false;
    if (pending.size() >= config.maxQueuedPackets) { ++counts.overflow; return false; }
    const bool duplicate = uniform(duplicateRandom) < config.duplicateProbability;
    if (duplicate && pending.size() + 1 < config.maxQueuedPackets) {
        NetworkPacket copy = packet; copy.duplicate = true;
        copy.arrivalTimeMs += config.duplicateDelayMs;
        insert(std::move(copy)); ++counts.duplicated;
    } else if (duplicate) ++counts.overflow;
    insert(std::move(packet)); return true;
}
bool NetworkSimulator::popArrived(double nowMs, NetworkPacket& packet) {
    if (!std::isfinite(nowMs) || pending.empty() || pending.front().arrivalTimeMs > nowMs) return false;
    packet = std::move(pending.front()); pending.erase(pending.begin());
    ++counts.delivered;
    if (!packet.duplicate) {
        if (haveDelivered && packet.sequence < greatestDelivered) ++counts.reordered;
        greatestDelivered = std::max(greatestDelivered, packet.sequence); haveDelivered = true;
    }
    return true;
}
PacketJitterBuffer::PacketJitterBuffer(std::size_t n) : capacity(std::clamp<std::size_t>(n, 1, 65536)) {}
void PacketJitterBuffer::reset(uint64_t first) { nextSequence = first; packets.clear(); counts = {}; }
bool PacketJitterBuffer::push(NetworkPacket p) {
    if (!std::isfinite(p.arrivalTimeMs) || p.sequence < nextSequence) { ++counts.late; return false; }
    if (packets.find(p.sequence) != packets.end()) { ++counts.duplicates; return false; }
    if (packets.size() == capacity) { ++counts.overflow; return false; }
    packets.emplace(p.sequence, std::move(p)); ++counts.accepted; return true;
}
bool PacketJitterBuffer::take(uint64_t sequence, double deadline, NetworkPacket& packet) {
    if (!std::isfinite(deadline) || sequence < nextSequence || sequence == std::numeric_limits<uint64_t>::max()) return false;
    while (!packets.empty() && packets.begin()->first < sequence) { packets.erase(packets.begin()); ++counts.late; }
    const auto found = packets.find(sequence);
    nextSequence = sequence + 1;
    if (found == packets.end()) { ++counts.missing; return false; }
    if (found->second.arrivalTimeMs > deadline) { packets.erase(found); ++counts.late; ++counts.missing; return false; }
    packet = std::move(found->second); packets.erase(found); return true;
}
bool PacketJitterBuffer::peek(uint64_t sequence, double deadline, NetworkPacket& packet) const {
    if (!std::isfinite(deadline) || sequence < nextSequence) return false;
    const auto found = packets.find(sequence);
    if (found == packets.end() || found->second.arrivalTimeMs > deadline) return false;
    packet = found->second; return true;
}
}
