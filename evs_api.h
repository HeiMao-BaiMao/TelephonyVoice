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
// The encoder's total_brate field accepts any of these. The decoder mirrors
// whatever bitrate the encoder is configured with.
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
// Encoder lifecycle.
// sample_rate_hz : 8000, 16000, 32000, 48000
// bitrate_bps    : one of EVS_BR_*
// max_bw         : bandwidth ceiling (encoder is allowed to drop below this)
// ---------------------------------------------------------------------------
EVS_Encoder* evs_enc_create(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw);
void         evs_enc_destroy(EVS_Encoder* enc);

// ---------------------------------------------------------------------------
// Encode one 20 ms frame.
// pcm_in         : 16-bit linear PCM, exactly sample_rate_hz/50 samples
// bitstream_out  : caller-allocated buffer for the encoded payload
// bitstream_max  : capacity of bitstream_out in bytes
// bitstream_used : on return, number of bytes actually written
//
// Returns EVS_OK or EVS_ERROR.
// ---------------------------------------------------------------------------
int evs_enc_process(EVS_Encoder* enc,
                    const short* pcm_in, int n_samples,
                    unsigned char* bitstream_out, int bitstream_max,
                    int* bitstream_used);

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
// ---------------------------------------------------------------------------
int evs_dec_process(EVS_Decoder* dec,
                    const unsigned char* bitstream_in, int bitstream_len,
                    short* pcm_out, int* n_samples);

// Decode one missing 20 ms frame using the reference decoder's packet-loss
// concealment path (FRAMEMODE_MISSING). No encoded payload is supplied.
int evs_dec_process_lost(EVS_Decoder* dec,
                         short* pcm_out, int* n_samples);

// ---------------------------------------------------------------------------
// Bitstream size hints.
// ---------------------------------------------------------------------------
int evs_max_bitstream_bytes(int sample_rate_hz);

#ifdef __cplusplus
}
#endif
