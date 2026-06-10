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
