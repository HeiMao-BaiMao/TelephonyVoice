/* In-memory adapter for the complete official TS 26.442 v16.4.0 reference.
 * No parent-side DSP replacements or floating-point fallback are used. */
#include "evs_api.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "options.h"
#include "prot_fx.h"

struct EVS_Encoder {
    Encoder_State_fx *st;
    Indice_fx *ind_buf;
    EVS_EncOptions options;
    int bitrate_bps;
    int rf_on, rf_offset, rf_hi;
    int last_num_bits, last_sid_update, last_sid_mode;
};
struct EVS_Decoder {
    Decoder_State_fx *st;
    UWord16 bit_stream[MAX_BITS_PER_FRAME + 16];
    Word16 pcm_buf[L_FRAME48k];
    int amr_wb_io;
};

static int valid_sample_rate(int sr) {
    return sr == 8000 || sr == 16000 || sr == 32000 || sr == 48000;
}
static int amr_wb_mode_from_rate(int bitrate) {
    static const int rates[] = {6600,8850,12650,14250,15850,18250,19850,23050,23850};
    int i;
    for (i = 0; i < 9; ++i) if (bitrate == rates[i]) return i;
    return -1;
}
static int valid_native_frame_rate(int rate) {
    switch (rate) {
        case 0: case 2400: case 2800: case 7200: case 8000: case 9600:
        case 13200: case 16400: case 24400: case 32000: case 48000:
        case 64000: case 96000: case 128000: return 1;
        default: return 0;
    }
}
static Word16 codec_mode_for_rate(int bitrate) {
    switch (bitrate) {
        case 9600: case 16400: case 24400: case 48000: case 96000: case 128000:
            return MODE2;
        default: return MODE1;
    }
}
int evs_max_bitstream_bytes(int sample_rate_hz) {
    (void)sample_rate_hz;
    return (2 + MAX_BITS_PER_FRAME) * (int)sizeof(uint16_t);
}

EVS_Encoder* evs_enc_create(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw) {
    return evs_enc_create_ex(sample_rate_hz, bitrate_bps, max_bw, NULL);
}
EVS_Encoder* evs_enc_create_ex(int sr, int bitrate, EVS_Bandwidth bw,
                               const EVS_EncOptions* opts) {
    EVS_EncOptions options;
    EVS_Encoder *enc;
    evs_enc_options_init(&options);
    if (opts) options = *opts;
    if (evs_enc_normalize_config(sr, &bitrate, &bw, &options) != EVS_OK) return NULL;
    enc = (EVS_Encoder*)calloc(1, sizeof(*enc));
    if (!enc) return NULL;
    enc->st = (Encoder_State_fx*)calloc(1, sizeof(*enc->st));
    enc->ind_buf = (Indice_fx*)calloc(MAX_NUM_INDICES, sizeof(*enc->ind_buf));
    if (!enc->st || !enc->ind_buf) {
        free(enc->ind_buf); free(enc->st); free(enc); return NULL;
    }
    enc->st->input_Fs_fx = sr;
    enc->st->input_frame_fx = (Word16)(sr / 50);
    enc->st->ind_list_fx = enc->ind_buf;
    enc->st->bitstreamformat = G192;
    enc->last_sid_mode = -1;
    evs_enc_reconfigure(enc, bitrate, bw, &options);
    enc->st->last_codec_mode = enc->st->codec_mode;
    enc->st->last_Opt_SC_VBR_fx = enc->st->Opt_SC_VBR_fx;
    init_encoder_fx(enc->st);
    return enc;
}
void evs_enc_destroy(EVS_Encoder* enc) {
    if (!enc) return;
    if (enc->st) { destroy_encoder_fx(enc->st); free(enc->st); }
    free(enc->ind_buf);
    free(enc);
}

void evs_enc_set_rf(EVS_Encoder* enc, int on, int offset, int hi) {
    if (!enc || !enc->st) return;
    if (on && (enc->bitrate_bps != 13200 || enc->st->input_Fs_fx < 16000 ||
               enc->st->max_bwidth_fx == NB || enc->options.amr_wb_io)) on = 0;
    if (on && offset != 0 && offset != 2 && offset != 3 && offset != 5 && offset != 7) return;
    enc->rf_on = on != 0;
    enc->rf_offset = on ? (offset ? offset : FEC_OFFSET) : 0;
    enc->rf_hi = on ? hi != 0 : 1;
    if (enc->st->Opt_RF_ON != (on != 0)) reset_rf_indices(enc->st);
    if (enc->bitrate_bps == 13200) enc->st->codec_mode = on ? MODE2 : MODE1;
    enc->st->Opt_RF_ON = (Word16)(on != 0);
    enc->st->rf_fec_offset = (Word16)enc->rf_offset;
    enc->st->rf_fec_indicator = (Word16)enc->rf_hi;
}
int evs_enc_reconfigure(EVS_Encoder* enc, int bitrate, EVS_Bandwidth bw,
                        const EVS_EncOptions* opts) {
    EVS_EncOptions options;
    if (!enc || !enc->st) return EVS_ERROR;
    options = opts ? *opts : enc->options;
    if (evs_enc_normalize_config(enc->st->input_Fs_fx, &bitrate, &bw, &options) != EVS_OK)
        return EVS_ERROR;
    /* Reapplying an unchanged UI configuration must not reset adaptive SID
     * timing or the codec's internal mode changes during comfort noise. */
    if (enc->bitrate_bps == bitrate && enc->st->max_bwidth_fx == (Word16)bw &&
        memcmp(&enc->options, &options, sizeof(options)) == 0) return EVS_OK;
    enc->options = options;
    enc->bitrate_bps = bitrate;
    enc->st->total_brate_fx = options.sc_vbr_enable ? ACELP_7k20 : bitrate;
    enc->st->max_bwidth_fx = (Word16)bw;
    enc->st->Opt_AMR_WB_fx = (Word16)options.amr_wb_io;
    enc->st->Opt_SC_VBR_fx = (Word16)options.sc_vbr_enable;
    enc->st->Opt_DTX_ON_fx = (Word16)options.dtx_enable;
    enc->st->var_SID_rate_flag_fx = options.dtx_sid_interval == 0;
    enc->st->interval_SID_fx = (Word16)(options.dtx_sid_interval ? options.dtx_sid_interval : FIXED_SID_RATE);
    enc->st->codec_mode = codec_mode_for_rate(enc->st->total_brate_fx);
    evs_enc_set_rf(enc, options.rf_enable, options.rf_fec_offset, options.rf_fec_hi);
    return EVS_OK;
}
int evs_enc_get_last_frame_info(const EVS_Encoder* enc, int* bits, int* update, int* mode) {
    if (!enc) return EVS_ERROR;
    if (bits) *bits = enc->last_num_bits;
    if (update) *update = enc->last_sid_update;
    if (mode) *mode = enc->last_sid_mode;
    return EVS_OK;
}
int evs_enc_process(EVS_Encoder* enc, const short* pcm, int n,
                    unsigned char* data, int capacity, int* used) {
    uint16_t stream[2 + MAX_BITS_PER_FRAME];
    int i, k, cursor = 2, count, needed;
    if (used) *used = 0;
    if (!enc || !enc->st || !pcm || !data || !used || n != enc->st->input_Fs_fx / 50)
        return EVS_ERROR;
    /* Match encoder.c: RF intent must be restored after a DTX frame. */
    evs_enc_set_rf(enc, enc->rf_on, enc->rf_offset, enc->rf_hi);
    if (enc->st->Opt_AMR_WB_fx) amr_wb_enc_fx(enc->st, pcm, (Word16)n);
    else evs_enc_fx(enc->st, pcm, (Word16)n);
    count = enc->st->nb_bits_tot_fx;
    enc->last_num_bits = count;
    enc->last_sid_update = enc->st->Opt_AMR_WB_fx && count == 35;
    enc->last_sid_mode = enc->st->Opt_AMR_WB_fx ? amr_wb_mode_from_rate(enc->st->total_brate_fx) : -1;
    needed = (2 + count) * (int)sizeof(uint16_t);
    if (count < 0 || count > MAX_BITS_PER_FRAME || capacity < needed) {
        reset_indices_enc_fx(enc->st);
        return EVS_ERROR;
    }
    stream[0] = SYNC_GOOD_FRAME;
    stream[1] = (uint16_t)count;
    for (i = 0; i < MAX_NUM_INDICES; ++i) {
        int bits = enc->ind_buf[i].nb_bits;
        uint32_t mask;
        if (bits <= 0) continue;
        if (bits > 16 || cursor + bits > 2 + count) {
            reset_indices_enc_fx(enc->st); return EVS_ERROR;
        }
        mask = UINT32_C(1) << (bits - 1);
        for (k = 0; k < bits; ++k, mask >>= 1)
            stream[cursor++] = ((uint16_t)enc->ind_buf[i].value & mask) ? G192_BIN1 : G192_BIN0;
    }
    reset_indices_enc_fx(enc->st);
    if (cursor != 2 + count) return EVS_ERROR;
    memcpy(data, stream, (size_t)needed);
    *used = needed;
    return EVS_OK;
}

EVS_Decoder* evs_dec_create(int sr, int bitrate) {
    return evs_dec_create_ex(sr, bitrate, 0);
}
EVS_Decoder* evs_dec_create_ex(int sr, int bitrate, int amr_wb_io) {
    EVS_Decoder *dec;
    if (!valid_sample_rate(sr)) return NULL;
    if (amr_wb_io ? amr_wb_mode_from_rate(bitrate) < 0 :
        (bitrate != 5900 && (!valid_native_frame_rate(bitrate) || bitrate < 7200))) return NULL;
    dec = (EVS_Decoder*)calloc(1, sizeof(*dec));
    if (!dec) return NULL;
    dec->st = (Decoder_State_fx*)calloc(1, sizeof(*dec->st));
    if (!dec->st) { free(dec); return NULL; }
    dec->amr_wb_io = amr_wb_io != 0;
    dec->st->bit_stream_fx = dec->bit_stream;
    dec->st->output_Fs_fx = sr;
    dec->st->output_frame_fx = (Word16)(sr / 50);
    dec->st->amrwb_rfc4867_flag = -1;
    dec->st->total_brate_fx = bitrate == 5900 ? ACELP_7k20 : bitrate;
    dec->st->Opt_AMR_WB_fx = (Word16)dec->amr_wb_io;
    dec->st->bitstreamformat = G192;
    init_decoder_fx(dec->st);
    return dec;
}
void evs_dec_destroy(EVS_Decoder* dec) {
    if (!dec) return;
    if (dec->st) { destroy_decoder(dec->st); free(dec->st); }
    free(dec);
}
static int decode_current(EVS_Decoder* dec, short* pcm, int* samples) {
    int n = dec->st->output_Fs_fx / 50;
    if (dec->st->Opt_AMR_WB_fx) amr_wb_dec_fx(dec->pcm_buf, dec->st);
    else evs_dec_fx(dec->st, dec->pcm_buf,
                   dec->st->codec_mode != MODE1 && dec->st->bfi_fx ? FRAMEMODE_MISSING : FRAMEMODE_NORMAL);
    if (dec->st->ini_frame_fx < MAX_FRAME_COUNTER) ++dec->st->ini_frame_fx;
    memcpy(pcm, dec->pcm_buf, (size_t)n * sizeof(*pcm));
    *samples = n;
    return EVS_OK;
}
int evs_dec_process(EVS_Decoder* dec, const unsigned char* data, int length,
                    short* pcm, int* samples) {
    unsigned char au[(MAX_BITS_PER_FRAME + 7) / 8] = {0};
    uint16_t sync, bits, word;
    int i, rate, mode;
    if (samples) *samples = 0;
    if (!dec || !dec->st || !data || !pcm || !samples || length < 4) return EVS_ERROR;
    memcpy(&sync, data, 2); memcpy(&bits, data + 2, 2);
    rate = bits * 50;
    mode = amr_wb_mode_from_rate(rate);
    if ((sync != SYNC_GOOD_FRAME && sync != SYNC_BAD_FRAME) || bits > MAX_BITS_PER_FRAME ||
        length != (2 + bits) * 2) return EVS_ERROR;
    if (dec->amr_wb_io ? (rate != 0 && rate != 1750 && mode < 0) : !valid_native_frame_rate(rate))
        return EVS_ERROR;
    if (sync == SYNC_BAD_FRAME) return evs_dec_process_lost(dec, pcm, samples);
    for (i = 0; i < bits; ++i) {
        memcpy(&word, data + (2 + i) * 2, 2);
        if (word != G192_BIN0 && word != G192_BIN1) return EVS_ERROR;
        if (word == G192_BIN1) au[i >> 3] |= (unsigned char)(0x80 >> (i & 7));
    }
    if (rate == 1750) mode = AMRWB_IO_SID;
    /* G192 keeps the reference's original bit order, unlike RTP AMR-WB. */
    read_indices_from_djb_fx(dec->st, au, (Word16)bits, (Word16)dec->amr_wb_io,
                            (Word16)(mode < 0 ? 0 : mode), 1, 0, 0);
    return decode_current(dec, pcm, samples);
}
int evs_dec_process_lost(EVS_Decoder* dec, short* pcm, int* samples) {
    unsigned char empty = 0;
    if (samples) *samples = 0;
    if (!dec || !dec->st || !pcm || !samples) return EVS_ERROR;
    read_indices_from_djb_fx(dec->st, &empty, 0, (Word16)dec->amr_wb_io, 0, 0, 0, 0);
    if (!dec->st->codec_mode && dec->st->last_codec_mode) dec->st->codec_mode = dec->st->last_codec_mode;
    return decode_current(dec, pcm, samples);
}
