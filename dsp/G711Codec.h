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
        // useAlaw: false (default) selects G.711 mu-law, true selects G.711 A-law.
        G711Codec(int sampleRate, bool useAlaw = false);
        // Switch between mu-law (false) and A-law (true) without recreating the codec.
        void setLaw(bool useAlaw);
        // Returns true if the codec is currently configured to use A-law.
        bool isAlaw() const { return useAlaw; }
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
    private:
        int sampleRate;
        bool useAlaw;
        WaveformConcealer plc;
    };

} // namespace TelephonyDSP
