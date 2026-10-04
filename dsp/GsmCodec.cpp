#include "dsp/GsmCodec.h"
extern "C" {
#include <gsm.h>
}

namespace TelephonyDSP {

    GSMCodec::GSMCodec() : gsmState(gsm_create()), gsmDecoder(gsm_create()) { plc.reset(getFrameSize()); }
    GSMCodec::~GSMCodec() {
        if (gsmState) gsm_destroy((gsm)gsmState);
        if (gsmDecoder) gsm_destroy((gsm)gsmDecoder);
    }
    void GSMCodec::reset() {
        if (gsmState) gsm_destroy((gsm)gsmState);
        if (gsmDecoder) gsm_destroy((gsm)gsmDecoder);
        gsmState = gsm_create(); gsmDecoder = gsm_create();
        plc.reset(getFrameSize()); resetImpairments();
    }
    void GSMCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        gsm_byte encoded[33] = {};
        const bool silent = dtxEnabled && hasVoiceActivity && !voiceActive;
        if (gsmState) gsm_encode((gsm)gsmState, (gsm_signal*)in, encoded);
        // GSM's upper nibble is the framing magic 0xd, never an audio bit.
        corruptPacked(encoded, sizeof(encoded), 4);
        const bool available = exchangePacket(CodecPacketFormat::GSM, encoded, sizeof(encoded),
                                              packetLost || !gsmState, silent);
        if (!available || !gsmDecoder || playout.payload.size() != sizeof(encoded) ||
            gsm_decode((gsm)gsmDecoder, playout.payload.data(), (gsm_signal*)out) != 0) {
            plc.conceal(out, getFrameSize());
            applyDtxOutput(out, lastDtx);
            return;
        }
        if (playout.dtx) plc.conceal(out, getFrameSize());
        applyDtxOutput(out, playout.dtx);
        if (!playout.dtx) plc.storeGoodFrame(out, getFrameSize());
    }

} // namespace TelephonyDSP
