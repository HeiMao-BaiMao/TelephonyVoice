#include <cstring>
#include "dsp/EvsCodec.h"

namespace TelephonyDSP {

    EVSCodec::EVSCodec(int sampleRate, int bitrateBps, EVS_Bandwidth maxBw,
                       bool scVbrEnabled, int dtxSidInterval)
        : sampleRate(sampleRate)
        , bitrateBps(bitrateBps)
        , maxBw(maxBw)
        , scVbrEnabled(scVbrEnabled)
        , dtxSidInterval(dtxSidInterval)
        , enc(nullptr)
        , dec(nullptr)
    {
        fallbackPLC.reset(getFrameSize());
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        // Non-distribution / personal build: enable the 3GPP EVS internal
        // VAD/DTX/SID/CNG path with a variable SID update interval (0 lets
        // the codec pick the per-frame interval, matching the reference
        // CLI's default behaviour). Channel-aware mode (RF) stays off: the
        // EVS spec only allows RF at 13.2 kbps with >= 16 kHz input.
        // SC-VBR (Source-Controlled VBR) is opt-in via the constructor
        // flag and is also re-applied in reset() below. JBM/RTP-packet-loss
        // handling remain future work (see README).
        EVS_EncOptions opts;
        evs_enc_options_init(&opts);
        opts.dtx_enable       = 1;
        opts.dtx_sid_interval = dtxSidInterval;     // 0 = variable SID (see evs_api.h)
        opts.rf_enable        = 0;
        opts.sc_vbr_enable    = scVbrEnabled ? 1 : 0;
        enc = evs_enc_create_ex(sampleRate, bitrateBps, maxBw, &opts);
        if (!enc) {
            // Refuse cleanly: if DTX is rejected for some reason (e.g. an
            // unsupported configuration we didn't anticipate) fall back to
            // the legacy options so the codec still encodes.
            enc = evs_enc_create(sampleRate, bitrateBps, maxBw);
        }
        dec = evs_dec_create(sampleRate, bitrateBps);
        bitstream.resize(evs_max_bitstream_bytes(sampleRate));
#endif
    }

    EVSCodec::~EVSCodec() {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        if (enc) evs_enc_destroy(enc);
        if (dec) evs_dec_destroy(dec);
#endif
    }

    void EVSCodec::reset() {
        fallbackPLC.reset(getFrameSize());
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        if (enc) { evs_enc_destroy(enc); }
        if (dec) { evs_dec_destroy(dec); dec = evs_dec_create(sampleRate, bitrateBps); }

        // Re-create the encoder with the same DTX options used in the ctor.
        EVS_EncOptions opts;
        evs_enc_options_init(&opts);
        opts.dtx_enable       = 1;
        opts.dtx_sid_interval = dtxSidInterval;
        opts.rf_enable        = 0;
        opts.sc_vbr_enable    = scVbrEnabled ? 1 : 0;
        enc = evs_enc_create_ex(sampleRate, bitrateBps, maxBw, &opts);
        if (!enc) {
            enc = evs_enc_create(sampleRate, bitrateBps, maxBw);
        }
#endif
    }

    void EVSCodec::setDtxSidInterval(int interval) {
        // Same normalization as the ctor: 0 (variable) or 3..100 (fixed
        // frames). Anything else collapses to 0 so evs_enc_create_ex never
        // rejects the new configuration.
        const int normalized = (interval == 0) ? 0
                              : ((interval >= 3 && interval <= 100) ? interval : 0);
        if (normalized == dtxSidInterval) {
            return;
        }
        dtxSidInterval = normalized;
        // The encoder was created with the previous dtx_sid_interval; the
        // only way to push a new value is to tear it down and recreate it
        // through reset(), which re-reads dtxSidInterval.
        reset();
    }

void EVSCodec::processFrame(const int16_t* in, int16_t* out, bool packetLost) {
        const int fs = getFrameSize();
        if (packetLost) {
#ifndef TELEPHONY_DISTRIBUTION_BUILD
            int n = 0;
            if (dec && evs_dec_process_lost(dec, out, &n) == EVS_OK && n == fs) {
                fallbackPLC.storeGoodFrame(out, fs);
                return;
            }
#endif
            fallbackPLC.conceal(out, fs);
            return;
        }

        if (!enc || !dec) {
            std::memcpy(out, in, fs * sizeof(int16_t));
            fallbackPLC.storeGoodFrame(out, fs);
            return;
        }
#ifndef TELEPHONY_DISTRIBUTION_BUILD
        int used = 0;
        if (evs_enc_process(enc, in, fs, bitstream.data(), (int)bitstream.size(), &used) != EVS_OK) {
            // On encode failure, pass input through unchanged
            std::memcpy(out, in, fs * sizeof(int16_t));
            fallbackPLC.storeGoodFrame(out, fs);
            return;
        }
        int n = 0;
        if (evs_dec_process(dec, bitstream.data(), used, out, &n) != EVS_OK) {
            std::memcpy(out, in, fs * sizeof(int16_t));
        }
        fallbackPLC.storeGoodFrame(out, fs);
#endif
    }

} // namespace TelephonyDSP
