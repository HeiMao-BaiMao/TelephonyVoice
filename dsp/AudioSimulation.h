#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace TelephonyDSP {

// These are deterministic local audio models, not radio/network conformance
// simulators. All PCM APIs use normalized floating point unless named PCM16.
// configure() validates/clamps settings and resets stream state. No processing
// method allocates. reset() preserves settings and restores the seeded stream.

class PsdComfortNoise {
public:
    static constexpr std::size_t fftSize = 256;
    static constexpr std::size_t filterTaps = 129;
    explicit PsdComfortNoise(double sampleRate = 8000.0);
    void configure(double sampleRate, uint32_t seed = 0xC001CAFEu);
    void reset();
    void observe(const float* samples, std::size_t count);
    void observePCM16(const int16_t* samples, std::size_t count);
    float next();
    void generate(float* samples, std::size_t count);
    void generatePCM16(int16_t* samples, std::size_t count);
    double targetRms() const { return rms_; }
    const std::array<double, fftSize / 2 + 1>& powerSpectrum() const { return psd_; }
private:
    void observeSample(double value);
    void analyse();
    double rate_ = 8000.0, rms_ = 0.0;
    uint32_t seed_ = 0xC001CAFEu, rng_ = seed_;
    std::size_t inputWrite_ = 0, inputCount_ = 0, hop_ = 0, noiseWrite_ = 0;
    bool haveSpectrum_ = false;
    std::array<double, fftSize> input_{};
    std::array<double, fftSize / 2 + 1> psd_{};
    std::array<double, filterTaps> taps_{}, noise_{};
};

class DtmfGenerator {
public:
    void configure(double sampleRate);
    // Q.23 frequency pairs. Timing/envelope are test-signal settings, not a
    // Q.24 receiver-conformance test. Invalid sequence characters are ignored.
    void setSequence(const std::string& digits, double toneMs = 75.0,
                     double gapMs = 50.0, double levelDb = -12.0, bool repeat = false);
    void reset();
    float next();
    void generate(float* out, std::size_t count);
    bool finished() const;
    static bool frequencies(char digit, double& low, double& high);
private:
    double rate_ = 8000.0, level_ = 0.2511886432;
    std::string digits_;
    std::size_t digit_ = 0;
    uint64_t position_ = 0, toneSamples_ = 600, gapSamples_ = 400, rampSamples_ = 16;
    bool repeat_ = false;
};

class ClockDriftResampler {
public:
    void configure(double sampleRate, double partsPerMillion = 0.0,
                   double bufferMs = 2.0);
    void setPpm(double partsPerMillion);
    void reset();
    float process(float input);
    void process(float* samples, std::size_t count);
    std::size_t latencySamples() const { return latency_; }
    uint64_t underruns() const { return underruns_; }
    uint64_t overruns() const { return overruns_; }
    double effectiveRatio() const { return ratio_; }
    double queuedSamples() const;
private:
    std::vector<float> fifo_;
    std::size_t latency_ = 16;
    uint64_t read_ = 0, write_ = 0, underruns_ = 0, overruns_ = 0;
    double fraction_ = 0.0, ratio_ = 1.0;
};

class HybridEcho {
public:
    void configure(double sampleRate, double delayMs = 40.0,
                   double returnGainDb = -18.0, double lowPassHz = 3400.0,
                   double highPassHz = 150.0);
    void reset();
    // Call inject then capture once per sample to feed actual codec output
    // back to the input. process() uses its own output for standalone testing.
    float inject(float dryInput) const;
    void capture(float processedOutput);
    float process(float dryInput);
    void process(float* samples, std::size_t count);
    std::size_t delaySamples() const { return delay_.size(); }
    double returnGain() const { return gain_; }
private:
    std::vector<float> delay_;
    std::size_t cursor_ = 0;
    double gain_ = 0.1258925412, lowAlpha_ = 0.1, highAlpha_ = 0.99;
    double low1_ = 0.0, low2_ = 0.0, high_ = 0.0, highPrevious_ = 0.0;
};

struct FadingState {
    double envelope = 1.0;
    double power = 1.0;
    double carrierToInterferenceDb = 30.0;
    // Fraction [0,1] of the caller's available codec mode range. This is a
    // tunable engineering mapping, not a 3GPP mandated mode-selection table.
    double suggestedModeFraction = 1.0;
};

class RayleighFading {
public:
    static constexpr std::size_t oscillators = 32;
    RayleighFading() { configure(8000.0); }
    void configure(double sampleRate, double dopplerHz = 5.0,
                   double meanCarrierToInterferenceDb = 20.0,
                   uint32_t seed = 0x52414449u);
    void reset();
    FadingState next();
    const FadingState& state() const { return state_; }
    // Apply bounded attenuation only. Complex flat fading is represented by
    // its envelope, never interpreted as a standards RF waveform/channel.
    float process(float input);
private:
    double rate_ = 8000.0, doppler_ = 5.0, meanCi_ = 20.0, ciSmoothed_ = 20.0;
    uint32_t seed_ = 0x52414449u;
    uint64_t sample_ = 0;
    std::array<double, oscillators> phaseI_{}, phaseQ_{}, cosineI_{}, cosineQ_{};
    std::array<double, oscillators> sineStep_{}, cosineStep_{};
    FadingState state_{};
};

class HandoverGate {
public:
    void configure(double sampleRate, double gapMs = 100.0,
                   double intervalSeconds = 0.0, double recoveryMs = 5.0);
    void reset();
    void trigger();
    float process(float input);
    void process(float* samples, std::size_t count);
    bool active() const { return remaining_ != 0; }
    uint64_t count() const { return count_; }
private:
    uint64_t gap_ = 800, recovery_ = 40, interval_ = 0;
    uint64_t sample_ = 0, remaining_ = 0, recovering_ = 0, count_ = 0;
};

struct Vad2Result {
    bool available = false;
    bool primarySpeech = false;
    bool speech = false;
    int hangoverFrames = 0; // reference VAD2 10 ms subframes
    int burstFrames = 0;
    uint64_t framesProcessed = 0;
};

// Actual OpenCORE/3GPP VAD2 spectral core, including its own adaptive noise,
// primary decision, burst smoothing and hangover. The standalone streaming
// front-end downsamples to 8 kHz and estimates an LTP flag; it is NOT claimed
// bit-exact to the complete AMR encoder front-end. Reference-core entry accepts
// exactly 80 preprocessed 8 kHz PCM samples and an encoder-supplied LTP flag.
// Build TELEPHONY_HAVE_VAD2=1 and link the three AudioSimulationVad2*.cpp
// adapter translation units through the opencore-amrnb target. Otherwise available()==false, with no fake VAD fallback.
class Vad2Detector {
public:
    Vad2Detector();
    ~Vad2Detector();
    Vad2Detector(const Vad2Detector&) = delete;
    Vad2Detector& operator=(const Vad2Detector&) = delete;
    void configure(double sampleRate);
    void reset();
    bool available() const;
    Vad2Result process(const float* samples, std::size_t count);
    Vad2Result processPCM16(const int16_t* samples, std::size_t count);
    Vad2Result processReferenceFrame(const int16_t* preprocessed80, bool ltpFlag);
    const Vad2Result& result() const { return result_; }
private:
    void consume(double input);
    void consume8k(double input);
    void* state_ = nullptr;
    double rate_ = 8000.0, resamplePhase_ = 0.0;
    double lowAlpha_ = 1.0, low1_ = 0.0, low2_ = 0.0, low3_ = 0.0, low4_ = 0.0;
    double high_ = 0.0, highPrevious_ = 0.0;
    std::size_t fill_ = 0, historyWrite_ = 0;
    std::array<int16_t, 80> frame_{};
    std::array<double, 240> pitchHistory_{};
    Vad2Result result_{};
};

} // namespace TelephonyDSP
