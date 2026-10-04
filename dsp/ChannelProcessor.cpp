#define NOMINMAX
#include "dsp/ChannelProcessor.h"
#include "dsp/EvsConfig.h"
#include "dsp/G711Codec.h"
#include "dsp/GsmCodec.h"
#include "dsp/AmrNbCodec.h"
#include "dsp/AmrWbCodec.h"
#include "dsp/EvsCodec.h"
#include "dsp/EvsCodecJbm.h"
#include "dsp/OpusCodec.h"
#include "CDSPResampler.h"
#include <cmath>
#include <cstring>

namespace TelephonyDSP {
    using A = AdvancedControl;

    ChannelProcessor::ChannelProcessor(double hostSR)
        : hostSampleRate(hostSR), currentMode(EraMode::Bypass),
          paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
          paramPacketLossRate(0.0f), paramNetworkDegradation(0.0f),
          packetLossSeed(0x12345678u), artifactSeed(12345u), packetLossBurstFrames(0),
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB),
          evsDtxSidInterval(0), evsScVbrEnabled(false), evsG711Law(0),
          opusBandwidth(1105), evsOpusBitrateBps(24000),
          amrWbMode(2), amrNbMode(7),
          downMaxInLen(0), upMaxInLen(0)
    {
        size_t bigSize = 131072;
        ringCodecIn.resize(bigSize);
        ringCodecOut.resize(bigSize);
        outputBuffer.resize(std::max(bigSize,(size_t)std::ceil(hostSampleRate)+8192));
        prepareInternalBuffers(4096);
        setMode(EraMode::Bypass); // Initialize logic
    }

    void ChannelProcessor::setAdvancedSettings(const AdvancedSettings& s) {
        if(advanced==s) return;
        const bool framingChanged=currentMode!=EraMode::Bypass && (
            (currentMode==EraMode::OPUS_VOIP && advanced.get(A::OpusFrameDuration)!=s.get(A::OpusFrameDuration))
            || advanced.enabled(A::NetworkEnabled)!=s.enabled(A::NetworkEnabled)
            || ((advanced.enabled(A::NetworkEnabled) || s.enabled(A::NetworkEnabled)) && advanced.get(A::PlaybackDelayMs)!=s.get(A::PlaybackDelayMs))
            || advanced.get(A::ClockDriftPpm)!=s.get(A::ClockDriftPpm)
            || advanced.enabled(A::HostClock)!=s.enabled(A::HostClock));
        const auto old=advanced;
        advanced=s;
        auto changed=[&](std::initializer_list<A> controls) { for(auto c:controls) if(old.get(c)!=advanced.get(c)) return true; return false; };
        const int rate=codec?codec->getSampleRate():(currentMode==EraMode::EVS_LIKE?32000:8000);
        transport.configure(advanced,paramPacketLossRate);
        if(changed({A::DtmfDigit,A::DtmfLevel,A::DtmfOnMs,A::DtmfGapMs})) {
            dtmf.configure(rate); const int digit=(int)advanced.get(A::DtmfDigit);
            dtmf.setSequence(digit<0?"":std::string(1,"0123456789*#ABCD"[digit]),advanced.get(A::DtmfOnMs),advanced.get(A::DtmfGapMs),20*std::log10(std::max(1e-6,advanced.get(A::DtmfLevel))),true);
        }
        if(changed({A::ClockDriftPpm})) drift.configure(rate,advanced.get(A::ClockDriftPpm));
        if(changed({A::EchoEnabled,A::EchoDelayMs,A::EchoGain,A::EchoCutoffHz})) echo.configure(rate,advanced.get(A::EchoDelayMs),20*std::log10(std::max(1e-6,advanced.get(A::EchoGain))),advanced.get(A::EchoCutoffHz));
        if(changed({A::FadingEnabled,A::FadingDopplerHz,A::CarrierToInterferenceDb})) fading.configure(rate,advanced.get(A::FadingDopplerHz),advanced.get(A::CarrierToInterferenceDb));
        if(changed({A::HandoverIntervalMs,A::HandoverGapMs})) handover.configure(rate,advanced.get(A::HandoverGapMs),advanced.get(A::HandoverIntervalMs)/1000);
        if(changed({A::VadMode})) { vad2.configure(rate); vadHangover=0; }
        if(changed({A::PsdNoise})) comfortNoise.configure(rate);
        if(old.enabled(A::AdaptiveBitrate) && old.enabled(A::FadingEnabled) && !(advanced.enabled(A::AdaptiveBitrate) && advanced.enabled(A::FadingEnabled))) {
            if(auto* nb=dynamic_cast<AMRNBCodec*>(codec.get())) nb->setMode(amrNbMode);
            if(auto* wb=dynamic_cast<AMRWBCodec*>(codec.get())) wb->setMode(amrWbMode);
            if(auto* evs=dynamic_cast<EVSCodec*>(codec.get())) { if(!advanced.enabled(A::EvsAmrWbIo)) evs->reconfigure(evsBitrateBps,evsMaxBandwidth); }
#if TELEPHONY_USE_EVS_JBM
            if(auto* jbm=dynamic_cast<EVSCodecJbm*>(codec.get())) jbm->reconfigure(evsBitrateBps,evsMaxBandwidth);
#endif
        }
        simulatedPathPLC.setSpectralNoise(advanced.enabled(A::PsdNoise));
        applyAdvancedCodec();
        if(changed({A::NetworkEnabled,A::BandwidthNarrowing,A::FilterCascade})) updateFilters();
        if(framingChanged) reset();
    }
    double ChannelProcessor::advancedDelayMs() const {
#if TELEPHONY_USE_EVS_JBM
        // The reference JBM's adaptive playout/time scaling is an intentional
        // time-varying wet effect, not a fixed host processing delay.
        if(currentMode==EraMode::EVS_JBM) return 0;
#endif
        double delay=0;
        if(advanced.enabled(A::NetworkEnabled) && currentMode!=EraMode::Bypass) {
            const double frameMs=codec?1000.0*codec->getFrameSize()/codec->getSampleRate():20;
            delay=std::ceil(advanced.get(A::PlaybackDelayMs)/frameMs)*frameMs;
        }
        if(advanced.get(A::ClockDriftPpm)!=0 && currentMode!=EraMode::Bypass) delay+=2;
        return delay;
    }
    ProcessingTelemetry ChannelProcessor::getTelemetry() const {
        auto t=transport.telemetry();
        if(auto* opus=dynamic_cast<OpusCodec*>(codec.get())) t.opusMode=(int)opus->getActualMode();
        return t;
    }
    void ChannelProcessor::configureAdvancedAudio() {
        const int rate=codec?codec->getSampleRate():(currentMode==EraMode::EVS_LIKE?32000:8000);
        comfortNoise.configure(rate);
        simulatedPathPLC.setSpectralNoise(advanced.enabled(A::PsdNoise));
        dtmf.configure(rate);
        const int digit=(int)advanced.get(A::DtmfDigit);
        dtmf.setSequence(digit<0?"":std::string(1,"0123456789*#ABCD"[digit]),advanced.get(A::DtmfOnMs),advanced.get(A::DtmfGapMs),20*std::log10(std::max(1e-6,advanced.get(A::DtmfLevel))),true);
        drift.configure(rate,advanced.get(A::ClockDriftPpm));
        driftPrimeRemaining=advanced.get(A::ClockDriftPpm)!=0
#if TELEPHONY_USE_EVS_JBM
            && currentMode!=EraMode::EVS_JBM
#endif
            ?drift.latencySamples():0;
        echo.configure(rate,advanced.get(A::EchoDelayMs),20*std::log10(std::max(1e-6,advanced.get(A::EchoGain))),advanced.get(A::EchoCutoffHz));
        fading.configure(rate,advanced.get(A::FadingDopplerHz),advanced.get(A::CarrierToInterferenceDb));
        handover.configure(rate,advanced.get(A::HandoverGapMs),advanced.get(A::HandoverIntervalMs)/1000);
        vad2.configure(rate); vadHangover=0;
        feedbackFrame.assign(codec?codec->getFrameSize():640,0);
        tandemLow1.reset(); tandemLow1.setLowpass(3400,rate); tandemLow2=tandemLow1; tandemCodec.reset();
    }
    void ChannelProcessor::applyAdvancedCodec() {
        if(!codec) return;
        codec->setTransport(advanced.enabled(A::NetworkEnabled)?&transport:nullptr);
        codec->configureBitErrors((float)advanced.get(A::BitErrorRate));
        codec->setSpectralConcealment(advanced.enabled(A::PsdNoise));
        const bool native=currentMode==EraMode::EVS_NATIVE || currentMode==EraMode::AMR_NB_3G || currentMode==EraMode::AMR_WB_VOLTE
#if TELEPHONY_USE_EVS_JBM
            || currentMode==EraMode::EVS_JBM
#endif
            ;
        codec->configureDtx(native || advanced.enabled(A::DtxEnabled),advanced.enabled(A::PureSilence));
        if(auto* opus=dynamic_cast<OpusCodec*>(codec.get())) {
            static constexpr float durations[]={2.5f,5,10,20,40,60};
            opus->setForceMode(OpusMode::Auto);
            opus->setExpertFrameDuration(durations[(int)advanced.get(A::OpusFrameDuration)]);
            opus->setFecEnabled(advanced.enabled(A::OpusFecEnabled));
            opus->setFecOnly(advanced.enabled(A::OpusFecOnly));
            opus->setForceMode(static_cast<OpusMode>((int)advanced.get(A::OpusForceMode)));
            opus->setFecPacketLossPercent(advanced.enabled(A::NetworkEnabled)?(int)advanced.get(A::OpusFecPercent):-1);
        }
        if(auto* evs=dynamic_cast<EVSCodec*>(codec.get())) {
            evs->setAutoBandwidth(advanced.enabled(A::EvsAutoBandwidth));
            static constexpr int ioRates[]={6600,8850,12650,14250,15850,18250,19850,23050,23850};
            evs->setAmrWbIo(advanced.enabled(A::EvsAmrWbIo),ioRates[(int)advanced.get(A::EvsIoMode)]);
        }
#if TELEPHONY_USE_EVS_JBM
        if(auto* jbm=dynamic_cast<EVSCodecJbm*>(codec.get())) {
            jbm->setAutoBandwidth(advanced.enabled(A::EvsAutoBandwidth));
            jbm->setClockDriftPpm(advanced.get(A::ClockDriftPpm));
            jbm->setSafetyMarginMs(advanced.enabled(A::NetworkEnabled)?(int)std::lround(advanced.get(A::PlaybackDelayMs)):60);
        }
#endif
        const int frame=codec->getFrameSize();
        codecFrameF.resize(frame); codecFrameSIn.resize(frame); codecFrameSOut.resize(frame);
        if(feedbackFrame.size()!=(size_t)frame) feedbackFrame.assign(frame,0);
    }
    bool ChannelProcessor::voiceDecision(const int16_t* data,int count,int rate) {
        if(advanced.get(A::VadMode)==1 && vad2.available()) return vad2.processPCM16(data,count).speech;
        double energy=0; for(int i=0;i<count;++i) energy+=double(data[i])*data[i];
        const bool primary=energy/std::max(1,count)>180.0*180.0;
        if(primary) vadHangover=(int)std::ceil(.15*rate/count);
        else if(vadHangover) --vadHangover;
        return primary || vadHangover>0;
    }
    void ChannelProcessor::prepareAdvancedFrame(int frame,int rate) {
        if(feedbackFrame.size()!=(size_t)frame) feedbackFrame.assign(frame,0);
        for(int i=0;i<frame;++i) {
            float value=codecFrameF[i];
            if(advanced.get(A::DtmfDigit)>=0) value+=dtmf.next();
            if(advanced.enabled(A::EchoEnabled)) { value=echo.inject(value); echo.capture(feedbackFrame[i]/32768.f); }
            if(advanced.enabled(A::FadingEnabled)) value=fading.process(value);
            codecFrameSIn[i]=clampToInt16(value*32767.f);
        }
        lastFrameSpeech=advanced.enabled(A::DtxEnabled)?voiceDecision(codecFrameSIn.data(),frame,rate):true;
        if(codec) codec->setVoiceActivity(lastFrameSpeech);
        if(advanced.enabled(A::AdaptiveBitrate) && advanced.enabled(A::FadingEnabled)) {
            const double fraction=fading.state().suggestedModeFraction;
            if(auto* nb=dynamic_cast<AMRNBCodec*>(codec.get())) nb->setMode((int)std::lround(fraction*7));
            if(auto* wb=dynamic_cast<AMRWBCodec*>(codec.get())) wb->setMode((int)std::lround(fraction*8));
            static constexpr int rates[]={7200,8000,9600,13200,16400,24400,32000,48000,64000,96000,128000};
            int choice=rates[(int)std::lround(fraction*10)];
            choice=std::min(choice,evsBitrateBps);
            if(auto* evs=dynamic_cast<EVSCodec*>(codec.get())) { if(!advanced.enabled(A::EvsAmrWbIo)) evs->setBitrate(choice); }
#if TELEPHONY_USE_EVS_JBM
            if(auto* jbm=dynamic_cast<EVSCodecJbm*>(codec.get())) jbm->reconfigure(choice,evsMaxBandwidth);
#endif
        }
    }
    void ChannelProcessor::finishAdvancedFrame(int frame,int rate,bool lost) {
        const bool simulated=currentMode==EraMode::PSTN_G711 || currentMode==EraMode::GSM_FR || currentMode==EraMode::EVS_LIKE;
        const bool silence=codec?codec->isDtxActive():lastOutputDtx;
        if(simulated && advanced.enabled(A::PsdNoise)) {
            if(lost || silence) comfortNoise.generatePCM16(codecFrameSOut.data(),frame);
            else comfortNoise.observePCM16(codecFrameSOut.data(),frame);
        }
        if(silence && advanced.enabled(A::PureSilence)) std::fill(codecFrameSOut.begin(),codecFrameSOut.end(),0);
        if(advanced.enabled(A::TandemNarrowband) && rate%8000==0 && frame*8000/rate<=480) {
            const int factor=rate/8000;
            const int nbCount=frame/factor;
            for(int i=0;i<nbCount;++i) {
                float sum=0;
                for(int j=0;j<factor;++j) sum+=tandemLow2.process(tandemLow1.process(codecFrameSOut[i*factor+j]/32768.f));
                tandemInput[i]=clampToInt16(sum/factor*32767.f);
            }
            tandemCodec.processSamples(tandemInput.data(),tandemOutput.data(),nbCount);
            for(int i=0;i<frame;++i) codecFrameSOut[i]=tandemOutput[i/factor];
        }
        feedbackFrame.assign(codecFrameSOut.begin(),codecFrameSOut.end());
        for(int i=0;i<frame;++i) {
            float value=codecFrameSOut[i]/32768.f;
            if(advanced.get(A::ClockDriftPpm)!=0
#if TELEPHONY_USE_EVS_JBM
                && currentMode!=EraMode::EVS_JBM
#endif
            ) value=drift.process(value);
            if(advanced.get(A::HandoverIntervalMs)>0) value=handover.process(value);
            if(modeTransientRemaining>0) { value+=(float)advanced.get(A::ModeTransient)*modeTransientPrevious*(modeTransientRemaining/32.f); --modeTransientRemaining; }
            codecFrameF[i]=value;
        }
        modeTransientPrevious=codecFrameF.empty()?0:codecFrameF.back();
    }

    ChannelProcessor::~ChannelProcessor() {}

    void ChannelProcessor::setSampleRate(double sr) {
        if (hostSampleRate != sr) {
            hostSampleRate = sr;
            outputBuffer.resize(std::max<size_t>(131072,(size_t)std::ceil(sr)+8192));
            recreateResamplers();
            configureAdvancedAudio();
            updateFilters();
            reset();
        }
    }

    void ChannelProcessor::setMode(EraMode mode) {
        mode = distributionSafeMode(mode);
        if (currentMode != mode) {
            currentMode = mode;
            recreateResamplers();
            updateFilters();
            recreateCodec();
            reset();
        }
    }

    void ChannelProcessor::setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw) {
        normalizeEvsConfig(sampleRateHz, bitrateBps, maxBw);
        bool changed = (evsSampleRate != sampleRateHz)
                    || (evsBitrateBps != bitrateBps)
                    || (evsMaxBandwidth != maxBw);
        evsSampleRate    = sampleRateHz;
        evsBitrateBps    = bitrateBps;
        evsMaxBandwidth  = maxBw;
#if TELEPHONY_USE_EVS_JBM
        // EVS_JBM uses the same EVS configuration knobs as EVS_NATIVE
        // (sample rate / bitrate / max bandwidth), so the same
        // change-detection path applies.
        const bool rebuildForEvs = (currentMode == EraMode::EVS_NATIVE
                                    || currentMode == EraMode::EVS_JBM);
#else
        const bool rebuildForEvs = (currentMode == EraMode::EVS_NATIVE);
#endif
        if(changed && rebuildForEvs && codec && codec->getSampleRate()==sampleRateHz) {
            if(auto* native=dynamic_cast<EVSCodec*>(codec.get())) {
                if(native->reconfigure(bitrateBps,maxBw,advanced.enabled(A::EvsAmrWbIo))) {
                    updateFilters(); modeTransientRemaining=32; return;
                }
            }
#if TELEPHONY_USE_EVS_JBM
            if(auto* jbm=dynamic_cast<EVSCodecJbm*>(codec.get())) {
                if(jbm->reconfigure(bitrateBps,maxBw)) { updateFilters(); modeTransientRemaining=32; return; }
            }
#endif
        }
        if (changed && rebuildForEvs) {
            recreateResamplers();
            recreateCodec();
            updateFilters();
            ringCodecIn.reset(); ringCodecOut.reset(); outputBuffer.reset();
        }
    }

    void ChannelProcessor::setOpusBandwidth(int bw) {
        // Cache first so mode changes that haven't rebuilt an OpusCodec
        // yet (and therefore can't push the value into a live encoder)
        // still pick the caller's preference up on the next recreateCodec()
        // pass. If we do have a live OpusCodec right now, push the new
        // cap directly so the change takes effect on the very next frame.
        opusBandwidth = bw;
        if (codec) {
            OpusCodec* oc = dynamic_cast<OpusCodec*>(codec.get());
            if (oc) {
                oc->setMaxBandwidth(opusBandwidth);
            }
        }
    }

    void ChannelProcessor::setEvsDtxSidInterval(int interval) {
        // Mirror the codec's own validation: 0 (variable) or 3..100
        // (fixed frames). Anything else collapses to 0 to keep the legacy
        // behaviour and to avoid a configuration rejection.
        const int normalized = (interval == 0) ? 0
                              : ((interval >= 3 && interval <= 100) ? interval : 0);
        evsDtxSidInterval = normalized;
        // Push the new value into the live codec if it is an EVS variant.
        // dynamic_cast is safe because EVSCodec / EVSCodecJbm are the only
        // codecs that expose setDtxSidInterval(); other codecs (AMR, G.711,
        // GSM, Opus) keep their previous behaviour.
        if (codec) {
            if (auto* evs = dynamic_cast<EVSCodec*>(codec.get())) {
                evs->setDtxSidInterval(normalized);
            }
#if TELEPHONY_USE_EVS_JBM
            else if (auto* evsJbm = dynamic_cast<EVSCodecJbm*>(codec.get())) {
                evsJbm->setDtxSidInterval(normalized);
            }
#endif
        }
    }

    void ChannelProcessor::setEvsScVbrEnabled(bool enable) {
        evsScVbrEnabled = enable;
        if (auto* evs = dynamic_cast<EVSCodec*>(codec.get())) {
            evs->setScVbrEnabled(enable);
        }
#if TELEPHONY_USE_EVS_JBM
        if (auto* evsJbm = dynamic_cast<EVSCodecJbm*>(codec.get())) {
            evsJbm->setScVbrEnabled(enable);
        }
#endif
    }

    void ChannelProcessor::setAmrNbMode(int mode) {
        // Clamp here too so the stashed value is always in range, even if
        // a future caller bypasses AMRNBCodec's own clamping.
        const int clamped = mode < 0 ? 0 : (mode > 7 ? 7 : mode);
        if (amrNbMode == clamped) return;
        amrNbMode = clamped;
        if(auto* nb=dynamic_cast<AMRNBCodec*>(codec.get())) nb->setMode(clamped);
        modeTransientRemaining=32;

    }

    void ChannelProcessor::setG711Law(int law) {
        const int normalized = law > 0 ? 1 : 0;
        if (evsG711Law == normalized) return;
        evsG711Law = normalized;
        if (auto* g711 = dynamic_cast<G711Codec*>(codec.get()))
            g711->setLaw(normalized != 0);
    }

    void ChannelProcessor::setAmrWbMode(int mode) {
        // Clamp here too so the stashed value is always in range, even if
        // a future caller bypasses AMRWBCodec's own clamping.
        const int clamped = mode < 0 ? 0 : (mode > 8 ? 8 : mode);
        if (amrWbMode == clamped) return;
        amrWbMode = clamped;
        if(auto* wb=dynamic_cast<AMRWBCodec*>(codec.get())) wb->setMode(clamped);
        modeTransientRemaining=32;

    }

    void ChannelProcessor::setOpusBitrate(int bps) {
        // Cache first so mode changes that haven't rebuilt an OpusCodec
        // yet (and therefore can't push the value into a live encoder)
        // still pick the caller's preference up on the next recreateCodec()
        // pass. Clamp here so the stashed value is always within Opus's
        // supported bitrate (6000..510000) range.
        const int clamped = bps < 6000 ? 6000 : (bps > 510000 ? 510000 : bps);
        evsOpusBitrateBps = clamped;
        // If a live OpusCodec is attached, push the new bitrate through
        // OpusCodec::setBitrate() so the change takes effect on the very
        // next frame (and OPUS_SET_BITRATE is re-issued on the encoder).
        if (codec) {
            OpusCodec* oc = dynamic_cast<OpusCodec*>(codec.get());
            if (oc) {
                oc->setBitrate(evsOpusBitrateBps);
            }
        }
        // Updating Opus CTLs in place preserves packet queues and codec
        // history. Rebuilding here also used to discard the requested bitrate.

    }

 void ChannelProcessor::configure(bool artifacts, float amount, float packetLossRate, float networkDegradation) {
        const float clampedLoss = std::clamp(packetLossRate, 0.0f, 0.95f);
        const float clampedDegradation = std::clamp(networkDegradation, 0.0f, 1.0f);
        const bool networkChanged = std::fabs(paramNetworkDegradation - clampedDegradation) > 0.0001f;
        paramArtifactsEnabled = artifacts;
        paramArtifactAmount = amount;
        paramPacketLossRate = clampedLoss;
        paramNetworkDegradation = clampedDegradation;
        if (networkChanged) {
            updateFilters();
        }
        transport.configure(advanced,clampedLoss);
        // Push the clamped values into the codec if one is currently
        // attached. Codecs that don't model a network (the default ICodec
        // base) treat this as a no-op; OpusCodec uses it to drive its
        // encoder CTLs and internal jitter-buffer state.
        if (codec) {
            codec->configureNetwork(advanced.enabled(A::NetworkEnabled)?0.f:clampedLoss, advanced.enabled(A::NetworkEnabled)?0.f:clampedDegradation);
        }
    }

void ChannelProcessor::reset() {
        ringCodecIn.reset(); ringCodecOut.reset(); outputBuffer.reset();
        hpFilter1.reset(); hpFilter2.reset();
        lpFilter1.reset(); lpFilter2.reset();
        packetLossBurstFrames = 0;
        packetLossSeed = 0x12345678u;
        artifactSeed = 12345u;
        simulatedPathPLC.reset(codec ? codec->getFrameSize() : 640);
        transport.reset(); comfortNoise.reset(); dtmf.reset(); drift.reset(); echo.reset(); fading.reset(); handover.reset(); vad2.reset();
        driftPrimeRemaining=advanced.get(A::ClockDriftPpm)!=0
#if TELEPHONY_USE_EVS_JBM
            && currentMode!=EraMode::EVS_JBM
#endif
            ?drift.latencySamples():0;
        std::fill(feedbackFrame.begin(),feedbackFrame.end(),0); vadHangover=0; lastOutputDtx=false;
        tandemLow1.reset();
        tandemLow1.setLowpass(3400,codec?codec->getSampleRate():(currentMode==EraMode::EVS_LIKE?32000:8000));
        tandemLow2=tandemLow1; tandemCodec.reset();
        modeTransientRemaining=0; modeTransientPrevious=0;
        if (resamplerDown) resamplerDown->clear();
        if (resamplerUp) resamplerUp->clear();
        if (codec) codec->reset();
#if TELEPHONY_EXPERIMENTAL_NETWORK
        if (speexAux) speexAux->reset();
#endif
        updateFilters();
    }

    size_t ChannelProcessor::getAvailableOutput() const {
        return outputBuffer.getReadAvailable();
    }

    float ChannelProcessor::getLastVadProb() const {
        if (!speexAux) return -1.0f;
        return speexAux->getSpeechProbability();
    }

    void ChannelProcessor::pushInput(const float* in, int numSamples) {
        if (!in || numSamples <= 0) return;
        if (currentMode == EraMode::Bypass) {
            outputBuffer.write(in, numSamples);
        } else {
            // Unified Resampling Path for all other modes (G.711, GSM, AMR, EVS)
            processCodec(numSamples, in);
        }
    }

    size_t ChannelProcessor::pullOutput(float* out, int numSamples) {
        if (!out || numSamples <= 0) return 0;
        size_t avail = outputBuffer.getReadAvailable();
        size_t count = (std::min)((size_t)numSamples, avail);
        outputBuffer.read(out, count);

        applyArtifacts(out, count);
        return count;
    }

    void ChannelProcessor::recreateResamplers() {
        resamplerDown.reset(); resamplerUp.reset();
        downMaxInLen = 0;
        upMaxInLen = 0;
        int targetSR = 0;
        switch (currentMode) {
            case EraMode::PSTN_G711:
            case EraMode::GSM_FR:
            case EraMode::AMR_NB_3G: targetSR = 8000; break;
            case EraMode::AMR_WB_VOLTE: targetSR = 16000; break;
            case EraMode::EVS_LIKE: targetSR = 32000; break;
            case EraMode::EVS_NATIVE: targetSR = evsSampleRate; break;
#if TELEPHONY_USE_EVS_JBM
            // EVS_JBM drives the EVS encoder+JBM at the configured
            // sample rate, so the resampler target mirrors EVS_NATIVE.
            case EraMode::EVS_JBM: targetSR = evsSampleRate; break;
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
            case EraMode::OPUS_VOIP: targetSR = 48000; break;
#endif
            default: targetSR = 0; break;
        }

        if (targetSR > 0) {
            // aMaxInLen is the maximum number of input samples that may be
            // passed to a single r8brain::CDSPResampler24::process() call.
            // We keep it at 1024 to match the historical buffer size, but
            // processCodec now chunks its calls so callers are free to push
            // more than this per pushInput().
            // Codec PCM is 16-bit. A 96 dB stopband is below its quantization
            // floor; minimum phase and a 10% transition preserve the speech
            // passband while keeping two-leg buffering inside the 150 ms budget.
            // The 24-bit/2% default needed ~600 ms per G.711 round trip.
            downMaxInLen = 1024;
            upMaxInLen = 1024;
            resamplerDown = std::make_unique<r8b::CDSPResampler>(hostSampleRate, targetSR, downMaxInLen, 10.0, 96.0, r8b::fprMinPhase);
            resamplerUp = std::make_unique<r8b::CDSPResampler>(targetSR, hostSampleRate, upMaxInLen, 10.0, 96.0, r8b::fprMinPhase);
        }
    }

    void ChannelProcessor::updateFilters() {
        float sampleRate = 0.0f;
        float highpassHz = 0.0f;
        float lowpassHz = 0.0f;
        float degradedFloorHz = 0.0f;

        auto applyBand = [&]() {
            if (sampleRate <= 0.0f || lowpassHz <= 0.0f) {
                hpFilter1.reset();
                lpFilter1.reset();
                return;
            }

            const float d = advanced.enabled(A::NetworkEnabled)?(float)advanced.get(A::BandwidthNarrowing):std::clamp(paramNetworkDegradation, 0.0f, 1.0f);
            highpassHz = highpassHz + d * ((std::max)(highpassHz, 260.0f) - highpassHz);
            lowpassHz = lowpassHz - d * (lowpassHz - degradedFloorHz);
            lowpassHz = std::clamp(lowpassHz, highpassHz + 300.0f, sampleRate * 0.45f);

            const int ramp=(int)std::lround(sampleRate*.005*(1-advanced.get(A::ModeTransient)));
            hpFilter1.setHighpass(highpassHz, sampleRate,ramp);
            lpFilter1.setLowpass(lowpassHz, sampleRate,ramp);
        };

        switch (currentMode) {
            case EraMode::PSTN_G711:
            case EraMode::GSM_FR:
                sampleRate = 8000.0f; // Filters apply at Codec SR
                highpassHz = 300.0f;
                lowpassHz = 3400.0f;
                degradedFloorHz = 1800.0f;
                applyBand();
                break;
            case EraMode::AMR_NB_3G:
                sampleRate = 8000.0f;
                highpassHz = 200.0f;
                lowpassHz = 3400.0f;
                degradedFloorHz = 1800.0f;
                applyBand();
                break;
            case EraMode::AMR_WB_VOLTE:
                sampleRate = 16000.0f;
                highpassHz = 50.0f;
                lowpassHz = 7000.0f;
                degradedFloorHz = 3400.0f;
                applyBand();
                break;
            case EraMode::EVS_LIKE:
                sampleRate = 32000.0f;
                highpassHz = 50.0f;
                lowpassHz = 14000.0f;
                degradedFloorHz = 4000.0f;
                applyBand();
                break;
            case EraMode::EVS_NATIVE:
                if ((advanced.enabled(A::NetworkEnabled)?advanced.get(A::BandwidthNarrowing):paramNetworkDegradation) <= 0.001f) {
                    // The real EVS already shapes its own bandwidth; skip the
                    // extra cascade unless the radio/path simulation asks for it.
                    hpFilter1.reset();
                    lpFilter1.reset();
                } else {
                    sampleRate = (float)evsSampleRate;
                    highpassHz = 50.0f;
                    switch (evsMaxBandwidth) {
                        case EVS_NB:  lowpassHz = 3400.0f; break;
                        case EVS_WB:  lowpassHz = 7000.0f; break;
                        case EVS_SWB: lowpassHz = 14000.0f; break;
                        case EVS_FB:  lowpassHz = 20000.0f; break;
                        default:      lowpassHz = 14000.0f; break;
                    }
                    lowpassHz = (std::min)(lowpassHz, sampleRate * 0.45f);
                    degradedFloorHz = 3400.0f;
                    applyBand();
                }
                break;
#if TELEPHONY_USE_EVS_JBM
            case EraMode::EVS_JBM:
                // EVS_JBM runs the real EVS encoder + JBM at the
                // configured sample rate, so its filter profile
                // matches EVS_NATIVE.
                if ((advanced.enabled(A::NetworkEnabled)?advanced.get(A::BandwidthNarrowing):paramNetworkDegradation) <= 0.001f) {
                    hpFilter1.reset();
                    lpFilter1.reset();
                } else {
                    sampleRate = (float)evsSampleRate;
                    highpassHz = 50.0f;
                    switch (evsMaxBandwidth) {
                        case EVS_NB:  lowpassHz = 3400.0f; break;
                        case EVS_WB:  lowpassHz = 7000.0f; break;
                        case EVS_SWB: lowpassHz = 14000.0f; break;
                        case EVS_FB:  lowpassHz = 20000.0f; break;
                        default:      lowpassHz = 14000.0f; break;
                    }
                    lowpassHz = (std::min)(lowpassHz, sampleRate * 0.45f);
                    degradedFloorHz = 3400.0f;
                    applyBand();
                }
                break;
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
            case EraMode::OPUS_VOIP:
                if ((advanced.enabled(A::NetworkEnabled)?advanced.get(A::BandwidthNarrowing):paramNetworkDegradation) <= 0.001f) {
                    // Opus already shapes its own bandwidth (up to 20 kHz FB);
                    // skip the extra cascade unless the path simulation asks.
                    hpFilter1.reset();
                    lpFilter1.reset();
                } else {
                    sampleRate = 48000.0f;
                    highpassHz = 50.0f;
                    lowpassHz = 18000.0f;
                    degradedFloorHz = 3400.0f;
                    applyBand();
                }
                break;
#endif
            default:
                hpFilter1.reset();
                lpFilter1.reset();
                break;
        }
        // Cascade setup: Both stages identical for 24dB/oct
        hpFilter2 = hpFilter1;
        lpFilter2 = lpFilter1;
    }

    float ChannelProcessor::nextPacketRandom() {
        packetLossSeed = packetLossSeed * 1664525u + 1013904223u;
        return (float)((packetLossSeed >> 8) & 0x00FFFFFFu) / 16777216.0f;
    }

    bool ChannelProcessor::shouldDropPacket() {
        if(advanced.enabled(A::NetworkEnabled)) return false;
        const float degradation = std::clamp(paramNetworkDegradation, 0.0f, 1.0f);
        const float derivedLoss = degradation * degradation * 0.08f;
        const float lossRate = std::clamp(paramPacketLossRate + derivedLoss, 0.0f, 0.95f);
        if (lossRate <= 0.0001f) return false;

        if (packetLossBurstFrames > 0) {
            --packetLossBurstFrames;
            return true;
        }

        const float meanBurstFrames = 1.0f + degradation * 3.0f;
        const float startProbability = lossRate / meanBurstFrames;
        if (nextPacketRandom() >= startProbability) return false;

        const int burst = 1 + (int)(nextPacketRandom() * meanBurstFrames);
        packetLossBurstFrames = (std::max)(0, burst - 1);
        return true;
    }

void ChannelProcessor::recreateCodec() {
        codec.reset();
        speexAux.reset();
        switch (currentMode) {
            case EraMode::PSTN_G711: codec = std::make_unique<G711Codec>(8000, evsG711Law > 0); break;
            case EraMode::GSM_FR: codec = std::make_unique<GSMCodec>(); break;
            case EraMode::AMR_NB_3G: codec = std::make_unique<AMRNBCodec>(true, amrNbMode); break;
            case EraMode::AMR_WB_VOLTE: codec = std::make_unique<AMRWBCodec>(true, amrWbMode); break;
            case EraMode::EVS_LIKE:
                codecFrameF.resize(32000 / 50);
                codecFrameSIn.resize(32000 / 50);
                codecFrameSOut.resize(32000 / 50);
                simulatedPathPLC.reset(32000 / 50);
                // EVS_LIKE is a filter-only path. Do not attach the Opus
                // speech gate here: quiet valid input must not become PLC.
                configureAdvancedAudio();
                return;
            case EraMode::EVS_NATIVE:
                codec = std::make_unique<EVSCodec>(evsSampleRate, evsBitrateBps, evsMaxBandwidth,
                                                   evsScVbrEnabled, evsDtxSidInterval);
                break;
#if TELEPHONY_USE_EVS_JBM
            case EraMode::EVS_JBM:
                // EVSCodecJbm runs the real EVS encoder + Stage-1 JBM
                // in one ICodec. See the class header comment in
                // TelephonyDSP.h for the full design.
                codec = std::make_unique<EVSCodecJbm>(evsSampleRate, evsBitrateBps, evsMaxBandwidth,
                                                     evsDtxSidInterval, evsScVbrEnabled);
                break;
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
            case EraMode::OPUS_VOIP:
                // 48 kHz, 24 kbps, complexity 6 (moderate). See OpusCodec.
                // The 4th argument carries the caller's OPUS_BANDWIDTH_*
                // cap (default 1105 = FB) so a live setOpusBandwidth()
                // change is honored on the very next codec rebuild.
                codec = std::make_unique<OpusCodec>(48000, evsOpusBitrateBps, 6, opusBandwidth);
                break;
#endif
            default: break;
        }
        if (codec) {
            int fs = codec->getFrameSize();
            int sr = codec->getSampleRate();
            codecFrameF.resize(fs); codecFrameSIn.resize(fs); codecFrameSOut.resize(fs);
            simulatedPathPLC.reset(fs);
            // Push the current clamped network parameters into the freshly
            // created codec so its first packet already reflects the
            // caller's loss/degradation settings rather than defaults.
            codec->configureNetwork(paramPacketLossRate, paramNetworkDegradation);
#if TELEPHONY_EXPERIMENTAL_NETWORK
            // Bring the experimental SpeexDSP helper online with the same
            // rate/frame as the codec. It does not change audio output yet
            // (see SpeexDSPAux header comment for the follow-up plan).
            if (!speexAux) speexAux = std::make_unique<SpeexDSPAux>();
            speexAux->configure(sr, fs);
#endif
        }
        configureAdvancedAudio();
        applyAdvancedCodec();
    }

    void ChannelProcessor::prepareInternalBuffers(int maxBlockSize) {
        resampInBuf.resize(maxBlockSize);
        tempProcessBuf.resize(maxBlockSize);
    }

    // Process logic: Downsample -> Filter -> Codec -> Buffer (Wait for Upsample?)
    // Note: r8brain is streaming. Filters are streaming. Codec is block or sample based.
    // 1. Downsample (Host SR -> Codec SR)   -- chunked to never exceed aMaxInLen
    // 2. Filter (at Codec SR)
    // 3. Codec (at Codec SR)
    // 4. Upsample (Codec SR -> Host SR)     -- chunked to never exceed aMaxInLen
    //    -> outputBuffer
    void ChannelProcessor::processCodec(int numSamples, const float* in) {
         if (numSamples <= 0) return;

         // ---- Stage 1: downsample, chunked so each process() call receives
         //               at most downMaxInLen input samples ----
         if (downMaxInLen > 0) {
             if (resampInBuf.size() < (size_t)downMaxInLen) resampInBuf.resize(downMaxInLen);
             int inOffset = 0;
             while (inOffset < numSamples) {
                 const int inChunk = std::min(numSamples - inOffset, downMaxInLen);
                 for (int i = 0; i < inChunk; ++i)
                     resampInBuf[i] = (double)in[inOffset + i];

                 double* dOutRaw = nullptr;
                 const int downCount = resamplerDown->process(resampInBuf.data(), inChunk, dOutRaw);

                 // The pointer returned by r8brain is owned by the resampler
                 // and stays valid until the next process() call, so it is
                 // safe to consume within this iteration of the chunk loop.
                 if ((int)tempProcessBuf.size() < downCount) tempProcessBuf.resize(downCount);
                 for (int i = 0; i < downCount; ++i) {
                     float x = (float)dOutRaw[i];
                     x = hpFilter1.process(x);
                     if(!advanced.enabled(A::NetworkEnabled) || advanced.enabled(A::FilterCascade)) x = hpFilter2.process(x);
                     x = lpFilter1.process(x);
                     if(!advanced.enabled(A::NetworkEnabled) || advanced.enabled(A::FilterCascade)) x = lpFilter2.process(x);
                     tempProcessBuf[i] = x;
                 }
                 ringCodecIn.write(tempProcessBuf.data(), downCount);
                 inOffset += inChunk;
             }
         }

         // Each input codec frame advances encoder and transport exactly once.
         // Initial transport/drift prefill is omitted here and included in the
         // host latency reserve instead, never double-counted as audio silence.
         if(currentMode==EraMode::EVS_LIKE || codec) {
             const int frame=codec?codec->getFrameSize():640;
             const int rate=codec?codec->getSampleRate():32000;
             while(ringCodecIn.getReadAvailable()>=(size_t)frame) {
                 ringCodecIn.read(codecFrameF.data(),frame);
                 prepareAdvancedFrame(frame,rate);
                 bool lost=shouldDropPacket();
                 if(codec) {
                     codec->processFrame(codecFrameSIn.data(),codecFrameSOut.data(),lost);
                     if(advanced.enabled(A::NetworkEnabled)) lost=!transport.lastPacketReceived();
                 } else {
                     if(advanced.enabled(A::NetworkEnabled)) {
                         std::vector<uint8_t> bytes(frame*2);
                         for(int i=0;i<frame;++i) { const uint16_t v=(uint16_t)codecFrameSIn[i]; bytes[2*i]=(uint8_t)(v>>8); bytes[2*i+1]=(uint8_t)v; }
                         CodecPlayout played;
                         lost=!transport.exchange({CodecPacketFormat::LinearPcm16,rate,frame,advanced.enabled(A::DtxEnabled) && !lastFrameSpeech,false},bytes.data(),bytes.size(),false,played);
                         if(!lost && played.payload.size()==bytes.size()) {
                             for(int i=0;i<frame;++i) codecFrameSOut[i]=(int16_t)((uint16_t(played.payload[2*i])<<8)|played.payload[2*i+1]);
                             lastOutputDtx=played.dtx;
                         } else lost=true;
                     } else {
                         std::copy(codecFrameSIn.begin(),codecFrameSIn.end(),codecFrameSOut.begin());
                         lastOutputDtx=advanced.enabled(A::DtxEnabled) && !lastFrameSpeech;
                     }
                     if(lost || lastOutputDtx) simulatedPathPLC.conceal(codecFrameSOut.data(),frame);
                     else simulatedPathPLC.storeGoodFrame(codecFrameSOut.data(),frame);
                 }
                 finishAdvancedFrame(frame,rate,lost);
                 if(advanced.enabled(A::NetworkEnabled) && transport.warmingUp()
#if TELEPHONY_USE_EVS_JBM
                     && currentMode!=EraMode::EVS_JBM
#endif
                 ) continue;
                 if(!advanced.enabled(A::NetworkEnabled)) {
                     if(auto* opus=dynamic_cast<OpusCodec*>(codec.get())) if(opus->isInternalTransportWarming()) continue;
                 }
                 const size_t skip=std::min(driftPrimeRemaining,(size_t)frame);
                 driftPrimeRemaining-=skip;
                 ringCodecOut.write(codecFrameF.data()+skip,frame-skip);
             }
         }

         // ---- Stage 3: upsample, chunked so each process() call receives
         //               at most upMaxInLen input samples ----
         int codecOutAvail = (int)ringCodecOut.getReadAvailable();
         if (codecOutAvail > 0 && upMaxInLen > 0) {
             if ((int)tempProcessBuf.size() < codecOutAvail) tempProcessBuf.resize(codecOutAvail);
             ringCodecOut.read(tempProcessBuf.data(), codecOutAvail);

             if (resampInBuf.size() < (size_t)upMaxInLen) resampInBuf.resize(upMaxInLen);
             int upOffset = 0;
             while (upOffset < codecOutAvail) {
                 const int upChunk = std::min(codecOutAvail - upOffset, upMaxInLen);
                 for (int i = 0; i < upChunk; ++i)
                     resampInBuf[i] = (double)tempProcessBuf[upOffset + i];

                 double* dUpOutRaw = nullptr;
                 const int upCount = resamplerUp->process(resampInBuf.data(), upChunk, dUpOutRaw);

                 // r8brain's output pointer is invalidated by the next
                 // process() call, so copy it into a member-scope scratch
                 // buffer (resampUpOutBuf) and write to outputBuffer before
                 // looping. Streaming order is preserved by chunking in input
                 // order.
                 if ((int)resampUpOutBuf.size() < upCount) resampUpOutBuf.resize(upCount);
                 for (int i = 0; i < upCount; ++i)
                     resampUpOutBuf[i] = (float)dUpOutRaw[i];
                 outputBuffer.write(resampUpOutBuf.data(), upCount);
                 upOffset += upChunk;
             }
         }
    }

    void ChannelProcessor::applyArtifacts(float* buffer, int numSamples) {
        if (!paramArtifactsEnabled || paramArtifactAmount <= 0.001f) return;
        auto randf = [&]() {
            artifactSeed = artifactSeed * 1664525u + 1013904223u;
            return (float)(artifactSeed & 0xFFFF) / 32768.0f - 1.0f;
        };
        for (int i=0; i<numSamples; ++i) {
            if (currentMode == EraMode::GSM_FR && ((randf() + 1.0f) * 500.0f) < (10 * paramArtifactAmount)) buffer[i] += randf() * 0.5f * paramArtifactAmount;
            // General noise
            buffer[i] += randf() * 0.01f * paramArtifactAmount;
        }
    }

} // namespace TelephonyDSP
