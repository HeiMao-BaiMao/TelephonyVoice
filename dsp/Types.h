#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>

namespace TelephonyDSP {

enum class EraMode {
        PSTN_G711 = 0,
        GSM_FR,
        AMR_NB_3G,
        AMR_WB_VOLTE,
        EVS_LIKE,    // Filter-only stand-in (safe for distribution, no codec)
        EVS_NATIVE,  // Real 3GPP EVS reference encoder+decoder (personal use)
        Bypass,
        // Experimental additions (BSD-licensed; non-distribution only).
        // Inserted at the end so existing numeric values remain stable.
        OPUS_VOIP,   // Opus encoder/decoder at VOIP-tuned settings (non-distribution)
#if TELEPHONY_USE_EVS_JBM
        // EVS_NATIVE plus the Stage-1 JBM/VoIP receive adapter
        // (evs_api_rx / 3GPP EvsRXlib). Off by default; enabled when
        // TELEPHONY_USE_EVS_JBM=ON. Non-distribution, float EVS only.
        EVS_JBM
#endif
    };

    enum class RouteEndpoint {
        FixedLine = 0,
        Mobile2G,
        Mobile3G,
        Mobile4G,
        Mobile5G,
        Mobile5GNative,
#if TELEPHONY_USE_EVS_JBM
        Mobile5GJbm
#endif
    };

    enum class DegradationSegment {
        Both = 0,
        InputToExchange,
        ExchangeToOutput,
        None
    };

    static const int LATENCY_MS = 150;

#ifdef TELEPHONY_DISTRIBUTION_BUILD
inline EraMode distributionSafeMode(EraMode m) {
    switch (m) {
        case EraMode::AMR_NB_3G:   case EraMode::AMR_WB_VOLTE:
        case EraMode::EVS_NATIVE:
#if TELEPHONY_USE_EVS_JBM
        case EraMode::EVS_JBM:
#endif
        case EraMode::OPUS_VOIP:   return EraMode::EVS_LIKE;
        default: return m;
    }
}
#else
inline EraMode distributionSafeMode(EraMode m) { return m; }
#endif

inline int clampToInt16(float v) {
    return std::isfinite(v) ? (int)std::clamp(v, -32768.0f, 32767.0f) : 0;
}

} // namespace TelephonyDSP
