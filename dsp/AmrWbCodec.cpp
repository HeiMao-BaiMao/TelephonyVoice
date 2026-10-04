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
    AMRWBCodec::~AMRWBCodec() { if (encState) E_IF_exit(encState); if (decState) D_IF_exit(decState); }
    void AMRWBCodec::setDtxEnabled(bool enable) {
        // AMR-WB's E_IF_init() does not take a DTX flag; the DTX setting
        // is the last (5th) argument of E_IF_encode(). We just remember
        // the new flag here; the next processFrame() picks it up.
        dtxEnabled = enable;
    }
    void AMRWBCodec::setMode(int mode) {
        // The reference encoder accepts mode per frame and handles its
        // transitions internally. Resetting would erase real transition artifacts.
        amrMode = std::clamp(mode, 0, 8);
    }
    void AMRWBCodec::reset() {
        resetImpairments();
        // dtxEnabled is preserved: AMR-WB DTX is supplied per E_IF_encode
        // call, not during E_IF_init, so resetting the encoder state does
        // not require re-asking the caller for the DTX preference.
        // The selected bitrate mode survives an explicit session reset.
        if (encState) E_IF_exit(encState);
        if (decState) D_IF_exit(decState);
        encState = E_IF_init();
        decState = D_IF_init();
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRWBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        unsigned char encoded[64] = {};
        int16_t silence[320] = {};
        const int16_t* source = dtxEnabled && hasVoiceActivity && !voiceActive ? silence : in;
        const int bytes = encState ? E_IF_encode(encState, amrMode, source, encoded, dtxEnabled ? 1 : 0) : 0;
        const int ft = (encoded[0] >> 3) & 15;
        static const int speechBits[16] = {132,177,253,285,317,365,397,461,477,40,0,0,0,0,0,0};
        if (bytes > 0) corruptPacked(encoded, (size_t)bytes, 8, ft == 9 ? 35u : (size_t)speechBits[ft]);
        bool available = exchangePacket(CodecPacketFormat::AMRWB, encoded,
                                               bytes > 0 ? (size_t)bytes : 0, packetLost || bytes <= 0,
                                               ft == 9 || ft == 15);
        if (available) {
            const int receivedFt = playout.payload.empty() ? -1 : (playout.payload[0] >> 3) & 15;
            const bool validFt = receivedFt >= 0 && (receivedFt <= 9 || receivedFt == 15 || receivedFt == 14);
            available = validFt && playout.payload.size() == (size_t)(1 + (speechBits[receivedFt] + 7) / 8);
        }
        if (available) {
            std::fill(lastSerial.begin(), lastSerial.end(), 0);
            std::copy(playout.payload.begin(), playout.payload.end(), lastSerial.begin());
        }
        if (decState) {
            D_IF_decode(decState, lastSerial.data(), out, available ? 0 : 1);
            applyDtxOutput(out, available ? playout.dtx : lastDtx);
            if (available) fallbackPLC.storeGoodFrame(out, getFrameSize());
        } else {
            fallbackPLC.conceal(out, getFrameSize());
            applyDtxOutput(out, lastDtx);
        }
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
