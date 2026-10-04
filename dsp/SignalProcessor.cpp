#define NOMINMAX
#include "dsp/SignalProcessor.h"
#include "dsp/EvsConfig.h"
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

    void SignalProcessor::setAdvancedSettings(const AdvancedSettings& settings) {
        if(advanced==settings) return;
        const int oldLatency=targetLatencySamples;
        const bool opusActive=routeModelEnabled?(endpointToMode(inputEndpoint)==EraMode::OPUS_VOIP || endpointToMode(outputEndpoint)==EraMode::OPUS_VOIP):currentMode==EraMode::OPUS_VOIP;
        const bool frameChanged=opusActive && advanced.get(AdvancedControl::OpusFrameDuration)!=settings.get(AdvancedControl::OpusFrameDuration);
        const bool impairedLeg=routeModelEnabled?(degradesInputLeg() || degradesOutputLeg()):currentMode!=EraMode::Bypass;
        const bool streamChanged=impairedLeg && (
            advanced.enabled(AdvancedControl::NetworkEnabled)!=settings.enabled(AdvancedControl::NetworkEnabled)
            || ((advanced.enabled(AdvancedControl::NetworkEnabled) || settings.enabled(AdvancedControl::NetworkEnabled)) && advanced.get(AdvancedControl::PlaybackDelayMs)!=settings.get(AdvancedControl::PlaybackDelayMs))
            || advanced.get(AdvancedControl::ClockDriftPpm)!=settings.get(AdvancedControl::ClockDriftPpm)
            || advanced.enabled(AdvancedControl::HostClock)!=settings.enabled(AdvancedControl::HostClock));
        advanced=settings;
        applyRouteToChannels();
        updateLatency();
        if(oldLatency!=targetLatencySamples || frameChanged || streamChanged) reset();
    }
    void SignalProcessor::setTransportTime(double seconds,bool playing) {
        if(!advanced.enabled(AdvancedControl::HostClock) || !std::isfinite(seconds)) return;
        if(hostTimeKnown && (playing || hostPlaying) && std::abs(seconds-expectedHostTime)>2.0/hostSampleRate) reset();
        expectedHostTime=seconds; hostTimeKnown=true; hostPlaying=playing;
        for(auto& ch:inputLegs) ch->setTransportTime(seconds,playing);
        for(auto& ch:outputLegs) ch->setTransportTime(seconds,playing);
    }
    ProcessingTelemetry SignalProcessor::getTelemetry() const {
        ProcessingTelemetry total; total.inputPeak=inputPeak; total.outputPeak=outputPeak;
        auto collect=[&](const auto& channels) {
            for(const auto& ch:channels) { const auto t=ch->getTelemetry();
                total.packets+=t.packets; total.lost+=t.lost; total.late+=t.late; total.duplicates+=t.duplicates;
                total.jitterMs=std::max(total.jitterMs,t.jitterMs); if(t.opusMode>=0) total.opusMode=t.opusMode;
            }
        };
        if(routeModelEnabled) collect(inputLegs); collect(outputLegs);
        total.measuredLoss=total.packets?double(total.lost)/total.packets:0;
        return total;
    }

    EraMode SignalProcessor::endpointToMode(RouteEndpoint endpoint) const {
        switch (endpoint) {
            case RouteEndpoint::FixedLine:       return EraMode::PSTN_G711;
            case RouteEndpoint::Mobile2G:        return EraMode::GSM_FR;
            case RouteEndpoint::Mobile3G:        return distributionSafeMode(EraMode::AMR_NB_3G);
            case RouteEndpoint::Mobile4G:        return distributionSafeMode(EraMode::AMR_WB_VOLTE);
            case RouteEndpoint::Mobile5G:        return EraMode::EVS_LIKE;
            case RouteEndpoint::Mobile5GNative:  return distributionSafeMode(EraMode::EVS_NATIVE);
            case RouteEndpoint::Opus: return distributionSafeMode(EraMode::OPUS_VOIP);
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
            auto forLeg=[&](bool degraded,bool input) {
                auto s=advanced;
                if(!degraded) {
                    for(auto c:{AdvancedControl::NetworkEnabled,AdvancedControl::BitErrorRate,AdvancedControl::FadingEnabled,AdvancedControl::ClockDriftPpm,AdvancedControl::EchoEnabled,AdvancedControl::HandoverIntervalMs}) s.setPlain((size_t)c,0);
                }
                if(routeModelEnabled && !input) s.setPlain((size_t)AdvancedControl::DtmfDigit,-1);
                return s;
            };
            inputLeg->setAdvancedSettings(forLeg(routeModelEnabled && degradesInputLeg(),true));
            outputLeg->setAdvancedSettings(forLeg(!routeModelEnabled || degradesOutputLeg(),false));
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
        if (!std::isfinite(sr) || sr < 8000.0 || sr > 384000.0) return;
        hostSampleRate = sr;
        updateLatency();
        for (auto& ch : inputLegs) ch->setSampleRate(sr);
        for (auto& ch : outputLegs) ch->setSampleRate(sr);
        for(auto& buffer:dryBuffers) buffer->resize(std::max<size_t>(131072,(size_t)std::ceil(sr)+8192));
        reset();
    }

    void SignalProcessor::setMode(EraMode mode) {
        mode = distributionSafeMode(mode);
        if (!routeModelEnabled && currentMode == mode) return;
        routeModelEnabled = false;
        currentMode = mode;
        updateLatency();
        applyRouteToChannels();
        reset();
    }

    void SignalProcessor::setRoute(RouteEndpoint input, RouteEndpoint output, DegradationSegment segment) {
        const bool pathChanged = !routeModelEnabled || inputEndpoint != input || outputEndpoint != output;
        const int oldLatency=targetLatencySamples;
        routeModelEnabled = true;
        inputEndpoint = input;
        outputEndpoint = output;
        degradationSegment = segment;
        updateLatency();
        applyRouteToChannels();
        if (pathChanged || oldLatency!=targetLatencySamples) reset();
    }

    void SignalProcessor::setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw) {
        normalizeEvsConfig(sampleRateHz, bitrateBps, maxBw);
        const bool changed = evsSampleRate != sampleRateHz;
        evsSampleRate   = sampleRateHz;
        evsBitrateBps   = bitrateBps;
        evsMaxBandwidth = maxBw;
        for (auto& ch : inputLegs) ch->setEVSConfig(sampleRateHz, bitrateBps, maxBw);
        for (auto& ch : outputLegs) ch->setEVSConfig(sampleRateHz, bitrateBps, maxBw);
        auto native = [](EraMode mode) {
            return mode == EraMode::EVS_NATIVE
#if TELEPHONY_USE_EVS_JBM
                || mode == EraMode::EVS_JBM
#endif
                ;
        };
        if (changed && (routeModelEnabled ? (native(endpointToMode(inputEndpoint)) || native(endpointToMode(outputEndpoint))) : native(currentMode))) reset();
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
        if (amrNbMode == clamped) return;
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
        if (amrWbMode == clamped) return;
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
        // supported bitrate (6000..510000) range, even
        // if a future caller bypasses the per-leg clamping.
        const int clamped = bps < 6000 ? 6000 : (bps > 510000 ? 510000 : bps);
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
        const int priorLatency=targetLatencySamples;
        paramDryWet = std::isfinite(dryWet) ? std::clamp(dryWet, 0.0f, 1.0f) : 1.0f;
        paramOutGain = std::pow(10.0f, (std::isfinite(outGaindB) ? std::clamp(outGaindB, -60.0f, 24.0f) : 0.0f) / 20.0f);
        paramArtifactsEnabled = artifacts;
        paramArtifactAmount = std::isfinite(artifactAmount) ? std::clamp(artifactAmount, 0.0f, 1.0f) : 0.0f;
        paramPacketLossRate = (std::isfinite(packetLossRate) ? std::clamp(packetLossRate, 0.0f, 0.95f) : 0.0f);
        paramNetworkDegradation = (std::isfinite(networkDegradation) ? std::clamp(networkDegradation, 0.0f, 1.0f) : 0.0f);
        applyRouteToChannels(); updateLatency();
        if(priorLatency!=targetLatencySamples) reset();
    }

    void SignalProcessor::setSimulateLatency(bool enable) {
        if (simulateLatency == enable) return;
        simulateLatency = enable;
        updateLatency();
        reset();
    }

    void SignalProcessor::reset() {
        inTotalSamples = 0;
        outTotalSamples = 0;
        inputPeak=outputPeak=0; hostTimeKnown=false;
        for (auto& ch : inputLegs) ch->reset();
        for (auto& ch : outputLegs) ch->reset();
        for (auto& db : dryBuffers) db->reset();
        applyRouteToChannels();
    }

    void SignalProcessor::updateLatency() {
        // Framed/resampled codecs require a deterministic reserve even when
        // callers disable the optional dry/bypass delay. Without it output
        // starvation inserts a different number of zeros for each block size.
        const bool needsCodecReserve = routeModelEnabled || currentMode != EraMode::Bypass;
        double additional=0;
        auto legDelay=[&](EraMode mode,bool degraded) {
            double d=0,frameMs=20;
            if(mode==EraMode::Bypass) return d;
#if TELEPHONY_USE_EVS_JBM
            if(mode==EraMode::EVS_JBM) return d; // adaptive wet-path delay is part of the simulation
#endif
            if(mode==EraMode::OPUS_VOIP) {
                static constexpr double duration[]={2.5,5,10,20,40,60};
                frameMs=duration[(int)advanced.get(AdvancedControl::OpusFrameDuration)];
            }
            if(degraded && advanced.enabled(AdvancedControl::NetworkEnabled)) d+=std::ceil(advanced.get(AdvancedControl::PlaybackDelayMs)/frameMs)*frameMs;
            else if(mode==EraMode::OPUS_VOIP) d+=(2+(int)((degraded?paramNetworkDegradation:0)*4))*frameMs;
            if(degraded && advanced.get(AdvancedControl::ClockDriftPpm)!=0
#if TELEPHONY_USE_EVS_JBM
                && mode!=EraMode::EVS_JBM
#endif
            ) d+=2;
            return d;
        };
        if(needsCodecReserve) {
            if(routeModelEnabled) {
                additional+=legDelay(endpointToMode(inputEndpoint),degradesInputLeg());
                additional+=legDelay(endpointToMode(outputEndpoint),degradesOutputLeg());
            } else additional=legDelay(currentMode,true);
        }
        targetLatencySamples = (simulateLatency || needsCodecReserve)
            ? (int)std::round(((LATENCY_MS+additional) / 1000.0) * hostSampleRate) : 0;
    }

    int SignalProcessor::getLatencySamples() const {
        return targetLatencySamples;
    }

    void SignalProcessor::ensureChannels(int count) {
        if (outputLegs.size() != (size_t)count) {
            inTotalSamples = outTotalSamples = 0;
            inputLegs.clear();
            outputLegs.clear();
            dryBuffers.clear();
            for (int i=0; i<count; ++i) {
                inputLegs.push_back(std::make_unique<ChannelProcessor>(hostSampleRate));
                outputLegs.push_back(std::make_unique<ChannelProcessor>(hostSampleRate));
                dryBuffers.push_back(std::make_unique<RingBuffer>(std::max<size_t>(131072,(size_t)std::ceil(hostSampleRate)+8192)));
            }
            applyRouteToChannels();
            if(hostTimeKnown) {
                for(auto& ch:inputLegs) ch->setTransportTime(expectedHostTime,hostPlaying);
                for(auto& ch:outputLegs) ch->setTransportTime(expectedHostTime,hostPlaying);
            }
        }
    }

    void SignalProcessor::process(float** inputs, int numIns, float** outputs, int numOuts, int numSamples) {
        if (numSamples <= 0 || !outputs || numOuts <= 0) return;
        for (int ch = 0; ch < numOuts; ++ch) if (!outputs[ch]) return;
        if (!inputs || numIns <= 0) {
            inputPeak=outputPeak=0;
            for (int ch = 0; ch < numOuts; ++ch) std::fill_n(outputs[ch], numSamples, 0.0f);
            return;
        }
        for (int ch = 0; ch < numIns; ++ch) if (!inputs[ch]) return;
        ensureChannels(numIns);
        if(hostTimeKnown) expectedHostTime+=numSamples/hostSampleRate;
        inputPeak=outputPeak=0;

        // Bound staging and ring-buffer use even for unusually large offline
        // blocks. Read every channel before writing any aliased output.
        constexpr int kChunkSize = 4096;
        for (int offset = 0; offset < numSamples; offset += kChunkSize) {
            const int count = std::min(kChunkSize, numSamples - offset);
            for (int ch = 0; ch < numIns; ++ch) {
                std::vector<float> input(count);
                for (int i = 0; i < count; ++i) {
                    const float sample = inputs[ch][offset + i];
                    input[i] = std::isfinite(sample) ? sample : 0.0f;
                    inputPeak=std::max(inputPeak,(double)std::abs(input[i]));
                }
                dryBuffers[ch]->write(input.data(), count);
                if (routeModelEnabled) {
                    inputLegs[ch]->pushInput(input.data(), count);
                    const auto available = inputLegs[ch]->getAvailableOutput();
                    std::vector<float> exchange(available);
                    inputLegs[ch]->pullOutput(exchange.data(), (int)available);
                    // Do not pad an incomplete codec frame with host-block
                    // silence. The second leg consumes actual streaming data.
                    outputLegs[ch]->pushInput(exchange.data(), (int)available);
                } else {
                    outputLegs[ch]->pushInput(input.data(), count);
                }
            }
            inTotalSamples += count;

            // Startup silence is measured on the output timeline exactly once.
            // Subtracting output silence from "readable input" on every block
            // previously left ordinary small blocks permanently silent.
            const int silence = (int)std::min<int64_t>(count,
                std::max<int64_t>(0, targetLatencySamples - outTotalSamples));
            const int ready = count - silence;
            std::vector<std::vector<float>> processed(numIns, std::vector<float>(count, 0.0f));
            for (int ch = 0; ch < numIns; ++ch) {
                std::vector<float> dry(ready, 0.0f), wet(ready, 0.0f);
                if (ready > 0) {
                    dryBuffers[ch]->read(dry.data(), ready);
                    outputLegs[ch]->pullOutput(wet.data(), ready);
                    for (int i = 0; i < ready; ++i)
                        processed[ch][silence + i] =
                            (dry[i] * (1.0f - paramDryWet) + wet[i] * paramDryWet) * paramOutGain;
                }
            }
            if (numOuts == 1 && numIns > 1) {
                for (int i = 0; i < count; ++i) {
                    float sum = 0.0f;
                    for (int ch = 0; ch < numIns; ++ch) sum += processed[ch][i];
                    outputs[0][offset + i] = sum / numIns;
                }
            } else {
                for (int ch = 0; ch < numOuts; ++ch)
                    std::memcpy(outputs[ch] + offset, processed[ch < numIns ? ch : 0].data(), count * sizeof(float));
            }
            for(int ch=0;ch<numOuts;++ch) for(int i=0;i<count;++i) outputPeak=std::max(outputPeak,(double)std::abs(outputs[ch][offset+i]));
            outTotalSamples += count;
        }
    }

} // namespace TelephonyDSP
