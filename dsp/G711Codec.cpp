#include "dsp/G711Codec.h"

namespace TelephonyDSP {

    G711Codec::G711Codec(int sr, bool useAlawFlag) : sampleRate(sr), useAlaw(useAlawFlag) {
        plc.reset(getFrameSize());
    }
    void G711Codec::setLaw(bool useAlawFlag) {
        useAlaw = useAlawFlag;
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
            if (useAlaw) {
                unsigned char a = linear2alaw(in[i]);
                out[i] = (int16_t)alaw2linear(a);
            } else {
                unsigned char u = linear2ulaw(in[i]);
                out[i] = (int16_t)ulaw2linear(u);
            }
        }
        plc.storeGoodFrame(out, fs);
    }

} // namespace TelephonyDSP
