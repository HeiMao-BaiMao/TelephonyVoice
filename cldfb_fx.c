/* ============================================================================
 *  Parent-repo FX helper for the 3GPP EVS reference.
 *
 *  Provides the CLDFB accessor family declared in
 *      external/3gpp-evs/lib_com/prot_fx.h (lines ~640-642, ~9914-9995)
 *  and implemented for the float reference in
 *      external/3gpp-evs/lib_com/cldfb.c.
 *
 *  Why this parent-side helper exists
 *  ----------------------------------
 *  The vendored EVS fixed-point snapshot contains the FX ROM tables
 *  (CLDFB80_*, rRotVectr_*, iRotVectr_*, cldfb_anaScale, ...), the
 *  FX struct layout and the function prototypes, but it does not ship
 *  a fixed-point implementation of the CLDFB analysis/synthesis bank.
 *  Because external/3gpp-evs/ is read-only, the port lives here and is
 *  attached to evs-lib-com-fx via EVS_FX_EXTRAS_LIB_COM.
 *
 *  Porting strategy
 *  ----------------
 *  A bit-exact fixed-point CLDFB is a large subsystem (prototype-filter
 *  polyphase folding, DST/DCT modulation and small FFTs).  For this
 *  batch the focus is on resolving the nine missing external symbols,
 *  keeping the encoder/decoder paths from crashing when CLDFB is
 *  exercised, and using the existing FX prototype-filter ROM tables.
 *
 *  The implementation below is therefore a deliberately conservative,
 *  self-contained DFT-modulated WOLA filterbank:
 *    - analysis uses the CLDFB80_* prototype filters and explicit
 *      complex modulation;
 *    - synthesis uses the inverse DFT of the subband slots (a simplified
 *      inverse that keeps output bounded and avoids pulling in missing
 *      FFT helpers);
 *    - all filter states live in a small static instance pool so no
 *      dynamic allocation is required.
 *
 *  Q-format assumptions
 *  --------------------
 *    - time input is treated as Q(timeIn_e); the analysis stores raw
 *      integer products and reports lb_scale == cldfb_anaScale so that
 *      downstream callers can interpret the Word32 subband data with
 *      the usual negative-exponent convention.
 *    - synthesis takes real/imag in Q(scaleFactor->lb_scale) and
 *      produces timeOut in Q(timeOut_e).
 *    - All critical accumulators use 64-bit signed arithmetic with
 *      explicit saturation.
 *
 *  Known limitations
 *  -----------------
 *    - Perfect reconstruction is not guaranteed; the synthesis inverse
 *      omits the prototype-filter overlap-add tail.  Audio quality is
 *      expected to be degraded compared with a full EVS CLDFB port.
 *    - resampleCldfb resets the filter state instead of warping it.
 *    - Only the six supported CLDFB channel counts (10/16/20/32/40/60)
 *      are handled.
 * ============================================================================ */

#include "options.h"
#include "cnst_fx.h"
#include "prot_fx.h"
#include "rom_com_fx.h"
#include "stl.h"
#include <math.h>
#include <stdlib.h>   /* calloc / free for the per-handle CLDFB instances */

/* -------------------------------------------------------------------------- */
/* Static resource limits                                                     */
/* -------------------------------------------------------------------------- */

#define EVS_FX_CLDFB_MAX_CHANNELS     CLDFB_NO_CHANNELS_MAX   /* 60 */
#define EVS_FX_CLDFB_MAX_COLS         CLDFB_NO_COL_MAX        /* 16 */
#define EVS_FX_CLDFB_MAX_FILTER_LEN   (10 * EVS_FX_CLDFB_MAX_CHANNELS)
#define EVS_FX_CLDFB_MAX_STATE_WORDS  (EVS_FX_CLDFB_MAX_FILTER_LEN + \
                                       EVS_FX_CLDFB_MAX_CHANNELS * EVS_FX_CLDFB_MAX_COLS)
#define EVS_FX_CLDFB_MAX_MOD_SIZE     (EVS_FX_CLDFB_MAX_CHANNELS * EVS_FX_CLDFB_MAX_FILTER_LEN)

#define EVS_FX_CLDFB_PI 3.14159265358979323846

/* One CLDFB instance.  CLDFB_FILTER_BANK is the first member so that a
 * HANDLE_CLDFB_FILTER_BANK pointer can be cast back to this struct.
 *
 * Instances are heap-allocated one per openCldfb() call and released by
 * deleteCldfb().  They used to come from a fixed pool of 8 static slots, which
 * the decoder silently exhausted as soon as more than one EVS codec instance
 * existed: a stereo render opens ten CLDFB banks (two encoders + two decoders,
 * three banks each in the decoder), openCldfb() then returned NULL, and the
 * reference dereferences the handle unconditionally
 * (init_dec_fx.c: "st_fx->cldfbSyn_fx->scale"), which killed the process with
 * an access violation in the second instance's init_decoder_fx(). */
typedef struct
{
    CLDFB_FILTER_BANK bank;
    Word16            state[EVS_FX_CLDFB_MAX_STATE_WORDS];
    Word16            memory[EVS_FX_CLDFB_MAX_STATE_WORDS];
} evs_fx_cldfb_inst_t;

/* Shared modulation tables, one entry per supported channel count. */
static Word16 s_cos_tbl[6][EVS_FX_CLDFB_MAX_MOD_SIZE];
static Word16 s_sin_tbl[6][EVS_FX_CLDFB_MAX_MOD_SIZE];
static Word16 s_mod_tables_ready[6] = {0, 0, 0, 0, 0, 0};

/* -------------------------------------------------------------------------- */
/* Local helpers                                                              */
/* -------------------------------------------------------------------------- */

static Word16 cldfb_supported_channels(Word16 m)
{
    return (Word16)((m == 10) || (m == 16) || (m == 20) || (m == 32) || (m == 40) || (m == 60));
}

static Word16 cldfb_channel_index(Word16 m)
{
    switch (m)
    {
    case 10: return 0;
    case 16: return 1;
    case 20: return 2;
    case 32: return 3;
    case 40: return 4;
    case 60: return 5;
    default: return 1;   /* 16 is a safe default */
    }
}

static const Word16 *cldfb_proto_filter(Word16 m)
{
    switch (m)
    {
    case 10: return CLDFB80_10;
    case 16: return CLDFB80_16;
    case 20: return CLDFB80_20;
    case 32: return CLDFB80_32;
    case 40: return CLDFB80_40;
    case 60: return CLDFB80_60;
    default: return CLDFB80_16;
    }
}

static Word16 cldfb_ana_scale_for(Word16 m)
{
    Word16 idx = cldfb_channel_index(m);
    return cldfb_anaScale[idx];
}

static evs_fx_cldfb_inst_t *cldfb_inst_from_handle(HANDLE_CLDFB_FILTER_BANK hs)
{
    /* CLDFB_FILTER_BANK is the first member of evs_fx_cldfb_inst_t. */
    return (evs_fx_cldfb_inst_t *)hs;
}

static void evs_fx_set16(Word16 *dst, Word16 val, Word32 n)
{
    Word32 i;
    for (i = 0; i < n; i++)
    {
        dst[i] = val;
    }
}

static void evs_fx_copy16(const Word16 *src, Word16 *dst, Word32 n)
{
    Word32 i;
    for (i = 0; i < n; i++)
    {
        dst[i] = src[i];
    }
}

static Word32 evs_fx_saturate32(long long v)
{
    if (v > 2147483647LL)
    {
        return 2147483647L;
    }
    if (v < -2147483648LL)
    {
        return -2147483648L;
    }
    return (Word32)v;
}

static Word16 evs_fx_saturate16(long long v)
{
    if (v > 32767LL)
    {
        return 32767;
    }
    if (v < -32768LL)
    {
        return -32768;
    }
    return (Word16)v;
}

static long long evs_fx_shift64(long long v, Word16 s)
{
    if (s >= 0)
    {
        return v >> s;
    }
    return v << (-s);
}

static void cldfb_init_mod_tables(Word16 m)
{
    Word16 idx = cldfb_channel_index(m);
    Word16 l, k;
    Word32 tbl_idx;
    double phase;

    if (s_mod_tables_ready[idx])
    {
        return;
    }

    l = (Word16)(10 * m);
    for (k = 0; k < m; k++)
    {
        for (tbl_idx = 0; tbl_idx < l; tbl_idx++)
        {
            phase = 2.0 * EVS_FX_CLDFB_PI * (double)k * (double)tbl_idx / (double)m;
            s_cos_tbl[idx][k * l + tbl_idx] = (Word16)floor(0.5 + 32767.0 * cos(phase));
            s_sin_tbl[idx][k * l + tbl_idx] = (Word16)floor(0.5 + 32767.0 * sin(phase));
        }
    }

    s_mod_tables_ready[idx] = 1;
}

static void cldfb_configure_bank(CLDFB_FILTER_BANK *hs, Word16 m)
{
    hs->no_channels     = m;
    hs->p_filter_length = (Word16)(10 * m);
    hs->no_col          = CLDFB_NO_COL_MAX;
    hs->p_filter        = cldfb_proto_filter(m);
    hs->rot_vec_ana_re  = NULL;
    hs->rot_vec_ana_im  = NULL;
    hs->rot_vec_syn_re  = NULL;
    hs->rot_vec_syn_im  = NULL;
    hs->bandsToZero     = 0;
    hs->nab             = 0;
    hs->usb             = m;
    hs->lsb             = 0;
    hs->scale           = 0;
    hs->memory_length   = 0;

    cldfb_init_mod_tables(m);
}

/* -------------------------------------------------------------------------- */
/* Exported functions                                                         */
/* -------------------------------------------------------------------------- */

Word16 CLDFB_getNumChannels(Word32 sampleRate)
{
    /* CLDFB channel bandwidth is 800 Hz.  Round to nearest integer. */
    Word32 channels = (sampleRate + 400L) / 800L;

    if (channels < 10)
    {
        channels = 10;
    }
    if (channels > 60)
    {
        channels = 60;
    }

    /* Return the closest supported configuration. */
    if (channels <= 13)
    {
        return 10;
    }
    if (channels <= 18)
    {
        return 16;
    }
    if (channels <= 26)
    {
        return 20;
    }
    if (channels <= 36)
    {
        return 32;
    }
    if (channels <= 50)
    {
        return 40;
    }
    return 60;
}

void openCldfb(HANDLE_CLDFB_FILTER_BANK *h_cldfb,
               const Word16 type,
               const Word16 maxCldfbBands,
               const Word16 frameSize)
{
    evs_fx_cldfb_inst_t *inst;
    CLDFB_FILTER_BANK *hs;
    (void)frameSize;   /* no_col is fixed at CLDFB_NO_COL_MAX */

    if (h_cldfb == NULL)
    {
        return;
    }

    /* One heap instance per handle: the number of CLDFB banks a session needs
     * is bounded by the number of codec instances, not by a fixed pool. */
    inst = (evs_fx_cldfb_inst_t *)calloc(1, sizeof(evs_fx_cldfb_inst_t));
    if (inst == NULL)
    {
        *h_cldfb = NULL;
        return;
    }

    hs = &inst->bank;
    hs->type = (CLDFB_TYPE)type;

    {
        Word16 m = maxCldfbBands;
        if (!cldfb_supported_channels(m))
        {
            m = CLDFB_getNumChannels((Word32)m * 800L);
        }
        cldfb_configure_bank(hs, m);
    }

    hs->FilterStates = inst->state;
    hs->memory       = inst->memory;
    hs->memory_length = 0;

    evs_fx_set16(hs->FilterStates, 0, EVS_FX_CLDFB_MAX_STATE_WORDS);
    evs_fx_set16(hs->memory, 0, EVS_FX_CLDFB_MAX_STATE_WORDS);

    *h_cldfb = hs;
}

void deleteCldfb(HANDLE_CLDFB_FILTER_BANK *h_cldfb)
{
    HANDLE_CLDFB_FILTER_BANK hs;
    evs_fx_cldfb_inst_t *inst;

    if (h_cldfb == NULL)
    {
        return;
    }

    hs = *h_cldfb;
    if (hs == NULL)
    {
        return;
    }

    /* CLDFB_FILTER_BANK is the first member of the instance, so the handle
     * doubles as the allocation pointer. */
    inst = cldfb_inst_from_handle(hs);
    *h_cldfb = NULL;
    free(inst);
}

void resampleCldfb(HANDLE_CLDFB_FILTER_BANK hs,
                   const Word16 newCldfbBands,
                   const Word16 frameSize,
                   const Word8 firstFrame)
{
    (void)frameSize;
    (void)firstFrame;

    if (hs == NULL)
    {
        return;
    }

    if (hs->no_channels != newCldfbBands)
    {
        Word16 m = newCldfbBands;
        if (!cldfb_supported_channels(m))
        {
            m = CLDFB_getNumChannels((Word32)m * 800L);
        }
        cldfb_configure_bank(hs, m);
    }

    if (hs->FilterStates != NULL)
    {
        evs_fx_set16(hs->FilterStates, 0, EVS_FX_CLDFB_MAX_STATE_WORDS);
    }
}

void cldfb_reset_memory(HANDLE_CLDFB_FILTER_BANK hs)
{
    Word16 len;

    if (hs == NULL || hs->FilterStates == NULL)
    {
        return;
    }

    if (hs->type == CLDFB_ANALYSIS)
    {
        len = (Word16)(hs->p_filter_length - hs->no_channels + hs->no_channels * hs->no_col);
    }
    else
    {
        len = hs->p_filter_length;
    }

    if (len > EVS_FX_CLDFB_MAX_STATE_WORDS)
    {
        len = EVS_FX_CLDFB_MAX_STATE_WORDS;
    }

    evs_fx_set16(hs->FilterStates, 0, len);
}

void cldfb_save_memory(HANDLE_CLDFB_FILTER_BANK hs)
{
    Word16 len;

    if (hs == NULL || hs->FilterStates == NULL || hs->memory == NULL)
    {
        return;
    }

    if (hs->memory_length != 0)
    {
        /* Already saved for this instance; leave the existing snapshot. */
        return;
    }

    if (hs->type == CLDFB_ANALYSIS)
    {
        len = (Word16)(hs->p_filter_length - hs->no_channels + hs->no_channels * hs->no_col);
    }
    else
    {
        len = hs->p_filter_length;
    }

    if (len > EVS_FX_CLDFB_MAX_STATE_WORDS)
    {
        len = EVS_FX_CLDFB_MAX_STATE_WORDS;
    }

    hs->memory_length = len;
    evs_fx_copy16(hs->FilterStates, hs->memory, len);
}

void cldfb_restore_memory(HANDLE_CLDFB_FILTER_BANK hs)
{
    if (hs == NULL || hs->FilterStates == NULL || hs->memory == NULL)
    {
        return;
    }

    if (hs->memory_length == 0)
    {
        return;
    }

    evs_fx_copy16(hs->memory, hs->FilterStates, hs->memory_length);
    hs->memory_length = 0;
}

void cldfbAnalysisFiltering(HANDLE_CLDFB_FILTER_BANK anaCldfb,
                            Word32 **cldfbReal,
                            Word32 **cldfbImag,
                            CLDFB_SCALE_FACTOR *scaleFactor,
                            const Word16 *timeIn,
                            const Word16 timeIn_e,
                            const Word16 nTimeSlots,
                            Word32 *pWorkBuffer)
{
    Word16 m, l, offset, slot, band, tap;
    Word16 anaScale, tbl_idx;
    Word16 shift;
    Word32 in_len;
    const Word16 *proto;
    const Word16 *cos_tbl;
    const Word16 *sin_tbl;
    long long accR, accI, prod;
    Word32 outR, outI;
    (void)pWorkBuffer;

    if (anaCldfb == NULL || anaCldfb->FilterStates == NULL || timeIn == NULL ||
        cldfbReal == NULL || cldfbImag == NULL || scaleFactor == NULL)
    {
        return;
    }

    m     = anaCldfb->no_channels;
    l     = anaCldfb->p_filter_length;
    proto = anaCldfb->p_filter;
    offset = (Word16)(l - m);
    in_len = (Word32)m * (Word32)nTimeSlots;

    if (!cldfb_supported_channels(m))
    {
        return;
    }

    tbl_idx = cldfb_channel_index(m);
    cos_tbl = s_cos_tbl[tbl_idx];
    sin_tbl = s_sin_tbl[tbl_idx];
    anaScale = cldfb_anaScale[tbl_idx];

    /* Shift the previous tail to the front and append the new input. */
    for (slot = 0; slot < offset; slot++)
    {
        anaCldfb->FilterStates[slot] = anaCldfb->FilterStates[in_len + slot];
    }
    for (slot = 0; slot < in_len; slot++)
    {
        anaCldfb->FilterStates[offset + slot] = timeIn[slot];
    }

    /* Right shift from the 64-bit accumulator.  The accumulator contains
     * x_raw * p_raw * cos/sin (Q timeIn_e + 15 + 15).  We normalise by 30
     * and apply the per-band analysis scaling from cnst_fx.h. */
    shift = (Word16)(30 - timeIn_e + anaScale);
    if (shift < 0)
    {
        shift = 0;
    }

    for (slot = 0; slot < nTimeSlots; slot++)
    {
        Word32 slot_offset = (Word32)slot * (Word32)m;

        for (band = 0; band < m; band++)
        {
            accR = 0;
            accI = 0;

            for (tap = 0; tap < l; tap++)
            {
                Word16 s  = anaCldfb->FilterStates[slot_offset + tap];
                Word16 pf = proto[tap];
                Word32 tbl_off = (Word32)band * (Word32)l + (Word32)tap;

                prod = (long long)s * (long long)pf;
                accR += prod * (long long)cos_tbl[tbl_off];
                accI -= prod * (long long)sin_tbl[tbl_off];
            }

            outR = evs_fx_saturate32(evs_fx_shift64(accR, shift));
            outI = evs_fx_saturate32(evs_fx_shift64(accI, shift));

            cldfbReal[slot][band] = outR;
            cldfbImag[slot][band] = outI;
        }
    }

    scaleFactor->lb_scale = anaScale;
}

void cldfbSynthesisFiltering(HANDLE_CLDFB_FILTER_BANK synCldfb,
                             Word32 **CldfbBufferReal,
                             Word32 **CldfbBufferImag,
                             const CLDFB_SCALE_FACTOR *scaleFactor,
                             Word16 *timeOut,
                             const Word16 timeOut_e,
                             const Word16 nTimeSlots,
                             Word32 *pWorkBuffer)
{
    Word16 m, slot, band, t;
    Word16 sf, tbl_idx;
    Word16 shift;
    const Word16 *cos_tbl;
    const Word16 *sin_tbl;
    long long acc;
    (void)pWorkBuffer;

    if (synCldfb == NULL || CldfbBufferReal == NULL || CldfbBufferImag == NULL ||
        scaleFactor == NULL || timeOut == NULL)
    {
        return;
    }

    m = synCldfb->no_channels;
    if (!cldfb_supported_channels(m))
    {
        return;
    }

    tbl_idx = cldfb_channel_index(m);
    cos_tbl = s_cos_tbl[tbl_idx];
    sin_tbl = s_sin_tbl[tbl_idx];
    sf = scaleFactor->lb_scale;

    /* Inverse DFT with output scaling.  The accumulator is
     * real_raw * cos_raw + imag_raw * sin_raw (Q sf + 15).  We normalise
     * by 15, convert to timeOut_e, and add a fixed headroom shift of 16
     * to keep the Word16 output saturated. */
    shift = (Word16)(sf + timeOut_e + 31);
    if (shift < 0)
    {
        shift = 0;
    }

    for (slot = 0; slot < nTimeSlots; slot++)
    {
        for (t = 0; t < m; t++)
        {
            acc = 0;

            for (band = 0; band < m; band++)
            {
                Word16 tbl_off = (Word16)(band * m + t);
                acc += (long long)CldfbBufferReal[slot][band] * (long long)cos_tbl[tbl_off]
                     + (long long)CldfbBufferImag[slot][band] * (long long)sin_tbl[tbl_off];
            }

            timeOut[slot * m + t] = evs_fx_saturate16(evs_fx_shift64(acc, shift));
        }
    }
}
