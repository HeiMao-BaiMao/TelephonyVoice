#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

// ---------------------------------------------------------------------------
// 3GPP EVS C API
//
// Wraps the 3GPP EVS reference encoder/decoder (TS 26.443 v12.7.0/v13.3.0
// for the floating-point variant, TS 26.442 v16.4.0 for the fixed-point
// variant) behind a clean in-memory interface that does not require
// FILE* round-trips for the bitstream.
// ---------------------------------------------------------------------------

typedef enum {
    EVS_NB  = 0,   // Narrowband       (8 kHz)
    EVS_WB  = 1,   // Wideband         (16 kHz)
    EVS_SWB = 2,   // Super-wideband   (32 kHz)
    EVS_FB  = 3,   // Fullband         (48 kHz)
} EVS_Bandwidth;

typedef enum {
    EVS_OK    =  0,
    EVS_ERROR = -1,
} EVS_Status;

typedef struct EVS_Encoder EVS_Encoder;
typedef struct EVS_Decoder EVS_Decoder;

// ---------------------------------------------------------------------------
// Supported EVS native bitrates (bps) for the chosen bandwidth.
// 5900 denotes source-controlled VBR, requires DTX, and uses 7200 internally.
// NB coding supports at most 24400 bps.
// ---------------------------------------------------------------------------
typedef enum {
    EVS_BR_5900   = 5900,
    EVS_BR_7200   = 7200,
    EVS_BR_8000   = 8000,
    EVS_BR_9600   = 9600,
    EVS_BR_13200  = 13200,
    EVS_BR_16400  = 16400,
    EVS_BR_24400  = 24400,
    EVS_BR_32000  = 32000,
    EVS_BR_48000  = 48000,
    EVS_BR_64000  = 64000,
    EVS_BR_96000  = 96000,
    EVS_BR_128000 = 128000,
} EVS_Bitrate;

// ---------------------------------------------------------------------------
// Frame size helper.
// Returns the number of 16-bit PCM samples for one 20 ms frame at the given
// sample rate. EVS is always a 20 ms codec (50 frames per second).
// ---------------------------------------------------------------------------
static inline int evs_frame_size(int sample_rate_hz) {
    return sample_rate_hz / 50;
}

// ---------------------------------------------------------------------------
// Optional encoder configuration (DTX/CNG, RF channel-aware, SC-VBR).
//
// All fields are ignored when passed via evs_enc_create_ex with opts==NULL,
// or when using the legacy evs_enc_create entry point. Defaults match the
// reference CLI's no-extra-flag path: DTX off, RF off, SC-VBR off.
// ---------------------------------------------------------------------------
typedef struct EVS_EncOptions {
    // ---- DTX / CNG (3GPP TS 26.443 VAD/DTX/SID/CNG) ----
    //   0 = DTX disabled (default), 1 = DTX enabled
    int  dtx_enable;
    //   0          -> variable SID update interval (var_SID_rate_flag=1,
    //                 interval_SID=0). This is a pre-init contract consumed
    //                 by init_encoder(), which promotes 0 to an internal
    //                 default (~12 frames) before evs_enc() ever sees it.
    //   3..100     -> fixed SID update interval in 20 ms frames
    //                 (var_SID_rate_flag=0, interval_SID=N)
    //   other      -> evs_enc_create_ex returns NULL (caller-visible error)
    int  dtx_sid_interval;

    // ---- Channel-aware mode (RF, TS 26.443 Annex C) ----
    //   0 = RF disabled (default), 1 = RF requested
    //   Ignored safely when bitrate != 13200 bps, sample rate < 16000 Hz,
    //   or the effective bandwidth ceiling is NB,
    //   matching the reference CLI's validation in io_enc.c.
    int  rf_enable;
    //   0 -> use the reference default (FEC_OFFSET, currently 3)
    //   2, 3, 5, 7 -> use the requested FEC offset
    //   other -> evs_enc_create_ex returns NULL
    int  rf_fec_offset;
    //   0 = LO, 1 (non-zero) = HI. 1 is the reference default.
    int  rf_fec_hi;

    // ---- Source-controlled VBR (SC-VBR, 5.90 kbps mode) ----
    //   0 = SC-VBR disabled unless bitrate_bps is 5900.
    //   Non-zero explicitly selects the 5900-bps SC-VBR mode, overriding
    //   the selected native bitrate and limiting bandwidth to NB/WB.
    //   SC-VBR requires dtx_enable != 0. Internally the codec uses 7200 bps.
    int  sc_vbr_enable;
} EVS_EncOptions;

// Convenience initializer. Equivalent to a zero-initialised struct, but
// keeps the "all disabled" defaults explicit and version-portable.
static inline void evs_enc_options_init(EVS_EncOptions* opts) {
    if (!opts) return;
    opts->dtx_enable       = 0;
    opts->dtx_sid_interval = 0;
    opts->rf_enable        = 0;
    opts->rf_fec_offset    = 0;
    opts->rf_fec_hi        = 1;
    opts->sc_vbr_enable    = 0;
}

// Validate and normalize an encoder request without initializing the codec.
// This inline helper is also available to distribution-build frontends.
// On success, bitrate_bps, max_bw and (if non-NULL) opts receive the effective
// public configuration. On error, no caller-owned values are changed.
// Bandwidth is a ceiling: it is reduced to the input sample rate and the
// selected bitrate's supported bandwidth. Unsupported RF requests are disabled.
// SC-VBR is represented publicly by 5900 bps, not its internal 7200-bps rate.
static inline int evs_enc_normalize_config(int sample_rate_hz, int* bitrate_bps,
                                          EVS_Bandwidth* max_bw,
                                          EVS_EncOptions* opts) {
    int bitrate;
    EVS_Bandwidth bw, sample_bw;
    EVS_EncOptions local;
    if (!bitrate_bps || !max_bw) return EVS_ERROR;
    bitrate = *bitrate_bps;
    bw = *max_bw;
    switch (sample_rate_hz) {
        case 8000: sample_bw = EVS_NB; break;
        case 16000: sample_bw = EVS_WB; break;
        case 32000: sample_bw = EVS_SWB; break;
        case 48000: sample_bw = EVS_FB; break;
        default: return EVS_ERROR;
    }
    if (bw < EVS_NB || bw > EVS_FB) return EVS_ERROR;
    switch (bitrate) {
        case 5900: case 7200: case 8000: case 9600: case 13200:
        case 16400: case 24400: case 32000: case 48000: case 64000:
        case 96000: case 128000: break;
        default: return EVS_ERROR;
    }
    evs_enc_options_init(&local);
    if (opts) local = *opts;
    local.dtx_enable = local.dtx_enable != 0;
    local.sc_vbr_enable = local.sc_vbr_enable != 0 || bitrate == 5900;
    local.rf_enable = local.rf_enable != 0;
    local.rf_fec_hi = local.rf_fec_hi != 0;
    if (local.dtx_enable && local.dtx_sid_interval != 0 &&
        (local.dtx_sid_interval < 3 || local.dtx_sid_interval > 100)) {
        return EVS_ERROR;
    }
    if (bw > sample_bw) bw = sample_bw;
    if (local.sc_vbr_enable) {
        if (!local.dtx_enable) return EVS_ERROR;
        bitrate = 5900;
    }
    if (bitrate < 9600 && bw > EVS_WB) bw = EVS_WB;
    if (bitrate < 16400 && bw > EVS_SWB) bw = EVS_SWB;
    if (bw == EVS_NB && bitrate > 24400) return EVS_ERROR;
    if (bitrate != 13200 || sample_rate_hz < 16000 || bw == EVS_NB) {
        local.rf_enable = 0;
    }
    if (local.rf_enable && local.rf_fec_offset != 0 &&
        local.rf_fec_offset != 2 && local.rf_fec_offset != 3 &&
        local.rf_fec_offset != 5 && local.rf_fec_offset != 7) {
        return EVS_ERROR;
    }
    if (!local.rf_enable) {
        local.rf_fec_offset = 0;
        local.rf_fec_hi = 1;
    }
    *bitrate_bps = bitrate;
    *max_bw = bw;
    if (opts) *opts = local;
    return EVS_OK;
}

// ---------------------------------------------------------------------------
// Encoder lifecycle.
// sample_rate_hz : 8000, 16000, 32000, 48000
// bitrate_bps    : one of EVS_BR_*
// max_bw         : bandwidth ceiling (encoder is allowed to drop below this)
// opts           : optional encoder configuration; pass NULL to use the
//                  legacy defaults (DTX/RF/SC-VBR all off, matching the
//                  historical evs_enc_create behaviour).
// Invalid configurations return NULL. Normalization follows
// evs_enc_normalize_config; in particular 5900 requires DTX via opts.
// ---------------------------------------------------------------------------
EVS_Encoder* evs_enc_create_ex(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw,
                               const EVS_EncOptions* opts);
EVS_Encoder* evs_enc_create(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw);
void         evs_enc_destroy(EVS_Encoder* enc);

// ---------------------------------------------------------------------------
// Encode one 20 ms frame.
// pcm_in         : 16-bit linear PCM, exactly sample_rate_hz/50 samples
// bitstream_out  : caller-allocated buffer for the encoded payload
// bitstream_max  : capacity of bitstream_out in bytes
// bitstream_used : on return, number of bytes actually written (zero on error)
// If capacity is insufficient, the frame is consumed and its packet discarded;
// subsequent calls remain usable. evs_max_bitstream_bytes avoids this case.
//
// Returns EVS_OK or EVS_ERROR.
// ---------------------------------------------------------------------------
int evs_enc_process(EVS_Encoder* enc,
                    const short* pcm_in, int n_samples,
                    unsigned char* bitstream_out, int bitstream_max,
                    int* bitstream_used);

// ---------------------------------------------------------------------------
// Channel-aware (RF) runtime control.
//
// Updates the channel-aware FEC parameters on an already-created encoder
// so a JBM/VoIP control loop can push its current FEC-offset / LO-HI
// estimate into the encoder between frames. The reference CLI only sets
// these at startup via -RF, so this setter is the public mirror for
// real-time feedback.
//
// rf_on :0 = RF off, non-zero = RF on.
// rf_fec_offset :0 -> use the reference default (FEC_OFFSET,3);
//2,3,5,7 -> use the requested FEC offset;
// any other value is silently ignored (no change).
// rf_fec_indicator :0 = LO, non-zero = HI.
//
// Same bitrate/sample-rate guard as the constructor: if rf_on is
// requested but the encoder is not configured for ACELP_13k20, the input
// rate is below 16 kHz, or its bandwidth ceiling is NB, RF is turned off
// and the encoder is reset
// to the no-RF defaults (rf_fec_offset =0, rf_fec_indicator =1).
// ---------------------------------------------------------------------------
void evs_enc_set_rf(EVS_Encoder* enc, int rf_on, int rf_fec_offset, int rf_fec_indicator);

// ---------------------------------------------------------------------------
// Decoder lifecycle.
// sample_rate_hz : 8000, 16000, 32000, 48000 (output rate)
// bitrate_bps    : bitrate that the encoder was configured with
// ---------------------------------------------------------------------------
EVS_Decoder* evs_dec_create(int sample_rate_hz, int bitrate_bps);
void         evs_dec_destroy(EVS_Decoder* dec);

// ---------------------------------------------------------------------------
// Decode one 20 ms frame.
// bitstream_in   : bytes returned by evs_enc_process
// bitstream_len  : number of bytes in bitstream_in
// pcm_out        : caller-allocated buffer, sample_rate_hz/50 samples
// n_samples      : on return, number of samples produced (sample_rate_hz/50)
//
// Returns EVS_OK or EVS_ERROR.
// Only native EVS payloads are supported; AMR-WB IO packets are rejected.
// ---------------------------------------------------------------------------
int evs_dec_process(EVS_Decoder* dec,
                    const unsigned char* bitstream_in, int bitstream_len,
                    short* pcm_out, int* n_samples);

// Decode one missing 20 ms frame using the reference decoder's packet-loss
// concealment or continuing comfort noise after a SID frame, as appropriate.
// No encoded payload is supplied.
int evs_dec_process_lost(EVS_Decoder* dec,
                         short* pcm_out, int* n_samples);

// ---------------------------------------------------------------------------
// Bitstream size hints.
// ---------------------------------------------------------------------------
int evs_max_bitstream_bytes(int sample_rate_hz);

#ifdef __cplusplus
}
#endif
