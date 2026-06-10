// Fixed-point EVS wrapper (TS 26.442 v12.15.0 / v13.10.0 / v14.6.0 / v15.4.0 / v16.4.0)
//
// This file is only compiled when TELEPHONY_USE_EVS_FX is enabled.
// Mirrors evs_api.c (float variant) but uses the Word16 / _fx-flavoured state
// structures and the fixed-point reference functions (init_encoder_fx,
// evs_enc_fx, init_decoder_fx, evs_dec_fx).
//
// The fixed-point reference shares the same G.192 bitstream format with the
// floating-point reference, so we re-use the same flat uint16_t encoding on
// top of write_indices_fx() / read_indices_fx() via a temporary FILE*.

#include "evs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 3GPP EVS reference headers (fixed-point variant).
// The cmake file emits empty stat_com.h / cnst.h redirect stubs into the
// first include directory so that prot_fx.h's include of stat_com.h and
// stat_com.h's transitive include of cnst.h do not pull in the float
// structures / constants that clash with cnst_fx.h.
#include "options.h"
#include "cnst_fx.h"
#include "prot_fx.h"
#include "stat_enc_fx.h"
#include "stat_dec_fx.h"

struct EVS_Encoder {
    Encoder_State_fx* st;
    Indice_fx*        ind_buf;   // MAX_NUM_INDICES entries
    FILE*             bitfile;   // tmpfile() roundtrip for the bitstream
};

struct EVS_Decoder {
    Decoder_State_fx* st;
    FILE*             bitfile;
};

// MAX_BITS_PER_FRAME is defined in cnst_fx.h (2560).
// MAX_NUM_INDICES = IND_UNUSED + 127 (also from cnst_fx.h).
// G192 / MIME / ACELP_13k20 / FEC_OFFSET / FIXED_SID_RATE / MODE1 / MODE2
// are all exposed by cnst_fx.h.

static FILE* open_temp_bitstream(void) {
    FILE* f = tmpfile();
    if (!f) {
        // tmpfile() may fail on some Windows configurations; fall back to
        // a real temp file path (same fallback as the float wrapper).
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
    (void)sample_rate_hz;
    return (2 + MAX_BITS_PER_FRAME) * (int)sizeof(unsigned short);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
EVS_Encoder* evs_enc_create(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw) {
    // Backward-compatible thin wrapper: historical behaviour had DTX / RF /
    // SC-VBR all disabled, matching the reference CLI's no-extra-flag path.
    return evs_enc_create_ex(sample_rate_hz, bitrate_bps, max_bw, NULL);
}

EVS_Encoder* evs_enc_create_ex(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw,
                               const EVS_EncOptions* opts) {
    if (sample_rate_to_index(sample_rate_hz) < 0) return NULL;
    if (bitrate_bps < 5900 || bitrate_bps > 128000) return NULL;

    // Resolve the effective options (defaults match the historical
    // evs_enc_create path: DTX / RF / SC-VBR all off, variable SID).
    EVS_EncOptions local;
    evs_enc_options_init(&local);
    if (opts) local = *opts;

    // ---- DTX validation: only specific intervals are acceptable ----
    if (local.dtx_enable) {
        if (local.dtx_sid_interval == 0) {
            // variable SID update interval (codec picks per-frame)
        } else if (local.dtx_sid_interval >= 3 && local.dtx_sid_interval <= 100) {
            // fixed SID update interval
        } else {
            // Out-of-range: fail cleanly (mirrors io_ini_enc_fx usage_enc()).
            return NULL;
        }
    }

    // ---- RF validation: only 13.2 kbps and >= 16 kHz input are accepted ----
    if (local.rf_enable && (bitrate_bps != ACELP_13k20 || sample_rate_hz < 16000)) {
        // Disabling safely matches the reference CLI's "Reset RF parameters
        // if NB input_Fs" / "channel-aware mode is supported only at 13.20"
        // paths. Emit the reference CLI's diagnostic so callers notice the
        // silent downgrade of their request.
        fprintf(stderr,
                "Warning: Channel-aware mode only available for 13.2 kbps WB/SWB\n"
                "Switched to normal mode!\n");
        local.rf_enable = 0;
    }
    if (local.rf_enable && local.rf_fec_offset != 0) {
        if (local.rf_fec_offset != 2 && local.rf_fec_offset != 3 &&
            local.rf_fec_offset != 5 && local.rf_fec_offset != 7) {
            return NULL;
        }
    }

    // ---- SC-VBR validation: the reference CLI only enables it for 5.90 kbps
    // and forces total_brate up to 7.20 kbps. Keep the caller's bitrate as-is
    // here (we are a wrapper, not a CLI), but the encoder will still respect
    // the bitrate we pass. We honour the flag conservatively: if enabled
    // together with DTX we leave DTX as requested; the reference handles the
    // interaction internally. ----

    EVS_Encoder* enc = (EVS_Encoder*)calloc(1, sizeof(EVS_Encoder));
    if (!enc) return NULL;

    enc->st = (Encoder_State_fx*)calloc(1, sizeof(Encoder_State_fx));
    enc->ind_buf = (Indice_fx*)calloc(MAX_NUM_INDICES, sizeof(Indice_fx));
    enc->bitfile = open_temp_bitstream();
    if (!enc->st || !enc->ind_buf || !enc->bitfile) {
        evs_enc_destroy(enc);
        return NULL;
    }

    // Mirror what io_ini_enc_fx() would have done for the CLI, but only the
    // fields actually consulted by init_encoder_fx() and evs_enc_fx().
    // Encoder_State_fx uses _fx-suffixed field names for the config block
    // and bare names for the RF block (see stat_enc_fx.h).
    enc->st->input_Fs_fx        = sample_rate_hz;
    enc->st->total_brate_fx     = bitrate_bps;
    enc->st->max_bwidth_fx      = (Word16)bandwidth_to_enum(max_bw);
    enc->st->Opt_AMR_WB_fx      = 0;            // 0 = native EVS (not AMR-WB IO)
    enc->st->bitstreamformat    = G192;
    enc->st->ind_list_fx        = enc->ind_buf;

    // ---- Apply DTX/CNG options (must be set before init_encoder_fx()) ----
    enc->st->Opt_DTX_ON_fx = local.dtx_enable ? 1 : 0;
    if (enc->st->Opt_DTX_ON_fx) {
        if (local.dtx_sid_interval == 0) {
            enc->st->var_SID_rate_flag_fx = 1;
            enc->st->interval_SID_fx      = 0;
        } else {
            enc->st->var_SID_rate_flag_fx = 0;
            enc->st->interval_SID_fx      = (Word16)local.dtx_sid_interval;
        }
    } else {
        // Keep reference defaults for the disabled path; this matches what
        // io_ini_enc_fx() does when -DTX is not passed.
        enc->st->var_SID_rate_flag_fx = 1;
        enc->st->interval_SID_fx      = FIXED_SID_RATE;
    }

    // ---- Apply RF options (RF fields are NOT _fx-suffixed) ----
    enc->st->Opt_RF_ON        = local.rf_enable ? 1 : 0;
    // rf_fec_indicator only carries meaning when RF is on (it selects LO/HI
    // for the channel-aware mode). When RF is off, match the legacy/reference
    // default of 1 so the encoder state matches what io_ini_enc_fx() would
    // have produced for the no-RF path.
    enc->st->rf_fec_indicator = enc->st->Opt_RF_ON ? (local.rf_fec_hi ? 1 : 0) : 1;
    if (enc->st->Opt_RF_ON) {
        // 0 in opts => fall back to the reference default (FEC_OFFSET, 3).
        enc->st->rf_fec_offset = (Word16)((local.rf_fec_offset == 0)
                                          ? FEC_OFFSET
                                          : local.rf_fec_offset);
    } else {
        enc->st->rf_fec_offset = 0;
    }

    // ---- Apply SC-VBR option ----
    enc->st->Opt_SC_VBR_fx = local.sc_vbr_enable ? 1 : 0;

    // Pick codec mode the way the CLI does.  Always start with MODE1; the
    // encoder will adjust if needed (MODE1 vs MODE2 decision is internal).
    enc->st->codec_mode      = MODE1;
    enc->st->last_codec_mode = enc->st->codec_mode;

    // Clamp max_bwidth to what the sample rate can carry (CLI does this).
    if (sample_rate_hz ==  8000 && enc->st->max_bwidth_fx > NB)  enc->st->max_bwidth_fx = NB;
    if (sample_rate_hz == 16000 && enc->st->max_bwidth_fx > WB)  enc->st->max_bwidth_fx = WB;
    if (sample_rate_hz == 32000 && enc->st->max_bwidth_fx > SWB) enc->st->max_bwidth_fx = SWB;

    init_encoder_fx(enc->st);

    return enc;
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

void evs_enc_set_rf(EVS_Encoder* enc, int rf_on, int rf_fec_offset, int rf_fec_indicator) {
    if (!enc || !enc->st) return;

    // Same bitrate/sample-rate guard as the constructor: if rf_on is
    // requested but the encoder is not at ACELP_13k20 or the input rate is
    // below 16 kHz, rf_on is silently turned off and the encoder is reset
    // to the no-RF defaults (rf_fec_offset = 0, rf_fec_indicator = 1).
    if (rf_on && (enc->st->total_brate_fx != ACELP_13k20 || enc->st->input_Fs_fx < 16000)) {
        rf_on = 0;
    }

    enc->st->Opt_RF_ON = rf_on ? 1 : 0;

    if (rf_on) {
        enc->st->rf_fec_indicator = rf_fec_indicator ? 1 : 0;

        if (rf_fec_offset == 0 || rf_fec_offset == 2 || rf_fec_offset == 3 ||
            rf_fec_offset == 5 || rf_fec_offset == 7) {
            enc->st->rf_fec_offset = (Word16)((rf_fec_offset == 0)
                                              ? FEC_OFFSET
                                              : rf_fec_offset);
        }
    } else {
        enc->st->rf_fec_offset    = 0;
        enc->st->rf_fec_indicator = 1;
    }
}

int evs_enc_process(EVS_Encoder* enc,
                    const short* pcm_in, int n_samples,
                    unsigned char* bitstream_out, int bitstream_max,
                    int* bitstream_used) {
    if (!enc || !enc->st || !pcm_in || !bitstream_out || !bitstream_used) return EVS_ERROR;
    if (n_samples != (int)(enc->st->input_Fs_fx / 50)) return EVS_ERROR;

    evs_enc_fx(enc->st, (const Word16*)pcm_in, (Word16)n_samples);

    // The encoder filled st->ind_list_fx via push_indice_fx() /
    // push_next_indice_fx(). write_indices_fx() serialises that into the
    // G.192 format into our temp file.
    UWord8 pFrame[(MAX_BITS_PER_FRAME + 7) >> 3];
    Word16 pFrame_size = 0;

    if (enc->st->bitstreamformat == MIME) {
        // No direct indices_to_serial() equivalent in the FX reference; for
        // the G.192 path (our default) write_indices_fx() reads ind_list_fx
        // directly so we leave pFrame empty.
    }

    rewind(enc->bitfile);
    write_indices_fx(enc->st, enc->bitfile, pFrame, pFrame_size);
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

    dec->st = (Decoder_State_fx*)calloc(1, sizeof(Decoder_State_fx));
    dec->bitfile = open_temp_bitstream();
    if (!dec->st || !dec->bitfile) {
        evs_dec_destroy(dec);
        return NULL;
    }

    dec->st->output_Fs_fx       = sample_rate_hz;
    dec->st->total_brate_fx     = bitrate_bps;
    dec->st->bitstreamformat    = G192;
    dec->st->bfi_fx             = 0;
    dec->st->prev_bfi_fx        = 0;
    dec->st->codec_mode         = MODE1;        // init_decoder_fx tolerates this

    init_decoder_fx(dec->st);

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

    dec->st->bfi_fx = 0;

    Word16 ok = read_indices_fx(dec->st, dec->bitfile, 0);
    if (!ok) return EVS_ERROR;

    int N = (int)(dec->st->output_Fs_fx / 50);
    Word16* out = (Word16*)calloc((size_t)N, sizeof(Word16));
    if (!out) return EVS_ERROR;

    evs_dec_fx(dec->st, out, FRAMEMODE_NORMAL);

    // The fixed-point reference produces Q0 PCM samples that already fit
    // in Word16 (the reference's own resample / de-emphasis chain takes
    // care of scaling).  Copy straight across with a saturation guard for
    // safety.
    for (int i = 0; i < N; ++i) {
        Word32 v = (Word32)out[i];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        pcm_out[i] = (short)v;
    }
    *n_samples = N;
    free(out);
    return EVS_OK;
}

int evs_dec_process_lost(EVS_Decoder* dec,
                         short* pcm_out, int* n_samples) {
    if (!dec || !dec->st || !pcm_out || !n_samples) return EVS_ERROR;

    int N = (int)(dec->st->output_Fs_fx / 50);
    Word16* out = (Word16*)calloc((size_t)N, sizeof(Word16));
    if (!out) return EVS_ERROR;

    dec->st->bfi_fx = 1;
    if (dec->st->codec_mode == 0 && dec->st->last_codec_mode != 0) {
        dec->st->codec_mode = dec->st->last_codec_mode;
    }

    evs_dec_fx(dec->st, out, FRAMEMODE_MISSING);

    for (int i = 0; i < N; ++i) {
        Word32 v = (Word32)out[i];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        pcm_out[i] = (short)v;
    }
    *n_samples = N;
    free(out);
    return EVS_OK;
}
