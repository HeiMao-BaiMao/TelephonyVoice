#pragma once

#include "evs_api.h"
#include <algorithm>

namespace TelephonyDSP {
// Frontend parameter choices are independent. Resolve incompatible combinations
// deterministically, so restoring a preset never depends on a previous route.
// Keep the SC-VBR toggle separate from the nominal fixed-rate choice: the codec
// applies it, and turning it off can restore the selected fixed bitrate.
inline void normalizeEvsConfig(int& sampleRateHz, int& bitrateBps, EVS_Bandwidth& maxBw) {
    EVS_EncOptions options;
    evs_enc_options_init(&options);
    options.dtx_enable = 1;
    if ((sampleRateHz == 8000 || maxBw == EVS_NB) && bitrateBps > 24400)
        bitrateBps = 24400;
    if (evs_enc_normalize_config(sampleRateHz, &bitrateBps, &maxBw, &options) != EVS_OK) {
        sampleRateHz = 32000;
        bitrateBps = 13200;
        maxBw = EVS_SWB;
    }
}
} // namespace TelephonyDSP
