#pragma once

#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"

namespace TelephonyDSP {

    class AMRWBCodec : public ICodec {
    public:
        // dtxEnabled controls whether the bundled 3GPP AMR-WB reference
        // encoder's internal DTX/CNG path is enabled. Default is true so
        // personal / non-distribution builds use the codec's native DTX
        // (VAD + SID + comfort noise) by default. The distribution stub
        // ignores the flag entirely and keeps its pass-through behavior.
        explicit AMRWBCodec(bool dtxEnabled = true);
        ~AMRWBCodec();
        void reset() override;
        int getSampleRate() const override { return 16000; }
        int getFrameSize() const override { return 320; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
        void setDtxEnabled(bool enable);
        bool isDtxEnabled() const { return dtxEnabled; }
    private:
        void* encState;
        void* decState;
        bool dtxEnabled;
        std::vector<unsigned char> lastSerial;
        WaveformConcealer fallbackPLC;
    };

} // namespace TelephonyDSP
