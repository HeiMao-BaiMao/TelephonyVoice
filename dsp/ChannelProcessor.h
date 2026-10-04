#pragma once

#include "dsp/Types.h"
#include "dsp/Biquad.h"
#include "dsp/RingBuffer.h"
#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"
#include "dsp/SpeexDSPAux.h"
#include "evs_api.h"
#include <memory>
#include <array>

namespace r8b { class CDSPResampler; }

namespace TelephonyDSP {

    class ChannelProcessor {
    public:
        ChannelProcessor(double hostSR);
        ~ChannelProcessor();

        void setSampleRate(double sr);
        void setMode(EraMode mode);
        void setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw);
        // Set the EVS DTX SID update interval (0 = variable, 3..100 = fixed
        // frames). Propagated to the currently attached EVS codec (if any)
        // and to the next codec built by recreateCodec(). Values outside
        // the supported set are clamped to 0 to mirror the codec's own
        // validation. No-op for non-EVS modes.
        void setEvsDtxSidInterval(int interval);
        // Selects G.711 mu-law (law == 0, default) or A-law (law > 0) for the
        // PSTN_G711 codec path. If the active codec is already a G.711 codec
        // its law is updated in place; if another mode is active the choice is
        // stashed and applied the next time recreateCodec() builds a G.711
        // codec (PSTN_G711 mode).
        void setG711Law(int law);
        // AMR-NB bitrate mode (0..7, 3GPP TS 26.104 mapping). Default 7
        // (12.2 kbps). Applied to the active AMR-NB codec in place via
        // recreateCodec(); for other modes the value is cached in
        // amrNbMode and picked up the next time AMR_NB_3G rebuilds its
        // codec. The ctor defaults to 7 so the legacy behaviour is
        // preserved.
        void setAmrNbMode(int mode);
        // AMR-WB bitrate mode (0..8, 3GPP TS 26.190 mapping). Default 2
        // (12.65 kbps). Applied to the active AMR-WB codec in place; if
        // another mode is active the choice is stashed and applied the
        // next time recreateCodec() builds an AMRWBCodec (AMR_WB_VOLTE).
        void setAmrWbMode(int mode);
        // Opus bandwidth cap (OPUS_BANDWIDTH_*: 1101..1105). Applied
        // directly to a live OpusCodec; for other modes the value is
        // cached and picked up the next time OPUS_VOIP rebuilds its
        // codec. Defaults to FB (1105) which mirrors Opus's own default.
        void setOpusBandwidth(int bw);
        // Opus target bitrate in bps (default 24000, range 6000..510000).
        // For OPUS_VOIP mode the active OpusCodec is updated in place via
        // OpusCodec::setBitrate(); for other modes the value is cached in
        // evsOpusBitrateBps and applied the next time OPUS_VOIP rebuilds
        // its codec. Active changes preserve the codec and packet queues.
        void setOpusBitrate(int bps);
        // Toggle EVS Source-Controlled VBR (sc_vbr_enable). If the active
        // codec is an EVSCodec or EVSCodecJbm the change is pushed
        // immediately via the codec's setScVbrEnabled(); for other modes
        // the value is cached in evsScVbrEnabled and applied the next
        // time recreateCodec() builds an EVS codec. The ctor defaults to
        // false so the legacy behaviour is preserved.
        void setEvsScVbrEnabled(bool enable);
        void reset();
        void pushInput(const float* in, int numSamples);
        size_t pullOutput(float* out, int numSamples);
        size_t getAvailableOutput() const;
        void configure(bool artifacts, float amount, float packetLossRate, float networkDegradation);

        // Returns the cached energy-VAD speech probability in [0,1] for the
        // most recent frame processed by SpeexDSPAux::runPreprocess(), or
        // -1.0f ("unknown") when SpeexDSPAux is not present (e.g. in
        // distribution builds or when the experimental helper was never
        // configured for this codec). Forwards SpeexDSPAux's own "unknown"
        // sentinel unchanged, so callers cannot accidentally treat a
        // disabled VAD as "definitely not speech".
        float getLastVadProb() const;

    private:
        double hostSampleRate;
        EraMode currentMode;
        bool paramArtifactsEnabled;
        float paramArtifactAmount;
        float paramPacketLossRate;
        float paramNetworkDegradation;
        uint32_t packetLossSeed;
        uint32_t artifactSeed;
        int packetLossBurstFrames;

        // EVS configuration (only used when currentMode == EVS_NATIVE)
        int evsSampleRate;
        int evsBitrateBps;
        EVS_Bandwidth evsMaxBandwidth;
        // 0 = variable SID interval (default), 3..100 = fixed frames.
        // Used by EVSCodec / EVSCodecJbm when recreateCodec() builds them.
        int evsDtxSidInterval;

        // EVS Source-Controlled VBR toggle. Mirrored into the
        // EVS_EncOptions when recreateCodec() builds an EVSCodec or
        // EVSCodecJbm. Pushed directly into a live codec via
        // setEvsScVbrEnabled() (which calls codec->setScVbrEnabled()).
        bool evsScVbrEnabled;

        // G.711 law selection: 0 = mu-law (default, US/Japan), >0 = A-law (Europe/ROW).
        // Applied to the active G.711 codec in PSTN_G711 mode; remembered across
        // mode changes so the next PSTN_G711 recreation picks it up.
        int evsG711Law;

        // Opus OPUS_BANDWIDTH_* cap (1101..1105). Cached across mode
        // changes so the next OpusCodec construction in OPUS_VOIP mode
        // picks the caller's choice up; pushed directly into a live
        // OpusCodec via setOpusBandwidth().
        int opusBandwidth;

        // Opus target bitrate in bps. Cached across mode changes so the
        // next OpusCodec construction in OPUS_VOIP mode picks the
        // caller's choice up; pushed directly into a live OpusCodec via
        // setOpusBitrate() (which itself re-issues OPUS_SET_BITRATE on
        // applyNetworkCtls). Default 24000 to preserve the legacy
        // 24 kbps default in recreateCodec().
        int evsOpusBitrateBps;

        // AMR-WB mode (0..8) selection. Applied to the active AMR-WB codec
        // in AMR_WB_VOLTE mode; remembered across mode changes so the next
        // AMR_WB_VOLTE recreation picks it up. See AmrWbCodec for the
        // bitrate mapping.
        int amrWbMode;

        // AMR-NB bitrate mode selection (0..7). Applied to the active
        // AMRNBCodec in AMR_NB_3G mode; remembered across mode changes so
        // the next AMR_NB_3G recreation picks it up. See AMRNBCodec for
        // the mode -> bitrate mapping.
        int amrNbMode;

        RingBuffer ringCodecIn;
        RingBuffer ringCodecOut;
        RingBuffer outputBuffer;

        std::unique_ptr<r8b::CDSPResampler> resamplerDown;
        std::unique_ptr<r8b::CDSPResampler> resamplerUp;
        std::unique_ptr<ICodec> codec;

        // Maximum input length that was used to construct the resamplers.
        // Callers of resamplerDown/resamplerUp must never pass more than this
        // many input samples in a single r8brain::CDSPResampler::process()
        // call, otherwise r8brain's pre-allocated internal buffers overflow.
        // We track both values so we can chunk process() calls safely.
        int downMaxInLen;
        int upMaxInLen;

        // Experimental SpeexDSP-backed VAD/DTX helper. Configured lazily when
        // a codec rate becomes available; reset() touches it but it does not
        // influence the audio path yet. Only present when
        // TELEPHONY_EXPERIMENTAL_NETWORK is enabled.
        std::unique_ptr<SpeexDSPAux> speexAux;

        // Buffers
        std::vector<double> resampInBuf;
        std::vector<float> codecFrameF;
        std::vector<int16_t> codecFrameSIn;
        std::vector<int16_t> codecFrameSOut;
        std::vector<float> tempProcessBuf;
        // Per-chunk staging for the upsampler output. r8brain's process()
        // returns a pointer to an internal buffer that is invalidated by the
        // next process() call, so we must copy each chunk out before the
        // next iteration of the chunking loop.
        std::vector<float> resampUpOutBuf;
        WaveformConcealer simulatedPathPLC;

        // Filters for G.711 (Cascaded for 24dB/oct)
        Biquad hpFilter1, hpFilter2;
        Biquad lpFilter1, lpFilter2;

        void recreateResamplers();
        void recreateCodec();
        void prepareInternalBuffers(int maxBlockSize);
        void updateFilters();
        float nextPacketRandom();
        bool shouldDropPacket();

        void processG711(int numSamples, const float* in, float* out);
        void processEVSLike(int numSamples, const float* in, float* out);
        void processCodec(int numSamples, const float* in);
        void applyArtifacts(float* buffer, int numSamples);
    };

} // namespace TelephonyDSP
