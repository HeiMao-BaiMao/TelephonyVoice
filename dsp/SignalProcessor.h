#pragma once

#include "dsp/Types.h"
#include "dsp/ChannelProcessor.h"
#include "dsp/RingBuffer.h"
#include "evs_api.h"
#include <vector>
#include <memory>

namespace TelephonyDSP {

    class SignalProcessor {
    public:
        SignalProcessor();
        ~SignalProcessor();

        void setSampleRate(double sampleRate);
        void setMode(EraMode mode);
        void setRoute(RouteEndpoint input, RouteEndpoint output, DegradationSegment degradationSegment);
        void setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw);
        // Forward an OPUS_BANDWIDTH_* cap (1101..1105) to every leg's
        // ChannelProcessor. Applied immediately to a live OpusCodec, or
        // cached for the next OPUS_VOIP rebuild.
        void setOpusBandwidth(int bw);
        // Forward an Opus target bitrate in bps (default 24000, range
        // 6..510000) to every leg's ChannelProcessor. Applied
        // immediately to a live OpusCodec, or cached for the next
        // OPUS_VOIP rebuild.
        void setOpusBitrate(int bps);
        // Forward an AMR-NB bitrate mode (0..7) to every leg's
        // ChannelProcessor. Cached for the next AMR_NB_3G rebuild.
        void setAmrNbMode(int mode);
        // Forward an AMR-WB bitrate mode (0..8) to every leg's
        // ChannelProcessor. Applied immediately to a live AMRWBCodec, or
        // cached for the next AMR_WB_VOLTE rebuild.
        void setAmrWbMode(int mode);
        // Forward the G.711 companding law (0 = mu-law, 1 = A-law) to
        // every leg's ChannelProcessor. Applied immediately to a live
        // G.711 codec, or cached for the next PSTN_G711 rebuild.
        void setG711Law(int law);
        // Forward the EVS Source-Controlled VBR toggle to every leg's
        // ChannelProcessor. Applied immediately to a live EVS codec, or
        // cached for the next EVS codec rebuild.
        void setEvsScVbrEnabled(bool enable);
        // Propagate the EVS DTX SID update interval (0 = variable, 3..100 =
        // fixed frames) to every input/output ChannelProcessor. The value
        // is remembered here so future codec rebuilds also see it.
        void setEvsDtxSidInterval(int interval);
        void setParameters(float dryWet, float outGaindB, bool artifacts, float artifactAmount,
                           float packetLossRate = 0.0f, float networkDegradation = 0.0f);
        void setSimulateLatency(bool enable);
        void reset();
        void process(float** inputs, int numIns, float** outputs, int numOuts, int numSamples);
        int getLatencySamples() const;

    private:
        double hostSampleRate;
        EraMode currentMode;
        bool routeModelEnabled;
        RouteEndpoint inputEndpoint;
        RouteEndpoint outputEndpoint;
        DegradationSegment degradationSegment;
        float paramDryWet;
        float paramOutGain;
        bool paramArtifactsEnabled;
        float paramArtifactAmount;
        float paramPacketLossRate;
        float paramNetworkDegradation;
        bool simulateLatency;

        // EVS configuration (only used when currentMode == EVS_NATIVE)
        int evsSampleRate;
        int evsBitrateBps;
        EVS_Bandwidth evsMaxBandwidth;

        // Opus OPUS_BANDWIDTH_* cap (1101..1105). Mirrored to every
        // ChannelProcessor via setOpusBandwidth().
        int opusBandwidth;
        // Opus target bitrate in bps (default 24000, range 6..510000).
        // Mirrored to every ChannelProcessor via setOpusBitrate().
        int opusBitrate;
        // AMR-NB bitrate mode (0..7). Mirrored to every ChannelProcessor
        // via setAmrNbMode(); each ChannelProcessor also clamps
        // independently.
        int amrNbMode;
        // AMR-WB bitrate mode (0..8). Mirrored to every ChannelProcessor
        // via setAmrWbMode(); each ChannelProcessor also clamps
        // independently.
        int amrWbMode;
        // G.711 companding law (0 = mu-law, 1 = A-law). Mirrored to
        // every ChannelProcessor via setG711Law().
        int g711Law;
        // EVS Source-Controlled VBR toggle. Mirrored to every
        // ChannelProcessor via setEvsScVbrEnabled().
        bool evsScVbrEnabled;
        // 0 = variable SID interval (default), 3..100 = fixed frames.
        // Forwarded to all ChannelProcessor instances so a later
        // recreateCodec() bakes the right value into the next EVS encoder.
        int evsDtxSidInterval;

        int targetLatencySamples;
        int64_t inTotalSamples;
        int64_t outTotalSamples;

        std::vector<std::unique_ptr<ChannelProcessor>> inputLegs;
        std::vector<std::unique_ptr<ChannelProcessor>> outputLegs;
        std::vector<std::unique_ptr<RingBuffer>> dryBuffers;

        void updateLatency();
        void ensureChannels(int count);
        EraMode endpointToMode(RouteEndpoint endpoint) const;
        bool degradesInputLeg() const;
        bool degradesOutputLeg() const;
        void applyRouteToChannels();
    };

} // namespace TelephonyDSP
