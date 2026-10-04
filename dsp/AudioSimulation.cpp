#include "dsp/AudioSimulation.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

#ifndef TELEPHONY_HAVE_VAD2
#define TELEPHONY_HAVE_VAD2 0
#endif

#if TELEPHONY_HAVE_VAD2
extern "C" {
void* telephony_vad2_create();
void telephony_vad2_destroy(void*);
void telephony_vad2_reset(void*);
int telephony_vad2_process(void*, const int16_t*, int, int*, int*);
}
#endif

namespace TelephonyDSP {
namespace {
constexpr double pi = 3.1415926535897932384626433832795;
double bounded(double x, double lo, double hi, double fallback) {
    return std::isfinite(x) ? std::clamp(x, lo, hi) : fallback;
}
double rate(double x) { return bounded(x, 8000.0, 384000.0, 8000.0); }
double pcm(double x) { return bounded(x, -1.0, 1.0, 0.0); }
int16_t pcm16(double x) {
    return static_cast<int16_t>(std::clamp(std::lround(pcm(x) * 32768.0), -32768L, 32767L));
}
uint32_t randomWord(uint32_t& state) {
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
double uniform(uint32_t& state) {
    return (static_cast<double>(randomWord(state)) + 0.5) / 4294967296.0;
}
void fft(std::array<std::complex<double>, PsdComfortNoise::fftSize>& a, bool inverse) {
    const auto n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const auto wlen = std::polar(1.0, (inverse ? 2.0 : -2.0) * pi / length);
        for (std::size_t i = 0; i < n; i += length) {
            std::complex<double> w = 1.0;
            for (std::size_t j = 0; j < length / 2; ++j) {
                const auto u = a[i + j], v = a[i + j + length / 2] * w;
                a[i + j] = u + v; a[i + j + length / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) for (auto& v : a) v /= static_cast<double>(n);
}
}

PsdComfortNoise::PsdComfortNoise(double sampleRate) { configure(sampleRate); }
void PsdComfortNoise::configure(double sampleRate, uint32_t seed) {
    rate_ = rate(sampleRate); seed_ = seed ? seed : 1u; reset();
}
void PsdComfortNoise::reset() {
    rng_ = seed_; inputWrite_ = inputCount_ = hop_ = noiseWrite_ = 0;
    haveSpectrum_ = false; rms_ = 0.0;
    input_.fill(0.0); psd_.fill(0.0); taps_.fill(0.0); noise_.fill(0.0);
    taps_[filterTaps / 2] = 1.0;
}
void PsdComfortNoise::observeSample(double value) {
    input_[inputWrite_] = pcm(value);
    inputWrite_ = (inputWrite_ + 1) % fftSize;
    inputCount_ = std::min(inputCount_ + 1, fftSize);
    if (++hop_ == fftSize / 2) {
        hop_ = 0;
        if (inputCount_ >= fftSize / 2) analyse();
    }
}
void PsdComfortNoise::observe(const float* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) observeSample(samples[i]);
}
void PsdComfortNoise::observePCM16(const int16_t* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) observeSample(samples[i] / 32768.0);
}
void PsdComfortNoise::analyse() {
    std::array<std::complex<double>, fftSize> spectrum{};
    double energy = 0.0, windowEnergy = 0.0;
    for (std::size_t i = 0; i < fftSize; ++i) {
        const double x = input_[(inputWrite_ + i) % fftSize];
        const double window = 0.5 - 0.5 * std::cos(2.0 * pi * i / (fftSize - 1));
        spectrum[i] = x * window;
        energy += x * x; windowEnergy += window * window;
    }
    fft(spectrum, false);
    const double a = haveSpectrum_ ? std::exp(-(fftSize / 2.0) / (rate_ * 0.15)) : 0.0;
    const double meanSquare = energy / inputCount_;
    rms_ = std::sqrt(a * rms_ * rms_ + (1.0 - a) * meanSquare);
    for (std::size_t k = 0; k <= fftSize / 2; ++k) {
        const double power = std::norm(spectrum[k]) / windowEnergy;
        psd_[k] = a * psd_[k] + (1.0 - a) * power;
        // Nonnegative magnitude from Welch-smoothed PSD, with conjugate
        // symmetry, produces a real symmetric finite impulse response.
        spectrum[k] = std::sqrt(std::max(0.0, psd_[k]));
        if (k && k < fftSize / 2) spectrum[fftSize - k] = spectrum[k];
    }
    fft(spectrum, true);
    double filterEnergy = 0.0;
    for (std::size_t i = 0; i < filterTaps; ++i) {
        const int centered = static_cast<int>(i) - static_cast<int>(filterTaps / 2);
        const std::size_t index = (centered + static_cast<int>(fftSize)) % fftSize;
        const double window = 0.42 - 0.5 * std::cos(2.0 * pi * i / (filterTaps - 1))
                                   + 0.08 * std::cos(4.0 * pi * i / (filterTaps - 1));
        taps_[i] = spectrum[index].real() * window;
        filterEnergy += taps_[i] * taps_[i];
    }
    if (filterEnergy > 1e-24) {
        const double scale = 1.0 / std::sqrt(filterEnergy);
        for (auto& tap : taps_) tap *= scale;
    } else {
        taps_.fill(0.0); taps_[filterTaps / 2] = 1.0;
    }
    haveSpectrum_ = true;
}
float PsdComfortNoise::next() {
    // Uniform innovation has unit variance; the FIR has unit energy.
    noise_[noiseWrite_] = (uniform(rng_) * 2.0 - 1.0) * std::sqrt(3.0);
    double sum = 0.0;
    std::size_t at = noiseWrite_;
    for (std::size_t k = 0; k < filterTaps; ++k) {
        sum += taps_[k] * noise_[at];
        at = at == 0 ? filterTaps - 1 : at - 1;
    }
    noiseWrite_ = (noiseWrite_ + 1) % filterTaps;
    return static_cast<float>(pcm(sum * rms_));
}
void PsdComfortNoise::generate(float* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) samples[i] = next();
}
void PsdComfortNoise::generatePCM16(int16_t* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) samples[i] = pcm16(next());
}

void DtmfGenerator::configure(double sampleRate) { rate_ = rate(sampleRate); reset(); }
bool DtmfGenerator::frequencies(char digit, double& low, double& high) {
    constexpr char keys[] = "123A456B789C*0#D";
    constexpr double lows[] = {697, 770, 852, 941}, highs[] = {1209, 1336, 1477, 1633};
    if (digit >= 'a' && digit <= 'd') digit -= 'a' - 'A';
    for (std::size_t i = 0; i < 16; ++i) if (keys[i] == digit) {
        low = lows[i / 4]; high = highs[i % 4]; return true;
    }
    low = high = 0.0; return false;
}
void DtmfGenerator::setSequence(const std::string& digits, double toneMs,
                              double gapMs, double levelDb, bool repeat) {
    digits_.clear();
    for (const char digit : digits) { double low, high; if (frequencies(digit, low, high)) digits_ += digit; }
    toneSamples_ = static_cast<uint64_t>(std::llround(rate_ * bounded(toneMs, 50.0, 1000.0, 75.0) / 1000.0));
    gapSamples_ = static_cast<uint64_t>(std::llround(rate_ * bounded(gapMs, 50.0, 1000.0, 50.0) / 1000.0));
    rampSamples_ = std::max<uint64_t>(1, static_cast<uint64_t>(rate_ * 0.002));
    level_ = levelDb <= -100.0 ? 0.0 : std::pow(10.0, bounded(levelDb, -80.0, 0.0, -12.0) / 20.0);
    repeat_ = repeat; reset();
}
void DtmfGenerator::reset() { position_ = 0; digit_ = 0; }
bool DtmfGenerator::finished() const { return digits_.empty() || digit_ >= digits_.size(); }
float DtmfGenerator::next() {
    if (finished()) return 0.0f;
    double out = 0.0;
    if (position_ < toneSamples_) {
        double low, high; frequencies(digits_[digit_], low, high);
        const double edge = static_cast<double>(std::min(position_, toneSamples_ - 1 - position_));
        const double env = edge < rampSamples_ ? 0.5 - 0.5 * std::cos(pi * edge / rampSamples_) : 1.0;
        const double t = static_cast<double>(position_) / rate_;
        out = 0.5 * level_ * env * (std::sin(2 * pi * low * t) + std::sin(2 * pi * high * t));
    }
    if (++position_ >= toneSamples_ + gapSamples_) {
        position_ = 0;
        if (++digit_ == digits_.size() && repeat_) digit_ = 0;
    }
    return static_cast<float>(out);
}
void DtmfGenerator::generate(float* out, std::size_t count) {
    if (out) for (std::size_t i = 0; i < count; ++i) out[i] = next();
}

void ClockDriftResampler::configure(double sampleRate, double ppm, double bufferMs) {
    latency_ = static_cast<std::size_t>(std::ceil(rate(sampleRate) * bounded(bufferMs, 0.25, 100.0, 2.0) / 1000.0));
    latency_ = std::max<std::size_t>(4, latency_);
    fifo_.assign(latency_ * 4 + 8, 0.0f);
    setPpm(ppm); reset();
}
void ClockDriftResampler::setPpm(double ppm) {
    ratio_ = 1.0 + bounded(ppm, -1000.0, 1000.0, 0.0) * 1e-6;
}
void ClockDriftResampler::reset() {
    std::fill(fifo_.begin(), fifo_.end(), 0.0f);
    read_ = 0; write_ = latency_; fraction_ = 0.0; underruns_ = overruns_ = 0;
}
double ClockDriftResampler::queuedSamples() const { return std::max(0.0, static_cast<double>(write_) - static_cast<double>(read_) - fraction_); }
float ClockDriftResampler::process(float input) {
    if (fifo_.empty()) return static_cast<float>(pcm(input));
    if (write_ - read_ >= fifo_.size() - 1) { ++read_; ++overruns_; }
    fifo_[write_ % fifo_.size()] = static_cast<float>(pcm(input)); ++write_;
    if (read_ + 1 >= write_) {
        // One-sample elastic-buffer expansion: bounded and observable. This
        // is linear interpolation/slip recovery, NOT the EVS reference APA.
        read_ = write_ - 2; ++underruns_;
    }
    const float a = fifo_[read_ % fifo_.size()], b = fifo_[(read_ + 1) % fifo_.size()];
    const float output = static_cast<float>(a + fraction_ * (b - a));
    fraction_ += ratio_;
    const auto whole = static_cast<uint64_t>(fraction_);
    read_ += whole; fraction_ -= static_cast<double>(whole);
    return output;
}
void ClockDriftResampler::process(float* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) samples[i] = process(samples[i]);
}

void HybridEcho::configure(double sampleRate, double delayMs, double returnGainDb,
                           double lowPassHz, double highPassHz) {
    const double sr = rate(sampleRate);
    delay_.assign(std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(sr * bounded(delayMs, 1.0, 500.0, 40.0) / 1000.0))), 0.0f);
    // HP impulse-response absolute sum is <=2, LP sums are 1. Keeping
    // 2*gain <1 proves BIBO stability of even the unsaturated feedback loop.
    gain_ = returnGainDb <= -100.0 ? 0.0 : std::min(0.45, std::pow(10.0, bounded(returnGainDb, -100.0, -6.94, -18.0) / 20.0));
    lowAlpha_ = 1.0 - std::exp(-2.0 * pi * bounded(lowPassHz, 100.0, sr * 0.45, 3400.0) / sr);
    highAlpha_ = std::exp(-2.0 * pi * bounded(highPassHz, 10.0, sr * 0.2, 150.0) / sr);
    reset();
}
void HybridEcho::reset() {
    std::fill(delay_.begin(), delay_.end(), 0.0f); cursor_ = 0;
    low1_ = low2_ = high_ = highPrevious_ = 0.0;
}
float HybridEcho::inject(float dryInput) const {
    const double echo = delay_.empty() ? 0.0 : delay_[cursor_] * gain_;
    return static_cast<float>(pcm(pcm(dryInput) + echo));
}
void HybridEcho::capture(float processedOutput) {
    if (delay_.empty()) return;
    low1_ += lowAlpha_ * (pcm(processedOutput) - low1_);
    low2_ += lowAlpha_ * (low1_ - low2_);
    high_ = highAlpha_ * (high_ + low2_ - highPrevious_); highPrevious_ = low2_;
    delay_[cursor_] = static_cast<float>(high_);
    cursor_ = (cursor_ + 1) % delay_.size();
}
float HybridEcho::process(float dryInput) { const float out = inject(dryInput); capture(out); return out; }
void HybridEcho::process(float* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) samples[i] = process(samples[i]);
}

void RayleighFading::configure(double sampleRate, double dopplerHz, double meanCiDb, uint32_t seed) {
    rate_ = rate(sampleRate); doppler_ = bounded(dopplerHz, 0.0, 500.0, 5.0);
    meanCi_ = bounded(meanCiDb, -40.0, 80.0, 20.0); seed_ = seed ? seed : 1u; reset();
}
void RayleighFading::reset() {
    uint32_t rng = seed_; sample_ = 0; ciSmoothed_ = meanCi_;
    for (std::size_t k = 0; k < oscillators; ++k) {
        const double i = 2.0 * pi * uniform(rng), q = 2.0 * pi * uniform(rng);
        phaseI_[k] = std::sin(i); cosineI_[k] = std::cos(i);
        phaseQ_[k] = std::sin(q); cosineQ_[k] = std::cos(q);
        // Midpoint angular quadrature of isotropically distributed arrival
        // angles approximates Clarke/Jakes J0 Doppler autocorrelation.
        const double angle = pi * (k + 0.5) / oscillators;
        const double omega = 2.0 * pi * doppler_ * std::cos(angle) / rate_;
        sineStep_[k] = std::sin(omega); cosineStep_[k] = std::cos(omega);
    }
    state_ = {}; state_.carrierToInterferenceDb = meanCi_;
}
FadingState RayleighFading::next() {
    double inPhase = 0.0, quadrature = 0.0;
    for (std::size_t k = 0; k < oscillators; ++k) {
        inPhase += phaseI_[k]; quadrature += phaseQ_[k];
        const double si = phaseI_[k], sq = phaseQ_[k];
        phaseI_[k] = si * cosineStep_[k] + cosineI_[k] * sineStep_[k];
        phaseQ_[k] = sq * cosineStep_[k] + cosineQ_[k] * sineStep_[k];
        cosineI_[k] = cosineI_[k] * cosineStep_[k] - si * sineStep_[k];
        cosineQ_[k] = cosineQ_[k] * cosineStep_[k] - sq * sineStep_[k];
        if ((sample_ & 4095u) == 4095u) {
            const double ri = std::hypot(phaseI_[k], cosineI_[k]), rq = std::hypot(phaseQ_[k], cosineQ_[k]);
            phaseI_[k] /= ri; cosineI_[k] /= ri; phaseQ_[k] /= rq; cosineQ_[k] /= rq;
        }
    }
    ++sample_;
    state_.power = (inPhase * inPhase + quadrature * quadrature) / oscillators;
    state_.envelope = std::sqrt(state_.power);
    const double ci = meanCi_ + 10.0 * std::log10(std::max(1e-12, state_.power));
    const double alpha = -std::expm1(-1.0 / (rate_ * 0.1));
    ciSmoothed_ += alpha * (ci - ciSmoothed_);
    state_.carrierToInterferenceDb = std::clamp(ciSmoothed_, -100.0, 100.0);
    // Deliberately explicit non-normative adaptation: -5 dB -> lowest mode,
    // 25 dB -> highest, smoothed over 100 ms to reduce mode chatter.
    state_.suggestedModeFraction = std::clamp((ciSmoothed_ + 5.0) / 30.0, 0.0, 1.0);
    return state_;
}
float RayleighFading::process(float input) {
    return static_cast<float>(pcm(input) * std::min(1.0, next().envelope));
}

void HandoverGate::configure(double sampleRate, double gapMs, double intervalSeconds, double recoveryMs) {
    const double sr = rate(sampleRate);
    gap_ = static_cast<uint64_t>(std::llround(sr * bounded(gapMs, 50.0, 200.0, 100.0) / 1000.0));
    recovery_ = static_cast<uint64_t>(std::llround(sr * bounded(recoveryMs, 0.0, 50.0, 5.0) / 1000.0));
    interval_ = static_cast<uint64_t>(std::llround(sr * bounded(intervalSeconds, 0.0, 3600.0, 0.0)));
    if (interval_) interval_ = std::max(interval_, gap_ + recovery_ + 1);
    reset();
}
void HandoverGate::reset() { sample_ = remaining_ = recovering_ = count_ = 0; }
void HandoverGate::trigger() {
    if (!remaining_) { remaining_ = gap_; recovering_ = recovery_; ++count_; }
}
float HandoverGate::process(float input) {
    if (interval_ && sample_ && sample_ % interval_ == 0) trigger();
    ++sample_;
    if (remaining_) { --remaining_; return 0.0f; }
    if (recovering_) {
        const double phase = static_cast<double>(recovery_ - recovering_ + 1) / recovery_;
        --recovering_;
        return static_cast<float>(pcm(input) * (0.5 - 0.5 * std::cos(pi * phase)));
    }
    return static_cast<float>(pcm(input));
}
void HandoverGate::process(float* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) samples[i] = process(samples[i]);
}

Vad2Detector::Vad2Detector() {
#if TELEPHONY_HAVE_VAD2
    state_ = telephony_vad2_create();
#endif
    configure(8000.0);
}
Vad2Detector::~Vad2Detector() {
#if TELEPHONY_HAVE_VAD2
    telephony_vad2_destroy(state_);
#endif
}
bool Vad2Detector::available() const { return state_ != nullptr; }
void Vad2Detector::configure(double sampleRate) {
    rate_ = rate(sampleRate);
    lowAlpha_ = 1.0 - std::exp(-2.0 * pi * 3000.0 / rate_);
    reset();
}
void Vad2Detector::reset() {
#if TELEPHONY_HAVE_VAD2
    telephony_vad2_reset(state_);
#endif
    resamplePhase_ = low1_ = low2_ = low3_ = low4_ = high_ = highPrevious_ = 0.0;
    fill_ = historyWrite_ = 0; frame_.fill(0); pitchHistory_.fill(0.0);
    result_ = {}; result_.available = available();
}
Vad2Result Vad2Detector::processReferenceFrame(const int16_t* preprocessed80, bool ltpFlag) {
#if TELEPHONY_HAVE_VAD2
    if (state_ && preprocessed80) {
        result_.speech = telephony_vad2_process(state_, preprocessed80, ltpFlag ? 1 : 0,
                                               &result_.hangoverFrames, &result_.burstFrames) != 0;
        result_.primarySpeech = result_.burstFrames > 0;
        ++result_.framesProcessed;
    }
#else
    (void)preprocessed80; (void)ltpFlag;
#endif
    return result_;
}
void Vad2Detector::consume8k(double input) {
    // Standalone front-end, separate from and not a replacement for AMR's
    // mandated encoder high-pass and open-loop predictor integration.
    const double highAlpha = std::exp(-2.0 * pi * 80.0 / 8000.0);
    high_ = highAlpha * (high_ + input - highPrevious_); highPrevious_ = input;
    const double value = pcm(high_);
    frame_[fill_++] = pcm16(value);
    pitchHistory_[historyWrite_] = value;
    historyWrite_ = (historyWrite_ + 1) % pitchHistory_.size();
    if (fill_ == frame_.size()) {
        double best = 0.0;
        for (std::size_t lag = 20; lag <= 147; ++lag) {
            double correlation = 0.0, e1 = 1e-12, e2 = 1e-12;
            for (std::size_t i = 0; i < 80; ++i) {
                const auto a = (historyWrite_ + pitchHistory_.size() - 1 - i) % pitchHistory_.size();
                const auto b = (a + pitchHistory_.size() - lag) % pitchHistory_.size();
                const double x = pitchHistory_[a], y = pitchHistory_[b];
                correlation += x * y; e1 += x * x; e2 += y * y;
            }
            best = std::max(best, correlation / std::sqrt(e1 * e2));
        }
        processReferenceFrame(frame_.data(), best > 0.65);
        fill_ = 0;
    }
}
void Vad2Detector::consume(double input) {
    if (!available()) return;
    input = pcm(input);
    low1_ += lowAlpha_ * (input - low1_); low2_ += lowAlpha_ * (low1_ - low2_);
    low3_ += lowAlpha_ * (low2_ - low3_); low4_ += lowAlpha_ * (low3_ - low4_);
    resamplePhase_ += 8000.0;
    if (resamplePhase_ >= rate_) {
        resamplePhase_ -= rate_;
        consume8k(rate_ == 8000.0 ? input : low4_);
    }
}
Vad2Result Vad2Detector::process(const float* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) consume(samples[i]);
    return result_;
}
Vad2Result Vad2Detector::processPCM16(const int16_t* samples, std::size_t count) {
    if (samples) for (std::size_t i = 0; i < count; ++i) consume(samples[i] / 32768.0);
    return result_;
}
} // namespace TelephonyDSP
