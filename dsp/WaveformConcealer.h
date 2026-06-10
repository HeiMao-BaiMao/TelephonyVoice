#pragma once

#include <vector>
#include <cstdint>

namespace TelephonyDSP {

    class WaveformConcealer {
    public:
        void reset(int frameSize);
        void storeGoodFrame(const int16_t* frame, int frameSize);
        void conceal(int16_t* out, int frameSize);

    private:
        std::vector<int16_t> history;
        float attenuation = 1.0f;
        int lastPitch = 80;

        int estimatePitch(int frameSize) const;
    };

} // namespace TelephonyDSP
