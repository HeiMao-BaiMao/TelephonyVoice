#pragma once

#include <cmath>

namespace TelephonyDSP {

    struct Biquad {
        float b0, b1, b2, a1, a2;
        float x1, x2, y1, y2;
        float targetB0=1,targetB1=0,targetB2=0,targetA1=0,targetA2=0;
        int rampRemaining=0;

        Biquad() { reset(); }
        // Reset to an identity pass-through and clear the delay line.
        // Callers use reset() to mean "no filtering" (e.g. EVS_NATIVE /
        // OPUS_VOIP skip the band cascade at zero network degradation), so
        // the neutral state must pass audio through unchanged. Zeroing b0
        // as well would turn the filter into a mute stage.
        void reset() {
            b0 = 1.0f;
            b1 = b2 = a1 = a2 = 0.0f;
            x1 = x2 = y1 = y2 = 0.0f;
            targetB0=1; targetB1=targetB2=targetA1=targetA2=0; rampRemaining=0;
        }
        
        // Calculate coefficients for Butterworth Highpass
        void setHighpass(float freq, float sampleRate, int transitionSamples=0) {
            float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
            float cosw0 = std::cos(w0);
            float alpha = std::sin(w0) / std::sqrt(2.0f);

            float norm = 1.0f / (1.0f + alpha);
            targetB0 = ((1.0f + cosw0) / 2.0f) * norm;
            targetB1 = -(1.0f + cosw0) * norm;
            targetB2 = targetB0;
            targetA1 = (-2.0f * cosw0) * norm;
            targetA2 = (1.0f - alpha) * norm;
            setRamp(transitionSamples);
        }

        // Calculate coefficients for Butterworth Lowpass
        void setLowpass(float freq, float sampleRate, int transitionSamples=0) {
            float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
            float cosw0 = std::cos(w0);
            float alpha = std::sin(w0) / std::sqrt(2.0f);

            float norm = 1.0f / (1.0f + alpha);
            targetB0 = ((1.0f - cosw0) / 2.0f) * norm;
            targetB1 = (1.0f - cosw0) * norm;
            targetB2 = targetB0;
            targetA1 = (-2.0f * cosw0) * norm;
            targetA2 = (1.0f - alpha) * norm;
            setRamp(transitionSamples);
        }

        void setRamp(int samples) {
            rampRemaining=samples>0?samples:0;
            if(!rampRemaining) { b0=targetB0;b1=targetB1;b2=targetB2;a1=targetA1;a2=targetA2; }
        }
        inline float process(float in) {
            if(rampRemaining>0) {
                const float step=1.f/rampRemaining;
                b0+=(targetB0-b0)*step; b1+=(targetB1-b1)*step; b2+=(targetB2-b2)*step;
                a1+=(targetA1-a1)*step; a2+=(targetA2-a2)*step; --rampRemaining;
            }
            float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in;
            y2 = y1; y1 = out;
            return out;
        }
    };

} // namespace TelephonyDSP
