#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace TelephonyDSP {
// Stable ordering: append controls only. IDs 200+index are shared by the VST,
// CLI, state extension and DSP. All new simulation is opt-in for old sessions.
enum class AdvancedControl : size_t {
    NetworkEnabled, BandwidthNarrowing, FilterCascade, JitterMs, JitterDistribution,
    JitterShape, JitterCorrelation, BurstMean, LossBoost, GilbertEnabled,
    GilbertP, GilbertR, GilbertGoodLoss, GilbertBadLoss, DuplicateRate,
    PlaybackDelayMs, BitErrorRate, DtxEnabled, PureSilence, PsdNoise,
    VadMode, DtmfDigit, DtmfLevel, DtmfOnMs, DtmfGapMs,
    HostClock, OpusFrameDuration, OpusFecPercent, OpusFecEnabled,
    EvsAutoBandwidth, EvsAmrWbIo, ModeTransient,
    ClockDriftPpm, EchoEnabled, EchoDelayMs, EchoGain, EchoCutoffHz,
    FadingEnabled, FadingDopplerHz, CarrierToInterferenceDb, AdaptiveBitrate,
    HandoverIntervalMs, HandoverGapMs, TandemNarrowband,
    PacketFormat, AmrOctetAligned, Redundancy, EvsPayloadStyle, LateProbability, LateDelayMs, OpusForceMode, OpusFecOnly, EvsIoMode, Count
};
struct AdvancedDescriptor {
    const char* key; const char* title; const char* units;
    double minimum, maximum, initial; int steps;
};
inline constexpr std::array<AdvancedDescriptor, static_cast<size_t>(AdvancedControl::Count)> advancedDescriptors {{
    {"network-enabled","Independent network","",0,1,0,1},
    {"bandwidth-narrowing","Bandwidth narrowing","",0,1,0,0},
    {"filter-cascade","Filter cascade","",0,1,1,1},
    {"jitter-ms","Jitter amplitude","ms",0,100,0,0},
    {"jitter-distribution","Jitter distribution","",0,3,0,3},
    {"jitter-shape","Jitter shape","",0.2,10,2,0},
    {"jitter-correlation","Jitter correlation","",0,0.99,0,0},
    {"burst-mean","Mean loss burst","frames",1,20,1,0},
    {"loss-boost","Additional loss","",0,0.95,0,0},
    {"gilbert-enabled","Gilbert-Elliott loss","",0,1,0,1},
    {"gilbert-p","Good to bad probability","",0,1,0.01,0},
    {"gilbert-r","Bad to good probability","",0,1,0.5,0},
    {"gilbert-good-loss","Good-state loss","",0,1,0,0},
    {"gilbert-bad-loss","Bad-state loss","",0,1,1,0},
    {"duplicate-rate","Duplicate probability","",0,1,0,0},
    {"playback-delay-ms","Packet playout delay","ms",0,100,20,0},
    {"ber","Payload bit error rate","",0,0.1,0,0},
    {"dtx","Legacy voice DTX","",0,1,0,1},
    {"pure-silence","DTX pure silence","",0,1,0,1},
    {"psd-noise","Spectral comfort noise","",0,1,1,1},
    {"vad-mode","Voice activity detector","",0,1,0,1},
    {"dtmf-digit","DTMF digit (-1 off)","",-1,15,-1,16},
    {"dtmf-level","DTMF level","",0,0.5,0.15,0},
    {"dtmf-on-ms","DTMF tone duration","ms",50,100,80,0},
    {"dtmf-gap-ms","DTMF gap","ms",50,500,50,0},
    {"host-clock","Follow host timeline","",0,1,0,1},
    {"opus-frame-duration","Opus frame duration","",0,5,3,5},
    {"opus-fec-percent","Opus expected loss","%",0,100,0,100},
    {"opus-fec","Opus in-band FEC","",0,1,1,1},
    {"evs-auto-bandwidth","EVS auto bandwidth","",0,1,0,1},
    {"evs-amr-wb-io","EVS AMR-WB IO","",0,1,0,1},
    {"mode-transient","Mode-change artifact","",0,1,0,0},
    {"clock-drift-ppm","Receiver clock drift","ppm",-50,50,0,0},
    {"echo","Hybrid line echo","",0,1,0,1},
    {"echo-delay-ms","Echo delay","ms",1,200,32,0},
    {"echo-gain","Echo feedback gain","",0,0.45,0.2,0},
    {"echo-cutoff-hz","Echo low-pass","Hz",100,8000,3400,0},
    {"fading","Rayleigh fading","",0,1,0,1},
    {"fading-doppler-hz","Fading Doppler","Hz",0.1,200,5,0},
    {"carrier-interference-db","Carrier / interference","dB",-10,40,20,0},
    {"adaptive-bitrate","C/I rate adaptation","",0,1,0,1},
    {"handover-interval-ms","Handover interval (0 off)","ms",0,60000,0,0},
    {"handover-gap-ms","Handover interruption","ms",50,200,100,0},
    {"tandem-narrowband","Narrowband tandem","",0,1,0,1},
    {"packet-format","Internal RTP packetization","",0,1,0,1},
    {"amr-octet-aligned","AMR octet-aligned","",0,1,1,1},
    { "redundancy","RFC2198 redundancy","",0,1,0,1},
    {"evs-payload-style","EVS payload style","",0,2,0,2},
    {"late-probability","Late packet probability","",0,1,0,0},
    {"late-delay-ms","Additional late delay","ms",0,500,100,0},
    {"opus-force-mode","Opus force mode","",0,3,0,3},
    {"opus-fec-only","Opus FEC-only playout","",0,1,0,1},
    {"evs-io-mode","EVS AMR-WB IO bitrate mode","",0,8,2,8}
}};
struct AdvancedSettings {
    std::array<double, advancedDescriptors.size()> normalized{};
    AdvancedSettings() {
        for(size_t i=0;i<normalized.size();++i) {
            const auto& d=advancedDescriptors[i]; normalized[i]=(d.initial-d.minimum)/(d.maximum-d.minimum);
        }
    }
    double get(AdvancedControl c) const {
        const size_t i=static_cast<size_t>(c); const auto& d=advancedDescriptors[i];
        double n=std::isfinite(normalized[i]) ? std::clamp(normalized[i],0.0,1.0) : (d.initial-d.minimum)/(d.maximum-d.minimum);
        if(d.steps) n=std::round(n*d.steps)/d.steps;
        return d.minimum+n*(d.maximum-d.minimum);
    }
    bool enabled(AdvancedControl c) const { return get(c)>0.5; }
    bool setPlain(size_t i,double value) {
        if(i>=normalized.size() || !std::isfinite(value)) return false;
        const auto& d=advancedDescriptors[i];
        if(value<d.minimum || value>d.maximum) return false;
        if(d.steps && std::abs((value-d.minimum)*d.steps/(d.maximum-d.minimum)-std::round((value-d.minimum)*d.steps/(d.maximum-d.minimum)))>1e-8) return false;
        normalized[i]=(value-d.minimum)/(d.maximum-d.minimum); return true;
    }
    bool operator==(const AdvancedSettings&) const = default;
};
struct ProcessingTelemetry {
    double inputPeak=0, outputPeak=0, measuredLoss=0, jitterMs=0;
    uint64_t packets=0, lost=0, duplicates=0, late=0;
    int opusMode=-1;
};
} // namespace TelephonyDSP
