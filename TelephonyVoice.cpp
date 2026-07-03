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
        // Size the bypass delay line to the latency the host is told about
        // so bypassed audio stays time-aligned with processed audio.
        bypassDelayLen = (int)dsp.getLatencySamples();
        bypassDelayPos = 0;
        bypassDelayBuf.assign(2, std::vector<float>((size_t)std::max(bypassDelayLen, 1), 0.0f));
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

    if (data.numInputs == 0 || data.numOutputs == 0) return kResultOk;
    if (data.symbolicSampleSize != kSample32) return kResultOk;

    int32 numInCh = data.inputs[0].numChannels;
    int32 numOutCh = data.outputs[0].numChannels;
    float** in = data.inputs[0].channelBuffers32;
    float** out = data.outputs[0].channelBuffers32;
    if (!in || !out || numInCh <= 0 || numOutCh <= 0) return kResultOk;

    const int delayLen = bypassDelayLen;
    const int maxDelayCh = (std::min)((int)numOutCh, (int)bypassDelayBuf.size());

    if (currentBypass) {
        // Latency-compensated bypass: emit the input delayed by the same
        // amount the host compensates for (getLatencySamples()), so
        // toggling bypass does not shift the audio in time.
        for (int ch = 0; ch < numOutCh; ch++) {
            float* src = in[ch % numInCh];
            float* dst = out[ch];
            if (delayLen <= 0 || ch >= maxDelayCh) {
                if (src != dst) memcpy(dst, src, data.numSamples * sizeof(float));
            } else {
                float* ring = bypassDelayBuf[ch].data();
                int pos = bypassDelayPos;
                for (int32 i = 0; i < data.numSamples; ++i) {
                    const float x = src[i]; // read before dst write: src may alias dst
                    dst[i] = ring[pos];
                    ring[pos] = x;
                    if (++pos == delayLen) pos = 0;
                }
            }
        }
        if (delayLen > 0) bypassDelayPos = (int)((bypassDelayPos + data.numSamples) % delayLen);
        return kResultOk;
    }

    // Keep the bypass delay line fed while processing normally so a
    // mid-playback bypass toggle plays correctly aligned dry audio
    // instead of a stale/zeroed buffer.
    if (delayLen > 0) {
        for (int ch = 0; ch < maxDelayCh; ch++) {
            const float* src = in[ch % numInCh];
            float* ring = bypassDelayBuf[ch].data();
            int pos = bypassDelayPos;
            for (int32 i = 0; i < data.numSamples; ++i) {
                ring[pos] = src[i];
                if (++pos == delayLen) pos = 0;
            }
        }
        bypassDelayPos = (int)((bypassDelayPos + data.numSamples) % delayLen);
    }

    // Pass to DSP (Multi-channel)
    dsp.process(in, numInCh, out, numOutCh, data.numSamples);

    return kResultOk;
}

void TelephonyVoiceProcessor::updateDSPParameters()
{
    currentEraMode = std::clamp(currentEraMode, 0, kMaxExposedEndpoint);
    currentOutputEndpoint = std::clamp(currentOutputEndpoint, 0, kMaxExposedEndpoint);
    currentDegradationSegment = std::clamp(currentDegradationSegment, 0, kMaxDegradationSegment);
    currentEvsMaxBw = std::clamp(currentEvsMaxBw, (int32)EVS_NB, (int32)EVS_FB);
    currentOpusBandwidth = std::clamp(currentOpusBandwidth, 1101, 1105);
    currentAmrNbMode = std::clamp(currentAmrNbMode, 0, kNumAmrNbModes - 1);
    currentAmrWbMode = std::clamp(currentAmrWbMode, 0, 8);
    currentG711Law = std::clamp(currentG711Law, 0, 1);
    currentEvsDtxSidInterval = std::clamp(currentEvsDtxSidInterval, 0, 100);
    currentEvsScVbr = std::clamp(currentEvsScVbr, 0, 1);
    currentOpusBitrate = std::clamp(currentOpusBitrate, 6, 510000);
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
    if (!streamer.readInt32(currentOpusBandwidth)) currentOpusBandwidth = kDefaultOpusBandwidth;
    if (!streamer.readInt32(currentAmrNbMode)) currentAmrNbMode = kDefaultAmrNbModeIndex;
    if (!streamer.readInt32(currentEvsDtxSidInterval)) currentEvsDtxSidInterval = 0;
    if (!streamer.readInt32(currentAmrWbMode)) currentAmrWbMode = kDefaultAmrWbModeIndex;
    if (!streamer.readInt32(currentG711Law)) currentG711Law = kDefaultG711LawIndex;
    if (!streamer.readInt32(currentEvsScVbr)) currentEvsScVbr = 0;
    if (!streamer.readInt32(currentOpusBitrate)) currentOpusBitrate = kOpusBitrateValues[kDefaultOpusBitrateIndex];
    currentOutputEndpoint = std::clamp(currentOutputEndpoint, 0, kMaxExposedEndpoint);
    currentDegradationSegment = std::clamp(currentDegradationSegment, 0, kMaxDegradationSegment);
    currentPacketLossRate = std::clamp(currentPacketLossRate, 0.0f, 0.95f);
    currentNetworkDegradation = std::clamp(currentNetworkDegradation, 0.0f, 1.0f);
    currentEvsMaxBw = std::clamp(currentEvsMaxBw, (int32)EVS_NB, (int32)EVS_FB);
    currentOpusBandwidth = std::clamp(currentOpusBandwidth, 1101, 1105);
    currentAmrNbMode = std::clamp(currentAmrNbMode, 0, kNumAmrNbModes - 1);
    currentEvsDtxSidInterval = std::clamp(currentEvsDtxSidInterval, 0, 100);
    currentAmrWbMode = std::clamp(currentAmrWbMode, 0, kNumAmrWbModes - 1);
    currentG711Law = std::clamp(currentG711Law, 0, 1);
    currentEvsScVbr = std::clamp(currentEvsScVbr, 0, 1);
    currentOpusBitrate = std::clamp(currentOpusBitrate, 6, 510000);
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
    streamer.writeInt32(currentOpusBandwidth);
    streamer.writeInt32(currentAmrNbMode);
    streamer.writeInt32(currentEvsDtxSidInterval);
    streamer.writeInt32(currentAmrWbMode);
    streamer.writeInt32(currentG711Law);
    streamer.writeInt32(currentEvsScVbr);
    streamer.writeInt32(currentOpusBitrate);
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

    parameters.addParameter(new RangeParameter(STR16("Dry/Wet"), kParamDryWet, STR16("%"), 0.0, 1.0, 0.5, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Output Gain"), kParamOutputGain, STR16("dB"), -60.0, 24.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Enable Artifacts"), kParamArtifactsEnabled, STR16(""), 0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Artifact Amount"), kParamArtifactAmount, STR16("%"), 0.0, 1.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("\u30d1\u30b1\u30c3\u30c8\u30ed\u30b9"), kParamPacketLossRate, STR16("%"), 0.0, 30.0, 0.0, 0, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("\u901a\u4fe1\u52a3\u5316"), kParamNetworkDegradation, STR16("%"), 0.0, 100.0, 0.0, 0, ParameterInfo::kCanAutomate));
    // EVS DTX SID update interval in 20 ms frames. 0 = variable (the
    // codec default, promoted to ~12 frames internally). 3..100 = fixed
    // SID update interval. Hosts that do not use EVS can leave this at 0
    // -- the value is only consumed by EVSCodec / EVSCodecJbm.
    parameters.addParameter(new RangeParameter(STR16("EVS DTX SID Intv"), kParamEvsDtxSidInterval, STR16(""), 0.0, 100.0, 0.0, 100, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("EVS SC-VBR"), kParamEvsScVbr, STR16(""), 0.0, 1.0, 0.0, 1, ParameterInfo::kCanAutomate));
    parameters.addParameter(new RangeParameter(STR16("Bypass"), kParamMasterBypass, STR16(""), 0, 1, 0, 1, ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass));

    return kResultOk;
}

tresult PLUGIN_API TelephonyVoiceController::setComponentState(IBStream* state)
{
    // Mirror TelephonyVoiceProcessor::setState() so the UI reflects the
    // values the processor just restored. The stream layout must stay in
    // lockstep with getState()/setState(); trailing fields are optional
    // for backward compatibility with older saved states.
    if (!state) return kResultFalse;

    IBStreamer streamer(state, kLittleEndian);
    int32 eraMode; float dry, gain, amt; bool art, byp;
    if (!streamer.readInt32(eraMode)) return kResultFalse;
    if (!streamer.readFloat(dry)) return kResultFalse;
    if (!streamer.readFloat(gain)) return kResultFalse;
    if (!streamer.readBool(art)) return kResultFalse;
    if (!streamer.readFloat(amt)) return kResultFalse;
    if (!streamer.readBool(byp)) return kResultFalse;

    int32 outputEndpoint, degradationSegment;
    float packetLossRate, networkDegradation;
    int32 evsSampleRate, evsBitrate, evsMaxBw, opusBandwidth, amrNbMode, evsDtxSidInterval;
    int32 amrWbMode, g711Law, evsScVbr, opusBitrate;
    if (!streamer.readInt32(outputEndpoint)) outputEndpoint = kDefaultOutputEndpoint;
    if (!streamer.readInt32(degradationSegment)) degradationSegment = 0;
    if (!streamer.readFloat(packetLossRate)) packetLossRate = 0.0f;
    if (!streamer.readFloat(networkDegradation)) networkDegradation = 0.0f;
    if (!streamer.readInt32(evsSampleRate)) evsSampleRate = kDefaultEvsSampleRate;
    if (!streamer.readInt32(evsBitrate)) evsBitrate = kDefaultEvsBitrate;
    if (!streamer.readInt32(evsMaxBw)) evsMaxBw = kDefaultEvsMaxBw;
    if (!streamer.readInt32(opusBandwidth)) opusBandwidth = kDefaultOpusBandwidth;
    if (!streamer.readInt32(amrNbMode)) amrNbMode = kDefaultAmrNbModeIndex;
    if (!streamer.readInt32(evsDtxSidInterval)) evsDtxSidInterval = 0;
    if (!streamer.readInt32(amrWbMode)) amrWbMode = kDefaultAmrWbModeIndex;
    if (!streamer.readInt32(g711Law)) g711Law = kDefaultG711LawIndex;
    if (!streamer.readInt32(evsScVbr)) evsScVbr = 0;
    if (!streamer.readInt32(opusBitrate)) opusBitrate = kOpusBitrateValues[kDefaultOpusBitrateIndex];

    setParamNormalized(kParamEraMode,
        (double)std::clamp(eraMode, 0, kMaxExposedEndpoint) / (double)kMaxExposedEndpoint);
    setParamNormalized(kParamOutputEndpoint,
        (double)std::clamp(outputEndpoint, 0, kMaxExposedEndpoint) / (double)kMaxExposedEndpoint);
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
