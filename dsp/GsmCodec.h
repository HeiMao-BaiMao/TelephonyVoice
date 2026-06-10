#pragma once

#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"

struct gsm_state;

namespace TelephonyDSP {

    class GSMCodec : public ICodec {
    public:
        GSMCodec();
        ~GSMCodec();
        void reset() override;
        int getSampleRate() const override { return 8000; }
        int getFrameSize() const override { return 160; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
    private:
        void* gsmState;
        WaveformConcealer plc;
    };

} // namespace TelephonyDSP
