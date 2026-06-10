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
        //
        // mode selects the AMR-WB encoding bitrate mode (0..8, default 2
        // = 12.65 kbps, matching the 3GPP TS 26.190 mapping):
        //   0=6.60  1=8.85  2=12.65  3=14.25  4=15.85
        //   5=18.25 6=19.85 7=23.05  8=23.85  (kbps)
        // Out-of-range values are clamped to the valid range.
        explicit AMRWBCodec(bool dtxEnabled = true, int mode = 2);
        ~AMRWBCodec();
        void reset() override;
        int getSampleRate() const override { return 16000; }
        int getFrameSize() const override { return 320; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
        void setDtxEnabled(bool enable);
        bool isDtxEnabled() const { return dtxEnabled; }
        // Select AMR-WB mode (0..8). Out-of-range values are clamped
        // silently. The encoder state is rebuilt via reset() so the
        // change takes effect on the next processFrame() call.
        void setMode(int mode);
        int getMode() const { return amrMode; }
    private:
        void* encState;
        void* decState;
        bool dtxEnabled;
        int amrMode;
        std::vector<unsigned char> lastSerial;
        WaveformConcealer fallbackPLC;
    };

} // namespace TelephonyDSP
