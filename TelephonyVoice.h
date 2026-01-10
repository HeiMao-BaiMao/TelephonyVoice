#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "TelephonyDSP.h"

namespace Steinberg {
namespace Vst {

    // Parameter Tags
    enum TelephonyParams {
        kParamEraMode = 100,
        kParamDryWet,
        kParamOutputGain,
        kParamArtifactsEnabled,
        kParamArtifactAmount,
        kParamMasterBypass // VST3 standard bypass
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
        float currentDryWet;
        float currentOutGain;
        bool currentArtifactsEnabled;
        float currentArtifactAmount;
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