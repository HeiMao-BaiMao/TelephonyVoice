#pragma once

#include "dsp/Types.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace TelephonyDSP {

// Encoded packet transport is deliberately independent of PCM processing.
// No sockets: the owner supplies an in-process loss/jitter/reordering queue.
enum class CodecPacketFormat { G711MuLaw, G711ALaw, GSM, AMRNB, AMRWB, EVSG192, Opus, LinearPcm16 };
struct CodecPacketInfo {
    CodecPacketFormat format;
    int sampleRate;
    int frameSamples;
    bool dtx = false;
    bool amrWbIo = false;
    bool amrWbSidUpdate = false;
    int amrWbSidMode = -1;
};
struct CodecPlayout {
    std::vector<uint8_t> payload;
    // Optional next packet, retained in the transport for its normal slot.
    std::vector<uint8_t> nextPayload;
    bool dtx = false;
    // Session-relative source clock metadata, preserved by the packet queue.
    bool hasTiming = false;
    uint64_t sequence = 0;
    double sentTimeMs = 0, arrivalTimeMs = 0, playoutTimeMs = 0;
    double sourceTimestampMs = 0;
};
struct CodecEncodedPacket {
    CodecPacketInfo info;
    std::vector<uint8_t> payload;
    uint64_t sequence = 0;
    double sourceTimestampMs = 0;
    bool lost = false;
};
class ICodecTransport {
public:
    virtual ~ICodecTransport() = default;
    virtual bool exchange(const CodecPacketInfo& info, const uint8_t* encoded,
                          size_t bytes, bool packetLost, CodecPlayout& received) = 0;
    virtual bool supportsBatchIngress() const { return false; }
    // Feed zero/one/multiple sender packets at this receiver deadline, then
    // return every arrived encoded packet. The codec's real JBM does playout.
    virtual void exchangeBatch(const std::vector<CodecEncodedPacket>& packets,
                               double receiverDeadlineMs,
                               std::vector<CodecPlayout>& arrivals) {
        (void)packets; (void)receiverDeadlineMs; arrivals.clear();
    }
};

class ICodec {
public:
    virtual ~ICodec() = default;
    virtual void reset() = 0;
    virtual int getSampleRate() const = 0;
    virtual int getFrameSize() const = 0;
    virtual void processFrame(const int16_t* in, int16_t* out, bool packetLost) = 0;
    virtual void configureNetwork(float, float) {}
    virtual void setSpectralConcealment(bool) {}
    void setTransport(ICodecTransport* transport) { packetTransport = transport; }
    virtual void configureDtx(bool enabled, bool pureSilence) {
        dtxEnabled = enabled; pureSilenceDtx = pureSilence;
    }
    // Channel's VAD decision. Native codecs retain their own SID hangover;
    // legacy PCM/GSM codecs use this decision for simulated DTX.
    void setVoiceActivity(bool active) { voiceActive = active; hasVoiceActivity = true; }
    bool isDtxActive() const { return lastDtx; }
    void configureBitErrors(float probability, uint32_t seed = 0xB17E7701u) {
        bitErrorRate = std::isfinite(probability) ? std::clamp(probability, 0.0f, 1.0f) : 0.0f;
        if (seed != bitErrorSeed) { bitErrorSeed = seed; bitErrorState = seed; }
    }
    uint64_t getInjectedBitErrors() const { return injectedBitErrors; }
protected:
    ICodecTransport* packetTransport = nullptr; // owner outlives this codec
    bool dtxEnabled = true;
    bool pureSilenceDtx = false;
    bool voiceActive = true;
    bool hasVoiceActivity = false;
    bool lastDtx = false;
    float bitErrorRate = 0.0f;
    uint32_t bitErrorSeed = 0xB17E7701u, bitErrorState = 0xB17E7701u;
    uint64_t injectedBitErrors = 0;
    CodecPlayout playout;
    void resetImpairments() { bitErrorState = bitErrorSeed; injectedBitErrors = 0; lastDtx = false; }
    bool drawBitError() {
        if (bitErrorRate <= 0) return false;
        bitErrorState = bitErrorState * 1664525u + 1013904223u;
        const bool flip = (double)bitErrorState / 4294967296.0 < bitErrorRate;
        injectedBitErrors += flip ? 1 : 0;
        return flip;
    }
    // Preserve container/ToC/length fields. BER only touches coded payload bits.
    void corruptPacked(uint8_t* bytes, size_t count, size_t protectedBits = 0,
                       size_t payloadBits = 0) {
        const size_t last = payloadBits ? std::min(count * 8, protectedBits + payloadBits) : count * 8;
        for (size_t bit = protectedBits; bit < last; ++bit)
            if (drawBitError()) bytes[bit / 8] ^= (uint8_t)(0x80u >> (bit % 8));
    }
    void corruptG192(uint8_t* bytes, size_t count) {
        if (count < 4) return;
        uint16_t bits; std::memcpy(&bits, bytes + 2, 2);
        if (count < 4u + 2u * bits) return;
        for (size_t i = 0; i < bits; ++i) {
            uint16_t symbol; std::memcpy(&symbol, bytes + 4 + i * 2, 2);
            if ((symbol == 0x007f || symbol == 0x0081) && drawBitError()) {
                symbol = symbol == 0x007f ? 0x0081 : 0x007f;
                std::memcpy(bytes + 4 + i * 2, &symbol, 2);
            }
        }
    }
    bool exchangePacket(CodecPacketFormat format, const uint8_t* data, size_t bytes,
                        bool lost, bool dtx = false, bool amrWbIo = false,
                        bool amrWbSidUpdate = false, int amrWbSidMode = -1) {
        playout.payload.clear(); playout.nextPayload.clear(); playout.dtx = false; playout.hasTiming = false;
        if (packetTransport)
            return packetTransport->exchange({format, getSampleRate(), getFrameSize(), dtx, amrWbIo, amrWbSidUpdate, amrWbSidMode},
                                             data, bytes, lost, playout);
        if (lost || !data || !bytes) return false;
        playout.payload.assign(data, data + bytes); playout.dtx = dtx;
        return true;
    }
    void applyDtxOutput(int16_t* out, bool active) {
        lastDtx = active;
        if (active && pureSilenceDtx) std::fill(out, out + getFrameSize(), int16_t(0));
    }
};

} // namespace TelephonyDSP
