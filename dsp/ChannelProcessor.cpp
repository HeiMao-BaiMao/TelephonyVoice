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
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB),
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
            case EraMode::PSTN_G711: codec = std::make_unique<G711Codec>(8000); break;
            case EraMode::GSM_FR: codec = std::make_unique<GSMCodec>(); break;
            case EraMode::AMR_NB_3G: codec = std::make_unique<AMRNBCodec>(); break;
            case EraMode::AMR_WB_VOLTE: codec = std::make_unique<AMRWBCodec>(); break;
            case EraMode::EVS_LIKE:
                codecFrameF.resize(32000 / 50);
                codecFrameSIn.resize(32000 / 50);
                codecFrameSOut.resize(32000 / 50);
                simulatedPathPLC.reset(32000 / 50);
                return;
            case EraMode::EVS_NATIVE:
                codec = std::make_unique<EVSCodec>(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
                break;
#if TELEPHONY_USE_EVS_JBM
            case EraMode::EVS_JBM:
                // EVSCodecJbm runs the real EVS encoder + Stage-1 JBM
                // in one ICodec. See the class header comment in
                // TelephonyDSP.h for the full design.
                codec = std::make_unique<EVSCodecJbm>(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
                break;
#endif
#if TELEPHONY_EXPERIMENTAL_NETWORK
            case EraMode::OPUS_VOIP:
                // 48 kHz, 24 kbps, complexity 6 (moderate). See OpusCodec.
                codec = std::make_unique<OpusCodec>(48000, 24000, 6);
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
        static uint32_t seed = 12345;
        auto randf = [&]() { seed = seed * 1664525 + 1013904223; return (float)(seed & 0xFFFF) / 32768.0f - 1.0f; };
        for (int i=0; i<numSamples; ++i) {
            if (currentMode == EraMode::GSM_FR && (rand() % 1000) < (10 * paramArtifactAmount)) buffer[i] += randf() * 0.5f * paramArtifactAmount;
            // General noise
            buffer[i] += randf() * 0.01f * paramArtifactAmount;
        }
    }

} // namespace TelephonyDSP
