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
        kParamDegradationSegment,
        kParamAmrWbMode,
        kParamOpusBandwidth,         // OPUS_BANDWIDTH_* (1101..1105); default FB
        kParamAmrNbMode,             // AMR-NB mode 0..7; default 7 (12.2 kbps)
        kParamG711Law,               // 0 = mu-law, 1 = A-law
        kParamEvsDtxSidInterval,     // 0 = variable SID, 3..100 = fixed frames
        kParamEvsScVbr,              // EVS Source-Controlled VBR toggle
        kParamOpusBitrate            // Opus target bitrate in bps (6..510000)
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
        int32 currentOpusBandwidth;  // OPUS_BANDWIDTH_* (1101..1105); default FB
        int32 currentOpusBitrate;    // Opus target bitrate in bps; default 24000
        int32 currentAmrNbMode;       // AMR-NB mode 0..7; default 7 (12.2 kbps)
        int32 currentAmrWbMode;       // AMR-WB mode 0..8; default 2 (12.65 kbps)
        int32 currentG711Law;         // 0 = mu-law (default), 1 = A-law
        int32 currentEvsDtxSidInterval; // 0 = variable SID (default), 3..100 = fixed frames
        int32 currentEvsScVbr;        // 0 = SC-VBR off (default), 1 = SC-VBR on
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
