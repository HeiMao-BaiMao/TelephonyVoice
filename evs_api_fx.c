// Fixed-point EVS wrapper (TS 26.442 v12.15.0 / v13.10.0 / v14.6.0 / v15.4.0 / v16.4.0)
//
// This file is only compiled when TELEPHONY_USE_EVS_FX is enabled.
// Mirrors evs_api.c but uses the Word16 / _fx-flavoured state structures and
// the fixed-point reference functions (init_encoder_fx, evs_enc_fx, ...).
//
// Status: scaffolding. The fixed-point reference shares the same CLI bitstream
// format (G.192) as the float version, so most of the encoding/decoding
// plumbing carries over. The Q-format arithmetic inside the codec is handled
// entirely by the reference itself; we just need to set up the state.

#include "evs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"
#include "cnst_fx.h"
#include "prot_fx.h"
#include "stat_enc_fx.h"
#include "stat_dec_fx.h"

struct EVS_Encoder {
    Encoder_State_fx* st;
    Word16*          ind_buf;
    FILE*            bitfile;
};

struct EVS_Decoder {
    Decoder_State_fx* st;
    FILE*             bitfile;
};

int evs_max_bitstream_bytes(int sample_rate_hz) {
    return (2 + MAX_BITS_PER_FRAME) * (int)sizeof(unsigned short);
}

EVS_Encoder* evs_enc_create(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw) {
    (void)sample_rate_hz; (void)bitrate_bps; (void)max_bw;
    fprintf(stderr, "evs_enc_create: fixed-point variant not yet wired up. "
                    "Re-run cmake with TELEPHONY_USE_EVS_FX=OFF or implement "
                    "this wrapper mirroring evs_api.c.\n");
    return NULL;
}

void evs_enc_destroy(EVS_Encoder* enc) {
    if (!enc) return;
    if (enc->st) {
        destroy_encoder_fx(enc->st);
        free(enc->st);
    }
    if (enc->ind_buf) free(enc->ind_buf);
    if (enc->bitfile) fclose(enc->bitfile);
    free(enc);
}

int evs_enc_process(EVS_Encoder* enc,
                    const short* pcm_in, int n_samples,
                    unsigned char* bitstream_out, int bitstream_max,
                    int* bitstream_used) {
    (void)enc; (void)pcm_in; (void)n_samples;
    (void)bitstream_out; (void)bitstream_max; (void)bitstream_used;
    return EVS_ERROR;
}

EVS_Decoder* evs_dec_create(int sample_rate_hz, int bitrate_bps) {
    (void)sample_rate_hz; (void)bitrate_bps;
    return NULL;
}

void evs_dec_destroy(EVS_Decoder* dec) {
    if (!dec) return;
    if (dec->st) {
        destroy_decoder_fx(dec->st);
        free(dec->st);
    }
    if (dec->bitfile) fclose(dec->bitfile);
    free(dec);
}

int evs_dec_process(EVS_Decoder* dec,
                    const unsigned char* bitstream_in, int bitstream_len,
                    short* pcm_out, int* n_samples) {
    (void)dec; (void)bitstream_in; (void)bitstream_len;
    (void)pcm_out; (void)n_samples;
    return EVS_ERROR;
}
