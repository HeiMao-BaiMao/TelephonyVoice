#include "TelephonyVoice.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "base/source/fstreamer.h"
#include <algorithm>

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
constexpr int32 kMaxExposedEndpoint = 6;
constexpr int32 kDefaultOutputEndpoint = 5;
#endif
constexpr int32 kMaxDegradationSegment = 3;

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

constexpr int32 kDefaultEvsSampleRate = 32000;
constexpr int32 kDefaultEvsBitrate = 13200;
constexpr int32 kDefaultEvsMaxBw = (int32)EVS_SWB;

TelephonyDSP::RouteEndpoint endpointFromParameter(int32 endpoint)
{
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

// Generate unique IDs (Placeholders)
FUID TelephonyVoiceProcessor::uid(0x8625E8D6, 0x22F3F7FE, 0x400ED50D, 0x3F00CBAA);
FUID TelephonyVoiceController::uid(0x715EC843, 0x443AC666, 0x012F2754, 0x19E07432);

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
    , currentDryWet(0.5f)
    , currentOutGain(0.0f)
    , currentArtifactsEnabled(false)
    , currentArtifactAmount(0.0f)
    , currentPacketLossRate(0.0f)
    , currentNetworkDegradation(0.0f)
    , currentBypass(false)
{
    setControllerClass(TelephonyVoiceController::uid);
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
    if (numIns == 1 && numOuts == 1) {
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
    dsp.setSampleRate(newSetup.sampleRate);
    dsp.setSimulateLatency(true); // VST uses latency
    return AudioEffect::setupProcessing(newSetup);
}

tresult PLUGIN_API TelephonyVoiceProcessor::setActive(TBool state)
{
    if (state) {
        dsp.reset();
        updateDSPParameters();
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
                if (queue->getPoint(numPoints - 1, sampleOffset, value) == kResultOk) {
                    ParamID pid = queue->getParameterId();
                    switch (pid) {
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

    if (data.numInputs == 0 || data.numOutputs == 0) return kResultOk;
    if (data.symbolicSampleSize != kSample32) return kResultOk;

    if (currentBypass) {
        if (data.inputs[0].channelBuffers32 && data.outputs[0].channelBuffers32) {
             for (int ch = 0; ch < data.numOutputs; ch++) {
                 float* src = data.inputs[0].channelBuffers32[ch % data.numInputs];
                 float* dst = data.outputs[0].channelBuffers32[ch];
                 if (src != dst) memcpy(dst, src, data.numSamples * sizeof(float));
             }
        }
        return kResultOk;
    }

    // Pass to DSP (Multi-channel)
    int32 numInCh = data.inputs[0].numChannels;
    int32 numOutCh = data.outputs[0].numChannels;
    float** in = data.inputs[0].channelBuffers32;
    float** out = data.outputs[0].channelBuffers32;

    dsp.process(in, numInCh, out, numOutCh, data.numSamples);

    return kResultOk;
}

void TelephonyVoiceProcessor::updateDSPParameters()
{
    currentEraMode = std::clamp(currentEraMode, 0, kMaxExposedEndpoint);
    currentOutputEndpoint = std::clamp(currentOutputEndpoint, 0, kMaxExposedEndpoint);
    currentDegradationSegment = std::clamp(currentDegradationSegment, 0, kMaxDegradationSegment);
    currentEvsMaxBw = std::clamp(currentEvsMaxBw, (int32)EVS_NB, (int32)EVS_FB);
    dsp.setRoute(endpointFromParameter(currentEraMode),
                 endpointFromParameter(currentOutputEndpoint),
                 degradationSegmentFromParameter(currentDegradationSegment));
    dsp.setParameters(currentDryWet, currentOutGain, currentArtifactsEnabled, currentArtifactAmount,
                      currentPacketLossRate, currentNetworkDegradation);
    dsp.setEVSConfig(currentEvsSampleRate, currentEvsBitrate, (EVS_Bandwidth)currentEvsMaxBw);
}

tresult PLUGIN_API TelephonyVoiceProcessor::setState(IBStream* state)
{
    IBStreamer streamer(state, kLittleEndian);
    int32 mode; float dry, gain, amt; bool art, byp;
    if (!streamer.readInt32(mode)) return kResultFalse;
    if (!streamer.readFloat(dry)) return kResultFalse;
    if (!streamer.readFloat(gain)) return kResultFalse;
    if (!streamer.readBool(art)) return kResultFalse;
    if (!streamer.readFloat(amt)) return kResultFalse;
    if (!streamer.readBool(byp)) return kResultFalse;
    currentEraMode = std::clamp(mode, 0, kMaxExposedEndpoint);
    currentDryWet = dry; currentOutGain = gain;
    currentArtifactsEnabled = art; currentArtifactAmount = amt;
    currentBypass = byp;
    if (!streamer.readInt32(currentOutputEndpoint)) currentOutputEndpoint = kDefaultOutputEndpoint;
    if (!streamer.readInt32(currentDegradationSegment)) currentDegradationSegment = 0;
    if (!streamer.readFloat(currentPacketLossRate)) currentPacketLossRate = 0.0f;
    if (!streamer.readFloat(currentNetworkDegradation)) currentNetworkDegradation = 0.0f;
    if (!streamer.readInt32(currentEvsSampleRate)) currentEvsSampleRate = kDefaultEvsSampleRate;
    if (!streamer.readInt32(currentEvsBitrate)) currentEvsBitrate = kDefaultEvsBitrate;
    if (!streamer.readInt32(currentEvsMaxBw)) currentEvsMaxBw = kDefaultEvsMaxBw;
    currentOutputEndpoint = std::clamp(currentOutputEndpoint, 0, kMaxExposedEndpoint);
    currentDegradationSegment = std::clamp(currentDegradationSegment, 0, kMaxDegradationSegment);
    currentPacketLossRate = std::clamp(currentPacketLossRate, 0.0f, 0.95f);
    currentNetworkDegradation = std::clamp(currentNetworkDegradation, 0.0f, 1.0f);
    currentEvsMaxBw = std::clamp(currentEvsMaxBw, (int32)EVS_NB, (int32)EVS_FB);
    updateDSPParameters();
    return kResultOk;
}

tresult PLUGIN_API TelephonyVoiceProcessor::getState(IBStream* state)
{
    IBStreamer streamer(state, kLittleEndian);
    streamer.writeInt32(currentEraMode); streamer.writeFloat(currentDryWet);
    streamer.writeFloat(currentOutGain); streamer.writeBool(currentArtifactsEnabled);
    streamer.writeFloat(currentArtifactAmount); streamer.writeBool(currentBypass);
    streamer.writeInt32(currentOutputEndpoint);
    streamer.writeInt32(currentDegradationSegment);
    streamer.writeFloat(currentPacketLossRate);
    streamer.writeFloat(currentNetworkDegradation);
    streamer.writeInt32(currentEvsSampleRate);
    streamer.writeInt32(currentEvsBitrate);
    streamer.writeInt32(currentEvsMaxBw);
    return kResultOk;
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

    StringListParameter* inputParam = new StringListParameter(STR16("in"), kParamEraMode, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    appendEndpointStrings(inputParam);
    inputParam->setNormalized(0.0);
    parameters.addParameter(inputParam);

    StringListParameter* outputParam = new StringListParameter(STR16("out"), kParamOutputEndpoint, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    appendEndpointStrings(outputParam);
    outputParam->setNormalized((double)kDefaultOutputEndpoint / (double)kMaxExposedEndpoint);
    parameters.addParameter(outputParam);

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

    StringListParameter* segmentParam = new StringListParameter(STR16("\u52a3\u5316\u533a\u9593: in -> \u4ea4\u63db\u5c40 -> out"), kParamDegradationSegment, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    segmentParam->appendString(STR16("\u4e21\u65b9"));
    segmentParam->appendString(STR16("in -> \u4ea4\u63db\u5c40"));
    segmentParam->appendString(STR16("\u4ea4\u63db\u5c40 -> out"));
    segmentParam->appendString(STR16("\u306a\u3057"));
    parameters.addParameter(segmentParam);

    parameters.addParameter(new RangeParameter(STR16("Dry/Wet"), kParamDryWet, STR16("%"), 0.0, 1.0, 0.5, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Output Gain"), kParamOutputGain, STR16("dB"), -60.0, 24.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Enable Artifacts"), kParamArtifactsEnabled, STR16(""), 0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Artifact Amount"), kParamArtifactAmount, STR16("%"), 0.0, 1.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("\u30d1\u30b1\u30c3\u30c8\u30ed\u30b9"), kParamPacketLossRate, STR16("%"), 0.0, 30.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("\u901a\u4fe1\u52a3\u5316"), kParamNetworkDegradation, STR16("%"), 0.0, 100.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Bypass"), kParamMasterBypass, STR16(""), 0, 1, 0, 1, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass));

    return kResultOk;
}

tresult PLUGIN_API TelephonyVoiceController::setComponentState(IBStream* state) { return kResultOk; }

} // namespace Vst
} // namespace Steinberg

#include "public.sdk/source/main/pluginfactory.h"

BEGIN_FACTORY_DEF("Voxengo (Aleksey Vaneev)", "https://www.voxengo.com", "mailto:info@voxengo.com")
    DEF_CLASS2(INLINE_UID_FROM_FUID(Steinberg::Vst::TelephonyVoiceProcessor::uid),
               PClassInfo::kManyInstances, kVstAudioEffectClass, "TelephonyVoice", Vst::kDistributable, Vst::PlugType::kFx, "1.0.0.0", "Telephony Voice Emulator", Steinberg::Vst::TelephonyVoiceProcessor::createInstance)
    DEF_CLASS2(INLINE_UID_FROM_FUID(Steinberg::Vst::TelephonyVoiceController::uid),
               PClassInfo::kManyInstances, kVstComponentControllerClass, "TelephonyVoiceController", 0, "", "1.0.0.0", "", Steinberg::Vst::TelephonyVoiceController::createInstance)
END_FACTORY
