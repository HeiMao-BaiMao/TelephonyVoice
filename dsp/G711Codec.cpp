#include "dsp/G711Codec.h"

namespace TelephonyDSP {

    G711Codec::G711Codec(int sr, bool useAlawFlag) : sampleRate(sr), useAlaw(useAlawFlag), encoded((size_t)(sr / 50)) {
        plc.reset(getFrameSize());
    }
    void G711Codec::setLaw(bool useAlawFlag) {
        useAlaw = useAlawFlag;
    }
    void G711Codec::reset() {
        plc.reset(getFrameSize());
        resetImpairments();
    }
    void G711Codec::processSamples(const int16_t* in, int16_t* out, int count) const {
        for (int i = 0; i < count; ++i)
            out[i] = (int16_t)(useAlaw ? alaw2linear(linear2alaw(in[i])) : ulaw2linear(linear2ulaw(in[i])));
    }
    void G711Codec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();

        const bool silent = dtxEnabled && hasVoiceActivity && !voiceActive;
        for (int i = 0; i < fs; ++i)
            encoded[(size_t)i] = useAlaw ? linear2alaw(in[i]) : linear2ulaw(in[i]);
        corruptPacked(encoded.data(), encoded.size());
        const bool available = exchangePacket(useAlaw ? CodecPacketFormat::G711ALaw : CodecPacketFormat::G711MuLaw,
                                              encoded.data(), encoded.size(), packetLost, silent);
        if (!available || playout.payload.size() != (size_t)fs) {
            plc.conceal(out, fs);
            applyDtxOutput(out, lastDtx);
            return;
        }
        for (int i = 0; i < fs; ++i)
            out[i] = (int16_t)(useAlaw ? alaw2linear(playout.payload[(size_t)i]) : ulaw2linear(playout.payload[(size_t)i]));
        if (playout.dtx) plc.conceal(out, fs);
        applyDtxOutput(out, playout.dtx);
        if (!playout.dtx) plc.storeGoodFrame(out, fs);
    }

} // namespace TelephonyDSP
