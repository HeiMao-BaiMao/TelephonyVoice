#pragma once

#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"
#include "evs_api.h"

namespace TelephonyDSP {

    class EVSCodec : public ICodec {
    public:
        EVSCodec(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw,
                 bool scVbrEnabled = false, int dtxSidInterval = 0);
        ~EVSCodec() override;
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return sampleRate / 50; }
        void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;

        // Toggle EVS Source-Controlled VBR (sc_vbr_enable). The value is
        // persisted on the instance and applied the next time reset() (or
        // the ctor) builds the encoder, so callers do not need to time
        // the call against the audio thread.
        void setScVbrEnabled(bool enable) { scVbrEnabled = enable; reset(); }
        bool getScVbrEnabled() const { return scVbrEnabled; }

        // Update the DTX SID update interval. 0 = variable (codec default,
        // promoted internally to ~12 frames by init_encoder). 3..100 = fixed
        // SID update interval in 20 ms frames. Values outside the supported
        // set are clamped to 0 to avoid evs_enc_create_ex rejecting the
        // configuration. The new value is applied immediately by tearing
        // down and re-creating the encoder through reset().
        void setDtxSidInterval(int interval);

        int getBitrate() const { return bitrateBps; }
        EVS_Bandwidth getMaxBandwidth() const { return maxBw; }

    private:
        int sampleRate;
        int bitrateBps;
        EVS_Bandwidth maxBw;

        // EVS Source-Controlled VBR toggle. Mirrored into the encoder's
        // EVS_EncOptions every time the encoder is (re)created. Persisted
        // across reset() so a later setScVbrEnabled() survives a session
        // restart.
        bool scVbrEnabled;
        // 0 = variable SID interval (default), 3..100 = fixed frames.
        // Mirrored into opts.dtx_sid_interval every time the encoder is
        // (re)created. Persisted across reset() so a later
        // setDtxSidInterval() survives a session restart.
        int dtxSidInterval;

        EVS_Encoder* enc;
        EVS_Decoder* dec;

        // Reusable scratch buffers for the bitstream roundtrip.
        std::vector<unsigned char> bitstream;
        WaveformConcealer fallbackPLC;
    };

} // namespace TelephonyDSP
