#pragma once
#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"

namespace TelephonyDSP {
constexpr int kOpusBandwidthNarrowband = 1101;
constexpr int kOpusBandwidthMediumband = 1102;
constexpr int kOpusBandwidthWideband = 1103;
constexpr int kOpusBandwidthSuperwideband = 1104;
constexpr int kOpusBandwidthFullband = 1105;
enum class OpusMode { Unknown = -1, Auto = 0, Silk = 1, Hybrid = 2, Celt = 3 };

class OpusCodec : public ICodec {
public:
    OpusCodec(int sampleRate, int bitrateBps, int complexity, int maxBw = kOpusBandwidthFullband);
    ~OpusCodec() override;
    void reset() override;
    int getSampleRate() const override { return sampleRate; }
    int getFrameSize() const override { return frameSize; }
    void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
    void configureNetwork(float packetLossRate, float networkDegradation) override;
    void configureDtx(bool enabled, bool pureSilence) override;
    void setBitrate(int bps);
    int getBitrate() const { return bitrateBps; }
    void setMaxBandwidth(int bw);
    int getMaxBandwidth() const { return maxBandwidth; }
    // Public expert-duration CTL. Changes framing, so caller must resize/reprime
    // its PCM buffers; encoder/decoder history is otherwise retained.
    bool setExpertFrameDuration(float milliseconds);
    float getExpertFrameDuration() const { return frameDurationMs; }
    OpusMode getActualMode() const { return actualMode; }
    // Version-pinned bundled private CTL (not stable public Opus API).
    // Actual ToC readout remains authoritative during codec transitions.
    bool setForceMode(OpusMode mode);
    OpusMode getForcedMode() const { return forcedMode; }
    static bool supportsForceMode();
    void setFecEnabled(bool enable);
    void setFecRecoveryEnabled(bool enable) { fecRecoveryEnabled = enable; }
    void setFecOnly(bool enable) { fecOnly = enable; }
    // -1 restores loss estimate derived from configureNetwork.
    void setFecPacketLossPercent(int percent);
    void setPlaybackDelayFrames(int frames);
    uint64_t getFecRecoveredFrames() const { return fecRecoveredFrames; }
    uint64_t getFecAttempts() const { return fecAttempts; }
    bool isInternalTransportWarming() const { return internalTransportWarming; }
    int getInternalDelayFrames() const { return basePlaybackDelay(); }
private:
    int sampleRate, bitrateBps, frameSize, complexity, maxBandwidth;
    float frameDurationMs = 20.0f;
    void* encoder = nullptr;
    void* decoder = nullptr;
    std::vector<unsigned char> bitstream;
    WaveformConcealer fallbackPLC;
    struct VoipPacket {
        uint32_t seq;
        int arrivalFrame;
        bool dtx;
        std::vector<unsigned char> data;
    };
    std::vector<VoipPacket> queue;
    uint32_t nextSeq = 0;
    int playbackFrame = 0;
    float cfgPacketLossRate = 0, cfgNetworkDegradation = 0;
    uint32_t jitterLcg = 0x9E3779B9u;
    bool fecEnabled = true, fecRecoveryEnabled = true, fecOnly = false;
    bool internalTransportWarming = true;
    int fecPacketLossPercent = -1, playbackDelayFrames = -1;
    OpusMode actualMode = OpusMode::Unknown, forcedMode = OpusMode::Auto;
    uint64_t fecRecoveredFrames = 0, fecAttempts = 0;
    int basePlaybackDelay() const;
    int derivedPacketLossPercent() const;
    void applyNetworkCtls();
    void corruptOpusPacket(int bytes);
    bool internalExchange(int bytes, bool lost, bool dtx);
};
} // namespace TelephonyDSP
