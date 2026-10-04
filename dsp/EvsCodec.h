#pragma once
#include "dsp/ICodec.h"
#include "dsp/WaveformConcealer.h"
#include "evs_api.h"

namespace TelephonyDSP {
EVS_Bandwidth estimateEvsInputBandwidth(const int16_t* input, int frameSize, int sampleRate, EVS_Bandwidth ceiling, EVS_Bandwidth current);

class EVSCodec : public ICodec {
public:
    EVSCodec(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw,
             bool scVbrEnabled = false, int dtxSidInterval = 0);
    ~EVSCodec() override;
    void reset() override;
    int getSampleRate() const override { return sampleRate; }
    int getFrameSize() const override { return sampleRate / 50; }
    void processFrame(const int16_t* in, int16_t* out, bool packetLost) override;
    void configureDtx(bool enabled, bool pureSilence) override;
    void setScVbrEnabled(bool enable);
    bool getScVbrEnabled() const { return scVbrEnabled; }
    void setDtxSidInterval(int interval);
    bool setBitrate(int bitrate);
    bool setMaxBandwidth(EVS_Bandwidth bandwidth);
    // Atomically change runtime mode while retaining both codec histories.
    bool reconfigure(int bitrate, EVS_Bandwidth bandwidth, bool amrWbIo = false);
    bool setAmrWbIo(bool enabled, int bitrate = 12650);
    bool getAmrWbIo() const { return amrWbIo; }
    void setAutoBandwidth(bool enabled);
    bool getAutoBandwidth() const { return autoBandwidth; }
    EVS_Bandwidth getActiveBandwidth() const { return activeBw; }
    EVS_Bandwidth getDetectedBandwidth() const { return detectedBw; }
    int getBitrate() const { return bitrateBps; }
    EVS_Bandwidth getMaxBandwidth() const { return maxBw; }
private:
    int sampleRate, bitrateBps;
    int nativeBitrateBps;
    int fixedBitrateBps;
    EVS_Bandwidth fixedMaxBw;
    EVS_Bandwidth nativeMaxBw;
    bool nativeScVbrEnabled;
    EVS_Bandwidth maxBw, activeBw, detectedBw;
    bool scVbrEnabled, amrWbIo = false, autoBandwidth = false;
    int dtxSidInterval, bandwidthHold = 0;
    EVS_Bandwidth pendingBw;
    EVS_Encoder* enc = nullptr;
    EVS_Decoder* dec = nullptr;
    std::vector<unsigned char> bitstream;
    WaveformConcealer fallbackPLC;
    EVS_EncOptions options() const;
    bool applyConfiguration(int bitrate, EVS_Bandwidth bandwidth, bool io, bool storeCeiling);
    void estimateBandwidth(const int16_t* input);
};
} // namespace TelephonyDSP
