// Floating-point EVS wrapper (TS 26.443 v12.7.0 / v13.3.0)
//
// Provides a clean in-memory C API around the 3GPP EVS reference encoder and
// decoder. The wire format between evs_enc_process() and evs_dec_process()
// stays the ITU-T G.192 word stream the reference CLI tools use, but the
// serialisation is done entirely in memory: the encoder replicates the G192
// branch of write_indices() into the caller's buffer, and the decoder feeds
// read_indices_from_djb() with the repacked compact access unit. No FILE*
// round-trip is involved, so this path performs no file I/O per frame (it
// previously staged every frame through tmpfile(), which is both unsafe on
// a real-time audio thread and broken on Windows for non-admin users).

#include "evs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

// 3GPP EVS reference headers (float variant)
#include "options.h"
#include "cnst.h"
#include "prot.h"
#include "stat_enc.h"
#include "stat_dec.h"

struct EVS_Encoder {
    Encoder_State* st;
    Indice*        ind_buf;     // MAX_NUM_INDICES entries
    int            bitrate_bps; // normalized public rate, stable during DTX
    int            rf_on;      // requested RF state, reapplied after SID frames
    int            rf_offset;
    int            rf_hi;
    EVS_EncOptions options;
    int last_num_bits, last_sid_update, last_sid_mode;
};

struct EVS_Decoder {
    Decoder_State* st;
    float*         pcm_buf;     // L_FRAME48k workspace, reused every frame
};

// MAX_BITS_PER_FRAME is defined in cnst.h (2560).
// MAX_NUM_INDICES = IND_UNUSED + 127 (also from cnst.h).

// Frame lengths that map to a defined EVS primary mode.
// Mirrors the switch in the reference rate2EVSmode() (lib_com/bitstream.c),
// which is static there, so the accepted set is restated here. Used to
// reject corrupt G.192 headers before they reach decoder_selectCodec().
static int is_valid_g192_rate(long rate) {
    switch (rate) {
        // EVS primary modes
        case 1750: case 6600: case 8850: case 12650: case 14250:
        case 15850: case 18250: case 19850: case 23050: case 23850:
        case 0:      case 2400:  case 2800:  case 7200:  case 8000:
        case 9600:   case 13200: case 16400: case 24400: case 32000:
        case 48000:  case 64000: case 96000: case 128000:
            return 1;
        default:
            return 0;
    }
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
    // Backward-compatible thin wrapper: historical behaviour had DTX / RF /
    // SC-VBR all disabled, matching the reference CLI's no-extra-flag path.
    return evs_enc_create_ex(sample_rate_hz, bitrate_bps, max_bw, NULL);
}

EVS_Encoder* evs_enc_create_ex(int sample_rate_hz, int bitrate_bps, EVS_Bandwidth max_bw,
                               const EVS_EncOptions* opts) {
    // Validate and normalize before allocating reference-code state. The
    // frontend uses the same helper so its effective settings stay coherent.
    EVS_EncOptions local;
    evs_enc_options_init(&local);
    if (opts) local = *opts;
    if (evs_enc_normalize_config(sample_rate_hz, &bitrate_bps, &max_bw, &local)
        != EVS_OK) return NULL;

    EVS_Encoder* enc = (EVS_Encoder*)calloc(1, sizeof(EVS_Encoder));
    if (!enc) return NULL;

    enc->st = (Encoder_State*)calloc(1, sizeof(Encoder_State));
    enc->ind_buf = (Indice*)calloc(MAX_NUM_INDICES, sizeof(Indice));
    if (!enc->st || !enc->ind_buf) {
        free(enc->ind_buf);
        free(enc->st);
        free(enc);
        return NULL;
    }

    // Mirror what io_ini_enc() would have done for the CLI, but only the
    // fields actually consulted by init_encoder() and evs_enc().
    enc->st->input_Fs        = sample_rate_hz;
    enc->bitrate_bps         = bitrate_bps;
    enc->st->total_brate     = local.sc_vbr_enable ? ACELP_7k20 : bitrate_bps;
    enc->st->max_bwidth      = (short)bandwidth_to_enum(max_bw);
    enc->st->Opt_AMR_WB      = local.amr_wb_io;
    enc->options             = local;
    enc->last_sid_mode       = -1;
    enc->st->bitstreamformat = G192;
    enc->st->ind_list        = enc->ind_buf;

    // ---- Apply DTX/CNG options (must be set before init_encoder()) ----
    enc->st->Opt_DTX_ON = local.dtx_enable ? 1 : 0;
    if (enc->st->Opt_DTX_ON) {
        if (local.dtx_sid_interval == 0) {
            enc->st->var_SID_rate_flag = 1;
            enc->st->interval_SID      = 0;
        } else {
            enc->st->var_SID_rate_flag = 0;
            enc->st->interval_SID      = (short)local.dtx_sid_interval;
        }
    } else {
        // Keep reference defaults for the disabled path; this matches what
        // io_enc.c does when -DTX is not passed.
        enc->st->var_SID_rate_flag = 1;
        enc->st->interval_SID      = FIXED_SID_RATE;
    }

    // ---- Apply RF options ----
    enc->st->Opt_RF_ON        = local.rf_enable ? 1 : 0;
    enc->rf_on               = local.rf_enable;
    enc->rf_offset           = local.rf_fec_offset;
    enc->rf_hi               = local.rf_fec_hi;
    // rf_fec_indicator only carries meaning when RF is on (it selects LO/HI
    // for the channel-aware mode). When RF is off, match the legacy/reference
    // default of 1 so the encoder state matches what io_ini_enc() would have
    // produced for the no-RF path.
    enc->st->rf_fec_indicator = enc->st->Opt_RF_ON ? (local.rf_fec_hi ? 1 : 0) : 1;
    if (enc->st->Opt_RF_ON) {
        // 0 in opts => fall back to the reference default (FEC_OFFSET, 3).
        enc->st->rf_fec_offset = (local.rf_fec_offset == 0)
                                 ? (short)FEC_OFFSET
                                 : (short)local.rf_fec_offset;
    } else {
        enc->st->rf_fec_offset = 0;
    }

    // Match io_ini_enc(), including last_Opt_SC_VBR. 5900 is a public
    // SC-VBR selection, never a valid constant total_brate for evs_enc().
    enc->st->Opt_SC_VBR = local.sc_vbr_enable ? 1 : 0;
    enc->st->last_Opt_SC_VBR = enc->st->Opt_SC_VBR;

    // MODE2 must be selected before init_encoder(): its state allocation
    // and first-frame signalling depend on this choice (io_enc.c).
    switch (enc->st->total_brate) {
        case 9600: case 16400: case 24400: case 48000:
        case 96000: case 128000:
            enc->st->codec_mode = MODE2;
            break;
        default:
            enc->st->codec_mode = MODE1;
            break;
    }
    if (enc->st->Opt_RF_ON) enc->st->codec_mode = MODE2;
    enc->st->last_codec_mode = enc->st->codec_mode;

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
    free(enc);
}

void evs_enc_set_rf(EVS_Encoder* enc, int rf_on, int rf_fec_offset, int rf_fec_indicator) {
    if (!enc || !enc->st) return;

    if (rf_on && (enc->bitrate_bps != ACELP_13k20 || enc->st->input_Fs < 16000 ||
                  enc->st->max_bwidth == NB)) {
        rf_on = 0;
    }
    if (rf_on && rf_fec_offset != 0 && rf_fec_offset != 2 && rf_fec_offset != 3 &&
        rf_fec_offset != 5 && rf_fec_offset != 7) return;
    enc->rf_on = rf_on != 0;
    enc->rf_offset = rf_fec_offset;
    enc->rf_hi = rf_fec_indicator != 0;

    // Mirror encoder.c's per-frame RF switching. Merely flipping Opt_RF_ON
    // leaves the old MODE1/MODE2 signalling state and stale RF indices.
    if (enc->st->Opt_RF_ON != (rf_on != 0)) reset_rf_indices(enc->st);
    if (enc->bitrate_bps == ACELP_13k20) {
        enc->st->codec_mode = rf_on ? MODE2 : MODE1;
    }

    enc->st->Opt_RF_ON = rf_on ? 1 : 0;

    if (rf_on) {
        enc->st->rf_fec_indicator = rf_fec_indicator ? 1 : 0;

        enc->st->rf_fec_offset = (short)((rf_fec_offset == 0)
                                         ? FEC_OFFSET
                                         : rf_fec_offset);
    } else {
        enc->st->rf_fec_offset   = 0;
        enc->st->rf_fec_indicator = 1;
    }
}

static int amr_wb_mode_from_rate(long bitrate) {
    static const int rates[] = {6600,8850,12650,14250,15850,18250,19850,23050,23850};
    for (int i = 0; i < 9; ++i) if (bitrate == rates[i]) return i;
    return -1;
}

int evs_enc_get_last_frame_info(const EVS_Encoder* enc, int* bits, int* update, int* mode) {
    if (!enc) return EVS_ERROR;
    if (bits) *bits = enc->last_num_bits;
    if (update) *update = enc->last_sid_update;
    if (mode) *mode = enc->last_sid_mode;
    return EVS_OK;
}

int evs_enc_reconfigure(EVS_Encoder* enc, int bitrate, EVS_Bandwidth max_bw,
                        const EVS_EncOptions* opts) {
    if (!enc || !enc->st) return EVS_ERROR;
    EVS_EncOptions options = opts ? *opts : enc->options;
    if (evs_enc_normalize_config(enc->st->input_Fs, &bitrate, &max_bw, &options) != EVS_OK)
        return EVS_ERROR;
    if (enc->bitrate_bps == bitrate && enc->st->max_bwidth == bandwidth_to_enum(max_bw) &&
        memcmp(&enc->options, &options, sizeof(options)) == 0) return EVS_OK;
    enc->options = options;
    enc->bitrate_bps = bitrate;
    enc->st->total_brate = options.sc_vbr_enable ? ACELP_7k20 : bitrate;
    enc->st->max_bwidth = (short)bandwidth_to_enum(max_bw);
    enc->st->Opt_AMR_WB = options.amr_wb_io;
    enc->st->Opt_SC_VBR = options.sc_vbr_enable;
    enc->st->Opt_DTX_ON = options.dtx_enable;
    enc->st->var_SID_rate_flag = options.dtx_sid_interval == 0;
    enc->st->interval_SID = (short)(options.dtx_sid_interval ? options.dtx_sid_interval : FIXED_SID_RATE);
    // Reference read_next_brate profile changes preserve all previous-frame
    // fields so the core performs its supported overlap/filter transitions.
    switch (enc->st->total_brate) {
        case 9600: case 16400: case 24400: case 48000: case 96000: case 128000:
            enc->st->codec_mode = MODE2; break;
        default: enc->st->codec_mode = MODE1; break;
    }
    evs_enc_set_rf(enc, options.rf_enable, options.rf_fec_offset, options.rf_fec_hi);
    return EVS_OK;
}

int evs_enc_process(EVS_Encoder* enc,
                    const short* pcm_in, int n_samples,
                    unsigned char* bitstream_out, int bitstream_max,
                    int* bitstream_used) {
    unsigned short stream[2 + MAX_BITS_PER_FRAME];
    unsigned short* pt_stream;
    short i, k, value, nb_bits;
    int mask, need;

    if (bitstream_used) *bitstream_used = 0;
    if (!enc || !enc->st || !pcm_in || !bitstream_out || !bitstream_used) return EVS_ERROR;
    if (n_samples != enc->st->input_Fs / 50) return EVS_ERROR;

    // DTX can temporarily change codec_mode and Opt_RF_ON. The reference
    // executable reapplies its requested RF settings before every frame;
    // omitting this step corrupts the MODE2 transition out of CNG.
    evs_enc_set_rf(enc, enc->rf_on, enc->rf_offset, enc->rf_hi);
    if (enc->st->Opt_AMR_WB) amr_wb_enc(enc->st, pcm_in, (short)n_samples);
    else evs_enc(enc->st, pcm_in, (short)n_samples);
    enc->last_num_bits = enc->st->nb_bits_tot;
    // Mirror indices_to_serial's explicit STI bit and current CMI selection.
    enc->last_sid_update = enc->st->Opt_AMR_WB && enc->last_num_bits == 35;
    enc->last_sid_mode = enc->st->Opt_AMR_WB ? amr_wb_mode_from_rate(enc->st->total_brate) : -1;

    // The encoder filled st->ind_list via push_indice(). Serialise it into
    // the G.192 word stream directly, replicating the G192 branch of the
    // reference write_indices() (lib_com/bitstream.c) including its
    // post-write clearing of the index list and bit counters.
    need = (2 + enc->st->nb_bits_tot) * (int)sizeof(unsigned short);
    if (bitstream_max < need) {
        // The input frame has been consumed, but no packet is returned.
        // Never leave its indices queued for the next encode call.
        reset_indices_enc(enc->st);
        return EVS_ERROR;
    }

    pt_stream = stream;
    *pt_stream++ = SYNC_GOOD_FRAME;
    *pt_stream++ = (unsigned short)enc->st->nb_bits_tot;

    for (i = 0; i < MAX_NUM_INDICES; i++) {
        value   = enc->st->ind_list[i].value;
        nb_bits = enc->st->ind_list[i].nb_bits;
        if (nb_bits > 0) {
            // mask from MSB to LSB
            mask = 1 << (nb_bits - 1);
            for (k = 0; k < nb_bits; k++) {
                *pt_stream++ = (value & mask) ? G192_BIN1 : G192_BIN0;
                mask >>= 1;
            }
        }
    }

    reset_indices_enc(enc->st);

    // memcpy instead of a direct unsigned short* store: the caller's byte
    // buffer is not guaranteed to be 2-byte aligned.
    memcpy(bitstream_out, stream, (size_t)need);
    *bitstream_used = need;

    return EVS_OK;
}

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
EVS_Decoder* evs_dec_create(int sample_rate_hz, int bitrate_bps) {
    return evs_dec_create_ex(sample_rate_hz, bitrate_bps, 0);
}
EVS_Decoder* evs_dec_create_ex(int sample_rate_hz, int bitrate_bps, int amr_wb_io) {
    if (sample_rate_to_index(sample_rate_hz) < 0) return NULL;
    if (amr_wb_io) {
        if (amr_wb_mode_from_rate(bitrate_bps) < 0) return NULL;
    } else if (bitrate_bps != 5900 &&
        (!is_valid_g192_rate(bitrate_bps) || bitrate_bps < 7200 || amr_wb_mode_from_rate(bitrate_bps) >= 0)) return NULL;

    EVS_Decoder* dec = (EVS_Decoder*)calloc(1, sizeof(EVS_Decoder));
    if (!dec) return NULL;

    dec->st = (Decoder_State*)calloc(1, sizeof(Decoder_State));
    // The reference decoder uses this buffer for its internal synthesis
    // before output-rate conversion. MODE2 NB can temporarily write 256
    // samples even when its final 8-kHz output contains only 160 samples.
    // Match decoder.c's maximum-rate workspace, not just the output length.
    dec->pcm_buf = (float*)calloc(L_FRAME48k, sizeof(float));
    if (!dec->st || !dec->pcm_buf) {
        free(dec->pcm_buf);
        free(dec->st);
        free(dec);
        return NULL;
    }

    dec->st->output_Fs       = sample_rate_hz;
    dec->st->total_brate     = bitrate_bps == 5900 ? ACELP_7k20 : bitrate_bps;
    dec->st->Opt_AMR_WB      = amr_wb_io != 0;
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
    if (dec->pcm_buf) free(dec->pcm_buf);
    free(dec);
}

int evs_dec_process(EVS_Decoder* dec,
                    const unsigned char* bitstream_in, int bitstream_len,
                    short* pcm_out, int* n_samples) {
    unsigned char au[(MAX_BITS_PER_FRAME + 7) >> 3];
    unsigned short sync_word, num_bits, w;
    const unsigned char* words;
    int i, k, N;

    if (!dec || !dec->st || !bitstream_in || !pcm_out || !n_samples) return EVS_ERROR;
    if (bitstream_len < 2 * (int)sizeof(unsigned short)) return EVS_ERROR;

    // Parse the G.192 header. The input buffer is a byte stream with no
    // alignment guarantee, so the 16-bit words are pulled out via memcpy.
    memcpy(&sync_word, bitstream_in, sizeof(unsigned short));
    memcpy(&num_bits, bitstream_in + sizeof(unsigned short), sizeof(unsigned short));

    if (sync_word != SYNC_GOOD_FRAME && sync_word != SYNC_BAD_FRAME) return EVS_ERROR;
    if (num_bits > MAX_BITS_PER_FRAME) return EVS_ERROR;
    if (bitstream_len < (2 + num_bits) * (int)sizeof(unsigned short)) return EVS_ERROR;
    if (!is_valid_g192_rate((long)num_bits * 50)) return EVS_ERROR;

    if (sync_word == SYNC_BAD_FRAME) {
        // A bad-frame marker carries no trustworthy payload; run the
        // decoder's own concealment exactly like a lost packet.
        return evs_dec_process_lost(dec, pcm_out, n_samples);
    }

    // Repack the G.192 soft bits into the compact MSB-first access unit
    // layout consumed by the reference read_indices_from_djb(), which
    // handles mode selection and the DTX (SID / NO_DATA) receive cases
    // in memory -- no FILE* required.
    memset(au, 0, sizeof(au));
    words = bitstream_in + 2 * sizeof(unsigned short);
    for (k = 0; k < (int)num_bits; k++) {
        memcpy(&w, words + (size_t)k * sizeof(unsigned short), sizeof(unsigned short));
        if (w != G192_BIN0 && w != G192_BIN1) return EVS_ERROR;
        if (w == G192_BIN1) {
            au[k >> 3] |= (unsigned char)(0x80 >> (k & 7));
        }
    }

    read_indices_from_djb(dec->st, au, num_bits, 0, 0);

    // read_indices_from_djb() flags an untransmitted DTX gap (zero-length
    // frame while not in CNG) as bfi; mirror the reference decoder main
    // loop and run concealment for it.
    if (dec->st->Opt_AMR_WB) amr_wb_dec(dec->st, dec->pcm_buf);
    else evs_dec(dec->st, dec->pcm_buf, dec->st->bfi ? FRAMEMODE_MISSING : FRAMEMODE_NORMAL);
    // decoder.c advances this outside evs_dec(). Keeping it at zero makes
    // every packet look like the first frame and breaks later mode changes.
    if (dec->st->ini_frame < MAX_FRAME_COUNTER) ++dec->st->ini_frame;

    N = dec->st->output_Fs / 50;
    for (i = 0; i < N; ++i) {
        float v = dec->pcm_buf[i];
        if (!isfinite(v)) v = 0.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (short)v;
    }
    *n_samples = N;
    return EVS_OK;
}

int evs_dec_process_lost(EVS_Decoder* dec,
                         short* pcm_out, int* n_samples) {
    unsigned char empty_au = 0;
    if (!dec || !dec->st || !pcm_out || !n_samples) return EVS_ERROR;

    int N = dec->st->output_Fs / 50;

    // A missing packet still needs the reference reader's per-frame reset
    // (bit position, BER and MDCT-switch flags). In CNG it means NO_DATA,
    // while an active decoder conceals it as a lost speech frame.
    read_indices_from_djb(dec->st, &empty_au, 0, 0, 0);
    if (dec->st->codec_mode == 0 && dec->st->last_codec_mode != 0) {
        dec->st->codec_mode = dec->st->last_codec_mode;
    }

    if (dec->st->Opt_AMR_WB) amr_wb_dec(dec->st, dec->pcm_buf);
    else evs_dec(dec->st, dec->pcm_buf, dec->st->bfi ? FRAMEMODE_MISSING : FRAMEMODE_NORMAL);
    if (dec->st->ini_frame < MAX_FRAME_COUNTER) ++dec->st->ini_frame;

    for (int i = 0; i < N; ++i) {
        float v = dec->pcm_buf[i];
        if (!isfinite(v)) v = 0.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (short)v;
    }
    *n_samples = N;
    return EVS_OK;
}
