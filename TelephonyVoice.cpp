#include "TelephonyVoice.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "base/source/fstreamer.h"

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

// Generate unique IDs (Placeholders)
FUID TelephonyVoiceProcessor::uid(0x8625E8D6, 0x22F3F7FE, 0x400ED50D, 0x3F00CBAA);
FUID TelephonyVoiceController::uid(0x715EC843, 0x443AC666, 0x012F2754, 0x19E07432);

// --------------------------------------------------------------------------
// Processor Implementation
// --------------------------------------------------------------------------
TelephonyVoiceProcessor::TelephonyVoiceProcessor()
    : currentEraMode(0)
    , currentDryWet(0.5f)
    , currentOutGain(0.0f)
    , currentArtifactsEnabled(false)
    , currentArtifactAmount(0.0f)
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
                        case kParamEraMode: currentEraMode = (int32)(value * 5.0 + 0.5); break;
                        case kParamDryWet: currentDryWet = (float)value; break;
                        case kParamOutputGain: currentOutGain = (float)(value * 84.0 - 60.0); break;
                        case kParamArtifactsEnabled: currentArtifactsEnabled = (value > 0.5); break;
                        case kParamArtifactAmount: currentArtifactAmount = (float)value; break;
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
    dsp.setMode((TelephonyDSP::EraMode)std::clamp(currentEraMode, 0, 5));
    dsp.setParameters(currentDryWet, currentOutGain, currentArtifactsEnabled, currentArtifactAmount);
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
    currentEraMode = mode; currentDryWet = dry; currentOutGain = gain;
    currentArtifactsEnabled = art; currentArtifactAmount = amt; currentBypass = byp;
    updateDSPParameters();
    return kResultOk;
}

tresult PLUGIN_API TelephonyVoiceProcessor::getState(IBStream* state)
{
    IBStreamer streamer(state, kLittleEndian);
    streamer.writeInt32(currentEraMode); streamer.writeFloat(currentDryWet);
    streamer.writeFloat(currentOutGain); streamer.writeBool(currentArtifactsEnabled);
    streamer.writeFloat(currentArtifactAmount); streamer.writeBool(currentBypass);
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

    StringListParameter* modeParam = new StringListParameter(STR16("Era Mode"), kParamEraMode, nullptr, ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    modeParam->appendString(STR16("PSTN (G.711)")); modeParam->appendString(STR16("GSM FR"));
    modeParam->appendString(STR16("AMR-NB (3G)")); modeParam->appendString(STR16("AMR-WB (VoLTE)"));
    modeParam->appendString(STR16("EVS-Like")); modeParam->appendString(STR16("Bypass"));
    parameters.addParameter(modeParam);

    parameters.addParameter(new RangeParameter(STR16("Dry/Wet"), kParamDryWet, STR16("%"), 0.0, 1.0, 0.5, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Output Gain"), kParamOutputGain, STR16("dB"), -60.0, 24.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Enable Artifacts"), kParamArtifactsEnabled, STR16(""), 0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Artifact Amount"), kParamArtifactAmount, STR16("%"), 0.0, 1.0, 0.0, 0, ParameterInfo::kCanAutomate));
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