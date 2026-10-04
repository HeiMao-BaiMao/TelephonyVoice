#include "TelephonyVoice.h"
#include "public.sdk/source/common/memorystream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include <map>
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
    tresult PLUGIN_API addPoint(int32, ParamValue input, int32& index) override { value = input; empty = false; index = 0; return kResultOk; }
};
class CapturedChanges final : public IParameterChanges {
public:
    std::map<ParamID, SingleChange> values;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    int32 PLUGIN_API getParameterCount() override { return (int32)values.size(); }
    IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        if (index < 0 || index >= (int32)values.size()) return nullptr;
        auto it = values.begin(); std::advance(it, index); return &it->second;
    }
    IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override {
        index = 0; return &values.try_emplace(id, id, 0.0).first->second;
    }
};
class TestComponentHandler final : public IComponentHandler {
public:
    int restarts = 0; int32 flags = 0;
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override { *obj = nullptr; return kNoInterface; }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 input) override { ++restarts; flags |= input; return kResultOk; }
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
    check(defaults.size() == 96 + 8 * TelephonyDSP::advancedDescriptors.size(), "saved state retains legacy prefix and appends versioned advanced extension");
    check(readInt(defaults, 76) == 0x58415654 && readInt(defaults, 80) == 1, "advanced state has explicit magic and version");
    check(restore(controller, defaults) == kResultOk, "controller restores default component state");
    auto olderExtension = defaults; writeBits(olderExtension, 84, 1); olderExtension.resize(104);
    check(restore(processor, olderExtension) == kResultOk && restore(controller, olderExtension) == kResultOk,
          "complete earlier extension counts retain defaults for appended controls");
    check(restore(processor, defaults) == kResultOk && restore(controller, defaults) == kResultOk, "current state restored after prior-extension test");
    for (ParamID id : {ParamID(kParamInputPeak), ParamID(kParamOutputPeak), ParamID(kParamMeasuredLoss), ParamID(kParamMeasuredJitter), ParamID(kParamOpusActualMode)}) {
        const auto& meter = controller.getParameterObject(id)->getInfo();
        check((meter.flags & ParameterInfo::kIsReadOnly) && !(meter.flags & ParameterInfo::kCanAutomate),
              "live telemetry is read-only and cannot be automated");
    }
    String128 label{};
    controller.getParamStringByValue(kParamAdvancedBase + (ParamID)TelephonyDSP::AdvancedControl::JitterDistribution, 1.0 / 3.0, label);
    check(label[0] == 'G' && label[1] == 'a', "jitter option labels match actual Gamma distribution enum");
    controller.getParamStringByValue(kParamAdvancedBase + (ParamID)TelephonyDSP::AdvancedControl::EvsPayloadStyle, 0.0, label);
    check(label[0] == 'A' && label[1] == 'u', "EVS payload list begins with actual Auto wire mode");
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
        const bool legal = length == defaults.size() || (length >= 20 && length <= 76 && (length - 20) % 4 == 0);
        const auto before = save(processor);
        const bool accepted = restore(processor, prefix) == kResultOk;
        check(accepted == legal, "only complete legacy field boundaries are accepted");
        if (!accepted) check(save(processor) == before, "failed restore is atomic");
        check((restore(controller, prefix) == kResultOk) == legal, "controller uses the same legacy-state validation");
    }
    // Appended controls are individually automatable and survive processor/controller restore.
    for (size_t index = 0; index < TelephonyDSP::advancedDescriptors.size(); ++index) {
        const ParamID id = kParamAdvancedBase + (ParamID)index;
        const double setting = double(index % 3) / 2.0;
        automate(processor, id, setting);
        auto bytes = save(processor);
        check(restore(controller, bytes) == kResultOk && controller.getParamNormalized(id) == setting,
              "advanced automation survives component-state round trip");
    }
    check(restore(processor, defaults) == kResultOk && restore(controller, defaults) == kResultOk, "default advanced state restores");
    for (size_t offset : {76u, 80u, 84u}) {
        auto invalid = defaults; writeBits(invalid, offset, 0xffffffffu);
        const auto before = save(processor);
        check(restore(processor, invalid) != kResultOk && save(processor) == before
            && restore(controller, invalid) != kResultOk, "invalid extension header is rejected atomically");
    }
    for (uint32 highBits : {0x7ff80000u, 0x7ff00000u, 0xbff00000u, 0x40000000u}) {
        auto invalid = defaults; writeBits(invalid, 96, 0); writeBits(invalid, 100, highBits);
        const auto before = save(processor);
        check(restore(processor, invalid) != kResultOk && save(processor) == before
            && restore(controller, invalid) != kResultOk, "non-finite and out-of-range advanced state rejected atomically");
    }
#if !defined(TELEPHONY_DISTRIBUTION_BUILD) && TELEPHONY_EXPERIMENTAL_NETWORK
    automate(processor, kParamInputRoute, 1.0); automate(processor, kParamOutputRoute, 1.0);
    auto opusState = save(processor);
    check(readInt(opusState, 0) == 4 && readInt(opusState, 20) == 4
        && readInt(opusState, 88) == 100 && readInt(opusState, 92) == 100,
        "Opus state uses compatible legacy fallback plus exact semantic route extension");
    check(restore(controller, opusState) == kResultOk && controller.getParamNormalized(kParamInputRoute) == 1.0
        && controller.getParamNormalized(kParamOutputRoute) == 1.0, "modern Opus routes restore in controller");
    automate(processor, kParamEraMode, 1.0);
    auto legacyAutomation = save(processor);
    check(readInt(legacyAutomation, 88) == maxEndpoint, "legacy route automation keeps historical normalized meaning after Opus");
    check(controller.setParamNormalized(kParamEraMode, 1.0) == kResultOk
        && controller.normalizedParamToPlain(kParamInputRoute, controller.getParamNormalized(kParamInputRoute)) == maxEndpoint,
        "controller legacy and modern route aliases stay synchronized");
#endif
    check(restore(processor, defaults) == kResultOk && restore(controller, defaults) == kResultOk, "defaults restored after modern route tests");
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
static void telemetryAndTransport() {
    TelephonyVoiceProcessor processor;
    TelephonyVoiceController controller;
    TestComponentHandler handler;
    processor.initialize(nullptr); controller.initialize(nullptr); controller.setComponentHandler(&handler);
    ProcessSetup setup{}; setup.sampleRate = 48000; setup.maxSamplesPerBlock = 512; setup.symbolicSampleSize = kSample32;
    processor.setupProcessing(setup);
    const auto needs = processor.getProcessContextRequirements();
    check((needs & IProcessContextRequirements::kNeedTransportState) && (needs & IProcessContextRequirements::kNeedTempo)
        && (needs & IProcessContextRequirements::kNeedProjectTimeMusic), "host is asked to supply transport and musical timing");
    SpeakerArrangement mono = SpeakerArr::kMono; processor.setBusArrangements(&mono, 1, &mono, 1);
    automate(processor, kParamOutputEndpoint, 0.0);
    automate(processor, kParamDryWet, 0.0);
    automate(processor, kParamAdvancedBase + (ParamID)TelephonyDSP::AdvancedControl::HostClock, 1.0);
    processor.setActive(true);
    float input[512], output[512]; std::fill_n(input, 512, 0.25f);
    float* in[] = {input}; float* out[] = {output};
    AudioBusBuffers inputBus{}; inputBus.numChannels = 1; inputBus.channelBuffers32 = in;
    AudioBusBuffers outputBus{}; outputBus.numChannels = 1; outputBus.channelBuffers32 = out;
    CapturedChanges changes;
    ProcessContext context{}; context.sampleRate = 48000; context.state = ProcessContext::kPlaying;
    ProcessData data{}; data.numInputs = data.numOutputs = 1; data.inputs = &inputBus; data.outputs = &outputBus;
    data.numSamples = 512; data.symbolicSampleSize = kSample32; data.outputParameterChanges = &changes; data.processContext = &context;
    bool finite = true;
    for (int block = 0; block < 35; ++block) {
        context.projectTimeSamples = block * 512;
        finite &= processor.process(data) == kResultOk;
        for (const auto& [id, queue] : changes.values) {
            finite &= std::isfinite(queue.value) && queue.value >= 0.0 && queue.value <= 1.0;
            controller.setParamNormalized(id, queue.value);
        }
    }
    check(finite && changes.values.size() == 6, "processor sends finite, normalized live telemetry and latency through output parameter queues");
    const double expectedPeak = (20.0 * std::log10(0.25) + 96.0) / 108.0;
    check(std::abs(changes.values.at(kParamInputPeak).value - expectedPeak) < 1e-6
        && std::abs(changes.values.at(kParamOutputPeak).value - expectedPeak) < 1e-6,
        "meters report actual input and audible output amplitudes");
    check(handler.restarts == 1 && (handler.flags & kLatencyChanged), "controller requests host latency recalculation exactly once for initial latency");
    // Turning on the queue changes latency during an active session.
    automate(processor, kParamAdvancedBase + (ParamID)TelephonyDSP::AdvancedControl::NetworkEnabled, 1.0);
    changes.values.clear(); context.projectTimeSamples += 512;
    check(processor.process(data) == kResultOk && changes.values.count(kParamLatencySamples), "advanced queue latency change reaches the controller");
    controller.setParamNormalized(kParamLatencySamples, changes.values.at(kParamLatencySamples).value);
    check(handler.restarts == 2 && processor.getLatencySamples() > 7200, "advanced queue updates reported host latency");
    // A seek also clears latency-compensated bypass, avoiding stale dry audio.
    automate(processor, kParamMasterBypass, 1.0);
    for (int block = 0; block < 30; ++block) { context.projectTimeSamples += 512; processor.process(data); }
    context.projectTimeSamples = 0;
    processor.process(data);
    check(std::all_of(output, output + 512, [](float sample) { return sample == 0.0f; }), "host timeline seek clears bypass delay together with DSP");
    // Quarter notes are converted to seconds, never milliseconds.
    context.sampleRate = 0; context.state |= ProcessContext::kProjectTimeMusicValid | ProcessContext::kTempoValid;
    context.projectTimeMusic = 4; context.tempo = 120;
    check(processor.process(data) == kResultOk, "valid music/tempo fallback accepts quarter-note host timing");
    processor.setActive(false); controller.setComponentHandler(nullptr); controller.terminate(); processor.terminate();
}
static void disconnectedInput() {
    for (bool bypass : {false, true}) {
        TelephonyVoiceProcessor processor; processor.initialize(nullptr);
        SpeakerArrangement mono = SpeakerArr::kMono, stereo = SpeakerArr::kStereo;
        processor.setBusArrangements(&mono, 1, &stereo, 1);
        ProcessSetup setup{}; setup.sampleRate = 48000; setup.maxSamplesPerBlock = 512; setup.symbolicSampleSize = kSample32;
        processor.setupProcessing(setup);
        automate(processor, kParamOutputEndpoint, 0.0);
        automate(processor, kParamDryWet, 0.0);
        automate(processor, kParamMasterBypass, bypass ? 1.0 : 0.0);
        processor.setActive(true);
        float input[512], left[512], right[512]; std::fill_n(input, 512, 0.5f);
        float* in[] = {input}; float* out[] = {left, right};
        AudioBusBuffers inputBus{}; inputBus.numChannels = 1; inputBus.channelBuffers32 = in;
        AudioBusBuffers outputBus{}; outputBus.numChannels = 2; outputBus.channelBuffers32 = out;
        CapturedChanges changes;
        ProcessData data{}; data.numSamples = 512; data.symbolicSampleSize = kSample32;
        data.numInputs = data.numOutputs = 1; data.inputs = &inputBus; data.outputs = &outputBus;
        data.outputParameterChanges = &changes;
        for (int block = 0; block < 35; ++block) processor.process(data);
        check(left[511] == 0.5f && right[511] == 0.5f, "disconnect regression first primes real audible delay history");
        data.numInputs = 0; data.inputs = nullptr;
        bool silent = true;
        for (int block = 0; block < 3; ++block) {
            std::fill_n(left, 512, -99.0f); std::fill_n(right, 512, 99.0f); outputBus.silenceFlags = 0;
            changes.values.clear();
            silent &= processor.process(data) == kResultOk && outputBus.silenceFlags == 3;
            silent &= std::all_of(left, left + 512, [](float value) { return value == 0.0f; })
                && std::all_of(right, right + 512, [](float value) { return value == 0.0f; });
            silent &= changes.values.count(kParamInputPeak) && changes.values.count(kParamOutputPeak)
                && changes.values.at(kParamInputPeak).value == 0.0 && changes.values.at(kParamOutputPeak).value == 0.0;
        }
        check(silent, "missing input clears prefilled outputs, sets silence flags and publishes silent peaks in wet and bypass modes");
        data.numInputs = 1; data.inputs = &inputBus; std::fill_n(input, 512, 0.75f);
        bool reprimes = true; const int latency = (int)processor.getLatencySamples();
        for (int block = 0; block < 35; ++block) {
            reprimes &= processor.process(data) == kResultOk;
            for (int sample = 0; sample < 512; ++sample) {
                const float expected = block * 512 + sample < latency ? 0.0f : 0.75f;
                reprimes &= left[sample] == expected && right[sample] == expected;
            }
        }
        check(reprimes, "input reconnection reprimes wet and bypass latency without replaying pre-disconnect samples");
        data.numInputs = data.numOutputs = 0; data.inputs = data.outputs = nullptr;
        check(processor.process(data) == kResultOk, "missing output remains a safe non-audio host flush");
        processor.setActive(false); processor.terminate();
    }
}
int main() { stateAndParameters(); audioProcessing(); stereoDownmix(); telemetryAndTransport(); disconnectedInput(); return failures ? 1 : 0; }
