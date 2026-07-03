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

// 3GPP EVS reference headers (float variant)
#include "options.h"
#include "cnst.h"
#include "prot.h"
#include "stat_enc.h"
#include "stat_dec.h"

struct EVS_Encoder {
    Encoder_State* st;
    Indice*        ind_buf;     // MAX_NUM_INDICES entries
};

struct EVS_Decoder {
    Decoder_State* st;
    float*         pcm_buf;     // output_Fs / 50 samples, reused every frame
};

// MAX_BITS_PER_FRAME is defined in cnst.h (2560).
// MAX_NUM_INDICES = IND_UNUSED + 127 (also from cnst.h).

// Frame lengths that map to a defined EVS primary or AMR-WB IO mode.
// Mirrors the switch in the reference rate2EVSmode() (lib_com/bitstream.c),
// which is static there, so the accepted set is restated here. Used to
// reject corrupt G.192 headers before they reach decoder_selectCodec().
static int is_valid_g192_rate(long rate) {
    switch (rate) {
        // EVS primary modes
        case 0:      case 2400:  case 2800:  case 7200:  case 8000:
        case 9600:   case 13200: case 16400: case 24400: case 32000:
        case 48000:  case 64000: case 96000: case 128000:
        // AMR-WB IO modes
        case 1750:   case 6600:  case 8850:  case 12650: case 14250:
        case 15850:  case 18250: case 19850: case 23050: case 23850:
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
            // Out-of-range: fail cleanly (mirrors io_enc.c usage_enc(), but
            // without exiting the caller).
            return NULL;
        }
    }

    // ---- RF validation: only 13.2 kbps and >= 16 kHz input are accepted ----
    if (local.rf_enable && (bitrate_bps != ACELP_13k20 || sample_rate_hz < 16000)) {
        // Disabling safely matches io_enc.c's "Reset RF parameters if NB
        // input_Fs" / "channel-aware mode is supported only at 13.20" paths.
        // Emit the reference CLI's diagnostic so callers notice the silent
        // downgrade of their request.
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

    enc->st = (Encoder_State*)calloc(1, sizeof(Encoder_State));
    enc->ind_buf = (Indice*)calloc(MAX_NUM_INDICES, sizeof(Indice));
    if (!enc->st || !enc->ind_buf) {
        evs_enc_destroy(enc);
        return NULL;
    }

    // Mirror what io_ini_enc() would have done for the CLI, but only the
    // fields actually consulted by init_encoder() and evs_enc().
    enc->st->input_Fs        = sample_rate_hz;
    enc->st->total_brate     = bitrate_bps;
    enc->st->max_bwidth      = (short)bandwidth_to_enum(max_bw);
    enc->st->Opt_AMR_WB      = 0;          // 0 = native EVS (not AMR-WB IO)
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

    // ---- Apply SC-VBR option ----
    enc->st->Opt_SC_VBR = local.sc_vbr_enable ? 1 : 0;

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
    free(enc);
}

void evs_enc_set_rf(EVS_Encoder* enc, int rf_on, int rf_fec_offset, int rf_fec_indicator) {
    if (!enc || !enc->st) return;

    if (rf_on && (enc->st->total_brate != ACELP_13k20 || enc->st->input_Fs < 16000)) {
        rf_on = 0;
    }

    enc->st->Opt_RF_ON = rf_on ? 1 : 0;

    if (rf_on) {
        enc->st->rf_fec_indicator = rf_fec_indicator ? 1 : 0;

        if (rf_fec_offset == 0 || rf_fec_offset == 2 || rf_fec_offset == 3 ||
            rf_fec_offset == 5 || rf_fec_offset == 7) {
            enc->st->rf_fec_offset = (short)((rf_fec_offset == 0)
                                             ? FEC_OFFSET
                                             : rf_fec_offset);
        }
    } else {
        enc->st->rf_fec_offset   = 0;
        enc->st->rf_fec_indicator = 1;
    }
}

int evs_enc_process(EVS_Encoder* enc,
                    const short* pcm_in, int n_samples,
                    unsigned char* bitstream_out, int bitstream_max,
                    int* bitstream_used) {
    unsigned short stream[2 + MAX_BITS_PER_FRAME];
    unsigned short* pt_stream;
    short i, k, value, nb_bits;
    int mask, need;

    if (!enc || !enc->st || !pcm_in || !bitstream_out || !bitstream_used) return EVS_ERROR;
    if (n_samples != enc->st->input_Fs / 50) return EVS_ERROR;

    evs_enc(enc->st, pcm_in, (short)n_samples);

    // The encoder filled st->ind_list via push_indice(). Serialise it into
    // the G.192 word stream directly, replicating the G192 branch of the
    // reference write_indices() (lib_com/bitstream.c) including its
    // post-write clearing of the index list and bit counters.
    need = (2 + enc->st->nb_bits_tot) * (int)sizeof(unsigned short);
    if (bitstream_max < need) return EVS_ERROR;

    pt_stream = stream;
    *pt_stream++ = SYNC_GOOD_FRAME;
    *pt_stream++ = (unsigned short)enc->st->nb_bits_tot;

    for (i = 0; i < MAX_NUM_INDICES; i++) {
        value   = enc->st->ind_list[i].value;
        nb_bits = enc->st->ind_list[i].nb_bits;
        if (nb_bits != -1) {
            // mask from MSB to LSB
            mask = 1 << (nb_bits - 1);
            for (k = 0; k < nb_bits; k++) {
                *pt_stream++ = (value & mask) ? G192_BIN1 : G192_BIN0;
                mask >>= 1;
            }
        }
    }

    for (i = 0; i < MAX_NUM_INDICES; i++) {
        enc->st->ind_list[i].nb_bits = -1;
    }
    enc->st->nb_bits_tot = 0;
    enc->st->next_ind = 0;
    enc->st->last_ind = -1;

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
    if (sample_rate_to_index(sample_rate_hz) < 0) return NULL;
    if (bitrate_bps < 5900 || bitrate_bps > 128000) return NULL;

    EVS_Decoder* dec = (EVS_Decoder*)calloc(1, sizeof(EVS_Decoder));
    if (!dec) return NULL;

    dec->st = (Decoder_State*)calloc(1, sizeof(Decoder_State));
    dec->pcm_buf = (float*)calloc(sample_rate_hz / 50, sizeof(float));
    if (!dec->st || !dec->pcm_buf) {
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
        if (w == G192_BIN1) {
            au[k >> 3] |= (unsigned char)(0x80 >> (k & 7));
        }
    }

    read_indices_from_djb(dec->st, au, num_bits, 0, 0);

    // read_indices_from_djb() flags an untransmitted DTX gap (zero-length
    // frame while not in CNG) as bfi; mirror the reference decoder main
    // loop and run concealment for it.
    evs_dec(dec->st, dec->pcm_buf, dec->st->bfi ? FRAMEMODE_MISSING : FRAMEMODE_NORMAL);

    N = dec->st->output_Fs / 50;
    for (i = 0; i < N; ++i) {
        float v = dec->pcm_buf[i];
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (short)v;
    }
    *n_samples = N;
    return EVS_OK;
}

int evs_dec_process_lost(EVS_Decoder* dec,
                         short* pcm_out, int* n_samples) {
    if (!dec || !dec->st || !pcm_out || !n_samples) return EVS_ERROR;

    int N = dec->st->output_Fs / 50;

    dec->st->bfi = 1;
    if (dec->st->codec_mode == 0 && dec->st->last_codec_mode != 0) {
        dec->st->codec_mode = dec->st->last_codec_mode;
    }

    evs_dec(dec->st, dec->pcm_buf, FRAMEMODE_MISSING);

    for (int i = 0; i < N; ++i) {
        float v = dec->pcm_buf[i];
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (short)v;
    }
    *n_samples = N;
    return EVS_OK;
}
