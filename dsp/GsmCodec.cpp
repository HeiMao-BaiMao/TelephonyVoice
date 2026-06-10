#include "dsp/GsmCodec.h"
extern "C" {
#include <gsm.h>
}

namespace TelephonyDSP {

    GSMCodec::GSMCodec() { gsmState = gsm_create(); plc.reset(getFrameSize()); }
    GSMCodec::~GSMCodec() { if (gsmState) gsm_destroy((gsm)gsmState); }
    void GSMCodec::reset() { if (gsmState) gsm_destroy((gsm)gsmState); gsmState = gsm_create(); plc.reset(getFrameSize()); }
    void GSMCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            plc.conceal(out, getFrameSize());
            return;
        }

        gsm_byte frame_encoded[33];
        gsm_encode((gsm)gsmState, (gsm_signal*)in, frame_encoded);
        gsm_decode((gsm)gsmState, frame_encoded, (gsm_signal*)out);
        plc.storeGoodFrame(out, getFrameSize());
    }

} // namespace TelephonyDSP
