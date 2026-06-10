#pragma once

#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"

extern "C" {
    unsigned char linear2ulaw(int pcm_val);
    int ulaw2linear(unsigned char u_val);
    unsigned char linear2alaw(int pcm_val);
    int alaw2linear(unsigned char a_val);
}

namespace TelephonyDSP {

    class G711Codec : public ICodec {
    public:
        G711Codec(int sampleRate);
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
    private:
        int sampleRate;
        WaveformConcealer plc;
    };

} // namespace TelephonyDSP
