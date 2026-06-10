#pragma once

#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"

namespace TelephonyDSP {

    class AMRNBCodec : public ICodec {
    public:
        // dtxEnabled controls whether the bundled 3GPP AMR-NB reference
        // encoder's internal DTX/CNG path is enabled. Default is true so
        // personal / non-distribution builds use the codec's native DTX
        // (VAD + SID + comfort noise) by default. The distribution stub
        // ignores the flag entirely and keeps its pass-through behavior.
        //
        // mode selects the AMR-NB bitrate mode (0..7, default 7 = MR122
        // = 12.2 kbps). Maps to the opencore-amrnb `Mode` enum:
        //   0=MR475 (4.75k) 1=MR515 (5.15k) 2=MR59  (5.9k)  3=MR67  (6.7k)
        //   4=MR74  (7.4k)  5=MR795 (7.95k) 6=MR102 (10.2k) 7=MR122 (12.2k)
        // Out-of-range values are clamped to 0..7. The distribution stub
        // stores the value for introspection but does not affect its
        // pass-through behavior.
        explicit AMRNBCodec(bool dtxEnabled = true, int mode = 7);
        ~AMRNBCodec();
        void reset() override;
        int getSampleRate() const override { return 8000; }
        int getFrameSize() const override { return 160; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
        void setDtxEnabled(bool enable);
        bool isDtxEnabled() const { return dtxEnabled; }
        // Selects AMR-NB mode in 0..7. Out-of-range values are clamped.
        // Stores the new value and calls reset() so the next encode uses
        // the chosen mode without carrying over encoder state.
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
