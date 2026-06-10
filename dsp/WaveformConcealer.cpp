#define NOMINMAX
#include "dsp/WaveformConcealer.h"
#include "dsp/Types.h"
#include <cstring>
#include <cmath>
#include <algorithm>

namespace TelephonyDSP {

    void WaveformConcealer::reset(int frameSize) {
        history.assign((std::max)(frameSize * 4, frameSize), 0);
        attenuation = 1.0f;
        lastPitch = (std::max)(20, frameSize / 2);
    }

    void WaveformConcealer::storeGoodFrame(const int16_t* frame, int frameSize) {
        if (frameSize <= 0) return;
        if (history.size() < (size_t)(frameSize * 4)) {
            reset(frameSize);
        }

        std::memmove(history.data(), history.data() + frameSize,
                     (history.size() - frameSize) * sizeof(int16_t));
        std::memcpy(history.data() + history.size() - frameSize, frame, frameSize * sizeof(int16_t));
        attenuation = 1.0f;
        lastPitch = estimatePitch(frameSize);
    }

    int WaveformConcealer::estimatePitch(int frameSize) const {
        if (frameSize <= 0 || history.size() < (size_t)(frameSize * 2)) {
            return (std::max)(20, frameSize / 2);
        }

        const int histSize = (int)history.size();
        const int current = histSize - frameSize;
        const int minLag = (std::max)(20, frameSize / 8);
        const int maxLag = (std::min)(frameSize * 2, current - 1);
        if (maxLag <= minLag) return (std::max)(20, frameSize / 2);

        float bestScore = -1.0f;
        int bestLag = lastPitch;
        for (int lag = minLag; lag <= maxLag; ++lag) {
            const int ref = current - lag;
            if (ref < 0) break;

            double corr = 0.0;
            double e1 = 1.0;
            double e2 = 1.0;
            for (int i = 0; i < frameSize; ++i) {
                const double a = history[current + i];
                const double b = history[ref + i];
                corr += a * b;
                e1 += a * a;
                e2 += b * b;
            }
            const float score = (float)(corr / std::sqrt(e1 * e2));
            if (score > bestScore) {
                bestScore = score;
                bestLag = lag;
            }
        }

        return bestScore > 0.15f ? bestLag : (std::max)(20, frameSize / 2);
    }

    void WaveformConcealer::conceal(int16_t* out, int frameSize) {
        if (frameSize <= 0) return;
        if (history.size() < (size_t)(frameSize * 2)) {
            reset(frameSize);
        }

        const int histSize = (int)history.size();
        const int pitch = std::clamp(lastPitch, 1, histSize);
        const int start = histSize - pitch;
        std::vector<int16_t> concealed(frameSize);

        for (int i = 0; i < frameSize; ++i) {
            const float intraFrameFade = 1.0f - 0.15f * ((float)i / (float)(std::max)(1, frameSize - 1));
            const int idx = start + (i % pitch);
            concealed[i] = clampToInt16((float)history[idx] * attenuation * intraFrameFade);
        }

        std::memcpy(out, concealed.data(), frameSize * sizeof(int16_t));
        std::memmove(history.data(), history.data() + frameSize,
                     (history.size() - frameSize) * sizeof(int16_t));
        std::memcpy(history.data() + history.size() - frameSize, concealed.data(), frameSize * sizeof(int16_t));
        attenuation = (std::max)(0.05f, attenuation * 0.82f);
    }

} // namespace TelephonyDSP
