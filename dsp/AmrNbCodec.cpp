#include <cstring>
#include "dsp/AmrNbCodec.h"
#ifndef TELEPHONY_DISTRIBUTION_BUILD
#include <interf_enc.h>
#include <interf_dec.h>
#endif

namespace TelephonyDSP {

#ifndef TELEPHONY_DISTRIBUTION_BUILD
    AMRNBCodec::AMRNBCodec(bool dtxEnabled, int mode)
        : dtxEnabled(dtxEnabled),
          amrMode(mode < 0 ? 0 : (mode > 7 ? 7 : mode)),
          lastSerial(32, 0) {
        encState = Encoder_Interface_init(dtxEnabled ? 1 : 0);
        decState = Decoder_Interface_init();
        fallbackPLC.reset(getFrameSize());
    }
    AMRNBCodec::~AMRNBCodec() { if (encState) Encoder_Interface_exit(encState); if (decState) Decoder_Interface_exit(decState); }
    void AMRNBCodec::setDtxEnabled(bool enable) {
        if (dtxEnabled == enable) return;
        // Allocate the replacement encoder first so we don't drop the
        // existing state if init fails; on failure we keep both the old
        // encoder and the previous dtxEnabled value.
        void* newEnc = Encoder_Interface_init(enable ? 1 : 0);
        if (!newEnc) return;
        Encoder_Interface_exit(encState);
        encState = newEnc;
        dtxEnabled = enable;
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRNBCodec::setMode(int mode) {
        // The reference encoder accepts mode per frame and handles its
        // transitions internally. Resetting would erase real transition artifacts.
        amrMode = std::clamp(mode, 0, 7);
    }
    void AMRNBCodec::reset() {
        resetImpairments();
        if (encState) Encoder_Interface_exit(encState);
        if (decState) Decoder_Interface_exit(decState);
        encState = Encoder_Interface_init(dtxEnabled ? 1 : 0);
        decState = Decoder_Interface_init();
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRNBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        unsigned char encoded[64] = {};
        int16_t silence[320] = {};
        const int16_t* source = dtxEnabled && hasVoiceActivity && !voiceActive ? silence : in;
        const int bytes = encState ? Encoder_Interface_Encode(encState, (Mode)amrMode, const_cast<short*>(source), encoded, 0) : 0;
        const int ft = (encoded[0] >> 3) & 15;
        static const int speechBits[16] = {95,103,118,134,148,159,204,244,39,0,0,0,0,0,0,0};
        if (bytes > 0) corruptPacked(encoded, (size_t)bytes, 8, ft == 8 ? 35u : (size_t)speechBits[ft]);
        bool available = exchangePacket(CodecPacketFormat::AMRNB, encoded,
                                               bytes > 0 ? (size_t)bytes : 0, packetLost || bytes <= 0,
                                               ft == 8 || ft == 15);
        if (available) {
            const int receivedFt = playout.payload.empty() ? -1 : (playout.payload[0] >> 3) & 15;
            const bool validFt = receivedFt >= 0 && (receivedFt <= 8 || receivedFt == 15);
            available = validFt && playout.payload.size() == (size_t)(1 + (speechBits[receivedFt] + 7) / 8);
        }
        if (available) {
            std::fill(lastSerial.begin(), lastSerial.end(), 0);
            std::copy(playout.payload.begin(), playout.payload.end(), lastSerial.begin());
        }
        if (decState) {
            Decoder_Interface_Decode(decState, lastSerial.data(), out, available ? 0 : 1);
            applyDtxOutput(out, available ? playout.dtx : lastDtx);
            if (available) fallbackPLC.storeGoodFrame(out, getFrameSize());
        } else {
            fallbackPLC.conceal(out, getFrameSize());
            applyDtxOutput(out, lastDtx);
        }
    }

#else
    AMRNBCodec::AMRNBCodec(bool dtxEnabled, int mode)
        : encState(nullptr), decState(nullptr), dtxEnabled(dtxEnabled),
          amrMode(mode < 0 ? 0 : (mode > 7 ? 7 : mode)),
          lastSerial(32, 0) {
        // Distribution stub: no external API calls. dtxEnabled and amrMode
        // are kept only to keep the constructor signature identical to the
        // non-distribution build, and isDtxEnabled()/getMode() report them
        // back unchanged.
        fallbackPLC.reset(getFrameSize());
    }
    AMRNBCodec::~AMRNBCodec() {}
    void AMRNBCodec::setDtxEnabled(bool enable) { dtxEnabled = enable; }
    void AMRNBCodec::setMode(int mode) { amrMode = mode < 0 ? 0 : (mode > 7 ? 7 : mode); }
    void AMRNBCodec::reset() { fallbackPLC.reset(getFrameSize()); }
    void AMRNBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            fallbackPLC.conceal(out, getFrameSize());
            return;
        }
        std::memcpy(out, in, 160 * sizeof(int16_t));
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }
#endif

} // namespace TelephonyDSP
