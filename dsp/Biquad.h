#pragma once

#include <cmath>

namespace TelephonyDSP {

    struct Biquad {
        float b0, b1, b2, a1, a2;
        float x1, x2, y1, y2;

        Biquad() { reset(); }
        void reset() {
            b0 = b1 = b2 = a1 = a2 = 0.0f;
            x1 = x2 = y1 = y2 = 0.0f;
        }
        
        // Calculate coefficients for Butterworth Highpass
        void setHighpass(float freq, float sampleRate) {
            float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
            float cosw0 = std::cos(w0);
            float alpha = std::sin(w0) / std::sqrt(2.0f);

            float norm = 1.0f / (1.0f + alpha);
            b0 = ((1.0f + cosw0) / 2.0f) * norm;
            b1 = -(1.0f + cosw0) * norm;
            b2 = ((1.0f + cosw0) / 2.0f) * norm;
            a1 = (-2.0f * cosw0) * norm;
            a2 = (1.0f - alpha) * norm;
        }

        // Calculate coefficients for Butterworth Lowpass
        void setLowpass(float freq, float sampleRate) {
            float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
            float cosw0 = std::cos(w0);
            float alpha = std::sin(w0) / std::sqrt(2.0f);

            float norm = 1.0f / (1.0f + alpha);
            b0 = ((1.0f - cosw0) / 2.0f) * norm;
            b1 = (1.0f - cosw0) * norm;
            b2 = ((1.0f - cosw0) / 2.0f) * norm;
            a1 = (-2.0f * cosw0) * norm;
            a2 = (1.0f - alpha) * norm;
        }

        inline float process(float in) {
            float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in;
            y2 = y1; y1 = out;
            return out;
        }
    };

} // namespace TelephonyDSP
