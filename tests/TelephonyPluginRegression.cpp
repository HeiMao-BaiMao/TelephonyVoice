#include "TelephonyVoice.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

// Keep processor/controller tests independent of a window system or VSTGUI.
// The production editor is linked only into the plug-in target.
IPlugView* PLUGIN_API TelephonyVoiceController::createView(FIDString) { return nullptr; }

static int failures = 0;
static void check(bool ok, const char* label) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", label);
    failures += !ok;
}
class SingleChange final : public IParameterChanges, public IParamValueQueue {
public:
    ParamID id; ParamValue value; bool empty = false;
    SingleChange(ParamID id, ParamValue value) : id(id), value(value) {}
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override { return index == 0 ? this : nullptr; }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID&, int32&) override { return nullptr; }
    ParamID PLUGIN_API getParameterId() override { return id; }
    int32 PLUGIN_API getPointCount() override { return empty ? 0 : 1; }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, ParamValue& result) override {
        if (index != 0 || empty) return kResultFalse;
        offset = 0; result = value; return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32, ParamValue, int32&) override { return kNotImplemented; }
};
static void automate(TelephonyVoiceProcessor& p, ParamID id, ParamValue value) {
    SingleChange change(id, value);
    ProcessData flush{};
    flush.inputParameterChanges = &change;
    check(p.process(flush) == kResultOk, "parameter-only flush succeeds");
}
static std::vector<char> save(TelephonyVoiceProcessor& processor) {
    MemoryStream stream;
    check(processor.getState(&stream) == kResultOk, "state serialization succeeds");
    return {stream.getData(), stream.getData() + stream.getSize()};
}
static tresult restore(TelephonyVoiceProcessor& processor, std::vector<char>& bytes) {
    MemoryStream stream(bytes.data(), (TSize)bytes.size());
    return processor.setState(&stream);
}
static tresult restore(TelephonyVoiceController& controller, std::vector<char>& bytes) {
    MemoryStream stream(bytes.data(), (TSize)bytes.size());
    return controller.setComponentState(&stream);
}
static int32 readInt(const std::vector<char>& bytes, size_t offset) {
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return (int32)(uint32(b[0]) | (uint32(b[1]) << 8) | (uint32(b[2]) << 16) | (uint32(b[3]) << 24));
}
static void writeBits(std::vector<char>& bytes, size_t offset, uint32 value) {
    for (int i = 0; i < 4; ++i) bytes[offset + i] = (char)(value >> (i * 8));
}
static void stateAndParameters() {
    TelephonyVoiceProcessor processor;
    TelephonyVoiceController controller;
    check(processor.initialize(nullptr) == kResultOk && controller.initialize(nullptr) == kResultOk, "component initialization");
#ifdef TELEPHONY_DISTRIBUTION_BUILD
    constexpr int maxEndpoint = 2;
#elif TELEPHONY_USE_EVS_JBM
    constexpr int maxEndpoint = 6;
#else
    constexpr int maxEndpoint = 5;
#endif
    ParameterInfo info{};
    bool found = false;
    for (int i = 0; i < controller.getParameterCount(); ++i) {
        controller.getParameterInfo(i, info);
        check(info.defaultNormalizedValue == controller.getParamNormalized(info.id), "parameter metadata default agrees with its initialized value");
        if (info.id == kParamEraMode) found = info.stepCount == maxEndpoint;
    }
    check(found, "route list has build-specific endpoint count");
    check(controller.normalizedParamToPlain(kParamDryWet, 0.5) == 50.0
        && controller.normalizedParamToPlain(kParamArtifactAmount, 0.5) == 50.0, "percent controls display actual percentages");
    auto defaults = save(processor);
    check(defaults.size() == 76, "saved state retains its existing 76-byte layout");
    check(restore(controller, defaults) == kResultOk, "controller restores default component state");
    for (int index = 0; index <= maxEndpoint; ++index) {
        const auto normalized = controller.plainParamToNormalized(kParamEraMode, index);
        automate(processor, kParamEraMode, normalized);
        automate(processor, kParamOutputEndpoint, normalized);
        auto bytes = save(processor);
        check(readInt(bytes, 0) == index && readInt(bytes, 20) == index, "route automation and persisted indices agree");
        check(restore(controller, bytes) == kResultOk
            && controller.getParamNormalized(kParamEraMode) == normalized
            && controller.getParamNormalized(kParamOutputEndpoint) == normalized, "controller route restore matches its own list mapping");
    }
    for (size_t length = 0; length <= defaults.size(); ++length) {
        auto prefix = std::vector<char>(defaults.begin(), defaults.begin() + length);
        const bool legal = length >= 20 && (length - 20) % 4 == 0;
        const auto before = save(processor);
        const bool accepted = restore(processor, prefix) == kResultOk;
        check(accepted == legal, "only complete legacy field boundaries are accepted");
        if (!accepted) check(save(processor) == before, "failed restore is atomic");
        check((restore(controller, prefix) == kResultOk) == legal, "controller uses the same legacy-state validation");
    }
    for (size_t offset : {4u, 8u, 14u, 28u, 32u}) {
        for (uint32 bits : {0x7fc00000u, 0x7f800000u, 0xff800000u}) {
            auto invalid = defaults;
            writeBits(invalid, offset, bits);
            const auto before = save(processor);
            check(restore(processor, invalid) != kResultOk && save(processor) == before, "non-finite saved values are rejected atomically");
            check(restore(controller, invalid) != kResultOk, "controller rejects non-finite saved values");
        }
    }
    auto invalid = defaults;
    for (size_t offset : {36u, 40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u, 72u}) writeBits(invalid, offset, 0x7fffffffu);
    check(restore(processor, invalid) == kResultOk && restore(controller, invalid) == kResultOk, "out-of-range codec settings are safely normalized");
    auto normalized = save(processor);
    check(readInt(normalized, 36) == 32000 && readInt(normalized, 40) == 13200 && readInt(normalized, 72) == 24000,
          "unsupported table settings use matching processor/controller defaults");
    auto highRateNb = defaults;
    writeBits(highRateNb, 36, 8000); writeBits(highRateNb, 40, 128000); writeBits(highRateNb, 44, EVS_FB);
    check(restore(processor, highRateNb) == kResultOk && restore(controller, highRateNb) == kResultOk,
          "independent EVS state controls receive deterministic safe combination");
    const auto safeNb = save(processor);
    check(safeNb == highRateNb,
          "state preserves nominal EVS choices while DSP applies safe effective caps");
    automate(processor, kParamEraMode, 0.4);
    const auto before = save(processor);
    automate(processor, kParamEraMode, std::numeric_limits<double>::quiet_NaN());
    automate(processor, kParamOutputGain, std::numeric_limits<double>::infinity());
    check(save(processor) == before, "non-finite automation is ignored");
    SingleChange empty(kParamEraMode, 1.0); empty.empty = true;
    ProcessData flush{}; flush.inputParameterChanges = &empty;
    check(processor.process(flush) == kResultOk && save(processor) == before, "empty parameter queue is safe");
    class FailedWrite final : public MemoryStream {
        tresult PLUGIN_API write(void*, int32, int32* written) override { if (written) *written = 0; return kResultFalse; }
    } failed;
    check(processor.getState(&failed) != kResultOk, "state save reports stream write failure");
    check(processor.setState(nullptr) != kResultOk && processor.getState(nullptr) != kResultOk
        && controller.setComponentState(nullptr) != kResultOk, "null state streams are rejected");
    controller.terminate(); processor.terminate();
}
static void audioProcessing() {
    TelephonyVoiceProcessor processor;
    processor.initialize(nullptr);
    SpeakerArrangement mono = SpeakerArr::kMono, stereo = SpeakerArr::kStereo, surround = SpeakerArr::k51;
    check(processor.setBusArrangements(&mono, 1, &stereo, 1) == kResultOk, "mono-to-stereo bus arrangement accepted");
    check(processor.setBusArrangements(&surround, 1, &stereo, 1) != kResultOk
        && processor.setBusArrangements(nullptr, 1, &stereo, 1) != kResultOk, "unsupported/null bus arrangements rejected");
    ProcessSetup setup{}; setup.sampleRate = 48000; setup.maxSamplesPerBlock = 512; setup.symbolicSampleSize = kSample64;
    check(processor.setupProcessing(setup) != kResultOk, "64-bit sample setup rejected");
    setup.symbolicSampleSize = kSample32;
    setup.sampleRate = std::numeric_limits<double>::quiet_NaN();
    check(processor.setupProcessing(setup) != kResultOk, "non-finite sample rate rejected");
    setup.sampleRate = 48000;
    check(processor.setupProcessing(setup) == kResultOk, "valid sample setup accepted");
    automate(processor, kParamDryWet, 0.0);
    processor.setActive(true);
    const int delay = processor.getLatencySamples();
    check(delay == 48000 * TelephonyDSP::LATENCY_MS / 1000, "plugin reports exact configured latency");
    bool correct = true;
    int absolute = 0;
    for (int block = 0; block < 60; ++block) {
        if (block == 8) automate(processor, kParamMasterBypass, 1.0);
        if (block == 35) automate(processor, kParamMasterBypass, 0.0);
        float left[512], right[512];
        for (int i = 0; i < 512; ++i) left[i] = float((absolute + i) % 1001) / 2000.0f;
        std::fill_n(right, 512, -99.0f);
        float* in[] = {left}; float* out[] = {left, right};
        AudioBusBuffers input{}; input.numChannels = 1; input.channelBuffers32 = in;
        AudioBusBuffers output{}; output.numChannels = 2; output.channelBuffers32 = out; output.silenceFlags = 3;
        ProcessData data{}; data.numInputs = data.numOutputs = 1; data.inputs = &input; data.outputs = &output;
        data.numSamples = 512; data.symbolicSampleSize = kSample32;
        correct = correct && processor.process(data) == kResultOk && output.silenceFlags == 0;
        for (int i = 0; i < 512; ++i) {
            const int sourceIndex = absolute + i - delay;
            const float expected = sourceIndex < 0 ? 0.0f : float(sourceIndex % 1001) / 2000.0f;
            correct = correct && left[i] == expected && right[i] == expected;
        }
        absolute += 512;
    }
    check(correct, "in-place mono/stereo bypass toggles preserve exact delay and continuous timeline");
    processor.setActive(false); processor.terminate();
}
static void stereoDownmix() {
    TelephonyVoiceProcessor processor;
    processor.initialize(nullptr);
    SpeakerArrangement stereo = SpeakerArr::kStereo, mono = SpeakerArr::kMono;
    check(processor.setBusArrangements(&stereo, 1, &mono, 1) == kResultOk, "stereo-to-mono arrangement accepted");
    ProcessSetup setup{}; setup.sampleRate = 48000; setup.maxSamplesPerBlock = 512; setup.symbolicSampleSize = kSample32;
    processor.setupProcessing(setup);
    automate(processor, kParamDryWet, 0);
    processor.setActive(true);
    bool correct = true;
    for (int block = 0; block < 60; ++block) {
        if (block == 12) automate(processor, kParamMasterBypass, 1);
        if (block == 30) automate(processor, kParamMasterBypass, 0);
        float left[512] = {}, right[512]; std::fill_n(right, 512, 0.4f);
        float* in[] = {left, right}; float* out[] = {left};
        AudioBusBuffers input{}; input.numChannels = 2; input.channelBuffers32 = in;
        AudioBusBuffers output{}; output.numChannels = 1; output.channelBuffers32 = out;
        ProcessData data{}; data.numInputs = data.numOutputs = 1; data.inputs = &input; data.outputs = &output;
        data.numSamples = 512; data.symbolicSampleSize = kSample32;
        correct = correct && processor.process(data) == kResultOk;
        for (int i = 0; i < 512; ++i)
            correct = correct && left[i] == ((block * 512 + i < (int)processor.getLatencySamples()) ? 0.0f : 0.2f);
    }
    check(correct, "stereo-to-mono sums both channels consistently during bypass toggles");
    processor.setActive(false); processor.terminate();
}
int main() { stateAndParameters(); audioProcessing(); stereoDownmix(); return failures ? 1 : 0; }
