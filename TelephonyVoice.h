#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "dsp/SignalProcessor.h"

namespace Steinberg {
namespace Vst {

    // Parameter Tags
    enum TelephonyParams {
        kParamEraMode = 100,
        kParamDryWet,
        kParamOutputGain,
        kParamArtifactsEnabled,
        kParamArtifactAmount,
        kParamMasterBypass, // VST3 standard bypass
        kParamPacketLossRate,
        kParamNetworkDegradation,
        kParamOutputEndpoint,
        kParamEvsSampleRate,
        kParamEvsBitrate,
        kParamEvsMaxBw,
        kParamDegradationSegment
    };

    // --------------------------------------------------------------------------
    // TelephonyVoiceProcessor
    // --------------------------------------------------------------------------
    class TelephonyVoiceProcessor : public AudioEffect {
    public:
        TelephonyVoiceProcessor();
        ~TelephonyVoiceProcessor();

        // AudioEffect Overrides
        tresult PLUGIN_API initialize(FUnknown* context) override;
        tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) override;
        tresult PLUGIN_API setupProcessing(ProcessSetup& newSetup) override;
        tresult PLUGIN_API setActive(TBool state) override;
        tresult PLUGIN_API process(ProcessData& data) override;
        
        // Persistence
        tresult PLUGIN_API setState(IBStream* state) override;
        tresult PLUGIN_API getState(IBStream* state) override;
        
        uint32 PLUGIN_API getLatencySamples() override;

        static FUnknown* createInstance(void*) { return (IAudioProcessor*)new TelephonyVoiceProcessor; }

        static FUID uid; 

    private:
        TelephonyDSP::SignalProcessor dsp;
        
        // Parameter values (normalized or plain)
        int32 currentEraMode;
        int32 currentOutputEndpoint;
        int32 currentDegradationSegment;
        int32 currentEvsSampleRate;
        int32 currentEvsBitrate;
        int32 currentEvsMaxBw;
        float currentDryWet;
        float currentOutGain;
        bool currentArtifactsEnabled;
        float currentArtifactAmount;
        float currentPacketLossRate;
        float currentNetworkDegradation;
        bool currentBypass;

        void updateDSPParameters();
    };

    // --------------------------------------------------------------------------
    // TelephonyVoiceController
    // --------------------------------------------------------------------------
    class TelephonyVoiceController : public EditController {
    public:
        TelephonyVoiceController();
        ~TelephonyVoiceController();

        tresult PLUGIN_API initialize(FUnknown* context) override;
        tresult PLUGIN_API setComponentState(IBStream* state) override;

        static FUnknown* createInstance(void*) { return (IEditController*)new TelephonyVoiceController; }

        static FUID uid;
    };

} // namespace Vst
} // namespace Steinberg
