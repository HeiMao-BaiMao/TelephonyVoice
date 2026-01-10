#pragma once

#include <vector>
#include <cstdint>
#include <cmath>
#include <memory>
#include <atomic>
#include <algorithm>

// External Libraries
#include "CDSPResampler.h"

extern "C" {
    unsigned char linear2ulaw(int pcm_val);
    int ulaw2linear(unsigned char u_val);
    unsigned char linear2alaw(int pcm_val);
    int alaw2linear(unsigned char a_val);
}

namespace TelephonyDSP {

    enum class EraMode {
        PSTN_G711 = 0,
        GSM_FR,
        AMR_NB_3G,
        AMR_WB_VOLTE,
        EVS_LIKE,
        Bypass
    };

    static const int LATENCY_MS = 100;

    // Simple Biquad Filter (Direct Form I)
    struct Biquad {
        float b0, b1, b2, a1, a2;
        float x1, x2, y1, y2;

        Biquad() { reset(); }
        void reset() {
            b0 = b1 = b2 = a1 = a2 = 0.0f;
            x1 = x2 = y1 = y2 = 0.0f;
        }
        
        // Calculate coefficients for Butterworth Highpass
        void setHighpass(float freq, float sampleRate) {
            float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
            float cosw0 = std::cos(w0);
            float alpha = std::sin(w0) / std::sqrt(2.0f);

            float norm = 1.0f / (1.0f + alpha);
            b0 = ((1.0f + cosw0) / 2.0f) * norm;
            b1 = -(1.0f + cosw0) * norm;
            b2 = ((1.0f + cosw0) / 2.0f) * norm;
            a1 = (-2.0f * cosw0) * norm;
            a2 = (1.0f - alpha) * norm;
        }

        // Calculate coefficients for Butterworth Lowpass
        void setLowpass(float freq, float sampleRate) {
            float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
            float cosw0 = std::cos(w0);
            float alpha = std::sin(w0) / std::sqrt(2.0f);

            float norm = 1.0f / (1.0f + alpha);
            b0 = ((1.0f - cosw0) / 2.0f) * norm;
            b1 = (1.0f - cosw0) * norm;
            b2 = ((1.0f - cosw0) / 2.0f) * norm;
            a1 = (-2.0f * cosw0) * norm;
            a2 = (1.0f - alpha) * norm;
        }

        inline float process(float in) {
            float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in;
            y2 = y1; y1 = out;
            return out;
        }
    };

    class RingBuffer {
    public:
        RingBuffer(size_t size = 0);
        void resize(size_t size);
        void reset();
        size_t getReadAvailable() const;
        size_t getWriteAvailable() const;
        size_t write(const float* data, size_t count);
        size_t read(float* data, size_t count);
        size_t peek(float* data, size_t count) const;
        void skip(size_t count);

    private:
        std::vector<float> buffer;
        size_t mask;
        std::atomic<size_t> writePos;
        std::atomic<size_t> readPos;
    };

    class ICodec {
    public:
        virtual ~ICodec() = default;
        virtual void reset() = 0;
        virtual int getSampleRate() const = 0;
        virtual int getFrameSize() const = 0;
        virtual void processFrame(const int16_t* in, int16_t* out) = 0;
    };

    class G711Codec : public ICodec {
    public:
        G711Codec(int sampleRate);
        void reset() override;
        int getSampleRate() const override { return sampleRate; }
        int getFrameSize() const override { return 1; }
        void processFrame(const int16_t* in, int16_t* out) override;
    private:
        int sampleRate;
    };

    class GSMCodec : public ICodec {
    public:
        GSMCodec();
        ~GSMCodec();
        void reset() override;
        int getSampleRate() const override { return 8000; }
        int getFrameSize() const override { return 160; }
        void processFrame(const int16_t* in, int16_t* out) override;
    private:
        void* gsmState;
    };

    class AMRNBCodec : public ICodec {
    public:
        AMRNBCodec();
        ~AMRNBCodec();
        void reset() override;
        int getSampleRate() const override { return 8000; }
        int getFrameSize() const override { return 160; }
        void processFrame(const int16_t* in, int16_t* out) override;
    private:
        void* encState;
        void* decState;
    };

    class AMRWBCodec : public ICodec {
    public:
        AMRWBCodec();
        ~AMRWBCodec();
        void reset() override;
        int getSampleRate() const override { return 16000; }
        int getFrameSize() const override { return 320; }
        void processFrame(const int16_t* in, int16_t* out) override;
    private:
        void* encState;
        void* decState;
    };

    class ChannelProcessor {
    public:
        ChannelProcessor(double hostSR);
        ~ChannelProcessor();

        void setSampleRate(double sr);
        void setMode(EraMode mode);
        void reset();
        void pushInput(const float* in, int numSamples);
        size_t pullOutput(float* out, int numSamples);
        size_t getAvailableOutput() const;
        void configure(bool artifacts, float amount);

    private:
        double hostSampleRate;
        EraMode currentMode;
        bool paramArtifactsEnabled;
        float paramArtifactAmount;

        RingBuffer ringCodecIn;
        RingBuffer ringCodecOut;
        RingBuffer outputBuffer;

        std::unique_ptr<r8b::CDSPResampler24> resamplerDown;
        std::unique_ptr<r8b::CDSPResampler24> resamplerUp;
        std::unique_ptr<ICodec> codec;

        // Buffers
        std::vector<double> resampInBuf;
        std::vector<float> codecFrameF;
        std::vector<int16_t> codecFrameSIn;
        std::vector<int16_t> codecFrameSOut;
        std::vector<float> tempProcessBuf;

        // Filters for G.711 (Cascaded for 24dB/oct)
        Biquad hpFilter1, hpFilter2;
        Biquad lpFilter1, lpFilter2;

        void recreateResamplers();
        void recreateCodec();
        void prepareInternalBuffers(int maxBlockSize);
        void updateFilters();
        
        void processG711(int numSamples, const float* in, float* out);
        void processEVSLike(int numSamples, const float* in, float* out);
        void processCodec(int numSamples, const float* in);
        void applyArtifacts(float* buffer, int numSamples);
    };

    class SignalProcessor {
    public:
        SignalProcessor();
        ~SignalProcessor();

        void setSampleRate(double sampleRate);
        void setMode(EraMode mode);
        void setParameters(float dryWet, float outGaindB, bool artifacts, float artifactAmount);
        void setSimulateLatency(bool enable);
        void reset();
        void process(float** inputs, int numIns, float** outputs, int numOuts, int numSamples);
        int getLatencySamples() const;

    private:
        double hostSampleRate;
        EraMode currentMode;
        float paramDryWet;
        float paramOutGain;
        bool paramArtifactsEnabled;
        float paramArtifactAmount;
        bool simulateLatency;

        int targetLatencySamples;
        int64_t inTotalSamples;
        int64_t outTotalSamples;

        std::vector<std::unique_ptr<ChannelProcessor>> channels;
        std::vector<std::unique_ptr<RingBuffer>> dryBuffers;

        void updateLatency();
        void ensureChannels(int count);
    };

}