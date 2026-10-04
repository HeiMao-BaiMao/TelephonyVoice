#include "dsp/AudioSimulation.h"
#include "dsp/WaveformConcealer.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

using namespace TelephonyDSP;
namespace {
constexpr double pi = 3.14159265358979323846;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
double rms(const std::vector<float>& x, std::size_t skip = 0) {
    double sum = 0.0; for (std::size_t i = skip; i < x.size(); ++i) sum += x[i] * x[i];
    return std::sqrt(sum / std::max<std::size_t>(1, x.size() - skip));
}
double spectralPower(const std::vector<float>& x, double frequency, double rate) {
    double re = 0.0, im = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        re += x[i] * std::cos(2 * pi * frequency * i / rate);
        im += x[i] * std::sin(2 * pi * frequency * i / rate);
    }
    return (re * re + im * im) / (x.size() * x.size());
}
void finiteBounded(const std::vector<float>& x, const char* what) {
    for (const auto value : x) require(std::isfinite(value) && std::abs(value) <= 1.00001f, what);
}
void testDtmf() {
    for (double sr : {8000.0, 11025.0, 44100.0, 48000.0, 192000.0, 384000.0}) {
        DtmfGenerator a, b; a.configure(sr); b.configure(sr);
        a.setSequence("5", 100.0, 50.0, -12.0, true); b.setSequence("5", 100.0, 50.0, -12.0, true);
        std::vector<float> x(static_cast<std::size_t>(sr * 0.3)), y(x.size());
        a.generate(x.data(), x.size());
        for (std::size_t i = 0; i < y.size();) {
            const auto n = std::min<std::size_t>(37, y.size() - i); b.generate(y.data() + i, n); i += n;
        }
        require(x == y, "DTMF block partition changed output");
        const auto tone = static_cast<std::size_t>(std::llround(sr * 0.1));
        const auto gap = static_cast<std::size_t>(std::llround(sr * 0.05));
        for (std::size_t i = tone; i < tone + gap; ++i) require(x[i] == 0.0f, "DTMF gap not silent");
        std::vector<float> on(x.begin(), x.begin() + tone);
        require(spectralPower(on, 770, sr) > 100 * spectralPower(on, 941, sr), "DTMF low group wrong");
        require(spectralPower(on, 1336, sr) > 100 * spectralPower(on, 1633, sr), "DTMF high group wrong");
        require(rms(on) > 0.1 && rms(on) < 0.14, "DTMF level not correct");
        a.reset(); a.generate(y.data(), y.size()); require(x == y, "DTMF reset not repeatable");
        finiteBounded(x, "DTMF output not bounded");
    }
    DtmfGenerator zeroLevel; zeroLevel.setSequence("1", 75, 50, -120);
    for (int i = 0; i < 1000; ++i) require(zeroLevel.next() == 0.0f, "zero DTMF level not silent");
    DtmfGenerator invalid; invalid.setSequence("z !"); require(invalid.finished() && invalid.next() == 0.0f, "invalid DTMF handling");
    for (char key : std::string("123A456B789C*0#D")) { double a, b; require(DtmfGenerator::frequencies(key, a, b) && a > 0 && b > a, "DTMF key map incomplete"); }
}
void testComfortNoise() {
    for (double sr : {8000.0, 44100.0, 384000.0}) {
        PsdComfortNoise a(sr), b(sr);
        std::vector<float> input(16384), x(32768), y(x.size());
        uint32_t seed = 123u; double low = 0.0;
        for (auto& v : input) {
            seed = seed * 1664525u + 1013904223u;
            const double noise = static_cast<double>(seed) / 2147483648.0 - 1.0;
            low += 0.15 * (noise - low); v = static_cast<float>(low * 0.4);
        }
        a.observe(input.data(), input.size());
        for (std::size_t i = 0; i < input.size();) {
            const auto n = std::min<std::size_t>(73, input.size() - i); b.observe(input.data() + i, n); i += n;
        }
        a.generate(x.data(), x.size());
        for (std::size_t i = 0; i < y.size();) {
            const auto n = std::min<std::size_t>(31, y.size() - i); b.generate(y.data() + i, n); i += n;
        }
        require(x == y, "PSD CNG partition invariance failed");
        require(a.powerSpectrum()[4] > a.powerSpectrum()[100] * 4, "PSD does not capture spectral color");
        const double ratio = rms(x, 256) / a.targetRms();
        require(ratio > 0.8 && ratio < 1.2, "CNG RMS mismatch");
        double smooth = 0.0, energyLow = 0.0, energyHigh = 0.0;
        for (auto v : x) { smooth += 0.2 * (v - smooth); energyLow += smooth * smooth; energyHigh += (v - smooth) * (v - smooth); }
        require(energyLow > energyHigh, "CNG spectral shaping absent");
        a.reset(); a.observe(input.data(), input.size()); a.generate(y.data(), y.size());
        require(x == y, "CNG reset not deterministic"); finiteBounded(x, "CNG bounds failed");
        a.reset(); a.generate(y.data(), y.size()); require(rms(y) == 0.0, "untrained CNG not silent");
    }
    WaveformConcealer plc; plc.reset(160);
    std::vector<int16_t> good(1600), loss(1600);
    for (std::size_t i = 0; i < good.size(); ++i) good[i] = static_cast<int16_t>(5000 * std::sin(2 * pi * 430 * i / 8000));
    plc.storeGoodFrame(good.data(), static_cast<int>(good.size())); plc.conceal(loss.data(), static_cast<int>(loss.size()));
    require(good != loss && std::any_of(loss.begin(), loss.end(), [](auto x) { return x != 0; }), "legacy PLC did not use shaped CNG");
}
void testClock() {
    for (double sr : {8000.0, 44100.0, 384000.0}) {
        for (double ppm : {-50.0, 0.0, 50.0}) {
            ClockDriftResampler a, b; a.configure(sr, ppm); b.configure(sr, ppm);
            std::vector<float> input(65536), x(input.size()), y(input.size());
            for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(0.4 * std::sin(2 * pi * 1000 * i / sr));
            x = y = input; a.process(x.data(), x.size());
            for (std::size_t i = 0; i < y.size();) { const auto n = std::min<std::size_t>(67, y.size() - i); b.process(y.data() + i, n); i += n; }
            require(x == y, "clock drift partition failed"); finiteBounded(x, "clock drift bounds failed");
            if (ppm == 0) for (std::size_t i = a.latencySamples(); i < x.size(); ++i)
                require(x[i] == input[i - a.latencySamples()], "zero ppm is not pure delay");
            a.reset(); y = input; a.process(y.data(), y.size()); require(x == y, "clock reset mismatch");
        }
    }
    for (double ppm : {-50.0, 50.0}) {
        ClockDriftResampler clock; clock.configure(8000, ppm, 0.25);
        for (int i = 0; i < 1200000; ++i) {
            const auto x = clock.process(0.25f);
            require(std::isfinite(x), "clock finite overrun/underrun");
            require(clock.queuedSamples() >= 0.0 && clock.queuedSamples() < 24.0, "clock FIFO unbounded");
        }
        require(ppm > 0 ? clock.underruns() > 0 : clock.overruns() > 0, "clock drift did not trigger elastic recovery");
    }
}
void testEcho() {
    HybridEcho muted; muted.configure(8000, 1, -120); require(muted.returnGain() == 0.0, "zero echo gain not silent");
    for (double sr : {8000.0, 44100.0, 384000.0}) {
        HybridEcho a, b; a.configure(sr, 20, -12, 2500, 100); b.configure(sr, 20, -12, 2500, 100);
        std::vector<float> x(static_cast<std::size_t>(sr * 0.5), 0.0f), y;
        x[0] = 0.5f; y = x; a.process(x.data(), x.size());
        for (std::size_t i = 0; i < y.size();) { const auto n = std::min<std::size_t>(127, y.size() - i); b.process(y.data() + i, n); i += n; }
        require(x == y, "echo partition changed output"); require(x[0] == 0.5f, "echo damaged dry impulse");
        for (std::size_t i = 1; i < a.delaySamples(); ++i) require(x[i] == 0.0f, "echo predates configured delay");
        require(std::abs(x[a.delaySamples()]) > 1e-6, "echo absent"); finiteBounded(x, "echo unstable");
        double tail = 0.0; for (std::size_t i = x.size() * 9 / 10; i < x.size(); ++i) tail += x[i] * x[i];
        require(tail < 1e-8, "echo tail failed to decay");
        a.reset(); std::fill(y.begin(), y.end(), 0.0f); y[0] = 0.5f; a.process(y.data(), y.size()); require(x == y, "echo reset mismatch");
        a.configure(sr, 1, 100, 20000, 1); require(a.returnGain() <= 0.45, "echo feedback gain not bounded");
        for (int i = 0; i < 100000; ++i) require(std::abs(a.process(1.0f)) <= 1.0f, "echo feedback runaway");
    }
}
void testFading() {
    RayleighFading defaultModel;
    for (int i = 0; i < 10000; ++i) require(std::isfinite(defaultModel.next().power), "default fading state invalid");
    double power = 0.0; int below1 = 0, n = 0;
    for (uint32_t seed = 1; seed <= 16; ++seed) {
        RayleighFading fading; fading.configure(8000, 17, 20, seed);
        for (int i = 0; i < 16000; ++i) { const auto x = fading.next(); power += x.power; below1 += x.power < 1; ++n; }
    }
    require(power / n > 0.85 && power / n < 1.15, "Rayleigh mean power not normalized");
    require(double(below1) / n > 0.55 && double(below1) / n < 0.72, "Rayleigh amplitude distribution missing");
    for (double sr : {8000.0, 44100.0, 384000.0}) {
        RayleighFading a, b; a.configure(sr, 5, 20); b.configure(sr, 5, 20);
        std::vector<float> x(8192), y(8192);
        for (auto& v : x) v = a.process(0.5f);
        for (auto& v : y) v = b.process(0.5f);
        require(x == y, "fading seed repeatability failed"); finiteBounded(x, "fading unbounded");
        a.reset(); for (auto& v : y) v = a.process(0.5f); require(x == y, "fading reset mismatch");
        const auto state = a.state(); require(state.suggestedModeFraction >= 0 && state.suggestedModeFraction <= 1, "CI mode range wrong");
    }
    RayleighFading low, high; low.configure(8000, 0, -30); high.configure(8000, 0, 60);
    for (int i = 0; i < 8000; ++i) { low.next(); high.next(); }
    require(low.state().suggestedModeFraction == 0 && high.state().suggestedModeFraction == 1, "CI adaptation did not respond");
}
void testHandover() {
    for (double sr : {8000.0, 44100.0, 384000.0}) {
        HandoverGate a, b; a.configure(sr, 100, 0, 5); b.configure(sr, 100, 0, 5); a.trigger(); b.trigger();
        std::vector<float> x(static_cast<std::size_t>(sr * 0.2), 0.5f), y(x);
        a.process(x.data(), x.size());
        for (std::size_t i = 0; i < y.size();) { const auto n = std::min<std::size_t>(11, y.size() - i); b.process(y.data() + i, n); i += n; }
        require(x == y, "handover partition invariance failed");
        for (std::size_t i = 0; i < static_cast<std::size_t>(std::llround(sr * 0.1)); ++i) require(x[i] == 0, "handover mute length wrong");
        require(x.back() == 0.5f && a.count() == 1 && !a.active(), "handover did not recover");
        a.reset(); require(a.count() == 0 && a.process(0.5f) == 0.5f, "handover reset failed");
        a.configure(sr, 50, 0.2, 5);
        for (int i = 0; i < static_cast<int>(sr); ++i) a.process(0.5f);
        require(a.count() == 4, "periodic handover count wrong");
    }
}
void testVad() {
    Vad2Detector detector;
#if TELEPHONY_HAVE_VAD2
    require(detector.available(), "reference VAD2 backend unavailable");
    for (double sr : {8000.0, 11025.0, 44100.0, 384000.0}) {
        Vad2Detector a, b; a.configure(sr); b.configure(sr);
        std::vector<float> input(static_cast<std::size_t>(sr * 2.0), 0.0f);
        for (std::size_t i = input.size() / 4; i < input.size() / 2; ++i)
            input[i] = static_cast<float>(0.2 * (std::sin(2 * pi * 180 * i / sr) + 0.5 * std::sin(2 * pi * 790 * i / sr)));
        // Compare every 10 ms output boundary despite irregular host blocks.
        std::size_t speechFrames = 0; bool sawHangover = false;
        for (std::size_t i = 0; i < input.size();) {
            const auto n = std::min<std::size_t>(137, input.size() - i);
            auto r = a.process(input.data() + i, n);
            for (std::size_t j = 0; j < n; ++j) b.process(input.data() + i + j, 1);
            const auto s = b.result();
            require(r.speech == s.speech && r.primarySpeech == s.primarySpeech && r.hangoverFrames == s.hangoverFrames && r.framesProcessed == s.framesProcessed,
                    "VAD2 partition invariance failed");
            speechFrames += r.speech; sawHangover |= r.speech && !r.primarySpeech;
            i += n;
        }
        require(a.result().framesProcessed == 200, "VAD2 wrong resampling/frame duration");
        require(speechFrames > 0, "VAD2 failed to detect synthetic voice");
        require(sawHangover, "VAD2 hangover absent");
        require(!a.result().speech, "VAD2 never returned to silence");
        const auto old = a.result(); a.reset(); a.process(input.data(), input.size());
        require(old.framesProcessed == a.result().framesProcessed && old.speech == a.result().speech, "VAD2 reset mismatch");
    }
#else
    require(!detector.available() && !detector.result().available, "disabled VAD2 falsely available");
    float input = 1.0f; require(!detector.process(&input, 1).available, "VAD2 fake fallback present");
#endif
}
void testInvalid() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    DtmfGenerator dtmf; dtmf.configure(nan); dtmf.setSequence("1", nan, nan, nan); require(std::isfinite(dtmf.next()), "invalid DTMF settings");
    HybridEcho echo; echo.configure(nan, nan, nan, nan, nan); require(echo.process(static_cast<float>(nan)) == 0, "echo NaN not contained");
    ClockDriftResampler drift; drift.configure(nan, nan, nan); require(std::isfinite(drift.process(static_cast<float>(nan))), "drift NaN not contained");
    PsdComfortNoise noise; float invalid = static_cast<float>(nan); for (int i = 0; i < 256; ++i) noise.observe(&invalid, 1); require(noise.next() == 0, "CNG NaN poisoned estimator");
}
}
int main() {
    try {
        testDtmf(); testComfortNoise(); testClock(); testEcho(); testFading(); testHandover(); testVad(); testInvalid();
        std::cout << "Audio simulation synthetic tests passed\n"; return 0;
    } catch (const std::exception& e) {
        std::cerr << "Audio simulation test failed: " << e.what() << '\n'; return 1;
    }
}
