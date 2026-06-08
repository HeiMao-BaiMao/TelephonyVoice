#define NOMINMAX
#include "TelephonyDSP.h"
#include <cstring>
#include <algorithm>
#include <cmath>

extern "C" {
#include "gsm.h"
}
#ifndef TELEPHONY_DISTRIBUTION_BUILD
#include "interf_enc.h" // AMR-NB
#include "interf_dec.h"
#include "enc_if.h" // AMR-WB (vo-amrwbenc)
#include "dec_if.h" // AMR-WB (opencore-amrwb)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace TelephonyDSP {

#ifdef TELEPHONY_DISTRIBUTION_BUILD
    static EraMode distributionSafeMode(EraMode mode) {
        return mode == EraMode::EVS_NATIVE ? EraMode::EVS_LIKE : mode;
    }
#else
    static EraMode distributionSafeMode(EraMode mode) {
        return mode;
    }
#endif

    static int16_t clampToInt16(float v) {
        return (int16_t)std::clamp(v, -32768.0f, 32767.0f);
    }

    void WaveformConcealer::reset(int frameSize) {
        history.assign((std::max)(frameSize * 4, frameSize), 0);
        attenuation = 1.0f;
        lastPitch = (std::max)(20, frameSize / 2);
    }

    void WaveformConcealer::storeGoodFrame(const int16_t* frame, int frameSize) {
        if (frameSize <= 0) return;
        if (history.size() < (size_t)(frameSize * 4)) {
            reset(frameSize);
        }

        std::memmove(history.data(), history.data() + frameSize,
                     (history.size() - frameSize) * sizeof(int16_t));
        std::memcpy(history.data() + history.size() - frameSize, frame, frameSize * sizeof(int16_t));
        attenuation = 1.0f;
        lastPitch = estimatePitch(frameSize);
    }

    int WaveformConcealer::estimatePitch(int frameSize) const {
        if (frameSize <= 0 || history.size() < (size_t)(frameSize * 2)) {
            return (std::max)(20, frameSize / 2);
        }

        const int histSize = (int)history.size();
        const int current = histSize - frameSize;
        const int minLag = (std::max)(20, frameSize / 8);
        const int maxLag = (std::min)(frameSize * 2, current - 1);
        if (maxLag <= minLag) return (std::max)(20, frameSize / 2);

        float bestScore = -1.0f;
        int bestLag = lastPitch;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            const int ref = current - lag;
            if (ref < 0) break;

            double corr = 0.0;
            double e1 = 1.0;
            double e2 = 1.0;
            for (int i = 0; i < frameSize; ++i) {
                const double a = history[current + i];
                const double b = history[ref + i];
                corr += a * b;
                e1 += a * a;
                e2 += b * b;
            }
            const float score = (float)(corr / std::sqrt(e1 * e2));
            if (score > bestScore) {
                bestScore = score;
                bestLag = lag;
            }
        }

        return bestScore > 0.15f ? bestLag : (std::max)(20, frameSize / 2);
    }

    void WaveformConcealer::conceal(int16_t* out, int frameSize) {
        if (frameSize <= 0) return;
        if (history.size() < (size_t)(frameSize * 2)) {
            reset(frameSize);
        }

        const int histSize = (int)history.size();
        const int pitch = std::clamp(lastPitch, 1, histSize);
        const int start = histSize - pitch;
        std::vector<int16_t> concealed(frameSize);

        for (int i = 0; i < frameSize; ++i) {
            const float intraFrameFade = 1.0f - 0.15f * ((float)i / (float)(std::max)(1, frameSize - 1));
            const int idx = start + (i % pitch);
            concealed[i] = clampToInt16((float)history[idx] * attenuation * intraFrameFade);
        }

        std::memcpy(out, concealed.data(), frameSize * sizeof(int16_t));
        std::memmove(history.data(), history.data() + frameSize,
                     (history.size() - frameSize) * sizeof(int16_t));
        std::memcpy(history.data() + history.size() - frameSize, concealed.data(), frameSize * sizeof(int16_t));
        attenuation = (std::max)(0.05f, attenuation * 0.82f);
    }

    RingBuffer::RingBuffer(size_t size) : mask(0), writePos(0), readPos(0) {
        if (size > 0) resize(size);
    }

    void RingBuffer::resize(size_t size) {
        size_t pot = 1;
        while (pot < size) pot <<= 1;
        buffer.resize(pot, 0.0f);
        mask = pot - 1;
        reset();
    }

    void RingBuffer::reset() {
        writePos = 0;
        readPos = 0;
        std::fill(buffer.begin(), buffer.end(), 0.0f);
    }

    size_t RingBuffer::getReadAvailable() const { return writePos - readPos; }
    size_t RingBuffer::getWriteAvailable() const { return buffer.size() - (writePos - readPos); }

    size_t RingBuffer::write(const float* data, size_t count) {
        size_t available = getWriteAvailable();
        if (count > available) count = available;
        size_t off = writePos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(buffer.data() + off, data, n1 * sizeof(float));
        if (n1 < count) std::memcpy(buffer.data(), data + n1, (count - n1) * sizeof(float));
        writePos += count;
        return count;
    }

    size_t RingBuffer::read(float* data, size_t count) {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        size_t off = readPos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(data, buffer.data() + off, n1 * sizeof(float));
        if (n1 < count) std::memcpy(data + n1, buffer.data(), (count - n1) * sizeof(float));
        readPos += count;
        return count;
    }

    size_t RingBuffer::peek(float* data, size_t count) const {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        size_t off = readPos & mask;
        size_t n1 = (std::min)(count, buffer.size() - off);
        std::memcpy(data, buffer.data() + off, n1 * sizeof(float));
        if (n1 < count) std::memcpy(data + n1, buffer.data(), (count - n1) * sizeof(float));
        return count;
    }

    void RingBuffer::skip(size_t count) {
        size_t available = getReadAvailable();
        if (count > available) count = available;
        readPos += count;
    }

    G711Codec::G711Codec(int sr) : sampleRate(sr) {
        plc.reset(getFrameSize());
    }
    void G711Codec::reset() {
        plc.reset(getFrameSize());
    }
    void G711Codec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();
        if (packetLost) {
            plc.conceal(out, fs);
            return;
        }

        for (int i = 0; i < fs; ++i) {
            unsigned char u = linear2ulaw(in[i]);
            out[i] = (int16_t)ulaw2linear(u);
        }
        plc.storeGoodFrame(out, fs);
    }

    GSMCodec::GSMCodec() { gsmState = gsm_create(); plc.reset(getFrameSize()); }
    GSMCodec::~GSMCodec() { if (gsmState) gsm_destroy((gsm)gsmState); }
    void GSMCodec::reset() { if (gsmState) gsm_destroy((gsm)gsmState); gsmState = gsm_create(); plc.reset(getFrameSize()); }
    void GSMCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            plc.conceal(out, getFrameSize());
            return;
        }

        gsm_byte frame_encoded[33];
        gsm_encode((gsm)gsmState, (gsm_signal*)in, frame_encoded);
        gsm_decode((gsm)gsmState, frame_encoded, (gsm_signal*)out);
        plc.storeGoodFrame(out, getFrameSize());
    }

#ifndef TELEPHONY_DISTRIBUTION_BUILD
    AMRNBCodec::AMRNBCodec() : lastSerial(32, 0) {
        encState = Encoder_Interface_init(0);
        decState = Decoder_Interface_init();
        fallbackPLC.reset(getFrameSize());
    }
    AMRNBCodec::~AMRNBCodec() { Encoder_Interface_exit(encState); Decoder_Interface_exit(decState); }
    void AMRNBCodec::reset() {
        Encoder_Interface_exit(encState);
        Decoder_Interface_exit(decState);
        encState = Encoder_Interface_init(0);
        decState = Decoder_Interface_init();
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRNBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            Decoder_Interface_Decode(decState, lastSerial.data(), (short*)out, 1);
            fallbackPLC.storeGoodFrame(out, getFrameSize());
            return;
        }

        Encoder_Interface_Encode(encState, MR122, (short*)in, lastSerial.data(), 0);
        Decoder_Interface_Decode(decState, lastSerial.data(), (short*)out, 0);
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }

    AMRWBCodec::AMRWBCodec() : lastSerial(64, 0) {
        encState = E_IF_init();
        decState = D_IF_init();
        fallbackPLC.reset(getFrameSize());
    }
    AMRWBCodec::~AMRWBCodec() { E_IF_exit(encState); D_IF_exit(decState); }
    void AMRWBCodec::reset() {
        E_IF_exit(encState);
        D_IF_exit(decState);
        encState = E_IF_init();
        decState = D_IF_init();
        std::fill(lastSerial.begin(), lastSerial.end(), 0);
        fallbackPLC.reset(getFrameSize());
    }
    void AMRWBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            D_IF_decode(decState, lastSerial.data(), out, 1);
            fallbackPLC.storeGoodFrame(out, getFrameSize());
            return;
        }

        E_IF_encode(encState, 2, in, lastSerial.data(), 0); // Mode 2: 12.65 kbit/s
        D_IF_decode(decState, lastSerial.data(), out, 0);
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }
#else
    AMRNBCodec::AMRNBCodec() : encState(nullptr), decState(nullptr), lastSerial(32, 0) {
        fallbackPLC.reset(getFrameSize());
    }
    AMRNBCodec::~AMRNBCodec() {}
    void AMRNBCodec::reset() { fallbackPLC.reset(getFrameSize()); }
    void AMRNBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            fallbackPLC.conceal(out, getFrameSize());
            return;
        }
        std::memcpy(out, in, 160 * sizeof(int16_t));
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }

    AMRWBCodec::AMRWBCodec() : encState(nullptr), decState(nullptr), lastSerial(64, 0) {
        fallbackPLC.reset(getFrameSize());
    }
    AMRWBCodec::~AMRWBCodec() {}
    void AMRWBCodec::reset() { fallbackPLC.reset(getFrameSize()); }
    void AMRWBCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        if (packetLost) {
            fallbackPLC.conceal(out, getFrameSize());
            return;
        }
        std::memcpy(out, in, 320 * sizeof(int16_t));
        fallbackPLC.storeGoodFrame(out, getFrameSize());
    }
#endif

    // ---------------------------------------------------------------------------
    // EVSCodec
    // ---------------------------------------------------------------------------
    EVSCodec::EVSCodec(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw)
        : sampleRate(sampleRate)
        , bitrateBps(bitrateBps)
        , maxBw(maxBw)
        , enc(nullptr)
        , dec(nullptr)
    {
        fallbackPLC.reset(getFrameSize());
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        enc = evs_enc_create(sampleRate, bitrateBps, maxBw);
        dec = evs_dec_create(sampleRate, bitrateBps);
        bitstream.resize(evs_max_bitstream_bytes(sampleRate));
#endif
    }

    EVSCodec::~EVSCodec() {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        if (enc) evs_enc_destroy(enc);
        if (dec) evs_dec_destroy(dec);
#endif
    }

    void EVSCodec::reset() {
        fallbackPLC.reset(getFrameSize());
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        if (enc) { evs_enc_destroy(enc); enc = evs_enc_create(sampleRate, bitrateBps, maxBw); }
        if (dec) { evs_dec_destroy(dec); dec = evs_dec_create(sampleRate, bitrateBps); }
#endif
    }

    void EVSCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();
        if (packetLost) {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
            int n = 0;
            if (dec && evs_dec_process_lost(dec, out, &n) == EVS_OK && n == fs) {
                fallbackPLC.storeGoodFrame(out, fs);
                return;
            }
#endif
            fallbackPLC.conceal(out, fs);
            return;
        }

        if (!enc || !dec) {
            std::memcpy(out, in, fs * sizeof(int16_t));
            fallbackPLC.storeGoodFrame(out, fs);
            return;
        }
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        int used = 0;
        if (evs_enc_process(enc, in, fs, bitstream.data(), (int)bitstream.size(), &used) != EVS_OK) {
            // On encode failure, pass input through unchanged
            std::memcpy(out, in, fs * sizeof(int16_t));
            fallbackPLC.storeGoodFrame(out, fs);
            return;
        }
        int n = 0;
        if (evs_dec_process(dec, bitstream.data(), used, out, &n) != EVS_OK) {
            std::memcpy(out, in, fs * sizeof(int16_t));
        }
        fallbackPLC.storeGoodFrame(out, fs);
#endif
    }

    ChannelProcessor::ChannelProcessor(double hostSR)
        : hostSampleRate(hostSR), currentMode(EraMode::Bypass),
          paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
          paramPacketLossRate(0.0f), paramNetworkDegradation(0.0f),
          packetLossSeed(0x12345678u), packetLossBurstFrames(0),
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB)
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
        if (changed && currentMode == EraMode::EVS_NATIVE) {
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
        updateFilters();
    }

    size_t ChannelProcessor::getAvailableOutput() const {
        return outputBuffer.getReadAvailable();
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
        int targetSR = 0;
        switch (currentMode) {
            case EraMode::PSTN_G711:
            case EraMode::GSM_FR:
            case EraMode::AMR_NB_3G: targetSR = 8000; break;
            case EraMode::AMR_WB_VOLTE: targetSR = 16000; break;
            case EraMode::EVS_LIKE: targetSR = 32000; break;
            case EraMode::EVS_NATIVE: targetSR = evsSampleRate; break;
            default: targetSR = 0; break;
        }

        if (targetSR > 0) {
            resamplerDown = std::make_unique<r8b::CDSPResampler24>(hostSampleRate, targetSR, 1024);
            resamplerUp = std::make_unique<r8b::CDSPResampler24>(targetSR, hostSampleRate, 1024);
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
            default: break;
        }
        if (codec) {
            int fs = codec->getFrameSize();
            codecFrameF.resize(fs); codecFrameSIn.resize(fs); codecFrameSOut.resize(fs);
            simulatedPathPLC.reset(fs);
        }
    }

    void ChannelProcessor::prepareInternalBuffers(int maxBlockSize) {
        resampInBuf.resize(maxBlockSize);
        tempProcessBuf.resize(maxBlockSize);
    }

    // Process logic: Downsample -> Filter -> Codec -> Buffer (Wait for Upsample?)
    // Note: r8brain is streaming. Filters are streaming. Codec is block or sample based.
    // 1. Downsample (Host SR -> Codec SR)
    // 2. Filter (at Codec SR)
    // 3. Codec (at Codec SR)
    // 4. Upsample (Codec SR -> Host SR) -> outputBuffer
    void ChannelProcessor::processCodec(int numSamples, const float* in) {
         if (resampInBuf.size() < (size_t)numSamples) resampInBuf.resize(numSamples);
         for(int i=0; i<numSamples; ++i) resampInBuf[i] = in[i];
         double* dOutRaw;
         int downCount = resamplerDown->process(resampInBuf.data(), numSamples, dOutRaw);
         
         if (tempProcessBuf.size() < (size_t)downCount) tempProcessBuf.resize(downCount);
         
         // Apply Filters (in place or copy)
         for(int i=0; i<downCount; ++i) {
             float x = (float)dOutRaw[i];
             x = hpFilter1.process(x);
             x = hpFilter2.process(x);
             x = lpFilter1.process(x);
             x = lpFilter2.process(x);
             tempProcessBuf[i] = x;
         }
         
         ringCodecIn.write(tempProcessBuf.data(), downCount);

         if (currentMode == EraMode::EVS_LIKE) {
             const int frameSize = 32000 / 50;
             while (ringCodecIn.getReadAvailable() >= (size_t)frameSize) {
                 ringCodecIn.read(codecFrameF.data(), frameSize);
                 for (int i = 0; i < frameSize; ++i) {
                     codecFrameSIn[i] = clampToInt16(codecFrameF[i] * 32767.0f);
                 }

                 if (shouldDropPacket()) {
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
         } else {
             // Actual Codec
             int frameSize = codec->getFrameSize();
             while (ringCodecIn.getReadAvailable() >= (size_t)frameSize) {
                 ringCodecIn.read(codecFrameF.data(), frameSize);
                 for(int i=0; i<frameSize; ++i) codecFrameSIn[i] = clampToInt16(codecFrameF[i] * 32767.0f);
                 codec->processFrame(codecFrameSIn.data(), codecFrameSOut.data(), shouldDropPacket());
                 for(int i=0; i<frameSize; ++i) codecFrameF[i] = codecFrameSOut[i] / 32768.0f;
                 ringCodecOut.write(codecFrameF.data(), frameSize);
             }
         }

         int codecOutAvail = (int)ringCodecOut.getReadAvailable();
         if (codecOutAvail > 0) {
             if (tempProcessBuf.size() < (size_t)codecOutAvail) tempProcessBuf.resize(codecOutAvail);
             ringCodecOut.read(tempProcessBuf.data(), codecOutAvail);
             
             if (resampInBuf.size() < (size_t)codecOutAvail) resampInBuf.resize(codecOutAvail);
             for(int i=0; i<codecOutAvail; ++i) resampInBuf[i] = tempProcessBuf[i];
             
             double* dUpOutRaw;
             int upCount = resamplerUp->process(resampInBuf.data(), codecOutAvail, dUpOutRaw);
             
             if (tempProcessBuf.size() < (size_t)upCount) tempProcessBuf.resize(upCount);
             for(int i=0; i<upCount; ++i) tempProcessBuf[i] = (float)dUpOutRaw[i];
             outputBuffer.write(tempProcessBuf.data(), upCount);
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

    SignalProcessor::SignalProcessor()
        : hostSampleRate(44100.0), currentMode(EraMode::Bypass),
          routeModelEnabled(false),
          inputEndpoint(RouteEndpoint::FixedLine), outputEndpoint(RouteEndpoint::Mobile5G),
          degradationSegment(DegradationSegment::Both),
          paramDryWet(1.0f), paramOutGain(1.0f), paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
          paramPacketLossRate(0.0f), paramNetworkDegradation(0.0f),
          simulateLatency(true),
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB),
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
        if (simulateLatency) targetLatencySamples = (int)std::round(0.1 * hostSampleRate);
        else targetLatencySamples = 0;
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

    void SignalProcessor::process(float** inputs, int numIns, float** outputs, int numOuts, int numSamples) {
        ensureChannels(numIns);
        inTotalSamples += numSamples;

        for (int i=0; i<numIns; ++i) {
            dryBuffers[i]->write(inputs[i], numSamples);
            if (routeModelEnabled) {
                std::vector<float> exchange(numSamples, 0.0f);
                inputLegs[i]->pushInput(inputs[i], numSamples);
                inputLegs[i]->pullOutput(exchange.data(), numSamples);
                outputLegs[i]->pushInput(exchange.data(), numSamples);
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
            std::vector<std::vector<float>> processedChannels(numIns, std::vector<float>(samplesToWrite));
            
            for (int i=0; i<numIns; ++i) {
                std::vector<float> d(samplesToWrite);
                if (dryBuffers[i]->getReadAvailable() >= (size_t)samplesToWrite) {
                    dryBuffers[i]->read(d.data(), samplesToWrite);
                } else {
                    dryBuffers[i]->read(d.data(), dryBuffers[i]->getReadAvailable());
                }

                std::vector<float> w(samplesToWrite);
                outputLegs[i]->pullOutput(w.data(), samplesToWrite);
                
                for (int s=0; s<samplesToWrite; ++s) {
                    processedChannels[i][s] = (d[s] * (1.0f - paramDryWet) + w[s] * paramDryWet) * paramOutGain;
                }
            }

            for (int ch=0; ch<numOuts; ++ch) {
                int inCh = (ch < numIns) ? ch : 0;
                float* dest = outputs[ch] + outputOffset;
                std::memcpy(dest, processedChannels[inCh].data(), samplesToWrite * sizeof(float));
            }
            
            outTotalSamples += samplesToWrite;
        }
    }
}
