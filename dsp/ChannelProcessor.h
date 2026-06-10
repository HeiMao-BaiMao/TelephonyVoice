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

namespace r8b { class CDSPResampler24; }

namespace TelephonyDSP {

    class ChannelProcessor {
    public:
        ChannelProcessor(double hostSR);
        ~ChannelProcessor();

        void setSampleRate(double sr);
        void setMode(EraMode mode);
        void setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw);
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
        int packetLossBurstFrames;

        // EVS configuration (only used when currentMode == EVS_NATIVE)
        int evsSampleRate;
        int evsBitrateBps;
        EVS_Bandwidth evsMaxBandwidth;

        RingBuffer ringCodecIn;
        RingBuffer ringCodecOut;
        RingBuffer outputBuffer;

        std::unique_ptr<r8b::CDSPResampler24> resamplerDown;
        std::unique_ptr<r8b::CDSPResampler24> resamplerUp;
        std::unique_ptr<ICodec> codec;

        // Maximum input length that was used to construct the resamplers.
        // Callers of resamplerDown/resamplerUp must never pass more than this
        // many input samples in a single r8brain::CDSPResampler24::process()
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
