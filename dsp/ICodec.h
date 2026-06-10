#pragma once

#include "dsp/Types.h"

namespace TelephonyDSP {

class ICodec {
    public:
        virtual ~ICodec() = default;
        virtual void reset() = 0;
        virtual int getSampleRate() const = 0;
        virtual int getFrameSize() const = 0;
        virtual void processFrame(const int16_t* in, int16_t* out, bool packetLost) = 0;
        // Optional hook used by ChannelProcessor to push the current clamped
        // network parameters (packet loss rate, network degradation) into the
        // codec. Default is a no-op so existing codecs do not need to react;
        // codecs that model real packetized VoIP (e.g. OpusCodec) override it
        // to wire CTLs / internal jitter-buffer state.
        virtual void configureNetwork(float packetLossRate, float networkDegradation) {}
    };

} // namespace TelephonyDSP
