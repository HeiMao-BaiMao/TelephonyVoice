#define NOMINMAX
#include "TelephonyDSP.h"
#include <cstring>
#include <algorithm>
#include <cmath>

extern "C" {
#include "gsm.h"
}
#include "interf_enc.h" // AMR-NB
#include "interf_dec.h"
#include "enc_if.h" // AMR-WB (vo-amrwbenc)
#include "dec_if.h" // AMR-WB (opencore-amrwb)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace TelephonyDSP {

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

    G711Codec::G711Codec(int sr) : sampleRate(sr) {}
    void G711Codec::reset() {}
    void G711Codec::processFrame(const int16_t* in, int16_t* out) {
        unsigned char u = linear2ulaw(*in);
        *out = (int16_t)ulaw2linear(u);
    }

    GSMCodec::GSMCodec() { gsmState = gsm_create(); }
    GSMCodec::~GSMCodec() { if (gsmState) gsm_destroy((gsm)gsmState); }
    void GSMCodec::reset() { if (gsmState) gsm_destroy((gsm)gsmState); gsmState = gsm_create(); }
    void GSMCodec::processFrame(const int16_t* in, int16_t* out) {
        gsm_byte frame_encoded[33];
        gsm_encode((gsm)gsmState, (gsm_signal*)in, frame_encoded);
        gsm_decode((gsm)gsmState, frame_encoded, (gsm_signal*)out);
    }

    AMRNBCodec::AMRNBCodec() { encState = Encoder_Interface_init(0); decState = Decoder_Interface_init(); }
    AMRNBCodec::~AMRNBCodec() { Encoder_Interface_exit(encState); Decoder_Interface_exit(decState); }
    void AMRNBCodec::reset() { Encoder_Interface_exit(encState); Decoder_Interface_exit(decState); encState = Encoder_Interface_init(0); decState = Decoder_Interface_init(); }
    void AMRNBCodec::processFrame(const int16_t* in, int16_t* out) {
        unsigned char serial[32];
        Encoder_Interface_Encode(encState, MR122, (short*)in, serial, 0);
        Decoder_Interface_Decode(decState, serial, (short*)out, 0);
    }

    AMRWBCodec::AMRWBCodec() { encState = E_IF_init(); decState = D_IF_init(); }
    AMRWBCodec::~AMRWBCodec() { E_IF_exit(encState); D_IF_exit(decState); }
    void AMRWBCodec::reset() { E_IF_exit(encState); D_IF_exit(decState); encState = E_IF_init(); decState = D_IF_init(); }
    void AMRWBCodec::processFrame(const int16_t* in, int16_t* out) {
        unsigned char serial[64];
        E_IF_encode(encState, 2, in, serial, 0); // Mode 2: 12.65 kbit/s
        D_IF_decode(decState, serial, out, 0);
    }

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
        enc = evs_enc_create(sampleRate, bitrateBps, maxBw);
        dec = evs_dec_create(sampleRate, bitrateBps);
        bitstream.resize(evs_max_bitstream_bytes(sampleRate));
    }

    EVSCodec::~EVSCodec() {
        if (enc) evs_enc_destroy(enc);
        if (dec) evs_dec_destroy(dec);
    }

    void EVSCodec::reset() {
        if (enc) { evs_enc_destroy(enc); enc = evs_enc_create(sampleRate, bitrateBps, maxBw); }
        if (dec) { evs_dec_destroy(dec); dec = evs_dec_create(sampleRate, bitrateBps); }
    }

    void EVSCodec::processFrame(const int16_t* in, int16_t* out) {
        if (!enc || !dec) return;
        int used = 0;
        if (evs_enc_process(enc, in, sampleRate / 50, bitstream.data(), (int)bitstream.size(), &used) != EVS_OK) {
            // On encode failure, pass input through unchanged
            std::memcpy(out, in, (sampleRate / 50) * sizeof(int16_t));
            return;
        }
        int n = 0;
        if (evs_dec_process(dec, bitstream.data(), used, out, &n) != EVS_OK) {
            std::memcpy(out, in, (sampleRate / 50) * sizeof(int16_t));
        }
    }

    ChannelProcessor::ChannelProcessor(double hostSR)
        : hostSampleRate(hostSR), currentMode(EraMode::Bypass),
          paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
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

    void ChannelProcessor::configure(bool artifacts, float amount) {
        paramArtifactsEnabled = artifacts;
        paramArtifactAmount = amount;
    }

    void ChannelProcessor::reset() {
        ringCodecIn.reset(); ringCodecOut.reset(); outputBuffer.reset();
        hpFilter1.reset(); hpFilter2.reset(); 
        lpFilter1.reset(); lpFilter2.reset();
        if (resamplerDown) resamplerDown->clear();
        if (resamplerUp) resamplerUp->clear();
        if (codec) codec->reset();
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
        switch (currentMode) {
            case EraMode::PSTN_G711:
            case EraMode::GSM_FR:
                sampleRate = 8000.0f; // Filters apply at Codec SR
                hpFilter1.setHighpass(300.0f, sampleRate);
                lpFilter1.setLowpass(3400.0f, sampleRate);
                break;
            case EraMode::AMR_NB_3G:
                sampleRate = 8000.0f;
                hpFilter1.setHighpass(200.0f, sampleRate);
                lpFilter1.setLowpass(3400.0f, sampleRate);
                break;
            case EraMode::AMR_WB_VOLTE:
                sampleRate = 16000.0f;
                hpFilter1.setHighpass(50.0f, sampleRate);
                lpFilter1.setLowpass(7000.0f, sampleRate);
                break;
            case EraMode::EVS_LIKE:
                sampleRate = 32000.0f;
                hpFilter1.setHighpass(50.0f, sampleRate);
                lpFilter1.setLowpass(14000.0f, sampleRate);
                break;
            case EraMode::EVS_NATIVE:
                // The real EVS already shapes its own bandwidth; skip the extra
                // biquad cascade so the codec's own spectral envelope is not
                // double-filtered.
                hpFilter1.reset();
                lpFilter1.reset();
                break;
            default:
                break;
        }
        // Cascade setup: Both stages identical for 24dB/oct
        hpFilter2 = hpFilter1;
        lpFilter2 = lpFilter1;
    }

    void ChannelProcessor::recreateCodec() {
        codec.reset();
        switch (currentMode) {
            case EraMode::PSTN_G711: codec = std::make_unique<G711Codec>(8000); break;
            case EraMode::GSM_FR: codec = std::make_unique<GSMCodec>(); break;
            case EraMode::AMR_NB_3G: codec = std::make_unique<AMRNBCodec>(); break;
            case EraMode::AMR_WB_VOLTE: codec = std::make_unique<AMRWBCodec>(); break;
            case EraMode::EVS_NATIVE:
                codec = std::make_unique<EVSCodec>(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
                break;
            default: break;
        }
        if (codec) {
            int fs = codec->getFrameSize();
            codecFrameF.resize(fs); codecFrameSIn.resize(fs); codecFrameSOut.resize(fs);
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
             // EVS Simulation: Just pass through (already filtered and resampled)
             // Or add dynamics? Spec says "Dynamics / Noise".
             // We'll treat it as "Codec" that passes through 1:1 samples
             int avail = (int)ringCodecIn.getReadAvailable();
             // Just move everything to Out
             if (avail > 0) {
                 if (tempProcessBuf.size() < (size_t)avail) tempProcessBuf.resize(avail);
                 ringCodecIn.read(tempProcessBuf.data(), avail);
                 ringCodecOut.write(tempProcessBuf.data(), avail);
             }
         } else {
             // Actual Codec
             int frameSize = codec->getFrameSize();
             while (ringCodecIn.getReadAvailable() >= (size_t)frameSize) {
                 ringCodecIn.read(codecFrameF.data(), frameSize);
                 for(int i=0; i<frameSize; ++i) codecFrameSIn[i] = (int16_t)std::clamp(codecFrameF[i] * 32767.0f, -32768.0f, 32767.0f);
                 codec->processFrame(codecFrameSIn.data(), codecFrameSOut.data());
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
          paramDryWet(1.0f), paramOutGain(1.0f), paramArtifactsEnabled(false), paramArtifactAmount(0.0f),
          simulateLatency(true),
          evsSampleRate(32000), evsBitrateBps(EVS_BR_13200), evsMaxBandwidth(EVS_SWB),
          targetLatencySamples(0), inTotalSamples(0), outTotalSamples(0)
    {
        updateLatency();
    }

    SignalProcessor::~SignalProcessor() {}

    void SignalProcessor::setSampleRate(double sr) {
        hostSampleRate = sr;
        updateLatency();
        for (auto& ch : channels) ch->setSampleRate(sr);
        reset();
    }

    void SignalProcessor::setMode(EraMode mode) {
        currentMode = mode;
        for (auto& ch : channels) {
            ch->setMode(mode);
            ch->setEVSConfig(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
        }
    }

    void SignalProcessor::setEVSConfig(int sampleRateHz, int bitrateBps, EVS_Bandwidth maxBw) {
        evsSampleRate   = sampleRateHz;
        evsBitrateBps   = bitrateBps;
        evsMaxBandwidth = maxBw;
        for (auto& ch : channels) ch->setEVSConfig(sampleRateHz, bitrateBps, maxBw);
    }

    void SignalProcessor::setParameters(float dryWet, float outGaindB, bool artifacts, float artifactAmount) {
        paramDryWet = dryWet;
        paramOutGain = std::pow(10.0f, outGaindB / 20.0f);
        paramArtifactsEnabled = artifacts;
        paramArtifactAmount = artifactAmount;
        for (auto& ch : channels) ch->configure(artifacts, artifactAmount);
    }

    void SignalProcessor::setSimulateLatency(bool enable) {
        simulateLatency = enable;
        updateLatency();
    }

    void SignalProcessor::reset() {
        inTotalSamples = 0;
        outTotalSamples = 0;
        for (auto& ch : channels) ch->reset();
        for (auto& db : dryBuffers) db->reset();
    }

    void SignalProcessor::updateLatency() {
        if (simulateLatency) targetLatencySamples = (int)std::round(0.1 * hostSampleRate);
        else targetLatencySamples = 0;
    }

    int SignalProcessor::getLatencySamples() const {
        return targetLatencySamples;
    }

    void SignalProcessor::ensureChannels(int count) {
        if (channels.size() != (size_t)count) {
            channels.clear();
            dryBuffers.clear();
            for (int i=0; i<count; ++i) {
                auto ch = std::make_unique<ChannelProcessor>(hostSampleRate);
                ch->setMode(currentMode);
                ch->setEVSConfig(evsSampleRate, evsBitrateBps, evsMaxBandwidth);
                ch->configure(paramArtifactsEnabled, paramArtifactAmount);
                channels.push_back(std::move(ch));
                dryBuffers.push_back(std::make_unique<RingBuffer>(131072));
            }
        }
    }

    void SignalProcessor::process(float** inputs, int numIns, float** outputs, int numOuts, int numSamples) {
        ensureChannels(numIns);
        inTotalSamples += numSamples;

        for (int i=0; i<numIns; ++i) {
            dryBuffers[i]->write(inputs[i], numSamples);
            channels[i]->pushInput(inputs[i], numSamples);
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
                channels[i]->pullOutput(w.data(), samplesToWrite);
                
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
