#pragma once

#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"
#include "evs_api.h"

namespace TelephonyDSP {

    class EVSCodec : public ICodec {
    public:
        EVSCodec(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw);
        ~EVSCodec() override;
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;

        int getBitrate() const { return bitrateBps; }
        EVS_Bandwidth getMaxBandwidth() const { return maxBw; }

    private:
        int sampleRate;
        int bitrateBps;
        EVS_Bandwidth maxBw;

        EVS_Encoder* enc;
        EVS_Decoder* dec;

        // Reusable scratch buffers for the bitstream roundtrip.
        std::vector<unsigned char> bitstream;
        WaveformConcealer fallbackPLC;
    };

} // namespace TelephonyDSP
