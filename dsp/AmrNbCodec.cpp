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
    AMRNBCodec::~AMRNBCodec() { Encoder_Interface_exit(encState); Decoder_Interface_exit(decState); }
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
        const int clamped = mode < 0 ? 0 : (mode > 7 ? 7 : mode);
        if (amrMode == clamped) return;
        amrMode = clamped;
        // The encoder state was created with no mode awareness, but we still
        // drop it on a mode change so internal history doesn't carry over
        // across bitrate switches. reset() rebuilds enc/dec state and clears
        // the last serial buffer.
        reset();
    }
    void AMRNBCodec::reset() {
        Encoder_Interface_exit(encState);
        Decoder_Interface_exit(decState);
        encState = Encoder_Interface_init(dtxEnabled ? 1 : 0);
        decState = Decoder_Interface_init();
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRNBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            Decoder_Interface_Decode(decState, lastSerial.data(), (short*)out, 1);
            fallbackPLC.storeGoodFrame(out, getFrameSize());
            return;
        }

        Encoder_Interface_Encode(encState, (Mode)amrMode, (short*)in, lastSerial.data(), 0);
        Decoder_Interface_Decode(decState, lastSerial.data(), (short*)out, 0);
        fallbackPLC.storeGoodFrame(out, getFrameSize());
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
