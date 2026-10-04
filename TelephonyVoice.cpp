#include "TelephonyVoice.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/base/ustring.h"
#include "base/source/fstreamer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>

// Define string macro for VST3 (UTF-16)
#ifndef STR16
#if defined(_WIN32) || defined(_WIN64)
#define STR16(x) L##x
#else
#define STR16(x) u##x
#endif
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace Steinberg {
namespace Vst {

#ifdef TELEPHONY_DISTRIBUTION_BUILD
constexpr int32 kMaxExposedEndpoint = 2;
constexpr int32 kDefaultOutputEndpoint = 2;
#else
#if TELEPHONY_USE_EVS_JBM
constexpr int32 kMaxExposedEndpoint = 6;
#else
constexpr int32 kMaxExposedEndpoint = 5;
#endif
constexpr int32 kDefaultOutputEndpoint = 5;
#endif
constexpr int32 kMaxDegradationSegment = 3;
constexpr int32 kOpusEndpoint = 100;
#if defined(TELEPHONY_DISTRIBUTION_BUILD) || !TELEPHONY_EXPERIMENTAL_NETWORK
constexpr int32 kModernMaxEndpoint = kMaxExposedEndpoint; // Opus is excluded from this build.
#else
constexpr int32 kModernMaxEndpoint = kMaxExposedEndpoint + 1;
#endif
constexpr uint32 kStateExtensionMagic = 0x58415654; // TVAX, little endian
constexpr uint32 kStateExtensionVersion = 1;
constexpr double kLatencyNormalization = 384000.0;
static int32 validEndpoint(int32 endpoint) {
    if (endpoint == kOpusEndpoint) {
#if !defined(TELEPHONY_DISTRIBUTION_BUILD) && TELEPHONY_EXPERIMENTAL_NETWORK
        return endpoint;
#elif defined(TELEPHONY_DISTRIBUTION_BUILD)
        return 2;
#else
        return 4;
#endif
    }
    return std::clamp(endpoint, 0, kMaxExposedEndpoint);
}
static int32 legacyEndpoint(int32 endpoint) {
#ifdef TELEPHONY_DISTRIBUTION_BUILD
    return endpoint == kOpusEndpoint ? 2 : validEndpoint(endpoint);
#else
    return endpoint == kOpusEndpoint ? 4 : validEndpoint(endpoint);
#endif
}
static int32 modernEndpointIndex(int32 endpoint) {
    return endpoint == kOpusEndpoint ? kModernMaxEndpoint : validEndpoint(endpoint);
}
static int32 endpointFromModernIndex(int32 index) {
#if !defined(TELEPHONY_DISTRIBUTION_BUILD) && TELEPHONY_EXPERIMENTAL_NETWORK
    if (index == kModernMaxEndpoint) return kOpusEndpoint;
#endif
    return validEndpoint(index);
}

constexpr int32 kNumEvsSampleRates = 4;
constexpr int32 kDefaultEvsSampleRateIndex = 2; // 32 kHz (SWB)
constexpr int32 kEvsSampleRateValues[kNumEvsSampleRates] = { 8000, 16000, 32000, 48000 };
static const char* kEvsSampleRateStrings[kNumEvsSampleRates] = {
    "8 kHz (NB)", "16 kHz (WB)", "32 kHz (SWB)", "48 kHz (FB)"
};

constexpr int32 kNumEvsBitrates = 12;
constexpr int32 kDefaultEvsBitrateIndex = 4; // 13.2 kbps
constexpr int32 kEvsBitrateValues[kNumEvsBitrates] = {
    5900, 7200, 8000, 9600, 13200, 16400, 24400, 32000, 48000, 64000, 96000, 128000
};
static const char* kEvsBitrateStrings[kNumEvsBitrates] = {
    "5.9 kbps", "7.2 kbps", "8.0 kbps", "9.6 kbps", "13.2 kbps", "16.4 kbps",
    "24.4 kbps", "32 kbps", "48 kbps", "64 kbps", "96 kbps", "128 kbps"
};

constexpr int32 kNumEvsMaxBws = 4;
constexpr int32 kDefaultEvsMaxBwIndex = 2; // SWB
constexpr int32 kEvsMaxBwValues[kNumEvsMaxBws] = {
    (int32)EVS_NB, (int32)EVS_WB, (int32)EVS_SWB, (int32)EVS_FB
};
static const char* kEvsMaxBwStrings[kNumEvsMaxBws] = {
    "NB", "WB", "SWB", "FB"
};

// Opus OPUS_BANDWIDTH_* cap exposed as a user-facing parameter. The
// values mirror opus_defines.h verbatim so they can be passed straight
// into opus_encoder_ctl(OPUS_SET_MAX_BANDWIDTH) downstream.
constexpr int32 kNumOpusBandwidths = 5;
constexpr int32 kDefaultOpusBandwidthIndex = 4; // FB (20 kHz)
constexpr int32 kOpusBandwidthValues[kNumOpusBandwidths] = {
    1101, // OPUS_BANDWIDTH_NARROWBAND      (4 kHz)
    1102, // OPUS_BANDWIDTH_MEDIUMBAND      (6 kHz)
    1103, // OPUS_BANDWIDTH_WIDEBAND        (8 kHz)
    1104, // OPUS_BANDWIDTH_SUPERWIDEBAND   (12 kHz)
    1105  // OPUS_BANDWIDTH_FULLBAND        (20 kHz, default)
};
static const char* kOpusBandwidthStrings[kNumOpusBandwidths] = {
    "NB (4 kHz)", "MB (6 kHz)", "WB (8 kHz)", "SWB (12 kHz)", "FB (20 kHz)"
};

// G.711 law: index 0 = mu-law, index 1 = A-law.
constexpr int32 kNumG711Laws = 2;
constexpr int32 kDefaultG711LawIndex = 0; // mu-law

constexpr int32 kDefaultEvsSampleRate = 32000;
constexpr int32 kDefaultEvsBitrate = 13200;
constexpr int32 kDefaultEvsMaxBw = (int32)EVS_SWB;
constexpr int32 kDefaultOpusBandwidth = 1105; // OPUS_BANDWIDTH_FULLBAND

constexpr int32 kNumAmrNbModes = 8;
constexpr int32 kDefaultAmrNbModeIndex = 7; // 12.2 kbps (MR122)
static const char* kAmrNbModeStrings[kNumAmrNbModes] = {
    "4.75 kbps", "5.15 kbps", "5.9 kbps", "6.7 kbps",
    "7.4 kbps", "7.95 kbps", "10.2 kbps", "12.2 kbps"
};

constexpr int32 kNumAmrWbModes = 9;
constexpr int32 kDefaultAmrWbModeIndex = 2; // 12.65 kbps

// Opus target bitrate choices in bps; passed straight into
// OpusCodec::setBitrate() via SignalProcessor::setOpusBitrate().
constexpr int32 kNumOpusBitrates = 12;
constexpr int32 kDefaultOpusBitrateIndex = 4; // 24 kbps (OpusCodec default)
constexpr int32 kOpusBitrateValues[kNumOpusBitrates] = {
    6000, 8000, 12000, 16000, 24000, 32000, 48000, 64000,
    96000, 128000, 192000, 256000
};

// Map a plain value back to its normalized position in a choice table.
// Used by setComponentState() to restore StringListParameter positions
// from the plain values persisted in the processor state.
static double normalizedFromTable(int32 value, const int32* table, int32 count, int32 defaultIndex)
{
    int32 idx = defaultIndex;
    for (int32 i = 0; i < count; ++i) {
        if (table[i] == value) { idx = i; break; }
    }
    return (double)idx / (double)(count - 1);
}

// Human-readable choices share the descriptor's normalized/physical mapping.
static const char* advancedChoiceLabel(size_t index, int choice) {
    static const char* binary[] = {"Off", "On"};
    static const char* jitter[] = {"Uniform", "Gamma", "Weibull", "Pareto"};
    static const char* vad[] = {"VAD1 / energy", "3GPP VAD2"};
    static const char* digits[] = {"Off", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#", "A", "B", "C", "D"};
    static const char* durations[] = {"2.5 ms", "5 ms", "10 ms", "20 ms", "40 ms", "60 ms"};
    static const char* payloads[] = {"Auto", "Compact", "Header-full"};
    static const char* modes[] = {"Auto", "SILK", "Hybrid", "CELT"};
    static const char* ioRates[] = {"6.60 kbps", "8.85 kbps", "12.65 kbps", "14.25 kbps", "15.85 kbps", "18.25 kbps", "19.85 kbps", "23.05 kbps", "23.85 kbps"};
    const char** labels = nullptr; int count = 0;
    if (index == 4) { labels = jitter; count = 4; }
    else if (index == 20) { labels = vad; count = 2; }
    else if (index == 21) { labels = digits; count = 17; }
    else if (index == 26) { labels = durations; count = 6; }
    else if (index == 47) { labels = payloads; count = 3; }
    else if (index == 50) { labels = modes; count = 4; }
    else if (index == 52) { labels = ioRates; count = 9; }
    else if (TelephonyDSP::advancedDescriptors[index].steps == 1) { labels = binary; count = 2; }
    return labels && choice >= 0 && choice < count ? labels[choice] : nullptr;
}
class AdvancedParameter final : public RangeParameter {
public:
    AdvancedParameter(const TChar* title, size_t index, const TChar* units)
        : RangeParameter(title, kParamAdvancedBase + (ParamID)index, units,
            TelephonyDSP::advancedDescriptors[index].minimum, TelephonyDSP::advancedDescriptors[index].maximum,
            TelephonyDSP::advancedDescriptors[index].initial, TelephonyDSP::advancedDescriptors[index].steps,
            ParameterInfo::kCanAutomate | (TelephonyDSP::advancedDescriptors[index].steps > 0
                && TelephonyDSP::advancedDescriptors[index].steps <= 16 ? ParameterInfo::kIsList : 0)), index(index) {}
    void toString(ParamValue normalized, String128 text) const override {
        const auto& descriptor = TelephonyDSP::advancedDescriptors[index];
        if (const char* label = advancedChoiceLabel(index, (int)std::lround(normalized * descriptor.steps)))
            UString(text, 128).fromAscii(label);
        else RangeParameter::toString(normalized, text);
    }
    bool fromString(const TChar* text, ParamValue& normalized) const override {
        char ascii[128]{}; UString(const_cast<TChar*>(text), 128).toAscii(ascii, 128);
        const auto& descriptor = TelephonyDSP::advancedDescriptors[index];
        for (int choice = 0; choice <= descriptor.steps; ++choice)
            if (const char* label = advancedChoiceLabel(index, choice))
                if (std::strcmp(ascii, label) == 0) { normalized = double(choice) / descriptor.steps; return true; }
        return RangeParameter::fromString(text, normalized);
    }
private:
    size_t index;
};

// The byte layout is deliberately append-only. Historical presets may end at
// any complete trailing field; partial fields are corrupt, not older presets.
struct SavedState {
    int32 eraMode = 0;
    float dry = 0.5f, gain = 0.0f;
    bool art = false;
    float amount = 0.0f;
    bool bypass = false;
    int32 outputEndpoint = kDefaultOutputEndpoint, segment = 0;
    float packetLoss = 0.0f, degradation = 0.0f;
    int32 evsSampleRate = kDefaultEvsSampleRate, evsBitrate = kDefaultEvsBitrate;
    int32 evsMaxBw = kDefaultEvsMaxBw, opusBandwidth = kDefaultOpusBandwidth;
    int32 amrNbMode = kDefaultAmrNbModeIndex, sidInterval = 0;
    int32 amrWbMode = kDefaultAmrWbModeIndex, g711Law = 0, scVbr = 0;
    int32 opusBitrate = kOpusBitrateValues[kDefaultOpusBitrateIndex];
    TelephonyDSP::AdvancedSettings advanced;
};

static int32 tableValueOrDefault(int32 value, const int32* table, int32 count, int32 defaultIndex)
{
    return std::find(table, table + count, value) != table + count ? value : table[defaultIndex];
}

static bool readSavedState(IBStream* state, SavedState& value)
{
    if (!state) return false;
    IBStreamer stream(state, kLittleEndian);
    if (!stream.readInt32(value.eraMode) || !stream.readFloat(value.dry)
        || !stream.readFloat(value.gain) || !stream.readBool(value.art)
        || !stream.readFloat(value.amount) || !stream.readBool(value.bypass)) return false;

    bool ended = false;
    auto trailing = [&](auto& field) {
        if (ended) return true;
        uint8_t bytes[4]{};
        const auto count = stream.readRaw(bytes, sizeof(bytes));
        if (count == 0) { ended = true; return true; }
        if (count != sizeof(bytes)) return false;
        const uint32_t bits = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8)
            | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
        static_assert(sizeof(field) == sizeof(bits));
        std::memcpy(&field, &bits, sizeof(field));
        return true;
    };
    if (!trailing(value.outputEndpoint) || !trailing(value.segment)
        || !trailing(value.packetLoss) || !trailing(value.degradation)
        || !trailing(value.evsSampleRate) || !trailing(value.evsBitrate)
        || !trailing(value.evsMaxBw) || !trailing(value.opusBandwidth)
        || !trailing(value.amrNbMode) || !trailing(value.sidInterval)
        || !trailing(value.amrWbMode) || !trailing(value.g711Law)
        || !trailing(value.scVbr) || !trailing(value.opusBitrate)) return false;

    if (!ended) {
        uint32 magic = 0;
        const auto bytes = stream.readRaw(&magic, sizeof(magic));
        if (bytes != 0) {
            // Read header using the same endian-aware streamer as numeric fields.
            if (bytes != sizeof(magic)) return false;
#if BYTEORDER == kBigEndian
            magic = ((magic & 0xffu) << 24) | ((magic & 0xff00u) << 8)
                  | ((magic & 0xff0000u) >> 8) | ((magic >> 24) & 0xffu);
#endif
            uint32 version = 0, count = 0;
            if (magic != kStateExtensionMagic || !stream.readInt32u(version)
                || version != kStateExtensionVersion || !stream.readInt32u(count)
                || count > value.advanced.normalized.size()
                || !stream.readInt32(value.eraMode) || !stream.readInt32(value.outputEndpoint)) return false;
            for (uint32 index = 0; index < count; ++index) {
                auto& normalized = value.advanced.normalized[index];
                if (!stream.readDouble(normalized) || !std::isfinite(normalized)
                    || normalized < 0.0 || normalized > 1.0) return false;
            }
            uint8 extra;
            if (stream.readRaw(&extra, 1) != 0) return false;
        }
    }

    if (!std::isfinite(value.dry) || !std::isfinite(value.gain)
        || !std::isfinite(value.amount) || !std::isfinite(value.packetLoss)
        || !std::isfinite(value.degradation)) return false;
    value.eraMode = validEndpoint(value.eraMode);
    value.outputEndpoint = validEndpoint(value.outputEndpoint);
    value.segment = std::clamp(value.segment, 0, kMaxDegradationSegment);
    value.dry = std::clamp(value.dry, 0.0f, 1.0f);
    value.gain = std::clamp(value.gain, -60.0f, 24.0f);
    value.amount = std::clamp(value.amount, 0.0f, 1.0f);
    value.packetLoss = std::clamp(value.packetLoss, 0.0f, 0.30f);
    value.degradation = std::clamp(value.degradation, 0.0f, 1.0f);
    value.evsSampleRate = tableValueOrDefault(value.evsSampleRate, kEvsSampleRateValues, kNumEvsSampleRates, kDefaultEvsSampleRateIndex);
    value.evsBitrate = tableValueOrDefault(value.evsBitrate, kEvsBitrateValues, kNumEvsBitrates, kDefaultEvsBitrateIndex);
    value.evsMaxBw = std::clamp(value.evsMaxBw, (int32)EVS_NB, (int32)EVS_FB);
    value.opusBandwidth = std::clamp(value.opusBandwidth, 1101, 1105);
    value.amrNbMode = std::clamp(value.amrNbMode, 0, kNumAmrNbModes - 1);
    value.amrWbMode = std::clamp(value.amrWbMode, 0, kNumAmrWbModes - 1);
    value.sidInterval = std::clamp(value.sidInterval, 0, 100);
    if (value.sidInterval < 3) value.sidInterval = 0;
    value.g711Law = std::clamp(value.g711Law, 0, 1);
    value.scVbr = std::clamp(value.scVbr, 0, 1);
    value.opusBitrate = tableValueOrDefault(value.opusBitrate, kOpusBitrateValues, kNumOpusBitrates, kDefaultOpusBitrateIndex);
    return true;
}

TelephonyDSP::RouteEndpoint endpointFromParameter(int32 endpoint)
{
#if !defined(TELEPHONY_DISTRIBUTION_BUILD) && TELEPHONY_EXPERIMENTAL_NETWORK
    if (endpoint == kOpusEndpoint) return TelephonyDSP::RouteEndpoint::Opus;
#endif
    endpoint = std::clamp(endpoint, 0, kMaxExposedEndpoint);
#ifdef TELEPHONY_DISTRIBUTION_BUILD
    switch (endpoint) {
        case 0: return TelephonyDSP::RouteEndpoint::FixedLine;
        case 1: return TelephonyDSP::RouteEndpoint::Mobile2G;
        case 2: return TelephonyDSP::RouteEndpoint::Mobile5G;
        default: return TelephonyDSP::RouteEndpoint::FixedLine;
    }
#else
    switch (endpoint) {
        case 0: return TelephonyDSP::RouteEndpoint::FixedLine;
        case 1: return TelephonyDSP::RouteEndpoint::Mobile2G;
        case 2: return TelephonyDSP::RouteEndpoint::Mobile3G;
        case 3: return TelephonyDSP::RouteEndpoint::Mobile4G;
        case 4: return TelephonyDSP::RouteEndpoint::Mobile5G;
        case 5: return TelephonyDSP::RouteEndpoint::Mobile5GNative;
#if TELEPHONY_USE_EVS_JBM
        case 6: return TelephonyDSP::RouteEndpoint::Mobile5GJbm;
#endif
        default: return TelephonyDSP::RouteEndpoint::FixedLine;
    }
#endif
}

TelephonyDSP::DegradationSegment degradationSegmentFromParameter(int32 segment)
{
    switch (std::clamp(segment, 0, kMaxDegradationSegment)) {
        case 0: return TelephonyDSP::DegradationSegment::Both;
        case 1: return TelephonyDSP::DegradationSegment::InputToExchange;
        case 2: return TelephonyDSP::DegradationSegment::ExchangeToOutput;
        case 3: return TelephonyDSP::DegradationSegment::None;
        default: return TelephonyDSP::DegradationSegment::Both;
    }
}

// Unique class IDs for this plugin. Changing these breaks plugin lookup
// in previously saved host projects, so they must stay stable from now on.
FUID TelephonyVoiceProcessor::uid(0x0DB311F2, 0x41851DD5, 0x85253F8B, 0x3D5EB34F);
FUID TelephonyVoiceController::uid(0xB8390C57, 0x44E3C17F, 0x44F35BB2, 0xA9E8274A);

// --------------------------------------------------------------------------
// Processor Implementation
// --------------------------------------------------------------------------
TelephonyVoiceProcessor::TelephonyVoiceProcessor()
    : currentEraMode(0)
    , currentOutputEndpoint(kDefaultOutputEndpoint)
    , currentDegradationSegment(0)
    , currentEvsSampleRate(kDefaultEvsSampleRate)
    , currentEvsBitrate(kDefaultEvsBitrate)
    , currentEvsMaxBw(kDefaultEvsMaxBw)
    , currentOpusBandwidth(kDefaultOpusBandwidth)
    , currentOpusBitrate(24000)
    , currentAmrNbMode(kDefaultAmrNbModeIndex)
    , currentAmrWbMode(2)
    , currentG711Law(0) // mu-law
    , currentEvsDtxSidInterval(0)
    , currentEvsScVbr(0)
    , currentDryWet(0.5f)
    , currentOutGain(0.0f)
    , currentArtifactsEnabled(false)
    , currentArtifactAmount(0.0f)
    , currentPacketLossRate(0.0f)
    , currentNetworkDegradation(0.0f)
    , currentBypass(false)
    , bypassDelayLen(0)
    , bypassDelayPos(0)
{
    setControllerClass(TelephonyVoiceController::uid);
    processContextRequirements.needTransportState().needProjectTimeMusic().needTempo();
}

TelephonyVoiceProcessor::~TelephonyVoiceProcessor() {}

tresult PLUGIN_API TelephonyVoiceProcessor::initialize(FUnknown* context)
{
    tresult result = AudioEffect::initialize(context);
    if (result != kResultOk) return result;

    addAudioInput(STR16("Stereo In"), SpeakerArr::kStereo);
    addAudioOutput(STR16("Stereo Out"), SpeakerArr::kStereo);

    return kResultOk;
}

tresult PLUGIN_API TelephonyVoiceProcessor::setBusArrangements(
    SpeakerArrangement* inputs, int32 numIns,
    SpeakerArrangement* outputs, int32 numOuts)
{
    if (inputs && outputs && numIns == 1 && numOuts == 1) {
        if ((inputs[0] == SpeakerArr::kMono || inputs[0] == SpeakerArr::kStereo) &&
            (outputs[0] == SpeakerArr::kMono || outputs[0] == SpeakerArr::kStereo)) 
        {
            return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
        }
    }
    return kResultFalse;
}

tresult PLUGIN_API TelephonyVoiceProcessor::setupProcessing(ProcessSetup& newSetup)
{
    if (!std::isfinite(newSetup.sampleRate) || newSetup.sampleRate < 8000.0
        || newSetup.sampleRate > 384000.0 || newSetup.maxSamplesPerBlock <= 0
        || canProcessSampleSize(newSetup.symbolicSampleSize) != kResultTrue)
        return kResultFalse;
    const auto result = AudioEffect::setupProcessing(newSetup);
    if (result != kResultOk) return result;
    dsp.setSampleRate(newSetup.sampleRate);
    dsp.setSimulateLatency(true); // VST uses latency
    return kResultOk;
}

tresult PLUGIN_API TelephonyVoiceProcessor::setActive(TBool state)
{
    if (state) {
        dsp.reset();
        updateDSPParameters();
        // Size the bypass delay line to the latency the host is told about
        // so bypassed audio stays time-aligned with processed audio.
        bypassDelayLen = (int)dsp.getLatencySamples();
        bypassDelayPos = 0;
        bypassDelayBuf.assign(2, std::vector<float>((size_t)std::max((int)processSetup.sampleRate, bypassDelayLen), 0.0f));
        reportedLatency = -1.0;
        transportTimeKnown = false;
    }
    return AudioEffect::setActive(state);
}

uint32 PLUGIN_API TelephonyVoiceProcessor::getLatencySamples()
{
    return (uint32)dsp.getLatencySamples();
}

tresult PLUGIN_API TelephonyVoiceProcessor::process(ProcessData& data)
{
    // 1. Process Parameter Changes
    if (data.inputParameterChanges) {
        int32 numParamsChanged = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < numParamsChanged; i++) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (queue) {
                ParamValue value;
                int32 sampleOffset;
                int32 numPoints = queue->getPointCount();
                if (numPoints > 0 && queue->getPoint(numPoints - 1, sampleOffset, value) == kResultOk
                    && std::isfinite(value)) {
                    value = std::clamp(value, 0.0, 1.0);
                    ParamID pid = queue->getParameterId();
                    if (pid >= kParamAdvancedBase && pid < kParamAdvancedBase + currentAdvanced.normalized.size()) {
                        currentAdvanced.normalized[pid - kParamAdvancedBase] = value;
                        continue;
                    }
                    switch (pid) {
                        case kParamInputRoute:
                            currentEraMode = endpointFromModernIndex((int32)std::lround(value * kModernMaxEndpoint));
                            break;
                        case kParamOutputRoute:
                            currentOutputEndpoint = endpointFromModernIndex((int32)std::lround(value * kModernMaxEndpoint));
                            break;
                        case kParamEraMode:
                            currentEraMode = std::clamp((int32)(value * kMaxExposedEndpoint + 0.5), 0, kMaxExposedEndpoint);
                            break;
                        case kParamOutputEndpoint:
                            currentOutputEndpoint = std::clamp((int32)(value * kMaxExposedEndpoint + 0.5), 0, kMaxExposedEndpoint);
                            break;
                        case kParamDegradationSegment:
                            currentDegradationSegment = std::clamp((int32)(value * kMaxDegradationSegment + 0.5), 0, kMaxDegradationSegment);
                            break;
                        case kParamEvsSampleRate: {
                            int32 idx = std::clamp((int32)(value * (kNumEvsSampleRates - 1) + 0.5), 0, kNumEvsSampleRates - 1);
                            currentEvsSampleRate = kEvsSampleRateValues[idx];
                            break;
                        }
                        case kParamEvsBitrate: {
                            int32 idx = std::clamp((int32)(value * (kNumEvsBitrates - 1) + 0.5), 0, kNumEvsBitrates - 1);
                            currentEvsBitrate = kEvsBitrateValues[idx];
                            break;
                        }
                        case kParamEvsMaxBw: {
                            int32 idx = std::clamp((int32)(value * (kNumEvsMaxBws - 1) + 0.5), 0, kNumEvsMaxBws - 1);
                            currentEvsMaxBw = kEvsMaxBwValues[idx];
                            break;
                        }
                        case kParamOpusBandwidth: {
                            int32 idx = std::clamp((int32)(value * (kNumOpusBandwidths - 1) + 0.5), 0, kNumOpusBandwidths - 1);
                            currentOpusBandwidth = kOpusBandwidthValues[idx];
                            break;
                        }
                        case kParamAmrNbMode: {
                            int32 idx = std::clamp((int32)(value * (kNumAmrNbModes - 1) + 0.5), 0, kNumAmrNbModes - 1);
                            currentAmrNbMode = idx;
                            break;
                        }
                        case kParamAmrWbMode: {
                            currentAmrWbMode = std::clamp((int32)(value * (kNumAmrWbModes - 1) + 0.5), 0, kNumAmrWbModes - 1);
                            break;
                        }
                        case kParamOpusBitrate: {
                            int32 idx = std::clamp((int32)(value * (kNumOpusBitrates - 1) + 0.5), 0, kNumOpusBitrates - 1);
                            currentOpusBitrate = kOpusBitrateValues[idx];
                            break;
                        }
                        case kParamG711Law:
                            currentG711Law = (value > 0.5) ? 1 : 0;
                            break;
                        case kParamEvsDtxSidInterval:
                            // Plain range is 0..100 frames; 1..2 are collapsed
                            // to 0 (variable SID) by ChannelProcessor.
                            currentEvsDtxSidInterval = std::clamp((int32)(value * 100.0 + 0.5), 0, 100);
                            break;
                        case kParamEvsScVbr:
                            currentEvsScVbr = (value > 0.5) ? 1 : 0;
                            break;
                        case kParamDryWet: currentDryWet = (float)value; break;
                        case kParamOutputGain: currentOutGain = (float)(value * 84.0 - 60.0); break;
                        case kParamArtifactsEnabled: currentArtifactsEnabled = (value > 0.5); break;
                        case kParamArtifactAmount: currentArtifactAmount = (float)value; break;
                        case kParamPacketLossRate: currentPacketLossRate = (float)value * 0.30f; break;
                        case kParamNetworkDegradation: currentNetworkDegradation = (float)value; break;
                        case kParamMasterBypass: currentBypass = (value > 0.5); break;
                    }
                }
            }
        }
        updateDSPParameters();
    }

    // VST3 projectTimeSamples and sampleRate are always valid when a context
    // exists. Music time is quarter notes, so fallback conversion uses 60/tempo.
    if (data.processContext) {
        const auto& context = *data.processContext;
        double seconds = 0.0;
        if (std::isfinite(context.sampleRate) && context.sampleRate > 0.0)
            seconds = double(context.projectTimeSamples) / context.sampleRate;
        else if ((context.state & ProcessContext::kProjectTimeMusicValid)
            && (context.state & ProcessContext::kTempoValid)
            && std::isfinite(context.projectTimeMusic) && std::isfinite(context.tempo) && context.tempo > 0.0)
            seconds = context.projectTimeMusic * 60.0 / context.tempo;
        else seconds = double(context.projectTimeSamples) / processSetup.sampleRate;
        const bool playing = (context.state & ProcessContext::kPlaying) != 0;
        if (currentAdvanced.enabled(TelephonyDSP::AdvancedControl::HostClock) && std::isfinite(seconds)) {
            if (transportTimeKnown && (playing || transportWasPlaying)
                && std::abs(seconds - expectedTransportTime) > 2.0 / processSetup.sampleRate) {
                bypassDelayPos = 0;
                for (auto& channel : bypassDelayBuf) std::fill(channel.begin(), channel.end(), 0.0f);
            }
            expectedTransportTime = seconds + std::max(0, data.numSamples) / processSetup.sampleRate;
            transportTimeKnown = true;
            transportWasPlaying = playing;
        } else transportTimeKnown = false;
        dsp.setTransportTime(seconds, playing);
    } else transportTimeKnown = false;

    // Hosts can flush parameters without supplying audio buses or samples.
    if (data.numSamples == 0) { publishTelemetry(data); return kResultOk; }
    if (data.numSamples < 0 || data.symbolicSampleSize != kSample32) return kResultFalse;
    if (data.numOutputs == 0) return kResultOk;
    if (data.numOutputs < 0 || data.numInputs < 0 || !data.outputs) return kResultFalse;

    const int32 numOutCh = data.outputs[0].numChannels;
    float** out = data.outputs[0].channelBuffers32;
    if (!out || numOutCh < 1 || numOutCh > 2) return kResultFalse;
    for (int ch = 0; ch < numOutCh; ++ch) if (!out[ch]) return kResultFalse;

    if (data.numInputs == 0) {
        // A disconnected input is not a parameter-only flush: the host still
        // owns an audible output buffer. Clear it and discard both wet and dry
        // history so reconnecting cannot replay audio from before disconnect.
        for (int ch = 0; ch < numOutCh; ++ch) std::fill_n(out[ch], data.numSamples, 0.0f);
        data.outputs[0].silenceFlags = (uint64(1) << numOutCh) - 1;
        dsp.reset();
        bypassDelayPos = 0;
        for (auto& channel : bypassDelayBuf) std::fill(channel.begin(), channel.end(), 0.0f);
        transportTimeKnown = false;
        publishTelemetry(data, out, numOutCh);
        return kResultOk;
    }
    if (!data.inputs) return kResultFalse;
    const int32 numInCh = data.inputs[0].numChannels;
    float** in = data.inputs[0].channelBuffers32;
    if (!in || numInCh < 1 || numInCh > 2) return kResultFalse;
    for (int ch = 0; ch < numInCh; ++ch) if (!in[ch]) return kResultFalse;

    // Silence flags describe input only: codec/delay tails can still be audible.
    data.outputs[0].silenceFlags = 0;
    std::vector<float> silence;
    float* source[2] = { in[0], in[numInCh - 1] };
    if (data.inputs[0].silenceFlags != 0) {
        silence.assign(data.numSamples, 0.0f);
        for (int ch = 0; ch < numInCh; ++ch)
            if (data.inputs[0].silenceFlags & (uint64(1) << ch)) source[ch] = silence.data();
    }

    const int delayLen = bypassDelayLen;
    const int maxDelayCh = (std::min)((int)numOutCh, (int)bypassDelayBuf.size());
    if (currentBypass) {
        // Keep codec state and delay tails moving while bypassed. Otherwise
        // re-enabling the effect can replay audio from before the bypass.
        std::vector<std::vector<float>> wet(numOutCh, std::vector<float>(data.numSamples));
        float* wetOut[2] = { wet[0].data(), wet[numOutCh - 1].data() };
        dsp.process(source, numInCh, wetOut, numOutCh, data.numSamples);
        for (int32 i = 0; i < data.numSamples; ++i) {
            // Snapshot all inputs before any output write, including a mono
            // input aliased to the first of two output channels.
            const float inputSamples[2] = { source[0][i], source[numInCh - 1][i] };
            for (int ch = 0; ch < numOutCh; ++ch) {
                const float sample = numOutCh == 1 && numInCh == 2
                    ? (inputSamples[0] + inputSamples[1]) * 0.5f : inputSamples[ch % numInCh];
                const float x = std::isfinite(sample) ? sample : 0.0f;
                if (delayLen <= 0 || ch >= maxDelayCh) out[ch][i] = x;
                else {
                    out[ch][i] = bypassDelayBuf[ch][bypassDelayPos];
                    bypassDelayBuf[ch][bypassDelayPos] = x;
                }
            }
            if (delayLen > 0 && ++bypassDelayPos == delayLen) bypassDelayPos = 0;
        }
        publishTelemetry(data, out, numOutCh);
        return kResultOk;
    }

    // Feed dry delay before DSP writes potentially in-place output.
    if (delayLen > 0) {
        for (int ch = 0; ch < maxDelayCh; ++ch) {
            const float* src = source[ch % numInCh];
            float* ring = bypassDelayBuf[ch].data();
            int pos = bypassDelayPos;
            for (int32 i = 0; i < data.numSamples; ++i) {
                const float sample = numOutCh == 1 && numInCh == 2
                    ? (source[0][i] + source[1][i]) * 0.5f : src[i];
                ring[pos] = std::isfinite(sample) ? sample : 0.0f;
                if (++pos == delayLen) pos = 0;
            }
        }
        bypassDelayPos = (int)((int64(bypassDelayPos) + data.numSamples) % delayLen);
    }
    dsp.process(source, numInCh, out, numOutCh, data.numSamples);
    publishTelemetry(data, out, numOutCh);

    return kResultOk;
}

void TelephonyVoiceProcessor::updateDSPParameters()
{
    currentEraMode = validEndpoint(currentEraMode);
    currentOutputEndpoint = validEndpoint(currentOutputEndpoint);
    currentDegradationSegment = std::clamp(currentDegradationSegment, 0, kMaxDegradationSegment);
    currentEvsMaxBw = std::clamp(currentEvsMaxBw, (int32)EVS_NB, (int32)EVS_FB);
    currentOpusBandwidth = std::clamp(currentOpusBandwidth, 1101, 1105);
    currentAmrNbMode = std::clamp(currentAmrNbMode, 0, kNumAmrNbModes - 1);
    currentAmrWbMode = std::clamp(currentAmrWbMode, 0, 8);
    currentG711Law = std::clamp(currentG711Law, 0, 1);
    currentEvsDtxSidInterval = std::clamp(currentEvsDtxSidInterval, 0, 100);
    currentEvsScVbr = std::clamp(currentEvsScVbr, 0, 1);
    currentOpusBitrate = std::clamp(currentOpusBitrate, 6000, 510000);
    dsp.setRoute(endpointFromParameter(currentEraMode),
                 endpointFromParameter(currentOutputEndpoint),
                 degradationSegmentFromParameter(currentDegradationSegment));
    dsp.setParameters(currentDryWet, currentOutGain, currentArtifactsEnabled, currentArtifactAmount,
                      currentPacketLossRate, currentNetworkDegradation);
    dsp.setEVSConfig(currentEvsSampleRate, currentEvsBitrate, (EVS_Bandwidth)currentEvsMaxBw);
    dsp.setOpusBandwidth(currentOpusBandwidth);
    dsp.setOpusBitrate(currentOpusBitrate);
    dsp.setAmrNbMode(currentAmrNbMode);
    dsp.setAmrWbMode(currentAmrWbMode);
    dsp.setG711Law(currentG711Law);
    dsp.setEvsDtxSidInterval(currentEvsDtxSidInterval);
    dsp.setEvsScVbrEnabled(currentEvsScVbr != 0);
    dsp.setAdvancedSettings(currentAdvanced);
    const int latency = (int)dsp.getLatencySamples();
    if (!bypassDelayBuf.empty() && latency != bypassDelayLen
        && latency <= (int)bypassDelayBuf.front().size()) {
        bypassDelayLen = latency;
        bypassDelayPos = 0;
        for (auto& channel : bypassDelayBuf) std::fill(channel.begin(), channel.end(), 0.0f);
    }
}

tresult PLUGIN_API TelephonyVoiceProcessor::setState(IBStream* state)
{
    SavedState value;
    if (!readSavedState(state, value)) return kResultFalse;
    currentEraMode = value.eraMode;
    currentDryWet = value.dry; currentOutGain = value.gain;
    currentArtifactsEnabled = value.art; currentArtifactAmount = value.amount;
    currentBypass = value.bypass;
    currentOutputEndpoint = value.outputEndpoint;
    currentDegradationSegment = value.segment;
    currentPacketLossRate = value.packetLoss;
    currentNetworkDegradation = value.degradation;
    currentEvsSampleRate = value.evsSampleRate;
    currentEvsBitrate = value.evsBitrate;
    currentEvsMaxBw = value.evsMaxBw;
    currentOpusBandwidth = value.opusBandwidth;
    currentAmrNbMode = value.amrNbMode;
    currentEvsDtxSidInterval = value.sidInterval;
    currentAmrWbMode = value.amrWbMode;
    currentG711Law = value.g711Law;
    currentEvsScVbr = value.scVbr;
    currentOpusBitrate = value.opusBitrate;
    currentAdvanced = value.advanced;
    updateDSPParameters();
    return kResultOk;
}

tresult PLUGIN_API TelephonyVoiceProcessor::getState(IBStream* state)
{
    if (!state) return kResultFalse;
    IBStreamer streamer(state, kLittleEndian);
    if (!streamer.writeInt32(legacyEndpoint(currentEraMode))) return kResultFalse;
    if (!streamer.writeFloat(currentDryWet)) return kResultFalse;
    if (!streamer.writeFloat(currentOutGain)) return kResultFalse;
    if (!streamer.writeBool(currentArtifactsEnabled)) return kResultFalse;
    if (!streamer.writeFloat(currentArtifactAmount)) return kResultFalse;
    if (!streamer.writeBool(currentBypass)) return kResultFalse;
    if (!streamer.writeInt32(legacyEndpoint(currentOutputEndpoint))) return kResultFalse;
    if (!streamer.writeInt32(currentDegradationSegment)) return kResultFalse;
    if (!streamer.writeFloat(currentPacketLossRate)) return kResultFalse;
    if (!streamer.writeFloat(currentNetworkDegradation)) return kResultFalse;
    if (!streamer.writeInt32(currentEvsSampleRate)) return kResultFalse;
    if (!streamer.writeInt32(currentEvsBitrate)) return kResultFalse;
    if (!streamer.writeInt32(currentEvsMaxBw)) return kResultFalse;
    if (!streamer.writeInt32(currentOpusBandwidth)) return kResultFalse;
    if (!streamer.writeInt32(currentAmrNbMode)) return kResultFalse;
    if (!streamer.writeInt32(currentEvsDtxSidInterval)) return kResultFalse;
    if (!streamer.writeInt32(currentAmrWbMode)) return kResultFalse;
    if (!streamer.writeInt32(currentG711Law)) return kResultFalse;
    if (!streamer.writeInt32(currentEvsScVbr)) return kResultFalse;
    if (!streamer.writeInt32(currentOpusBitrate)) return kResultFalse;
    if (!streamer.writeInt32u(kStateExtensionMagic) || !streamer.writeInt32u(kStateExtensionVersion)
        || !streamer.writeInt32u((uint32)currentAdvanced.normalized.size())
        || !streamer.writeInt32(currentEraMode) || !streamer.writeInt32(currentOutputEndpoint)) return kResultFalse;
    for (double normalized : currentAdvanced.normalized)
        if (!streamer.writeDouble(normalized)) return kResultFalse;
    return kResultOk;
}

void TelephonyVoiceProcessor::publishTelemetry(ProcessData& data, float** output, int channels)
{
    if (!data.outputParameterChanges) return;
    const auto telemetry = dsp.getTelemetry();
    const auto publish = [&](ParamID id, double value) {
        int32 index = 0;
        if (auto* queue = data.outputParameterChanges->addParameterData(id, index))
            return queue->addPoint(std::max(0, data.numSamples - 1), std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0, index) == kResultOk;
        return false;
    };
    const auto peakNormalized = [](double peak) {
        return (std::clamp(20.0 * std::log10(std::max(peak, 1e-12)), -96.0, 12.0) + 96.0) / 108.0;
    };
    double outputPeak = telemetry.outputPeak;
    if (output) {
        outputPeak = 0.0;
        for (int channel = 0; channel < channels; ++channel)
            for (int32 sample = 0; sample < data.numSamples; ++sample)
                outputPeak = std::max(outputPeak, double(std::abs(output[channel][sample])));
    }
    publish(kParamInputPeak, peakNormalized(telemetry.inputPeak));
    publish(kParamOutputPeak, peakNormalized(outputPeak));
    publish(kParamMeasuredLoss, telemetry.measuredLoss);
    publish(kParamMeasuredJitter, telemetry.jitterMs / 1000.0);
    publish(kParamOpusActualMode, double(std::clamp(telemetry.opusMode, 0, 3)) / 3.0);
    const double latency = double(dsp.getLatencySamples()) / kLatencyNormalization;
    if (latency != reportedLatency) {
        if (publish(kParamLatencySamples, latency)) reportedLatency = latency;
    }
}

// --------------------------------------------------------------------------
// Controller Implementation
// --------------------------------------------------------------------------
TelephonyVoiceController::TelephonyVoiceController() {}
TelephonyVoiceController::~TelephonyVoiceController() {}

tresult PLUGIN_API TelephonyVoiceController::initialize(FUnknown* context)
{
    tresult result = EditController::initialize(context);
    if (result != kResultOk) return result;

    auto appendEndpointStrings = [](StringListParameter* param) {
        param->appendString(STR16("\u56fa\u5b9a\u96fb\u8a71"));
        param->appendString(STR16("2G\u643a\u5e2f"));
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        param->appendString(STR16("3G\u643a\u5e2f"));
        param->appendString(STR16("4G\u643a\u5e2f"));
#endif
        param->appendString(STR16("5G\u643a\u5e2f"));
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        param->appendString(STR16("5G\u643a\u5e2f (\u7cbe\u5bc6)"));
#if TELEPHONY_USE_EVS_JBM
        param->appendString(STR16("5G\u643a\u5e2f (JBM)"));
#endif
#endif
    };

    StringListParameter* inputParam = new StringListParameter(STR16("Input (legacy automation)"), kParamEraMode, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList | ParameterInfo::kIsHidden);
    appendEndpointStrings(inputParam);
    inputParam->setNormalized(0.0);
    parameters.addParameter(inputParam);

    StringListParameter* outputParam = new StringListParameter(STR16("Output (legacy automation)"), kParamOutputEndpoint, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList | ParameterInfo::kIsHidden);
    appendEndpointStrings(outputParam);
    outputParam->setNormalized((double)kDefaultOutputEndpoint / (double)kMaxExposedEndpoint);
    parameters.addParameter(outputParam);

    for (ParamID id : {ParamID(kParamInputRoute), ParamID(kParamOutputRoute)}) {
        auto* route = new StringListParameter(id == kParamInputRoute ? STR16("Input Route") : STR16("Output Route"),
            id, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
        appendEndpointStrings(route);
#if !defined(TELEPHONY_DISTRIBUTION_BUILD) && TELEPHONY_EXPERIMENTAL_NETWORK
        route->appendString(STR16("Opus VoIP"));
#endif
        route->setNormalized(id == kParamInputRoute ? 0.0 : double(kDefaultOutputEndpoint) / kModernMaxEndpoint);
        parameters.addParameter(route);
    }

    StringListParameter* evsSampleRateParam = new StringListParameter(STR16("EVS Sample Rate"), kParamEvsSampleRate, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    evsSampleRateParam->appendString(STR16("8 kHz (NB)"));
    evsSampleRateParam->appendString(STR16("16 kHz (WB)"));
    evsSampleRateParam->appendString(STR16("32 kHz (SWB)"));
    evsSampleRateParam->appendString(STR16("48 kHz (FB)"));
    evsSampleRateParam->setNormalized((double)kDefaultEvsSampleRateIndex / (double)(kNumEvsSampleRates - 1));
    parameters.addParameter(evsSampleRateParam);

    StringListParameter* evsBitrateParam = new StringListParameter(STR16("EVS Bitrate"), kParamEvsBitrate, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    evsBitrateParam->appendString(STR16("5.9 kbps"));
    evsBitrateParam->appendString(STR16("7.2 kbps"));
    evsBitrateParam->appendString(STR16("8.0 kbps"));
    evsBitrateParam->appendString(STR16("9.6 kbps"));
    evsBitrateParam->appendString(STR16("13.2 kbps"));
    evsBitrateParam->appendString(STR16("16.4 kbps"));
    evsBitrateParam->appendString(STR16("24.4 kbps"));
    evsBitrateParam->appendString(STR16("32 kbps"));
    evsBitrateParam->appendString(STR16("48 kbps"));
    evsBitrateParam->appendString(STR16("64 kbps"));
    evsBitrateParam->appendString(STR16("96 kbps"));
    evsBitrateParam->appendString(STR16("128 kbps"));
    evsBitrateParam->setNormalized((double)kDefaultEvsBitrateIndex / (double)(kNumEvsBitrates - 1));
    parameters.addParameter(evsBitrateParam);

    StringListParameter* evsMaxBwParam = new StringListParameter(STR16("EVS Max Bandwidth"), kParamEvsMaxBw, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    evsMaxBwParam->appendString(STR16("NB"));
    evsMaxBwParam->appendString(STR16("WB"));
    evsMaxBwParam->appendString(STR16("SWB"));
    evsMaxBwParam->appendString(STR16("FB"));
    evsMaxBwParam->setNormalized((double)kDefaultEvsMaxBwIndex / (double)(kNumEvsMaxBws - 1));
    parameters.addParameter(evsMaxBwParam);

    StringListParameter* opusBwParam = new StringListParameter(STR16("Opus Max Bandwidth"), kParamOpusBandwidth, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    opusBwParam->appendString(STR16("NB (4 kHz)"));
    opusBwParam->appendString(STR16("MB (6 kHz)"));
    opusBwParam->appendString(STR16("WB (8 kHz)"));
    opusBwParam->appendString(STR16("SWB (12 kHz)"));
    opusBwParam->appendString(STR16("FB (20 kHz)"));
    opusBwParam->setNormalized((double)kDefaultOpusBandwidthIndex / (double)(kNumOpusBandwidths - 1));
    parameters.addParameter(opusBwParam);

    StringListParameter* amrNbModeParam = new StringListParameter(STR16("AMR-NB Mode"), kParamAmrNbMode, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    amrNbModeParam->appendString(STR16("4.75 kbps"));
    amrNbModeParam->appendString(STR16("5.15 kbps"));
    amrNbModeParam->appendString(STR16("5.90 kbps"));
    amrNbModeParam->appendString(STR16("6.70 kbps"));
    amrNbModeParam->appendString(STR16("7.40 kbps"));
    amrNbModeParam->appendString(STR16("7.95 kbps"));
    amrNbModeParam->appendString(STR16("10.2 kbps"));
    amrNbModeParam->appendString(STR16("12.2 kbps"));
    amrNbModeParam->setNormalized((double)kDefaultAmrNbModeIndex / (double)(kNumAmrNbModes - 1));
    parameters.addParameter(amrNbModeParam);

    StringListParameter* amrWbModeParam = new StringListParameter(STR16("AMR-WB Mode"), kParamAmrWbMode, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    amrWbModeParam->appendString(STR16("6.60 kbps"));
    amrWbModeParam->appendString(STR16("8.85 kbps"));
    amrWbModeParam->appendString(STR16("12.65 kbps"));
    amrWbModeParam->appendString(STR16("14.25 kbps"));
    amrWbModeParam->appendString(STR16("15.85 kbps"));
    amrWbModeParam->appendString(STR16("18.25 kbps"));
    amrWbModeParam->appendString(STR16("19.85 kbps"));
    amrWbModeParam->appendString(STR16("23.05 kbps"));
    amrWbModeParam->appendString(STR16("23.85 kbps"));
    amrWbModeParam->setNormalized((double)kDefaultAmrWbModeIndex / (double)(kNumAmrWbModes - 1));
    parameters.addParameter(amrWbModeParam);

    StringListParameter* opusBitrateParam = new StringListParameter(STR16("Opus Bitrate"), kParamOpusBitrate, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    opusBitrateParam->appendString(STR16("6 kbps"));
    opusBitrateParam->appendString(STR16("8 kbps"));
    opusBitrateParam->appendString(STR16("12 kbps"));
    opusBitrateParam->appendString(STR16("16 kbps"));
    opusBitrateParam->appendString(STR16("24 kbps"));
    opusBitrateParam->appendString(STR16("32 kbps"));
    opusBitrateParam->appendString(STR16("48 kbps"));
    opusBitrateParam->appendString(STR16("64 kbps"));
    opusBitrateParam->appendString(STR16("96 kbps"));
    opusBitrateParam->appendString(STR16("128 kbps"));
    opusBitrateParam->appendString(STR16("192 kbps"));
    opusBitrateParam->appendString(STR16("256 kbps"));
    opusBitrateParam->setNormalized((double)kDefaultOpusBitrateIndex / (double)(kNumOpusBitrates - 1));
    parameters.addParameter(opusBitrateParam);

    StringListParameter* g711LawParam = new StringListParameter(STR16("G.711 Law"), kParamG711Law, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    g711LawParam->appendString(STR16("\u03bc-law"));
    g711LawParam->appendString(STR16("A-law"));
    g711LawParam->setNormalized((double)kDefaultG711LawIndex / (double)(kNumG711Laws - 1));
    parameters.addParameter(g711LawParam);

    StringListParameter* segmentParam = new StringListParameter(STR16("\u52a3\u5316\u533a\u9593: in -> \u4ea4\u63db\u5c40 -> out"), kParamDegradationSegment, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    segmentParam->appendString(STR16("\u4e21\u65b9"));
    segmentParam->appendString(STR16("in -> \u4ea4\u63db\u5c40"));
    segmentParam->appendString(STR16("\u4ea4\u63db\u5c40 -> out"));
    segmentParam->appendString(STR16("\u306a\u3057"));
    parameters.addParameter(segmentParam);

    parameters.addParameter(new RangeParameter(STR16("Dry/Wet"), kParamDryWet, STR16("%"), 0.0, 100.0, 50.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Output Gain"), kParamOutputGain, STR16("dB"), -60.0, 24.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Enable Artifacts"), kParamArtifactsEnabled, STR16(""), 0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Artifact Amount"), kParamArtifactAmount, STR16("%"), 0.0, 100.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("\u30d1\u30b1\u30c3\u30c8\u30ed\u30b9"), kParamPacketLossRate, STR16("%"), 0.0, 30.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("\u901a\u4fe1\u52a3\u5316"), kParamNetworkDegradation, STR16("%"), 0.0, 100.0, 0.0, 0, ParameterInfo::kCanAutomate));
    // EVS DTX SID update interval in 20 ms frames. 0 = variable (the
    // codec default, promoted to ~12 frames internally). 3..100 = fixed
    // SID update interval. Hosts that do not use EVS can leave this at 0
    // -- the value is only consumed by EVSCodec / EVSCodecJbm.
    parameters.addParameter(new RangeParameter(STR16("EVS DTX SID Intv"), kParamEvsDtxSidInterval, STR16(""), 0.0, 100.0, 0.0, 100, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("EVS SC-VBR"), kParamEvsScVbr, STR16(""), 0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Bypass"), kParamMasterBypass, STR16(""), 0, 1, 0, 1, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass));

    for (size_t index = 0; index < TelephonyDSP::advancedDescriptors.size(); ++index) {
        const auto& descriptor = TelephonyDSP::advancedDescriptors[index];
        String128 title{}, units{};
        UString(title, 128).fromAscii(descriptor.title);
        UString(units, 128).fromAscii(descriptor.units);
        auto* parameter = new AdvancedParameter(title, index, units);
        parameter->setPrecision(descriptor.steps ? 0 : (descriptor.maximum <= 1 ? 5 : 2));
        parameters.addParameter(parameter);
    }
    parameters.addParameter(new RangeParameter(STR16("Input peak"), kParamInputPeak, STR16("dBFS"), -96, 12, -96, 0, ParameterInfo::kIsReadOnly));
    parameters.addParameter(new RangeParameter(STR16("Output peak"), kParamOutputPeak, STR16("dBFS"), -96, 12, -96, 0, ParameterInfo::kIsReadOnly));
    parameters.addParameter(new RangeParameter(STR16("Network measured packet loss"), kParamMeasuredLoss, STR16("%"), 0, 100, 0, 0, ParameterInfo::kIsReadOnly));
    parameters.addParameter(new RangeParameter(STR16("Network measured packet jitter"), kParamMeasuredJitter, STR16("ms"), 0, 1000, 0, 0, ParameterInfo::kIsReadOnly));
    auto* opusMode = new StringListParameter(STR16("Opus actual mode"), kParamOpusActualMode, nullptr, ParameterInfo::kIsReadOnly | ParameterInfo::kIsList);
    for (const auto* mode : {STR16("Inactive"), STR16("SILK"), STR16("Hybrid"), STR16("CELT")}) opusMode->appendString(mode);
    parameters.addParameter(opusMode);
    parameters.addParameter(new RangeParameter(STR16("Latency samples"), kParamLatencySamples, STR16("samples"), 0, kLatencyNormalization, 0, 0, ParameterInfo::kIsReadOnly | ParameterInfo::kIsHidden));
    auto* page = new StringListParameter(STR16("Editor page"), kParamEditorPage, nullptr, ParameterInfo::kIsHidden | ParameterInfo::kIsList);
    for (const auto* name : {STR16("Basic route"), STR16("Network"), STR16("Speech / codecs"), STR16("Channel effects"), STR16("Packet format")}) page->appendString(name);
    parameters.addParameter(page);

    // Hosts use metadata defaults for their reset/default preset actions.
    // StringListParameter::setNormalized changes only the live value.
    for (int32 i = 0; i < parameters.getParameterCount(); ++i) {
        auto* parameter = parameters.getParameterByIndex(i);
        parameter->getInfo().defaultNormalizedValue = parameter->getNormalized();
    }

    return kResultOk;
}

tresult PLUGIN_API TelephonyVoiceController::setParamNormalized(ParamID tag, ParamValue value)
{
    if (!std::isfinite(value)) return kResultFalse;
    value = std::clamp(value, 0.0, 1.0);
    const auto previous = getParamNormalized(tag);
    const auto result = EditController::setParamNormalized(tag, value);
    if (result != kResultOk) return result;
    if (tag == kParamEraMode || tag == kParamOutputEndpoint) {
        const int32 endpoint = (int32)std::lround(value * kMaxExposedEndpoint);
        EditController::setParamNormalized(tag == kParamEraMode ? kParamInputRoute : kParamOutputRoute,
            double(endpoint) / kModernMaxEndpoint);
    } else if (tag == kParamInputRoute || tag == kParamOutputRoute) {
        const int32 endpoint = endpointFromModernIndex((int32)std::lround(value * kModernMaxEndpoint));
        EditController::setParamNormalized(tag == kParamInputRoute ? kParamEraMode : kParamOutputEndpoint,
            double(legacyEndpoint(endpoint)) / kMaxExposedEndpoint);
    } else if (tag == kParamLatencySamples && value != previous && componentHandler) {
        componentHandler->restartComponent(kLatencyChanged);
    }
    return result;
}

tresult PLUGIN_API TelephonyVoiceController::setComponentState(IBStream* state)
{
    SavedState value;
    if (!readSavedState(state, value)) return kResultFalse;
    const auto eraMode = value.eraMode, outputEndpoint = value.outputEndpoint;
    const auto degradationSegment = value.segment;
    const auto dry = value.dry, gain = value.gain, amt = value.amount;
    const auto art = value.art, byp = value.bypass;
    const auto packetLossRate = value.packetLoss, networkDegradation = value.degradation;
    const auto evsSampleRate = value.evsSampleRate, evsBitrate = value.evsBitrate;
    const auto evsMaxBw = value.evsMaxBw, opusBandwidth = value.opusBandwidth;
    const auto amrNbMode = value.amrNbMode, evsDtxSidInterval = value.sidInterval;
    const auto amrWbMode = value.amrWbMode, g711Law = value.g711Law;
    const auto evsScVbr = value.scVbr, opusBitrate = value.opusBitrate;

    setParamNormalized(kParamEraMode,
        (double)legacyEndpoint(eraMode) / (double)kMaxExposedEndpoint);
    setParamNormalized(kParamOutputEndpoint,
        (double)legacyEndpoint(outputEndpoint) / (double)kMaxExposedEndpoint);
    setParamNormalized(kParamInputRoute, double(modernEndpointIndex(eraMode)) / kModernMaxEndpoint);
    setParamNormalized(kParamOutputRoute, double(modernEndpointIndex(outputEndpoint)) / kModernMaxEndpoint);
    for (size_t index = 0; index < value.advanced.normalized.size(); ++index)
        setParamNormalized(kParamAdvancedBase + (ParamID)index, value.advanced.normalized[index]);
    setParamNormalized(kParamDegradationSegment,
        (double)std::clamp(degradationSegment, 0, kMaxDegradationSegment) / (double)kMaxDegradationSegment);
    setParamNormalized(kParamDryWet, std::clamp(dry, 0.0f, 1.0f));
    setParamNormalized(kParamOutputGain, std::clamp((gain + 60.0) / 84.0, 0.0, 1.0));
    setParamNormalized(kParamArtifactsEnabled, art ? 1.0 : 0.0);
    setParamNormalized(kParamArtifactAmount, std::clamp(amt, 0.0f, 1.0f));
    // The processor stores the plain rate (0..0.30); the parameter is
    // normalized over that same 30 % span.
    setParamNormalized(kParamPacketLossRate, std::clamp(packetLossRate / 0.30, 0.0, 1.0));
    setParamNormalized(kParamNetworkDegradation, std::clamp(networkDegradation, 0.0f, 1.0f));
    setParamNormalized(kParamMasterBypass, byp ? 1.0 : 0.0);
    setParamNormalized(kParamEvsSampleRate,
        normalizedFromTable(evsSampleRate, kEvsSampleRateValues, kNumEvsSampleRates, kDefaultEvsSampleRateIndex));
    setParamNormalized(kParamEvsBitrate,
        normalizedFromTable(evsBitrate, kEvsBitrateValues, kNumEvsBitrates, kDefaultEvsBitrateIndex));
    setParamNormalized(kParamEvsMaxBw,
        normalizedFromTable(evsMaxBw, kEvsMaxBwValues, kNumEvsMaxBws, kDefaultEvsMaxBwIndex));
    setParamNormalized(kParamOpusBandwidth,
        normalizedFromTable(opusBandwidth, kOpusBandwidthValues, kNumOpusBandwidths, kDefaultOpusBandwidthIndex));
    setParamNormalized(kParamAmrNbMode,
        (double)std::clamp(amrNbMode, 0, kNumAmrNbModes - 1) / (double)(kNumAmrNbModes - 1));
    setParamNormalized(kParamEvsDtxSidInterval,
        (double)std::clamp(evsDtxSidInterval, 0, 100) / 100.0);
    setParamNormalized(kParamAmrWbMode,
        (double)std::clamp(amrWbMode, 0, kNumAmrWbModes - 1) / (double)(kNumAmrWbModes - 1));
    setParamNormalized(kParamG711Law, (g711Law != 0) ? 1.0 : 0.0);
    setParamNormalized(kParamEvsScVbr, (evsScVbr != 0) ? 1.0 : 0.0);
    setParamNormalized(kParamOpusBitrate,
        normalizedFromTable(opusBitrate, kOpusBitrateValues, kNumOpusBitrates, kDefaultOpusBitrateIndex));

    return kResultOk;
}

} // namespace Vst
} // namespace Steinberg

#include "public.sdk/source/main/pluginfactory.h"

BEGIN_FACTORY_DEF("Rumia Channel", "https://github.com/HeiMao-BaiMao/TelephonyVoice", "")
    DEF_CLASS2(INLINE_UID_FROM_FUID(Steinberg::Vst::TelephonyVoiceProcessor::uid),
               PClassInfo::kManyInstances, kVstAudioEffectClass, "TelephonyVoice", Vst::kDistributable, Vst::PlugType::kFx, "1.0.0.0", "Telephony Voice Emulator", Steinberg::Vst::TelephonyVoiceProcessor::createInstance)
    DEF_CLASS2(INLINE_UID_FROM_FUID(Steinberg::Vst::TelephonyVoiceController::uid),
               PClassInfo::kManyInstances, kVstComponentControllerClass, "TelephonyVoiceController", 0, "", "1.0.0.0", "", Steinberg::Vst::TelephonyVoiceController::createInstance)
END_FACTORY
