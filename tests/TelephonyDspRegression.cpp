#include "dsp/SignalProcessor.h"
#include "dsp/RingBuffer.h"
#include "dsp/OpusCodec.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace TelephonyDSP;
static int failures = 0;
static void check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
    failures += !ok;
}
static std::vector<float> stimulus(int count) {
    std::vector<float> data(count);
    for (int i = 0; i < count; ++i)
        data[i] = 0.3f * std::sin(0.039f * i) + 0.17f * std::sin(0.137f * i);
    data[0] = 0.75f;
    return data;
}
static std::vector<float> render(SignalProcessor& dsp, const std::vector<float>& input, int block, bool inPlace = false) {
    std::vector<float> result(input.size(), 0.0f);
    for (size_t offset = 0; offset < input.size(); offset += block) {
        const int count = (int)std::min<size_t>(block, input.size() - offset);
        std::vector<float> source(input.begin() + offset, input.begin() + offset + count);
        float* in = source.data();
        float* out = inPlace ? source.data() : result.data() + offset;
        dsp.process(&in, 1, &out, 1, count);
        if (inPlace) std::copy_n(source.data(), count, result.data() + offset);
    }
    return result;
}
static bool matches(const std::vector<float>& a, const std::vector<float>& b, float tolerance = 0.0f) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!std::isfinite(a[i]) || !std::isfinite(b[i]) || std::fabs(a[i] - b[i]) > tolerance) return false;
    return true;
}
static void exactLatency() {
    for (int rate : {8000, 44100, 48000, 96000, 192000}) {
        const auto input = stimulus(rate / 2);
        for (int block : {1, 127, 512, 8192}) {
            SignalProcessor dsp;
            dsp.setSampleRate(rate);
            dsp.setMode(EraMode::Bypass);
            const auto output = render(dsp, input, block);
            const int latency = dsp.getLatencySamples();
            bool ok = latency == (int)std::round(rate * (LATENCY_MS / 1000.0));
            for (size_t i = 0; i < output.size(); ++i)
                ok = ok && output[i] == (i < (size_t)latency ? 0.0f : input[i - latency]);
            char label[100];
            std::snprintf(label, sizeof label, "exact reported dry delay at %d Hz / block %d", rate, block);
            check(ok, label);
        }
    }
}
static void streamingAndReset() {
    const auto input = stimulus(48000);
    SignalProcessor dsp;
    dsp.setSampleRate(48000);
    dsp.setRoute(RouteEndpoint::FixedLine, RouteEndpoint::FixedLine, DegradationSegment::Both);
    const auto block512 = render(dsp, input, 512);
    dsp.reset();
    check(matches(block512, render(dsp, input, 127), 1e-6f), "two-leg route is independent of host block boundaries");
    dsp.reset();
    check(matches(block512, render(dsp, input, 512, true)), "codec route supports in-place processing");
    check(std::any_of(block512.begin() + 10000, block512.end(), [](float s) { return std::fabs(s) > 0.001f; }),
          "latency-enabled route produces sustained non-silent output");
    dsp.setParameters(1.0f, 0.0f, true, 0.4f, 0.3f, 0.4f);
    dsp.reset();
    const auto degraded = render(dsp, input, 512);
    dsp.reset();
    check(matches(degraded, render(dsp, input, 512)), "reset reproduces packet loss and per-instance artifacts");
    dsp.setMode(EraMode::Bypass);
    dsp.setParameters(1.0f, 0.0f, false, 0.0f);
    dsp.setSimulateLatency(false);
    check(matches(input, render(dsp, input, 512)), "disabling latency clears obsolete delay queues");
    dsp.reset();
    const auto large = stimulus(300000);
    check(matches(large, render(dsp, large, 300000, true)), "large offline in-place blocks do not overflow fixed rings");
}
static void emptyAndNonfinite() {
    SignalProcessor dsp;
    dsp.setSampleRate(48000);
    dsp.setMode(EraMode::Bypass);
    dsp.setSimulateLatency(false);
    dsp.process(nullptr, 0, nullptr, 0, 0);
    float output[3] = {9, 9, 9};
    float* out = output;
    dsp.process(nullptr, 0, &out, 1, 3);
    check(output[0] == 0 && output[1] == 0 && output[2] == 0, "no-input DSP call initializes output to silence");
    float input[3] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 0.5f };
    float* in = input;
    dsp.setParameters(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                      true, std::numeric_limits<float>::quiet_NaN());
    dsp.process(&in, 1, &out, 1, 3);
    check(output[0] == 0 && output[1] == 0 && output[2] == 0.5f, "invalid float inputs/parameters cannot poison future audio");
    check(clampToInt16(std::numeric_limits<float>::quiet_NaN()) == 0, "PCM conversion handles NaN without undefined integer conversion");
    RingBuffer empty;
    check(empty.read(nullptr, 0) == 0 && empty.write(nullptr, 0) == 0 && empty.peek(nullptr, 0) == 0,
          "empty ring operations do not dereference null storage");
}
static void channelConfiguration() {
    const auto input = stimulus(48000);
    auto run = [&](bool reapply) {
        SignalProcessor dsp;
        dsp.setSampleRate(48000);
        dsp.setMode(EraMode::PSTN_G711);
        dsp.setParameters(1, 0, false, 0, 0.4f, 0.5f);
        std::vector<float> result(input.size());
        for (size_t offset = 0; offset < input.size(); offset += 512) {
            if (reapply) { dsp.setG711Law(0); dsp.setParameters(1, 0, false, 0, 0.4f, 0.5f); }
            const int count = (int)std::min<size_t>(512, input.size() - offset);
            float* in = const_cast<float*>(input.data()) + offset;
            float* out = result.data() + offset;
            dsp.process(&in, 1, &out, 1, count);
        }
        return result;
    };
    check(matches(run(false), run(true)), "unchanged controls preserve live codec PLC history");
#if TELEPHONY_EXPERIMENTAL_NETWORK
    OpusCodec minimumRate(48000, 6, 6, 1105);
    check(minimumRate.getBitrate() == 6000, "Opus construction clamps bps to the supported6kbps minimum");
    minimumRate.setBitrate(-1);
    check(minimumRate.getBitrate() == 6000, "Opus live controls use the same minimum as construction");
    SignalProcessor dsp;
    dsp.setSampleRate(48000);
    dsp.setOpusBitrate(64000);
    dsp.setMode(EraMode::OPUS_VOIP);
    render(dsp, input, 512);
    dsp.setMode(EraMode::EVS_LIKE);
    const auto switched = render(dsp, input, 127);
    check(std::all_of(switched.begin(), switched.end(), [](float x) { return std::isfinite(x); }),
          "switching Opus to EVS-like rebuilds correctly sized VAD state");
#endif
}
static void routeTransitionsAndQuietAudio() {
    const auto input = stimulus(24000);
    for (int rate : {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 384000}) {
        const auto input = stimulus(rate / 2);
        SignalProcessor route;
        route.setSampleRate(rate);
        route.setRoute(RouteEndpoint::FixedLine, RouteEndpoint::FixedLine, DegradationSegment::Both);
        const auto expected = render(route, input, 512);
        route.reset();
        check(matches(expected, render(route, input, 127), 1e-6f), "framed two-leg timing is block independent at supported sample rates");
        route.setSimulateLatency(false);
        route.reset();
        check(matches(expected, render(route, input, 127), 1e-6f), "codec timing reserve remains when optional bypass delay is disabled");
    }
    SignalProcessor changed, fresh;
    changed.setSampleRate(48000); fresh.setSampleRate(48000);
    changed.setRoute(RouteEndpoint::FixedLine, RouteEndpoint::FixedLine, DegradationSegment::Both);
    render(changed, input, 512);
    changed.setRoute(RouteEndpoint::FixedLine, RouteEndpoint::Mobile5G, DegradationSegment::Both);
    fresh.setRoute(RouteEndpoint::FixedLine, RouteEndpoint::Mobile5G, DegradationSegment::Both);
    changed.setParameters(0.5f, 0, false, 0); fresh.setParameters(0.5f, 0, false, 0);
    check(matches(render(changed, input, 512), render(fresh, input, 512)), "route changes re-prime dry and wet against the same timeline");
    for (float amplitude : {0.01f, 0.05f}) {
        SignalProcessor quiet;
        quiet.setSampleRate(48000);
        quiet.setMode(EraMode::EVS_LIKE);
        auto tone = stimulus(24000);
        for (auto& sample : tone) sample *= amplitude;
        const auto output = render(quiet, tone, 512);
        double energy = 0;
        for (size_t i = 10000; i < output.size(); ++i) energy += output[i] * output[i];
        check(energy > 1e-4, "EVS-like filter-only path preserves quiet audio at zero packet loss");
    }
#ifndef TELEPHONY_DISTRIBUTION_BUILD
    for (EraMode mode : {EraMode::EVS_NATIVE
#if TELEPHONY_USE_EVS_JBM
        , EraMode::EVS_JBM
#endif
    }) {
        SignalProcessor continuous, automated;
        for (auto* p : {&continuous, &automated}) { p->setSampleRate(48000); p->setMode(mode); }
        const auto expected = render(continuous, input, 512);
        std::vector<float> actual(input.size());
        for (size_t offset = 0; offset < input.size(); offset += 512) {
            automated.setParameters(1, 0, false, 0);
            automated.setEvsScVbrEnabled(false);
            const int count = (int)std::min<size_t>(512, input.size() - offset);
            float* in = const_cast<float*>(input.data()) + offset;
            float* out = actual.data() + offset;
            automated.process(&in, 1, &out, 1, count);
        }
        check(matches(expected, actual), "unchanged EVS controls never reset live codec state");
        automated.setEVSConfig(32000, 16400, EVS_SWB);
        SignalProcessor newConfig;
        newConfig.setSampleRate(48000); newConfig.setMode(mode); newConfig.setEVSConfig(32000, 16400, EVS_SWB);
        check(matches(render(automated, input, 512), render(newConfig, input, 512)), "EVS configuration changes re-prime a consistent compensated timeline");
        continuous.setEVSConfig(8000, 13200, EVS_NB);
        continuous.setEVSConfig(8000, 32000, EVS_NB);
        newConfig.setEVSConfig(8000, 32000, EVS_NB);
        check(matches(render(continuous, input, 512), render(newConfig, input, 512)), "unsupported NB/high-rate choice normalizes independently of prior state");
    }
#endif
}
int main() {
    exactLatency(); streamingAndReset(); emptyAndNonfinite(); channelConfiguration(); routeTransitionsAndQuietAudio();
    return failures ? 1 : 0;
}
