#include "dsp/OpusCodec.h"
#include <cstring>
#if TELEPHONY_EXPERIMENTAL_NETWORK
#include <opus.h>
// Pinned submodule private ABI: compile against its real definitions rather
// than inventing OPUS_GET_MODE or hard-coding an unverified CTL number.
#include "external/opus/src/opus_private.h"
static_assert(OPUS_SET_FORCE_MODE_REQUEST == 11002 && MODE_SILK_ONLY == 1000 &&
              MODE_HYBRID == 1001 && MODE_CELT_ONLY == 1002, "Review pinned Opus force-mode ABI");
#endif

namespace TelephonyDSP {
OpusCodec::OpusCodec(int sr, int bitrate, int quality, int maxBw)
    : sampleRate(sr), bitrateBps(std::clamp(bitrate, 6000, 510000)),
      frameSize(sr / 50), complexity(std::clamp(quality, 0, 10)),
      maxBandwidth(std::clamp(maxBw, 1101, 1105)), bitstream(1276 * 3) {
    fallbackPLC.reset(frameSize, sampleRate);
    queue.reserve(64);
#if TELEPHONY_EXPERIMENTAL_NETWORK
    int error = 0;
    encoder = opus_encoder_create(sampleRate, 1, OPUS_APPLICATION_VOIP, &error);
    if (error != OPUS_OK) encoder = nullptr;
    decoder = opus_decoder_create(sampleRate, 1, &error);
    if (error != OPUS_OK) decoder = nullptr;
    if (encoder) opus_encoder_ctl((OpusEncoder*)encoder, OPUS_SET_COMPLEXITY(complexity));
    applyNetworkCtls();
#endif
}
OpusCodec::~OpusCodec() {
#if TELEPHONY_EXPERIMENTAL_NETWORK
    if (encoder) opus_encoder_destroy((OpusEncoder*)encoder);
    if (decoder) opus_decoder_destroy((OpusDecoder*)decoder);
#endif
}
void OpusCodec::reset() {
    fallbackPLC.reset(frameSize, sampleRate); resetImpairments();
    queue.clear(); nextSeq = 0; playbackFrame = 0; jitterLcg = 0x9E3779B9u;
    actualMode = OpusMode::Unknown; fecAttempts = fecRecoveredFrames = 0;
    internalTransportWarming = basePlaybackDelay() > 0;
#if TELEPHONY_EXPERIMENTAL_NETWORK
    if (encoder) opus_encoder_ctl((OpusEncoder*)encoder, OPUS_RESET_STATE);
    if (decoder) opus_decoder_ctl((OpusDecoder*)decoder, OPUS_RESET_STATE);
    applyNetworkCtls();
#endif
}
int OpusCodec::basePlaybackDelay() const {
    return playbackDelayFrames >= 0 ? playbackDelayFrames : 2 + (int)(cfgNetworkDegradation * 4);
}
int OpusCodec::derivedPacketLossPercent() const {
    return fecPacketLossPercent >= 0 ? fecPacketLossPercent : (int)std::round(100 *
        std::clamp(cfgPacketLossRate + cfgNetworkDegradation * cfgNetworkDegradation * .08f, 0.0f, .95f));
}
void OpusCodec::applyNetworkCtls() {
#if TELEPHONY_EXPERIMENTAL_NETWORK
    if (!encoder) return;
    auto* enc = (OpusEncoder*)encoder;
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(fecEnabled ? 1 : 0));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(derivedPacketLossPercent()));
    opus_encoder_ctl(enc, OPUS_SET_MAX_BANDWIDTH(maxBandwidth));
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(bitrateBps));
    opus_encoder_ctl(enc, OPUS_SET_DTX(dtxEnabled ? 1 : 0));
    const int mode = forcedMode == OpusMode::Silk ? MODE_SILK_ONLY : forcedMode == OpusMode::Hybrid ? MODE_HYBRID :
                     forcedMode == OpusMode::Celt ? MODE_CELT_ONLY : OPUS_AUTO;
    opus_encoder_ctl(enc, OPUS_SET_FORCE_MODE(mode));
    const int bandwidth = forcedMode == OpusMode::Silk ? std::min(maxBandwidth, OPUS_BANDWIDTH_WIDEBAND) :
                          forcedMode == OpusMode::Hybrid ? maxBandwidth : OPUS_AUTO;
    opus_encoder_ctl(enc, OPUS_SET_BANDWIDTH(bandwidth));
#endif
}
void OpusCodec::configureNetwork(float loss, float degradation) {
    cfgPacketLossRate = std::clamp(loss, 0.0f, .95f);
    cfgNetworkDegradation = std::clamp(degradation, 0.0f, 1.0f); applyNetworkCtls();
}
void OpusCodec::configureDtx(bool enabled, bool pureSilence) {
    ICodec::configureDtx(enabled, pureSilence); applyNetworkCtls();
}
void OpusCodec::setBitrate(int bps) { bitrateBps = std::clamp(bps, 6000, 510000); applyNetworkCtls(); }
void OpusCodec::setMaxBandwidth(int bw) { maxBandwidth = std::clamp(bw, 1101, 1105); applyNetworkCtls(); }
bool OpusCodec::supportsForceMode() {
#if TELEPHONY_EXPERIMENTAL_NETWORK
    return true;
#else
    return false;
#endif
}
bool OpusCodec::setForceMode(OpusMode mode) {
#if TELEPHONY_EXPERIMENTAL_NETWORK
    if (mode != OpusMode::Auto && mode != OpusMode::Silk && mode != OpusMode::Hybrid && mode != OpusMode::Celt) return false;
    if ((mode == OpusMode::Silk || mode == OpusMode::Hybrid) && frameDurationMs < 10) return false;
    if (mode == OpusMode::Hybrid && (sampleRate < 24000 || maxBandwidth < kOpusBandwidthSuperwideband || bitrateBps < 16000)) return false;
    if (!encoder) return false;
    const int value = mode == OpusMode::Silk ? MODE_SILK_ONLY : mode == OpusMode::Hybrid ? MODE_HYBRID :
                      mode == OpusMode::Celt ? MODE_CELT_ONLY : OPUS_AUTO;
    if (opus_encoder_ctl((OpusEncoder*)encoder, OPUS_SET_FORCE_MODE(value)) != OPUS_OK) return false;
    forcedMode = mode; applyNetworkCtls(); return true;
#else
    return mode == OpusMode::Auto;
#endif
}
void OpusCodec::setFecEnabled(bool enabled) { fecEnabled = enabled; applyNetworkCtls(); }
void OpusCodec::setFecPacketLossPercent(int percent) {
    fecPacketLossPercent = std::clamp(percent, -1, 100); applyNetworkCtls();
}
void OpusCodec::setPlaybackDelayFrames(int frames) { playbackDelayFrames = std::clamp(frames, -1, 48); }
bool OpusCodec::setExpertFrameDuration(float ms) {
    static const float durations[] = {2.5f, 5, 10, 20, 40, 60};
    int index = -1;
    for (int i = 0; i < 6; ++i) if (ms == durations[i]) index = i;
    if (index < 0 || (ms < 10 && (forcedMode == OpusMode::Silk || forcedMode == OpusMode::Hybrid))) return false;
#if TELEPHONY_EXPERIMENTAL_NETWORK
    if (!encoder || opus_encoder_ctl((OpusEncoder*)encoder,
            OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_2_5_MS + index)) != OPUS_OK) return false;
#else
    return false;
#endif
    if (frameDurationMs != ms) {
        frameDurationMs = ms; frameSize = (int)std::lround(sampleRate * ms / 1000.0);
        queue.clear(); nextSeq = 0; playbackFrame = 0; fallbackPLC.reset(frameSize, sampleRate);
    }
    return true;
}
void OpusCodec::corruptOpusPacket(int bytes) {
#if TELEPHONY_EXPERIMENTAL_NETWORK
    const unsigned char* frames[48]; opus_int16 sizes[48]; unsigned char toc; int offset = 0;
    const int count = opus_packet_parse(bitstream.data(), bytes, &toc, frames, sizes, &offset);
    // Parsing gives each coded frame, preserving the ToC, lacing and padding.
    if (count > 0) for (int i = 0; i < count; ++i)
        corruptPacked(bitstream.data() + (frames[i] - bitstream.data()), (size_t)sizes[i]);
#else
    (void)bytes;
#endif
}
bool OpusCodec::internalExchange(int bytes, bool lost, bool dtx) {
    playout.payload.clear(); playout.nextPayload.clear(); playout.dtx = false;
    const uint32_t seq = nextSeq++;
    jitterLcg = jitterLcg * 1664525u + 1013904223u;
    if (!lost && bytes > 0) {
        const int jitter = (int)std::round(((jitterLcg >> 8) & 0xffffu) / 65535.0f * cfgNetworkDegradation * 4);
        // Network arrival is separate from playout delay. The extra buffered
        // packet is what makes next-packet FEC actually reachable.
        queue.push_back({seq, (int)seq + jitter, dtx,
                         std::vector<unsigned char>(bitstream.begin(), bitstream.begin() + bytes)});
    }
    const int64_t wanted = (int64_t)playbackFrame - basePlaybackDelay();
    internalTransportWarming = wanted < 0;
    queue.erase(std::remove_if(queue.begin(), queue.end(), [wanted](const VoipPacket& p) {
        return (int64_t)p.seq < wanted;
    }), queue.end());
    bool found = false;
    for (auto& p : queue) if (wanted >= 0 && p.arrivalFrame <= playbackFrame) {
        if ((int64_t)p.seq == wanted) { playout.payload = p.data; playout.dtx = p.dtx; found = true; }
        if ((int64_t)p.seq == wanted + 1) playout.nextPayload = p.data;
    }
    if (queue.size() > 64) queue.erase(queue.begin(), queue.begin() + (queue.size() - 64));
    ++playbackFrame;
    return found;
}
void OpusCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
#if TELEPHONY_EXPERIMENTAL_NETWORK
    auto* enc = (OpusEncoder*)encoder; auto* dec = (OpusDecoder*)decoder;
    // Always encode a lost frame: Opus LBRR in the NEXT packet depends on it.
    const int bytes = enc ? opus_encode(enc, in, frameSize, bitstream.data(), (opus_int32)bitstream.size()) : 0;
    int inDtx = 0;
    if (enc) opus_encoder_ctl(enc, OPUS_GET_IN_DTX(&inDtx));
    if (bytes > 0) {
        const auto toc = bitstream[0];
        actualMode = (toc & 0x80) ? OpusMode::Celt : (toc & 0x60) == 0x60 ? OpusMode::Hybrid : OpusMode::Silk;
        corruptOpusPacket(bytes);
    }
    if (packetTransport) internalTransportWarming = false;
    const bool available = packetTransport ? exchangePacket(CodecPacketFormat::Opus, bitstream.data(),
        bytes > 0 ? (size_t)bytes : 0, packetLost || bytes <= 0, inDtx != 0) :
        internalExchange(bytes, packetLost || bytes <= 0, inDtx != 0);
    int decoded = -1;
    if (dec && available && !fecOnly)
        decoded = opus_decode(dec, playout.payload.data(), (opus_int32)playout.payload.size(), out, frameSize, 0);
    if (dec && (decoded != frameSize) && fecRecoveryEnabled && !playout.nextPayload.empty()) {
        ++fecAttempts;
        const bool hasLbrr = opus_packet_has_lbrr(playout.nextPayload.data(), (opus_int32)playout.nextPayload.size()) > 0;
        decoded = opus_decode(dec, playout.nextPayload.data(), (opus_int32)playout.nextPayload.size(), out, frameSize, 1);
        if (decoded == frameSize && hasLbrr) ++fecRecoveredFrames;
    }
    if (dec && decoded != frameSize) decoded = opus_decode(dec, nullptr, 0, out, frameSize, 0);
    if (decoded != frameSize) fallbackPLC.conceal(out, frameSize);
    applyDtxOutput(out, available ? playout.dtx : lastDtx);
    if (available && decoded == frameSize) fallbackPLC.storeGoodFrame(out, frameSize);
#else
    std::memcpy(out, in, (size_t)frameSize * sizeof(int16_t));
    applyDtxOutput(out, dtxEnabled && hasVoiceActivity && !voiceActive);
    (void)packetLost;
#endif
}
} // namespace TelephonyDSP
