// Floating-point EVS wrapper (TS 26.443 v12.7.0 / v13.3.0)
//
// Provides a clean in-memory C API around the 3GPP EVS reference encoder and
// decoder. The bitstream is staged through a temporary FILE* using the same
// G.192 format the reference CLI tools use, so we reuse write_indices() and
// read_indices() without re-implementing the serialisation.

#include "evs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// 3GPP EVS reference headers (float variant)
#include "options.h"
#include "cnst.h"
#include "prot.h"
#include "stat_enc.h"
#include "stat_dec.h"

struct EVS_Encoder {
    Encoder_State* st;
    Indice*        ind_buf;     // MAX_NUM_INDICES entries
    FILE*          bitfile;     // tmpfile() roundtrip for the bitstream
};

struct EVS_Decoder {
    Decoder_State* st;
    FILE*          bitfile;
};

// MAX_BITS_PER_FRAME is defined in cnst.h (2560).
// MAX_NUM_INDICES = IND_UNUSED + 127 (also from cnst.h).

static FILE* open_temp_bitstream(void) {
    FILE* f = tmpfile();
    if (!f) {
        // tmpfile() may fail on some Windows configurations; fall back to
        // a real temp file path.
        const char* path = "evs_tandem.192";
        f = fopen(path, "w+b");
    }
    return f;
}

static int sample_rate_to_index(int sr_hz) {
    switch (sr_hz) {
        case  8000: return 0;
        case 16000: return 1;
        case 32000: return 2;
        case 48000: return 3;
        default:    return -1;
    }
}

static int bandwidth_to_enum(EVS_Bandwidth bw) {
    switch (bw) {
        case EVS_NB:  return NB;
        case EVS_WB:  return WB;
        case EVS_SWB: return SWB;
        case EVS_FB:  return FB;
        default:      return -1;
    }
}

int evs_max_bitstream_bytes(int sample_rate_hz) {
    // G.192 stream is one Word16 per bit plus a 2-word header.
    return (2 + MAX_BITS_PER_FRAME) * (int)sizeof(unsigned short);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
EVS_Encoder* evs_enc_create(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw) {
    if (sample_rate_to_index(sample_rate_hz) < 0) return NULL;
    if (bitrate_bps < 5900 || bitrate_bps > 128000) return NULL;

    EVS_Encoder* enc = (EVS_Encoder*)calloc(1, sizeof(EVS_Encoder));
    if (!enc) return NULL;

    enc->st = (Encoder_State*)calloc(1, sizeof(Encoder_State));
    enc->ind_buf = (Indice*)calloc(MAX_NUM_INDICES, sizeof(Indice));
    enc->bitfile = open_temp_bitstream();
    if (!enc->st || !enc->ind_buf || !enc->bitfile) {
        evs_enc_destroy(enc);
        return NULL;
    }

    // Mirror what io_ini_enc() would have done for the CLI, but only the
    // fields actually consulted by init_encoder() and evs_enc().
    enc->st->input_Fs        = sample_rate_hz;
    enc->st->total_brate     = bitrate_bps;
    enc->st->max_bwidth      = (short)bandwidth_to_enum(max_bw);
    enc->st->Opt_AMR_WB      = 0;          // 0 = native EVS (not AMR-WB IO)
    enc->st->Opt_DTX_ON      = 0;
    enc->st->Opt_RF_ON       = 0;
    enc->st->Opt_SC_VBR      = 0;
    enc->st->rf_fec_offset   = 0;
    enc->st->rf_fec_indicator= 1;
    enc->st->interval_SID    = FIXED_SID_RATE;
    enc->st->var_SID_rate_flag = 1;
    enc->st->bitstreamformat = G192;
    enc->st->ind_list        = enc->ind_buf;

    // Pick codec mode the way the CLI does. The values follow cnst.h.
    //   - 5.90k = SC-VBR (we treat as constant; not used here)
    //   - 13.2k RF/LO = MODE2
    //   - bitrates that need MODE2: 13.2k (HQ partial), 16.4k WB TBE, 24.4k WB BWE, 32k SWB TBE, 48k SWB BWE, 64k FB TBE, 96k FB BWE, 128k
    //   - everything else at this point is MODE1
    // Always start with MODE1; the encoder will adjust if needed.
    enc->st->codec_mode = MODE1;
    enc->st->last_codec_mode = enc->st->codec_mode;

    // Clamp max_bwidth to what the sample rate can carry (CLI does this).
    if (sample_rate_hz ==  8000 && enc->st->max_bwidth > NB)  enc->st->max_bwidth = NB;
    if (sample_rate_hz == 16000 && enc->st->max_bwidth > WB)  enc->st->max_bwidth = WB;
    if (sample_rate_hz == 32000 && enc->st->max_bwidth > SWB) enc->st->max_bwidth = SWB;

    init_encoder(enc->st);

    return enc;
}

void evs_enc_destroy(EVS_Encoder* enc) {
    if (!enc) return;
    if (enc->st) {
        destroy_encoder(enc->st);
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
    if (!enc || !enc->st || !pcm_in || !bitstream_out || !bitstream_used) return EVS_ERROR;
    if (n_samples != enc->st->input_Fs / 50) return EVS_ERROR;

    evs_enc(enc->st, pcm_in, (short)n_samples);

    // The encoder filled st->ind_list via push_indice(). write_indices()
    // serialises that into the G.192 format into our temp file.
    UWord8 pFrame[(MAX_BITS_PER_FRAME + 7) >> 3];
    Word16 pFrame_size = 0;

    if (enc->st->bitstreamformat == MIME) {
        indices_to_serial(enc->st, pFrame, &pFrame_size);
    }

    rewind(enc->bitfile);
    write_indices(enc->st, enc->bitfile, pFrame, pFrame_size);
    fflush(enc->bitfile);

    // Copy the G.192 stream out as a flat byte buffer.
    rewind(enc->bitfile);
    int need = (2 + MAX_BITS_PER_FRAME) * (int)sizeof(unsigned short);
    if (bitstream_max < need) return EVS_ERROR;

    unsigned short* stream = (unsigned short*)bitstream_out;
    size_t read = fread(stream, sizeof(unsigned short), 2 + MAX_BITS_PER_FRAME, enc->bitfile);
    *bitstream_used = (int)read * (int)sizeof(unsigned short);
    if (read < 2) return EVS_ERROR;

    return EVS_OK;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
EVS_Decoder* evs_dec_create(int sample_rate_hz, int bitrate_bps) {
    if (sample_rate_to_index(sample_rate_hz) < 0) return NULL;
    if (bitrate_bps < 5900 || bitrate_bps > 128000) return NULL;

    EVS_Decoder* dec = (EVS_Decoder*)calloc(1, sizeof(EVS_Decoder));
    if (!dec) return NULL;

    dec->st = (Decoder_State*)calloc(1, sizeof(Decoder_State));
    dec->bitfile = open_temp_bitstream();
    if (!dec->st || !dec->bitfile) {
        evs_dec_destroy(dec);
        return NULL;
    }

    dec->st->output_Fs       = sample_rate_hz;
    dec->st->total_brate     = bitrate_bps;
    dec->st->Opt_AMR_WB      = 0;
    dec->st->bitstreamformat = G192;
    dec->st->bfi             = 0;
    dec->st->prev_bfi        = 0;
    dec->st->codec_mode      = MODE1;        // init_decoder tolerates this

    init_decoder(dec->st);

    return dec;
}

void evs_dec_destroy(EVS_Decoder* dec) {
    if (!dec) return;
    if (dec->st) {
        destroy_decoder(dec->st);
        free(dec->st);
    }
    if (dec->bitfile) fclose(dec->bitfile);
    free(dec);
}

int evs_dec_process(EVS_Decoder* dec,
                    const unsigned char* bitstream_in, int bitstream_len,
                    short* pcm_out, int* n_samples) {
    if (!dec || !dec->st || !bitstream_in || !pcm_out || !n_samples) return EVS_ERROR;
    if (bitstream_len <= 0) return EVS_ERROR;

    rewind(dec->bitfile);
    if ((int)fwrite(bitstream_in, 1, bitstream_len, dec->bitfile) != bitstream_len) return EVS_ERROR;
    fflush(dec->bitfile);
    rewind(dec->bitfile);

    dec->st->bfi = 0;

    short ok = read_indices(dec->st, dec->bitfile, 0);
    if (!ok) return EVS_ERROR;

    float* out = (float*)calloc(dec->st->output_Fs / 50, sizeof(float));
    if (!out) return EVS_ERROR;

    evs_dec(dec->st, out, FRAMEMODE_NORMAL);

    int N = dec->st->output_Fs / 50;
    for (int i = 0; i < N; ++i) {
        float v = out[i];
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (short)v;
    }
    *n_samples = N;
    free(out);
    return EVS_OK;
}

int evs_dec_process_lost(EVS_Decoder* dec,
                         short* pcm_out, int* n_samples) {
    if (!dec || !dec->st || !pcm_out || !n_samples) return EVS_ERROR;

    int N = dec->st->output_Fs / 50;
    float* out = (float*)calloc(N, sizeof(float));
    if (!out) return EVS_ERROR;

    dec->st->bfi = 1;
    if (dec->st->codec_mode == 0 && dec->st->last_codec_mode != 0) {
        dec->st->codec_mode = dec->st->last_codec_mode;
    }

    evs_dec(dec->st, out, FRAMEMODE_MISSING);

    for (int i = 0; i < N; ++i) {
        float v = out[i];
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (short)v;
    }
    *n_samples = N;
    free(out);
    return EVS_OK;
}
