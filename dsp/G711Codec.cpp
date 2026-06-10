#include "dsp/G711Codec.h"

namespace TelephonyDSP {

    G711Codec::G711Codec(int sr) : sampleRate(sr) {
        plc.reset(getFrameSize());
    }
    void G711Codec::reset() {
        plc.reset(getFrameSize());
    }
    void G711Codec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();
        if (packetLost) {
            plc.conceal(out, fs);
            return;
        }

        for (int i = 0; i < fs; ++i) {
            unsigned char u = linear2ulaw(in[i]);
            out[i] = (int16_t)ulaw2linear(u);
        }
        plc.storeGoodFrame(out, fs);
    }

} // namespace TelephonyDSP
