#define NOMINMAX
#include "dsp/SignalProcessor.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace TelephonyDSP {

    SignalProcessor::SignalProcessor()
        : hostSampleRate(44100.0), currentMode(EraMode::Bypass),
          routeModelEnabled(false),
          inputEndpoint(RouteEndpoint::FixedLine), outputEndpoint(RouteEndpoint::Mobile5G),
          degradationSegment(DegradationSegment::Both),
          paramDryWet(1.0f), paramOutGain(1.0f), paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
          paramPacketLossRate(0.0f), paramNetworkDegradation(0.0f),
          simulateLatency(true),
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB),
          opusBandwidth(1105), // OPUS_BANDWIDTH_FULLBAND; matches Opus's own default
          opusBitrate(24000), // Opus target bitrate; matches Opus's own default.
          amrNbMode(7),       // 12.2 kbps (MR122); matches AMRNBCodec's own default.
          amrWbMode(2),       // 12.65 kbps; matches AMRWBCodec's own default.
          g711Law(0),         // 0 = mu-law (US/Japan default).
          evsScVbrEnabled(false), // SC-VBR is opt-in.
          evsDtxSidInterval(0),   // 0 = variable SID (codec default).
          targetLatencySamples(0), inTotalSamples(0), outTotalSamples(0)
    {
        updateLatency();
    }

    SignalProcessor::~SignalProcessor() {}

    EraMode SignalProcessor::endpointToMode(RouteEndpoint endpoint) const {
        switch (endpoint) {
            case RouteEndpoint::FixedLine:       return EraMode::PSTN_G711;
            case RouteEndpoint::Mobile2G:        return EraMode::GSM_FR;
            case RouteEndpoint::Mobile3G:        return distributionSafeMode(EraMode::AMR_NB_3G);
            case RouteEndpoint::Mobile4G:        return distributionSafeMode(EraMode::AMR_WB_VOLTE);
            case RouteEndpoint::Mobile5G:        return EraMode::EVS_LIKE;
            case RouteEndpoint::Mobile5GNative:  return distributionSafeMode(EraMode::EVS_NATIVE);
#if TELEPHONY_USE_EVS_JBM
            case RouteEndpoint::Mobile5GJbm:     return distributionSafeMode(EraMode::EVS_JBM);
#endif
            default:                             return EraMode::Bypass;
        }
    }

    bool SignalProcessor::degradesInputLeg() const {
        return degradationSegment == DegradationSegment::Both
            || degradationSegment == DegradationSegment::InputToExchange;
    }

    bool SignalProcessor::degradesOutputLeg() const {
        return degradationSegment == DegradationSegment::Both
            || degradationSegment == DegradationSegment::ExchangeToOutput;
    }

    void SignalProcessor::applyRouteToChannels() {
        for (size_t i = 0; i < outputLegs.size(); ++i) {
            auto& inputLeg = inputLegs[i];
            auto& outputLeg = outputLegs[i];
            inputLeg->setEVSConfig(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
            outputLeg->setEVSConfig(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
            // Keep the per-channel EVS DTX SID interval in sync so a later
            // recreateCodec() (e.g. after a setMode) bakes the right value
            // into the next EVS encoder.
            inputLeg->setEvsDtxSidInterval(evsDtxSidInterval);
            outputLeg->setEvsDtxSidInterval(evsDtxSidInterval);
            // Mirror every other cached configuration knob to the new
            // legs so ensureChannels() pickers up the current values.
            inputLeg->setOpusBandwidth(opusBandwidth);
            outputLeg->setOpusBandwidth(opusBandwidth);
            inputLeg->setOpusBitrate(opusBitrate);
            outputLeg->setOpusBitrate(opusBitrate);
            inputLeg->setAmrNbMode(amrNbMode);
            outputLeg->setAmrNbMode(amrNbMode);
            inputLeg->setAmrWbMode(amrWbMode);
            outputLeg->setAmrWbMode(amrWbMode);
            inputLeg->setG711Law(g711Law);
            outputLeg->setG711Law(g711Law);
            inputLeg->setEvsScVbrEnabled(evsScVbrEnabled);
            outputLeg->setEvsScVbrEnabled(evsScVbrEnabled);

            if (routeModelEnabled) {
                inputLeg->setMode(endpointToMode(inputEndpoint));
                outputLeg->setMode(endpointToMode(outputEndpoint));

                const float inputLoss = degradesInputLeg() ? paramPacketLossRate : 0.0f;
                const float inputDeg = degradesInputLeg() ? paramNetworkDegradation : 0.0f;
                const float outputLoss = degradesOutputLeg() ? paramPacketLossRate : 0.0f;
                const float outputDeg = degradesOutputLeg() ? paramNetworkDegradation : 0.0f;

                inputLeg->configure(paramArtifactsEnabled, paramArtifactAmount, inputLoss, inputDeg);
                outputLeg->configure(paramArtifactsEnabled, paramArtifactAmount, outputLoss, outputDeg);
            } else {
                inputLeg->setMode(EraMode::Bypass);
                inputLeg->configure(false, 0.0f, 0.0f, 0.0f);
                outputLeg->setMode(currentMode);
                outputLeg->configure(paramArtifactsEnabled, paramArtifactAmount,
                                     paramPacketLossRate, paramNetworkDegradation);
            }
        }
    }

    void SignalProcessor::setSampleRate(double sr) {
        hostSampleRate = sr;
        updateLatency();
        for (auto& ch : inputLegs) ch->setSampleRate(sr);
        for (auto& ch : outputLegs) ch->setSampleRate(sr);
        reset();
    }

    void SignalProcessor::setMode(EraMode mode) {
        mode = distributionSafeMode(mode);
        routeModelEnabled = false;
        currentMode = mode;
        applyRouteToChannels();
    }

    void SignalProcessor::setRoute(RouteEndpoint input, RouteEndpoint output, DegradationSegment segment) {
        routeModelEnabled = true;
        inputEndpoint = input;
        outputEndpoint = output;
        degradationSegment = segment;
        applyRouteToChannels();
    }

    void SignalProcessor::setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw) {
        evsSampleRate   = sampleRateHz;
        evsBitrateBps   = bitrateBps;
        evsMaxBandwidth = maxBw;
        for (auto& ch : inputLegs) ch->setEVSConfig(sampleRateHz, bitrateBps, maxBw);
        for (auto& ch : outputLegs) ch->setEVSConfig(sampleRateHz, bitrateBps, maxBw);
    }

    void SignalProcessor::setOpusBandwidth(int bw) {
        // Cache at the SignalProcessor level so new legs created later
        // (e.g. by ensureChannels) pick it up via the next applyRouteToChannels
        // sweep. Existing legs get the value pushed immediately.
        opusBandwidth = bw;
        for (auto& ch : inputLegs) ch->setOpusBandwidth(bw);
        for (auto& ch : outputLegs) ch->setOpusBandwidth(bw);
    }

    void SignalProcessor::setEvsDtxSidInterval(int interval) {
        // Cache the value (applyRouteToChannels() will read it on the next
        // pass) and forward it to every live ChannelProcessor so the value
        // is applied to the currently attached codec (if any) immediately.
        evsDtxSidInterval = interval;
        for (auto& ch : inputLegs) ch->setEvsDtxSidInterval(interval);
        for (auto& ch : outputLegs) ch->setEvsDtxSidInterval(interval);
    }

    void SignalProcessor::setAmrNbMode(int mode) {
        // Clamp at the SignalProcessor boundary so the in-flight value is
        // always within AMRNBCodec's 0..7 range; each ChannelProcessor
        // also clamps independently. Cache the clamped value so new
        // legs created later (via ensureChannels) pick it up.
        const int clamped = mode < 0 ? 0 : (mode > 7 ? 7 : mode);
        amrNbMode = clamped;
        for (auto& ch : inputLegs)  ch->setAmrNbMode(clamped);
        for (auto& ch : outputLegs) ch->setAmrNbMode(clamped);
    }

    void SignalProcessor::setAmrWbMode(int mode) {
        // Clamp at the SignalProcessor boundary so the in-flight value is
        // always within AMRWBCodec's 0..8 range; each ChannelProcessor
        // also clamps independently. Cache the clamped value so new
        // legs created later (via ensureChannels) pick it up.
        const int clamped = mode < 0 ? 0 : (mode > 8 ? 8 : mode);
        amrWbMode = clamped;
        for (auto& ch : inputLegs)  ch->setAmrWbMode(clamped);
        for (auto& ch : outputLegs) ch->setAmrWbMode(clamped);
    }

    void SignalProcessor::setG711Law(int law) {
        // Cache so new legs created later (via ensureChannels) pick
        // it up via applyRouteToChannels().
        g711Law = law;
        for (auto& ch : inputLegs) ch->setG711Law(law);
        for (auto& ch : outputLegs) ch->setG711Law(law);
    }

    void SignalProcessor::setOpusBitrate(int bps) {
        // Clamp here so the cached value is always within Opus's
        // OPUS_BITRATE_MIN..OPUS_BITRATE_MAX (6..510000) range, even
        // if a future caller bypasses the per-leg clamping.
        const int clamped = bps < 6 ? 6 : (bps > 510000 ? 510000 : bps);
        opusBitrate = clamped;
        for (auto& ch : inputLegs)  ch->setOpusBitrate(clamped);
        for (auto& ch : outputLegs) ch->setOpusBitrate(clamped);
    }

    void SignalProcessor::setEvsScVbrEnabled(bool enable) {
        evsScVbrEnabled = enable;
        for (auto& ch : inputLegs)  ch->setEvsScVbrEnabled(enable);
        for (auto& ch : outputLegs) ch->setEvsScVbrEnabled(enable);
    }

    void SignalProcessor::setParameters(float dryWet, float outGaindB, bool artifacts, float artifactAmount,
                                        float packetLossRate, float networkDegradation) {
        paramDryWet = dryWet;
        paramOutGain = std::pow(10.0f, outGaindB / 20.0f);
        paramArtifactsEnabled = artifacts;
        paramArtifactAmount = artifactAmount;
        paramPacketLossRate = std::clamp(packetLossRate, 0.0f, 0.95f);
        paramNetworkDegradation = std::clamp(networkDegradation, 0.0f, 1.0f);
        applyRouteToChannels();
    }

    void SignalProcessor::setSimulateLatency(bool enable) {
        simulateLatency = enable;
        updateLatency();
    }

    void SignalProcessor::reset() {
        inTotalSamples = 0;
        outTotalSamples = 0;
        for (auto& ch : inputLegs) ch->reset();
        for (auto& ch : outputLegs) ch->reset();
        for (auto& db : dryBuffers) db->reset();
        applyRouteToChannels();
    }

    void SignalProcessor::updateLatency() {
        // LATENCY_MS (dsp/Types.h) is the single source of truth for the
        // latency reported to the host: the bypass path in TelephonyVoice.cpp
        // compensates using getLatencySamples(), so the delay there and the
        // delay the host is told about must come from the same constant.
        if (simulateLatency) {
            targetLatencySamples = (int)std::round((LATENCY_MS / 1000.0) * hostSampleRate);
        } else {
            targetLatencySamples = 0;
        }
    }

    int SignalProcessor::getLatencySamples() const {
        return targetLatencySamples;
    }

    void SignalProcessor::ensureChannels(int count) {
        if (outputLegs.size() != (size_t)count) {
            inputLegs.clear();
            outputLegs.clear();
            dryBuffers.clear();
            for (int i=0; i<count; ++i) {
                inputLegs.push_back(std::make_unique<ChannelProcessor>(hostSampleRate));
                outputLegs.push_back(std::make_unique<ChannelProcessor>(hostSampleRate));
                dryBuffers.push_back(std::make_unique<RingBuffer>(131072));
            }
            applyRouteToChannels();
        }
    }

    void SignalProcessor::ensureScratch(int numIns, int numSamples) {
        if ((int)exchangeScratch.size() < numSamples) exchangeScratch.resize(numSamples);
        if ((int)dryScratch.size()      < numSamples) dryScratch.resize(numSamples);
        if ((int)wetScratch.size()      < numSamples) wetScratch.resize(numSamples);
        if ((int)legOutScratch.size()   < numIns)     legOutScratch.resize(numIns);
        for (int i = 0; i < numIns; ++i) {
            if ((int)legOutScratch[i].size() < numSamples) legOutScratch[i].resize(numSamples);
        }
    }

    void SignalProcessor::process(float** inputs, int numIns, float** outputs, int numOuts, int numSamples) {
        ensureChannels(numIns);
        ensureScratch(numIns, numSamples);
        inTotalSamples += numSamples;

        for (int i=0; i<numIns; ++i) {
            dryBuffers[i]->write(inputs[i], numSamples);
            if (routeModelEnabled) {
                inputLegs[i]->pushInput(inputs[i], numSamples);
                // pullOutput() may return fewer samples than requested; clear
                // the tail it did not write so the output leg never sees
                // stale scratch data from a previous block.
                const size_t got = inputLegs[i]->pullOutput(exchangeScratch.data(), numSamples);
                if (got < (size_t)numSamples) {
                    std::fill(exchangeScratch.begin() + got,
                              exchangeScratch.begin() + numSamples, 0.0f);
                }
                outputLegs[i]->pushInput(exchangeScratch.data(), numSamples);
            } else {
                outputLegs[i]->pushInput(inputs[i], numSamples);
            }
        }

        int64_t readable = (inTotalSamples - targetLatencySamples) - outTotalSamples;
        int samplesToWrite = numSamples;
        int outputOffset = 0;

        if (readable < 0) {
            int silence = (int)(std::min)((int64_t)samplesToWrite, -readable);
            for (int ch=0; ch<numOuts; ++ch) {
                std::memset(outputs[ch], 0, silence * sizeof(float));
            }
            samplesToWrite -= silence;
            outputOffset += silence;
            outTotalSamples += silence;
        }

        if (samplesToWrite > 0) {
            // Reused scratch is not zero-initialised per block, so clear the
            // regions the readers below only partially overwrite.
            std::fill(dryScratch.begin(), dryScratch.begin() + samplesToWrite, 0.0f);
            std::fill(wetScratch.begin(), wetScratch.begin() + samplesToWrite, 0.0f);

            for (int i=0; i<numIns; ++i) {
                float* d = dryScratch.data();
                if (dryBuffers[i]->getReadAvailable() >= (size_t)samplesToWrite) {
                    dryBuffers[i]->read(d, samplesToWrite);
                } else {
                    dryBuffers[i]->read(d, dryBuffers[i]->getReadAvailable());
                }

                float* w = wetScratch.data();
                outputLegs[i]->pullOutput(w, samplesToWrite);

                float* dst = legOutScratch[i].data();
                for (int s=0; s<samplesToWrite; ++s) {
                    dst[s] = (d[s] * (1.0f - paramDryWet) + w[s] * paramDryWet) * paramOutGain;
                }
            }

            for (int ch=0; ch<numOuts; ++ch) {
                int inCh = (ch < numIns) ? ch : 0;
                float* dest = outputs[ch] + outputOffset;
                std::memcpy(dest, legOutScratch[inCh].data(), samplesToWrite * sizeof(float));
            }

            outTotalSamples += samplesToWrite;
        }
    }

} // namespace TelephonyDSP
