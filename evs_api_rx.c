// 3GPP EVS Stage-1 JBM / VoIP receive adapter (experimental, float only)
//
// Wraps external/3gpp-evs/lib_dec/EvsRXlib.h behind a parent-repo C API
// that does not require the caller to know about Decoder_State, G.192, or
// JB4 internals. The header (evs_api_rx.h) is the contract; this file is
// the only implementation.
//
// Build wiring:
//   * Compiled into TelephonyDSP only when TELEPHONY_USE_EVS_JBM=ON
//     together with the float EVS variant and a non-distribution build
//     (see CMakeLists.txt).
//   * The fixed-point variant (TELEPHONY_USE_EVS_FX) is incompatible; the
//     adapter would need a parallel wrapper against stat_dec_fx /
//     Decoder_State_fx which is intentionally not in scope here.
//   * The distribution build never includes this file.

#include "evs_api_rx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 3GPP EVS reference headers (float variant). The cmake module already
// exposes lib_dec include paths to TelephonyDSP via evs-lib-dec PUBLIC
// include dirs, so these resolve at build time.
#include "options.h"      // Word16 / UWord8 / Word32 typedefs
#include "cnst.h"         // MAX_BITS_PER_FRAME, G192, MODE1
#include "prot.h"         // init_decoder / destroy_decoder
#include "stat_dec.h"     // Decoder_State
#include "EvsRXlib.h"     // EVS_RX_Open / FeedFrame / GetSamples / ...
#include "jbm_pcmdsp_apa.h"  // APA_BUF (12288)

// Internal scratch capacity for one decoded frame in Word16 samples.
// APA_BUF is the hard upper bound the EVS time-scaler may write into
// the JBM's pcmBuf per frame; allocate at least that much internally so
// the caller never has to know about it.
#define EVS_RX_JBM_APA_BUF (APA_BUF)

struct EVS_RxJbm {
    int             sample_rate_hz;
    int             bitrate_bps;

    // Embedded decoder state. EVS_RX_Open keeps a borrowed pointer and
    // EVS_RX_Close destroys it. We own the allocation.
    Decoder_State*  dec_state;

    // JBM/decoder handle. NULL after EVS_RX_Close has run, which is how
    // we avoid double-destroy.
    EVS_RX_HANDLE   h_rx;

    // Scratch buffer for the AU payload. EVS_RX_FeedFrame requires a
    // non-const unsigned char* and copies (auSize+7)/8 bytes out of it,
    // so we copy the caller's payload here to (a) tolerate a const input
    // and (b) tolerate incoming AUs that share a single buffer across
    // multiple calls. Sized for the worst-case 128 kbps / 20 ms AU.
    unsigned char*  au_scratch;
    int             au_scratch_bytes;

    // Internal PCM buffer for EVS_RX_GetSamples. APA_BUF Word16 samples
    // is the hard upper bound the JBM/time-scaler may write.
    short*          pcm_buf;
};

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

static int sample_rate_supported(int sr_hz) {
    switch (sr_hz) {
        case  8000:
        case 16000:
        case 32000:
        case 48000:
            return 1;
        default:
            return 0;
    }
}

// Maximum payload size in BITS for one 20 ms EVS frame. Matches the
// reference decoder's hard cap (see MAX_AU_SIZE in cnst.h, derived from
// 128000 bps / 50 fps * 8 bits-per-byte). Use this to validate the
// incoming AU bit count rather than recomputing it locally.
#define EVS_RX_JBM_MAX_AU_BITS MAX_BITS_PER_FRAME

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
EVS_RxJbm* evs_rx_jbm_create(int sample_rate_hz,
                             int bitrate_bps,
                             int jbm_safety_margin_ms) {
    if (!sample_rate_supported(sample_rate_hz)) return NULL;
    if (bitrate_bps < 5900 || bitrate_bps > 128000) return NULL;
    if (jbm_safety_margin_ms <= 0) jbm_safety_margin_ms = 60;

    EVS_RxJbm* rx = (EVS_RxJbm*)calloc(1, sizeof(EVS_RxJbm));
    if (!rx) return NULL;

    rx->sample_rate_hz = sample_rate_hz;
    rx->bitrate_bps    = bitrate_bps;

    rx->dec_state = (Decoder_State*)calloc(1, sizeof(Decoder_State));
    if (!rx->dec_state) {
        evs_rx_jbm_destroy(rx);
        return NULL;
    }

    // Mirror the minimum set of fields that evs_dec_create() (in evs_api.c)
    // sets before init_decoder(). The JBM path calls init_decoder() lazily
    // on the first real frame inside EVS_RX_GetSamples, but it still reads
    // output_Fs / total_brate / Opt_AMR_WB / bitstreamformat / bfi /
    // prev_bfi / codec_mode while it owns the Decoder_State. Match the
    // non-JBM wrapper's defaults so the state stays consistent regardless
    // of which entry point the caller used elsewhere.
    rx->dec_state->output_Fs       = sample_rate_hz;
    rx->dec_state->total_brate     = bitrate_bps;
    rx->dec_state->Opt_AMR_WB      = 0;          // native EVS, not AMR-WB IO
    rx->dec_state->bitstreamformat = G192;
    rx->dec_state->bfi             = 0;
    rx->dec_state->prev_bfi        = 0;
    rx->dec_state->codec_mode      = 0;          // unknown before first frame

    // Allocate scratch buffers.
    rx->au_scratch_bytes = (EVS_RX_JBM_MAX_AU_BITS + 7) / 8;
    rx->au_scratch = (unsigned char*)calloc(1, rx->au_scratch_bytes);
    if (!rx->au_scratch) {
        evs_rx_jbm_destroy(rx);
        return NULL;
    }

    rx->pcm_buf = (short*)calloc(EVS_RX_JBM_APA_BUF, sizeof(short));
    if (!rx->pcm_buf) {
        evs_rx_jbm_destroy(rx);
        return NULL;
    }

    // Open the JBM/decoder. EVS_RX_Open internally:
    //   * keeps our Decoder_State pointer
    //   * resets codec_mode = 0
    //   * creates + inits the JB4 jitter buffer with the safety margin
    //   * creates the APA time-scaler and the FIFO-after-time-scaler
    // On failure it cleans up its own allocations and resets *phRX to
    // NULL; we still need to free our scratch + dec_state below.
    if (EVS_RX_Open(&rx->h_rx, rx->dec_state, (Word16)jbm_safety_margin_ms)
            != EVS_RX_NO_ERROR) {
        rx->h_rx = NULL;
        evs_rx_jbm_destroy(rx);
        return NULL;
    }

    return rx;
}

void evs_rx_jbm_destroy(EVS_RxJbm* rx) {
    if (!rx) return;

    // EVS_RX_Close internally calls destroy_decoder(rx->dec_state) on the
    // embedded Decoder_State and frees the EVS_RX handle (JBM, time-scaler
    // and FIFO). It also nulls *phRX, so a second call is a safe no-op.
    //
    // IMPORTANT: EVS_RX_Close destroys the Decoder_State contents (the
    // EVS internal buffers via destroy_decoder), but it does NOT free
    // the Decoder_State struct itself -- that allocation is owned by this
    // wrapper and must be released here. Do NOT call destroy_decoder
    // directly from this adapter: EVS_RX_Close already does it, and a
    // manual call would be a double-free.
    if (rx->h_rx) {
        EVS_RX_Close(&rx->h_rx);
    }

    // Free the Decoder_State struct we allocated in evs_rx_jbm_create.
    // This must run whether or not EVS_RX_Open succeeded: if Open failed
    // we still own the calloc'd Decoder_State and need to release it.
    if (rx->dec_state) {
        free(rx->dec_state);
        rx->dec_state = NULL;
    }

    if (rx->au_scratch) {
        free(rx->au_scratch);
        rx->au_scratch = NULL;
    }
    if (rx->pcm_buf) {
        free(rx->pcm_buf);
        rx->pcm_buf = NULL;
    }

    free(rx);
}

// ---------------------------------------------------------------------------
// Feed / pull
// ---------------------------------------------------------------------------
int evs_rx_jbm_feed_frame(EVS_RxJbm* rx,
                          const unsigned char* au_bits,
                          int au_bits_count,
                          unsigned short rtp_sequence_number,
                          unsigned long  rtp_timestamp_ms,
                          unsigned int  receive_time_ms) {
    if (!rx || !rx->h_rx || !au_bits) return EVS_ERROR;
    if (au_bits_count <= 0 || au_bits_count > EVS_RX_JBM_MAX_AU_BITS) {
        return EVS_ERROR;
    }

    // Copy the incoming AU into our own scratch so EVS_RX_FeedFrame can
    // hold the pointer for the lifetime of the JBM slot without aliasing
    // the caller's buffer across calls.
    int byte_count = (au_bits_count + 7) / 8;
    if (byte_count > rx->au_scratch_bytes) {
        // Defensive; should not happen because of the bit-count check
        // above (EVS_RX_JBM_MAX_AU_BITS is the same width).
        return EVS_ERROR;
    }
    memcpy(rx->au_scratch, au_bits, byte_count);

    EVS_RX_ERROR err = EVS_RX_FeedFrame(
        rx->h_rx,
        rx->au_scratch,
        (unsigned int)au_bits_count,
        rtp_sequence_number,
        (unsigned long)rtp_timestamp_ms,
        receive_time_ms);

    return (err == EVS_RX_NO_ERROR) ? EVS_OK : EVS_ERROR;
}

int evs_rx_jbm_get_samples(EVS_RxJbm* rx,
                           short* pcm_out,
                           int pcm_capacity_samples,
                           unsigned int system_time_ms,
                           int* n_samples) {
    if (!rx || !rx->h_rx || !pcm_out || !n_samples) return EVS_ERROR;

    int expected = rx->sample_rate_hz / 50;   // 20 ms frame
    if (pcm_capacity_samples < expected) return EVS_ERROR;

    // EVS_RX_GetSamples writes sample_rate_hz/50 samples per call into
    // pcmBuf (the same nSamplesFrame it asserts on). The internal
    // time-scaler can run in-place on the same buffer; the FIFO stage is
    // hidden. We use rx->pcm_buf (APA_BUF samples) so the assertion
    // hEvsRX->nSamplesFrame <= APA_BUF inside the reference code holds
    // for every supported sample rate.
    unsigned int written = 0;
    EVS_RX_ERROR err = EVS_RX_GetSamples(
        rx->h_rx,
        &written,
        rx->pcm_buf,
        (unsigned int)EVS_RX_JBM_APA_BUF,
        system_time_ms);

    if (err != EVS_RX_NO_ERROR) return EVS_ERROR;

    // The contract: on success the JBM returns exactly sample_rate_hz/50
    // samples (one frame). If a future EVS_RX revision ever returned more,
    // truncate instead of overflowing the caller's buffer.
    if ((int)written != expected) return EVS_ERROR;

    memcpy(pcm_out, rx->pcm_buf, expected * sizeof(short));
    *n_samples = expected;
    return EVS_OK;
}

int evs_rx_jbm_get_fec_offset(EVS_RxJbm* rx, int* offset, int* fec_hi) {
    if (!rx || !rx->h_rx || !offset || !fec_hi) return EVS_ERROR;

    short opt_offset = 0;
    short fec_hi_raw = 0;
    EVS_RX_ERROR err = EVS_RX_Get_FEC_offset(rx->h_rx, &opt_offset, &fec_hi_raw);
    if (err != EVS_RX_NO_ERROR) return EVS_ERROR;

    *offset = (int)opt_offset;
    *fec_hi = (int)fec_hi_raw;
    return EVS_OK;
}

int evs_rx_jbm_is_empty(EVS_RxJbm* rx) {
    if (!rx || !rx->h_rx) return EVS_ERROR;
    return EVS_RX_IsEmpty(rx->h_rx) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// G.192 short-stream -> compact MSB-first EVS AU helper.
//
// Local constants; kept private to this translation unit so the parent
// smoke executable and any future caller can rely on the same numbers
// without re-declaring them.
// ---------------------------------------------------------------------------

// Good-frame sync word used by the 3GPP EVS reference's G.192
// short-stream output. The helper is intentionally for encoder-produced
// good frames, not arbitrary G.192/bad-frame streams.
#define EVS_RX_G192_SYNC_GOOD_FRAME 0x6B21u

// Payload-word encoding used by the 3GPP EVS reference's G.192
// short-stream: 0x0081 means "bit is 1", 0x007F means "bit is 0".
#define EVS_RX_G192_BIT1 0x0081u

// Maximum payload size we accept on the convert path, in bits. Matches
// the reference decoder's hard cap (MAX_BITS_PER_FRAME = 2560 in cnst.h)
// and is the same number MAX_BITS_PER_FRAME is derived from in the EVS
// reference. The compact buffer must accommodate (2560 + 7) / 8 = 320
// bytes; callers should size accordingly.
#define EVS_RX_G192_MAX_AU_BITS  2560
#define EVS_RX_G192_MAX_AU_BYTES 320

int evs_rx_jbm_g192_to_compact_au(const unsigned char* bitstream,
                                  int bitstream_used,
                                  unsigned char* compact,
                                  int compact_capacity) {
    if (!bitstream || !compact) return EVS_ERROR;
    if (bitstream_used < (int)(2 * sizeof(unsigned short))) return EVS_ERROR;

    // The G.192 short-stream is laid out as a native-endian array of
    // uint16 words: word[0] = SYNC_WORD, word[1] = nb_bits, then the
    // payload bit words. The EVS reference writes the bitstream as
    // sizeof(unsigned short) per element, so the bit-count math is in
    // multiples of that.
    const unsigned short* words = (const unsigned short*)bitstream;
    int nwords = bitstream_used / (int)sizeof(unsigned short);

    if (words[0] != EVS_RX_G192_SYNC_GOOD_FRAME) return EVS_ERROR;

    unsigned int nb_bits = (unsigned int)words[1];
    if (nb_bits < 1u || nb_bits > EVS_RX_G192_MAX_AU_BITS) return EVS_ERROR;
    if (nwords < 2 + (int)nb_bits) return EVS_ERROR;

    int compact_bytes = (int)((nb_bits + 7u) / 8u);
    if (compact_bytes > EVS_RX_G192_MAX_AU_BYTES) return EVS_ERROR;
    if (compact_capacity < compact_bytes) return EVS_ERROR;

    // Walk the payload words, set bit i in the compact output MSB-first
    // whenever word[2+i] == 0x0081. Zero-initialise first so a sparse
    // payload (lots of 0x007F words) does not leave stale bits behind.
    memset(compact, 0, (size_t)compact_bytes);
    for (unsigned int i = 0; i < nb_bits; ++i) {
        if (words[2u + i] != EVS_RX_G192_BIT1) continue;
        int byte_index = (int)(i >> 3);
        int bit_index  = 7 - (int)(i & 0x7u);
        compact[byte_index] |= (unsigned char)(1u << bit_index);
    }

    return (int)nb_bits;
}
