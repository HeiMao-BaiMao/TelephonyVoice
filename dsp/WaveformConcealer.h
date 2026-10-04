#pragma once

#include "dsp/AudioSimulation.h"
#include <cstdint>
#include <vector>

namespace TelephonyDSP {
// Local loss-concealment models, not codec-native PLC. The default produces
// deterministic PSD-shaped comfort noise; the alternative repeats the latest
// good frame with a gradual fade. Neither path learns from concealed output.
class WaveformConcealer {
public:
    void reset(int frameSize);
    void reset(int frameSize, int sampleRate);
    void setSpectralNoise(bool enabled) { spectralNoise = enabled; }
    void storeGoodFrame(const int16_t* frame, int frameSize);
    // No allocation, including when frameSize differs from the stored frame.
    void conceal(int16_t* out, int frameSize);
private:
    PsdComfortNoise comfortNoise;
    std::vector<int16_t> lastGoodFrame;
    std::size_t repeatPosition = 0;
    double attenuation = 1.0;
    double sampleDecay = 0.99876045; // 0.82 per 20 ms at the default 8 kHz.
    bool spectralNoise = true;
    bool haveGoodFrame = false;
};
} // namespace TelephonyDSP
