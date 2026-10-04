#include "dsp/WaveformConcealer.h"
#include <algorithm>
#include <cmath>

namespace TelephonyDSP {
void WaveformConcealer::reset(int frameSize) {
    // Existing codec callers supply 20 ms frame sizes. The spectral estimator
    // remains streaming and does not depend on later call partition sizes.
    reset(frameSize, static_cast<int>(std::clamp(static_cast<double>(frameSize) * 50.0, 8000.0, 384000.0)));
}
void WaveformConcealer::reset(int frameSize, int sampleRate) {
    const double rate = std::clamp(static_cast<double>(sampleRate), 8000.0, 384000.0);
    comfortNoise.configure(rate);
    lastGoodFrame.assign(static_cast<std::size_t>(std::max(0, frameSize)), 0);
    repeatPosition = 0;
    attenuation = 1.0;
    sampleDecay = std::pow(0.82, 1.0 / (rate * 0.02));
    haveGoodFrame = false;
}
void WaveformConcealer::storeGoodFrame(const int16_t* frame, int frameSize) {
    if (!frame || frameSize <= 0) return;
    const auto count = static_cast<std::size_t>(frameSize);
    comfortNoise.observePCM16(frame, count);
    lastGoodFrame.resize(count);
    std::copy_n(frame, count, lastGoodFrame.begin());
    repeatPosition = 0;
    attenuation = 1.0;
    haveGoodFrame = true;
}
void WaveformConcealer::conceal(int16_t* out, int frameSize) {
    if (!out || frameSize <= 0) return;
    const auto count = static_cast<std::size_t>(frameSize);
    if (spectralNoise) {
        comfortNoise.generatePCM16(out, count);
        return;
    }
    if (!haveGoodFrame || lastGoodFrame.empty()) {
        std::fill_n(out, count, int16_t(0));
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<int16_t>(lastGoodFrame[repeatPosition] * attenuation);
        if (++repeatPosition == lastGoodFrame.size()) repeatPosition = 0;
        attenuation *= sampleDecay;
        // Once even full-scale PCM would round to silence, stop the decay
        // before extremely long loss bursts can enter the denormal range.
        if (attenuation < 0.5 / 32768.0) attenuation = 0.0;
    }
}
} // namespace TelephonyDSP
