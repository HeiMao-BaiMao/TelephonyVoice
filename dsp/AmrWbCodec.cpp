#include "dsp/AmrWbCodec.h"
#ifndef TELEPHONY_DISTRIBUTION_BUILD
#include <enc_if.h>
#include <dec_if.h>
#endif

namespace TelephonyDSP {

#ifndef TELEPHONY_DISTRIBUTION_BUILD
    AMRWBCodec::AMRWBCodec(bool dtxEnabled) : dtxEnabled(dtxEnabled), lastSerial(64, 0) {
        encState = E_IF_init();
        decState = D_IF_init();
        fallbackPLC.reset(getFrameSize());
    }
    AMRWBCodec::~AMRWBCodec() { E_IF_exit(encState); D_IF_exit(decState); }
    void AMRWBCodec::setDtxEnabled(bool enable) {
        // AMR-WB's E_IF_init() does not take a DTX flag; the DTX setting
        // is the last (5th) argument of E_IF_encode(). We just remember
        // the new flag here; the next processFrame() picks it up.
        dtxEnabled = enable;
    }
    void AMRWBCodec::reset() {
        // dtxEnabled is preserved: AMR-WB DTX is supplied per E_IF_encode
        // call, not during E_IF_init, so resetting the encoder state does
        // not require re-asking the caller for the DTX preference.
        E_IF_exit(encState);
        D_IF_exit(decState);
        encState = E_IF_init();
        decState = D_IF_init();
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRWBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            D_IF_decode(decState, lastSerial.data(), out, 1);
            fallbackPLC.storeGoodFrame(out, getFrameSize());
            return;
        }

        E_IF_encode(encState, 2, in, lastSerial.data(), dtxEnabled ? 1 : 0); // Mode 2: 12.65 kbit/s
        D_IF_decode(decState, lastSerial.data(), out, 0);
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }
#else
    AMRWBCodec::AMRWBCodec(bool dtxEnabled) : encState(nullptr), decState(nullptr), dtxEnabled(dtxEnabled), lastSerial(64, 0) {
        // Distribution stub: no external API calls. dtxEnabled is kept
        // only to keep the constructor signature identical to the
        // non-distribution build, and isDtxEnabled() reports it back.
        fallbackPLC.reset(getFrameSize());
    }
    AMRWBCodec::~AMRWBCodec() {}
    void AMRWBCodec::setDtxEnabled(bool enable) { dtxEnabled = enable; }
    void AMRWBCodec::reset() { fallbackPLC.reset(getFrameSize()); }
    void AMRWBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            fallbackPLC.conceal(out, getFrameSize());
            return;
        }
        std::memcpy(out, in, 320 * sizeof(int16_t));
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }
#endif

} // namespace TelephonyDSP
