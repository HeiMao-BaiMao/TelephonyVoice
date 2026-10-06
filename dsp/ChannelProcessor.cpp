#define NOMINMAX
#include "dsp/ChannelProcessor.h"
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

    ChannelProcessor::ChannelProcessor(double hostSR)
        : hostSampleRate(hostSR), currentMode(EraMode::Bypass),
          paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
          paramPacketLossRate(0.0f), paramNetworkDegradation(0.0f),
          packetLossSeed(0x12345678u), packetLossBurstFrames(0),
          artifactSeed(0xA5A5F00Du),
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB),
          evsDtxSidInterval(0),
          // These four used to be left out of the initialiser list and were
          // therefore read while uninitialised: setG711Law() compares against
          // the cached law (and now short-circuits on an unchanged value),
          // setEvsScVbrEnabled()/setOpusBandwidth()/setAmrNbMode() are all
          // consulted by recreateCodec().  Seed them with the same defaults
          // SignalProcessor and the codec classes use.
          evsScVbrEnabled(false),
          evsG711Law(0),        // 0 = mu-law
          opusBandwidth(kOpusBandwidthFullband),
          evsOpusBitrateBps(24000), // default Opus target bitrate (24 kbps).
          amrWbMode(2), // 12.65 kbps; matches AMRWBCodec's own default.
          amrNbMode(7), // 12.2 kbps (MR122); matches AMRNBCodec's own default.
          downMaxInLen(0), upMaxInLen(0)
    {
        size_t bigSize = 131072;
        ringCodecIn.resize(bigSize);
        ringCodecOut.resize(bigSize);
        outputBuffer.resize(bigSize);
        prepareInternalBuffers(4096);
        setMode(EraMode::Bypass); // Initialize logic
    }

    ChannelProcessor::~ChannelProcessor() {}

    void ChannelProcessor::setSampleRate(double sr) {
        if (hostSampleRate != sr) { 
            hostSampleRate = sr; 
            recreateResamplers(); 
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
            ringCodecIn.reset(); ringCodecOut.reset();
        }
    }

    void ChannelProcessor::setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw) {
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
        if (changed && rebuildForEvs) {
            recreateResamplers();
            recreateCodec();
            ringCodecIn.reset(); ringCodecOut.reset();
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
        // Always rebuild the active AMR_NB_3G codec so the freshly
        // constructed AMRNBCodec picks up the new mode. The change is
        // stashed for non-AMR_NB_3G modes and applied the next time
        // recreateCodec() builds an AMRNBCodec.
        if (currentMode == EraMode::AMR_NB_3G) {
            recreateCodec();
            ringCodecIn.reset(); ringCodecOut.reset();
        }
    }

    void ChannelProcessor::setG711Law(int law) {
        // No-op when the law did not change.  This setter is called on every
        // parameter update (SignalProcessor::applyRouteToChannels() mirrors
        // the cached value into both legs, and the VST calls that on every
        // parameter change), so the unconditional recreateCodec() below used
        // to destroy and re-create the G.711 codec - including its resampler
        // and PLC state - on every knob move while PSTN_G711 was selected.
        if (evsG711Law == law) return;
        evsG711Law = law;
        // If the active codec is a G.711 codec, push the new law flag through
        // in place; otherwise the change is remembered in evsG711Law and picked
        // up the next time recreateCodec() builds a G.711 codec.
        if (codec) {
            G711Codec* g711 = dynamic_cast<G711Codec*>(codec.get());
            if (g711) {
                g711->setLaw(law > 0);
            }
        }
        if (currentMode == EraMode::PSTN_G711) {
            // Rebuild so the freshly-created G711Codec is constructed with the
            // new evsG711Law flag and resampler/buffer state is consistent.
            recreateCodec();
        }
    }

    void ChannelProcessor::setAmrWbMode(int mode) {
        // Clamp here too so the stashed value is always in range, even if
        // a future caller bypasses AMRWBCodec's own clamping.
        const int clamped = mode < 0 ? 0 : (mode > 8 ? 8 : mode);
        if (amrWbMode == clamped) return;
        amrWbMode = clamped;
        // If the active codec is an AMR-WB codec, push the new mode
        // through in place (its setMode() also resets encoder state);
        // otherwise the change is remembered in amrWbMode and picked up
        // the next time recreateCodec() builds an AMRWBCodec.
        if (codec) {
            AMRWBCodec* amrWb = dynamic_cast<AMRWBCodec*>(codec.get());
            if (amrWb) {
                amrWb->setMode(amrWbMode);
            }
        }
        if (currentMode == EraMode::AMR_WB_VOLTE) {
            // Rebuild so the freshly-created AMRWBCodec is constructed
            // with the new amrWbMode and resampler/buffer state is
            // consistent. AMRWBCodec::setMode() already resets the
            // encoder state for the in-place path above, so we only
            // need the full recreateCodec() for the case where no live
            // codec was attached.
            recreateCodec();
            ringCodecIn.reset(); ringCodecOut.reset();
        }
    }

    void ChannelProcessor::setOpusBitrate(int bps) {
        // Cache first so mode changes that haven't rebuilt an OpusCodec
        // yet (and therefore can't push the value into a live encoder)
        // still pick the caller's preference up on the next recreateCodec()
        // pass. Clamp here so the stashed value is always within Opus's
        // legal OPUS_BITRATE_MIN..OPUS_BITRATE_MAX (6..510000) range.
        const int clamped = bps < 6 ? 6 : (bps > 510000 ? 510000 : bps);
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
        if (currentMode == EraMode::OPUS_VOIP) {
            // The current OpusCodec's bitstream buffer is sized in the
            // ctor against the original sample rate and an assumed upper
            // bitrate; for a small (e.g. 6 kbps) target the existing
            // buffer is still comfortably oversized. We rebuild anyway
            // so a future rate change can't strand a too-small buffer
            // and so the encoder state is consistent with the new
            // target bitrate.
            recreateCodec();
            ringCodecIn.reset(); ringCodecOut.reset();
        }
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
        // Push the clamped values into the codec if one is currently
        // attached. Codecs that don't model a network (the default ICodec
        // base) treat this as a no-op; OpusCodec uses it to drive its
        // encoder CTLs and internal jitter-buffer state.
        if (codec) {
            codec->configureNetwork(clampedLoss, clampedDegradation);
        }
    }

void ChannelProcessor::reset() {
        ringCodecIn.reset(); ringCodecOut.reset(); outputBuffer.reset();
        hpFilter1.reset(); hpFilter2.reset();
        lpFilter1.reset(); lpFilter2.reset();
        packetLossBurstFrames = 0;
        // Re-seed the artifact generator so a reset() (host seek / transport
        // restart) reproduces the same deterministic noise pattern.
        artifactSeed = 0xA5A5F00Du;
        simulatedPathPLC.reset(640);
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
        if (currentMode == EraMode::Bypass) {
            outputBuffer.write(in, numSamples);
        } else {
            // Unified Resampling Path for all other modes (G.711, GSM, AMR, EVS)
            processCodec(numSamples, in);
        }
    }

    size_t ChannelProcessor::pullOutput(float* out, int numSamples) {
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
            downMaxInLen = 1024;
            upMaxInLen = 1024;
            resamplerDown = std::make_unique<r8b::CDSPResampler24>(hostSampleRate, targetSR, downMaxInLen);
            resamplerUp = std::make_unique<r8b::CDSPResampler24>(targetSR, hostSampleRate, upMaxInLen);
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

            const float d = std::clamp(paramNetworkDegradation, 0.0f, 1.0f);
            highpassHz = highpassHz + d * ((std::max)(highpassHz, 260.0f) - highpassHz);
            lowpassHz = lowpassHz - d * (lowpassHz - degradedFloorHz);
            lowpassHz = std::clamp(lowpassHz, highpassHz + 300.0f, sampleRate * 0.45f);

            hpFilter1.setHighpass(highpassHz, sampleRate);
            lpFilter1.setLowpass(lowpassHz, sampleRate);
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
                if (paramNetworkDegradation <= 0.001f) {
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
                if (paramNetworkDegradation <= 0.001f) {
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
                if (paramNetworkDegradation <= 0.001f) {
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

    // Deterministic [0,1) generator for the artifact path.  Same LCG as
    // nextPacketRandom() but with its own state so loss and artifact
    // randomness do not perturb each other.
    float ChannelProcessor::nextArtifactRandom() {
        artifactSeed = artifactSeed * 1664525u + 1013904223u;
        return (float)((artifactSeed >> 8) & 0x00FFFFFFu) / 16777216.0f;
    }

    bool ChannelProcessor::shouldDropPacket() {
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
                                                     evsDtxSidInterval);
                break;
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
            case EraMode::OPUS_VOIP:
                // 48 kHz, 24 kbps, complexity 6 (moderate). See OpusCodec.
                // The 4th argument carries the caller's OPUS_BANDWIDTH_*
                // cap (default 1105 = FB) so a live setOpusBandwidth()
                // change is honored on the very next codec rebuild.
                codec = std::make_unique<OpusCodec>(48000, 24000, 6, opusBandwidth);
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
                     x = hpFilter2.process(x);
                     x = lpFilter1.process(x);
                     x = lpFilter2.process(x);
                     tempProcessBuf[i] = x;
                 }
                 ringCodecIn.write(tempProcessBuf.data(), downCount);
                 inOffset += inChunk;
             }
         }

         // ---- Stage 2: codec/EVS_LIKE processing ----
         // The SpeexDSPAux energy VAD now drives DTX for OPUS_VOIP and
         // EVS_LIKE: when it classifies the current frame as silence, the
         // effective `packetLost` flag is forced true so the codec path
         // emits comfort noise (Opus's own CNG via DTX, the simulated
         // path PLC for EVS_LIKE) instead of transmitting the silent
         // frame. EVS_NATIVE / EVS_JBM have their own 3GPP VAD and are
         // intentionally left alone; AMR / G.711 / GSM are not in the
         // experimental network scope and are not touched here either.
         if (currentMode == EraMode::EVS_LIKE) {
             const int frameSize = 32000 / 50;
             while (ringCodecIn.getReadAvailable() >= (size_t)frameSize) {
                 ringCodecIn.read(codecFrameF.data(), frameSize);
                 for (int i = 0; i < frameSize; ++i) {
                     codecFrameSIn[i] = clampToInt16(codecFrameF[i] * 32767.0f);
                 }

                 // Run SpeexDSPAux (denoise + cached energy-VAD) on the
                 // freshly-quantized int16 frame so lastFrameIsSpeech()
                 // reflects *this* frame, not a previous one.
                 if (speexAux) speexAux->runPreprocess(codecFrameSIn.data());
                 const bool energyVadSilence =
                     speexAux && !speexAux->lastFrameIsSpeech();

                 if (shouldDropPacket() || energyVadSilence) {
                     simulatedPathPLC.conceal(codecFrameSOut.data(), frameSize);
                 } else {
                     std::memcpy(codecFrameSOut.data(), codecFrameSIn.data(), frameSize * sizeof(int16_t));
                     simulatedPathPLC.storeGoodFrame(codecFrameSOut.data(), frameSize);
                 }

                 for (int i = 0; i < frameSize; ++i) {
                     codecFrameF[i] = codecFrameSOut[i] / 32768.0f;
                 }
                 ringCodecOut.write(codecFrameF.data(), frameSize);
             }
         } else if (codec) {
             int frameSize = codec->getFrameSize();
             while (ringCodecIn.getReadAvailable() >= (size_t)frameSize) {
                 ringCodecIn.read(codecFrameF.data(), frameSize);
                 for(int i=0; i<frameSize; ++i) codecFrameSIn[i] = clampToInt16(codecFrameF[i] * 32767.0f);

                 bool packetLost = shouldDropPacket();
                 // Energy-VAD-driven DTX is only wired into the
                 // experimental OPUS_VOIP mode here; other codecs in the
                 // generic branch (AMR, G.711, GSM) keep their existing
                 // packet-loss-only behavior. EVS_NATIVE / EVS_JBM never
                 // reach this branch (they have their own dispatch).
                 if (currentMode == EraMode::OPUS_VOIP && speexAux) {
                     speexAux->runPreprocess(codecFrameSIn.data());
                     if (!speexAux->lastFrameIsSpeech()) {
                         packetLost = true; // force Opus DTX/CNG
                     }
                 }

                 codec->processFrame(codecFrameSIn.data(), codecFrameSOut.data(), packetLost);
                 for(int i=0; i<frameSize; ++i) codecFrameF[i] = codecFrameSOut[i] / 32768.0f;
                 ringCodecOut.write(codecFrameF.data(), frameSize);
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
        // Deterministic, per-instance LCG (see nextArtifactRandom()).  The
        // previous implementation used a function-local `static` seed plus
        // std::rand(), which made the artifact noise depend on how many
        // ChannelProcessor instances existed, on their processing order, and
        // on the C runtime's global rand() state - i.e. offline renders were
        // not reproducible.
        const float gsmClickProbability = 10.0f * paramArtifactAmount / 1000.0f;
        for (int i = 0; i < numSamples; ++i) {
            if (currentMode == EraMode::GSM_FR &&
                nextArtifactRandom() < gsmClickProbability) {
                buffer[i] += (nextArtifactRandom() * 2.0f - 1.0f) * 0.5f * paramArtifactAmount;
            }
            // General noise
            buffer[i] += (nextArtifactRandom() * 2.0f - 1.0f) * 0.01f * paramArtifactAmount;
        }
    }

} // namespace TelephonyDSP
