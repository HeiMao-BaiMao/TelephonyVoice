#include "dsp/EvsCodec.h"
#include <array>
#include <complex>
#include <cstring>

namespace TelephonyDSP {
EVSCodec::EVSCodec(int rate, int bitrate, EVS_Bandwidth bandwidth, bool scVbr, int sidInterval)
    : sampleRate(rate), bitrateBps(bitrate), nativeBitrateBps(bitrate), fixedBitrateBps(bitrate == 5900 ? 13200 : bitrate), fixedMaxBw(bandwidth), nativeMaxBw(bandwidth),
      nativeScVbrEnabled(scVbr), maxBw(bandwidth), activeBw(bandwidth),
      detectedBw(bandwidth), scVbrEnabled(scVbr),
      dtxSidInterval(sidInterval >= 3 && sidInterval <= 100 ? sidInterval : 0), pendingBw(bandwidth) {
    reset();
}
EVS_EncOptions EVSCodec::options() const {
    EVS_EncOptions opts; evs_enc_options_init(&opts);
    opts.dtx_enable = dtxEnabled; opts.dtx_sid_interval = dtxSidInterval;
    opts.sc_vbr_enable = scVbrEnabled; opts.amr_wb_io = amrWbIo;
    return opts;
}
EVSCodec::~EVSCodec() {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    evs_enc_destroy(enc); evs_dec_destroy(dec);
#endif
}
void EVSCodec::reset() {
    fallbackPLC.reset(getFrameSize()); resetImpairments();
    bandwidthHold = 0; activeBw = detectedBw = pendingBw = maxBw;
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    evs_enc_destroy(enc); evs_dec_destroy(dec); enc = nullptr; dec = nullptr;
    auto opts = options(); int rate = bitrateBps; auto bw = maxBw;
    if (evs_enc_normalize_config(sampleRate, &rate, &bw, &opts) == EVS_OK) {
        bitrateBps = rate; scVbrEnabled = opts.sc_vbr_enable != 0; activeBw = detectedBw = pendingBw = bw;
        if (!amrWbIo) { maxBw = nativeMaxBw = bw; nativeBitrateBps = rate; nativeScVbrEnabled = scVbrEnabled; }
        enc = evs_enc_create_ex(sampleRate, rate, bw, &opts);
        dec = evs_dec_create_ex(sampleRate, rate, amrWbIo);
    }
    bitstream.resize((size_t)evs_max_bitstream_bytes(sampleRate));
#endif
}
bool EVSCodec::applyConfiguration(int bitrate, EVS_Bandwidth bw, bool io, bool storeCeiling) {
    const int requestedBitrate = bitrate; const auto requestedBw = bw;
    auto opts = options(); opts.amr_wb_io = io; if (io) opts.sc_vbr_enable = 0;
    if (evs_enc_normalize_config(sampleRate, &bitrate, &bw, &opts) != EVS_OK) return false;
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    if (!enc || evs_enc_reconfigure(enc, bitrate, bw, &opts) != EVS_OK) return false;
#endif
    if (io && !amrWbIo) {
        nativeBitrateBps = bitrateBps; nativeMaxBw = maxBw; nativeScVbrEnabled = scVbrEnabled;
    }
    if (!io && storeCeiling && opts.sc_vbr_enable && requestedBitrate != 5900) {
        auto fixedOpts = opts; fixedOpts.sc_vbr_enable = 0;
        int fixedRate = requestedBitrate; auto fixedBw = requestedBw;
        if (evs_enc_normalize_config(sampleRate, &fixedRate, &fixedBw, &fixedOpts) == EVS_OK) {
            fixedBitrateBps = fixedRate; fixedMaxBw = fixedBw;
        }
    }
    bitrateBps = bitrate; activeBw = bw; amrWbIo = io; scVbrEnabled = opts.sc_vbr_enable != 0;
    // IO has a fixed WB coding ceiling; it must not replace the user's
    // native profile. Auto bandwidth similarly only changes the live ceiling.
    if (!io && storeCeiling) {
        maxBw = nativeMaxBw = bw; nativeBitrateBps = bitrate; nativeScVbrEnabled = scVbrEnabled;
        if (!scVbrEnabled) { fixedBitrateBps = bitrate; fixedMaxBw = bw; }
    }
    return true;
}
bool EVSCodec::reconfigure(int bitrate, EVS_Bandwidth bw, bool io) {
    return applyConfiguration(bitrate, bw, io, true);
}
bool EVSCodec::setBitrate(int bitrate) { return reconfigure(bitrate, maxBw, amrWbIo); }
bool EVSCodec::setMaxBandwidth(EVS_Bandwidth bw) {
    if (!amrWbIo) return reconfigure(bitrateBps, bw, false);
    auto opts = options(); opts.amr_wb_io = 0; opts.sc_vbr_enable = nativeScVbrEnabled;
    int bitrate = nativeBitrateBps;
    if (evs_enc_normalize_config(sampleRate, &bitrate, &bw, &opts) != EVS_OK) return false;
    maxBw = nativeMaxBw = bw; return true;
}
bool EVSCodec::setAmrWbIo(bool enabled, int bitrate) {
    if (enabled == amrWbIo && (!enabled || bitrate == bitrateBps)) return true;
    if (enabled) return reconfigure(bitrate, EVS_WB, true);
    const bool previousScVbr = scVbrEnabled;
    scVbrEnabled = nativeScVbrEnabled;
    if (reconfigure(nativeBitrateBps, nativeMaxBw, false)) return true;
    scVbrEnabled = previousScVbr; return false;
}
void EVSCodec::configureDtx(bool enabled, bool pureSilence) {
    const bool before = dtxEnabled;
    ICodec::configureDtx(enabled, pureSilence);
    if (before != enabled && !applyConfiguration(bitrateBps, activeBw, amrWbIo, false)) dtxEnabled = before;
}
void EVSCodec::setScVbrEnabled(bool enabled) {
    if (scVbrEnabled == enabled) return;
    const bool before = scVbrEnabled;
    if (enabled && !amrWbIo) { fixedBitrateBps = bitrateBps; fixedMaxBw = maxBw; }
    scVbrEnabled = enabled;
    if (!reconfigure(enabled ? 5900 : fixedBitrateBps, enabled ? maxBw : fixedMaxBw, false)) scVbrEnabled = before;
}
void EVSCodec::setDtxSidInterval(int interval) {
    interval = interval >= 3 && interval <= 100 ? interval : 0;
    if (interval == dtxSidInterval) return;
    const int before = dtxSidInterval; dtxSidInterval = interval;
    if (!applyConfiguration(bitrateBps, activeBw, amrWbIo, false)) dtxSidInterval = before;
}
void EVSCodec::setAutoBandwidth(bool enabled) {
    if (enabled == autoBandwidth) return;
    autoBandwidth = enabled; bandwidthHold = 0;
    if (!enabled) applyConfiguration(bitrateBps, maxBw, amrWbIo, false);
}
EVS_Bandwidth estimateEvsInputBandwidth(const int16_t* input, int frameSize, int sampleRate, EVS_Bandwidth ceiling, EVS_Bandwidth current) {
    // 256-point Hann-windowed spectrum: no allocation or codec reset.
    constexpr int size = 256;
    std::array<std::complex<float>, size> bins{};
    const int count = std::min(frameSize, size);
    double inputEnergy = 0;
    for (int i = 0; i < count; ++i) {
        const float window = .5f - .5f * std::cos(6.28318530718f * i / (count - 1));
        bins[i] = input[i] * window; inputEnergy += (double)input[i] * input[i];
    }
    if (inputEnergy < count * 16.0) return current; // silence provides no bandwidth evidence
    for (unsigned i = 1, j = 0; i < size; ++i) {
        unsigned bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit; if (i < j) std::swap(bins[i], bins[j]);
    }
    for (int length = 2; length <= size; length <<= 1) {
        const std::complex<float> root = std::polar(1.0f, -6.28318530718f / length);
        for (int i = 0; i < size; i += length) {
            std::complex<float> factor(1, 0);
            for (int k = 0; k < length / 2; ++k) {
                auto even = bins[i + k], odd = bins[i + k + length / 2] * factor;
                bins[i + k] = even + odd; bins[i + k + length / 2] = even - odd; factor *= root;
            }
        }
    }
    double energy[4] = {};
    for (int k = 1; k <= size / 2; ++k) {
        const float frequency = (float)k * sampleRate / size;
        const int band = frequency <= 4000 ? 0 : frequency <= 8000 ? 1 : frequency <= 16000 ? 2 : 3;
        energy[band] += std::norm(bins[k]);
    }
    const double total = energy[0] + energy[1] + energy[2] + energy[3];
    int candidate = 0;
    for (int band = 1; band <= (int)ceiling; ++band)
        if (energy[band] > total * .015) candidate = band;
    return (EVS_Bandwidth)candidate;
}
void EVSCodec::estimateBandwidth(const int16_t* input) {
    if (!autoBandwidth || amrWbIo) return;
    int candidate = (int)estimateEvsInputBandwidth(input, getFrameSize(), sampleRate, maxBw, activeBw);
    // Native EVS >24.4kbps cannot code NB. Avoid illegal auto requests.
    if (candidate == 0 && bitrateBps > 24400) candidate = 1;
    detectedBw = (EVS_Bandwidth)candidate;
    if (detectedBw != pendingBw) { pendingBw = detectedBw; bandwidthHold = 1; }
    else ++bandwidthHold;
    if (pendingBw != activeBw && bandwidthHold >= (pendingBw > activeBw ? 2 : 8))
        applyConfiguration(bitrateBps, pendingBw, false, false);
}
void EVSCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
    const int fs = getFrameSize();
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    if (!enc || !dec) { std::fill(out, out + fs, int16_t(0)); return; }
    estimateBandwidth(in);
    std::array<int16_t, 960> silence{};
    const int16_t* source = dtxEnabled && hasVoiceActivity && !voiceActive ? silence.data() : in;
    int bytes = 0, bits = 0, sidUpdate = 0, sidMode = -1;
    const bool encoded = evs_enc_process(enc, source, fs, bitstream.data(), (int)bitstream.size(), &bytes) == EVS_OK;
    evs_enc_get_last_frame_info(enc, &bits, &sidUpdate, &sidMode);
    if (encoded) corruptG192(bitstream.data(), (size_t)bytes);
    const bool available = exchangePacket(CodecPacketFormat::EVSG192, bitstream.data(),
        encoded ? (size_t)bytes : 0, packetLost || !encoded, bits == 0 || bits == 35 || bits == 48,
        amrWbIo, sidUpdate != 0, sidMode);
    int samples = 0;
    int status = available ? evs_dec_process(dec, playout.payload.data(), (int)playout.payload.size(), out, &samples) : EVS_ERROR;
    if (status != EVS_OK || samples != fs) status = evs_dec_process_lost(dec, out, &samples);
    if (status != EVS_OK || samples != fs) fallbackPLC.conceal(out, fs);
    applyDtxOutput(out, available ? playout.dtx : lastDtx);
    if (available) fallbackPLC.storeGoodFrame(out, fs);
#else
    if (packetLost) fallbackPLC.conceal(out, fs); else std::memcpy(out, in, (size_t)fs * sizeof(int16_t));
    applyDtxOutput(out, dtxEnabled && hasVoiceActivity && !voiceActive);
#endif
}
} // namespace TelephonyDSP
