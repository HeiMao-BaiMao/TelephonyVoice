// Fixed-point EVS wrapper (TS 26.442 v12.15.0 / v13.10.0 / v14.6.0 / v15.4.0 / v16.4.0)
//
// This file is only compiled when TELEPHONY_USE_EVS_FX is enabled.
// Mirrors evs_api.c (float variant) but uses the Word16 / _fx-flavoured state
// structures and the fixed-point reference functions (init_encoder_fx,
// evs_enc_fx, init_decoder_fx, evs_dec_fx).
//
// The fixed-point reference shares the same G.192 bitstream format with the
// floating-point reference.
//
// Encoding is fully in-memory, like the float wrapper: the encoder replicates
// the G192 branch of write_indices_fx() (lib_com/bitstream_fx.c) straight into
// the caller's buffer, so no temporary file is touched and concurrent plugin
// instances cannot share one.
//
// Decoding still goes through the reference's file-based G.192 reader
// read_indices_fx() with a private tmpfile() per decoder instance.  The
// alternative in-memory entry point, read_indices_from_djb_fx(), implements the
// *RTP payload* transport instead: it skips the G.192 SID/CRC/BER bookkeeping
// and drives a static helper that is not exported from the submodule, so
// swapping it in changes DTX behaviour (measured: it crashed on the first
// frame).  Porting the G.192 semantics in memory therefore requires either a
// submodule change or a full re-implementation, and is deferred until the
// fixed-point core itself is ported.

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
};

struct EVS_Decoder {
    Decoder_State_fx* st;
    // Private scratch file for the per-frame G.192 round-trip required by
    // read_indices_fx() (see evs_dec_process()).  tmpfile() creates a unique,
    // anonymous stream per decoder instance; the previous implementation also
    // had a fallback that opened the *fixed* name "evs_tandem.192" relative to
    // the current directory, which two plugin instances (or two processes
    // sharing a directory) would have shared and corrupted.  If tmpfile()
    // fails we now fail the codec creation instead of corrupting silently -
    // EVSCodec then passes audio through untouched.
    FILE*             bitfile;
    // Backing store for st->bit_stream_fx.
    //
    // The fixed-point reference declares this field as a *pointer*
    // (lib_dec/stat_dec_fx.h: "UWord16 *bit_stream_fx;") whereas the float
    // variant embeds a fixed array ("unsigned short bit_stream[MAX_BITS_PER_FRAME+16]"
    // in lib_dec/stat_dec.h).  Nothing in external/3gpp-evs allocates the FX
    // pointer - the unpacking code only fills it in:
    //
    //     bit_stream_ptr = st->bit_stream_fx;
    //     for (k = 0; k < num_bits; ++k) *bit_stream_ptr++ = (*pt_stream++ == G192_BIN1);
    //     (lib_com/bitstream_fx.c, read_indices_fx G.192 "GOOD frame" branch)
    //
    // so a calloc'd Decoder_State_fx leaves it NULL and the first decoded
    // frame faults with an access violation (0xC0000005).  Allocate
    // MAX_BITS_PER_FRAME+16 entries, mirroring the float array size (the
    // compact-AU reader also appends 16 zero bits for the arithmetic coder),
    // and hand the pointer to the decoder state.  Ownership stays here so
    // evs_dec_destroy() frees exactly what was allocated.
    UWord16*          bitStreamBuf;
};

// MAX_BITS_PER_FRAME is defined in cnst_fx.h (2560).
// MAX_NUM_INDICES = IND_UNUSED + 127 (also from cnst_fx.h).
// G192 / MIME / ACELP_13k20 / FEC_OFFSET / FIXED_SID_RATE / MODE1 / MODE2
// are all exposed by cnst_fx.h.

// Scratch stream for the file-based G.192 reader.  tmpfile() only: see the
// EVS_Decoder::bitfile comment for why the old fixed-name fallback is gone.
// (MSVC's CRT deprecation warning for tmpfile() is suppressed locally; the
// function is plain C89 and remains the portable choice here.)
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
static FILE* open_temp_bitstream(void) {
    return tmpfile();
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

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

    // The fixed-point FD-CNG family is still a set of link-unblock stubs
    // (lib_enc/fdcng_enc_fx.c and its decoder counterpart): noise
    // estimation, SID encoding and comfort-noise generation do not
    // implement the reference behaviour, and letting the DTX/SID path run
    // through them corrupts memory - with DTX enabled a mono render died
    // with 0xC0000409 inside evs_enc_fx after ~30 frames of real speech.
    //
    // DTX is therefore forced off here, which keeps the encoder on the
    // speech-only path that the ported core handles correctly: the same
    // render then completes and its output correlates 0.994 with the
    // float EVS reference.  Remove this override once FD-CNG is ported.
    local.dtx_enable = 0;

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
    if (!enc->st || !enc->ind_buf) {
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

    // Digital silence must not reach the fixed-point core: the basic-op
    // div_s() implementation in basic_op/basop32.c calls abort() when its
    // denominator is zero ("Division by zero, Fatal error"), and an
    // all-zero frame is exactly the input that makes the Levinson-Durbin /
    // gain recursion divide by zero.  The reference CLI never sees such a
    // frame because its input files always carry a noise floor, but the
    // plugin does: TelephonyRunner feeds zeroed blocks while flushing and
    // real speech has silent passages - either one killed the process.
    //
    // Skipping evs_enc_fx() leaves the index list empty (the previous
    // write_indices_fx() run already cleared it, and nb_bits_tot_fx is 0
    // after init_encoder_fx()), so the serialisation below emits a valid
    // zero-length G.192 frame.  That is the standard "no data" marker and
    // the decoder maps it onto its DTX / SP_LOST path.
    {
        int all_zero = 1;
        for (int i = 0; i < n_samples; ++i) {
            if (pcm_in[i] != 0) { all_zero = 0; break; }
        }
        if (!all_zero) {
            evs_enc_fx(enc->st, (const Word16*)pcm_in, (Word16)n_samples);
        }
    }

    // The encoder filled st->ind_list_fx via push_indice_fx() /
    // push_next_indice_fx().  Serialise that into the caller's buffer as a
    // G.192 word stream, replicating the G192 branch of the reference
    // write_indices_fx() (lib_com/bitstream_fx.c) including its post-write
    // index-list reset, so no temporary file is needed.
    {
        const Word32 numBits = (Word32)enc->st->nb_bits_tot_fx;
        const int need = (int)(2u + (unsigned)numBits) * (int)sizeof(unsigned short);
        if (bitstream_max < need) return EVS_ERROR;

        unsigned short stream[2 + MAX_BITS_PER_FRAME];
        unsigned short* pt_stream = stream;
        int i, k;

        for (i = 0; i < 2 + MAX_BITS_PER_FRAME; ++i) {
            stream[i] = 0;
        }
        *pt_stream++ = (unsigned short)SYNC_GOOD_FRAME;
        *pt_stream++ = (unsigned short)enc->st->nb_bits_tot_fx;

        for (i = 0; i < MAX_NUM_INDICES; ++i) {
            const Word16 nb_bits = enc->st->ind_list_fx[i].nb_bits;
            if (nb_bits != -1) {
                // mask from MSB to LSB
                Word32 mask = 1 << (nb_bits - 1);
                for (k = 0; k < nb_bits; ++k) {
                    *pt_stream++ = (enc->st->ind_list_fx[i].value & mask)
                                       ? (unsigned short)G192_BIN1
                                       : (unsigned short)G192_BIN0;
                    mask >>= 1;
                }
            }
        }

        // Clearing of indices (mirrors the reference writer).
        for (i = 0; i < MAX_NUM_INDICES; ++i) {
            enc->st->ind_list_fx[i].nb_bits = -1;
        }
        enc->st->nb_bits_tot_fx = 0;
        enc->st->next_ind_fx    = 0;
        enc->st->last_ind_fx    = -1;

        // memcpy instead of a direct unsigned short* store: the caller's byte
        // buffer has no alignment guarantee.
        memcpy(bitstream_out, stream, (size_t)need);
        *bitstream_used = need;
    }

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
    // See the EVS_Decoder::bitStreamBuf comment: the FX reference reads/writes
    // the unpacked bit array through this pointer and never allocates it.
    dec->bitStreamBuf = (UWord16*)calloc(MAX_BITS_PER_FRAME + 16, sizeof(UWord16));
    if (!dec->st || !dec->bitfile || !dec->bitStreamBuf) {
        evs_dec_destroy(dec);
        return NULL;
    }
    dec->st->bit_stream_fx = dec->bitStreamBuf;

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
    if (dec->bitStreamBuf) free(dec->bitStreamBuf);
    if (dec->bitfile) fclose(dec->bitfile);
    free(dec);
}

int evs_dec_process(EVS_Decoder* dec,
                    const unsigned char* bitstream_in, int bitstream_len,
                    short* pcm_out, int* n_samples) {
    if (!dec || !dec->st || !bitstream_in || !pcm_out || !n_samples) return EVS_ERROR;
    if (bitstream_len <= 0) return EVS_ERROR;

    // The G.192 reader stays file-backed on purpose.  The reference exposes
    // two different decoders for the two transports:
    //   * read_indices_fx()          - G.192 word stream (what we produce)
    //   * read_indices_from_djb_fx() - compact RTP access unit
    // The G.192 path additionally runs SID/CRC/BER bookkeeping and drives the
    // static read_indices_mime_handle_dtx() helper, which is not exported, so
    // an in-memory reimplementation would have to clone that logic.  Rather
    // than diverge from the reference semantics we keep read_indices_fx() and
    // only make sure its temporary file is private to this instance (see
    // evs_dec_create()).
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

    evs_dec_fx(dec->st, out,
               dec->st->bfi_fx ? FRAMEMODE_MISSING : FRAMEMODE_NORMAL);

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
