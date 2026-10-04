#include <cstring>
#include "dsp/AmrWbCodec.h"
#include <algorithm>
#ifndef TELEPHONY_DISTRIBUTION_BUILD
#include <enc_if.h>
#include <dec_if.h>
#endif

namespace TelephonyDSP {

#ifndef TELEPHONY_DISTRIBUTION_BUILD
    AMRWBCodec::AMRWBCodec(bool dtxEnabled, int mode)
        : dtxEnabled(dtxEnabled),
          amrMode(std::clamp(mode, 0, 8)),
          lastSerial(64, 0) {
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
    void AMRWBCodec::setMode(int mode) {
        const int clamped = std::clamp(mode, 0, 8);
        if (clamped == amrMode) return;
        amrMode = clamped;
        // Rebuild the encoder/decoder state so a subsequent processFrame()
        // call picks up the new mode. reset() preserves amrMode and
        // dtxEnabled by design.
        reset();
    }
    void AMRWBCodec::reset() {
        // dtxEnabled is preserved: AMR-WB DTX is supplied per E_IF_encode
        // call, not during E_IF_init, so resetting the encoder state does
        // not require re-asking the caller for the DTX preference.
        // amrMode is also preserved: setMode() is the only path that
        // changes the bitrate, and it always invalidates the encoder
        // state itself by calling reset() after the assignment.
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

        E_IF_encode(encState, amrMode, in, lastSerial.data(), dtxEnabled ? 1 : 0); // Mode amrMode (0..8)
        D_IF_decode(decState, lastSerial.data(), out, 0);
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }
#else
    AMRWBCodec::AMRWBCodec(bool dtxEnabled, int mode)
        : encState(nullptr), decState(nullptr),
          dtxEnabled(dtxEnabled),
          amrMode(std::clamp(mode, 0, 8)),
          lastSerial(64, 0) {
        // Distribution stub: no external API calls. dtxEnabled and
        // amrMode are kept only to keep the constructor signature
        // identical to the non-distribution build, and the getters
        // report them back.
        fallbackPLC.reset(getFrameSize());
    }
    AMRWBCodec::~AMRWBCodec() {}
    void AMRWBCodec::setDtxEnabled(bool enable) { dtxEnabled = enable; }
    void AMRWBCodec::setMode(int mode) {
        const int clamped = std::clamp(mode, 0, 8);
        if (clamped == amrMode) return;
        amrMode = clamped;
        // No encoder state to rebuild in the distribution stub, but keep
        // the behavior contract identical to the non-distribution build.
        fallbackPLC.reset(getFrameSize());
    }
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
